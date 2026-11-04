# Project 1 — Smart Baby Monitoring System: Master Plan

**Course:** BCA182 Embedded Systems Programming · **Points:** 100 · **Board:** RT-Thread RT-Spark (Spark-1, STM32F407ZGT6, HW1.1)
**Status:** Planning complete 2026-11-04 · **RTOS decision:** Zephyr RTOS (see §1.1)

---

## 1. Decisions & Constraints

| Decision | Choice | Rationale |
|---|---|---|
| RTOS | **Zephyr** (PlatformIO `framework = zephyr`, board `black_f407zg`) | User decision (same precedent as Lab 4). RW007 Wi-Fi driver already Zephyr-ported and proven in Lab 3; Zephyr native TCP/IP + MQTT subsys eliminates the largest STM32Cube-route risk (no TCP/IP stack anywhere in our codebase). |
| Toolchain | VS Code + PlatformIO, `pio run` / `pio test -e native` / `pio check` | Per spec |
| Cry inference | **On-device** (emlearn RandomForest), backend endpoint as documented fallback | Agent estimates: ~29-feature MFCC RF = 5–20 KB flash, µs inference, <0.2 % CPU — fits easily; spec permits on-device when measured RAM/timing prove feasibility |
| Audio source | **Onboard electret mic** → ES8388 ADC (differential LIN1) → I2S3_ext RX | Schematic sheet 9/10 confirms onboard mic (M1 BA012AP42 → MIC_P/MIC_N → ES8388 pins 24/23). Spec's "external mic required" claim is wrong for HW1.1 — cite schematic, no external BOM |
| Transport | MQTT (Zephyr `CONFIG_MQTT_LIB` over zsock), no TLS in baseline (LAN, documented) | MbedTLS RAM cost on F407 to be measured; TLS = stretch goal |
| Event storage | SD card (FatFs) primary, RAM ring fallback | Lab 4 `data_logger.c` pattern proven |
| Live data to app | HTTP polling (5 s latest / 10 s unacked events), not WebSocket/FCM | Agent E: self-healing, no reconnect state machine, no Firebase setup |

### 1.1 RTOS deviation note (required deliverable)

The spec bans RT-Thread OS/Studio, Arduino, and ESP-IDF by name — **Zephyr is not on the ban list** — but the toolchain section mandates `framework = stm32cube` + FreeRTOS, and rubric line 2 reads "STM32Cube HAL + FreeRTOS firmware (25 pts)". We will document the deviation in `docs/design.md` §RTOS-deviation (modeled on Lab 4 `docs/design.md` §8): Zephyr-not-banned argument, an RTOS-concept equivalence table (threads/tasks, k_msgq/queues, k_mutex/mutexes, k_sem/semaphores), and the engineering rationale. **Risk acknowledged:** partial or full loss of that rubric line is possible; accepted by user.

---

## 2. System Architecture

```
┌─────────────────────────────┐        ┌──────────────────────────────┐        ┌──────────────────┐
│ RT-Spark device (Zephyr)    │        │ Backend (PC/SBC, LAN)        │        │ Android app      │
│                             │ MQTT   │                              │  HTTP  │                  │
│ SensorTask (AHT10/AP3216C/  │──────▶ │ Mosquitto broker             │──────▶ │ Live dashboard   │
│   ICM20608 over soft-I2C)   │ HTTP   │ FastAPI service:             │ (poll) │ Cry notification │
│ AudioTask (mic→ES8388→      │        │  - ingest → SQLite           │        │ History + search │
│   I2S3_ext→MFCC→cry model)  │        │  - REST API (latest/history/ │        │ Ack + settings   │
│ AlertTask (thresholds,      │        │    ack, settings, unacked)   │        │ Stale indication │
│   hysteresis, ack, buzzer)  │        │  - debounced event store     │        │ Disclaimer       │
│ NetTask (RW007 SPI, DHCP,   │        │  (optional: /api/infer)      │        │                  │
│   MQTT pub/sub, resilience) │        └──────────────────────────────┘        └──────────────────┘
│ UiTask (ST7789 dashboard)   │
│ LogTask (SD FatFs events)   │
└─────────────────────────────┘
```

Data flow: sensors + mic → device tasks → MQTT publish (`babymonitor/reading`, `babymonitor/event`) → backend ingests to SQLite → REST API → app polls. Cry event → backend stores as unacked event → app notification → ack POST → device LED/LCD/buzzer acknowledge state. Offline: device queues events to SD with RTC-relative timestamps, replays on reconnect (resilience requirement).

---

## 3. Component Inventory & Justification (rubric §"System design")

| Resource | Function | Source |
|---|---|---|
| AHT10 (I2C 0x38, soft-I2C PF1/PF0) | Room temp/humidity; display, log, threshold alerts | Lab 4 driver, drop-in |
| AP3216C (I2C 0x1E) | Room lighting (night dimming of alerts); proximity = placement/obstruction indicator only, **never** infant presence | Lab 4 driver, drop-in |
| ICM20608-G (I2C 0x68) | Device motion/orientation (knocked/tipped fault detection) | Lab 4 driver, drop-in |
| **Onboard electret mic M1 → ES8388** | **Baby-cry audio capture**, 16 kHz mono 16-bit | Schematic sheet 9/10; Lab 2 codec driver (TX proven, RX = new) |
| ES8388 DAC → CN3 jack | Loopback verification, optional local audio cue | Lab 2, proven |
| RW007 Wi-Fi (SPI2, CS PF10/INT PG11/RST PG15) | MQTT/HTTP to backend | Lab 3 native driver, proven |
| ST7789 240×240 (FSMC NE3) | Status dashboard: readings, cry/alert/fault states, device condition | Lab 2/3/4 driver + UI pattern |
| Buzzer (TIM3 CH3 PB0) + RGB LEDs (PF11/12, PE3) | Local normal/alert/fault indication; ack/mute via buttons | Lab 2 PWM code; Lab 3/4 LED patterns |
| 4 arrow keys + KEY0 (PC0/1/4/5, PA0) | Local acknowledge / mute / test actions | Lab 3/4 button code |
| SD card (SDIO 4-bit, DMA2 S3/S6) | Event + reading log (offline queue), WAV capture evidence | Lab 4 `sd_card.c`/`data_logger.c` |
| W25Q128 SPI flash (SPI2, CS PB12) | **Not used** — shares SPI2 with RW007; documented in inventory (excluded by bus conflict) | Schematic sheet 7/10 |
| RTC (CR1220 + LSE 32.768 kHz) + SNTP | Event timestamping | New, small (Zephyr RTC + SNTP sample) |
| **No external hardware** | Onboard mic makes external mic module unnecessary | Schematic justification |

---

## 4. Reusable Asset Map

| Phase | From | Assets (verified paths) |
|---|---|---|
| Skeleton | Lab 4 | `platformio.ini` (cppcheck + native Unity env), `zephyr/CMakeLists.txt`, `prj.conf` baseline, `black_f407zg_pro.overlay` (console/rng/sdmmc1), boot-probe pattern, `sleep_until()` |
| Sensors | Lab 4 | `soft_i2c.c/h`, `aht10.c/h`, `ap3216c.c/h`, `icm20608.c/h` (graceful-ABSENT built in) |
| Audio | Lab 2 + Side_Quest | `audio_codec_es8388.c/h` (ADC regs already configured — see §P2), Side_Quest `k_sem` DMA handoff pattern, `audio_engine.c:85–172` buzzer PWM |
| Decision logic | Lab 4 | `cycle_logic.c` hysteresis + one-shot-ack state machine skeleton (pure C, host-tested) |
| ML | Lab 3 + Lab 4 | emlearn inline-export pipeline; `train_cycle_model.py` CLI/report structure; `train_model.py` header-wrapping + confidence wrapper; CMSIS-DSP MFCC (new, per §P3) |
| Connectivity | Lab 3 (+ Project_2 CONFIG precedent) | `rw007_wifi.c/h`, `rw007_netif.c/h`, `wifi_storage.c/h`, `wifi_portal.c/h` (optional), verified `CONFIG_NET_*`+`CONFIG_MQTT_LIB` block |
| Logging | Lab 4 | `sd_card.c/h` (keep SD-recovery), `data_logger.c/h` (retarget path/header) |
| Android | Lab 3 | `android_app/` Gradle skeleton, Material dark theme, callback→UI pattern |

---

## 5. Phase Plan

Estimates in student-days; ⊕ = parallelizable. Exit criteria are the evidence gates — a phase is not "done" until its commit is pushed and its exit criteria demonstrably pass.

### P0 — Scaffold & board bring-up (1.5 d)
- Create repo structure (§4 copy-list minimal): `platformio.ini` (from Lab 4, `test_ignore = test_baby`), `zephyr/{CMakeLists.txt, prj.conf, boards/black_f407zg_pro.overlay}` (console→USART1 + `&rng`), `.gitignore` additions (secrets, `android_app/local.properties`, datasets, `ml_model/data/`), README stub.
- Bring-up: banner on ST-LINK VCP, LED-blink thread, one `sleep_until()` periodic thread, LCD hello.
- Copy LCD + LED matrix + buttons; verify flash via onboard ST-Link.
- **Exit:** `pio run` 0 errors/warnings; boot log clean; LED+LCD alive; GitHub repo created, initial commit pushed.

### P1 — Local sensing, UI & alerts (2.5 d)
- Copy Lab 4 sensor drivers; SensorTask at 2 s period (deadline-resync); latest-value `k_msgq` pattern.
- `baby_logic.c` (pure C): temp/humidity thresholds with hysteresis, alert state machine (normal/alert/fault) with one-shot alert until ack (from `cycle_logic.c`), device-motion fault from ICM20608.
- Dashboard UI (Lab 4 differential-redraw pattern): readings, state, last-update; buzzer PWM alarm; LED matrix tint; button ack/mute.
- Unity native tests for `baby_logic` (boundary + transition tests); `pio check` 0 defects.
- **Exit:** live dashboard reflects controlled changes (warm air, darken, tilt); ack silences alarm; 10+ tests green.

### P2 — Audio capture path (3.5 d) ⚠ highest technical risk — full spec captured from subagent
Facts verified against Lab 2 code + RM0090/AN4031:
- ES8388 ADC is **already configured** by `es8388_codec_init()` (0x00=0x12 play&record, 0x09=0xBB PGA, 0x0A=0x00 LIN1/RIN1 differential = mic wiring, 0x0C=0x0D/0x0D=0x02 16-bit ratio-256, 0x03=0x09). **Add:** `0x0F=0x60` (ADC unmute w/ fade — Lab 2 never writes it); make PGA 0x09 tunable (0xBB ≈ +33 dB may clip the electret — try 0x66/0x88); optional ALC regs 0x12–0x16 documented as option.
- RX data line is **PB4** (`I2S3_ext_SD` ← ADCDAT), not PB5 (PB5 = TX/DACDAT). De-JTAG PB4 (NJTRST→AF6), same as proven PB3/PA15.
- **DMA1 Stream 2 Channel 2** = I2S3_EXT_RX (ST AN4031) — Stream 0/1 Ch0 assumption was wrong; Stream 2 is free (DMA1 S5 Ch0 = TX, DMA2 S3/S6 Ch4 = SDIO). Circular, 16-bit, 512 words, ping-pong halves, `k_sem` handoff (Side_Quest pattern); ISR clears **LISR/LIFCR** (low set — Stream 2, don't copy TX's HISR/HIFCR).
- I2S3ext: slave RX, `I2SCFGR = I2SMOD | I2SCFG_0 | I2SCFG_1`, `CR2 |= RXDMAEN`; discard first half-buffer (alignment), count `SR.OVR`.
- Clock: one shared WS/CK ⇒ single bus rate. Run everything at 16 kHz: keep PLLI2S N=271/R=2 (135.5 MHz) with `I2SDIV=16, ODD=1` → 16039 Hz (+0.24 %), **or** exact: PLLI2SN=213/R=2 + I2SDIV=13 → 16003.6 Hz. Codec regs need no change (ratio 256). `CONFIG_PM=n` (STOP kills PLLI2S — Side_Quest precedent).
- RAM: mono 16 kHz int16 = 32 KB/s; 3 s ≈ 96 KB in SRAM (CCM is not DMA-reachable — keep DMA buffers out of CCM).
- Steps: codec-ADC enable → PB4 pinmux → I2S3ext+DMA RX → capture thread → 3 s RAM record → loopback playback (mono→duplicate L,R into proven TX staging path at 16 kHz) → WAV dump to SD as evidence.
- **Exit:** loopback audible + correct duration/rate; WAV on SD verified on PC; OVR=0 over 5 min soak; CPU load + RAM measured and logged.

### P3 — Cry ML pipeline (5 d) ⊕ PC-side work parallel with P2
- Datasets (agent-verified licenses): **train-cry** = donateacry-corpus (ODbL 1.0, ~457 clips, ffmpeg .3gp→16 kHz WAV); **train-neg** = ESC-50 non-cry classes folds 1–3 (excluding all crying_baby); **val** = ESC-50 fold 4 + held-out donateacry recordings; **independent test (frozen)** = all 40 ESC-50 crying_baby clips (zero seen in training — leakage structurally impossible via fold design) + ≥20 ESC-50 fold-5 non-cry + **confusers** = UrbanSound8K children_playing/street_music, ESC-50 siren/chainsaw, ≥10 silence/low-energy windows.
- Features: 16 kHz mono, 512-sample Hann frames hop 256 → `arm_rfft_fast_f32` → 40 mel → log → `arm_mfcc_f32` 13 MFCCs → per-window mean/std aggregation ≈ 29 features (RAM ~4 KB + 16–32 KB ring; ~1–2 ms CPU per s of audio). Goertzel band-energy fallback (Lab 4 pattern) if CMSIS-DSP integration drags. PC/NumPy extractor = documented mirror; unit-test PC↔C parity.
- Model: sklearn RandomForest 10×depth 5–8 → emlearn inline C (`eml_trees`); fallback MLP 1×32 via `eml_net`. Trainer `train_cry_model.py` from Lab 4 CLI pattern; `model_report.txt` = confusion matrix + P/R/F1 + confuser table + rule-based baseline row. Realistic target: F1 ≥ 0.85 on curated test (literature band 70–95 %, cite).
- Event confirmation rule: threshold + 2-of-3 consecutive positive windows (spec's debounce requirement).
- **Exit:** model committed as `include/cry_model.h` + provenance; report with matrix/metrics; RAM/timing table; Unity tests for aggregator.

### P4 — Connectivity (2.5 d) — full port checklist captured from subagent
- Copy `rw007_wifi.*`, `rw007_netif.*`, `wifi_storage.*` (+ `wifi_portal.*` optional; stub its 3 UI hooks if UI not ready). Delete dead `rw007_wifi_ble_cfgwifi` decl. Overlay: console+rng only; **leave `&spi2` unclaimed** (driver owns registers; driver holds PB12 high — flash bus conflict handled).
- prj.conf: LAB_4 baseline + Project_2-verified net block (`NET_L2_ETHERNET`, `DHCPV4`, `SOCKETS`, `TCP`, `MQTT_LIB`, `ENTROPY_STM32_RNG` + `&rng`, DNS resolver, heap 12288). Raise `NET_PKT_*`/`NET_BUF_*` for TCP+MQTT after first socket test.
- Integration: `rw007_wifi_init()` → scan → saved-cred join (wifi_storage flash sector 11) → portal fallback (180 s, with its clean DHCP-server teardown ordering preserved) → `net_dhcpv4_start`.
- **Must add:** link-loss supervisor (poll `rw007_wifi_is_connected()` / react to `EVT_DISCONNECT` → carrier off, DHCP stop, re-join, backoff), exponential backoff, offline event queue (SD) + replay, device-fault states (never-connected / dropped / server-unreachable) on LCD, driver watchdog (RST pulse + re-init).
- MQTT topics: `babymonitor/reading` (JSON: temp, hum, lux, proximity, motion, ts), `babymonitor/event` (cry/env/fault, ack-id), subscribe `babymonitor/cmd` (ack, mute).
- Gotchas: credential flash write holds `irq_lock()` ~1–2 s — never during MQTT traffic; single SPI TX slot (`-EBUSY` under load — acceptable, TCP retransmits).
- **Exit:** MQTT pub/sub with Mosquitto verified; disconnect/reconnect demo log; fault state visible; events queued offline delivered on recovery.

### P5 — Backend service (2.5 d) ⊕ parallel with P2/P3
- Mosquitto (auth: per-device user/pass) + FastAPI + SQLite (`readings`, `events`, `acks`, `settings` tables). Subscriber: MQTT→DB. REST: `GET /api/latest`, `GET /api/history?type=&search=&limit=`, `GET /api/events/unacked?after=`, `POST /api/ack/{id}`, `POST /api/settings`, optional `POST /api/infer` (backend classifier fallback). Debounce: event store dedups within window. Token auth; secrets via `.env` (gitignored).
- **Exit:** end-to-end device→broker→DB→API with logs; curl evidence for each endpoint.

### P6 — Android app (4.5 d) — full file-by-file plan captured from subagent
- Base: copy Lab 3 `android_app/` → package `com.msuiit.bca180.babymonitor`; keep Material dark theme + callback pattern; delete `BleManager.kt`, `UdpReceiver.kt` (keep as threading template), `ActivityPacket.kt`.
- New: `MonitorRepository.kt` (OkHttp 4.12, single-thread executor, `Handler(mainLooper)` callbacks, adaptive backoff 5→30 s), `NotificationHelper.kt` (channels `cry_alerts` IMPORTANCE_HIGH + `system_status` LOW; POST_NOTIFICATIONS runtime request; dedupe via lastEventId; mute-until), `HistoryActivity` (backend-fed RecyclerView + search + filter — no Room), `SettingsActivity` (server addr, mute duration, sound), `StalePolicy` (green <15 s / amber 15–60 s / red >60 s, dimmed values).
- Manifest: add POST_NOTIFICATIONS + `usesCleartextTraffic="true"` (easily-forgotten LAN-HTTP blocker); drop multicast/Wi-Fi-state perms.
- Disclaimer string in 4 places: dashboard footer, every cry notification subtext, first-launch "I understand" dialog, History/Settings footers: *"Classroom prototype — not a medical device."*
- **Exit:** cry event → heads-up notification → ack round-trip; history searchable; stale pill correct; airplane-mode test shows offline banner without freeze.

### P7 — Integration, testing & evaluation (3.5 d)
- Classifier eval on device: replay 40 unseen cry + ≥20 non-cry + confuser clips (SD → feature path), on-device confusion matrix vs PC-sklearn drift check.
- E2E notification: ≥10 triggered trials, capture-to-notification latency measured (device DWT + backend ts + app ts).
- Resource measurements: per-thread stack high-water (`k_thread_stack_space_get`), heap, task periods, worst latency, DWT inference bench — evidence table.
- Resilience: Wi-Fi off/on, backend kill/restart, SD-removal, sensor-absent boot (graceful-ABSENT).
- Reference check: AHT10 vs a reference thermometer; document conditions/limits.
- Full unit suite + `pio check` 0 defects; all evidence into `docs/test-results.md`.
- **Exit:** every "Testing and Evaluation" table row of the spec has recorded evidence.

### P8 — Documentation & submission (2.5 d)
- `docs/laboratory-report.md` (objective, architecture, inventory + external-module justification = "none, schematic sheet 9/10", wiring/pin diagram, task/communication design, data flow; Mermaid diagrams per Lab 3 precedent) → PDF.
- ML documentation: dataset provenance/licenses, split protocol, matrix, inference-location rationale (on-device, measured).
- README: exact build env, reproducible `pio` setup/flash/monitor, wiring notes, evidence links.
- Demo video: live device, app notification, history, controls, ≥1 network failure/recovery.
- RTOS deviation note (§1.1) finalized in `docs/design.md`.
- **Exit:** tag `v1.0-submission` on GitHub; all deliverables present.

**Total ≈ 28 student-days (~5–6 weeks single-stream; P3-PC/P5/P6 overlap compresses to ~4–5 weeks).**

---

## 6. Risk Register

| # | Risk | L×I | Mitigation |
|---|---|---|---|
| R1 | Rubric penalizes Zephyr vs mandated STM32Cube+FreeRTOS | M×H | Deviation note (§1.1); keep all decision logic pure-C host-tested; equivalence table |
| R2 | I2S3_ext RX unproven in our codebase | M×H | P2 is a dedicated phase with loopback-first strategy; **fallback:** ESP32-S3 as external mic module (proven 2 Mbps UART protocol, reverse direction) — pre-justified BOM entry |
| R3 | Mic PGA clipping / poor SNR | M×M | Tunable 0x09, peak logging, ALC option |
| R4 | Classifier F1 below target | M×M | Honest framing vs literature; confuser analysis; threshold/confirmation tuning on val set only; MLP fallback |
| R5 | Dataset conversion friction (donateacry .3gp/.caf) | L×M | ffmpeg batch script early in P3; ESC-50 fallback negatives |
| R6 | MbedTLS RAM on F407 | H×L | Baseline is LAN MQTT without TLS; TLS measured as stretch |
| R7 | RW007 SPI/flash-write stalls | L×M | §P4 gotcha list (write creds before traffic) |
| R8 | SD FIFO overrun / card quirks | L×M | Keep proven `clk-div=10` + Lab 4 recovery path |
| R9 | App cleartext HTTP blocked | L×M | Manifest flag day-one (§P6) |

## 7. Commit Discipline (per AGENTS.md)

One conventional-imperative commit per milestone, e.g.:
`Scaffold Project 1 PlatformIO/Zephyr structure and board bring-up` → `Integrate onboard sensors with LCD dashboard and alert state machine` → `Bring up onboard microphone capture via ES8388 ADC and I2S3_ext RX DMA` → `Train baby-cry RandomForest and export emlearn model header` → `Add RW007 Wi-Fi connectivity with MQTT publishing and reconnect resilience` → `Implement FastAPI backend with SQLite event store` → `Develop Android monitoring app with notifications, history, and ack` → `Complete integration testing: classifier eval, e2e trials, resource measurements` → `Add project report, ML documentation, and demo video (submission tag)`.

## 8. Rubric Cross-Check

| Criterion (pts) | Covered by |
|---|---|
| System design & component justification (15) | §3 inventory + schematic citations, pin map, board manifest (P0), architecture §2 |
| STM32Cube HAL + FreeRTOS firmware (25) | ⚠ Deviation: Zephyr firmware with full task/IPC design + measured memory/timing (P7). Partial-risk documented (§1.1) |
| Baby-cry ML pipeline (20) | P3 + P7 eval: cited datasets, leak-free unseen test, matrix/P/R/F1, thresholds, on-device rationale |
| Backend, connectivity & mobile app (20) | P4+P5+P6: MQTT/TLS-tradeoff, secure secret handling, timely debounced alerts, history, stale/error states |
| Testing, reliability & safety (15) | P7 evidence tables, fault recovery, disclaimers, safety limits (no breathing/medical claims) |
| Documentation & reproducibility (5) | P8: README, report, ML docs, tests, tagged release, demo video |
