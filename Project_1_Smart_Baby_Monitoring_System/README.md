# Project 1 — Smart Baby Monitoring System
## IoT Baby Monitor on RT-Thread RT-Spark (STM32F407ZGT6, Zephyr RTOS)

**Smart Baby Monitoring System** is the BCA182 Embedded Systems Programming capstone
project (MSU-IIT, 100 pts): an IoT baby-monitoring prototype that watches the room
environment with the RT-Spark board's onboard sensors, listens for baby-cry events
through the onboard electret microphone and ES8388 audio codec, classifies them
on-device with a lightweight ML model, and notifies a caregiver through an Android
app via a Wi-Fi → MQTT → backend pipeline. This repository is currently a
**scaffold**: the planning documents are complete and board bring-up (P0) is under
way — every feature listed below is *planned*, not yet implemented.

> **RTOS note:** this project deliberately uses **Zephyr RTOS** instead of the
> spec's STM32Cube HAL + FreeRTOS mandate. The rationale, the FreeRTOS↔Zephyr
> equivalence table, and the accepted rubric risk are documented in
> [docs/design.md §7](docs/design.md#7-rtos-deviation-rationale).

## Features (planned)

- Live room temperature/humidity (AHT10) and ambient light (AP3216C) readings on the ST7789 LCD dashboard, with threshold alerts
- Baby-cry detection: onboard mic → ES8388 ADC (16 kHz mono) → MFCC features → on-device RandomForest classifier (emlearn C), confirmed events only
- Device fault detection (knocked/tipped via ICM20608); buzzer + LED local alerts with button acknowledge/mute
- Wi-Fi (RW007) → MQTT publishing of readings and events; offline event queueing to microSD with replay on reconnect
- Backend: Mosquitto broker + FastAPI service over SQLite (readings, events, acks) with a REST API
- Android app: live readings, cry notifications, searchable event history, acknowledgement, stale-data indication

## Hardware

**Board:** RT-Thread RT-Spark (Spark-1, HW rev 1.1) — STM32F407ZGT6 (Cortex-M4F,
168 MHz, 1 MB flash, 128 KB SRAM + 64 KB CCM) with onboard ST-Link
debugger/serial-VCP. Flashing and the console both run through the onboard ST-Link;
serial console is USART1 @ 115200 baud.

Onboard resources and their planned functions (full justification in
[docs/design.md §3](docs/design.md#3-component-inventory--justification)):

| Resource | Planned function |
|---|---|
| AHT10 (I2C) | Room temperature/humidity thresholds, display, log |
| AP3216C (I2C) | Room lighting (night dimming); proximity = placement/obstruction indicator only, **never** infant presence |
| ICM20608-G (I2C) | Device motion/orientation (knocked/tipped fault detection) — not infant movement |
| Electret mic M1 → ES8388 codec (I2S3) | Baby-cry audio capture, differential LIN1 input (schematic sheet 9/10) — no external mic module needed |
| ES8388 DAC → audio jack | Loopback verification of the capture path |
| RW007 Wi-Fi (SPI2) | MQTT/HTTP transport to the backend |
| ST7789 240×240 LCD (FSMC) | Local status dashboard |
| Buzzer (TIM3 PWM), RGB LEDs / LED matrix | Local normal/alert/fault indication |
| 4 arrow keys + KEY0 | Local acknowledge / mute / test actions |
| microSD (SDIO 4-bit) | Offline event log + replay queue, WAV capture evidence |
| RTC (CR1220 + LSE) + SNTP | Event timestamping |
| W25Q128 SPI flash | **Excluded** — shares SPI2 with the RW007 (documented in design.md §3) |

## Project structure

| Path | Purpose | Status |
|---|---|---|
| `src/` | Firmware: drivers, Zephyr threads, decision logic, ML inference | does not exist yet |
| `include/` | Public headers, incl. the generated `cry_model.h` | exists (P0: LCD/LED headers) |
| `test/` | Host-native Unity unit tests (`pio test -e native`) | does not exist yet |
| `ml_model/` | Dataset prep, training script, emlearn export, model report | does not exist yet |
| `backend/` | Mosquitto config, FastAPI + SQLite service | does not exist yet |
| `android_app/` | Android caregiver app (notifications, history, ack) | does not exist yet |
| `zephyr/` | `prj.conf`, `CMakeLists.txt`, board overlay | exists (P0) |
| `docs/` | Course spec PDF, project plan, design document | exists |

## Build & Flash

Prerequisites:

- [Visual Studio Code](https://code.visualstudio.com/) with the
  [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=PlatformIO.platformio-ide)
- ST STM32 platform: `pio platform install "ststm32"`
- Git

Commands (per `platformio.ini`: `board = black_f407zg`, `framework = zephyr`):

```bash
pio run                   # build the firmware
pio run -t upload         # flash via the onboard ST-Link
pio device monitor        # serial console @ 115200 baud (ST-LINK VCP, USART1)
```

The monitor speed is **115200** (USART1 through the onboard ST-LINK virtual COM
port). Once the test suite exists, host-side verification follows the course
contract: `pio test -e native` (Unity) and `pio check` (Cppcheck, 0 defects).

## Documentation

- [docs/project-plan.md](docs/project-plan.md) — master plan: decisions & constraints, system architecture, component inventory, phase plan P0–P8, risk register, rubric cross-check
- [docs/BCA182 - Project 1.pdf](docs/BCA182%20-%20Project%201.pdf) — the official course specification (MSU-IIT)
- [docs/design.md](docs/design.md) — design document (skeleton, filled in per phase)

## Status

| Phase | Scope | Status |
|---|---|---|
| P0 | Board bring-up | **Complete** (build+flash+serial verified) |
| P1 | Sensing & alerts | **Complete** (21/21 tests, cppcheck 0) |
| P2 | Audio capture | Pending |
| P3 | Cry ML | Pending |
| P4 | Wi-Fi & MQTT | Pending |
| P5 | Backend service | Pending |
| P6 | Android app | Pending |
| P7 | Integration testing | Pending |
| P8 | Submission docs | Pending |

## License & Acknowledgements

Coursework for BCA182 Embedded Systems Programming, MSU-IIT. Per the spec's
attribution rule ("code taken or adapted from public examples must be attributed"):

- Board drivers and infrastructure are adapted from the author's **own prior
  coursework** in this repository — Laboratory 2 (ES8388 codec, ST7789 LCD),
  Laboratory 3 (native RW007 Wi-Fi driver, emlearn ML deployment path, Android app
  skeleton), and Laboratory 4 (AHT10/AP3216C/ICM20608 drivers, SD/FatFs logging,
  alert state-machine pattern) — and are re-verified per phase rather than copied
  blindly.
- The onboard resource pinout is taken from the **RT-Thread RT-Spark schematic**
  (sheets 7/10 and 9/10), which is also the basis for the external-module
  justification (none required).

## Safety

**Classroom prototype — not a medical device. Not intended for infant safety or medical use.**
