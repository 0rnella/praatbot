# 🧠 ESP32 Voice Input Project

This project captures audio using an INMP441 microphone connected to an ESP32-C3 board, processes it (e.g., sends it to AssemblyAI), and displays results (such as transcribed text or LED feedback). The goal is to have it send the transcribed text to an AI, have the AI respond, and use text-to-speech to output the sound as well.

---

## 🛠️ Hardware

- ESP32-WROOM-32 or ESP32-C3 SuperMini
- INMP441 I2S Microphone
- (Optional) LEDs with resistors
- Breadboard + jumper wires
- USB-C cable

---

## 🔌 Building Your Robot Step by Step!

Time to connect all the parts! We'll build this like LEGO - one piece at a time.

### Step 1: Set up your breadboard power 🔋
First, we need to give power to everything on our breadboard:
1. Take a wire and connect the ESP32's **3.3V** pin to the **red (+) strip** on your breadboard
2. Take another wire and connect the ESP32's **GND** pin to the **blue (-) strip** on your breadboard

*The red strip gives electricity, the blue strip brings it back to the controller.*

### Step 2: Connect your speaker to the amplifier 🔊
1. Connect the speaker's **red (+) wire** to the amplifier's **+ terminal**
2. Connect the speaker's **black (-) wire** to the amplifier's **- terminal**

*This is how your robot will make sound!*

### Step 3: Connect the button 🔘
1. Connect one side of the button to **GPIO 3** on your ESP32
2. Connect the other side of the button to the **blue (-) strip** on your breadboard

*This is how your robot will listen for button presses!*

### Step 4: Connect the microphone (INMP441) 🎤
1. Connect the microphone's **VCC** to the **red (+) strip** on your breadboard
2. Connect the microphone's **GND** to the **blue (-) strip** on your breadboard
3. Connect the microphone's **WS** to **GPIO 2** on your ESP32
4. Connect the microphone's **SCK** to **GPIO 1** on your ESP32
5. Connect the microphone's **SD** to **GPIO 0** on your ESP32

*This is how your robot will hear you!*

### Step 5: Connect the amplifier (MAX98357A) 📢
1. Connect the amplifier's **VCC** to the **red (+) strip** on your breadboard
2. Connect the amplifier's **GND** to the **blue (-) strip** on your breadboard
3. Connect the amplifier's **DIN** to **GPIO 7** on your ESP32
4. Connect the amplifier's **BCLK** to **GPIO 9** on your ESP32
5. Connect the amplifier's **LRC** to **GPIO 10** on your ESP32

*This is how your robot will talk back to you!*

### 🎯 Quick Check - What connects where:

| Part              | Pin/Wire      | Goes to                |
|-------------------|---------------|------------------------|
| ESP32             | 3.3V          | Red (+) strip          |
| ESP32             | GND           | Blue (-) strip         |
| Button            | One side      | GPIO 3                 |
| Button            | Other side    | Blue (-) strip         |
| Microphone        | VCC           | Red (+) strip          |
| Microphone        | GND           | Blue (-) strip         |
| Microphone        | WS            | GPIO 2                 |
| Microphone        | SCK           | GPIO 1                 |
| Microphone        | SD            | GPIO 0                 |
| Amplifier         | VCC           | Red (+) strip          |
| Amplifier         | GND           | Blue (-) strip         |
| Amplifier         | DIN           | GPIO 7                 |
| Amplifier         | BCLK          | GPIO 9                 |
| Amplifier         | LRC           | GPIO 10                |
| Speaker           | Red (+) wire  | Amplifier + terminal   |
| Speaker           | Black (-) wire| Amplifier - terminal   |

---

## 🧰 Arduino IDE Setup

1. Open **Arduino IDE**
2. Go to `File > Preferences`
   - In *Additional Board URLs*, add:  
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Go to `Tools > Board > Boards Manager`
   - Search for `esp32` and install it (by Espressif Systems)
4. Go to `Tools > Board` and select:
   - Go to Tools → Select your board:
    -> **ESP32 Dev Module** for ESP32-WROOM-32
    -> **ESP32C3 Dev Module** for ESP32-C3
5. Go to `Tools > Port` and select the port labeled something like:
   - `/dev/ttyUSB0` or `usbserial-####`
6. Go to  `Tools > Manage Libraries` and install libraries:
   - ArduinoJson by Benoit Blanchon (https://arduinojson.org/)
   - WiFiManager by tzapu (https://github.com/tzapu/WiFiManager)
7. Recommended settings:
   - Flash Size: 4MB
   - Upload Speed: 115200
   - USB CDC On Boot: Enabled (This allows us to use serial monitor with usb-c)
   - Erase Flash: "All Flash Contents"

---

## 🚀 Upload

1. Plug in your ESP32-C3
2. Click ✅ Upload in Arduino
3. If upload fails, press and hold **BOOT** while clicking Upload

--- 

## 🧪 Serial Monitor

- Open `Tools > Serial Monitor`
- Set **baud rate** to `115200`
- Look for logs like:


## Set up WiFi

You can hardcode your Wi-Fi at the beginning of the arduino code. However, if you want to be able to use different wifis, you can also set up your wifi dynamically:

Once your board is connected to a power source, you should see in your computer's wifi settings a new network appear: `ESP32-Robot-Setup`. Connect to it, and in the portal which appears, click "Configure WiFi" then add your WiFi settings.

## Software (Advanced!!!)

By default, the robot will connect to a source that I have set up (which should look like `https://robot-server-782703035576.europe-west1.run.app`). 
However, you may want to change this, for example if you want to debug the server code on your own machine, or if you want to deploy your own version. 

If so, change the `server_url` variable to your server.

### Requirements

You must have an AssemblyAI and an OpenAI developer keys. 

###  To run the server locally:

You must have python 3 installed locally. Within this repo:

```
cd server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

If you are on Mac and don't yet have ffmpeg installed, install it:
```
brew install ffmpeg  # if not already installed (macOS)
```

Then start the server:
```
python proxy_server_audio.py
```