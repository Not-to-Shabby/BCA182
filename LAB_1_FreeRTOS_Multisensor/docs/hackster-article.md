# Building a Real-Time Multisensor Room Monitor with STM32 and FreeRTOS

**Author**: Perch Arnel II Montefalcon  
**Course**: BCA182 – Embedded Systems Programming  
**Institution**: Mindanao State University – Iligan Institute of Technology (MSU-IIT)  
**Collaborator & Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)  
**GitHub Repository**: [https://github.com/Not-to-Shabby/BCA182](https://github.com/Not-to-Shabby/BCA182)  

---

## 1. Project Overview & Motivation
Indoor air quality, ambient lighting, and room occupancy directly impact human productivity, health, and energy conservation. In embedded systems, managing multiple real-time sensors alongside responsive user displays and safety alarms is a classic engineering challenge. 

Traditional bare-metal approaches rely on a monolithic super-loop (`while(1)`), where slow sensor reads (such as the DHT22's ~20 ms single-wire pulse handshake) freeze the processor, causing sluggish button responses and screen flicker.

In this project, we design and implement a **fully concurrent environmental room monitor** on the **STM32 Blue Pill (STM32F103C8T6)** using **FreeRTOS** and the **STM32Cube framework**. We eliminate blocking delays, guarantee drift-free periodic sampling, protect shared hardware resources with mutexes, and implement an automated sleep state machine.

---

## 2. Key Features
- **Concurrent Task Architecture**: 6 independent FreeRTOS tasks with prioritized preemptive scheduling.
- **Multisensor Telemetry**:
  - **Temperature & Humidity**: DHT22 single-wire protocol via ARM Cortex-M DWT cycle counter.
  - **Ambient Light**: Analog photoresistor (LDR) sampled via 12-bit ADC1 on PA0.
  - **Occupancy Detection**: Digital PIR motion sensor on PA3.
- **Interactive UI**: SSD1306 128x64 OLED display running over 400 kHz Hardware I2C1, navigated via a KY-040 rotary encoder with forward and reverse wraparound.
- **Out-of-Bounds Safety Alarm**: Active buzzer alerts occupants with a 1 kHz PWM tone if temperature drops below $18.0^\circ\text{C}$ or exceeds $30.0^\circ\text{C}$.
- **Smart Power-Saving State Machine**:
  - Automatically puts the OLED panel into low-power standby after 15 seconds without motion.
  - Motion or rotary knob interaction immediately restores active mode.
- **Zero Arduino Abstractions**: Built strictly with ST HAL drivers and native FreeRTOS APIs.

---

## 3. Hardware & Circuit Configuration

```text
       +-------------------------------------------------------+
       |               STM32F103C8T6 (Blue Pill)               |
       |                                                       |
       |  PA0 (ADC1_IN0) <---- Analog Voltage ---- LDR Sensor  |
       |  PA1 (GPIO O.D) <---> Single-Wire    <---> DHT22      |
       |  PA2 (TIM2_CH3) ----> PWM Tone (1kHz) --> Active Buzzer|
       |  PA3 (GPIO In)  <---- Digital Motion <--- PIR Sensor  |
       |  PA4 (EXTI4)    <---- Quadrature CLK <-- KY-040 Enc.  |
       |  PA5 (GPIO In)  <---- Quadrature DT  <-- KY-040 Enc.  |
       |  PB6 (I2C1_SCL) ----> I2C Clock -------- SSD1306 OLED |
       |  PB7 (I2C1_SDA) <---> I2C Data --------> SSD1306 OLED |
       |  PA9 (USART1_TX)----> Serial Monitor (115200 8N1)     |
       |  PA10(USART1_RX)<---- Serial Terminal                 |
       |  PC13 (GPIO Out)----> Heartbeat LED (Active LOW)      |
       +-------------------------------------------------------+
```

---

## 4. FreeRTOS Task Architecture

| Task | Priority | Responsibility | IPC Mechanism |
|---|---|---|---|
| **`InputTask`** | **3** (High) | Debounces encoder pulses; cycles display page; wakes sleeping system | `displayModeQueue`, `systemEvents` |
| **`MotionTask`** | **3** (High) | Samples PIR sensor on PA3 every 100 ms | Sets/clears `EVENT_MOTION` & `EVENT_PIR_LEVEL` |
| **`StateTask`** | **3** (High) | Central state machine; manages 15 s sleep/wake timer | Sets/clears `EVENT_ACTIVE` |
| **`SensorTask`** | **2** (Med) | Samples DHT22 & LDR every 2000 ms using **`vTaskDelayUntil`** | Pushes to `sensorToDisplayQueue` & `sensorToAlarmQueue` |
| **`AlarmTask`** | **2** (Med) | Evaluates $18^\circ\text{C}-30^\circ\text{C}$ temperature limits; drives buzzer | Reads `sensorToAlarmQueue`, sets `EVENT_ALARM` |
| **`DisplayTask`** | **1** (Low) | Exclusively manages SSD1306 OLED; renders telemetry views | Reads `sensorToDisplayQueue`, `displayModeQueue` |

---

## 5. Software Engineering & Testing

### 1. Drift-Free Periodic Execution (`vTaskDelayUntil`)
Periodic sampling with standard delays (`vTaskDelay(2000)`) accumulates execution and preemption overhead into timing drift. Using `vTaskDelayUntil(&lastWakeTime, 2000)` enforces an exact 0.5 Hz sampling frequency anchored to absolute tick counts.

### 2. Thread-Safe Logging (`serialMutex`)
Concurrent tasks outputting diagnostic strings to USART1 are synchronized using a FreeRTOS recursive mutex (`serialMutex`). This prevents character interleaving and ensures clean terminal telemetry.

### 3. Automated Unit Testing (100% Pass)
Hardware-independent decision logic was decoupled into pure C modules (`logic.c`) and verified on the host PC using the **Unity** test runner (`pio test -e native`):
- 5 Temperature alarm threshold tests $\to$ **PASSED**
- 4 Rotary encoder bidirectional navigation tests $\to$ **PASSED**
- 4 System state transition tests $\to$ **PASSED**
- Total: **13 of 13 unit tests succeeded**.

### 4. Static Code Analysis (0 Defects)
Verified using PlatformIO Check with `cppcheck`:
- **0 high, 0 medium, 0 low defects**.

---

## 6. Challenges & Simulation Solutions
During Wokwi virtual simulation, we diagnosed an emulator limitation where standard Cortex-M3 exception return unstacking (`EXC_RETURN` via `SVC 0`) failed during task startup. 

We engineered a **direct Thread-mode context-switching port**:
- The first task is launched in Thread mode on `PSP` using `prvTaskBootstrap`.
- Task yields execute via `vPortYieldDirect()` without relying on `PendSV`.
- The scheduler tick is driven by hardware timer `TIM3` running at 20 Hz, reducing browser simulation overhead while preserving 100% functional fidelity.

---

## 7. Accreditation & Collaboration
This project is submitted in partial fulfillment of the requirements for **BCA182: Embedded Systems Programming** at **Mindanao State University – Iligan Institute of Technology**.

- **Course Evaluator / Collaborator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)
- **Full Source Code & Schematics**: [https://github.com/Not-to-Shabby/BCA182](https://github.com/Not-to-Shabby/BCA182)
