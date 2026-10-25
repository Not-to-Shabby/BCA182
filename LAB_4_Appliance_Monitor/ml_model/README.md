# Lab 4 ML Model — Appliance Cycle Classifier

## Files

- `train_cycle_model.py` — trainer: feature extraction (mirrors the C code exactly),
  RandomForest (10 trees, depth 8), emlearn inline-C export, held-out metrics,
  rule-based baseline comparison.
- `cycle_model_eml.h` — **generated** C forest compiled into the firmware
  (committed so the firmware always builds).
- `model_report.txt` — latest training metrics (regenerated on every run).

## Bootstrap vs. real data

The committed model was trained on **synthetic** appliance windows so the full
chain (firmware + tests) runs before real data exists. Synthetic metrics are
NOT product accuracy. To train the real model:

1. Put the board on the appliance, enter **COLLECT** mode (`KEY1` long → `KEY0`
   cycles label IDLE/WASH/SPIN), and capture the console stream at 115200 baud
   while the machine runs each phase. Save e.g. `capture_wash.csv`
   (the stream starts with a `#` header comment; raw ICM-20608 counts are
   converted to g / rad/s by the trainer).
2. Retrain:
   ```
   python ml_model/train_cycle_model.py --real capture_idle.csv capture_wash.csv capture_spin.csv
   ```
3. Rebuild and flash; `cycle_model_eml.h` now carries the real forest. The
   held-out confusion matrix and per-class precision/recall land in
   `model_report.txt` for the lab report.

## Held-out split policy

`train_test_split(stratify, test_size=0.2, random_state=42)` on *windows*.
When training on real captures from multiple sessions, pass captures from at
least two independent sessions and hold out one session entirely (edit
`load_real` to filter by filename) to honor the lab's leakage rule
(no near-duplicate windows on both sides of the split).
