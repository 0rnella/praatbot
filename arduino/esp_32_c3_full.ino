#include <HTTPClient.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

WiFiManager wifiManager;

// Fixed WiFi credentials to try first
const char* WIFI_SSID = "pixie";     // Replace with your WiFi name
const char* WIFI_PASSWORD = "ornellaf"; // Replace with your WiFi password

// Proxy server IP + port
String server_url = "https://robot-server-782703035576.europe-west1.run.app";
String upload_url = server_url + "/upload";

WiFiClientSecure testClient;
WiFiClient plainClient;
bool useHttps = server_url.startsWith("https://");

// Local Flask dev server only speaks plain HTTP, so pick the right client
// based on the server_url scheme instead of always doing a TLS handshake.
WiFiClient& getHttpClient() {
  return useHttps ? (WiFiClient&)testClient : plainClient;
}

// Pin definitions
#define BUTTON_PIN 3
#define LED_PIN 8  // Built-in LED

// I2S pins for microphone (RX)
#define MIC_I2S_SD 0    // Serial Data
#define MIC_I2S_SCK 1   // Serial Clock
#define MIC_I2S_WS 2    // Word Select

// I2S pins for audio output (TX) - MAX98357A
#define AUDIO_I2S_DIN 7   // Data to MAX98357A
#define AUDIO_I2S_BCLK 9  // Bit Clock
#define AUDIO_I2S_LRC 10  // Left/Right Clock

// Recording settings
#define SAMPLE_RATE     16000
#define RECORD_SECONDS  2
#define SAMPLE_SIZE     2      // 16-bit
#define BUFFER_SIZE     (SAMPLE_RATE * RECORD_SECONDS * SAMPLE_SIZE)

uint8_t audio_buffer[BUFFER_SIZE];

// Button state tracking
bool lastButtonState = HIGH;
bool recording = false;
int recording_position = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  // Setup pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("🤖 AI Robot Starting Up!");

  // Initialize global SSL client
  testClient.setInsecure();

  // Try connecting to fixed WiFi credentials first
  connectToWiFi();
  
  // Initialize I2S for microphone (RX mode)
  setupMicrophoneI2S();
  
  Serial.println("🎤 Microphone ready!");
  Serial.println("🔘 Press button to record and ask a question!");
}

void loop() {
  // Read button state
  bool currentButtonState = digitalRead(BUTTON_PIN);
  
  // Button pressed (falling edge) - start recording
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    delay(50); // Debounce
    startRecording();
  }
  
  // Button released (rising edge) - stop recording
  if (lastButtonState == LOW && currentButtonState == HIGH) {
    delay(50); // Debounce
    if (recording) {
      stopRecordingAndProcess();
    }
  }
  
  // While recording, keep collecting audio data
  if (recording) {
    recordAudioData();
  }
  
  lastButtonState = currentButtonState;
  delay(10);
}

void connectToWiFi() {
  Serial.println("📶 Attempting to connect to WiFi...");
  Serial.printf("📶 Trying to connect to: %s\n", WIFI_SSID);
  
  // Try to connect with fixed credentials
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Wait up to 15 seconds for connection
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("✅ WiFi connected successfully!");
    Serial.printf("📍 IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("📶 Signal strength: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println();
    Serial.println("❌ Failed to connect with fixed credentials");
    Serial.println("🔧 Starting WiFi configuration portal...");
    Serial.println("📱 Connect to 'ESP32-Robot-Setup' network to configure WiFi");
    
    // Fall back to WiFiManager portal
    wifiManager.autoConnect("ESP32-Robot-Setup");
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ WiFi configured and connected via portal!");
      Serial.printf("📍 IP address: %s\n", WiFi.localIP().toString().c_str());
    }
  }
}

void setupMicrophoneI2S() {
  // First uninstall any existing I2S driver
  esp_err_t err = i2s_driver_uninstall(I2S_NUM_0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("⚠️ I2S uninstall warning: %d\n", err);
  }
  
  delay(100); // Give it time to clean up
  
  // I2S config for microphone input
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = MIC_I2S_SCK,
    .ws_io_num = MIC_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_I2S_SD
  };

  err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ Failed installing microphone I2S driver: %d\n", err);
    delay(1000); // Wait and try once more
    err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
      Serial.printf("❌ Microphone I2S install failed again: %d\n", err);
      return;
    }
  }
  
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ Failed setting microphone I2S pins: %d\n", err);
    return;
  }

  Serial.println("✅ Microphone I2S initialized!");
}

void setupAudioI2S(uint32_t sampleRate = 16000) {  // Default to 16kHz
  // Uninstall microphone I2S first
  esp_err_t err = i2s_driver_uninstall(I2S_NUM_0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("⚠️ Audio I2S uninstall warning: %d\n", err);
  }
  
  delay(200); // Give it more time to clean up
  
  Serial.printf("🔧 Setting up I2S for %d Hz playback\n", sampleRate);
  
  // Very conservative I2S config for ESP32-C3
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = sampleRate,  // Keep the original sample rate
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono output for better performance
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,    // More buffers for smoother playback
    .dma_buf_len = 256,    // Smaller buffers for ESP32-C3
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = AUDIO_I2S_BCLK,
    .ws_io_num = AUDIO_I2S_LRC,
    .data_out_num = AUDIO_I2S_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ Failed installing audio I2S driver: %d\n", err);
    return;
  }
  
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ Failed setting audio I2S pins: %d\n", err);
    return;
  }

  Serial.println("✅ Conservative audio I2S initialized!");
}

void startRecording() {
  recording = true;
  recording_position = 0;
  digitalWrite(LED_PIN, HIGH);
  Serial.println("🎤 Recording... Keep button pressed, release when done!");
  
  // Clear the buffer
  memset(audio_buffer, 0, BUFFER_SIZE);
}

void recordAudioData() {
  if (!recording) return;
  
  // Read a small chunk of audio data
  const int chunk_size = 512;
  size_t bytes_read = 0;
  
  if (recording_position < BUFFER_SIZE) {
    int remaining_space = BUFFER_SIZE - recording_position;
    int read_size = min(chunk_size, remaining_space);
    
    i2s_read(I2S_NUM_0, audio_buffer + recording_position, read_size, &bytes_read, 10);
    recording_position += bytes_read;
    
    // If buffer is full, stop recording
    if (recording_position >= BUFFER_SIZE) {
      Serial.println("📏 Buffer full! Stopping recording...");
      stopRecordingAndProcess();
    }
  }
}

void stopRecordingAndProcess() {
  if (!recording) return;
  
  recording = false;
  digitalWrite(LED_PIN, LOW);
  
  Serial.printf("🎤 Recording complete! Recorded %d bytes\n", recording_position);

  // Process audio with gain (only process the recorded portion)
  int16_t* samples = (int16_t*)audio_buffer;
  int sample_count = recording_position / 2;  // 16-bit samples
  float gain = 8.0; // Adjust if too quiet/loud

  for (int i = 0; i < sample_count; i++) {
    int32_t val = samples[i] * gain;
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;
    samples[i] = val;
  }

  // Calculate average signal level
  int32_t sum = 0;
  for (int i = 0; i < sample_count; i++) {
    sum += abs(samples[i]);
  }
  int avg_signal = (sample_count > 0) ? sum / sample_count : 0;
  Serial.printf("🔊 Avg signal level: %d\n", avg_signal);

  Serial.println("🤔 Thinking...");
  Serial.println("✅ Processing complete! Sending to AI...");

  // Send to server (use actual recorded size, not full buffer)
  sendAudioToServer(recording_position);
}

void sendAudioToServer(int actual_data_size) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Wi-Fi not connected!");
    return;
  }

  // Test server connectivity first
  Serial.println("🔍 Testing server connectivity...");
  HTTPClient testHttp;
  testHttp.begin(getHttpClient(), server_url + "/health");
  testHttp.setTimeout(10000);
  
  int testResponse = testHttp.GET();
  Serial.printf("🏥 Health check response: %d\n", testResponse);
  if (testResponse > 0) {
    String testReply = testHttp.getString();
    Serial.println("🏥 Server response: " + testReply);
  }
  testHttp.end();
  
  if (testResponse <= 0) {
    String testReply = testHttp.getString();
    Serial.println("test response"+ testReply);
    Serial.println("❌ Cannot reach server! Check IP address and server status.");
    return;
  }

  // Check available heap memory
  Serial.printf("🧠 Free heap: %d bytes\n", ESP.getFreeHeap());

  HTTPClient http;
  http.setTimeout(40000);
  
  // Add debugging for the connection
  Serial.println(useHttps ? "🔗 Attempting HTTPS connection..." : "🔗 Attempting HTTP connection...");
  if (!http.begin(getHttpClient(), server_url + "/upload")) {
    Serial.println("❌ Failed to begin connection");
    return;
  }

  http.addHeader("Content-Type", "multipart/form-data; boundary=----WebKitFormBoundary");
  Serial.println("✅ Connection established");

  String header = "------WebKitFormBoundary\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String footer = "\r\n------WebKitFormBoundary--\r\n";

  // Calculate required size (use actual recorded data size)
  int bodyLength = header.length() + 44 + actual_data_size + footer.length();
  Serial.printf("📏 Need %d bytes, have %d bytes free\n", bodyLength, ESP.getFreeHeap());

  // Try to allocate memory
  uint8_t* full_body = (uint8_t*)malloc(bodyLength);
  
  if (!full_body) {
    Serial.println("❌ Not enough memory! Try reducing RECORD_SECONDS or reboot ESP32.");
    http.end();
    return;
  }

  Serial.println("✅ Memory allocated successfully!");

  // WAV header (use actual data size)
  uint8_t wav_header[44];
  writeWAVHeader(wav_header, actual_data_size, SAMPLE_RATE);

  // Assemble the request body
  int pos = 0;
  memcpy(full_body + pos, header.c_str(), header.length()); 
  pos += header.length();
  memcpy(full_body + pos, wav_header, 44); 
  pos += 44;
  memcpy(full_body + pos, audio_buffer, actual_data_size);  // Only send recorded data
  pos += actual_data_size;
  memcpy(full_body + pos, footer.c_str(), footer.length());

  // Send request
  Serial.println("📤 Sending audio to AI server...");
  Serial.println("⏳ This may take 10-20 seconds for transcription + TTS...");
  
  // Send the request (this will block)
  Serial.println("🚀 Sending POST request...");
  int responseCode = http.POST(full_body, bodyLength);
  String reply = http.getString();
  
  Serial.printf("📬 Response code: %d\n", responseCode);
  
  // Add detailed error reporting
  if (responseCode == -1) {
    Serial.println("❌ Connection timeout or failed");
    Serial.printf("🔍 WiFi connected: %s\n", WiFi.status() == WL_CONNECTED ? "Yes" : "No");
    Serial.printf("🔍 Free heap: %d bytes\n", ESP.getFreeHeap());
  } else if (responseCode < 0) {
    Serial.printf("❌ HTTP Error code: %d\n", responseCode);
  }
  
  if (responseCode == 200) {
    processAIResponse(reply);
  } else {
    Serial.println("❌ Failed to send audio to server");
    Serial.println("Response: " + reply);
  }

  free(full_body);
  http.end();
  
  Serial.println("🔘 Ready for next question! Press button to record.");
}

// Extract response processing to separate function
void processAIResponse(String reply) {
  // Parse JSON response
  DynamicJsonDocument doc(2048);  // Increased size for AI response
  DeserializationError err = deserializeJson(doc, reply);
  
  if (!err && doc.containsKey("text")) {
    String transcript = doc["text"].as<String>();
    Serial.println("🎤 I heard: \"" + transcript + "\"");
    
    // Check if we have an AI response
    if (doc.containsKey("ai_response")) {
      String ai_response = doc["ai_response"].as<String>();
      Serial.println("🤖 Robot says: \"" + ai_response + "\"");
      
      // Check if there's audio to play
      if (doc.containsKey("has_audio") && doc["has_audio"] == true) {
        String audio_url = doc["audio_url"].as<String>();
        String full_audio_url = server_url + audio_url;
        
        Serial.println("🔗 Audio URL from server: " + audio_url);
        Serial.println("🔗 Full URL: " + full_audio_url);
        Serial.println("🔗 Server base: " + String(upload_url));
        
        playAudioFromURL(full_audio_url);
      } else {
        Serial.println("📝 No audio available, showing text only");
      }
      
      // Flash LED to indicate successful AI interaction
      for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(150);
        digitalWrite(LED_PIN, LOW);
        delay(150);
      }
    } else {
      // Just transcription
      Serial.println("📝 Transcription only");
    }
    
  } else {
    Serial.println("❌ Failed to parse AI response");
    Serial.println("Raw response: " + reply);
  }
}

// Buffered audio playback - read ahead, play in small chunks
void playAudioFromURL(String url) {
  Serial.println("🔊 Buffered audio streaming...");
  Serial.println("🎵 Getting audio from: " + url);
  Serial.printf("🧠 Free heap before audio download: %d bytes\n", ESP.getFreeHeap());
  
  // Add a small delay to let the previous HTTP connection fully close
  delay(1000);
  
  HTTPClient http;
  
  if (!http.begin(getHttpClient(), url)) {
    Serial.println("❌ Failed to begin audio connection");
    return;
  }

  http.setTimeout(20000); // Shorter timeout for audio download
  Serial.println("✅ Audio connection established, making GET request...");
  
  int httpCode = http.GET();
  Serial.printf("🎵 Audio GET response code: %d\n", httpCode);
  
  // Add detailed error reporting for the audio request
  if (httpCode == -1) {
    Serial.println("❌ HTTP_ERROR_CONNECTION_REFUSED or timeout");
    Serial.printf("🔍 Client connected: %s\n", testClient.connected() ? "Yes" : "No");
    Serial.printf("🔍 Free heap: %d bytes\n", ESP.getFreeHeap());
  } else if (httpCode == -11) {
    Serial.println("❌ HTTP_ERROR_READ_TIMEOUT");
  } else if (httpCode == -10) {
    Serial.println("❌ HTTP_ERROR_CONNECTION_LOST");
  } else if (httpCode < 0) {
    Serial.printf("❌ HTTP error code: %d\n", httpCode);
  }
  
  if (httpCode == HTTP_CODE_OK) {
    Serial.printf("✅ HTTP Success, Content-Length: %d bytes\n", http.getSize());
    
    WiFiClient* stream = http.getStreamPtr();
    
    // Skip WAV header - we know it's 44 bytes for our files
    uint8_t header[44];
    stream->readBytes(header, 44);
    
    // Get audio parameters from header
    uint16_t numChannels = header[22] | (header[23] << 8);
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    uint16_t bitsPerSample = header[34] | (header[35] << 8);
    
    uint32_t dataSize = http.getSize() - 44;  // Remaining data after header
    
    Serial.printf("📊 %d ch, %d Hz, %d-bit, %u bytes audio data\n", 
                  numChannels, sampleRate, bitsPerSample, dataSize);
    Serial.printf("📊 Expected duration: %.2f seconds\n", 
                  (float)dataSize / (sampleRate * numChannels * (bitsPerSample/8)));
    
    // Setup I2S with MUCH lower sample rate for ESP32-C3
    uint32_t playbackRate = min((uint32_t)sampleRate, (uint32_t)16000);  // Cap at 16kHz
    if (playbackRate != sampleRate) {
      Serial.printf("🔽 Reducing playback rate from %d to %d Hz\n", sampleRate, playbackRate);
    }
    
    setupAudioI2S(playbackRate);
    
    Serial.println("🎵 Starting buffered playback...");
    
    // Use very small chunks that ESP32-C3 can handle
    const size_t CHUNK_SIZE = 128;  // Tiny chunks
    uint8_t audioBuffer[CHUNK_SIZE];
    
    size_t totalRead = 0;
    size_t totalPlayed = 0;
    unsigned long startTime = millis();
    
    while (http.connected() && totalRead < dataSize) {
      // Read a small chunk
      size_t toRead = min((size_t)CHUNK_SIZE, (size_t)(dataSize - totalRead));
      size_t bytesRead = stream->readBytes(audioBuffer, toRead);
      
      if (bytesRead == 0) {
        Serial.println("⚠️ No more data from stream");
        break;
      }
      
      totalRead += bytesRead;
      
      // Try to play this chunk with very aggressive timeout management
      size_t chunkPos = 0;
      while (chunkPos < bytesRead) {
        size_t remaining = bytesRead - chunkPos;
        size_t bytesWritten = 0;
        
        // Use short timeout, don't block forever
        esp_err_t result = i2s_write(I2S_NUM_0, audioBuffer + chunkPos, remaining, &bytesWritten, 50);
        
        if (result == ESP_OK && bytesWritten > 0) {
          chunkPos += bytesWritten;
          totalPlayed += bytesWritten;
        } else {
          // I2S queue might be full, wait a bit
          delay(5);  // Very short delay
          
          // If we're stuck for too long, give up on this chunk
          static unsigned long lastProgress = millis();
          if (millis() - lastProgress > 1000) {
            Serial.printf("⏭️ Skipping stuck chunk at %u bytes\n", totalPlayed);
            chunkPos = bytesRead;  // Skip this chunk
            lastProgress = millis();
          }
        }
      }
      
      // Progress every 10KB
      if (totalRead % 10240 == 0) {
        float progress = (float)totalRead / dataSize * 100;
        Serial.printf("📊 %.1f%% read, %u played\n", progress, totalPlayed);
      }
      
      // Small delay to let I2S catch up
      if (totalRead % 1024 == 0) {
        delay(1);  // Tiny pause every KB
      }
    }
    
    unsigned long endTime = millis();
    float actualDuration = (endTime - startTime) / 1000.0;
    float expectedDuration = (float)dataSize / (playbackRate * numChannels * (bitsPerSample/8));
    
    Serial.printf("✅ Buffered playback complete!\n");
    Serial.printf("📊 Read: %u, Played: %u bytes in %.2f sec\n", totalRead, totalPlayed, actualDuration);
    Serial.printf("📊 Expected: %.2f sec (%.1f%% data played)\n", 
                  expectedDuration, (float)totalPlayed / dataSize * 100);
    
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
  }
  
  http.end();
  delay(500);
  setupMicrophoneI2S();
  Serial.println("🔇 Back to microphone mode");
}
// Create WAV header for the audio data
void writeWAVHeader(uint8_t* header, int dataSize, int sampleRate) {
  int fileSize = dataSize + 36;
  int byteRate = sampleRate * SAMPLE_SIZE;

  memcpy(header, "RIFF", 4);
  *(uint32_t*)(header + 4) = fileSize;
  memcpy(header + 8, "WAVEfmt ", 8);
  *(uint32_t*)(header + 16) = 16;           // Subchunk1Size
  *(uint16_t*)(header + 20) = 1;            // AudioFormat (PCM)
  *(uint16_t*)(header + 22) = 1;            // NumChannels
  *(uint32_t*)(header + 24) = sampleRate;   // SampleRate
  *(uint32_t*)(header + 28) = byteRate;     // ByteRate
  *(uint16_t*)(header + 32) = SAMPLE_SIZE;  // BlockAlign
  *(uint16_t*)(header + 34) = 16;           // BitsPerSample
  memcpy(header + 36, "data", 4);
  *(uint32_t*)(header + 40) = dataSize;     // Subchunk2Size
}