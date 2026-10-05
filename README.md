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
| [**`LAB_2_ZephyrRTOS_Personal_MP3_Player`**](./LAB_2_ZephyrRTOS_Personal_MP3_Player) | Personal MP3 Player | RT-Thread Spark Board (STM32F407ZGT6)<br>**Zephyr RTOS v4.x** | Concurrent 3-thread Zephyr audio player featuring 8-song classical synthesizer, ES8388 3.5mm stereo audio via circular DMA, 4-bit SDIO FAT32, WAV streaming, and RealNetworks Helix fixed-point MP3 decoder. | Done |
| [**`LAB_3_EdgeAI_Activity_Tracker`**](./LAB_3_EdgeAI_Activity_Tracker) | Edge AI Activity Tracker & BLE Telemetry Gateway | RT-Thread Spark Board (STM32F407ZGT6)<br>**On-Device TinyML (Cortex-M4F FPU)** | Full-stack TinyML physical activity classifier (Walking vs. Running) trained on Kaggle `run-or-walk` (99.96% accuracy), executing in < 5 µs on-chip with InvenSense ICM-20608 6-axis IMU, Realtek RW007 BLE GATT notification service (Method A), and companion native Android mobile app. | Done |

---

## Multi-Laboratory Architecture Comparison Matrix

| Architectural Dimension | Laboratory 1 (`LAB_1_FreeRTOS_Multisensor`) | Laboratory 2 (`LAB_2_ZephyrRTOS_Personal_MP3_Player`) | Laboratory 3 (`LAB_3_EdgeAI_Activity_Tracker`) |
|---|---|---|---|
| **Target Microcontroller** | STMicroelectronics **STM32F103C8T6** | STMicroelectronics **STM32F407ZGT6** | STMicroelectronics **STM32F407ZGT6** |
| **Development Board** | STM32 Blue Pill | RT-Thread Spark Board ("星火 1 号") | RT-Thread Spark Board ("星火 1 号") |
| **Core Architecture** | ARM Cortex-M3 @ 72 MHz (No FPU) | ARM Cortex-M4F @ 168 MHz (Hardware FPU) | ARM Cortex-M4F @ 168 MHz (Hardware FPU) |
| **Flash & RAM** | 64 KB Flash, 20 KB SRAM | 1024 KB Flash, 192 KB SRAM + 64 KB CCM | 1024 KB Flash, 192 KB SRAM + 64 KB CCM |
| **Primary Paradigm** | Real-Time Multitasking (FreeRTOS) | Real-Time Audio DSP & Streaming | **Edge AI / TinyML & BLE Telemetry** |
| **Machine Learning** | None (Rule-based Thresholds) | None (Digital Audio Decoding) | **10-Tree Random Forest ($depth=5$) via emlearn (Zero-Allocation C on FPU)** |
| **Sensors & Input** | DHT22, LDR, PIR, Rotary Encoder | Directional Buttons & Potentiometer | **InvenSense ICM-20608-G (6-Axis IMU via I2C2)** |
| **Wireless / Telemetry** | USART1 Terminal Telemetry | USART1 Console & ST-Link VCP | **Realtek RW007 Apache NimBLE GATT Server (Method A Service `0xFEE7` / Char `0xFEA2`)** |
| **Mobile Client** | None | None | **Native Android Application (Kotlin, BLE GATT Client)** |
| **Display Subsystem** | 0.96" SSD1306 128×64 OLED via Hardware I2C1 | 1.3" ST7789 v3 240×240 Color TFT via 8080 FSMC Parallel Bank 3 | 1.3" ST7789 v3 LCD + Android Mobile Dashboard |
| **Host Unit Testing** | MinGW GCC + Unity: **13/13 Passing** | MinGW GCC + Unity: **25/25 Passing** | MinGW GCC + Unity: **37/37 Passing** |
| **Static Code Analysis** | Cppcheck: **0 Defects** | Cppcheck: **0 Defects** | Cppcheck: **0 Defects** |

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
