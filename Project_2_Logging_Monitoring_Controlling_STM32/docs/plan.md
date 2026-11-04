# BenchWatch — Project 2 Plan, Architecture & Deviation Note

Target: **Project 2 — Logging, Monitoring, and Controlling Using the
STM32F407-based Development Board** (BCA182, 100 pts, Asst. Prof. Paul Rodolf
P. Castor, © September 2026).

## 1. Application scenario (original & justified)

**BenchWatch — laboratory workbench sentinel.** A shared electronics bench is
left unattended for long periods: heat buildup damages equipment, lights get
left on, and instrument pilfering/disturbance goes unnoticed.

- **AHT10** (temp/humidity): thermal-comfort logging of the bench area; long
  trends identify ventilation problems.
- **AP3216C** (light/proximity): light-on detection and "someone is at the
  bench" wake for the display (proximity threshold).
- **ICM20608** (accel/gyro): bench disturbance events (knocks, tipping) from
  the acceleration magnitude leaving ±0.35 g around 1 g.
- **Local feedback**: ST7789 live dashboard, LED-matrix link/mode status,
  buzzer beep on disturbance, KEY0 local mute, KEY1 local LED mode.
- **Remote control** (≥2 distinct commands): `buzzer on/off`, `led
  off/status/rainbow`, plus `lcd on/off` and `ping`. Every command is
  acknowledged with the **actual** resulting state; malformed payloads are
  rejected. Priority rule: **local input outranks remote buzzer commands**
  while a KEY0-triggered 60 s mute window is open.

## 2. RTOS deviation note (Zephyr instead of STM32Cube + FreeRTOS)

The assignment prescribes STM32Cube HAL/LL + FreeRTOS + PlatformIO. This
project uses **Zephyr RTOS** (`framework = zephyr` in PlatformIO, same
board/toolchain) for the following documented reasons:

1. **Verified RW007 Wi-Fi driver.** The board's Wi-Fi module has no
   STM32Cube HAL driver. Under Zephyr, the Laboratory 3 RW007 SPI driver is
   already ported, hardware-verified on this exact board, and exposes a full
   Ethernet-class `netif` with BSD sockets, TCP, and DHCP — which is
   precisely what an MQTT client needs. Re-implementing the RW007 SPI
   protocol from scratch under STM32Cube would consume the project's schedule
   without contributing to the graded learning outcomes.
2. **Verified peripheral drivers.** AHT10, AP3216C, ICM20608, ST7789,
   LED-matrix, microSD drivers were hardware-verified in Labs 3-4 and are
   reused here unchanged (plain C over GPIO/SDMMC).
3. **Proven toolchain.** The PlatformIO/Zephyr build, Unity host tests, and
   cppcheck flow are established across Labs 2-4, reducing integration risk.
4. **Concept parity.** The assignment's firmware-design criteria (concurrent
   tasks, priorities, queues/mutexes, `vTaskDelayUntil`-style periodic
   scheduling, error handling) are all satisfied 1:1 with Zephyr threads,
   `k_msgq`/`k_mutex`, and absolute-deadline sleeps.

The deviation is declared up front (platformio.ini header, README, report) so
instructor sign-off can be sought before the demonstration, as in Laboratory 4.

## 3. Firmware architecture

| Thread | Prio | Period | Responsibility |
|---|---|---|---|
| `net_thread` | 4 | event-driven (poll 50-100 ms) | Wi-Fi/DHCP bring-up, MQTT session, reconnect with backoff, publish telemetry (5 s), status (30 s), acks (immediate) |
| `sensor_thread` | 5 | 200 ms absolute deadline | ICM20608 every cycle; AHT10 + AP3216C every 2 s; snapshot to state store + telemetry queue |
| `control_thread` | 6 | 50 ms | debounced KEY0/KEY1, command application + priority rules, acks, buzzer/LED matrix |
| `ui_thread` | 12 | 500 ms | LCD dashboard, backlight wake on proximity |

**IPC:** `g_telemetry_q` (sensor→net, 8×snapshot), `g_cmd_q` (net→control),
`g_ack_q` (control→net), `g_state_lock` mutex guarding the latest snapshot.
Drops on queue-full are counted, never blocking sensing.

**Topics** (`bca182/<group-id>/...`, JSON):

| Direction | Topic | Payload |
|---|---|---|
| publish | `bca182/group-1/benchwatch/telemetry` | device_id, seq, ts, temperature_c, humidity_pct, light, proximity, ax..gz, motion_mag, motion_event, buzzer, led_mode, wifi, mqtt |
| publish | `.../status` | online, buzzer, led_mode, wifi, mqtt, uptime_s |
| subscribe | `.../cmd/indicator` | `{"id":N,"cmd":"buzzer\|led\|lcd\|ping","arg":"..."}` |
| publish | `.../ack` | id, accepted, reason, actual, ts |

Timestamps: device `ts` is uptime-ms (diagnostic only); the ingestion service
stamps every point with the **server UTC clock** (reliable timestamps
requirement). Backend: `tools/stack/docker-compose.yml` (Mosquitto, InfluxDB
2.7 bucket `benchwatch`, Grafana with provisioned dashboard, ingestion
service), `tools/ingestion/ingestion.py` validates payloads and counts
rejects.

## 4. Phase plan

| Phase | Scope | Exit criterion |
|---|---|---|
| 0 ✅ | Scaffold: build config, drivers ported, 4 threads, MQTT client, ingestion, Grafana, 14 native tests | firmware builds; tests pass |
| 1 | Hardware verify on RT-Spark: sensors, LCD, matrix, buttons, buzzer pin check | serial log shows live readings |
| 2 | Wi-Fi + MQTT end-to-end: broker on LAN, telemetry in InfluxDB, panels live | Grafana shows real data |
| 3 | Onboarding portal (SoftAP + DHCP server, from Lab 3) for credential entry without reflashing | phone can join device to Wi-Fi |
| 4 | Control hardening: QoS/keepalive tuning, malformed-command fuzz, ack trials ×10 (min/median/max) | test matrix rows green |
| 5 | Reliability: 30-min sustained run, Wi-Fi/broker drop recovery demo, microSD buffering (optional) | continuity log + missing-data report |
| 6 | Report (architecture, inventory, task design, topics, InfluxDB schema, Grafana panels), photos, verification matrix, portfolio README update | submission package complete |

## 5. Verification matrix (draft — fill before the demo)

| # | Category | Input / condition | Expected | Observed | Pass |
|---|---|---|---|---|---|
| 1 | Peripherals | breathe on AHT10 | temp/humidity change in telemetry | | |
| 2 | Peripherals | cover AP3216C | light↓, proximity↑, backlight wakes | | |
| 3 | Peripherals | tap bench | motion_event + local beep | | |
| 4 | Task behavior | run 5 min | 200 ms/2 s cadences held; no watchdog resets | | |
| 5 | Telemetry | publish 5 s cadence | points in InfluxDB with plausible server timestamps | | |
| 6 | Remote cmd | `{"id":1,"cmd":"buzzer","arg":"on"}` | buzzer on, ack `buzzer=on` | | |
| 7 | Remote cmd | `{"id":2,"cmd":"led","arg":"rainbow"}` | matrix rainbow, ack | | |
| 8 | Remote cmd | malformed payload | ack rejected `malformed_command`, no actuation | | |
| 9 | Priority | KEY0 mute, then buzzer on | ack rejected `local_override`; LED cmds pass | | |
| 10 | Recovery | broker down 30 s → up | sensing continues; MQTT reconnects, no reflash | | |
| 11 | Recovery | Wi-Fi AP down 30 s → up | reassociation + lease + MQTT resume | | |
| 12 | Sustained | 30 min run | expected vs received counts, errors, task health | | |
| 13 | Timing | 10 command-response trials | min/median/max reported | | |

## 6. Risk register

| Risk | Mitigation |
|---|---|
| Buzzer pin unverified (APP_BUZZER placeholder PA8) | check RT-Spark schematic in Phase 1 before flashing actuation |
| MQTT keepalive under Wokwi-like latency | LAN broker only; keepalive 30 s; tune in Phase 4 |
| Instructor requires exact STM32Cube stack | deviation declared early (§2); port layer of telemetry.c is framework-agnostic |
| Credential leakage | no secrets in repo: Wi-Fi in flash via wifi_storage, broker token via compose env placeholder |

## 7. Phase 1 hardware findings (2026-11-05, live on RT-Spark via ST-LINK COM7)

| Item | Result |
|---|---|
| Zephyr v4.4.0 boot | clean, single banner, no faults/asserts/reboots |
| AP3216C | present at 0x1E |
| ICM20608 | present, WHO_AM_I 0xAF at 0x68 |
| microSD | FatFs mounted at /SD: |
| ST7789 LCD | initialized over FSMC, backlight on |
| AHT10 | **not fitted on this board kit** ("Not detected at 0x38 — absent from kit"); matches the Laboratory 4 inventory finding. Documented limitation — driver retained; thermal telemetry unavailable on this unit |
| RW007 | module alive (v2.1.0-a7a0d089-57, STA MAC fc:58:4a:a6:e5:e2); scaffold re-ran init on every retry which the module rejects (CMD_INIT -2) — fixed to init-once + connect-only retries |
| Buzzer | placeholder PA8 replaced by PB0 TIM3_CH3 PWM (2.5 kHz), the pin verified by the Laboratory 2 audio engine |
| Wi-Fi credentials | not stored on flash → serial-console provisioning added (console_getline → wifi_storage_save); SoftAP portal remains optional (Phase 3) |

Broker endpoint set to the lab PC LAN IP 192.168.1.2:1883 (Mosquitto in tools/stack).
