# BCA182 Embedded Systems Programming

**Department of Computer Applications**  
**College of Computer Studies**  
**Mindanao State University – Iligan Institute of Technology**

---

## Repository Overview

This repository contains the engineering laboratories, firmware implementations, and technical documentation for **BCA182: Embedded Systems Programming**.

All firmware in this repository is implemented using **PlatformIO** and the **STM32Cube framework** on the **STM32 Blue Pill (STM32F103C8T6)** development board, incorporating native **FreeRTOS** real-time operating system primitives, HAL peripheral drivers, and Wokwi simulation models.

---

## Laboratory Index

| Laboratory Directory | Project Title | Description | Status |
|---|---|---|---|
| [**`LAB_1_FreeRTOS_Multisensor`**](./LAB_1_FreeRTOS_Multisensor) | Real-Time Multisensor Room Monitoring System | Concurrent FreeRTOS telemetry node integrating DHT22, LDR, PIR, rotary encoder, SSD1306 OLED, and buzzer alarm. | In Progress |

---

## Technical & Architectural Guidelines

- **Microcontroller**: STM32F103C8T6 (ARM Cortex-M3 @ 72 MHz)
- **Framework**: STM32Cube HAL (`framework = stm32cube`) — *Arduino abstractions prohibited*
- **RTOS**: FreeRTOS Kernel v10.3.1 (ARM Cortex-M3 GCC port)
- **Simulation**: Wokwi Virtual Circuit Simulation in Visual Studio Code Insiders
- **Testing & Quality**: Automated unit tests via Unity framework (`pio test`) and static code analysis (`pio check`)

For detailed agent rules, workflow guidelines, and FreeRTOS port configurations, see [**`AGENTS.md`**](./AGENTS.md).

---

## Author & Academic Information

- **Student Developer**: Perch Arnel II Montefalcon
- **Institution**: Mindanao State University – Iligan Institute of Technology (MSU-IIT)
- **Course**: BCA182 Embedded Systems Programming
- **Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)
