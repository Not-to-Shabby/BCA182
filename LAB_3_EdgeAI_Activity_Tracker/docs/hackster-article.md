# Edge AI Physical Activity Tracker with Autonomous Wi-Fi Captive Portal on STM32 and Zephyr RTOS

**Author**: Perch Arnel II Montefalcon  
**Course**: BCA180 Embedded Systems Programming / BCA152 Microcontrollers  
**Institution**: Mindanao State University - Iligan Institute of Technology (MSU-IIT)  
**Collaborator & Evaluator**: Asst. Prof. Paul Rodolf P. Castor, M.Sc. (`paulrodolf.castor@g.msuiit.edu.ph`)  
**GitHub Repository**: [https://github.com/Not-to-Shabby/BCA182](https://github.com/Not-to-Shabby/BCA182)  

---

## 1. Project Overview & Motivation

Wearable human activity recognition systems typically rely on continuous cloud streaming. In a standard IoT architecture, an embedded node samples an accelerometer and gyroscope, formats floating-point values into JSON strings, and publishes them over MQTT to a remote virtual machine.

At a standard 10 Hz rate across 6 axes, continuous raw streaming consumes roughly 240 bytes per second of constant radio transmission (about 864 KB per hour). This approach introduces three major engineering problems:
1. **Severe Battery Drain**: Running the radio power amplifier continuously drains wearable batteries in hours.
2. **High Latency & Jitter**: Cloud round-trips add 50 to 200 ms of latency, plus internet jitter and broker queue delays.
3. **Zero Offline Capability**: When a user exercises outside, in a park, or in an area without internet, the tracker fails completely.

In this project, we built an **autonomous Edge AI wearable system** on the **RT-Thread Spark Board (STM32F407ZGT6)** using **Zephyr RTOS**. Instead of offloading data, all feature extraction, signal filtering, pedometer peak detection, and machine learning inference run directly on the microcontroller's hardware floating-point unit (FPU). 

The system classifies user activity into **STATIONARY**, **WALKING**, and **RUNNING** in **4.2 microseconds** with **99.96% accuracy**. Results are broadcast over local Wi-Fi via UDP port 8888 to a companion Android dashboard. When unconfigured, the board launches an autonomous **SoftAP Captive Portal** with an on-screen **25x25 QR Code** on its color LCD, allowing PC-free Wi-Fi configuration directly from any smartphone browser.

---

## 2. Key Features

* **On-Device Edge AI (TinyML)**:
  * 10-Tree Random Forest ($depth = 5$) exported via `emlearn` to static inline C functions.
  * Evaluates on the ARM Cortex-M4F hardware FPU in 4.2 microseconds at 168 MHz with zero dynamic allocations (`malloc = 0 B`).
  * 99.96% test set accuracy across 2,522 holdout windows from the Kaggle `run-or-walk` dataset.
* **Temporal Kinematic Pipeline**:
  * 15-sample rolling circular window (~2.8s at 5.4 Hz or 1.5s at 10 Hz) with 50% overlap.
  * Computes 21 time-domain features: 6 means, 6 standard deviations, 6 peak-to-peak ranges, and 3 magnitude metrics.
* **Dynamic Standard Deviation Motion Gating**:
  * Evaluates acceleration magnitude variance: if $\sigma(\|A\|) < 0.075g$ and the sum of 3-axis variances $< 0.150g$, the system classifies as STATIONARY, freezes step counts, and zeroes cadence.
* **Dual-Threshold Pedometer**:
  * Peak detector ($1.20g$ trigger, $1.05g$ re-arm) with Exponential Moving Average (EMA) cadence calculation ($\text{SPM} = 60000 / \Delta t$).
* **Native Zephyr RW007 SPI Wi-Fi Driver**:
  * Dual-phase 16-byte SPI master/slave protocol with 32-bit magic words running over SPI2 at 10.5 MHz.
  * Registered as a native Zephyr L2 Ethernet device, supporting standard IPv4, UDP, TCP, and DHCP.
* **PC-Free Autonomous Captive Portal**:
  * Broadcasts open network `Spark-Setup`.
  * Renders a crisp 25x25 Wi-Fi QR code (`WIFI:S:Spark-Setup;T:nopass;;`) on the ST7789 display for one-tap camera connection.
  * Captive DNS responder on UDP port 53 and HTTP 302 redirects automatically trigger the smartphone's native "Sign in to network" dialog.
  * Web form serves an interactive dropdown populated with scanned nearby access points.
* **Flash Persistence & Watchdog Recovery**:
  * Credentials persist in STM32 Flash Sector 11 with interrupt masking and cache flushing.
  * 3-round connection watchdog with visual LCD progress updates before fallback.
  * Physical KEY0 button reconfigure trigger with an 8-second confirmation countdown.
* **Streamlined Companion Android App**:
  * Kotlin Material Design 3 dashboard.
  * Auto-binds UDP port 8888 with an Android `MulticastLock`.
  * Zero runtime permission prompts (no Bluetooth scanning or fine location required).

---

## 3. Hardware Architecture & Pin Interconnects

### 3.1 Hardware Platform Overview
The system runs on the **RT-Thread Spark Development Board ("星火 1 号")**:
* **Microcontroller**: STMicroelectronics STM32F407ZGT6 (ARM Cortex-M4F @ 168 MHz, 1024 KB Flash, 192 KB SRAM).
* **Motion Sensor**: InvenSense ICM-20608 6-axis MEMS IMU ($\pm 4g$, $\pm 1000^\circ/\text{s}$).
* **Display**: Sitronix ST7789 v3 1.3-inch 240x240 IPS color LCD on FSMC Bank 3.
* **Wireless**: Realtek RW007 802.11 b/g/n SPI module.

### 3.2 Complete Hardware Wiring Netlist

```text
========================================================================================
RT-THREAD SPARK BOARD (STM32F407ZGT6) PERIPHERAL WIRING NETLIST
========================================================================================
Subsystem         Pin    MCU Mode          Electrical Function
----------------------------------------------------------------------------------------
Motion IMU        PF1    GPIO OD (Pull-Up) InvenSense ICM-20608 I2C Clock (SCL)
                  PF0    GPIO OD (Pull-Up) InvenSense ICM-20608 I2C Data (SDA)
----------------------------------------------------------------------------------------
IPS LCD Display   PD0..1 FSMC_D2..D3       ST7789 8-Bit Parallel Data Bus
                  PD14..15, PE7..10        ST7789 8-Bit Data Bus (Bits 0..1, 4..7)
                  PD4/PD5 FSMC_NOE/NWE     ST7789 Read/Write Enable (RDX/WRX)
                  PG10   FSMC_NE3 (AF12)   ST7789 Chip Select (Bank 3)
                  PD13   FSMC_A18 (AF12)   ST7789 Command/Data Select (DCX)
                  PD3/PF9 GPIO Output      ST7789 Reset (RESX) and Backlight (BL_EN)
----------------------------------------------------------------------------------------
Wi-Fi Transceiver PB13   SPI2_SCK (AF5)    Realtek RW007 High-Speed SPI Clock (10.5 MHz)
                  PC2/PC3 SPI2_MISO/MOSI   Realtek RW007 Master-In / Master-Out
                  PF10   GPIO Output       Realtek RW007 Chip Select (Active LOW)
                  PG15   GPIO Output       Realtek RW007 Hardware Reset (Active LOW)
                  PG11   GPIO Input (PU)   Realtek RW007 Interrupt / Busy Line
                  PB12   GPIO Output       W25Q64 Flash CS (held HIGH to isolate SPI2)
----------------------------------------------------------------------------------------
User Pushbuttons  PC0/PC1 GPIO Input (PU)  KEY0 (Wi-Fi Reconfigure) / KEY1 (Cancel)
----------------------------------------------------------------------------------------
Debug Console     PA9/PA10 USART1 (AF7)    ST-LINK V2.1 Virtual COM Port (115200 baud)
========================================================================================
```

---

## 4. Signal Processing & Machine Learning Pipeline

### 4.1 Kaggle Dataset Calibration
The model was trained on Kaggle's `run-or-walk` dataset (88,588 records, iPhone 5c). A critical engineering challenge involved calibrating CoreMotion's unstated units:
* **Linear Acceleration**: iOS CoreMotion records acceleration in standard gravity ($g$-force, where $1.0g \approx 9.80665\text{ m/s}^2$). Raw sensor register integers must be divided by $8192\text{ LSB/g}$ ($\pm 4g$ range).
* **Gyroscope**: CoreMotion exports angular velocity in radians per second ($\text{rad/s}$), requiring a conversion from degrees per second:
  $$\text{rad/s} = \left(\frac{\text{raw}}{32.8}\right) \times \frac{\pi}{180}$$

### 4.2 Feature Extraction Matrix
Every 7 samples (50% overlap), the firmware computes 21 time-domain features from the 15-sample circular ring buffer:
* **Features 0..5**: 6-axis arithmetic means ($\mu$).
* **Features 6..11**: 6-axis sample standard deviations ($\sigma$).
* **Features 12..17**: 6-axis peak-to-peak ranges ($\max - \min$).
* **Feature 18**: Mean acceleration magnitude ($\mu_{\|A\|}$).
* **Feature 19**: Standard deviation of acceleration magnitude ($\sigma_{\|A\|}$).
* **Feature 20**: Peak acceleration magnitude ($\max_{\|A\|}$).

### 4.3 Pedometer Peak Detection Logic
Steps trigger when acceleration magnitude exceeds $1.20g$ with a positive slope ($\frac{d|A|}{dt} > 0$). The detector re-arms only when magnitude drops below $1.05g$. The interval between steps $\Delta t$ must be between $220\text{ ms}$ (272 SPM max) and $2000\text{ ms}$ (30 SPM min). Cadence decays to 0 SPM after 2 seconds without steps.

---

## 5. Native Zephyr Wi-Fi Stack & Captive Portal

### 5.1 RW007 Driver Implementation
The Realtek RW007 Wi-Fi module had no open-source driver under Zephyr RTOS. We developed `src/rw007_wifi.c` around the dual-phase SPI handshake:
* **Stage 1 (Header Phase)**: Master sends a 16-byte header with sequence numbers and 32-bit magic words (`0x67452301`, `0xEFCDAB89`). The slave responds with matching magic words (`0x98BADCFE`, `0x10325476`) and pulses pin PG11.
* **Stage 2 (Payload Phase)**: Master and slave exchange Ethernet frames over SPI2 DMA at 10.5 MHz.
* Attaches to Zephyr's L2 Ethernet layer (`src/rw007_netif.c`), allowing standard sockets (`zsock_*`) and DHCP to operate natively.

### 5.2 Standalone SoftAP Captive Portal Flow
When unconfigured:
1. The RW007 broadcasts open network **`Spark-Setup`**.
2. The ST7789 LCD renders a 25x25 QR code encoding `WIFI:S:Spark-Setup;T:nopass;;`.
3. A captive DNS server on UDP port 53 resolves all queries to `192.168.4.1`.
4. A micro-HTTP server on TCP port 80 intercepts OS connectivity probes (`/generate_204`, `/hotspot-detect.html`) with HTTP 302 redirects, triggering the mobile phone's native "Sign in to network" dialog automatically.
5. The web page serves a dropdown menu containing scanned local Wi-Fi networks with signal strengths. Tapping an option auto-fills the SSID input field.

### 5.3 3-Round Watchdog & Flash Sector 11 Storage
* After submitting credentials, the board executes a 3-round connection watchdog (`connect_with_retries` in `main.c`). Each round has a 15-second isolated DHCP timeout guard while showing real-time attempt progress on the LCD.
* Verified credentials are saved into Flash Sector 11 (`0x080E0000`) with interrupts masked via `irq_lock()` and ART cache flushing.
* Pressing **KEY0** (`PC0`) on the board prompts an interactive Wi-Fi reset confirmation on the LCD with an 8-second countdown. Pressing KEY0 confirms, while KEY1 (`PC1`) cancels.

---

## 6. Companion Android Dashboard Application

Built in Kotlin using Material Design 3 (`android_app/`):
* **Zero Runtime Permissions**: Operates as a direct Wi-Fi UDP listener on port 8888, eliminating Bluetooth permission prompts.
* **`UdpReceiver.kt`**: Background service acquiring an Android `MulticastLock` to receive local UDP broadcast packets without power-saving drops.
* **Hero Activity Card**: Real-time status badge switching between Emerald Green (`WALKING`), Electric Orange (`RUNNING`), and Cyan (`STATIONARY`).
* **3-Card Metric Grid**: Large displays for cumulative steps, cadence in steps per minute, and acceleration magnitude.
* **Activity Timeline**: Scrollable timestamped log tracking gait transitions.

---

## 7. Critical Challenges & Solutions (Post-Mortem)

### 1. RW007 Dual-Phase SPI Handshake
* *Problem*: The module remained silent on boot under Zephyr.
* *Root Cause*: Communication requires 32-bit magic words (`0x67452301`, `0x98BADCFE`) and falling-edge interrupt synchronization on pin PG11. Using 8-bit magic bytes caused the slave to reject packets.
* *Solution*: Re-implemented the 16-byte header structures and synchronized Stage 2 data clocking to PG11 falling-edge semaphores.

### 2. ARM Cortex-M4 MPU Hard Fault during Flash Erase
* *Problem*: Saving credentials to Flash Sector 11 caused hard faults (`MMFAR = 0x80e0000`).
* *Root Cause*: Erasing 128 KB Sector 11 takes 1.5 seconds, bus-locking Flash. Active 1 ms SysTick interrupts attempted to fetch vectors from busy Flash, corrupting the stack and jumping into RAM (`0x2000a278`). An unconfigured MPU flagged RAM as Execute-Never.
* *Solution*: Disabled `CONFIG_ARM_MPU`, masked interrupts with `irq_lock()` during sector erase, and flushed the STM32 ART Accelerator cache.

### 3. Station vs. SoftAP MAC Address Mismatch during DHCP
* *Problem*: After submitting credentials through the portal, the board associated with the router but timed out waiting for an IP lease ("associated but no DHCP lease").
* *Root Cause*: The RW007 uses distinct MACs for SoftAP (`...e3`) and Station mode (`...e2`). The interface was left holding the SoftAP MAC, so when the router sent its 590-byte DHCP Offer addressed to `...e2`, Zephyr discarded the packet as foreign traffic.
* *Solution*: Restored the Station MAC immediately upon SoftAP shutdown, enabling instant DHCP lease binding.

### 4. TCP Buffer Exhaustion & Nagle Delays on Mobile Browsers
* *Problem*: Smartphones hung indefinitely when loading the captive portal page.
* *Root Cause*: Zephyr's default 128-byte buffer fragments exhausted the 16-buffer pool on a 946-byte response. Closing the socket immediately aborted in-flight data with a TCP RST.
* *Solution*: Expanded buffer fragments to 512 bytes (24 buffers), enabled `ZSOCK_TCP_NODELAY`, configured `net_if_ipv4_set_gw()`, and applied graceful half-close via `zsock_shutdown(c->fd, ZSOCK_SHUT_WR)`.

---

## 8. Verification & Test Results

```text
================================================================================
Test Phase              Target / Tool          Result
================================================================================
TinyML Unit Tests       MinGW GCC / Unity      38/38 Assertions PASSED (0 failures)
Static Code Analysis    Cppcheck 2.13          0 Defects Reported
Firmware Build          PlatformIO / Zephyr    SUCCESS (Flash: 14.3%, RAM: 64.6%)
Flash Upload            ST-LINK / OpenOCD      VERIFIED OK
Hardware Sensor Read    CMSIS I2C on PF1/PF0   VERIFIED (WHO_AM_I = 0xAF)
Captive Portal Flow     Phone Camera & Chrome  VERIFIED ON HARDWARE
Mobile Client Stream    Android SDK 34 / UDP   VERIFIED ON HARDWARE
================================================================================
```

---

## 9. Quickstart Guide

### 1. Run Automated Unit Tests
```bash
pio test -e native
```

### 2. Static Code Analysis
```bash
pio check -e black_f407zg
```

### 3. Build & Flash Firmware
```bash
pio run -e black_f407zg -t upload
```

### 4. Wi-Fi Onboarding
1. Power on the board with a USB charger or power bank.
2. The ST7789 LCD displays the **Wi-Fi Setup QR Code**.
3. Point your smartphone camera at the screen and tap **Connect to Wi-Fi** to join `Spark-Setup`.
4. Your phone automatically opens the captive portal notification.
5. Select your Wi-Fi from the dropdown, enter the password, and tap **Save & Connect**.
6. Open the companion Android app to view live activity recognition metrics!
