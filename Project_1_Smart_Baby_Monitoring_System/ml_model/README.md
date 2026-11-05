# ml_model - P3 Baby-Cry ML Pipeline (PC side)

Two-class **cry / non-cry** audio classifier for the Smart Baby Monitoring
System, trained on this PC and exported as **pure C** (emlearn inline forest)
to `../include/cry_model.h` for on-device inference. Implements the frozen
evaluation protocol in `docs/project-plan.md` §P3.

All scripts are deterministic (fixed seeds), use only NumPy + stdlib + SciPy
(train-time augmentation only) + scikit-learn + emlearn, and live entirely
inside `ml_model/`.

---

## Pipeline

```
dataset_prep.py          acquire_v2.py + prep_v2.py     extract_features.py
-----------------        ------------------------------ -----------------------
git clone --depth 1      acquire_v2.py <name>: convert  stdlib wave -> float mono
donateacry + ESC-50      new corpora -> 16 kHz wav16 +  windowed-sinc resample 16kHz
ffmpeg .3gp/.caf->WAV    manifest_<name>.csv (candidates) 512/256 Hann frames
manifest.csv             prep_v2.py: md5 content-hash   40-mel -> log -> DCT-II
(path,label,split,       dedup vs every existing split, 13 MFCCs -> 63 feats/window
 fold,source_id)         intra-candidate dedup, <1 s    (v1 29-summary block + 34
                         drop, 10% stratified val,      temporal-dynamics features:
                         feature-cache pre-warm ->      MFCC-delta mean/std, log-energy
                         manifest_v2.csv                delta mean/std, spectral
                                                        centroid/rolloff(0.85)/flatness
                                                        mean/std) (pure NumPy)

augment.py
-----------------------------------------------------------------------------------
v7 wave augmentation of TRAIN windows only, recipe split by domain:
close-mic donateacry cries -> FULL recipe (real ESC-50 ambience mix SNR
10-25 dB, reverb, lowpass 2.5-7 kHz, volume + loud branch, pitch/speed,
hum); wave-2 distant/room cries (SICCD/CSIBE/hoyyu_icd) -> MILD recipe
(synthetic noise + volume only - already domain-matched audio is not
double-degraded); non-cry windows -> noise + volume calibration variants
-> features_aug_cache.npz + stress-val deployment proxy for val cries

train_cry_model.py
-----------------------------------------------------------------------------------
manifest_v2.csv -> windows (cached) + augmented train windows (cached, seeded)
-> candidates (seed 42: rf16x7 / rf24x6 / rf30x8) with sample_weight x3 on the
   18 nearest loud-mechanical/animal ESC-50 train categories (NEAR_CATS -
   the confuser categories themselves are in NO training split by protocol)
-> VAL-only comparison: cry side = raw val cry (incl. real distant clips)
   + stress-val cry proxy, FP gate = confuser split (10%)
-> cry-vote-share threshold swept 40..90 (firmware rule replicated exactly)
-> FROZEN TEST evaluated once at the chosen point
-> emlearn inline forest -> include/cry_model.h (same API as v1)
```

## Run commands (in order)

Using the PlatformIO Python on this PC:

```bash
PY="C:/Users/Perch/.platformio/penv/Scripts/python.exe"

# 0. one-time deps (already done on this PC: numpy 2.4.6, scipy 1.17.1,
#    scikit-learn 1.9.1, emlearn 0.23.2)
"$PY" -m pip install numpy scipy scikit-learn emlearn

# 1. sanity-check the hand-rolled feature extractor + the augmenter
"$PY" ml_model/extract_features.py --selftest
"$PY" ml_model/augment.py --selftest

# 2. clone datasets, convert audio, build manifest.csv  (see caveats below)
"$PY" ml_model/dataset_prep.py

# 2b. wave-2 corpora (one-time, already run): convert + merge with content
#     dedup, build manifest_v2.csv, pre-warm the feature cache
# "$PY" ml_model/acquire_v2.py siccd   # csibe / hoyyu_icd likewise
"$PY" ml_model/prep_v2.py

# 3. train, evaluate, export C header + report (full protocol incl. the
#    one-shot frozen-test evaluation); wave-2 training reads manifest_v2.csv
"$PY" ml_model/train_cry_model.py \
    --manifest manifest_v2.csv \
    --candidates rf16x7,rf24x6,rf30x8 \
    --aug-variants 1 --noncry-aug-frac 0.5 \
    --dataset-dir ml_model \
    --out ../include/cry_model.h \
    --report model_report.txt

# 3b. val-only development runs (candidate/threshold tuning without ever
#     touching the frozen test) - use while iterating on recipes:
# "$PY" ml_model/train_cry_model.py --skip-frozen-test
```

Notes:
- Step 2 needs `git` and `ffmpeg` on PATH. Each dataset clone is capped at
  ~6 minutes; a failed/slow clone prints a WARNING and the pipeline still
  runs on whatever is present.
- Step 3 caches window features in `ml_model/features_cache.npz` (keyed by
  cache version + path + mtime + size; version `v2` = feature-set v2's 63
  features), augmented train features in `ml_model/features_aug_cache.npz`
  (invalidated by feature width, rebuilt once for v2 - the waveforms replay
  identically because the per-(file, variant) seeds are unchanged), and the
  real-ambience noise pool in `ml_model/noise_pool_aug-v5.npz`, so re-runs
  are fast and reproducible.
- `include/cry_model.h` is written **only** when the frozen test set holds
  >= 20 cry AND >= 20 non-cry clips; otherwise `model_report.txt` says
  exactly what is missing. The exported header is also size-gated
  (`--header-max-bytes`, default 1.5 MB as of wave 2): emlearn inline trees
  are tiny in flash (the v6 332 KB header compiled to ~15 KB; the v7
  1.276 MB rf30x8 header compiles to ~171 KB, firmware total 338 KB = 32.2%
  of the 1 MB STM32F407 flash), so the v6 400 KB source budget was raised.

## Frozen evaluation protocol (docs/project-plan.md §P3) - unchanged

| split      | contents                                                                                     | role |
|------------|----------------------------------------------------------------------------------------------|------|
| `train`    | donateacry-corpus (all clips, label=cry, split by **source recording**) + ESC-50 folds 1-3 non-cry (all classes except `crying_baby`) + **wave 2**: SICCD + CSIBE + hoyyu_icd candidates (hash-deduped, 32,085 clips) | fit (+ **wave augmentation of train windows only**) |
| `val`      | ESC-50 fold 4 non-cry + donateacry source recordings held out of train + **wave 2**: 10% stratified hash-deduped candidate sample (3,565 clips) | candidate + threshold tuning (plus a label-preserving stress proxy of the val cry clips) |
| `test`     | **FROZEN**: ALL 40 ESC-50 `crying_baby` clips (any fold - zero appear in training, leakage structurally impossible by fold design) + deterministic 40-clip fold-5 non-cry subset (seed 182) | reported metrics only, **evaluated once** at the chosen operating point |
| `confuser` | ESC-50 `siren`, `helicopter`, `chainsaw`, `rooster` (all folds) + UrbanSound8K `children_playing`/`street_music` **if manually placed** | false-positive table + the task's 10% FP gate |

- No `crying_baby` clip is ever trained on; val/test/confuser audio never
  enters training data or the noise pool (pool = train non-cry clips only).
- The threshold sweep replicates the firmware rule exactly
  (`src/cry_task.c`): cry-positive iff the forest majority votes CRY AND
  `(cry_votes * 100) // n_trees >= CRY_CONF_THRESHOLD`.
- The v5 trainer also reports an honest **rule-based baseline** (single
  log-energy threshold tuned on the original train split) next to the forest.

## v2/v3 domain-gap work (why recall moved)

The v1 forest recalled only **0.29** of frozen-test cry windows: donateacry
is close-mic phone-app audio while the frozen test is ambient room
recordings (test-cry ZCR 0.185 vs train-cry 0.138; mfcc00_mean -56.9 vs
-76.7 - the test cries are LOUD and bright, the training cries quiet and
dull). Diagnostics showed the forest keys on spectral-shape MFCCs
(mfcc05/07/02/09) and, with naive synthetic-noise augmentation, learns the
shortcut "flat white/pink noise = cry" while real ambience stays noncry.

The v5 recipe (all train-split audio only) closes part of that gap:

1. **Real-ambience mixing** (p=0.5): each augmented cry window is mixed with
   a REAL ESC-50 fold 1-3 non-cry window at SNR 10-25 dB - cry stays
   dominant (v3/v4 showed noise-buried cries at SNR 0-14 dB blur the
   boundary and inflate ambient FP), creating contrastive pairs
   (cry+ambience -> cry vs the same ambience alone -> noncry).
2. **Room reverb** (p=0.7): 2-3 decaying echo taps, 25-110 ms.
3. **Distant-speaker lowpass** (p=0.6): prewarped Tustin first-order,
   2.5-7 kHz.
4. **Volume** -6..+14 dB with a +10..+20 dB loud branch (p=0.3) **mirrored
   on non-cry windows** so loudness alone is never a cry cue (ESC-50 cries
   are loud).
5. **Pitch/speed** +/-10 % (linear-interp resample trick, per file),
   **50 Hz hum** (-32..-22 dB rel) at low level.
6. ALL training non-cry windows also get synthetic-noise + volume variants.

Two augmented variants per cry clip (train set 14,097 -> 36,897 windows);
every (file, variant) carries a CRC32-derived RNG seed, so the caches are
order-independent and reproducible. `augment.py --selftest` verifies SNR
accuracy, 1/f tilt, reverb/lowpass/pitch behaviour, and clip guards.

**Threshold tuning lesson (v2, documented)**: tuning the vote threshold on
raw val cries alone drives it far too high - val cries are in-domain and the
forest is near-saturation-confident on them, so val picked T=75 which
collapsed frozen-test recall to 0.170 (vs 0.290 at majority). v5 therefore
tunes on a **stress-val proxy**: the deployment channel (reverb, real-noise
mix SNR 8-18 dB, lowpass p=0.5 for speaker-playback vs ambient-mic
channels) applied to the val cry windows - still 100 % val data, still
label-preserving. The FP gate stays on the confuser split (the task's
"confuser FP <= 10%" metric); the raw-val noncry FP rate is reported at
every sweep point (it runs higher than the confuser rate because fold-4
ambience covers all 40 ESC-50 classes).

## Exported model header (include/cry_model.h)

Generated only by the trainer (never hand-edited); public API and semantics
unchanged from v1:

- provenance comment (date, versions, dataset shas, feature list,
  augmentation recipe, val-tuned operating point, frozen-test summary),
- `extern "C"` guard + include guard,
- emlearn inline forest made header-safe (`static inline`, unused-parameter
  silenced) - zero gcc `-Wall -Wextra` warnings,
- `cry_model_predict(const float *features)` -> 0/1,
- `cry_model_predict_confidence(const float *features, uint8_t *out_conf)`
  tree-vote confidence wrapper (confidence = vote share of the PREDICTED
  class; a vote tie -> NONCRY, mirroring sklearn argmax).

Pair it with `CRY_CONF_THRESHOLD` in `include/cry_task.h` (currently
**73u** = 22/30 trees, the val-tuned operating point of the shipped v7
model).

Verified: the generated header was compiled standalone (`gcc -std=c99 -Wall
-Wextra`, zero warnings) and its predictions + confidences match sklearn
exactly on test vectors (retraining the forest from the caches reproduces
the shipped model bit-for-bit; `tree_votes` = emlearn leaf-majority votes).

## Datasets & licensing

| dataset | url | license | usage here |
|---------|-----|---------|------------|
| donateacry-corpus | github.com/gveres/donateacry-corpus | **ODbL 1.0** | cry training (all clips) |
| ESC-50 | github.com/karolpiczak/ESC-50 | **CC BY-NC 3.0** | non-cry train folds 1-3 (+ their ambience for cry mixing), val fold 4, frozen test (40 crying_baby + 40 fold-5), confusers |
| SICCD (Sakha & Ameri 2026) | zenodo.org/records/21848098 | **CC BY 4.0** | wave-2 cry training (3,200 clips, 4 cry-emotion classes) |
| CSIBE (Kertesz 2018) | zenodo.org/records/1243714 | **CC BY 4.0** | wave-2 cry training incl. 960 distant/reverb AIBO room cries + 13 domestic non-cry classes |
| hoyyu1/infant-cry-detection (ICD_MMSP, IEEE MMSP 2025) | huggingface.co/datasets/hoyyu1/infant-cry-detection | **none stated - research-use-only** | wave-2 cry training incl. 4,552 CryCeleb distant room cries + non-cry. **Training OK; raw audio NEVER redistributed** |
| UrbanSound8K (optional) | urbansounddataset.weebly.com | CC BY-NC 3.0 | confusers only, never auto-downloaded |

The base datasets are **non-commercial / research-education** licensed; the
CC BY 4.0 additions permit commercial use individually but the combined
corpus stays non-commercial via ESC-50/donateacry, matching the academic
course context. The hoyyu_icd research-use-only constraint is honored: it is
used for training only and is never redistributed (the trained model is a
statistical derivative; attribution recorded here and in the shipped header
provenance). Clones live under `ml_model/data/` (git-ignored). Cloned
commits are recorded in `ml_model/dataset_provenance.txt` at prep time.

## Current status (v7 retrain - wave-2 corpus merge, November 2026)

Wave 2 merged three newly acquired corpora into training (prep_v2.py ->
manifest_v2.csv) and retrained. **The merged distant/room cry data moved the
needle decisively on the frozen test:**

| metric (frozen test, evaluated once at each ship point) | v6 (rf16x7, T=75) | **v7 (rf30x8, T=73)** |
|---|---|---|
| cry precision / recall / F1 | 0.802 / **0.345** / 0.483 | 0.928 / **0.705** / **0.801** |
| noncry FP (fold-5 subset) | 17/200 = 8.5% | **11/200 = 5.5%** |
| accuracy | 0.630 | **0.825** |
| cry clips with >= 1 positive window | 29/40 | **38/40** |
| cry clips with >= 2 positive windows | 19/40 | **33/40** |
| noncry clips with >= 1 FP window | 8/40 | **5/40** |
| confuser FP (gate <= 10%) | 9.6% | **9.5%** |
| header size / firmware flash | 332 KB / 167 KB total | 1.276 MB / 338 KB total (32.2%) |

Frozen-test cry recall doubled (+0.36 absolute) and F1 rose +0.32 while the
confuser gate held and ambient FP fell - the diagnosis was right all along:
the model needed to HEAR distant/room cries (960 CSIBE AIBO + 4,552 CryCeleb
clips), not just have close-mic cries synthesized to look distant.

### Wave-2 merge (prep_v2.py): method and stats

- **Fingerprint**: md5 of the 16 kHz mono signed-16-bit PCM bytes (16 kHz
  mono/s16 files hashed verbatim from the WAV data chunk; other formats -
  ESC-50 is 44.1 kHz - routed through the pipeline's own loader + resampler
  and quantized to s16, so fingerprints are comparable across corpora).
  Verified: identical copies match; a +1e-4 DC-shifted copy hashes
  differently. **EXACT-content matching only**: re-encoded / gain- or
  pitch-shifted duplicates are NOT caught.
- **Routing policy** (mission contract): candidate hash == existing
  `test`/`confuser` clip -> DROP ENTIRELY (frozen test AND FP-gate
  protection); == existing `val` clip -> train (never val: tuning
  contamination); == existing `train` clip -> train; == another candidate ->
  keep one deterministic representative, drop the rest; < 1 s (no 1 s
  window) -> dropped.
- **Stats** (61,448 candidates scanned, 0 errors, 2.6 min on 12 workers):
  0 frozen-test/confuser collisions (ESC-50 protection confirmed),
  2,688 intra-candidate duplicates dropped (2,526 hoyyu_icd domestic
  repeats + 162 SICCD), 23,110 clips shorter than 1 s dropped (79% of CSIBE
  non-cry and 56% of CSIBE cry are sub-second AIBO event snippets),
  **0 exact-content overlaps with donateacry** (the SICCD overlap warning
  was precautionary - no byte-identical clips exist; re-encoded overlap
  cannot be excluded by design and none was detectable), 35,650 eligible ->
  3,565 val (10% stratified by corpus x label; CSIBE grouped by underlying
  source file via the `__<chunk>__` filename ordinal) + 32,085 train.
  Audit trail: `dropped_clips_v2.csv`; full log: `prep_v2_report.txt`.
- SICCD's ~330 internally-augmented clips carry no metadata marker and could
  not be identified or excluded individually (mild intra-train duplication
  risk, no evaluation leakage).
- Non-cry candidates were included in FULL (extraction measured ~11 ms/clip;
  the mission's 1:2..1:4 cry:noncry band is met by clip count 1:1.1 after
  the sub-second drop, ~1:0.85 by windows, and `class_weight=balanced`
  equalizes the effective prior).

### Wave-2 recipe (selected by val-only diagnostics, diag_v2.py E1..E23)

Naive full inclusion FAILED the task's confuser gate: the dev run measured
confuser FP 0.185 at T=75 (v6: 0.096) and no candidate passed below T=86.
Per-category breakdown showed siren + rooster FP. Root causes found by
controlled ablation (all val-only, frozen test untouched):

1. The confuser categories are in NO training split by protocol (dataset_prep
   routes them to split=confuser for ALL ESC-50 folds), so the gate tests
   unseen-category generalization - and the wave-2 cry corpora genuinely
   overlap that region (distant broadband bursts ~ rooster, melodic wails ~
   siren).
2. Applying the full close-mic-channel recipe (reverb/lowpass/real-noise) to
   already-distant wave-2 cries double-degrades them and smears the cry
   template toward the confuser region.

Fixes (the shipped "E20r30" recipe):
- **Mild augmentation routing** (augment.py `CLOSE_MIC_CRY_PREFIX`):
  donateacry cries keep the full recipe; SICCD/CSIBE/hoyyu_icd cries get the
  mild noise+volume recipe (1 variant per clip; non-cry calibration frac
  0.5).
- **NEAR_CATS sample weighting** (x3.0, ESC-50 sources only): the 18 nearest
  loud-mechanical/animal ESC-50 train categories (engine, train, fireworks,
  car_horn, church_bells, glass_breaking, hand_saw, vacuum_cleaner,
  washing_machine, crow, hen, sheep, pig, cow, thunderstorm, wind, sea_waves,
  rain) push the noncry boundary outward around the unseen confuser
  population. Confuser-split audio is never touched, so the gate still
  measures unseen-category generalization. A first implementation that
  matched ANY corpus's category names accidentally weighted CSIBE's own
  `vacuum_cleaner` class; shipped weighting is ESC-50-only (the exact
  composition validated in diagnostics; prod reproduces the diagnostic
  sweep bit-for-bit).
- Real-noise pool unchanged (ESC-50 folds 1-3 only) so the v5/v6 augmented
  cache stays valid.

Candidate sweep on the merged val (gate-passing bests): rf16x7@81% F1 0.787,
rf24x6@79% F1 0.777, **rf30x8@73% F1 0.830 (P 0.975 / R 0.722; raw val cry
recall 0.807, stress-val recall 0.637, val noncry FP 0.041, confuser FP
0.095)** -> ships. The 1.5 MB header budget admits rf30x8 (1,276,170 bytes,
30 trees x depth <= 8); compiled firmware flash 338,116 B = 32.2% of 1 MB,
RAM 95.0% (net_task/audio-buffer work tracked separately by the firmware
side).

### v6 (previous ship, for comparison)

- Environment: numpy 2.4.6 / scipy 1.17.1 / scikit-learn 1.9.1 /
  emlearn 0.23.2 in the PlatformIO venv; git + ffmpeg available.
- `extract_features.py --selftest`: **ALL PASS** (21 checks, including the
  v2 delta/spectral definitions); `augment.py --selftest`: **ALL PASS**.
- Feature-set v2: the 29 v1 summary features keep indices 0-28 and 34
  temporal-dynamics features are appended (63 total, under the 64 budget):
  per-frame MFCC deltas (edge-replicated first difference
  `d[t] = (m'[t+1]-m'[t-1])/2`, mean+std = 26), per-frame log-energy delta
  mean+std (2), per-frame spectral centroid / rolloff(0.85) / flatness
  mean+std (6). The on-device front end (`src/audio_features.c`) computes
  all 63 (pure C, FFT workspace + per-frame histories in DTCM) and
  `pio test -e native` proves PC<->C parity: **golden max |err| = 4.8e-07**
  (tolerance 1e-3), 7/7 feature tests + 22/22 baby-logic tests pass.
- Shipped model: **rf16x7** - RandomForestClassifier(n_estimators=16,
  max_depth=7, class_weight="balanced", random_state=42) trained on
  36,897 windows (14,097 original + 22,800 augmented, same augmentation
  waveforms as v5 replayed through the new front end). Header
  `include/cry_model.h`: **332,196 bytes** (16 trees x depth <= 7), inside
  the 400 KB budget.
- Candidate sweep on VAL (seed 42; cry side = raw + stress val cries, gate
  = confuser FP <= 10 %): rf12x7 0.844 (only gate-passing point T=83),
  rf16x7 **0.876** (T=75), rf18x7 0.871 (T=77), rf20x7 0.882 (T=75),
  rf30x8 **0.890** (T=73), et30x8 0.807, et12x7 0.676. The val winner
  rf30x8 measures **1.18 MB** and rf20x7 **415 KB** - both exceed the
  400 KB header budget, so the exporter shipped rf16x7 (330 KB), the
  best val candidate that fits.
- Val-tuned operating point: **cry vote share >= 75 %** (12/16 trees)
  - cry F1 on the tuning set (raw + stress val cries) **0.876**
    (P 0.939 / R 0.821; raw-val cry recall 0.928, stress-val cry recall
    0.713), raw-val noncry FP 0.086, **confuser FP 0.096** (gate 0.10).
- FROZEN TEST (400 windows = 40 crying_baby + 40 fold-5 non-cry clips,
  evaluated once at T=75 %):
  - cry: precision **0.802** / recall **0.345** / F1 **0.483**
  - noncry: precision 0.583 / recall 0.915 (FP 17/200 = **8.5 %**)
  - accuracy 0.630; energy baseline: near-chance (cry recall 196/200 but
    noncry FP 179/200).
  - Confuser FP table (800 windows): chainsaw 5.0 %, helicopter 0.5 %,
    rooster 16.5 %, siren 16.5 % - **TOTAL 77/800 = 9.6 %** (<= 10 % gate).
  - Clip-level view (informational, device needs 2-of-3 consecutive
    windows): 29/40 cry clips have >= 1 positive window, 19/40 have >= 2;
    8/40 noncry clips have >= 1 FP window, 4/40 have >= 2.
- Honest interpretation: the 0.50-recall target was **not reached**
  (0.345). Feature-set v2 delivered on its in-domain promise - the val
  winner improved (rf30x8 val F1 0.890 / stress recall 0.748 vs v5's best
  0.885 / 0.757) and the sweep produced a whole family of stronger
  candidates - but the frozen-test gain is marginal: cry recall
  **0.335 -> 0.345**, F1 0.480 -> 0.483, at a confuser FP that rose
  7.7 % -> 9.6 % (still inside the gate). Two structural reasons:
  (1) the header budget removed the top of the val ranking (rf30x8/rf20x7
  unshippable), and (2) temporal dynamics are not cry-specific - sirens
  and roosters are strongly modulated too, so the delta/spectral features
  narrow the gap between cries AND dynamic confusers (at the matched
  T=75 %, the v6 rf12x7's confuser FP is 0.104 vs v5's 0.077). An
  intermediate full-protocol run (same deterministic pipeline before the
  candidate grid was extended) also evaluated rf12x7@83 % on the frozen
  test: recall 0.270 / F1 0.404 / confuser FP 5.4 % - reported for
  transparency; its finer-granularity sibling rf16x7@75 % ships instead,
  chosen on VAL + header size only.
- The plan's MLP(32) fallback is **not executable on this PC**: emlearn's
  network exporter builds its evaluator with distutils and requires MSVC
  14, which is not installed (documented blocker, not silently dropped).
- Export: `include/cry_model.h` (332,196 bytes) + `CRY_CONF_THRESHOLD 75u`
  in `include/cry_task.h`; standalone gcc compile clean; sklearn parity
  verified exactly.

### v5 (previous ship, for comparison)

- rf12x7 on the 29-feature front end, T=75 % (9/12 trees): val cry F1
  0.885, frozen cry P 0.848 / R 0.335 / F1 0.480, noncry FP 6.0 %,
  confuser FP 7.7 % (62/800), clip-level 27/40 and 18/40; header
  263,746 bytes.

## File inventory

| file | purpose |
|------|---------|
| `dataset_prep.py` | dataset download/conversion/manifest builder (this file's step 2) |
| `acquire_v2.py` | Datasets v2 converter: data/<name> -> wav16/ + manifest_<name>.csv |
| `prep_v2.py` | wave-2 merger: md5 content-hash dedup, 10% stratified val split, feature-cache pre-warm -> manifest_v2.csv |
| `diag_v2.py` | wave-2 val-only recipe diagnostics (E1..E23 ablations; chose the shipped E20r30 recipe) |
| `extract_features.py` | pure-NumPy MFCC front-end + `--selftest` |
| `augment.py` | wave-level augmentation (v7: full recipe for close-mic cries, mild recipe for wave-2 distant cries), stress-val proxy, caches + `--selftest` |
| `train_cry_model.py` | trainer/evaluator/emlearn exporter (step 3; `--manifest manifest_v2.csv`) |
| `manifest.csv` | frozen-protocol split manifest (generated, v1 datasets) |
| `manifest_v2.csv` | merged train/val manifest (generated: base rows + wave-2 candidates) |
| `manifest_<name>.csv` | per-dataset candidate manifest snippets (generated, see below) |
| `prep_v2_report.txt` | wave-2 merge log: dedup method + stats + split summary (generated) |
| `dropped_clips_v2.csv` | wave-2 dedup audit trail: every dropped candidate + reason (generated, git-ignored) |
| `dataset_provenance.txt` | clone shas, licenses, seed, warnings (generated) |
| `features_cache.npz` | per-clip window feature cache (generated, git-ignored) |
| `features_aug_cache.npz` | augmented-train-window feature cache (generated, git-ignored) |
| `noise_pool_aug-v5.npz` | real-ambience noise pool for cry mixing (generated, git-ignored) |
| `model_report.txt` | training report (generated) |
| `data/` | dataset clones + converted WAVs (generated, git-ignored) |

## Datasets v2 acquisition log (November 2026)

Goal: expand training beyond donateacry + ESC-50 because cross-domain
(frozen-test) cry recall is stuck at **0.335** - the README's own diagnosis
("next lever: distant/room-recorded cry training data"). Three additional
corpora were acquired, converted to 16 kHz mono PCM16 WAV, and released to
the orchestrator as **candidate** data only (split assignment is left to
the integration step; the frozen test set is untouched; `manifest.csv`
was NOT regenerated).

Every converted clip lives at `ml_model/data/<name>/wav16/**.wav`; every
row is listed in `ml_model/manifest_<name>.csv`
(columns `path,label,split,source_id`, ALL rows `split=candidate`).
Conversion tool: `ml_model/acquire_v2.py <name>` (idempotent, ffmpeg
`-ac 1 -ar 16000`, duration derived from output size). Raw archives were
verified against the publisher's byte sizes, extracted with 7-Zip, and the
archives deleted after extraction to stay inside the disk budget.

| dataset | source | license | acquired | why / composition |
|---------|--------|---------|----------|-------------------|
| **SICCD** (2026-08-08, Sakha & Ameri) | zenodo.org/records/21848098 (DOI 10.5281/zenodo.21848098) | **CC BY 4.0** | **YES** - 3,200 clips / 4.44 h (cry only) | Standardized Infant Cry Classification Dataset: 4 cry-emotion classes (Hunger / Need_Attention / Physical_Pains / Scared), 2,870 original + 330 augmented (train split only). Compiled from 3 public Kaggle/HF repos; possible overlap with donateacry - de-duplicate by content hash before re-splitting. Already 16 kHz mono PCM16, fixed 5.0 s clips. |
| **CSIBE** (Common Sounds in Bedrooms, 2018-05-08, Csaba Kertesz) | zenodo.org/records/1243714 (DOI 10.5281/zenodo.1243714) | **CC BY 4.0** | **YES** - 29,770 clips (cry 1,200 / noncry 28,570) | RAW: 5,954 clips - 240 baby_cry + 5,714 noncry across 13 domestic classes (ambient_noise, bell, cat_meow, cicada, dog_bark, fart, guitar, laugh, parrot, piano, speech, traffic, vacuum_cleaner). AIBO: 23,816 short (~1 s) distant robot-mic recordings in 4 conditions (30 cm, 3 m up + 1 m, each +/- reverb) - includes **960 distant/reverb baby cries**, exactly the room-distance cry domain the v5 diagnosis asked for. |
| **hoyyu1/infant-cry-detection** (ICD_MMSP, 2025) | huggingface.co/datasets/hoyyu1/infant-cry-detection (paper: IEEE MMSP 2025, arxiv.org/abs/2508.19308) | **none stated** (public research artifact; no license field in README/HF metadata) | **YES** - 28,478 clips / 39.57 h (cry 13,330 / noncry 15,148) | 39.6 h household-environment cry detection set, 5 balanced folds (official `all28479.csv` metadata shipped in `data/hoyyu_icd/`). Positive: EnesBabyCries 5,026 + CryCeleb 4,552 (distant room recordings) + xunfei 3,752. Negative: domestic 4,506 + vox 4,963 + meow 1,953 + non 2,076 + silence 1,650. |
| Baby Chillanto | taylor0.bmd.mu.edu request form | request-only | no - form gated | Per acquisition policy: requires a request form; documented only. |
| AudioSet / FSD (Google) | research.google.com/audioset | ToS-restricted | no - ToS + size | Per acquisition policy. |
| Freesound packs | freesound.org | CC mix (per sound) | no - API key required | Per acquisition policy. |
| UrbanSound8K | urbansounddataset.weebly.com | CC BY-NC 3.0 | no - request form | Already the optional confuser source in the v5 protocol. |
| TUT Rare Sound Events (2017) | zenodo.org/records/1160455 / 401395 | other-nc | no - 7.4/17.5 GB (> 4 GB cap) | Over budget; NC license. |
| The Sounds of Home (gbibbo) | zenodo.org/records/12737915 (+4 sibling records) | CC BY 4.0 | no - 119 GB (>> 4 GB cap) | Real 14-mic in-home recordings; ideal negative side but ~40x over budget. |
| mahmudulhasan01/baby_crying_sound | huggingface.co | unknown | no - duplicate | donateacry re-upload (same 5-class taxonomy); we already hold the ODbL original. |
| DynamicSuperb/Infant_Crying_Classification_Dataset | huggingface.co | unclear | no - parquet-only, 414 clips | Would need a pyarrow install for 414 marginal clips; low value. |
| lequocduong/dataset_infant_crying | github.com | none | no - no license | Default all-rights-reserved; unusable without author permission. |
| ICBHI 2017 respiratory corpus | bhichallenge.med.auth.gr | custom | no - out of scope | Medical respiratory sounds, not cry/domestic (explicitly excluded). |
| Kaggle-hosted cry sets (warcoder etc.) | kaggle.com | mixed | no - no Kaggle credentials | Per acquisition policy (no API login); SICCD + hoyyu1 already incorporate the main public Kaggle cry repos. |

### New candidate data on disk (post-conversion, 16 kHz mono PCM16)

| dataset | cry clips | noncry clips | total duration | manifest snippet |
|---------|-----------|--------------|----------------|------------------|
| `data/siccd/` | 3,200 | 0 | 4.44 h | `manifest_siccd.csv` |
| `data/csibe/` | 1,200 | 28,570 | 5.88 h | `manifest_csibe.csv` |
| `data/hoyyu_icd/` | 13,330 | 15,148 | 39.57 h (18.51 h cry / 21.06 h noncry) | `manifest_hoyyu_icd.csv` |
| **total new** | **17,730** | **43,718** | | |

vs. the v5 training base (457 donateacry cry clips + ~2,200 ESC-50
non-cry clips): cry-side diversity grows ~39x, with distant/room-reverb
coverage (CSIBE-AIBO 960 + CryCeleb 4,552 clips) directly targeting the
0.335 cross-domain recall bottleneck.

### Licensing constraints on training use

- **SICCD, CSIBE: CC BY 4.0** - attribution required (Sakha & Ameri 2026 /
  Kertesz 2018, DOIs above); commercial use permitted; compatible with the
  academic course context.
- **hoyyu1/infant-cry-detection: no license stated.** Public research
  artifact accompanying a peer-reviewed paper (IEEE MMSP 2025) with public
  code (github.com/fhfjsd1/ICD_MMSP); treated as research-use-only. It is
  itself compiled from public datasets (EnesBabyCries, CryCeleb, xunfei
  speech/noise corpora) - derivatives may inherit the strictest underlying
  terms; do not redistribute the raw audio.
- **ESC-50 (CC BY-NC 3.0) + donateacry (ODbL 1.0)** remain
  non-commercial/research-education; the overall corpus therefore stays
  non-commercial regardless of the CC BY additions.
- All new data was delivered `split=candidate`: it was de-duplicated and
  re-split by `prep_v2.py` (md5 content-hash policy above; **no
  byte-identical overlap with donateacry was found** - the SICCD overlap
  warning was precautionary), merged into `manifest_v2.csv` (train/val
  only), and the frozen test protocol was not re-drawn (`manifest.csv`
  untouched). Integration outcome: see "Current status (v7)" above.
