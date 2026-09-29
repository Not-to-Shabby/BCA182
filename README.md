# BCA182 Embedded Systems Programming

**Department of Computer Applications**  
**College of Computer Studies**  
**Mindanao State University – Iligan Institute of Technology**

---

## Repository Overview

This repository contains the engineering laboratories, firmware implementations, and technical documentation for **BCA182: Embedded Systems Programming**.

All firmware in this repository is built using **PlatformIO** and production-grade real-time operating systems (**FreeRTOS** and **Zephyr RTOS**). Across laboratories, strict embedded systems discipline is enforced: **the Arduino framework and abstractions are strictly prohibited**, utilizing native RTOS primitives, ST HAL / Zephyr peripheral drivers, and decoupled architectures for automated unit testing.

---

## Laboratory Index

| Laboratory Directory | Project Title | Target Hardware & RTOS | Description | Status |
|---|---|---|---|---|
| [**`LAB_1_FreeRTOS_Multisensor`**](./LAB_1_FreeRTOS_Multisensor) | Real-Time Multisensor Room Monitoring System | STM32 Blue Pill (STM32F103C8T6)<br>**FreeRTOS Kernel v10.3.1** | Concurrent 6-task FreeRTOS telemetry node integrating DHT22, LDR, PIR, rotary encoder, SSD1306 OLED, and buzzer alarm. | Done |
| [**`LAB_2_ZephyrRTOS_Personal_MP3_Player`**](./LAB_2_ZephyrRTOS_Personal_MP3_Player) | Personal MP3 Player | RT-Thread Spark Board (STM32F407ZGT6)<br>**Zephyr RTOS v4.x** | Concurrent 3-thread Zephyr audio player with directional D-pad controls (UP/DOWN track scroll, LEFT/RIGHT volume, PRESS play/pause), hardware TIM3_CH3 PWM note synthesis, ST7789 LCD telemetry, and RGB LED indicators. | Phase 5: Audio Synthesizer & Music Playback Verified |

---

## Multi-Laboratory Architecture Comparison Matrix

| Architectural Dimension | Laboratory 1 (`LAB_1_FreeRTOS_Multisensor`) | Laboratory 2 (`LAB_2_ZephyrRTOS_Personal_MP3_Player`) |
|---|---|---|
| **Target Microcontroller** | STMicroelectronics **STM32F103C8T6** | STMicroelectronics **STM32F407ZGT6** |
| **Development Board** | STM32 Blue Pill | RT-Thread Spark Board ("星火 1 号") |
| **Core Architecture** | ARM Cortex-M3 @ 72 MHz (No FPU) | ARM Cortex-M4F @ 168 MHz (Hardware FPU) |
| **Flash & RAM** | 64 KB Flash, 20 KB SRAM | 1024 KB Flash, 192 KB SRAM + 64 KB CCM |
| **Real-Time OS** | **FreeRTOS Kernel v10.3.1** | **Zephyr RTOS v4.x** |
| **Concurrency Model** | 6 Preemptive Tasks (`vTaskDelayUntil`) | 3 Cooperative / Preemptive Threads (`k_sleep`) |
| **Display Subsystem** | 0.96" SSD1306 128×64 OLED via Hardware I2C1 | 1.3" ST7789 v3 240×240 Color TFT via 8080 FSMC Parallel |
| **Audio Generation** | Hardware TIM2 PWM Active Buzzer (1 kHz alarm tone) | Hardware TIM3_CH3 PWM Buzzer (PB0) + ES8388 Codec (I2C/I2S) |
| **User Input** | KY-040 Quadrature Rotary Encoder (EXTI4 / GPIO) | Directional D-Pad (UP/DOWN Track, LEFT/RIGHT Vol) + USER_BUTTON |
| **Synchronization** | Queues, Recursive Mutex, Event Groups | Mutex (`k_mutex`), Kernel Semaphores, Workqueues |
| **Host Unit Testing** | MinGW GCC + Unity Test Framework (`pio test -e native`) | Decoupled Pure Decision Logic (`player_logic.c`) |
| **Power Conservation** | Activity State Machine (`ACTIVE` / `INACTIVE` OLED sleep) | Kernel Idle Sleep Loop (`k_sleep(K_FOREVER)`) |

---

## Technical & Architectural Guidelines

- **Framework Policies**:
  - Lab 1 uses the **STM32Cube framework** (`framework = stm32cube`) with native FreeRTOS.
  - Lab 2 uses the **Zephyr RTOS framework** (`framework = zephyr`) targeting `black_f407zg`.
  - Arduino framework convenience functions (`digitalWrite`, `analogRead`, `delay`) are strictly prohibited across all laboratories.
- **Repository Cleanliness Policy**:
  - The Git root directory is strictly reserved for `README.md`, `AGENTS.md`, `.gitignore`, and the self-contained laboratory project folders.
  - Build outputs (`.pio/`), IDE configurations (`.vscode/`), intermediate binaries (`*.elf`, `*.bin`, `*.hex`), and course documents (`*.docx`, `*.docx.md`) must never be staged or committed to the repository root.

For detailed development rules, hardware constraints, and workflow guidelines, see [**`AGENTS.md`**](./AGENTS.md).

---

## Author & Academic Information

- **Student Developer**: Perch Arnel II Montefalcon
- **Institution**: Mindanao State University – Iligan Institute of Technology (MSU-IIT)
- **Course**: BCA182 Embedded Systems Programming
- **Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)
