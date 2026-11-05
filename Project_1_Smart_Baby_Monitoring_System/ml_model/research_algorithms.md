# Research: Better On-Device ML Algorithms for Baby-Cry Detection

**Scope**: Web research only. No code changed. This document surveys (1) published lightweight baby-cry detectors, (2) the actual export capabilities of emlearn 0.23.2, (3) feature-front-end upgrades feasible in our C mirror, (4) alternative inference stacks for Zephyr + Cortex-M4F, and ends with (5) a ranked list of concrete upgrade paths and a recommended next action.

**Date**: 2026-11-05.

---

## 0. Current baseline (facts, from `ml_model/model_report.txt`)

| Item | Value |
|---|---|
| Model | sklearn RandomForest 12 trees x depth 7, `class_weight=balanced`, inline emlearn C export |
| Features | 29 = 13 MFCC mean + 13 MFCC std + log-energy mean/std + ZCR mean, over 1 s windows @ 16 kHz |
| Firmware rule | cry iff strict majority of trees vote CRY **and** cry vote share >= 75 % |
| VAL cry F1 | 0.885 (P 0.916 / R 0.857); raw-val cry recall 0.956, stress-val recall 0.757 |
| **FROZEN TEST** | cry recall **0.335**, precision 0.848; accuracy 0.637 |
| Clip-level (frozen test) | 27/40 cry clips have >= 1 positive window; 18/40 have >= 2 positives |
| Confuser FP rate | 7.7 % (worst: rooster 14.5 %, siren 13.0 %) |
| Model size | 262,095 bytes inline header (budget 400 KB; flash 1 MB, 14.5 % used) |
| RAM | main SRAM 99.5 % full; **DTCM/CCM 58 KB free** (CPU-only, no DMA) |

**Diagnosis of the recall ceiling** (accepted framing): (a) the 29-feature summary destroys within-window temporal dynamics — a cry has syllabic structure (0.5–2 Hz syllable rate) that mean/std cannot express; (b) domain shift — training is donateacry close-mic phone clips, frozen test is 40 distant/ambient ESC-50 `crying_baby` clips (YouTube); (c) emlearn's model zoo (no CNN/SVM) limits model families.

---

## 1. What published lightweight baby-cry detectors use

### 1.1 Classical ML on MFCC summaries (our family)

| Study | Features | Model | Result | Notes |
|---|---|---|---|---|
| Riadi et al. 2024, JEEEMI ([link](https://jeeemi.org/index.php/jeeemi/article/view/350)) | 10 MFCC, 7 s clips | RandomForest | acc 0.844, F1 0.773 (5-class donateacry) | Directly comparable baseline to our RF |
| EURASIP JASMP 2021 review, Singh et al. ([link](https://link.springer.com/article/10.1186/s13636-021-00197-5)) | MFCC (+ prosodic) | SVM / GMM / DL | SVM+MFCC up to ~96 % | Most-cited cry review (200+ cites); notes combining prosodic features with MFCC improves accuracy |
| WIREs Data Mining & Knowledge Discovery 2026 review ([link](https://wires.onlinelibrary.wiley.com/doi/10.1002/widm.70101)) | MFCC | KNN | 93 % on DonateACry | Survey of classical benchmarks |
| IEEE TAAS 2026 survey ([abstract](https://www.computer.org/csdl/journal/ta/2026/01/11267068/2bUqPTUno40)) | MFCC | lightweight KNN | 98.8 % reported | Survey-level claim; KNN is a poor fit for MCU (brute-force distance over training set) |
| Sharma et al. 2019 ([link](https://www.researchgate.net/publication/338361725)) | statistical features | GMM | 81.3 % (5-class, 714 babies) | Classic frame-level GMM approach |
| Alaie et al. 2016, PMC ([link](https://pmc.ncbi.nlm.nih.gov/articles/PMC4971135)) | MFCC frames | GMM-UBM + likelihood-ratio test | pathology screening | The canonical "GMM as likelihood function + LLR decision" pattern |
| Patil et al., EUSIPCO 2022 ([PDF](https://eurasip.org/Proceedings/Eusipco/Eusipco2022/pdfs/0001313.pdf)) | subband Teager energy | GMM, decision by **log-likelihood ratio** | — | Frame/utterance LLR scoring recipe, cheap enough for MCU |

**Takeaway**: within the "summary + classical classifier" family, nothing beats what we already have by a large margin; RF ~0.84 acc on 5-class donateacry is typical. The gains in this family come from the **front end** (delta features, level-robust normalization) and **decision layer**, not from swapping RF for SVM/KNN.

### 1.2 Deep / lightweight audio models (what "good" looks like)

| Study | Model | Input | Result | MCU relevance |
|---|---|---|---|---|
| SE-ResNet-Transformer, Sensors 2024 ([PMC11510884](https://pmc.ncbi.nlm.nih.gov/articles/PMC11510884)) | SE-ResNet + Transformer | **Mixed MFCC = MFCC + delta + delta-delta**, 20 ms frames / 10 ms hop, per-emphasis 0.97 | 93 % on donateacry+Chillanto mix (baselines: ResNet18 87 %, plain ResNet 84–88 %; prior CNN work 86–90 %) | Ablation shows **mixed MFCC beats plain MFCC** — direct evidence for delta features |
| ICSD dataset paper, arXiv 2024 ([arXiv:2408.10561](https://arxiv.org/html/2408.10561v1)) | MT-CRNN (Mean-Teacher CNN+GRU), CRNN+BEATs | log-mel + SSL features | Real strongly-labeled test: cry F1 0.820 → **0.880** (competitive variant); synthetic test > 0.95 | Modern realistic-condition benchmark: **heavy CRNN systems reach cry F1 0.82–0.88 on real noisy data** — sets a sane expectation ceiling |
| DS-CNN / "Hello Edge" (Zhang et al. 2017) + ARM CMSIS-NN blog ([ARM](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/high-accuracy-keyword-spotting-on-cortex-m-processors), [EURASIP 2020](https://link.springer.com/article/10.1186/s13636-020-00176-2)) | depthwise-separable CNN | log-mel patches (KWS-style) | ~94 %+ on Speech Commands with tens of k parameters; deployed on Cortex-M via CMSIS-NN | The reference architecture for MCU audio CNN; int8, flash-resident weights |
| BC-ResNet (Kim et al., Interspeech 2021 [page](https://www.isca-archive.org/interspeech_2021/kim21l_interspeech.html); DCASE 2021 Workshop [PDF](https://dcase.community/documents/workshop2021/proceedings/DCASE2021Workshop_Kim_12.pdf); [arXiv:2111.06531](https://arxiv.org/abs/2111.06531); [code](https://github.com/Qualcomm-AI-research/bcresnet)) | broadcasted residual 1D+2D CNN | log-mel | 98.0 % / 98.7 % top-1 (Speech Commands v1/v2); ASC variant 76.3 % @ 315 k params, **75.3 % after compression to 61 KB non-zero parameters** (DCASE 2021 Task 1a) | SOTA low-complexity audio net; the ~61 KB int8 regime fits our 400 KB flash budget |
| TinyML infant-cry prototype, IEEE Access 2025 ([PDF](https://ieeexplore.ieee.org/iel8/6287639/10820123/11132295.pdf)) | TinyML CNN | MFCC/MFE | resource-constrained MCU target (paywalled; see also Infineon DEEPCRAFT commercial cry model, [link](https://www.infineon.com/design-resources/embedded-software/deepcraft-edge-ai-solutions/deepcraft-ready-models/deepcraft-ready-model-for-baby-cry-detection), claimed robust to 5 m and indoor noise) | Confirms commercial edge vendors ship cry detection on MCUs with mel-front-end CNNs |

**Takeaway**: every system that meaningfully beats the summary+classical family on realistic/distant audio uses a **time-frequency image front end (log-mel or mixed MFCC) + a CNN/CRNN**, trained with heavy augmentation. The gap between our frozen recall (0.335) and modern real-condition cry F1 (0.82–0.88) is mostly front end + domain shift, not classifier capacity.

---

## 2. What emlearn 0.23.2 actually supports (verified against source at tag `0.23.2`)

Verified from the docs ([status page](https://emlearn.readthedocs.io/en/latest/source/README.html), [docs index](https://emlearn.readthedocs.io/en/latest/)) and the tagged source files ([eml_net_common.h](https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/eml_net_common.h), [eml_net.h](https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/eml_net.h), [net.py](https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/net.py), [mixture.py](https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/mixture.py)):

| C module | Python source | What is supported | Constraints / size behavior |
|---|---|---|---|
| `eml_trees` | sklearn | RandomForestClassifier, ExtraTreesClassifier, DecisionTreeClassifier (+ regressors) | Inline C: all nodes as `const` arrays in flash; **zero RAM** at inference; size = f(node count), independent of feature count. Feature-quantized nodes (int8/uint8) available to shrink tables |
| `eml_net` | sklearn `MLPClassifier`; Keras `Sequential` | **Dense (fully-connected) layers only** — Keras converter raises `NotImplementedError` for any non-Dense layer type; activations: `identity`, `relu`, `logistic`, `softmax`, `tanh` | float32 `const` weights in flash; 2 activation scratch buffers in RAM sized to the largest layer (hundreds of bytes for small MLPs). A fixed-point variant exists (`eml_net_fixedpoint.h`) but only for `inline` inference |
| `eml_mixture` | sklearn `GaussianMixture`, `BayesianGaussianMixture` | GMM scoring; all covariance types (`full`, `tied`, `diag`, `spherical`) converted to full at export; `predict_log_proba` available | Full covariance = `F^2` floats per component (29 features, 8 comps x 2 classes ≈ 27 KB flash; `diag` ≈ 1 KB). Small float scratch RAM. Less battle-tested than trees |
| `eml_bayes` | sklearn | GaussianNaiveBayes | Tiny; weaker classifier |
| `eml_distance` | sklearn | EllipticEnvelope (Mahalanobis) | Anomaly detection use |
| `eml_neighbors` | sklearn | NearestNeighbors (brute-force) | O(N_train) per frame — infeasible for 20 k+ training windows on MCU |
| — | — | **No SVM, no CNN/conv layers, no RNN** in 0.23.2 | emlearn-micropython ships CNNs via TinyMaix, but that is not in the C library |

Also relevant: emlearn ships C **feature-extraction** utilities ([docs](https://emlearn.readthedocs.io/en/latest/feature_extraction.html)): RMS, ZCR, min/max/mean/median/percentile summaries, IIR filters (scipy SOS design), FFT, and mel-filterbank spectrogram — explicitly designed so "the same C code runs in training and on target", which is exactly our golden-feature mirror strategy. And there is an official [Zephyr getting-started guide](https://emlearn.readthedocs.io/en/latest/getting_started_zephyr.html).

**Size/accuracy trade-off notes for our case**:
- Tree size is driven by node count (trees x depth), **not** by the number of input features — so widening the feature vector from 29 to ~55 costs essentially no flash. Our current 262 KB header leaves room for deeper/wider ensembles under the 400 KB budget.
- `rf24x6` (already trained in `train_cry_model.py` candidates) scored val F1 0.883 vs 0.885 for `rf12x7` — the RF is **saturated on the current features**; more trees/deeper trees are not the bottleneck. This is the single most important emlearn-zoo finding: the classifier is not what is holding recall back.

---

## 3. Feature-front-end improvements proven in literature and feasible in our C mirror

The FFT + mel pipeline already runs ~real time with headroom, so front-end compute is nearly free. Ranked by evidence strength for our failure mode (dynamics destroyed + level/domain shift):

1. **Delta and delta-delta MFCC (MFCC-Δ, MFCC-ΔΔ)** — regress each MFCC stream over consecutive frames (standard 2-frame window, regression formula e.g. in [this primer](https://desh2608.github.io/2019-07-26-delta-feats)). Proven for cry: the SE-ResNet-Transformer paper's "mixed MFCC" (MFCC+Δ+Δ²) beat plain MFCC and single-stream variants ([Sensors 2024](https://pmc.ncbi.nlm.nih.gov/articles/PMC11510884)). Cost in C: one extra feature pass over the per-frame MFCC matrix we already compute; pool Δ/Δ² with mean (+ optionally std). Feature count 29 → ~55. Tree flash cost: unchanged (Section 2).
2. **Per-frame classification + vote/pool aggregation instead of window-summary** — the standard SED architecture is: frame-level probabilities → median/mean smoothing → threshold (or two-threshold hysteresis) → event decision ([Çakır et al. 2019, post-processing comparison](https://arxiv.org/html/1906.06909v2); [Dinkel et al. 2019, double thresholding](https://arxiv.org/abs/1904.03841); [Ebbers et al. 2023, piPSDS](https://inria.hal.science/hal-04385022)). Concretely for us: run the RF (or GMM) on each ~32–64 ms frame, then derive window features such as *share of positive frames*, *max/median frame probability*, *run-length of positive frames*. This restores the temporal dimension that mean/std throws away, without any new runtime dependency. Compute: 12 trees x ≤127 nodes x ~31 frames/s ≈ tens of k node comparisons per second — negligible on a 168 MHz M4F.
3. **PCEN (Per-Channel Energy Normalization)** for level/distance robustness — per-mel-channel one-pole automatic gain control followed by dynamic-range compression: `E_smooth[t] = (1-α)·E_smooth[t-1] + α·E[t]`, output `(E / (E_smooth + δ))^r − ε`, with the canonical parameters α ≈ 0.98, δ ≈ 2–10, r ≈ 0.25–0.5 ([Wang et al. 2017, original far-field KWS paper](https://research.google/pubs/trainable-frontend-for-robust-and-far-field-keyword-spotting); [Lostanlen et al., IEEE SPL "Why and How"](https://www.lostanlen.com/wp-content/uploads/2019/12/lostanlen2019spl.pdf) with [companion explainer](https://www.justinsalamon.com/news/per-channel-energy-normalization-why-and-how); applied to urban SED in [Ick et al. 2021](https://par.nsf.gov/servlets/purl/10249857)). This directly targets the close-mic → distant domain shift (level invariance), and is ~15 lines of C + one `n_mels`-float state array (RAM-safe). Must be mirrored in Python training — our `gen_feature_golden.py` harness exists precisely for this.
4. **Better temporal pooling** — replace/augment `mean`+`std` with: mean of top-k frames (e.g., top-3 of 31), max, and median per coefficient. Top-k pooling emphasizes cry syllables embedded in silence; costs one sort over the frame axis in C. Low risk, cheap A/B.
5. **Spectral shape descriptors** — spectral centroid / rolloff / flatness means (and stds) are standard complements to MFCC in the cry literature ([EURASIP 2021 review](https://link.springer.com/article/10.1186/s13636-021-00197-5)); they help separate noisy confusers (siren = flat, rooster = impulsive) from harmonic cries. We already have the FFT bins; each descriptor is a few dot products.
6. **Training-side only (no C change)**: SpecAugment-style time/frequency masking on the frame feature matrix, and adding the ESC-50 confuser classes (rooster/siren/chainsaw/helicopter) as explicit hard negatives — consistent with the augmentation recipe already in `augment.py` and the SED literature's multi-condition training practice.

Items 1+2+3 together address all three named causes: (a) dynamics restored by per-frame scoring + deltas, (b) level shift mitigated by PCEN, (c) unchanged (still emlearn) — but (c) stops mattering once the front end is fixed, because the RF was saturated, not starved of capacity.

---

## 4. Alternative inference stacks under Zephyr + PlatformIO on M4F — honest assessment

| Stack | What it is | Flash / RAM behavior | Integration cost on OUR stack | Honest verdict |
|---|---|---|---|---|
| **emlearn 0.23.2** (status quo) | C99 inline codegen, sklearn/Keras-dense | const flash, ~0 RAM | already integrated | Keep. Zero-risk, feature-extraction C included, Zephyr guide exists |
| **TFLite Micro as Zephyr module** ([Zephyr docs + sample](https://docs.zephyrproject.org/latest/samples/modules/tflite-micro/tflite-micro.html), [module issue #34295](https://github.com/zephyrproject-rtos/zephyr/issues/34295)) | Official `modules/lib/tflite-micro` pulled by `west update` | Interpreter + tensor arena in **RAM**: hello-world ~20 KB arena; a small audio CNN typically needs **30–80 KB arena** depending on width | HIGH: C++ runtime in a Zephyr+PlatformIO build, `west` module wiring through PlatformIO's Zephyr, int8 quantization-aware training pipeline (TFLite converter), mel-spectrogram C front end, arena sizing | The cleanest "official" path, but the arena must fit in **DTCM 58 KB** (main SRAM is full) — feasible only for a narrow model, and the toolchain churn is the real cost, not the model |
| **ST Edge AI Core (`stm32ai` CLI)** ([getting started](https://dev.st.com/edge-ai-docs/general/latest/en/docs/tutorials/st-edge-ai-core-getting-started.html), [CLI manual](https://dev.st.com/edge-ai-docs/st-edge-ai-core/4.0.1/en/docs/user-manual/core-stm32-cli.html), [product page](https://www.st.com/en/development-tools/stedgeai-core.html)) | Free ST CLI: converts `.tflite`/`.onnx`/`.keras` into optimized C (CMSIS-NN kernels) for STM32 | Much leaner than TFLM — third-party measurement: **~36 KB RAM vs ~151 KB for TFLM on the same network** ([stupid-projects benchmark](https://www.stupid-projects.com/posts/machine-learning-on-embedded-part-4)); 8-bit quantization shrinks flash ([X-CUBE-AI datasheet](https://www.mouser.com/pdfDocs/x-cube-ai-2.pdf)) | MEDIUM: run `stm32ai` offline on the PC, copy the generated C files into `src/`/`lib/` and build with PlatformIO — no CubeMX needed for codegen (validation is CubeMX-centric, can be skipped or scripted via CLI) | The **practical CNN path** if we ever go CNN: same Keras training as TFLM, far less RAM pressure, generated plain C fits PlatformIO |
| **CMSIS-NN standalone** ([GitHub](https://github.com/ARM-software/CMSIS-NN), [manual](https://arm-software.github.io/CMSIS-NN/latest), [paper](https://arxiv.org/abs/1801.06601)) | int8 conv/depthwise/FC/pooling kernels (2–5x speedup, ~3x energy vs float) | weights const flash, buffers caller-allocated | HIGH as a hand-rolled path: manual graph execution, quantization params, buffer plumbing; LOW if used *under* TFLM/ST Edge AI | Only worth it hand-rolled for one fixed small CNN; otherwise consume it via ST Edge AI output |
| **onnx2c** ([GitHub](https://github.com/kraiskil/onnx2c)) or **emx-onnx-cgen** ([GitHub](https://github.com/emmtrix/emx-onnx-cgen), [announcement](https://www.emmtrix.com/news/onnx-to-generic-c-for-embedded-ai.html)) | ONNX → single C file, no dynamic memory, TinyML target | static buffers, model-dependent | MEDIUM: train in PyTorch → ONNX → generate C → drop into PlatformIO; but onnx2c's conv support is less optimized (no CMSIS-NN) and the project is single-maintainer | Good middle ground for small conv/dense nets; less hardened than ST Edge AI for STM32 |

**Bottom line**: for the emlearn-family upgrades (Sections 3, 5) there is no reason to leave emlearn — it already satisfies every constraint and has the Zephyr/PlatformIO story solved. A CNN is only worth the stack churn via **ST Edge AI Core generated C** (not raw TFLM), and only if the front-end work (Section 3) demonstrably stalls.

---

## 5. Ranked upgrade paths

Expected gains are calibrated to: (i) our own clip-level counts (27/40 frozen cry clips already emit >= 1 positive window — decision logic alone can recover a lot), (ii) the literature deltas above, (iii) the frozen-test protocol being a cross-domain test (donateacry train vs ESC-50 distant test), which bounds what any retrain can achieve.

| Rank | Path | What changes | Expected frozen-test cry recall | Integration cost | Risk |
|---|---|---|---|---|---|
| **1** | **Front-end v3 on emlearn RF**: per-frame scoring (share of positive frames / median / max frame score as the window decision), + MFCC Δ and ΔΔ pooled features, + PCEN replacing fixed log compression, + top-k / median pooling, + spectral centroid/rolloff/flatness; retrain same RF candidates (+ ExtraTrees A/B) and re-tune threshold on VAL only | `extract_features.py` + C mirror + `train_cry_model.py`; same export path | **0.50–0.65** at precision ≥ 0.8 (deltas + per-frame voting attack cause (a); PCEN attacks (b)) | 2–4 days (golden-feature infra already exists) | Low — no new runtime; main risk is Python/C numeric divergence of PCEN/deltas (mitigated by golden test) |
| **2** | **Decision-layer rescue (no retraining)**: rolling firmware confirmation — cry event iff ≥ 2 of last 4 windows positive (hysteresis: enter at ≥ 2, exit at 0 for N windows), per SED post-processing practice ([Çakır 2019](https://arxiv.org/html/1906.06909v2), [Dinkel 2019](https://arxiv.org/abs/1904.03841)) | `src/cry_task.c` only | Event/clip-level recall 0.335 → ~0.45–0.55 today (from clip counts: ≥2-of-N confirmation matches 18/40 cry clips at 3/40 noncry FP clips; ≥1-of-N would give 27/40 but 8/40 FP) | 0.5–1 day | Very low — pure firmware logic, frozen model untouched |
| **3** | **Frame-level GMM log-likelihood-ratio detector via `eml_mixture`**: train cry-GMM and non-cry-GMM on frame features (PCEN-mel or MFCC+Δ), score each frame by LLR, smooth, threshold ([classic GMM-UBM/LLR pattern](https://pmc.ncbi.nlm.nih.gov/articles/PMC4971135), [EUSIPCO 2022](https://eurasip.org/Proceedings/Eusipco/Eusipco2022/pdfs/0001313.pdf)); either standalone or fused with RF votes | new `eml_mixture` tables (diag cov ≈ 1 KB; full cov 29-dim x 8 comps x 2 ≈ 27 KB flash), modest C glue (runtime struct + pointers, not inline codegen) | 0.45–0.60 (complementary probability model; handles dynamics naturally; level-robust only if PCEN applied) | 3–5 days | Medium — emlearn GMM path is less trodden than trees; needs its own golden test |
| **4** | **MLP A/B via `eml_net`**: convert `sklearn MLPClassifier` (or small Keras dense net) on the expanded v3 feature set; activations relu/softmax; weights const float32 | training script only + drop-in header; ~1 KB RAM for activation buffers | 0–0.05 over RF (RF is saturated; MLP adds capacity where we don't need it) | 1 day | Low — cheap to include in the Rank-1 candidate sweep, but do not expect it to move frozen recall |
| **5** | **Mel-patch CNN (DS-CNN / BC-ResNet-lite) via ST Edge AI Core** (`stm32ai` generate C → PlatformIO), int8, arena sized to DTCM 58 KB ([ST Edge AI](https://dev.st.com/edge-ai-docs/st-edge-ai-core/index.html), [DS-CNN on M](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/high-accuracy-keyword-spotting-on-cortex-m-processors), [BC-ResNet](https://arxiv.org/abs/2111.06531)) | new Keras training pipeline, .tflite → `stm32ai`, mel front end in C, C++-free generated runtime | **0.70–0.85** (published real-condition cry F1 0.82–0.88 with CRNNs; KWS-grade CNNs ~94–98 % on their domains) — highest ceiling | 1–2 weeks | Medium-high — RAM arena must fit 58 KB DTCM; new training/export toolchain; largest blast radius; realistically a post-course or stretch item |

Deliberately rejected: SVM (not exportable by emlearn 0.23.2), KNN (`eml_neighbors` is brute-force O(N) per frame — infeasible with 36 k training windows), raw TFLM-Zephyr module (strictly dominated by ST Edge AI generated C on RAM and integration friction for this project).

---

## Recommended next action

Execute **Rank 2 today** (a ~10-line change in `src/cry_task.c`: 2-of-4 rolling-window confirmation with hysteresis on the existing per-window model output), because the frozen-test clip counts show nearly half the missed cry clips already produce positive windows that the current single-window rule discards — this alone should lift event-level recall from 0.335 toward 0.45–0.55 at unchanged precision, with zero retraining risk. Then commit the next 2–4 days to **Rank 1**: keep the emlearn RandomForest (it is provably not the bottleneck — `rf24x6` val F1 0.883 vs 0.885 for `rf12x7` means the classifier is saturated), and instead upgrade the front end to v3 — per-frame RF scoring with share-of-positive-frames / median / max pooling, MFCC delta and delta-delta features, PCEN in place of fixed log compression, and top-k temporal pooling — retraining the same candidate grid (including an ExtraTrees variant) and re-tuning the vote-share threshold on VAL only, exactly as the frozen protocol requires. Validate every new C mirror step against `gen_feature_golden.py` before the single frozen-test evaluation. Include the Rank-4 MLP candidate in the same sweep since it costs one conversion run, and defer Rank 3 (GMM-LLR) and Rank 5 (CNN via ST Edge AI Core) until after the course deadline — Rank 5 is the only path with a realistic shot at frozen recall above 0.7, but its 1–2 week toolchain integration and the 58 KB DTCM arena constraint make it a poor fit for the remaining project window.

---

## Sources

- emlearn docs — status/supported models: https://emlearn.readthedocs.io/en/latest/source/README.html
- emlearn docs index (classification, mixture, neighbors, Zephyr guide, feature extraction): https://emlearn.readthedocs.io/en/latest/
- emlearn 0.23.2 source (eml_net activations/layers; GMM covariance handling; Keras Dense-only converter): https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/eml_net_common.h , https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/net.py , https://github.com/emlearn/emlearn/blob/0.23.2/emlearn/mixture.py
- emlearn FOSDEM 2025 talk (RF sizing on MCUs): https://archive.fosdem.org/2025/events/attachments/fosdem-2025-4524-milliwatt-sized-machine-learning-on-microcontrollers-with-emlearn/slides/238880/FOSDEM_20_zDTNPqa.pdf
- Riadi et al. 2024, RF on donateacry (JEEEMI): https://jeeemi.org/index.php/jeeemi/article/view/350
- Singh et al. 2021, infant cry review (EURASIP JASMP): https://link.springer.com/article/10.1186/s13636-021-00197-5
- WIRES 2026 comprehensive cry review: https://wires.onlinelibrary.wiley.com/doi/10.1002/widm.70101
- IEEE TAAS 2026 cry survey: https://www.computer.org/csdl/journal/ta/2026/01/11267068/2bUqPTUno40
- SE-ResNet-Transformer, Sensors 2024 (mixed MFCC + delta ablation): https://pmc.ncbi.nlm.nih.gov/articles/PMC11510884
- ICSD dataset + CRNN baselines, arXiv 2024: https://arxiv.org/html/2408.10561v1
- Alaie et al. 2016, GMM-UBM likelihood-ratio cry pathology: https://pmc.ncbi.nlm.nih.gov/articles/PMC4971135
- Patil et al., EUSIPCO 2022, GMM LLR classification: https://eurasip.org/Proceedings/Eusipco/Eusipco2022/pdfs/0001313.pdf
- Sharma et al. 2019, GMM cry decoding: https://www.researchgate.net/publication/338361725
- Infineon DEEPCRAFT baby-cry commercial edge model: https://www.infineon.com/design-resources/embedded-software/deepcraft-edge-ai-solutions/deepcraft-ready-models/deepcraft-ready-model-for-baby-cry-detection
- DS-CNN / Hello Edge deployment (ARM blog): https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/high-accuracy-keyword-spotting-on-cortex-m-processors
- DS-CNN EURASIP 2020 (Sørensen et al.): https://link.springer.com/article/10.1186/s13636-020-00176-2
- BC-ResNet KWS (Interspeech 2021): https://www.isca-archive.org/interspeech_2021/kim21l_interspeech.html ; code: https://github.com/Qualcomm-AI-research/bcresnet
- BC-ResNet ASC (DCASE 2021 Workshop): https://dcase.community/documents/workshop2021/proceedings/DCASE2021Workshop_Kim_12.pdf ; arXiv: https://arxiv.org/abs/2111.06531
- PCEN original (Wang et al. 2017): https://research.google/pubs/trainable-frontend-for-robust-and-far-field-keyword-spotting
- PCEN Why and How (Lostanlen et al., IEEE SPL): https://www.lostanlen.com/wp-content/uploads/2019/12/lostanlen2019spl.pdf ; explainer: https://www.justinsalamon.com/news/per-channel-energy-normalization-why-and-how
- PCEN for urban SED (Ick et al. 2021): https://par.nsf.gov/servlets/purl/10249857
- SED post-processing comparison (Çakır et al. 2019): https://arxiv.org/html/1906.06909v2
- Double thresholding / duration robustness (Dinkel et al. 2019): https://arxiv.org/abs/1904.03841
- piPSDS post-processing-independent evaluation (Ebbers et al. 2023): https://inria.hal.science/hal-04385022
- MFCC delta primer: https://desh2608.github.io/2019-07-26-delta-feats
- TFLite Micro Zephyr sample (official module): https://docs.zephyrproject.org/latest/samples/modules/tflite-micro/tflite-micro.html ; module issue: https://github.com/zephyrproject-rtos/zephyr/issues/34295
- ST Edge AI Core: https://www.st.com/en/development-tools/stedgeai-core.html ; getting started: https://dev.st.com/edge-ai-docs/general/latest/en/docs/tutorials/st-edge-ai-core-getting-started.html ; CLI manual: https://dev.st.com/edge-ai-docs/st-edge-ai-core/4.0.1/en/docs/user-manual/core-stm32-cli.html
- X-CUBE-AI vs TFLM RAM benchmark: https://www.stupid-projects.com/posts/machine-learning-on-embedded-part-4
- X-CUBE-AI datasheet (int8 flash/RAM effects): https://www.mouser.com/pdfDocs/x-cube-ai-2.pdf
- CMSIS-NN: https://github.com/ARM-software/CMSIS-NN ; manual: https://arm-software.github.io/CMSIS-NN/latest ; paper: https://arxiv.org/abs/1801.06601
- onnx2c: https://github.com/kraiskil/onnx2c ; emx-onnx-cgen: https://github.com/emmtrix/emx-onnx-cgen
- Edge Impulse cry-detection community data strategy (practical MCU audio guidance): https://forum.edgeimpulse.com/t/baby-cry-audio-data-strategy-guidance/10039
