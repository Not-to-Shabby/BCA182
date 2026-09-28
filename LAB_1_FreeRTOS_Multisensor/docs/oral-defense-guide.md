# Technical Defense & Oral Checkoff Preparation Guide

**Course**: BCA182 – Embedded Systems Programming  
**Author**: Perch Arnel II Montefalcon  
**Activity**: Laboratory Activity No. 1: Real-Time Multisensor Room Monitoring System  
**Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)  

---

This document provides rigorous, engineering-grade answers to all **15 technical defense questions** from **Part XXIV (Section 64)** of the syllabus.

---

### 1. Why did you create `SensorTask`?
**Answer**:  
`SensorTask` was created to isolate periodic environmental data acquisition (DHT22 temperature/humidity and LDR ambient light) from user interface handling, alarm actuation, and display rendering. Environmental metrics change relatively slowly (over seconds), whereas display refreshes and rotary encoder user inputs have completely different timing and latency constraints. Placing sensor acquisition in its own task allows us to enforce strict periodic execution using `vTaskDelayUntil()` without blocking or delaying time-critical user navigation.

---

### 2. Why does each task have its assigned priority?
**Answer**:  
Priorities in FreeRTOS denote **scheduling urgency and latency sensitivity**, not functional importance:
- **`InputTask` (Priority 3, Highest)**: User interaction with the rotary encoder generates fast quadrature pulses. Dropped pulses result in a sluggish, unresponsive UI. It requires immediate execution upon movement.
- **`MotionTask` & `StateTask` (Priority 3, Highest)**: Human presence and power state transitions must react instantaneously to wake the display.
- **`SensorTask` (Priority 2, Medium)**: Periodic environmental sampling must run on exact intervals. Priority 2 allows it to preempt low-priority display rendering to maintain precise 2000 ms timing.
- **`AlarmTask` (Priority 2, Medium)**: Safety-critical temperature limits must be evaluated promptly when new data arrives.
- **`DisplayTask` (Priority 1, Lowest)**: Rendering a 1024-byte framebuffer over I2C takes tens of milliseconds of CPU/bus time. Because human visual persistence tolerates small latencies, placing `DisplayTask` at Priority 1 guarantees that heavy display flushes never starve urgent input decoding or sensor acquisition.

---

### 3. What does `vTaskDelayUntil()` do?
**Answer**:  
`vTaskDelayUntil(&lastWakeTime, period)` calculates the exact unblock time relative to `lastWakeTime`, the absolute tick count from the *previous* period, and automatically updates `lastWakeTime`. 

Unlike `vTaskDelay()`, which delays relative to the moment it is called and accumulates timing drift from task execution and preemption, `vTaskDelayUntil()` guarantees a constant, deterministic execution frequency (e.g., exactly 0.5 Hz / 2000 ms) with zero cumulative phase error.

---

### 4. What happens to a task while it is delayed?
**Answer**:  
When a task calls a blocking delay such as `vTaskDelay()` or `vTaskDelayUntil()`, the kernel transitions the task from the **Running** state to the **Blocked** state. The kernel removes the task from the Ready list and places it onto the Delayed Task List. 

The scheduler immediately selects the next highest-priority task in the Ready list to execute. The blocked task consumes **zero CPU cycles** until its target tick expires or an event unblocks it.

---

### 5. What information crosses your queue?
**Answer**:  
Structured telemetry snapshots defined by `struct SensorData`:
```c
typedef struct {
    float temperature;    // Ambient temperature in degrees Celsius
    float humidity;       // Relative humidity percentage (0-100%)
    int   lightLevel;     // Relative ambient light level (0-100%)
    bool  motionDetected; // Live PIR motion detection flag
    bool  dhtValid;       // Sensor read validity flag
} SensorData;
```
In addition, `displayModeQueue` carries the `DisplayMode` enum (`TEMPERATURE`, `HUMIDITY`, `LIGHT`, `MOTION`) from `InputTask` to `DisplayTask`.

---

### 6. Why did you use a queue rather than unsynchronized global variables?
**Answer**:  
Unsynchronized global variables introduce **data race conditions, torn reads, and lack of synchronization**:
1. **Atomic Integrity**: `SensorData` is a multi-word struct. If `SensorTask` is updating `temperature` and `humidity` when an interrupt or higher-priority task preempts it, the reading task would read a partially updated struct (e.g., new temperature with old humidity).
2. **Synchronization**: FreeRTOS queues automatically block the consumer task until data is ready, eliminating CPU-wasting polling loops.
3. **Decoupling**: Length-1 queues with `xQueueOverwrite()` ensure consumers always read the freshest atomic snapshot without coupling task execution rates.

---

### 7. What resource does your mutex protect?
**Answer**:  
`serialMutex` protects the hardware **`USART1` peripheral** (specifically `USART1->DR` and `USART1->SR`).

---

### 8. Where could a race condition occur?
**Answer**:  
A race condition would occur during serial diagnostic logging:
Multiple concurrent tasks (`SensorTask`, `InputTask`, `AlarmTask`, `StateTask`) write formatted strings to USART1. If `SensorTask` begins transmitting `" [Sensor] Temp: 25.0 C "` and is preempted mid-string by `InputTask` transmitting `" [Encoder] View: Light "`, the physical transmit register would interleave the bytes, resulting in corrupt, unparseable logs like:
`" [Sens[Encoder] View: Light or] Temp: 25.0 C "`.
Using `serialMutex` guarantees that only one task owns the UART bus until its message is completely sent.

---

### 9. What does your event group represent?
**Answer**:  
`systemEvents` is a 4-bit event synchronization mechanism representing discrete, system-wide binary conditions:
- **`EVENT_ACTIVE` (Bit 0)**: High when the system is in `ACTIVE` state (OLED on); cleared when in `INACTIVE` state (OLED standby).
- **`EVENT_MOTION` (Bit 1)**: Pulsed whenever physical motion or rotary encoder interaction occurs, triggering `StateTask` to wake the system and reset the 15-second inactivity timer.
- **`EVENT_ALARM` (Bit 2)**: Set by `AlarmTask` when temperature is outside normal bounds ($<18.0^\circ\text{C}$ or $>30.0^\circ\text{C}$); read by `DisplayTask` to render the `! TEMP ALARM !` banner.
- **`EVENT_PIR_LEVEL` (Bit 3)**: Reflects the live logic level of the PIR sensor for telemetry logging.

---

### 10. Which task owns the OLED, and why?
**Answer**:  
`DisplayTask` **exclusively owns the SSD1306 OLED display**.  
*Why*: The I2C bus and the SSD1306 display controller maintain internal state (column/page pointers and command/data framing). If multiple tasks issued commands to the OLED concurrently, I2C bus transactions would collide, corrupt the display RAM, and lock up the hardware controller. By enforcing a single-owner pattern, `DisplayTask` receives requests via queues and renders frames sequentially without any possibility of I2C bus contention.

---

### 11. What happens if a high-priority task never blocks?
**Answer**:  
If a high-priority task (e.g., Priority 3) never blocks, yields, or waits on an IPC primitive, it enters an uncontrolled busy loop and causes **complete CPU starvation** of all lower-priority tasks (Priorities 2, 1, and the Idle task). 

The lower-priority tasks will never be scheduled. Display rendering halts, sensor sampling stops, and the FreeRTOS idle task cannot run to reclaim memory or service watchdog timers.

---

### 12. What is the difference between Ready and Blocked?
**Answer**:  
- **Ready**: The task has all resources it needs to execute and is immediately eligible for processor time, but is currently waiting in the Ready List because a task of equal or higher priority is currently in the Running state.
- **Blocked**: The task cannot execute even if the CPU were idle. It is waiting on an external event, queue message, semaphore, or timer expiration. It resides on an event list or delayed list and consumes zero CPU cycles.

---

### 13. What functionality did your unit tests actually verify?
**Answer**:  
The unit tests verified all **deterministic, hardware-independent decision logic** (13 test cases in `test_logic.c` using Unity):
1. **Temperature Boundary Logic (5 tests)**:
   - Below lower limit ($17.9^\circ\text{C}$) $\to$ `ALARM_LOW_TEMPERATURE`
   - Exact lower boundary ($18.0^\circ\text{C}$) $\to$ `ALARM_NORMAL`
   - Normal operating temperature ($25.0^\circ\text{C}$) $\to$ `ALARM_NORMAL`
   - Exact upper boundary ($30.0^\circ\text{C}$) $\to$ `ALARM_NORMAL`
   - Above upper limit ($30.1^\circ\text{C}$) $\to$ `ALARM_HIGH_TEMPERATURE`
2. **Display Navigation Logic (4 tests)**:
   - Forward cyclic transition (`Temp` $\to$ `Humidity`)
   - Forward wraparound (`Motion` $\to$ `Temp`)
   - Reverse cyclic transition (`Humidity` $\to$ `Temp`)
   - Reverse wraparound (`Temp` $\to$ `Motion`)
3. **State Machine Transitions (4 tests)**:
   - ACTIVE without timeout ($10\text{ s} < 15\text{ s}$) $\to$ remains `ACTIVE`
   - ACTIVE after timeout ($15\text{ s} \ge 15\text{ s}$) $\to$ transitions to `INACTIVE`
   - INACTIVE without motion $\to$ remains `INACTIVE`
   - INACTIVE with motion $\to$ immediately transitions to `ACTIVE`

---

### 14. What did static analysis discover?
**Answer**:  
Running PlatformIO Check with `cppcheck` discovered 8 low-severity style and type warnings:
- `unsignedLessThanZero` in `dht22.c`: An unsigned timestamp subtraction (`elapsed = DWT->CYCCNT - start`) was flagged when comparing against unsigned ticks. Suppressed with inline annotations because modular unsigned arithmetic is wrap-safe on ARM Cortex-M.
- `constParameterPointer` in `diagnostics.c`, `main.c`, and `stm32f1xx_hal_msp.c`: Function parameters that were only read were flagged to be declared `const`. Signatures were either updated (e.g. `const uint32_t *stacked_regs`) or suppressed where constrained by fixed FreeRTOS and STM32 HAL callback prototypes.
- The final static analysis passed with **0 high, 0 medium, and 0 low defects**.

---

### 15. What would differ if this system ran on physical hardware?
**Answer**:  
Four key architectural differences would apply on bare silicon:
1. **Exception Unstacking & Port Startup**: Physical Cortex-M3 silicon executes `SVC 0` and unrolls the initial thread stack via hardware `EXC_RETURN` (`0xFFFFFFFD`) natively, whereas Wokwi required a Thread-mode direct bootstrap (`prvTaskBootstrap`) to bypass an emulator unstacking bug.
2. **Tick Timer**: Physical hardware uses the internal `SysTick` timer at 1000 Hz (1 ms tick resolution), whereas the Wokwi implementation uses `TIM3` at 20 Hz (50 ms tick) to minimize web browser simulation overhead.
3. **ADC Calibration**: Physical STM32 ADCs require software calibration (`HAL_ADCEx_Calibration_Start()`) at boot to nullify internal capacitor offsets, which Wokwi simulates as ideal.
4. **Physical Pull-Up Resistors**: Real hardware requires external physical $4.7\text{ k}\Omega$ pull-ups on PB6/PB7 (I2C) and $10\text{ k}\Omega$ on PA1 (DHT22) to handle cable capacitance at 400 kHz, whereas Wokwi models ideal pull-ups internally.
