# Laundry Sentinel — User Guide

RT-Spark (STM32F407) · Zephyr RTOS · Laboratory Activity 4

## 1. What it does

Place the board on top of your twin-tub washer. It senses the machine's
vibration 100 times per second, runs a trained random-forest model **on the
board**, and shows which tub is running:

| Display | Meaning | LED 8 |
|---|---|---|
| **IDLE** | machine quiet / off | dim blue |
| **WASH** | wash tub agitator running | cyan |
| **SPIN** | spin-dry tub running | orange |
| **DONE** | a tub stopped after ≥ 8 s | green + LED 16 flashing |

When a tub that ran at least 8 seconds goes silent, the strip under the title
blinks red: **WASH DONE! MOVE CLOTHES** (time to move the load to the spin
tub) or **SPIN DONE! HANG DRY**. The blink continues until you press **KEY0**
or 5 minutes pass. Every event is written to the SD card in `cycle_log.csv`
(columns: `uptime_ms,from_state,to_state,alert`; states 0=IDLE 1=WASH 2=SPIN
3=DONE).

## 2. Controls (two buttons)

- **KEY0** (short press) — acknowledge/clear a DONE alert; in service modes:
  page / label toggle.
- **KEY1** (short press) — move to the next service mode.
- **KEY1** (hold ~1.5 s) — enter the service menu from RUN; from COLLECT, jump
  to BENCH.

## 3. Mode map

```
RUN ──(KEY1 hold)──► DIAG ──(KEY1)──► COLLECT ──(KEY1 hold)──► BENCH ──(KEY1)──► RUN
 │                    │                  │
 KEY0 = ack alert     KEY0 = page        KEY0 = cycle label
                       (sensors / log)   (IDLE → WASH → SPIN)
```

- **DIAG** page 1: sensor status, model info, state-change count. Page 2 (KEY0):
  the newest logged events.
- **COLLECT**: streams raw 100 Hz training CSV on the USB console.
- **BENCH**: times 50 live inferences and shows median / 95th-percentile in µs.

## 4. Reading the RUN dashboard

- **Big word** — current state (large 3× font).
- **VIBRATION bar + `0.123g 12Hz`** — vibration strength and dominant
  frequency (wash ≈ low Hz, spin ≈ 10–35 Hz).
- **`ML:… RULE:… OK`** — the trained model vs. the rule baseline; `!!` means
  they disagree this window (interesting, not fatal).
- **`UP mm:ss T:..C H:..%`** — time in the current state; temperature/humidity
  shows `sensor absent` when the AHT10 isn't fitted (normal on this kit).
- **LED matrix**: 0–7 = vibration level, 8 = state color, 16 = alert flash
  (cyan = wash done, orange = spin done), 17 = hot room, 18 = dark room.

## 5. Collecting real training data (Phase 5) — 15 minutes

The board ships with a **bootstrap model** trained on synthetic vibration. For
the real lab numbers you capture your own machine once and retrain.

1. Connect the ST-LINK USB; open a terminal on **COM7 @ 115200**
   (`pio device monitor -e black_f407zg`, or PuTTY) **with logging to file**.
2. KEY1 hold → DIAG → KEY1 → **COLLECT**. A `#` header line is printed.
3. With the machine **off**, leave label **IDLE** for ~2 minutes.
4. Start the **wash tub only**, KEY0 until the big word says **WASH** (cyan),
   capture 2–3 minutes.
5. Stop wash, start the **spin tub only**, KEY0 to **SPIN**, capture 2–3 min.
   (Bumping the machine mid-capture is fine — label per what the tub is doing;
   mixed-label windows are dropped automatically.) One longer capture with
   labels switched on time, or separate files per phase, both work.
6. Stop logging. Save captures as `capture_*.csv` (lines starting with `#` or
   `[` are ignored by the trainer).
7. On the PC (one-time: `pip install scikit-learn emlearn numpy`):
   ```
   cd D:\BCA182_Lab\LAB_4_Appliance_Monitor
   python ml_model\train_cycle_model.py --real capture_idle.csv capture_wash.csv capture_spin.csv
   ```
   This retrains the forest, rewrites `ml_model/cycle_model_eml.h`, and prints
   the held-out confusion matrices (also saved to `ml_model/model_report.txt`).
8. Reflash: `pio run -e black_f407zg -t upload`. The monitor now runs your
   machine's real model.

Tips: keep the board in the **same spot** for capture and daily use; avoid
capturing while both tubs run; do two sessions per phase if you can and hold
one session out of training to report clean generalization numbers.

## 6. Running the inference benchmark (Phase 6)

KEY1 hold → DIAG → KEY1 → COLLECT → KEY1 hold → **BENCH**. It runs
automatically (~15 s), then shows `median` and `p95` in µs on the LCD and
prints `[BENCH] 50 runs: median … p95 …` on the console. KEY1 returns to RUN.

## 7. Getting the logs

Pull the SD card: `cycle_log.csv` has every transition since the card was
first used. With no SD card the last 32 events stay in RAM (visible on DIAG
page 2) and the console still prints `[LOG] …` lines.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| Boot prints `primary vibration sensor absent` | Re-seat power; the product needs the onboard ICM-20608 (it is soldered on — a re-flash usually clears it). |
| No CSV in COLLECT | Terminal must be COM7 @ 115200; check the ST-LINK VCP driver. |
| `sensor absent` under T/RH | Normal — the AHT10 is not fitted on this kit. |
| States flicker between WASH/SPIN | Retrain on real captures (Section 5); the synthetic bootstrap only approximates your machine. |
| Alert never appears | The tub must run ≥ 8 s and then go fully quiet for ~2 s; check the vibration bar moves while the tub runs. |

## 9. Safety

Informational monitor only: it observes vibration — it never switches the
appliance. Keep it dry and away from water; it is not a safety interlock.
