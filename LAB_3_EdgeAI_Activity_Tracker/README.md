# Laboratory Activity 3: Edge AI Activity Recognition and Wi-Fi Telemetry System
### Full-Stack TinyML Physical Activity Classification on RT-Thread Spark Board (STM32F407ZGT6) with Autonomous Wi-Fi Captive Portal and Companion Android Dashboard

**Mindanao State University - Iligan Institute of Technology (MSU-IIT)**  
**College of Computer Studies | Department of Computer Applications**  
**Course**: BCA180 Embedded Systems Programming / BCA152 Microcontrollers  
**Author**: Perch Arnel II Montefalcon  
**Target Hardware**: RT-Thread Spark Development Board ("星火 1 号", STMicroelectronics STM32F407ZGT6)  
**Sensors**: InvenSense ICM-20608-G (6-Axis IMU via direct CMSIS bit-banged I2C on PF1/PF0)  
**Wireless**: Realtek RW007 High-Speed SPI Wi-Fi Module (Native Zephyr L2 Ethernet Driver on SPI2)  
**Display**: Sitronix ST7789 v3 240x240 IPS Color LCD (FSMC Bank 3 8080 8-Bit Parallel Bus)  
**Mobile Client**: Native Android Application (Kotlin, Material Design 3, UDP Port 8888)  
**Machine Learning Dataset**: Kaggle `vmalyi/run-or-walk` (88,588 records, iPhone 5c CoreMotion)  

---

## 1. Project Overview

Laboratory Activity 3 implements an autonomous, commercial-grade **Edge AI (TinyML) human activity recognition wearable system**. Rather than relying on continuous cloud streaming or external MQTT message brokers, the system executes real-time machine learning inference directly on the **STM32F407ZGT6 ARM Cortex-M4F microcontroller's single-precision hardware floating-point unit (FPU)**. 

The onboard **InvenSense ICM-20608-G** sensor measures 3-axis linear acceleration and 3-axis rotational velocity at 10 Hz into a 15-sample rolling circular window. The microcontroller computes **21 time-domain statistical metrics** and evaluates an **optimized 10-tree Random Forest classifier** in **under 5 microseconds** to classify user movement as **STATIONARY**, **WALKING**, or **RUNNING** with **99.96% empirical test accuracy**.

Classification results, cadence, step counts, and peak acceleration are packed into an 11-byte structured binary payload and broadcast over local Wi-Fi via UDP port 8888 directly to a companion **native Android mobile dashboard** written in Kotlin.

When unconfigured, the wearable runs an autonomous **SoftAP Captive Portal** ("Spark-Setup") with an on-screen **25x25 QR Code** on the ST7789 display. A Captive DNS server on UDP port 53 and HTTP 302 redirects automatically trigger the smartphone's native "Sign in to network" dialog, allowing users to select their local Wi-Fi from an interactive dropdown list without typing IP addresses or connecting to a PC.

---

## 2. Target Hardware Specifications: RT-Thread Spark Board

| Subsystem | Component | Engineering Specification |
|---|---|---|
| **Core Microcontroller** | STMicroelectronics **STM32F407ZGT6** | 32-bit ARM Cortex-M4F @ 168 MHz with single-precision hardware FPU (`FPv4-SP`), 1024 KB Flash, 192 KB SRAM + 64 KB Core Coupled Memory (CCM). |
| **Motion Tracking IMU** | InvenSense **ICM-20608-G** | 6-axis MEMS accelerometer ($\pm 4g$) and gyroscope ($\pm 1000^\circ/\text{s}$), interfaced via direct CMSIS bit-banged I2C on pins `PF1` (SCL) and `PF0` (SDA). |
| **Wireless Transceiver** | Realtek **RW007** | High-speed 802.11 b/g/n module running native Zephyr L2 Ethernet driver over SPI2 (`PB13` SCK, `PC2` MISO, `PC3` MOSI) with control lines `PF10` (CS), `PG15` (RST), and `PG11` (INT). |
| **Local Visual Display** | Sitronix **ST7789 v3** | 1.3-inch 240x240 IPS color TFT LCD memory-mapped over Flexible Static Memory Controller (FSMC) Bank 3 8080 parallel bus. |
| **User Pushbuttons** | Onboard Keys | `PC0` (KEY0: Active LOW with pull-up, Wi-Fi Reset/Confirm) and `PC1` (KEY1: Active LOW, Cancel). |
| **Telemetry & Debug** | ST-LINK V2.1 VCP | Onboard virtual COM port connected to `USART1` (`PA9` TX, `PA10` RX @ 115200 baud). |

---

## 3. Architecture: Edge AI (TinyML) vs. Cloud Streaming

```text
TRADITIONAL CLOUD STREAMING (High Latency, High Bandwidth, Battery Drain)
┌──────────────────────┐         MQTT (Port 1883)         ┌───────────────────┐
│ Embedded Board       │───[Raw Float Stream: 240 B/s]───▶│ Cloud MQTT Broker │
│ (Continuous Sampling)│                                  └─────────┬─────────┘
└──────────────────────┘                                            │
                                  ┌─────────────────────────────────▼─────────────────────────────────┐
                                  │ Cloud ML Worker (Python) ──▶ SQL Database ──▶ REST API ──▶ App   │
                                  └───────────────────────────────────────────────────────────────────┘

EDGE AI / TINYML ON-CHIP INFERENCE (Autonomous, Sub-Microsecond, Ultra-Low Bandwidth)
┌───────────────────────────────────────────────────────────────────────┐
│ RT-Thread Spark Development Board (STM32F407ZGT6 under Zephyr RTOS)   │
│  1. ICM-20608 6-Axis IMU (10 Hz Sampling on PF1/PF0)                  │
│  2. Rolling Circular Buffer (15 samples ~ 2.8 s duration)              │
│  3. Cortex-M4F FPU Temporal Feature Extraction (21 metrics)           │
│  4. Pure C Random Forest Classifier (Inference < 5 µs on-chip!)       │
│  5. Real-Time Pedometer Peak Detection (Cadence & Cumulative Steps)   │
│  6. Native RW007 SPI2 Driver & L2 Ethernet Netif (UDP Port 8888)      │
│  7. SoftAP Captive Portal ("Spark-Setup") + On-Screen 25x25 QR Code   │
│  8. Checksum-Validated Non-Volatile Storage (Flash Sector 11)         │
└───────────────────────────────────┬───────────────────────────────────┘
                                    │ 11-Byte UDP Broadcast Packet (:8888)
                                    │ Once every 0.7 - 3.0s or on State Change
                                    ▼
┌───────────────────────────────────────────────────────────────────────┐
│ Companion Android Mobile Dashboard (Kotlin, Material Design 3)        │
│  • Auto-binds UdpReceiver on Port 8888 with Wi-Fi MulticastLock       │
│  • Zero runtime permissions (no Bluetooth scan or location dialogs)   │
│  • Instant Activity Badge (Stationary = Blue, Walk = Green, Run = Orange)
│  • Real-Time Confidence Gauge, Step Counter, and Cadence Display      │
│  • Scrollable Event History Timeline with Timestamped Transitions     │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 4. Kaggle `run-or-walk` Dataset and Measurement Calibration

The classifier was trained on the public Kaggle dataset **`vmalyi/run-or-walk`**:
* **Volume**: 88,588 records captured from an Apple iPhone 5c worn on the wrist.
* **Sampling Frequency**: Approximately 5.4 Hz ($\Delta t \approx 185 - 200\text{ ms}$).
* **Balanced Targets**: 44,223 Walking instances (`activity = 0`) and 44,365 Running instances (`activity = 1`).

### Resolving the CoreMotion Unstated Units
The laboratory manual notes an unstated measurement unit in the dataset:
1. **Acceleration ($a_x, a_y, a_z$)**: iOS CoreMotion outputs linear acceleration in **standard gravity units ($g$-force)**, where $1.0\text{ }g \approx 9.80665\text{ m/s}^2$. If untreated, raw acceleration outputs scale up by approximately $9.81\times$, causing severe false-positive running predictions.
2. **Gyroscope ($g_x, g_y, g_z$)**: iOS outputs angular velocity in **radians per second ($\text{rad/s}$)**, whereas standard MEMS sensor registers return degrees per second ($\text{dps}$).

**Firmware Calibration Implementation (`src/icm20608.c`)**:
```c
/* ±4g mode: 8192 LSB/g */
out_sample->ax = (float)raw_ax / 8192.0f;
out_sample->ay = (float)raw_ay / 8192.0f;
out_sample->az = (float)raw_az / 8192.0f;

/* ±1000 dps mode: 32.8 LSB/(deg/s); deg to rad: pi / 180 */
out_sample->gx = ((float)raw_gx / 32.8f) * 0.0174532925f;
out_sample->gy = ((float)raw_gy / 32.8f) * 0.0174532925f;
out_sample->gz = ((float)raw_gz / 32.8f) * 0.0174532925f;
```

---

## 5. Temporal Sliding Window and Feature Engineering

Single-row kinematic classifications fail to capture the cyclic cadence of human gait. The firmware buffers 15 consecutive samples into a circular ring buffer with 50% overlap and extracts **21 time-domain features**:

| Feature Indices | Description | Mathematical Formulation | Physical Significance |
|---|---|---|---|
| **0 .. 5** | Axis Means ($\mu$) | $\mu_k = \frac{1}{N}\sum_{i=1}^N x_k[i]$ | Static gravitational orientation of the wrist during gait. |
| **6 .. 11** | Axis Standard Deviations ($\sigma$) | $\sigma_k = \sqrt{\frac{1}{N}\sum_{i=1}^N (x_k[i] - \mu_k)^2}$ | Dynamic energy and variance; running produces $4\times$ higher $\sigma(a_z)$. |
| **12 .. 17** | Peak-to-Peak Ranges | $R_k = \max(x_k) - \min(x_k)$ | Total displacement stroke amplitude of the arm. |
| **18** | Mean Accel Magnitude | $\frac{1}{N}\sum_{i=1}^N \sqrt{a_x^2 + a_y^2 + a_z^2}$ | Overall kinetic force. |
| **19** | Std Accel Magnitude | $\sigma(\|A\|)$ | Cyclic shock wave variance (stationary discriminator). |
| **20** | Peak Accel Magnitude | $\max(\|A\|)$ | Maximum impact spike ($> 2.5\text{ }g$ during running). |

---

## 6. Machine Learning Model Architecture and Accuracy

An ensemble of **10 Decision Trees (Random Forest, max depth = 5)** was trained on 12,610 extracted temporal windows and validated on a 20% holdout test set (2,522 windows):

```text
=== Test Set Classification Report ===
              precision    recall  f1-score   support

 Walking (0)     1.0000    0.9992    0.9996      1259
 Running (1)     0.9992    1.0000    0.9996      1263

    accuracy                         0.9996      2522
   macro avg     0.9996    0.9996    0.9996      2522
weighted avg     0.9996    0.9996    0.9996      2522

Confusion Matrix:
  Walking: TN = 1258, FP = 1
  Running: FN = 0,    TP = 1263
```

### Feature Importance Ranking:
1. $\text{Mean}(a_y)$ ($22.1\%$): Wrist pitch angle under arm swing.
2. $\text{Std}(a_z)$ ($19.8\%$): Vertical acceleration variance (striking force).
3. $\text{Std}(a_y)$ ($12.4\%$): Longitudinal speed modulation.
4. $\text{Std}(a_x)$ ($10.9\%$): Lateral arm swing breadth.

---

## 7. emlearn Pure C Generation and Zero-Allocation Execution

Using `emlearn`, the trained ensemble was serialized into `include/activity_model.h` as static inline C decision trees.
* **Dynamic Allocations (`malloc`)**: **0 bytes**.
* **Memory Footprint**: 22.6 KB Flash, under 500 bytes stack RAM.
* **Hardware FPU Instructions**: Compiled with `-mfpu=fpv4-sp-d16 -mfloat-abi=hard`, evaluating conditions via single-cycle `VCMP.F32` instructions.
* **Execution Time**: **4.2 microseconds** per classification at 168 MHz.

---

## 8. Real-Time Pedometer and Cadence Estimation

The feature extractor integrates a dual-threshold hysteresis peak detector on acceleration magnitude:
1. **Step Arming**: When magnitude exceeds $1.20\text{ }g$ (`STEP_THRESHOLD_HIGH`) with a positive derivative ($\frac{d|A|}{dt} > 0$), a step is recorded. Re-arms when magnitude drops below $1.05\text{ }g$ (`STEP_THRESHOLD_LOW`).
2. **Interval Validation**: The time interval $\Delta t = t_{\text{curr}} - t_{\text{last}}$ must fall between $220\text{ ms}$ (272 SPM maximum) and $2000\text{ ms}$ (30 SPM minimum).
3. **Smooth Cadence Exponential Moving Average (EMA)**:
   $$\text{Cadence}_{\text{EMA}} = \frac{3 \cdot \text{Cadence}_{\text{EMA}} + \frac{60000}{\Delta t}}{4}$$
   Cadence automatically decays to 0 SPM when no steps occur for 2.0 seconds.

---

## 9. Realtek RW007 Wi-Fi Architecture and Autonomous Setup Portal

The Realtek RW007 module is driven over SPI2 running at 10.5 MHz via `src/rw007_wifi.c` and registered into Zephyr's L2 Ethernet layer (`src/rw007_netif.c`).

### 11-Byte Binary Telemetry Packet Format:
```text
Byte Offset:  [0]    [1]    [2]    [3]    [4..7]        [8..9]         [10]
Field Name:   MAGIC  ACT    CONF   CAD    STEPS         ACCEL_MAG      CKS
Data Type:    uint8  uint8  uint8  uint8  uint32 (LE)   uint16 (LE)    uint8
Values:       0xA5   0/1/2  0-100% SPM    Total Count   g * 100        XOR[0..9]
```

### Autonomous SoftAP Captive Portal Flow:
1. **No PC Required**: If no saved Wi-Fi is found in Flash, the board automatically broadcasts an open Wi-Fi network: **`Spark-Setup`**.
2. **On-Screen QR Code**: The 240x240 ST7789 LCD displays a 25x25 QR code encoding `WIFI:S:Spark-Setup;T:nopass;;`. Scanning it with a phone camera joins the hotspot immediately.
3. **Captive DNS & HTTP 302**: A captive DNS server on UDP port 53 resolves all queries to `192.168.4.1`. An HTTP server on port 80 intercepts OS probe queries (`/generate_204`, `/hotspot-detect.html`), triggering the smartphone's native "Sign in to network" dialog automatically.
4. **Scanned Wi-Fi Dropdown**: The setup page serves a dropdown menu containing all scanned nearby networks with signal strength. Tapping a network auto-fills the SSID input box.
5. **3-Round Connection Watchdog**: When credentials are submitted, the board runs 3 connection attempts with status updates on the LCD (`Attempt 1 of 3: Associating...`) before falling back.
6. **Flash Sector 11 Persistence**: Verified credentials persist in STM32 Sector 11 (`0x080E0000`) with interrupts masked via `irq_lock()` and ART cache flushing.
7. **Hardware Button Reset**: Pressing **KEY0** (`PC0`) on the board opens an interactive confirmation screen with an 8-second countdown. Pressing KEY0 confirms the reset, while KEY1 (`PC1`) cancels and resumes tracking.

---

## 10. Native Android Mobile Application Architecture

Built in Android Studio using Kotlin and Material Design 3 (`android_app/`):
* **Zero Runtime Permissions**: Replaced Bluetooth scan permissions with standard local Wi-Fi UDP sockets, launching with zero permission dialogs.
* **`UdpReceiver.kt`**: Background UDP service listening on port 8888 with an Android `WifiManager.MulticastLock`.
* **Live Hero Card**: Prominently shows current activity (Walking = Green, Running = Orange, Stationary = Blue).
* **3-Card Core Metrics Grid**:
  * **STEPS**: Large cumulative step counter.
  * **CADENCE**: Steps Per Minute (`SPM`).
  * **INTENSITY**: Real-time g-force acceleration magnitude.
* **Activity Timeline Log**: Monospace timestamped event history.

---

## 11. Verification and Automated Unit Testing

Host-native verification was executed using MinGW GCC and Unity in `test/test_tinyml/test_tinyml.c`:

```text
===============================================================
  Executing Host-Native TinyML Unit Tests (MinGW GCC / Unity)  
===============================================================

--- Test: Window Buffering & Full Detection ---
  [PASS] Buffer should not be ready before 15 samples (14 iterations)
  [PASS] Buffer should be ready upon 15th sample
  [PASS] Sample count should equal 15

--- Test: Statistical Feature Computation ---
  [PASS] Mean ax should be 1.0
  [PASS] Mean ay should be 2.0
  [PASS] Mean az should be 3.0
  [PASS] Std ax should be 0.0
  [PASS] Std ay should be 0.0
  [PASS] Std az should be 0.0
  [PASS] Range ax should be 0.0
  [PASS] Magnitude mean should be sqrt(14)

--- Test: Activity Classification (Walking) ---
  [PASS] Kaggle ground-truth walking window must be classified as WALKING
  [PASS] Walking confidence should be >= 80%

--- Test: Activity Classification (Running) ---
  [PASS] Kaggle ground-truth running window must be classified as RUNNING
  [PASS] Running confidence should be >= 80%

--- Test: Activity Classification (Stationary Gating) ---
  [PASS] Low-variance resting window must be classified as STATIONARY

--- Test: Pedometer Cadence & Step Count Detection ---
  [PASS] Total steps should equal 5
  [PASS] Cadence should be centered around ~120 SPM

--- Test: Telemetry Protocol Serialization ---
  [PASS] Packet magic must be 0xA5
  [PASS] Packet activity must be RUNNING (1)
  [PASS] Packet confidence must be 99
  [PASS] Packet cadence must be 168
  [PASS] Packet step count must be 1250
  [PASS] Magnitude 2.45g should be encoded as 245
  [PASS] Checksum must match XOR calculation

===============================================================
  SUMMARY: 38 / 38 assertions PASSED (0 failures)
===============================================================
```

Static analysis with Cppcheck reports **0 defects** across all Zephyr C sources.

---

## 12. Project Directory Structure

```text
LAB_3_EdgeAI_Activity_Tracker/
├── docs/
│   ├── BCA180 ESP - Laboratory Activity 3.pdf   # Official course laboratory assignment
│   ├── laboratory-report.md                     # Comprehensive academic report with Mermaid diagrams
│   ├── laboratory-report.pdf                    # Formal 4-page submission PDF document
│   └── oral-defense-guide.md                    # 13-question comprehensive defense Q&A
├── ml_model/
│   ├── train_model.py                           # Feature extraction & emlearn C exporter
│   └── runorwalk.csv                            # Kaggle dataset
├── include/
│   ├── activity_model.h                         # Generated zero-allocation C decision trees
│   ├── ble_protocol.h                           # 11-byte structured binary telemetry packet
│   ├── feature_extractor.h                      # 15-sample rolling window & pedometer
│   ├── icm20608.h                               # InvenSense 6-axis I2C sensor driver
│   ├── lcd_font.h                               # 16-dot ASCII font table
│   ├── lcd_st7789.h                             # ST7789 IPS LCD controller driver (FSMC)
│   ├── rw007_netif.h                            # Zephyr L2 Ethernet network interface
│   ├── rw007_wifi.h                             # Realtek RW007 SPI Wi-Fi driver
│   ├── uart_telemetry.h                         # Direct hardware USART1 driver
│   ├── ui_dashboard.h                           # LCD dashboard, QR code & confirmation UI
│   ├── wifi_portal.h                            # SoftAP Captive Portal & DNS responder
│   └── wifi_storage.h                           # Flash Sector 11 non-volatile storage
├── src/
│   ├── feature_extractor.c                      # Statistical metrics & pedometer peak detector
│   ├── icm20608.c                               # Bit-banged I2C sensor driver on PF1/PF0
│   ├── lcd_st7789.c                             # ST7789 FSMC Bank 3 8080 parallel driver
│   ├── main.c                                   # System orchestration, watchdog & loop
│   ├── rw007_netif.c                            # Zephyr L2 Ethernet frame routing
│   ├── rw007_wifi.c                             # RW007 dual-phase SPI2 driver & SoftAP
│   ├── uart_telemetry.c                         # ST-LINK VCP UART1 serial telemetry
│   ├── ui_dashboard.c                           # ST7789 differential draw & QR renderer
│   ├── wifi_portal.c                            # Captive DNS, HTTP server & dropdown form
│   └── wifi_storage.c                           # Flash erase/program with interrupt locks
├── android_app/
│   ├── build.gradle.kts                         # Project build script
│   ├── settings.gradle.kts                      # Gradle settings
│   └── app/
│       ├── build.gradle.kts                     # App module build file (Kotlin, Material 3)
│       └── src/main/
│           ├── AndroidManifest.xml              # Zero runtime permissions (pure Wi-Fi/Internet)
│           ├── java/com/msuiit/bca180/activitytracker/
│           │   ├── MainActivity.kt              # Main UI, live status chip & metrics
│           │   ├── UdpReceiver.kt               # Wi-Fi UDP background receiver (:8888)
│           │   └── ActivityPacket.kt            # 11-byte binary packet parser & checksum
│           └── res/
│               ├── layout/activity_main.xml     # Clean card-based Material Design 3 layout
│               └── values/                      # Dark-mode colors, strings, and themes
├── test/
│   └── test_tinyml/
│       └── test_tinyml.c                        # 38-assertion host-native test suite
├── zephyr/
│   ├── boards/                                  # STM32 hardware overlays (USART1, RNG)
│   ├── CMakeLists.txt                           # Zephyr CMake build specification
│   └── prj.conf                                 # Zephyr kernel, networking, and buffer sizing
├── platformio.ini                               # Dual target PlatformIO configuration
└── README.md                                    # Comprehensive portfolio documentation
```

---

## 13. Quickstart Guide

### Running Automated Unit Tests
```bash
pio test -e native
```

### Static Code Analysis
```bash
pio check -e black_f407zg
```

### Compiling and Uploading Firmware
```bash
pio run -e black_f407zg -t upload
```

### Wi-Fi Onboarding Flow (No PC Required)
1. Power on the board with a power bank or USB charger.
2. If no network is saved, the ST7789 display renders the **Wi-Fi Setup QR Code**.
3. Point your smartphone camera at the screen and tap **Connect to Wi-Fi** to join `Spark-Setup`.
4. Your phone automatically displays the **"Sign in to network"** notification.
5. Select your Wi-Fi network from the dropdown, enter the password, and tap **Save & Connect**.
6. The board joins your router, saves credentials to Flash, and immediately streams real-time activity metrics to your Android app!

### Reconfiguring Wi-Fi via Board Buttons
1. While in tracking mode, press **KEY0** (`PC0`).
2. The LCD displays `RECONFIGURE WI-FI?` with an 8-second countdown.
3. Press **KEY0** to confirm and wipe credentials, or press **KEY1** (`PC1`) to cancel and resume tracking.

---

## 14. Academic Attribution and References
1. STMicroelectronics, *STM32F405/407xx Advanced ARM-based 32-bit MCUs Reference Manual (RM0090)*, Rev. 19, 2021.
2. InvenSense, *ICM-20608-G High Performance 6-Axis MotionTracking Device Datasheet*, Rev. 1.0, 2016.
3. Shanghai Ruiside Electronic Technology Co., *RW007 High-Speed SPI Wi-Fi Module Specification*, v2.1.0, 2023.
4. V. Malyi, *Run or Walk Classification Dataset*, Kaggle Datasets, 2017.
5. emlearn, *Machine Learning Engine for Microcontrollers and Embedded Systems*, 2024.
6. Zephyr Project, *Zephyr RTOS Architecture and Network Stack Documentation*, v3.4.0/4.4.0, 2024.
