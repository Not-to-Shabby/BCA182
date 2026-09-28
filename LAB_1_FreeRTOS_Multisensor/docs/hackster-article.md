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

### 3.1 Simulated Circuit Diagram
![Wokwi Simulation Circuit](./images/wokwi-circuit.png)  
*Figure 1: Wokwi circuit simulation diagram displaying the STM32 Blue Pill connected to sensors and actuators.*

### 3.2 Pin Connections and Wiring Netlist

| Source Pin (STM32) | Target Pin (Component) | Wire Color | Signal Type | Description |
|---|---|---|---|---|
| **`3V3.1`** | **`oled1:VCC`**, **`dht1:VCC`**, **`ldr1:VCC`**, **`pir1:VCC`**, **`encoder1:VCC`** | **Red** | Power (+3.3V) | Main regulated 3.3V DC power rail |
| **`GND.1`** | **`oled1:GND`**, **`dht1:GND`**, **`ldr1:GND`**, **`pir1:GND`**, **`encoder1:GND`**, **`buzzer1:2`** | **Black** | Ground (0V) | Common system reference ground |
| **`PA0`** | **`ldr1:AO`** | **Orange** | Analog In (ADC1_IN0) | Ambient light sensor analog voltage |
| **`PA1`** | **`dht1:SDA`** | **Green** | Bidirectional Open-Drain | DHT22 single-wire digital communications |
| **`PA2`** | **`buzzer1:1`** | **Purple** | Digital Out (TIM2_CH3) | 1 kHz audible PWM alarm signal |
| **`PA3`** | **`pir1:OUT`** | **Gold** | Digital In (Pulldown) | Human presence motion pulse |
| **`PA4`** | **`encoder1:CLK`** | **Cyan** | Digital In (EXTI4) | Rotary encoder quadrature clock interrupt |
| **`PA5`** | **`encoder1:DT`** | **Magenta** | Digital In (Pull-up) | Rotary encoder quadrature direction line |
| **`PB6`** | **`oled1:SCL`** | **Yellow** | Alternate Function (I2C1) | Hardware I2C Clock (400 kHz Fast Mode) |
| **`PB7`** | **`oled1:SDA`** | **Blue** | Alternate Function (I2C1) | Hardware I2C Data line (400 kHz Fast Mode) |
| **`PA9`** | **`$serialMonitor:RX`** | **Amber** | Alternate Function (USART1) | Serial diagnostic logging (115200 8N1) |
| **`PA10`** | **`$serialMonitor:TX`** | **Amber** | Input | Virtual serial monitor input |

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

We engineered a **direct Thread-mode context-switching port** (adapted from the open-source implementation by [Djaver Hassan](https://github.com/djaverhassan/bca182-freertos-multisensor), credit: Ni-ear):
- The first task is launched in Thread mode on `PSP` using `prvTaskBootstrap`.
- Task yields execute via `vPortYieldDirect()` without relying on `PendSV`.
- The scheduler tick is driven by hardware timer `TIM3` running at 20 Hz, reducing browser simulation overhead while preserving 100% functional fidelity.

---

## 7. Accreditation & Collaboration
This project is submitted in partial fulfillment of the requirements for **BCA182: Embedded Systems Programming** at **Mindanao State University – Iligan Institute of Technology**.

- **Course Evaluator / Collaborator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)
- **Full Source Code & Schematics**: [https://github.com/Not-to-Shabby/BCA182](https://github.com/Not-to-Shabby/BCA182)
