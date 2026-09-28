# Agent Operating Guide: BCA182 Repository

This document establishes development rules, hardware constraints, simulation workflows, and architectural standards for any AI agent operating on this repository.

---

## 1. Repository Layout & Cleanliness Rule

**CRITICAL POLICY**:
The root directory of this repository (`https://github.com/Not-to-Shabby/BCA182`) must remain strictly organized. Only the following items are permitted at the Git root:
- `README.md` (Repository overview and portfolio navigation)
- `AGENTS.md` (This agent instruction guide)
- `.gitignore` (Standard exclusions for course docs, build artifacts, and IDE files)
- `LAB_1_FreeRTOS_Multisensor/` (The self-contained Laboratory 1 project directory)

All course assignment files (such as `*.docx`, `*.docx.md`), PlatformIO build outputs (`.pio/`), IDE configurations (`.vscode/`), and intermediate binaries (`*.elf`, `*.bin`, `*.hex`) must never be staged or committed to the repository root.

---

## 2. Development & Simulation Workflow

- **IDE & Tooling Environment**: PlatformIO builds, test execution, and Wokwi circuit simulation are run in **Visual Studio Code Insiders** or via `wokwi-cli`.
- **Wokwi Serial Monitor Wiring**:
  - The Wokwi STM32 Blue Pill model requires explicit connections in `diagram.json`:
    - `[ "stm32:A9", "$serialMonitor:RX", "amber", [] ]`
    - `[ "stm32:A10", "$serialMonitor:TX", "amber", [] ]`
  - Without these wires, virtual USART1 serial output will not appear in the Wokwi terminal.
- **Agent Verification Contract**:
  - Before asking the user for a test run or committing, verify statically:
    - `pio run`: Clean build with zero compilation errors and warnings.
    - `pio test -e native`: 13 automated unit tests pass on the host MinGW environment via Unity.
    - `pio check`: Cppcheck static code analysis reports zero defects.

---

## 3. STM32Cube & FreeRTOS Architecture Standards

- **Strict Framework Ban**: The Arduino framework, Arduino core libraries, and Arduino-style convenience abstractions are strictly prohibited by course rules. All code must use the **STM32Cube framework** (`framework = stm32cube`), ST HAL drivers, and native FreeRTOS C/C++ APIs.
- **Wokwi Cortex-M3 FreeRTOS Compatibility Port**:
  - *Context Switching*: Wokwi's virtual Cortex-M3 emulator fails during hardware exception return (`EXC_RETURN` via `SVC 0`) from task startup. To bypass this emulator defect, `lib/FreeRTOS/portable/GCC/ARM_CM3/port.c` implements a direct Thread-mode bootstrap (`prvTaskBootstrap` on `PSP`) and naked `vPortYieldDirect()`.
  - *Tick Source*: Driven by hardware timer `TIM3` at 20 Hz (`configTICK_RATE_HZ = 20`) to eliminate high-frequency interrupt overhead in web browser emulation. `TIM3_IRQHandler` advances both the FreeRTOS tick and `uwTick`.
  - *Idle Hook Yield*: `vApplicationIdleHook()` executes `__WFI()` and calls `taskYIELD()` when `xPortConsumeTickYield()` indicates a pending tick switch.
  - *Attribution*: This direct context-switching port pattern was adapted from the open-source implementation by [Djaver Hassan](https://github.com/djaverhassan/bca182-freertos-multisensor) (credit: Ni-ear).
- **Hardware I2C1 for SSD1306 OLED**:
  - Configured on pins **PB6 (SCL)** and **PB7 (SDA)** in Alternate Function Open-Drain mode (`GPIO_MODE_AF_OD`) with pull-ups.
  - Initialized at 400 kHz Fast Mode and communicated via `HAL_I2C_Mem_Write()`.

---

## 4. Multi-Tasking & IPC Architecture

- **Task Decomposition**: 6 discrete FreeRTOS tasks with prioritized preemptive scheduling:
  - `MotionTask` (Priority 3): Periodic 100 ms PIR monitoring on PA3.
  - `StateTask` (Priority 3): Central state machine managing 15 s sleep/wake timer.
  - `InputTask` (Priority 3): EXTI4 rotary encoder decoding on PA4/PA5; wakes sleeping device.
  - `SensorTask` (Priority 2): Periodic 2000 ms acquisition using **`vTaskDelayUntil()`**.
  - `AlarmTask` (Priority 2): Temperature limit evaluation ($18^\circ\text{C}-30^\circ\text{C}$) and TIM2 PWM buzzer control on PA2.
  - `DisplayTask` (Priority 1): Exclusive owner of SSD1306 OLED; enters sleep when `INACTIVE`.
- **IPC Mechanisms**:
  - **Queues**: `sensorToDisplayQueue`, `sensorToAlarmQueue`, `displayModeQueue`.
  - **Mutex**: `serialMutex` (recursive) guarding `USART1` terminal telemetry.
  - **Event Group**: `systemEvents` (`EVENT_ACTIVE`, `EVENT_MOTION`, `EVENT_ALARM`, `EVENT_PIR_LEVEL`).

---

## 5. Software Modularity & Unit Testing

- **Decoupled Architecture**: All hardware-independent decision logic is isolated in `src/logic.c` and `include/logic.h`:
  - `evaluateTemperature()`: 5 boundary conditions tested.
  - `nextDisplayMode()` / `previousDisplayMode()`: 4 bidirectional traversal tests with wraparound.
  - `evaluateSystemState()`: 4 state transition tests.
- **Unit Testing**: Located in `LAB_1_FreeRTOS_Multisensor/test/test_logic/test_logic.c`, executed via `pio test -e native` on MinGW GCC using Unity.

---

## 6. Commit & Git Discipline

- Commits must be made incrementally after each technical milestone following conventional imperative style:
  - `Initialize repository structure, agent rules, and Lab 1 scaffold`
  - `Configure initial Wokwi simulation and FreeRTOS foundation (Phase 2)`
  - `Complete Phase 2: FreeRTOS baseline multitasking verified in Wokwi (Task A & Task B)`
  - `Complete Phase 3: Live sensor acquisition (DHT22 & LDR) and OLED telemetry via FreeRTOS Queue verified`
  - `Implement Phase 4: Full 6-task FreeRTOS architecture (InputTask, AlarmTask, MotionTask, StateTask, SensorTask, DisplayTask)`
  - `Allow rotary encoder interaction to wake system from INACTIVE sleep mode`
  - `Complete Phase 5: Automated unit test suite (13/13 passing) and static code analysis (0 defects)`
  - `Complete Phase 6: Comprehensive 20-section portfolio README, formal academic laboratory report, oral defense guide, and Hackster.io article`
  - `Add circuit simulation diagram screenshot and complete wiring netlist to documentation`
- Never combine unrelated features into monolithic or vague commits.
