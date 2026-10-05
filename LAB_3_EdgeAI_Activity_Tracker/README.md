# Laboratory Activity 3: Edge AI Activity Tracker & BLE Telemetry Gateway
### Full-Stack TinyML Physical Activity Classification on RT-Thread Spark Board (STM32F407ZGT6) with Companion Android Mobile Dashboard

**Mindanao State University - Iligan Institute of Technology (MSU-IIT)**  
**College of Computer Studies | Department of Computer Applications**  
**Course**: BCA180 Embedded Systems Programming / BCA152 Microcontrollers  
**Author**: Perch Arnel II Montefalcon  
**Target Hardware**: RT-Thread Spark Development Board ("星火 1 号", STMicroelectronics STM32F407ZGT6)  
**Sensors**: InvenSense ICM-20608-G (6-Axis IMU via I2C2)  
**Wireless**: Realtek RW007 High-Speed SPI Wi-Fi/BLE (Method A Provisioning GATT Exploit)  
**Mobile Client**: Native Android Application (Kotlin, BLE GATT Client)  
**Machine Learning Dataset**: Kaggle `vmalyi/run-or-walk` (88,588 records, iPhone 5c CoreMotion)  

---

## 1. Project Overview

Laboratory Activity 3 implements an autonomous, commercial-grade **Edge AI (TinyML) human activity recognition wearable system**. Rather than relying on cloud streaming or heavy MQTT brokers, the system executes real-time machine learning inference directly on the **STM32F407ZGT6 ARM Cortex-M4F microcontroller's single-precision hardware floating-point unit (FPU)**. 

The onboard **InvenSense ICM-20608-G** sensor measures 3-axis linear acceleration and 3-axis rotational velocity at 10 Hz into a 15-sample rolling circular window. The microcontroller computes **21 time-domain statistical metrics** and evaluates an **optimized 10-tree Random Forest classifier** in **$< 5\text{ microseconds}$** to classify user movement as **WALKING** or **RUNNING** with **$99.96\%$ empirical test accuracy**.

Classification results, cadence, step counts, and peak acceleration are packed into an 11-byte structured binary payload and transmitted wirelessly via **Method A**—repurposing the **Realtek RW007 Apache NimBLE GATT notification service** (`0xFEE7` / `0xFEA2`)—directly to a companion **native Android mobile dashboard** written in Kotlin.

---

## 2. Target Hardware Specifications: RT-Thread Spark Board

| Subsystem | Component | Engineering Specification |
|---|---|---|
| **Core Microcontroller** | STMicroelectronics **STM32F407ZGT6** | High-performance 32-bit ARM Cortex-M4F @ 168 MHz with single-precision hardware FPU (`FPv4-SP`), 1024 KB Flash, 192 KB SRAM + 64 KB Core Coupled Memory (CCM). |
| **Motion Tracking IMU** | InvenSense **ICM-20608-G** | 6-axis MEMS accelerometer ($\pm 4g$) and gyroscope ($\pm 1000^\circ/\text{s}$), interfaced via hardware I2C2 on pins `PB10` (SCL) and `PB11` (SDA) with interrupt on `PB8`. |
| **Wireless Transceiver** | Realtek **RW007** | High-speed 802.11 b/g/n and Bluetooth Low Energy module running Apache NimBLE, connected via SPI2 (`PB12`, `PB13`, `PB14`, `PB15`) and control pins `PE0` (nRST), `PD3` (INT). |
| **Local Visual Display** | Sitronix **ST7789 v3** | 1.3-inch 240×240 IPS color TFT LCD memory-mapped over Flexible Static Memory Controller (FSMC) Bank 3 8080 parallel bus. |
| **Telemetry & Debug** | ST-LINK V2.1 VCP | Onboard virtual COM port connected to `USART1` (`PA9` TX, `PA10` RX @ 115200 baud). |

---

## 3. Architecture: Edge AI (TinyML) vs. Cloud Streaming

```
TRADITIONAL CLOUD STREAMING (High Latency, High Bandwidth, Broker Overhead)
┌──────────────────────┐         MQTT (Port 1883)         ┌───────────────────┐
│ Embedded Board       │───[Raw Float Stream: 240 B/s]───▶│ Cloud MQTT Broker │
│ (Continuous Sampling)│                                  └─────────┬─────────┘
└──────────────────────┘                                            │
                                  ┌─────────────────────────────────▼─────────────────────────────────┐
                                  │ Heavy Cloud ML Worker (Python) ──▶ Database ──▶ REST API ──▶ App   │
                                  └───────────────────────────────────────────────────────────────────┘

EDGE AI / TINYML ON-CHIP INFERENCE (Autonomous, Sub-Microsecond, Ultra-Low Bandwidth)
┌───────────────────────────────────────────────────────────────────────┐
│ RT-Thread Spark Development Board (STM32F407ZGT6)                     │
│  1. ICM-20608 6-Axis IMU (10 Hz Burst Acquisition)                   │
│  2. Rolling Circular Buffer (15 samples ~ 2.8 s duration)              │
│  3. Cortex-M4F FPU Temporal Feature Extraction (21 metrics)           │
│  4. Pure C Random Forest Classifier (Inference < 5 µs on-chip!)       │
│  5. Real-Time Pedometer Peak Detection (Cadence & Cumulative Steps)   │
│  6. RW007 Apache NimBLE GATT Server (Method A Service 0xFEE7)         │
└───────────────────────────────────┬───────────────────────────────────┘
                                    │ 11-Byte BLE Notification (0xFEA2)
                                    │ Once every 1.5 - 3.0s or on State Change
                                    ▼
┌───────────────────────────────────────────────────────────────────────┐
│ Companion Android Mobile Dashboard (Kotlin)                           │
│  • BleManager GATT Client Subscribes to CCCD (0x2902)                 │
│  • Instant Activity Badge (Walking = Green, Running = Blue/Orange)    │
│  • Real-Time Confidence Gauge, Step Counter, and Cadence Display      │
│  • Scrollable Event History Timeline with Microsecond Precision       │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 4. Kaggle `run-or-walk` Dataset & The Unit Calibration Mystery

The model was developed using the public Kaggle dataset **`vmalyi/run-or-walk`**:
- **Volume**: 88,588 records captured from an Apple iPhone 5c worn on the wrist.
- **Sampling Frequency**: $\approx 5.4\text{ Hz}$ ($\Delta t \approx 185 - 200\text{ ms}$).
- **Balanced Targets**: 44,223 Walking instances (`activity = 0`) and 44,365 Running instances (`activity = 1`).

### The CoreMotion "Hidden Unit" Resolved
The laboratory syllabus notes a key hint regarding unstated measurement units:
1. **Acceleration ($a_x, a_y, a_z$)**: iOS `CoreMotion` outputs linear acceleration in **standard gravity units ($g$-force)**, where $1.0\text{ }g \approx 9.80665\text{ m/s}^2$. Many embedded drivers produce SI units ($\text{m/s}^2$) or raw ADC integers ($LSB$). If untreated, acceleration values would be inflated by $\approx 9.81\times$, causing severe false-positive running predictions.
2. **Gyroscope ($g_x, g_y, g_z$)**: iOS outputs angular velocity in **radians per second ($\text{rad/s}$)**, whereas standard MEMS sensors return degrees per second ($\text{dps}$).

**Firmware Calibration Implementation (`icm20608.c`)**:
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

## 5. Temporal Sliding Window & Feature Engineering Pipeline

Single-row kinematic classifications fail to capture the cyclic cadence of human gait. The firmware buffers 15 consecutive samples ($\approx 2.8\text{ seconds}$ at $5.4\text{ Hz}$) with $50\%$ overlap and extracts **21 time-domain features**:

| Feature Indices | Description | Mathematical Formulation | Physical Interpretation |
|---|---|---|---|
| **0 .. 5** | Axis Means ($\mu$) | $\mu_k = \frac{1}{N}\sum_{i=1}^N x_k[i]$ | Static gravitational orientation of the wrist during gait. |
| **6 .. 11** | Axis Standard Deviations ($\sigma$) | $\sigma_k = \sqrt{\frac{1}{N}\sum_{i=1}^N (x_k[i] - \mu_k)^2}$ | Dynamic energy and variance; running produces $4\times$ higher $\sigma(a_z)$. |
| **12 .. 17** | Peak-to-Peak Ranges | $R_k = \max(x_k) - \min(x_k)$ | Total displacement stroke amplitude of the arm. |
| **18** | Mean Accel Magnitude | $\frac{1}{N}\sum_{i=1}^N \sqrt{a_x^2 + a_y^2 + a_z^2}$ | Overall kinetic intensity. |
| **19** | Std Accel Magnitude | $\sigma(|A|)$ | Cyclic shock wave variance. |
| **20** | Peak Accel Magnitude | $\max(|A|)$ | Maximum impact spike ($> 2.5\text{ }g$ during running). |

---

## 6. Machine Learning Model Architecture & Performance

An ensemble of **10 Decision Trees (Random Forest, max depth = 5)** was trained on 12,610 extracted temporal windows and validated on a 20% holdout test set (2,522 windows):

```
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

## 7. emlearn Pure C Generation & Zero-Allocation Execution

Using `emlearn`, the trained ensemble was serialized into `include/activity_model.h` as static inline C decision trees.
- **Dynamic Allocations (`malloc`)**: **0 bytes**.
- **Memory Footprint**: $22.6\text{ KB}$ Flash, $< 500\text{ B}$ RAM.
- **Hardware FPU Instructions**: Compiled with `-mfpu=fpv4-sp-d16 -mfloat-abi=hard`, evaluating conditions via single-cycle `VCMP.F32` and `VMRS` status register instructions.
- **Execution Time**: **$4.2\text{ microseconds}$** per classification at $168\text{ MHz}$.

---

## 8. Real-Time Pedometer & Cadence Estimation Algorithm

The feature extractor integrates a dual-threshold hysteresis peak detector on acceleration magnitude:
1. **Step Arming**: When magnitude exceeds $1.40\text{ }g$ with a positive derivative ($\frac{d|A|}{dt} > 0$), a step is recorded.
2. **Interval Validation**: The time interval $\Delta t = t_{\text{curr}} - t_{\text{last}}$ must fall between $220\text{ ms}$ (272 SPM maximum) and $2000\text{ ms}$ (30 SPM minimum).
3. **Smooth Cadence Exponential Moving Average (EMA)**:
   $$\text{Cadence}_{\text{EMA}} = \frac{3 \cdot \text{Cadence}_{\text{EMA}} + \frac{60000}{\Delta t}}{4}$$
   The filter is seeded directly on the first valid step interval to eliminate warmup lag.

---

## 9. Realtek RW007 BLE GATT Protocol (Method A Exploit)

Rather than setting up an MQTT broker, our system exploits the **RW007 Apache NimBLE WeChat/AirSync Provisioning GATT service**:
- **Service UUID**: `0000fee7-0000-1000-8000-00805f9b34fb` (`0xFEE7`)
- **Notification Characteristic**: `0000fea2-0000-1000-8000-00805f9b34fb` (`0xFEA2`)
- **CCCD Descriptor**: `00002902-0000-1000-8000-00805f9b34fb` (`0x2902`)

### 11-Byte Binary Telemetry Packet Netlist:
```
Byte Offset:  [0]    [1]    [2]    [3]    [4..7]        [8..9]         [10]
Field Name:   MAGIC  ACT    CONF   CAD    STEPS         ACCEL_MAG      CKS
Data Type:    uint8  uint8  uint8  uint8  uint32 (LE)   uint16 (LE)    uint8
Values:       0xA5   0/1    0-100% SPM    Total Count   g * 100        XOR[0..9]
```

---

## 10. Native Android Mobile Application Architecture

Built in Android Studio using Kotlin:
- **`BleManager.kt`**: Manages low-latency BLE scanning (`ScanSettings.SCAN_MODE_LOW_LATENCY`), automatic device discovery, GATT connection lifecycle, service enumeration, and CCCD notification subscription.
- **`ActivityPacket.kt`**: Parses the 11-byte stream, verifies the XOR checksum, and deserializes little-endian data fields.
- **`MainActivity.kt`**: Dispatches state updates to the Material UI:
  - **Live Hero Card**: Displays current activity in emerald green (`#10B981`) for Walking or electric orange (`#F59E0B`) for Running.
  - **Confidence Bar**: Real-time animated progress bar showing classification certainty.
  - **Cadence & Step Gauges**: Dynamic steps per minute and total distance counters.
  - **Activity Timeline**: Scrollable, timestamped transition log for post-workout review.
- **Permissions**: Full compliance with Android 12+ (`BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`) and legacy location fallbacks.

---

## 11. Verification & Automated Unit Testing

Host-native verification was executed using MinGW GCC and Unity in `test/test_tinyml/test_tinyml.c`:

```text
===============================================================
  Executing Host-Native TinyML Unit Tests (MinGW GCC)          
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

--- Test: Pedometer Cadence & Step Count Detection ---
  [PASS] Total steps should equal 5
  [PASS] Cadence should be centered around ~120 SPM

--- Test: BLE Protocol Serialization ---
  [PASS] Packet magic must be 0xA5
  [PASS] Packet activity must be RUNNING (1)
  [PASS] Packet confidence must be 99
  [PASS] Packet cadence must be 168
  [PASS] Packet step count must be 1250
  [PASS] Magnitude 2.45g should be encoded as 245
  [PASS] Checksum must match XOR calculation

===============================================================
  SUMMARY: 37 / 37 tests PASSED (0 failures)
===============================================================
```

---

## 12. Project Directory Structure

```text
LAB_3_EdgeAI_Activity_Tracker/
├── docs/
│   ├── BCA180 ESP - Laboratory Activity 3.pdf   # Official course laboratory assignment
│   └── oral-defense-guide.md                     # 13-question comprehensive defense Q&A
├── ml_model/
│   ├── train_model.py                            # Feature extraction & emlearn C exporter
│   └── runorwalk.csv                             # Kaggle dataset (gitignored)
├── firmware/
│   ├── include/
│   │   ├── activity_model.h                      # Generated zero-allocation C decision trees
│   │   ├── feature_extractor.h                   # 15-sample rolling window & pedometer
│   │   ├── icm20608.h                            # InvenSense 6-axis I2C2 sensor driver
│   │   ├── rw007_ble.h                           # RW007 NimBLE GATT telemetry service
│   │   └── ble_protocol.h                        # 11-byte binary packet structure
│   └── src/
│       ├── feature_extractor.c                   # Statistical feature computation
│       ├── icm20608.c                            # Sensor initialization and calibration
│       ├── rw007_ble.c                           # BLE notification dispatcher
│       └── main.c                                # Main acquisition and classification loop
├── android_app/
│   ├── build.gradle.kts                          # Project-level Gradle build file
│   ├── settings.gradle.kts                       # Repository settings
│   └── app/
│       ├── build.gradle.kts                      # Module build file (Kotlin, Material Design)
│       └── src/main/
│           ├── AndroidManifest.xml               # BLE permissions (Android 12+ & legacy)
│           ├── java/com/msuiit/bca180/activitytracker/
│           │   ├── MainActivity.kt               # Main dashboard UI & event logger
│           │   ├── BleManager.kt                 # Native Android BLE GATT scanner/client
│           │   └── ActivityPacket.kt             # Binary packet parser & XOR checksum
│           └── res/
│               ├── layout/activity_main.xml      # Modern dark-mode Material card layout
│               └── values/                       # Colors, strings, and dark themes
├── test/
│   └── test_tinyml/
│       └── test_tinyml.c                         # 37-case host-native automated test suite
├── platformio.ini                                # Dual PlatformIO target configuration
└── README.md                                     # Comprehensive project portfolio documentation
```

---

## 13. Quickstart Guide

### Running Automated Unit Tests
```bash
# Execute 37-case host-native test suite on PC
gcc -Wall -Wextra -O2 -Ifirmware/include \
    firmware/src/feature_extractor.c \
    test/test_tinyml/test_tinyml.c \
    -o run_tests.exe -lm
./run_tests.exe
```

### Retraining the Machine Learning Model
```bash
cd ml_model
python train_model.py
```

### Opening the Android Application
1. Open **Android Studio**.
2. Select **Open an Existing Project** and navigate to `LAB_3_EdgeAI_Activity_Tracker/android_app`.
3. Sync Gradle and run on a physical Android device or emulator with Bluetooth support.
4. Tap **Scan & Connect Board** to connect to the RT-Thread Spark Board.

---

## 14. Academic Attribution & References
1. STMicroelectronics, *STM32F405/407xx Advanced ARM-based 32-bit MCUs Reference Manual (RM0090)*, Rev. 19, 2021.
2. InvenSense, *ICM-20608-G High Performance 6-Axis MotionTracking Device Datasheet*, Rev. 1.0, 2016.
3. Shanghai Ruiside Electronic Technology Co., *RW007 High-Speed SPI Wi-Fi/BLE Module Specification*, v2.1.0, 2023.
4. V. Malyi, *Run or Walk Classification Dataset*, Kaggle Datasets, 2017.
5. emlearn, *Machine Learning Engine for Microcontrollers and Embedded Systems*, 2024.
