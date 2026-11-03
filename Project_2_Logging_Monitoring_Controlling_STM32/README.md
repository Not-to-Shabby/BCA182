# Project 2 — BenchWatch: Logging, Monitoring & Controlling (RT-Spark / STM32F407)

**BCA182 Embedded Systems Programming — Project 2** · "Logging, Monitoring, and
Controlling Using the STM32F407-based Development Board" (100 pts)

BenchWatch is a laboratory **workbench sentinel**: it monitors environmental
conditions, light/proximity and bench motion, **logs** every measurement into
InfluxDB, **monitors** current and historical conditions on a Grafana
dashboard, and **controls** onboard outputs (buzzer, LED matrix, LCD
backlight) through validated MQTT commands with acknowledgments.

```
┌─────────────────────────────┐  MQTT  ┌──────────────────────┐  write  ┌────────────────┐
│ RT-Spark STM32F407 (Zephyr) │ ─────► │ Mosquitto broker     │ ──────► │ InfluxDB 2.7   │
│ sensor/net/control/ui tasks │ ◄───── │ (docker, LAN)        │         │ (time-series)  │
└─────────────────────────────┘  cmd   └──────────────────────┘         └───────┬────────┘
        ▲ local buttons + buzzer + LED matrix + ST7789 LCD      ack             │ query
        │                                                ┌──────────────────────┐▼
        └──────────── priority: local > remote ──────────│ Python ingestion svc │  Grafana
                                                         └──────────────────────┘  dashboards
```

## Hardware inventory (RT-Spark, all onboard)

| Component | Bus | Use in BenchWatch |
|---|---|---|
| AHT10 temperature/humidity | soft I2C (PF0/PF1) | bench thermal comfort telemetry |
| AP3216C light/proximity | soft I2C | light level + "someone at bench" wake |
| ICM20608 accel/gyro | SPI | bench disturbance / orientation events |
| SK6805 LED matrix | GPIO | status output (`led` command: off/status/rainbow) |
| ST7789 LCD | GPIO | live dashboard, backlight auto-wake on proximity |
| Buzzer | GPIO (PA8, verify) | alert output (`buzzer` command + motion beep) |
| KEY0/KEY1 buttons | PC0/PC1 | local input: mute window / LED mode (local > remote) |
| RW007 Wi-Fi | SPI2 (PB13/14/15) + PF10/PG15/PG11 | MQTT transport (verified Lab 3 driver) |
| microSD (SDIO 4-bit) | SDMMC | optional offline log buffering |
| W25Q64 SPI flash, RTC, IR | SPI/I2C | inventoried; not required by scenario |

## Repository layout

```
├── docs/                 assignment PDF, plan.md (phases, deviation note, test matrix)
├── include/ src/         Zephyr firmware (4 tasks + RW007/sensor/display drivers)
├── test/test_telemetry/  host-native Unity tests (hardware-independent logic)
├── tools/stack/          docker compose: mosquitto + influxdb + grafana + provisioning
├── tools/ingestion/      MQTT -> InfluxDB ingestion service (Python)
└── zephyr/               prj.conf, CMakeLists.txt, board overlay (PlatformIO/Zephyr)
```

## Quick start

### 1. Firmware (VS Code + PlatformIO)

```bash
pio run                 # build (framework = zephyr, board black_f407zg)
pio run -t upload       # flash via ST-LINK
pio device monitor      # 115200 8N1 on the ST-LINK VCP
pio test -e native      # host-native Unity suite
```

Set `APP_BROKER_HOST` in `include/app_config.h` to your PC's LAN IP, and adapt
`APP_MQTT_TOPIC_BASE` (must be unique per group). Wi-Fi credentials are stored
in flash (wifi_storage, carried over from Lab 3); no secrets are committed.

### 2. Backend stack

```bash
cd tools/stack
docker compose up -d    # mosquitto :1883, influxdb :8086, grafana :3000, ingestion
```

Grafana opens with a provisioned "BenchWatch" dashboard (temperature/humidity,
light/proximity, motion + events, link health + actual output state, acks).
Change the placeholder InfluxDB token in `docker-compose.yml` and the matching
datasource provisioning file before any non-lab use.

### 3. Remote control

```bash
mosquitto_pub -h <broker> -t bca182/group-1/benchwatch/cmd/indicator \
  -m '{"id":1,"cmd":"buzzer","arg":"on"}'
mosquitto_pub -h <broker> -t bca182/group-1/benchwatch/cmd/indicator \
  -m '{"id":2,"cmd":"led","arg":"rainbow"}'
```

Every command is answered on `.../ack` with the command id, accepted/rejected
and the **actual** resulting state. Malformed or unknown commands are rejected,
never guessed. KEY0 opens a 60 s local mute window that outranks remote buzzer
commands (documented priority rule).

## RTOS deviation note

The assignment prescribes STM32Cube HAL/LL + FreeRTOS. This project uses
**Zephyr RTOS** (`framework = zephyr`) to reuse the verified RW007 SPI Wi-Fi
stack and toolchain from Labs 2-4; the full justification is in
[docs/plan.md §2](docs/plan.md).

## Verification (draft matrix — full results in the report)

| Category | Demonstration |
|---|---|
| Onboard peripherals | AHT10/AP3216C/ICM20608 live readings, buttons, buzzer, LED matrix, LCD |
| Task behavior | 4 concurrent Zephyr threads; msgq + mutex IPC; absolute-deadline timing |
| Telemetry & persistence | device MQTT message → ingestion → InfluxDB → Grafana panel |
| Remote command | buzzer + led (+ lcd, ping); malformed rejected; ack reflects actual state |
| Failure & recovery | broker/Wi-Fi down: sensing continues; MQTT resumes without reflash |
| Sustained run | 30 min continuity + ≥10 command-response trials (min/median/max) |
