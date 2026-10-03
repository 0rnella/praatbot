from flask import Flask, request, jsonify, send_file, g
import requests
import time
import os
import uuid
from datetime import datetime, timezone
from dotenv import load_dotenv
from google.cloud import firestore
import logging
import sys
import subprocess

# Load environment variables from .env file
load_dotenv()

# Configure logging for Cloud Run
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    stream=sys.stdout
)

app = Flask(__name__)

ASSEMBLYAI_API_KEY = os.getenv("ASSEMBLYAI_API_KEY")
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")

# Environment detection - use /tmp in production environments (Cloud Run, Docker)
IS_PRODUCTION = os.getenv("PORT") is not None or os.getenv("K_SERVICE") is not None or os.getenv("GOOGLE_CLOUD_PROJECT") is not None

# Child-friendly robot prompt
ROBOT_PROMPT = """The following is a message from by a child under 12 within the context of a robotics activity. The robotics activity is space-themed. You are a robot created for entertainment purposes. Please answer this message in an entertaining, yet informative way that does not include any profanity and remains age-appropriate. Your response should be short enough to be able to read out within 7 seconds (around 30-40 words max). Be enthusiastic and fun, and perhaps reference space in some way!

Child's message: """

# Fun fact prompt
FUN_FACT_PROMPT = """You are a fun space-themed robot sharing interesting facts about space with children under 12. Generate a single, amazing and age-appropriate fun fact on the topic of space. Make it exciting and easy to understand! Your response should be short enough to read out within 7 seconds (around 30-40 words max). Start with something like "Hey! Did you know..." or "Here's something amazing..." and be enthusiastic!"""

# --- Firestore-backed device tracker -----------------------------------
# Persists across scale-to-zero/redeploys, unlike local memory or disk.
FIRESTORE_PROJECT_ID = "aha-robot"
FIRESTORE_DATABASE = "praatbot"
DEVICES_COLLECTION = "devices"
CONVERSATIONS_SUBCOLLECTION = "conversations"

firestore_db = firestore.Client(project=FIRESTORE_PROJECT_ID, database=FIRESTORE_DATABASE)

# Routes that aren't actual device traffic and shouldn't register an entry.
DEVICE_TRACKING_SKIP_ENDPOINTS = {'root', 'devices_endpoint', 'device_conversations_endpoint', 'serve_audio'}


@app.before_request
def track_device():
    if request.endpoint in DEVICE_TRACKING_SKIP_ENDPOINTS:
        return

    # X-Device-Id is a stable per-board ID (derived from the ESP32's eFuse
    # MAC - see the Arduino sketch), so repeat requests from the same board
    # update the same entry. X-Device-Name is just a label and isn't unique
    # (it's hardcoded the same on every board right now).
    device_id = request.headers.get('X-Device-Id') or str(uuid.uuid4())
    display_name = request.headers.get('X-Device-Name') or 'praatbot'
    now = datetime.now(timezone.utc)

    doc_ref = firestore_db.collection(DEVICES_COLLECTION).document(device_id)
    if doc_ref.get().exists:
        doc_ref.update({'last_seen': now, 'display_name': display_name})
    else:
        doc_ref.set({'display_name': display_name, 'first_seen': now, 'last_seen': now})

    g.device_id = device_id


def save_conversation(device_id, user_text, ai_response, was_fun_fact=False):
    """Log one Q&A exchange under the device's conversation history."""
    (firestore_db.collection(DEVICES_COLLECTION).document(device_id)
     .collection(CONVERSATIONS_SUBCOLLECTION).add({
         'timestamp': datetime.now(timezone.utc),
         'user_text': user_text,
         'ai_response': ai_response,
         'was_fun_fact': was_fun_fact,
     }))


@app.route('/devices', methods=['GET'])
def devices_endpoint():
    """View all tracked devices."""
    docs = (firestore_db.collection(DEVICES_COLLECTION)
            .order_by('last_seen', direction=firestore.Query.DESCENDING)
            .stream())
    result = [
        {
            'device_id': doc.id,
            'display_name': doc.get('display_name'),
            'first_seen': doc.get('first_seen').isoformat(),
            'last_seen': doc.get('last_seen').isoformat(),
        }
        for doc in docs
    ]
    return jsonify({'count': len(result), 'devices': result})


@app.route('/devices/<device_id>/conversations', methods=['GET'])
def device_conversations_endpoint(device_id):
    """View a device's conversation history, most recent first."""
    docs = (firestore_db.collection(DEVICES_COLLECTION).document(device_id)
            .collection(CONVERSATIONS_SUBCOLLECTION)
            .order_by('timestamp', direction=firestore.Query.DESCENDING)
            .stream())
    result = [
        {
            'conversation_id': doc.id,
            'timestamp': doc.get('timestamp').isoformat(),
            'user_text': doc.get('user_text'),
            'ai_response': doc.get('ai_response'),
            'was_fun_fact': doc.get('was_fun_fact'),
        }
        for doc in docs
    ]
    return jsonify({'count': len(result), 'conversations': result})

# Root route
@app.route('/')
def root():
    return jsonify({
        'status': 'AI Robot Server - Cloud Deployed',
        'endpoints': {
            'health': '/health',
            'upload': '/upload',
            'fun-fact': '/fun-fact',
            'audio': '/audio/<filename>',
            'devices': '/devices',
            'device-conversations': '/devices/<device_id>/conversations'
        }
    })

# Route to receive audio from ESP32
@app.route('/upload', methods=['POST'])
def upload():
    if 'audio' not in request.files:
        return jsonify({'error': 'No audio file uploaded'}), 400

    audio_file = request.files['audio']
    audio_data = audio_file.read()

    # Save locally for debugging
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    mic_input_audio_dir = "/tmp/mic_input" if IS_PRODUCTION else "mic_input"
    filename = f"{mic_input_audio_dir}/mic_input_{timestamp}.wav"
    
    # Create directory if it doesn't exist
    os.makedirs(mic_input_audio_dir, exist_ok=True)
    
    with open(filename, "wb") as f:
        f.write(audio_data)
    
    print(f"🎤 Audio saved: {filename}")

    # Step 1: Upload audio to AssemblyAI
    print("📄 Transcribing audio...")
    upload_response = requests.post(
        'https://api.assemblyai.com/v2/upload',
        headers={'authorization': ASSEMBLYAI_API_KEY},
        data=audio_data
    )
    
    if upload_response.status_code != 200:
        return jsonify({'error': 'Failed to upload audio'}), 500
    
    upload_url = upload_response.json()['upload_url']

    # Step 2: Request transcription
    transcript_response = requests.post(
        'https://api.assemblyai.com/v2/transcript',
        headers={'authorization': ASSEMBLYAI_API_KEY, 'content-type': 'application/json'},
        json={'audio_url': upload_url}
    )
    
    if transcript_response.status_code != 200:
        return jsonify({'error': 'Failed to request transcription'}), 500
    
    transcript_id = transcript_response.json()['id']

    # Step 3: Polling for transcription completion
    print(f"⏳ Waiting for transcription (ID: {transcript_id})...")
    max_attempts = 30  # 60 seconds max
    attempt = 0
    
    while attempt < max_attempts:
        polling_response = requests.get(
            f'https://api.assemblyai.com/v2/transcript/{transcript_id}',
            headers={'authorization': ASSEMBLYAI_API_KEY}
        )
        
        if polling_response.status_code != 200:
            return jsonify({'error': 'Failed to check transcription status'}), 500
        
        result = polling_response.json()
        
        if result['status'] == 'completed':
            transcript_text = result['text']
            print(f"✅ Transcription: '{transcript_text}'")
            
            # Step 4: Handle empty input by generating a fun fact
            if not transcript_text or transcript_text.strip() == "":
                print("🎲 No speech detected - generating fun fact...")
                fun_fact_response = generate_fun_fact(is_redirect=True)
                save_conversation(g.device_id, transcript_text or "[No speech detected]",
                                   fun_fact_response['text'], was_fun_fact=True)
                return jsonify({
                    'text': transcript_text or "[No speech detected]",
                    'ai_response': fun_fact_response['text'],
                    'has_audio': fun_fact_response['has_audio'],
                    'audio_url': fun_fact_response.get('audio_url', '')
                })

            try:
                print("🤖 Generating AI response...")
                chat_response = get_ai_response(transcript_text)
                print(f"🎭 AI Response: '{chat_response}'")
                save_conversation(g.device_id, transcript_text, chat_response)

                # Step 5: Convert AI response to speech (FIXED SAMPLE RATE)
                print("🔊 Converting to speech...")
                audio_file_path = text_to_speech(chat_response, timestamp)

                if audio_file_path:
                    return jsonify({
                        'text': transcript_text,
                        'ai_response': chat_response,
                        'has_audio': True,
                        'audio_url': f'/audio/{timestamp}.wav'
                    })
                else:
                    return jsonify({
                        'text': transcript_text,
                        'ai_response': chat_response,
                        'has_audio': False
                    })

            except Exception as e:
                print(f"❌ AI Response Error: {e}")
                fallback_response = "Oops! My robot brain is having trouble right now. Try asking me again!"
                save_conversation(g.device_id, transcript_text, fallback_response)
                return jsonify({
                    'text': transcript_text,
                    'ai_response': fallback_response,
                    'has_audio': False
                })
            
        elif result['status'] == 'failed':
            return jsonify({'error': result['error']}), 500
        
        time.sleep(2)
        attempt += 1
    
    return jsonify({'error': 'Transcription timeout'}), 500

def get_ai_response(user_question):
    """Get child-friendly response from OpenAI"""
    if not OPENAI_API_KEY:
        return "Sorry! I need my AI brain to be connected to answer questions."
    
    try:
        response = requests.post(
            'https://api.openai.com/v1/chat/completions',
            headers={
                'Authorization': f'Bearer {OPENAI_API_KEY}',
                'Content-Type': 'application/json'
            },
            json={
                'model': 'gpt-3.5-turbo',
                'messages': [
                    {
                        'role': 'system',
                        'content': ROBOT_PROMPT
                    },
                    {
                        'role': 'user', 
                        'content': user_question
                    }
                ],
                'max_tokens': 80,
                'temperature': 0.8
            },
            timeout=10
        )
        
        if response.status_code == 200:
            ai_text = response.json()['choices'][0]['message']['content'].strip()
            return ai_text
        else:
            print(f"OpenAI API Error: {response.status_code} - {response.text}")
            return "My robot circuits are a bit confused right now! Ask me something else!"
            
    except requests.exceptions.Timeout:
        return "I'm thinking too hard! Ask me again!"
    except Exception as e:
        print(f"AI Error: {e}")
        return "Beep boop! Something went wrong in my robot brain!"

def get_fun_fact(is_redirect=False):
    """Get a fun fact from OpenAI"""
    if not OPENAI_API_KEY:
        return "Sorry! I need my AI brain to be connected to share fun facts."
    
    # Build the prompt - prepend context if this is a redirect due to no sound
    prompt = FUN_FACT_PROMPT
    if is_redirect:
        prompt = "The child just tried to say something but you couldn't hear them clearly. Start your response by apologizing that you couldn't hear them, then share a fun fact. Use phrases like 'Sorry, I didn't hear you clearly, but here's a fun fact!' or 'Oops, I missed that! Here's something cool instead...' " + prompt
    
    try:
        response = requests.post(
            'https://api.openai.com/v1/chat/completions',
            headers={
                'Authorization': f'Bearer {OPENAI_API_KEY}',
                'Content-Type': 'application/json'
            },
            json={
                'model': 'gpt-3.5-turbo',
                'messages': [
                    {
                        'role': 'system',
                        'content': prompt
                    }
                ],
                'max_tokens': 80,
                'temperature': 0.9  # Higher temperature for more varied fun facts
            },
            timeout=10
        )
        
        if response.status_code == 200:
            fun_fact = response.json()['choices'][0]['message']['content'].strip()
            return fun_fact
        else:
            print(f"OpenAI API Error: {response.status_code} - {response.text}")
            return "Here's a robot fact: I'm powered by electricity and curiosity!"
            
    except requests.exceptions.Timeout:
        return "Here's a quick fact: Did you know robots can think really fast?"
    except Exception as e:
        print(f"Fun Fact Error: {e}")
        return "Fun fact: Robots love learning new things, just like you!"

def generate_fun_fact(is_redirect=False):
    """Generate a fun fact with audio"""
    try:
        print("🎲 Generating fun fact...")
        fun_fact_text = get_fun_fact(is_redirect)
        print(f"🎲 Fun Fact: '{fun_fact_text}'")
        
        # Generate timestamp for audio file
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S") + "_funfact"
        
        # Convert fun fact to speech
        print("🔊 Converting fun fact to speech...")
        audio_file_path = text_to_speech(fun_fact_text, timestamp)
        
        if audio_file_path:
            return {
                'text': fun_fact_text,
                'has_audio': True,
                'audio_url': f'/audio/{timestamp}.wav'
            }
        else:
            return {
                'text': fun_fact_text,
                'has_audio': False
            }
            
    except Exception as e:
        print(f"❌ Fun Fact Generation Error: {e}")
        return {
            'text': "Oops! My fun fact generator is having trouble. Here's one: Robots are awesome!",
            'has_audio': False
        }

# Route for getting fun facts directly
@app.route('/fun-fact', methods=['GET'])
def fun_fact_endpoint():
    """Endpoint to get a random fun fact"""
    fun_fact_response = generate_fun_fact()
    return jsonify(fun_fact_response)

def text_to_speech(text, timestamp):
    """Convert text to speech using OpenAI TTS - FIXED FOR ESP32"""
    if not OPENAI_API_KEY:
        return None
        
    try:
        # Request TTS with 16kHz to match ESP32 setup
        response = requests.post(
            'https://api.openai.com/v1/audio/speech',
            headers={
                'Authorization': f'Bearer {OPENAI_API_KEY}',
                'Content-Type': 'application/json'
            },
            json={
                'model': 'tts-1',
                'input': text,
                'voice': 'nova',  # Child-friendly voice
                'response_format': 'wav',
                'speed': 0.9  # Slightly slower for kids
                # Note: OpenAI TTS outputs at 24kHz by default
            },
            timeout=30
        )
        
        if response.status_code == 200:
            # Create audio directory if it doesn't exist
            audio_dir = "/tmp/audio_responses" if IS_PRODUCTION else "audio_responses"
            os.makedirs(audio_dir, exist_ok=True)
            
            # Save the original audio file first
            original_filename = f"{audio_dir}/{timestamp}_original.wav"
            with open(original_filename, 'wb') as f:
                f.write(response.content)
            
            print(f"🔊 Original TTS audio saved: {original_filename}")
            
            # Convert to 16kHz using ffmpeg (if available) or save as-is
            final_filename = f"{audio_dir}/{timestamp}.wav"
            
            # Try to convert sample rate using ffmpeg
            try:
                # Convert 24kHz to 16kHz using ffmpeg
                cmd = [
                    'ffmpeg', '-y', '-i', original_filename,
                    '-ar', '16000',  # Set to 16kHz
                    '-ac', '1',      # Mono
                    '-sample_fmt', 's16',  # 16-bit
                    final_filename
                ]
                result = subprocess.run(cmd, capture_output=True, text=True)
                
                if result.returncode == 0:
                    print(f"✅ Converted to 16kHz: {final_filename}")
                    # Remove original to save space
                    os.remove(original_filename)
                    return final_filename
                else:
                    print(f"⚠️ FFmpeg conversion failed: {result.stderr}")
                    # Fall back to original file
                    os.rename(original_filename, final_filename)
                    print("⚠️ Using original TTS file (may have sample rate mismatch)")
                    return final_filename
                    
            except FileNotFoundError:
                print("⚠️ FFmpeg not found - using original TTS audio")
                print("⚠️ Install ffmpeg for proper sample rate conversion")
                os.rename(original_filename, final_filename)
                return final_filename
            
        else:
            print(f"OpenAI TTS Error: {response.status_code} - {response.text}")
            return None
            
    except Exception as e:
        print(f"TTS Error: {e}")
        return None

# Route to serve audio files
@app.route('/audio/<filename>')
def serve_audio(filename):
    try:
        # Use the same directory logic as in text_to_speech function
        audio_dir = "/tmp/audio_responses" if IS_PRODUCTION else "audio_responses"
        file_path = f"{audio_dir}/{filename}"
        print(f"🎵 Serving audio file: {file_path}")
        
        # Check if file exists
        if not os.path.exists(file_path):
            print(f"❌ Audio file not found: {file_path}")
            return jsonify({'error': 'Audio file not found'}), 404
        
        # Log file info
        file_size = os.path.getsize(file_path)
        print(f"📁 Serving file size: {file_size} bytes")
        
        # Always serve as WAV with correct headers
        return send_file(file_path, mimetype='audio/wav')
        
    except Exception as e:
        print(f"❌ Error serving audio: {e}")
        return jsonify({'error': 'Audio file not found'}), 404

# Health check endpoint
@app.route('/health', methods=['GET'])
def health():
    return jsonify({'status': 'Robot server is running! 🤖', 'env': 'cloud'})

if __name__ == '__main__':
    # Check if API keys are set
    if not ASSEMBLYAI_API_KEY:
        print("⚠️  WARNING: ASSEMBLYAI_API_KEY environment variable not set!")
    if not OPENAI_API_KEY:
        print("⚠️  WARNING: OPENAI_API_KEY environment variable not set!")
    
    print("🤖 Starting AI Robot Server with TTS...")
    print("🎤 Ready to receive audio and respond with properly formatted speech!")
    
    # Check for ffmpeg
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True)
        print("✅ FFmpeg found - audio conversion will work properly")
    except FileNotFoundError:
        print("⚠️ FFmpeg not found - install it for best audio quality")
        print("   Ubuntu/Debian: sudo apt install ffmpeg")
        print("   macOS: brew install ffmpeg")
        print("   Windows: Download from https://ffmpeg.org/")
    
    port = int(os.environ.get('PORT', 5050))
    # Local runs use an adhoc self-signed cert so the ESP32's secureClient
    # (TLS-only) can connect the same way it does to the deployed server.
    app.run(host='0.0.0.0', port=port, debug=False, ssl_context='adhoc')