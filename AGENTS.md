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

- **IDE & Tooling Environment**: PlatformIO builds, test execution, and Wokwi circuit simulation are run by the user within **Visual Studio Code Insiders**.
- **Agent Verification Contract**:
  - The agent does not have access to an interactive GUI Wokwi runtime.
  - Before requesting a simulation run from the user, the agent must ensure static correctness:
    - Clean compilation with no unresolved warnings or broken references.
    - Deterministic decision logic verified through automated unit tests.
    - Accurate pinout and device wiring defined in `diagram.json` and `wokwi.toml`.
  - When the user runs the simulation in VS Code Insiders, they will copy and share the Serial Monitor output (115200 baud) and report hardware indicators (such as the on-board PC13 status LED).
  - The agent interprets this feedback to confirm behavior or diagnose regressions.

---

## 3. STM32Cube & FreeRTOS Architecture Standards

- **Strict Framework Ban**: The Arduino framework, Arduino core libraries, and Arduino-style convenience abstractions are strictly prohibited by course rules. All code must use the **STM32Cube framework** (`framework = stm32cube`), ST HAL drivers, and native FreeRTOS C/C++ APIs.
- **Vendored FreeRTOS 10.3.1 (ARM Cortex-M3 Port)**:
  - **Single Port Compilation**: `lib/FreeRTOS/library.json` must configure `build.srcFilter` to compile exactly one portable target: `+<portable/GCC/ARM_CM3/port.c>`.
  - **VTOR Initialization**: Before starting the scheduler (`vTaskStartScheduler()`), the firmware must initialize `SCB->VTOR = FLASH_BASE;` so the Cortex-M3 SVC-0 handler correctly restores the initial task stack pointer from flash offset 0.
  - **Wokwi NVIC Priority-Probe Clamp**: Wokwi's virtual MCU does not mask unimplemented NVIC priority bits. The priority-bits probe in `xPortStartScheduler()` must be safely clamped to prevent assertion deadlocks.
  - **Combined SysTick Handler**: `main.c` must define `SysTick_Handler()` to call both `HAL_IncTick()` (for HAL timing/delays) and `xPortSysTickHandler()` (for FreeRTOS kernel context switching).

---

## 4. Multi-Tasking & IPC Architecture

- **Task Decomposition**: Systems must be modular and concurrent. Single-task super-loops are unacceptable.
  - `SensorTask`: Periodic sensor acquisition using `vTaskDelayUntil()`.
  - `DisplayTask`: Dedicated, exclusive owner of the SSD1306 OLED display.
  - `InputTask`: Rotary encoder polling/event decoding.
  - `MotionTask`: PIR motion monitoring and inactivity timer.
  - `AlarmTask`: Temperature threshold evaluation and buzzer control.
  - `StateTask` / System State: Centralized `ACTIVE` / `INACTIVE` state management.
- **IPC Mechanisms**:
  - **Queues**: Used for transferring structured telemetry (`SensorData`) from producer tasks to consumers.
  - **Mutexes**: Must protect shared hardware resources (such as USART1 serial printing) against interleaved corruption.
  - **Event Groups / Notifications**: Used for discrete system events (`EVENT_ACTIVE`, `EVENT_MOTION`, `EVENT_ALARM`).

---

## 5. Software Modularity & Unit Testing

- **Decoupled Architecture**: All hardware-independent decision logic must be strictly separated from hardware peripheral access.
  - `evaluateTemperature()` must be a pure function testable with floating-point values.
  - `nextDisplayMode()` and `previousDisplayMode()` must be pure cyclic state machines.
  - `evaluateSystemState()` must handle inactivity transitions deterministically.
- **Unit Testing**: Unit tests are located in `LAB_1_FreeRTOS_Multisensor/test/` and run using the Unity framework.

---

## 6. Commit & Git Discipline

- Commits must be made incrementally after each technical milestone.
- Commit messages must follow conventional imperative style:
  - `Initialize repository structure and agent rules`
  - `Configure initial Wokwi simulation and FreeRTOS foundation`
  - `Implement DHT22 sensor acquisition`
  - `Add LDR measurement`
  - `Add sensor data queue and serial mutex`
  - `Implement OLED display task`
  - `Add rotary encoder navigation`
  - `Implement alarm task`
  - `Add PIR motion and system state machine`
  - `Add FreeRTOS event group`
  - `Add alarm, navigation, and state unit tests`
  - `Resolve static analysis findings`
  - `Complete Wokwi verification and fault experiments`
  - `Finalize technical documentation and laboratory report`
- Never combine unrelated features into monolithic or vague commits (e.g., "update", "working").
