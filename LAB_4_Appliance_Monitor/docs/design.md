# Laboratory Activity 4 — Design Document
## Laundry Sentinel: An Edge AI Appliance Cycle Monitor

**Course:** BCA182 Embedded Systems Programming
**Platform:** RT-Thread RT-Spark (STM32F407ZGT6), Zephyr RTOS, PlatformIO
**Student framework note:** This project deliberately uses **Zephyr RTOS** instead of the
default STM32Cube + FreeRTOS prescription. Rationale and risk are documented in
[Section 8](#8-rtos-deviation-zephyrtos-vs-stm32cubefreertos).

---

## 1. Consumer Product Concept and Design

### 1.1 Problem statement (one paragraph)

Laundry appliances are fire and water hazards when forgotten: a dryer left running
unattended overnight is a documented household fire risk, and wet clothes left in a
finished cycle grow mold and wrinkle within hours. The target user is a busy household
member (including deaf or hard-of-hearing users who cannot hear an end-of-cycle buzzer,
and users in apartments where the machine is on a different floor or in a shared
basement). The operating environment is a laundry room: strong whole-body vibration,
high humidity, and low ambient light. **Laundry Sentinel** is a small USB-powered
monitor placed on top of (or beside) the machine. It senses the machine's vibration
signature with the onboard ICM-20608 six-axis IMU, classifies the appliance state
on-device with a trained random-forest model, and reports **IDLE → WASH → SPIN → DONE**
on the LCD and 19-LED matrix, flashing an attention pattern when the cycle finishes
until the user acknowledges it. Every state transition is timestamped and logged to the
microSD card, so the product also answers "how long did this cycle actually take?" and
"did the machine ever stop unexpectedly?".

### 1.2 Alternative ideas considered

| # | Alternative | Why not selected |
|---|-------------|------------------|
| 1 | **Context-aware study desk** (IMU posture + AP3216C light → "take a break / lights too dim" coach) | Classification states are subjective (what is "good posture"?), labeled data is hard to collect consistently, and a false alarm has near-zero cost — weak acceptance criteria. |
| 2 | **Personal-item movement monitor** (IMU on a drawer/luggage → alert if moved) | Reduces to a threshold on acceleration magnitude; a trained model adds no measurable value over the baseline, which defeats the lab's ML-evaluation criterion. |
| 3 | **Laundry Sentinel (selected)** | Machine states are physically distinct (agitation ~1–3 Hz vs spin 10–25 Hz vs idle noise floor), data can be labeled reliably by simply running the real appliance through cycles, errors have a clear cost (false DONE alert wakes the household; missed DONE leaves clothes damp), and ≥ 3 meaningful states exist. |

### 1.3 Classification states

| State | Physical meaning | Signal signature |
|-------|------------------|------------------|
| `IDLE` (0) | Machine off or finished, user not yet responded | Vibration near noise floor (< ~0.02 g RMS) |
| `WASH` (1) | Agitation / tumbling phase | Strong low-frequency content (1–3 Hz), moderate RMS |
| `SPIN` (2) | High-RPM extract phase | Dominant 10–25 Hz component, high RMS, high crest factor |
| `DONE` (3) | **Derived event**: SPIN observed ≥ 10 s followed by sustained IDLE with decaying residual vibration | Alert until acknowledged |

The model itself classifies the three *measured* states (IDLE / WASH / SPIN);
`DONE` is derived deterministically by the state machine from a verified
`SPIN → IDLE` transition (documented in `cycle_logic.c`). This gives four
meaningful product states while keeping the trained model honest about what it
can actually observe.

### 1.4 Block diagram

```
                        ┌────────────────────────────────────────────────────────┐
                        │                RT-Thread RT-Spark (STM32F407ZGT6)      │
                        │                      Zephyr RTOS                       │
                        │                                                        │
  ICM-20608 (I2C2)─────►│ Sampler thread ──► SPSC ring (200 samples @ 100 Hz)    │
  100 Hz vibration      │       (prio 6)              │                          │
                        │                             ▼                          │
  AHT10 (I2C2)─────────►│ Inference thread (prio 10, every 1 s):                 │
  ambient T/RH (context)│   window → 14 features → RandomForest (emlearn)        │
                        │   + rule-based baseline ──► hysteresis state machine   │
  AP3216C (I2C2)───────►│        │                    │                          │
  ambient light (night  │        ▼                    ▼                          │
  mode / diagnostics)   │  ┌──────────┐          k_msgq(state)                   │
                        │  │ data_    │               │                          │
  KEY0/KEY1 buttons ───►│  │ logger.c │         ┌─────┴─────┐                    │
  (ack / service mode)  │  └────┬─────┘         ▼           ▼                    │
                        │       ▼          UI thread   LED matrix              │
                        │  microSD event  (prio 12,     (prio 12):              │
                        │  log (FatFs)    4 Hz LCD)     state colors + alert   │
                        └────────────────────────────────────────────────────────┘
                                                     │
                                                     ▼
                                     User: LCD dashboard + LED flash
                                     (DONE alert until KEY0 acknowledge)
```

### 1.5 Component-to-function matrix

| Component | Fitted? | Role | Product function |
|-----------|---------|------|------------------|
| ICM-20608 6-axis IMU | Verified (Lab 3 bring-up, I2C2 @ 0x68) | **Primary sensing** | 100 Hz vibration → windowed features → on-device state inference |
| AHT10 temp/humidity | Probe at boot; graceful ABSENT if not fitted | **Context sensing** | Ambient conditions logged with every event (mold-risk context for damp laundry) |
| AP3216C light/proximity | Probe at boot; graceful ABSENT if not fitted | **Context sensing** | Night mode: dims LCD and slows LED alert pulse in a dark laundry room |
| ST7789 1.3" LCD | Verified (Lab 2/3) | **User interaction (output)** | Dashboard: state, per-band vibration bars, cycle timeline, last event |
| SK6805 19-LED matrix | Verified (Lab 2 side quest) | **User interaction (output)** | State color + attention flash on DONE (visible across a room) |
| KEY0 / KEY1 buttons | Verified (Lab 3, PC0/PC1) | **User interaction (input)** | KEY0: acknowledge/clear alert · KEY1: enter/exit service mode |
| microSD (FatFs) | Verified (Lab 2/3) | **Logging** | Timestamped `cycle_log.csv` events + training CSV capture |
| RW007 Wi-Fi | Fitted | **Diagnostic/service** | Status reported in diagnostics; live telemetry is a documented stretch goal, never required for product decisions |
| ES8388 codec | Fitted | **Diagnostic/service** | Listed in diagnostics; audio beep on DONE is a stretch goal |
| External NOR flash (W25Q64) | Fitted | **Diagnostic/service** | Status reported in diagnostics; reserved for future log back-fill |
| RTC | Fitted | **Logging** | Timestamps (fallback: Zephyr uptime when not set) |
| IR / CAN / RS-485 | Not used by product | **Diagnostic** | Documented as not applicable (no fitted peripheral for them in this kit) |

### 1.6 Acceptance criteria

1. **Detection**: on a real washer, ≥ 90 % of 1-second windows during a full cycle
   are classified into the correct state by the trained model (held-out machine data),
   and every real `SPIN → IDLE` transition produces exactly one DONE alert.
2. **Latency**: median end-to-end state-change latency (vibration onset → LCD/LED
   update) ≤ 2.5 s; measured over ≥ 50 labeled transitions in the bench log.
3. **Inference cost**: report median and 95th-percentile inference time over ≥ 50
   on-board executions (service-mode bench), plus Flash/RAM/model footprint.
4. **Baseline comparison**: the rule-based baseline and the random forest are both
   evaluated on the same held-out windows; the report states the confusion matrices
   and argues whether the ML model earns its keep.
5. **Robustness**: machine unplugged mid-cycle → no hang, state falls back to IDLE,
   event logged; SD card removed → logging degrades to RAM ring + serial, product
   decisions unaffected; sensor I2C failure → LCD banner + diagnostics entry.
6. **Safety/privacy**: the product only senses vibration, temperature, and light of
   the user's own appliance; no camera, no microphone, no cloud dependency; it makes
   no safety-critical claims (it is an informational monitor, not a shut-off device).

---

## 2. Architecture (Zephyr threads — the FreeRTOS "task table" equivalent)

| Thread | Priority | Stack | Period | IPC in | IPC out |
|--------|----------|-------|--------|--------|---------|
| `sampler` | 6 | 2048 | 10 ms (absolute deadline) | — | SPSC sample ring |
| `inference` | 10 | 4096 | 1 s | sample ring | `state_msgq`, logger calls |
| `ui` | 12 | 2048 | 250 ms | `state_msgq`, context snapshot | LCD + LED matrix |
| `service` | 14 | 2048 | 50 ms poll | buttons | mode changes, CSV stream, bench results |

Synchronization: lock-free SPSC ring for samples; `k_msgq` for decisions; `k_mutex`
around SD/FatFs; `k_event` for alert-acknowledge; buttons polled and debounced in
`service`. All heavy math uses single-precision FPU (`CONFIG_FPU=y`).

## 3. Machine learning pipeline

- **Capture**: service-mode **COLLECT** streams labeled 100 Hz CSV (`ax..gz,label`)
  over the console while the user runs real appliance phases; `KEY1` cycles the
  active label, each row carries it (session-based labeling).
- **Features** (14, per 200-sample window): accel-magnitude RMS, peak-to-peak,
  crest factor, per-axis std ×3, zero-crossing rate, Goertzel band energies
  (low 1–3 Hz, mid 3–10 Hz, high 10–50 Hz), dominant frequency (Goertzel argmax
  1–50 Hz), gyro RMS, gyro peak-to-peak.
- **Model**: scikit-learn `RandomForestClassifier` (10 trees, depth 8) exported
  with **emlearn** to pure C (`ml_model/cycle_model_eml.h`) — same proven
  deployment path as Lab 3. No dynamic allocation on device.
- **Baseline**: fixed rules on RMS + dominant frequency (`cycle_logic.c`), for the
  required ML-vs-baseline comparison.
- **Bootstrap**: until real data exists, `tools`-generated **synthetic** windows
  (documented as such) let the full chain compile and run; every metric in the
  report must come from real held-out appliance data, never the synthetic set.

## 4. Repository map

```
LAB_4_Appliance_Monitor/
├── docs/design.md           ← this document
├── ml_model/train_cycle_model.py  (train + emlearn export + metrics)
├── ml_model/cycle_model_eml.h     (generated C forest — committed)
├── src/  (firmware: drivers, features, logic, UI, threads)
├── test/test_cycle/         (host-native Unity tests for pure logic)
└── zephyr/                  (prj.conf, CMakeLists, board overlay)
```

## 5. Test plan (summary)

1. Host-native Unity suite: feature extractor on synthetic vectors (known RMS/dominant
   freq), baseline rules at boundaries, hysteresis + DONE derivation, model wrapper
   sanity, CSV formatting.
2. Bench (on board): service-mode BENCH runs the model on ≥ 50 recorded windows,
   printing per-inference µs; REPORT phase collects the numbers for the lab report.
3. soak: 15-minute continuous run; offline by construction (no network in the
   decision path); failure cases: SD removed, IMU disconnected, button held.

## 6. Privacy, safety, accessibility

- No microphone, camera, or cloud: only the user's own appliance vibration and room
  light/temperature leave the board as anonymous numbers on an SD card.
- Accessibility: visual DONE alert (LED matrix flash + LCD banner) is designed to be
  noticeable for deaf users; acknowledge button has no time pressure.
- Safety: explicitly an informational prototype; it does **not** control or switch off
  the appliance and must not be presented as a safety device.

## 7. Roadmap (phases)

- [x] Phase 1: scaffold, design, proven driver ports (LCD, SD, LED, I2C/IMU)
- [x] Phase 2: feature extractor + baseline + state machine + host tests
- [x] Phase 3: synthetic model bootstrap + emlearn deployment
- [x] Phase 4: firmware integration (threads, UI, logging, service modes)
- [ ] Phase 5: real appliance data collection, retrain, held-out metrics
- [ ] Phase 6: on-board bench (50+ inferences), 15-min soak, lab report + demo

## 8. RTOS deviation: ZephyrRTOS vs STM32Cube/FreeRTOS

The activity sheet prescribes STM32Cube HAL/LL + FreeRTOS. This submission uses
**Zephyr RTOS** (`framework = zephyr`) for these reasons, documented for the grader:

1. **Verified prior art on this exact board**: the student's Lab 3 (graded) and the
   Lab 2 side quest run production-quality Zephyr firmware on this same RT-Spark
   board with this same PlatformIO setup (`platform = ststm32`, `board =
   black_f407zg`, ST-LINK upload). Every hardware claim in this project (I2C2 pins,
   LCD bring-up, FatFs mount, LED timing) is backed by already-demonstrated
   firmware, reducing bring-up risk to near zero.
2. **Same engineering discipline, different kernel**: the lab's substantive
   criteria — pre-emptive multi-threading, queues/mutexes/events, deterministic
   `vTaskDelayUntil`-equivalent sampling (`k_sleep` on absolute deadlines),
   per-thread stack accounting, fault handling — are all delivered by Zephyr's
   native APIs. The thread table in §2 is the direct analog of the requested
   FreeRTOS task table.
3. **Consistency across the student's laboratory series** (Labs 2 and 3 are both
   Zephyr-based), letting the lab report compare architectures honestly.
4. Instructor sign-off is assumed per course policy on documented deviations;
   this section exists precisely to make the deviation visible and defensible.
