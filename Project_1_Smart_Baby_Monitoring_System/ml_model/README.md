# ml_model - P3 Baby-Cry ML Pipeline (PC side)

Two-class **cry / non-cry** audio classifier for the Smart Baby Monitoring
System, trained on this PC and exported as **pure C** (emlearn inline forest)
to `../include/cry_model.h` for on-device inference. Implements the frozen
evaluation protocol in `docs/project-plan.md` §P3.

All scripts are deterministic (fixed seeds), use only NumPy + stdlib +
scikit-learn + emlearn, and live entirely inside `ml_model/`.

---

## Pipeline

```
dataset_prep.py          extract_features.py            train_cry_model.py
-----------------        -----------------------        --------------------------
git clone --depth 1      stdlib wave -> float mono      manifest -> windows (cached)
donateacry + ESC-50      windowed-sinc resample 16kHz   RandomForest 10x depth 6
ffmpeg .3gp/.caf->WAV    512/256 Hann frames            class_weight=balanced
manifest.csv             40-mel -> log -> DCT-II        frozen-test metrics
(path,label,split,       13 MFCCs -> 29 feats/window    confuser FP table
 fold,source_id)         (pure NumPy, CMSIS mirror)     energy-baseline row
                                                        -> include/cry_model.h
```

## Run commands (in order)

Using the PlatformIO Python on this PC:

```bash
PY="C:/Users/Perch/.platformio/penv/Scripts/python.exe"

# 0. one-time deps (already done on this PC: numpy 2.4.6, scikit-learn 1.9.1, emlearn 0.23.2)
"$PY" -m pip install numpy scikit-learn emlearn

# 1. sanity-check the hand-rolled feature extractor (shapes/determinism/resampler)
"$PY" ml_model/extract_features.py --selftest

# 2. clone datasets, convert audio, build manifest.csv  (see caveats below)
"$PY" ml_model/dataset_prep.py

# 3. train, evaluate, export C header + report
"$PY" ml_model/train_cry_model.py \
    --dataset-dir ml_model \
    --out ../include/cry_model.h \
    --report model_report.txt
```

Notes:
- Step 2 needs `git` and `ffmpeg` on PATH. Each dataset clone is capped at
  ~6 minutes; a failed/slow clone prints a WARNING and the pipeline still
  runs on whatever is present.
- Step 3 caches window features in `ml_model/features_cache.npz`
  (keyed by path + mtime + size + extractor version), so re-runs are fast.
- `include/cry_model.h` is written **only** when the frozen test set holds
  >= 20 cry AND >= 20 non-cry clips; otherwise `model_report.txt` says
  exactly what is missing.

## Frozen evaluation protocol (docs/project-plan.md §P3)

| split      | contents                                                                                     | role |
|------------|----------------------------------------------------------------------------------------------|------|
| `train`    | donateacry-corpus (all clips, label=cry, split by **source recording**) + ESC-50 folds 1-3 non-cry (all classes except `crying_baby`) | fit |
| `val`      | ESC-50 fold 4 non-cry + donateacry source recordings held out of train                        | sanity check |
| `test`     | **FROZEN**: ALL 40 ESC-50 `crying_baby` clips (any fold - zero appear in training, leakage structurally impossible by fold design) + deterministic 40-clip fold-5 non-cry subset (seed 182) | reported metrics only |
| `confuser` | ESC-50 `siren`, `helicopter`, `chainsaw`, `rooster` (all folds) + UrbanSound8K `children_playing`/`street_music` **if manually placed** at `data/UrbanSound8K` | false-positive table only |

- ESC-50's own `src_file` grouping is preserved, and its fixed 5-fold design
  guarantees each source recording lives in exactly one fold.
- donateacry chunked exports of one recording share a base name; the
  `source_id` heuristic in `dataset_prep.py` groups them so no source
  recording straddles train/val.
- The trainer also reports an honest **rule-based baseline** (single
  log-energy threshold tuned on train) next to the forest.

## Feature extractor (extract_features.py)

Pure NumPy mirror of the planned CMSIS-DSP C front-end, every constant
documented for a later PC<->C parity unit test:

1. stdlib `wave` loader (PCM 8/16/24/32-bit, multi-channel averaged);
   `.3gp`/`.caf`/... raise `UnsupportedAudio` - convert with ffmpeg first.
2. Blackman-windowed-sinc bandlimited resampler to 16 kHz mono (33-tap
   kernel, per-phase unity-DC normalization; no scipy, no `np.interp`).
3. 512-sample frames, hop 256, periodic Hann window.
4. rfft -> power/512 -> 40 triangular mel filters (HTK scale, 0-8 kHz,
   hand-built matrix) -> ln (floor 1e-10) -> orthonormal DCT-II -> 13 MFCCs.
5. Per 1 s (16000-sample) window, 61 frames: MFCC mean+std (26) + frame
   log-energy mean/std (2) + mean frame ZCR (1) = **29 features**
   (`FEATURE_NAMES` order; `extract_window()` is the import-level API).

`--selftest` asserts filterbank/DCT properties, bit-exact determinism,
resampler correctness (44.1k->16k tone preservation, 8k->16k upsample) and
feature finiteness.

## Exported model header (include/cry_model.h)

Generated only by the trainer (never hand-edited):

- provenance comment (date, dataset versions/shas, sklearn/emlearn versions,
  full feature list, frozen-test summary),
- `extern "C"` guard + include guard,
- emlearn inline forest made header-safe (`static inline`, unused-parameter
  silenced) - zero gcc `-Wall -Wextra` warnings,
- `cry_model_predict(const float *features)` -> 0/1,
- `cry_model_predict_confidence(const float *features, uint8_t *out_conf)`
  tree-vote confidence wrapper (Lab 3 pattern; a 10-vote tie -> NONCRY,
  mirroring sklearn argmax).

Verified: the generated header was compiled standalone (`gcc -std=c99 -Wall
-Wextra`, zero warnings) and its predictions + confidences match sklearn
exactly on test vectors.

## Datasets & licensing

| dataset | url | license | usage here |
|---------|-----|---------|------------|
| donateacry-corpus | github.com/gveres/donateacry-corpus | **ODbL 1.0** | cry training (all clips) |
| ESC-50 | github.com/karolpiczak/ESC-50 | **CC BY-NC 3.0** | non-cry train folds 1-3, val fold 4, frozen test (40 crying_baby + 40 fold-5), confusers |
| UrbanSound8K (optional) | urbansounddataset.weebly.com | CC BY-NC 3.0 | confusers only, never auto-downloaded |

Both primary datasets are **non-commercial / research-education** licensed;
this matches the academic course context. Clones live under
`ml_model/data/` (git-ignored; re-creatable with step 2). Cloned commits are
recorded in `ml_model/dataset_provenance.txt` at prep time.

## Current status

- Environment: numpy 2.4.6 / scikit-learn 1.9.1 / emlearn 0.23.2 installed
  into the PlatformIO venv; git + ffmpeg 9.0.2 available.
- `extract_features.py --selftest`: **ALL PASS** (13/13 checks).
- `dataset_prep.py`: **both datasets downloaded** - ESC-50 (1.53 GB) and
  donateacry-corpus (0.17 GB, 1585 clips ffmpeg-converted to 16 kHz WAV,
  0 failures). `manifest.csv`: 3265 clips
  (train 1356 cry / 1080 noncry, val 229 cry / 360 noncry,
  frozen test 40 cry / 40 noncry, confusers 160). UrbanSound8K not present
  (optional; confuser table covers ESC-50 confusers only).
- **Full training run completed** (14,097 train windows; 4 clips too short
  for a 1 s window were skipped). Key numbers - full detail in
  `model_report.txt`:
  - VALIDATION (donateacry holdout + ESC-50 fold-4 noncry): accuracy 0.926,
    cry F1 0.919 - the model fits its training domain well.
  - FROZEN TEST (40 ESC-50 crying_baby + 40 fold-5 non-cry clips, 400
    windows): accuracy 0.632; cry precision 0.921 / **recall 0.290 / F1 0.441**;
    noncry precision 0.579 / recall 0.975 / F1 0.726.
  - Confuser false-positive rate (forest): 0-5 % of windows across
    siren/helicopter/chainsaw/rooster, vs **83-95 %** for the energy
    baseline - the MFCC forest is dramatically more selective.
  - Rule-based baseline on the frozen test: accuracy 0.542 (near chance).
- Honest interpretation: the plan's F1 >= 0.85 target is **not met** by this
  frozen configuration. The dominant error mode is cry recall on ESC-50
  `crying_baby` clips (0.29) despite 0.94 cry recall on held-out
  donateacry - a **domain shift** between phone-app cry recordings and
  Freesound ambient clips, exactly what the frozen unseen test is designed
  to expose. Candidate next steps (none change the frozen protocol):
  wider/deeper forest or the plan's MLP 1x32 `eml_net` fallback, domain
  augmentation (room reverb/noise on donateacry), or fusing the donor
  corpus with distant-mic cry data.
- Export: `include/cry_model.h` written (101,664 bytes, 10 trees x
  depth <= 6, 29 features, provenance + extern "C" + both wrappers).
  Verified standalone: `gcc -std=c99 -Wall -Wextra` **zero warnings**, and
  `cry_model_predict` / `cry_model_predict_confidence` outputs match
  sklearn (class + tree-vote confidence) exactly on frozen-test vectors.
- Remaining before P3 exit: CMSIS-DSP C port of the front-end +
  PC<->C parity unit test (this NumPy extractor is the reference), RAM/
  timing table, Unity tests for the aggregator, 2-of-3 window debounce on
  device.

## File inventory

| file | purpose |
|------|---------|
| `dataset_prep.py` | dataset download/conversion/manifest builder (this file's step 2) |
| `extract_features.py` | pure-NumPy MFCC front-end + `--selftest` |
| `train_cry_model.py` | trainer/evaluator/emlearn exporter (step 3) |
| `manifest.csv` | frozen-protocol split manifest (generated) |
| `dataset_provenance.txt` | clone shas, licenses, seed, warnings (generated) |
| `features_cache.npz` | per-clip window feature cache (generated) |
| `model_report.txt` | training report (generated) |
| `data/` | dataset clones + converted WAVs (generated, git-ignored) |
