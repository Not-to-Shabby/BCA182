# Laboratory Activity 4 — Laundry Sentinel
## Edge AI Appliance Cycle Monitor on RT-Thread RT-Spark (STM32F407ZGT6, Zephyr RTOS)

**Laundry Sentinel** is a consumer appliance monitor for a **twin-tub
semiautomatic washer** (separate manually-timed wash and spin-dry tubs): placed
on the machine's shared chassis, it senses vibration with the onboard ICM-20608
IMU, classifies the active tub **on the STM32** with a trained random-forest
model (10 trees, 14 vibration features, emlearn-generated C), and reports
**IDLE / WASH / SPIN** on the ST7789 LCD and the 19-LED matrix. When a tub that
ran ≥ 8 s falls silent, it raises a per-tub **DONE** alert — **WASH DONE**
("move clothes to the spin tub") or **SPIN DONE** ("hang the laundry") — on the
LCD banner and a tinted LED flash until acknowledged with KEY0. Every
transition is timestamped to the microSD as `cycle_log.csv`, so it also answers
"how long did each tub actually run?".

> **RTOS note:** this lab intentionally uses **Zephyr RTOS** instead of the
> sheet's default STM32Cube + FreeRTOS. The deviation and its rationale are
> documented in [docs/design.md §8](docs/design.md); the thread table there is
> the direct analog of the requested FreeRTOS task table.

## Product behavior

| State | Meaning | Feedback |
|-------|---------|----------|
| IDLE | both tubs quiet / off | dim blue LED, LCD dashboard |
| WASH | wash tub agitator running | cyan LED, low-band vibration gauge |
| SPIN | spin-dry tub running (~1200–2100 RPM) | orange LED, high-band + dominant-freq gauge |
| WASH DONE | wash tub stopped after ≥ 8 s | red blinking banner "WASH DONE! Move" + cyan LED 16 flash until KEY0 |
| SPIN DONE | spin tub stopped after ≥ 8 s | red blinking banner "SPIN DONE! Hang" + orange LED 16 flash until KEY0 |

Twin-tub note: both tubs share one chassis, so one board senses both. If both
tubs run simultaneously the classifier reports the dominant one (normally
spin); alerts key off sustained-quiet periods, so no tub-stop event is lost.

Context sensing: AHT10 temperature/humidity and AP3216C ambient light (both
probed at boot, graceful ABSENT) feed night mode (dark room dims the LED
alert) and are logged with events.

## Service modes (KEY1 long-press to enter)

| Mode | KEY0 | KEY1 |
|------|------|------|
| RUN | acknowledge DONE alert | (long) → DIAG |
| DIAG | toggle page (sensors / event log) | → COLLECT |
| COLLECT | cycle training label (IDLE→WASH→SPIN) | (long) → BENCH |
| BENCH | — | → RUN (results stay on screen) |

COLLECT streams raw 100 Hz CSV over the console for retraining;
BENCH times 50 live inferences and reports median/p95 µs — the evidence for
the lab's on-device inference requirement.

## Repository layout

```
├── docs/design.md        problem, alternatives, states, block diagram,
│                         component-function matrix, acceptance criteria
├── ml_model/             trainer + generated emlearn forest + metrics
├── src/                  firmware (drivers, features, logic, UI, threads)
├── test/test_cycle/      13 host-native Unity tests of the decision chain
└── zephyr/               prj.conf, CMakeLists.txt, board overlay (250 MHz-capable)
```

## Firmware architecture (Zephyr threads)

| Thread | Prio | Stack | Period | Job |
|--------|------|-------|--------|-----|
| sampler | 6 | 2048 | 10 ms (absolute deadline) | ICM-20608 @ 100 Hz → SPSC ring (+ raw CSV in COLLECT) |
| inference | 10 | 6144 | 1 s | window → 14 features → forest + baseline → hysteresis → events → SD log |
| ui | 12 | 4096 | 250 ms | LCD dashboard + LED matrix state/alert rendering |
| service | 14 | 4096 | 50 ms | debounced buttons, DIAG/COLLECT/BENCH |

Shared bit-banged I2C (PF1/PF0) is mutex-arbitrated; the sample ring is
spinlock-guarded with copy-out-under-lock so the sampler's 10 ms deadline
survives feature math.

## Verification status (Phase 4 complete)

- `pio test -e native` — **13/13 passing** (features on known sine windows,
  baseline boundaries, forest signatures, hysteresis, DONE derivation,
  short-spin guard, DONE timeout, service-mode transitions).
- `pio check` (cppcheck) — **0 defects** (0 HIGH / 0 MEDIUM / 0 LOW).
- `pio run -e black_f407zg` — 0 warnings; Flash **9.2 %** (96.9 KB / 1 MB),
  main RAM **41.8 %** (54.8 KB / 128 KB).

## Build & flash

```
pio run -e black_f407zg          # build
pio run -e black_f407zg -t upload  # ST-LINK
pio device monitor -e black_f407zg # console @ 115200 (COM7)
pio test -e native               # host unit tests
```

## Remaining phases

- **Phase 5**: collect real twin-tub captures (COLLECT mode; label IDLE with the
  machine off, WASH with the wash tub only, SPIN with the spin tub only; ≥ 2
  sessions each), retrain (`ml_model/train_cycle_model.py --real ...`), report
  held-out metrics and the forest-vs-baseline comparison.
- **Phase 6**: on-board bench numbers (50+ inferences), 15-minute soak,
  failure-mode tests (SD absent, IMU disconnected), formal lab report and demo.
