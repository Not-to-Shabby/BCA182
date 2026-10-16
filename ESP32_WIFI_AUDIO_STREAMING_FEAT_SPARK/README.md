# ESP32-S3 Wi-Fi Audio Streaming Co-Processor (featuring RT-Thread RT-Spark)

This standalone firmware enables the **Espressif ESP32-S3** running **Zephyr RTOS** to act as a dedicated **Wi-Fi Audio Streaming & DLNA Receiver**, offloading all wireless networking and packet jitter from the **RT-Thread RT-Spark (STM32F407 @ 240 MHz)** development board.

---

## 1. System Architecture

```text
[Android Smartphone (Spotify / YouTube / FLAC)]
              │
              ▼  Wi-Fi (Lossless PCM / DLNA / UDP @ 44.1kHz 16-bit Stereo)
         [ESP32-S3]
              │
              │  3-Wire High-Speed UART Link @ 2.0 Mbps
              │  • GPIO 17 (TX) ──► RT-Spark Pin 10 (PA3 / USART2_RX)
              │  • GPIO 18 (RX) ◄── RT-Spark Pin 8  (PA2 / USART2_TX)
              │  • GND          ──► RT-Spark GND
              ▼
  [RT-Thread RT-Spark Board]
   ├──► ST7789 240×240 IPS LCD (Live Song Title, Artist, Wi-Fi Signal, Stereo VU Meters)
   ├──► 19-LED SK6805 RGB Matrix (Music-reactive real-time color visualizer)
   ├──► Onboard Buttons (Play/Pause, Skip, Volume control sent back to Phone)
   └──► Everest ES8388 Codec (3.5mm Headphone Jack Hi-Fi Output)
```

---

## 2. Hardware Wiring (ESP32-S3 Super Mini)

Connect the **ESP32-S3 Super Mini** side header pins directly to the RT-Spark's **40-Pin Header (`P2`)**:

| ESP32-S3 Super Mini Pin | Header Location | Function | RT-Spark Board Pin | Header `P2` Pin |
| :--- | :--- | :--- | :--- | :---: |
| **`TX` (GPIO 43)** | Right Header | UART1 TX (Audio PCM + Metadata) | **`PA3`** (`USART2_RX`) | **Pin 10** |
| **`RX` (GPIO 44)** | Right Header | UART1 RX (Button Playback Controls) | **`PA2`** (`USART2_TX`) | **Pin 8** |
| **`GND`** | Left Header | Common Ground | **`GND`** | **Pin 4, 6, or 9** |
| **`5V` (VBUS)** | Left Header | Board Power (Powers the Super Mini) | **`+5V_SYS`** | **Pin 2** |

*Note: You do NOT need to solder to the tiny pads on the underside of the Super Mini! The pins labeled `TX` and `RX` on the outer breadboard headers are `GPIO 43` and `GPIO 44`, which connect directly with female-to-female jumper wires.*

---

## 3. Wi-Fi Modes & Smart Discovery

1. **Boot Handshake:**  
   On startup, the ESP32-S3 pings the RT-Spark board over UART for 2 seconds.
   * If detected: Enters **Spark-Controlled Mode** (Wi-Fi network selection and scans can be driven directly from the RT-Spark LCD).
   * If not detected: Enters **Auto Hotspot + Home Wi-Fi** mode:
     - Tries connecting to home Wi-Fi.
     - If unavailable, launches its own standalone Wi-Fi Access Point:
       - **SSID:** `RT-Spark-Audio`
       - **Password:** `12345678`
       - **IP Address:** `192.168.4.1`

---

## 4. How to Stream Audio from Android

### A. Universal Streaming (Spotify, YouTube, Apple Music, Netflix)
1. Install **AirMusic** from the Google Play Store (works on Android 10+ without root).
2. Connect your phone's Wi-Fi to **`RT-Spark-Audio`** (or your home network).
3. In AirMusic, tap **"RT-Spark Hi-Fi"**.
4. Play any music on Spotify or YouTube—it streams losslessly to your RT-Spark board!

### B. High-Res File Casting (FLAC / WAV / MP3)
1. Open **BubbleUPnP**, **VLC**, or **Hi-Fi Cast** on Android.
2. Select **"RT-Spark Hi-Fi"** as the DLNA Renderer.
3. Song title, artist, and progress are transmitted automatically to the RT-Spark LCD.

### C. Custom Android App (Low Latency UDP)
Use the included Kotlin snippet at `android_client/AudioStreamHelper.kt` to stream raw audio chunks directly to UDP port `44100`.

---

## 5. Building & Flashing

```bash
cd D:\BCA182_Lab\ESP32_WIFI_AUDIO_STREAMING_FEAT_SPARK
pio run -t upload
```
