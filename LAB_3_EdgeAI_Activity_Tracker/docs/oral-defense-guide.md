# Oral Defense Technical Guide: Laboratory Activity 3 (Activity Recognition)
## Edge AI Physical Activity Classifier & RW007 BLE Telemetry Gateway
**Mindanao State University - Iligan Institute of Technology (MSU-IIT)**  
**College of Computer Studies | Department of Computer Applications**  
**Course**: BCA180 Embedded Systems Programming / BCA152 Microcontrollers  

---

## Part 1: System Overview & Edge AI Architecture

### Q1: What is the core engineering objective of Laboratory Activity 3?
**Answer**:  
The objective is to design and build an end-to-end wearable activity classification system that detects human physical activity (**Walking** versus **Running**) using the 6-axis inertial motion sensors (accelerometer and gyroscope) on the RT-Thread Spark Development Board (`STM32F407ZGT6`) and transmits telemetry to a companion Android smartphone application.

### Q2: Why did you choose on-device Edge AI (TinyML) instead of raw sensor streaming to a cloud server?
**Answer**:  
We evaluated both architectures and chose **Edge AI** for four major engineering advantages:
1. **Bandwidth Reduction (99.5% savings)**: Streaming raw 6-axis floating-point sensor data at $10\text{ Hz}$ consumes $\approx 240\text{ bytes/second}$ of continuous network payload. Running inference on-chip compresses the stream to a single $11\text{ byte}$ notification packet sent once every $1.5\text{ to }3.0\text{ seconds}$ or strictly upon activity state transitions.
2. **Deterministic Real-Time Latency**: Cloud offloading incurs round-trip network delays ($50 - 200\text{ ms}$) plus broker queue jitter. On-device execution on the ARM Cortex-M4F hardware FPU evaluates all 10 decision trees in **$< 5\text{ microseconds}$** at $168\text{ MHz}$.
3. **RF Power Efficiency**: Radio transmission (Wi-Fi/Bluetooth) is the highest power consumer on embedded wearable nodes. Reducing RF active transmission duty cycle by over $95\%$ dramatically extends battery life.
4. **Autonomous Offline Functionality**: The wearable functions as an independent pedometer/activity monitor even when disconnected from networks or smartphone gateways.

---

## Part 2: Machine Learning & Signal Processing Pipeline

### Q3: What dataset was utilized, and what were its key characteristics?
**Answer**:  
We trained on the Kaggle **`vmalyi/run-or-walk`** dataset containing **88,588 records** captured at $\approx 5.4\text{ Hz}$ across walking and running activities with an iPhone 5c on the subject's wrist. The data fields include 3-axis linear acceleration ($a_x, a_y, a_z$) and 3-axis rotational velocity ($g_x, g_y, g_z$).

### Q4: The laboratory manual notes a "key hint about the measurement unit used to represent the sensor data." What was this hint, and how did you resolve it?
**Answer**:  
The Kaggle dataset was captured using iOS `CoreMotion`:
- **Acceleration** is provided in units of **standard gravity ($g$-force)**, where $1.0\text{ }g \approx 9.80665\text{ m/s}^2$, rather than SI $\text{m/s}^2$. If firmware were to stream raw $\text{m/s}^2$ without conversion, readings would be scaled up by $\approx 9.81\times$, causing severe false-positive running predictions.
- **Gyroscope** is recorded in **radians per second ($\text{rad/s}$)** rather than degrees per second ($\text{dps}$).
Our firmware's sensor driver (`icm20608.c`) calibrates the onboard InvenSense ICM-20608-G outputs into exact $g$-forces ($1.0\text{ }g = 8192\text{ LSB}$ at $\pm 4g$ full scale) and radians per second ($\text{dps} \times \pi / 180^\circ$ at $\pm 1000\text{ dps}$ full scale) to maintain identical feature distributions with the training domain.

### Q5: Why is instantaneous single-sample classification flawed, and how does your temporal feature window solve this?
**Answer**:  
Human gait is cyclic. During arm swing cycles in walking and running, instantaneous kinematic values cross overlapping velocity boundaries. Classifying isolated samples produces noisy, oscillating predictions.  
To capture temporal dynamics, our firmware maintains a **15-sample rolling circular window** ($\approx 2.8\text{ seconds}$ at $5.4\text{ Hz}$, with $50\%$ overlap). For each window, we extract **21 statistical features**:
- **6 Means ($\mu$)**: Baseline gravitational orientation.
- **6 Standard Deviations ($\sigma$)**: Signal variance and energy; running exhibits significantly higher $\sigma(a_z)$ and $\sigma(a_y)$.
- **6 Peak-to-Peak Ranges ($\max - \min$)**: Dynamic stroke amplitude.
- **3 Vector Magnitude Metrics**: $\text{Mean}$, $\text{Std}$, and $\text{Max}$ of $\sqrt{a_x^2 + a_y^2 + a_z^2}$, capturing shock impacts.

### Q6: What machine learning model was chosen, and how is it executed on the microcontroller?
**Answer**:  
We trained a **10-Tree Random Forest Classifier** ($depth = 5$).  
- **Test Set Accuracy**: **$99.96\%$** ($F_1\text{-score} = 0.9996$, with only 1 false positive across 2,522 test windows).
- **Embedded C Compilation**: Using `emlearn`, the trained ensemble was compiled into pure static inline C functions (`activity_model.h`). It requires **zero dynamic memory allocation** (`malloc = 0 B`), consumes $\approx 22\text{ KB}$ of Flash, and executes directly on the Cortex-M4F single-precision floating-point unit (`FPv4-SP`).

---

## Part 3: Hardware Interfacing & BLE Provisioning Exploit

### Q7: What motion sensor is on the RT-Thread Spark board, and how is it interfaced?
**Answer**:  
The board features an onboard **InvenSense ICM-20608-G** 6-axis MotionTracking device. According to `schematic.pdf`, it is connected to **I2C2** on pins **PB10 (SCL)** and **PB11 (SDA)** with interrupt pin **PB8 (ICM_INT)**. It is configured for $\pm 4g$ accelerometer range, $\pm 1000\text{ dps}$ gyroscope range, and 44.8 Hz digital low-pass filtering (DLPF).

### Q8: What wireless module is present, and how does "Method A" work without MQTT?
**Answer**:  
The board incorporates a **Realtek RW007** high-speed SPI module running an embedded **Apache NimBLE** Bluetooth Low Energy stack.  
Although the manufacturer firmware advertises peripheral BLE mode primarily for WeChat Wi-Fi provisioning, we exploited this existing GATT architecture (**Method A**):
- Service UUID: **`0xFEE7`** (AirSync / Provisioning Service)
- Notification Characteristic: **`0xFEA2`** (Asynchronous Status Characteristic)
Instead of returning Wi-Fi status codes, our firmware transmits an 11-byte structured binary telemetry packet over characteristic `0xFEA2`. The smartphone app subscribes to notifications via the Client Characteristic Configuration Descriptor (`0x2902`), establishing an instant, wireless telemetry stream with zero MQTT broker overhead.

### Q9: Describe the binary structure of your 11-byte BLE telemetry packet.
**Answer**:  
The packet is packed tightly (`#pragma pack(push, 1)`) into 11 bytes:
```
Offset  Field            Type        Description
[0]     magic            uint8_t     Sync byte (0xA5)
[1]     activity         uint8_t     0 = Walking, 1 = Running
[2]     confidence       uint8_t     Model confidence percentage (0..100)
[3]     cadence_spm      uint8_t     Pedometer cadence (steps/min)
[4..7]  step_count       uint32_t    Cumulative step count (little-endian)
[8..9]  accel_mag_x100   uint16_t    Acceleration magnitude in g * 100
[10]    checksum         uint8_t     XOR checksum across bytes [0..9]
```
The client validates both the `0xA5` magic prefix and the XOR checksum before updating the user interface, preventing corrupt packets from displaying.

---

## Part 4: Android Mobile Application Architecture

### Q10: How does the Android mobile application process the incoming BLE stream?
**Answer**:  
The app is developed in **Kotlin** following modern Android architectural standards:
1. **`BleManager.kt`**: Scans for the Spark Board advertisement (`ScanSettings.SCAN_MODE_LOW_LATENCY`), initiates connection, discovers Service `0xFEE7`, enables local notifications on Characteristic `0xFEA2`, and writes `ENABLE_NOTIFICATION_VALUE` to CCCD descriptor `0x2902`.
2. **`ActivityPacket.kt`**: Validates the 11-byte binary payload, verifies the XOR checksum, extracts integer/float fields using `ByteBuffer` (little-endian), and maps the activity enum.
3. **`MainActivity.kt`**: Posts parsed packets to the main UI thread to animate the Live Activity Badge, update the confidence meter progress bar, update the cadence/step counters, and append timestamped transition records to the activity history timeline.

### Q11: How are modern Android 12+ (API 31+) Bluetooth permissions handled?
**Answer**:  
Android 12 separated Bluetooth into granular runtime permissions. Our `AndroidManifest.xml` and `MainActivity.kt` request:
- `BLUETOOTH_SCAN` (with `neverForLocation` flag)
- `BLUETOOTH_CONNECT`
For Android 11 and older, it falls back to `ACCESS_FINE_LOCATION` and `BLUETOOTH_ADMIN`.

---

## Part 5: Verification, Testing & Academic Rigor

### Q12: How was the software verified prior to deployment?
**Answer**:  
We implemented an automated host-native unit test suite (`test/test_tinyml/test_tinyml.c`) running MinGW GCC and Unity:
- **37 / 37 automated tests passing (0 failures)**.
- Verified: Window buffer ring management, 21-feature statistical math precision, ground-truth Kaggle walking classification ($\ge 80\%$ confidence), ground-truth Kaggle running classification ($\ge 80\%$ confidence), pedometer cadence peak-detection and moving average initialization, and BLE packet serialization/XOR checksum verification.

### Q13: What is the computational complexity of the on-device feature extraction and model inference?
**Answer**:  
- **Feature Extraction**: $O(N)$ where $N = 15$ samples. Two linear passes compute sum, min/max, mean, variance, and vector magnitude. Total operations: $\approx 360$ floating-point instructions.
- **Inference**: $O(D \times T)$ where $D = 5$ (tree depth) and $T = 10$ (number of trees). At most 50 floating-point comparisons (`VCMP.F32`) evaluate all trees.
At $168\text{ MHz}$, total execution time is under **$5\text{ microseconds}$**, leaving $> 99.9\%$ of CPU cycles available for sensor acquisition, UI, and power management.
