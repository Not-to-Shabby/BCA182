/**
 * @file cry_task.c
 * @brief On-device baby-cry classification task (P3 Smart Baby Monitor).
 *
 * Pipeline (implements the frozen contract in cry_task.h):
 *   1. Wait for the boot loopback demo to release the audio path
 *      (audio_capture_demo_done(), polled at 100 ms).
 *   2. Claim the demo's 32078-sample record buffer as the 1 s window scratch
 *      (audio_capture_scratch_buffer(); no extra RAM cost) and precompute the
 *      FFT tables once (audio_features_init()).
 *   3. Drain the OLDEST 16000 mono samples (AUDIO_FEAT_WINDOW_SAMPLES) out of
 *      the capture ring (audio_capture_ring_read()) into the window buffer.
 *   4. Classify the full window: 29 features (audio_features_compute) ->
 *      emlearn forest confidence (cry_model_predict_confidence).
 *   5. Apply the spec's confirmation rule: a window is cry-positive when the
 *      model calls CRY with vote share >= CRY_CONF_THRESHOLD (70%), and the
 *      alarm fires only when >= 2 of the last 3 consecutive windows are
 *      positive. Detection latency is therefore ~1 window to fill (1-2 s)
 *      plus the 2-of-3 hysteresis, i.e. ~2-3 s end to end -- within spec for
 *      a notification use case.
 *   6. Publish cry_detected/cry_confidence and re-run the shared decision
 *      logic under the monitor-state lock (cry -> ALERT drives the existing
 *      buzzer/LED/banner machinery via the sensor task's next refresh too).
 *
 * Windows are NON-OVERLAPPING for v1 (filled = 0 after each compute): at
 * 16 kHz the ring refills a fresh window in ~1 s. Hop overlap (e.g. recompute
 * every 0.5 s on a 50% hop for faster reaction) is a tuning option left off.
 */

#include "cry_task.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "audio_capture.h"
#include "audio_features.h"
#include "baby_logic.h"
#include "cry_model.h"
#include "monitor_state.h"

/* Confirmation rule (spec): >= 2 of the last 3 consecutive 1 s windows. */
#define CRY_VOTE_HISTORY  3u
#define CRY_VOTES_NEEDED  2u

/* ------------------------------------------------------------------- thread */
static void cry_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* Park until the boot demo has finished using the mic path and the
	 * demo record buffer (it becomes our window scratch). */
	while (!audio_capture_demo_done()) {
		k_sleep(K_MSEC(100));
	}

	/* One-time setup: FFT twiddle precompute + scratch buffer claim. */
	audio_features_init();
	int16_t *win = audio_capture_scratch_buffer();
	uint32_t filled = 0;

	/* Last 3 per-window "cry" flags (circular), for the 2-of-3 rule. */
	bool vote_hist[CRY_VOTE_HISTORY] = { false, false, false };
	uint32_t vote_pos = 0;
	bool prev_detected = false; /* matches the zero-initialized shared state */

	for (;;) {
		/* Drain the OLDEST samples into the window until it is full.
		 * The ring holds ~1 s (16039 samples); a fresh window needs
		 * AUDIO_FEAT_WINDOW_SAMPLES (16000). */
		uint32_t n = audio_capture_ring_read(win + filled,
						     AUDIO_FEAT_WINDOW_SAMPLES -
							     filled);
		filled += n;
		if (filled < AUDIO_FEAT_WINDOW_SAMPLES) {
			k_sleep(K_MSEC(50)); /* not a full second yet */
			continue;
		}

		/* Classify one complete 1 s window. */
		float feat[AUDIO_FEAT_COUNT];
		audio_features_compute(win, AUDIO_FEAT_WINDOW_SAMPLES, feat);

		/* cry_model_predict_confidence() reports the vote share of the
		 * PREDICTED class: a window the forest confidently calls
		 * NONCRY (e.g. 90% noncry) must NOT count as cry-positive just
		 * because its confidence is high. Require the CRY class AND a
		 * cry vote share >= CRY_CONF_THRESHOLD. cry_confidence is
		 * published as the share of trees voting CRY (0..100). */
		uint8_t conf_pred = 0;
		const cry_class_t cls =
			cry_model_predict_confidence(feat, &conf_pred);
		const uint32_t cry_pct = (cls == CRY_PRED_CRY)
					     ? (uint32_t)conf_pred
					     : (100u - (uint32_t)conf_pred);
		const bool positive = (cls == CRY_PRED_CRY) &&
				      (cry_pct >= CRY_CONF_THRESHOLD);

		/* 2-of-3 consecutive windows confirmation (hysteresis). */
		vote_hist[vote_pos] = positive;
		vote_pos = (vote_pos + 1u) % CRY_VOTE_HISTORY;
		uint32_t votes = 0;
		for (uint32_t i = 0; i < CRY_VOTE_HISTORY; i++) {
			votes += vote_hist[i] ? 1u : 0u;
		}
		const bool confirmed = (votes >= CRY_VOTES_NEEDED);

		/* Publish under the shared-state lock: the cry input feeds the
		 * same FAULT > ALERT > NORMAL state machine the sensor task
		 * drives, so the decision layer runs mutually excluded. */
		monitor_state_lock();
		monitor_state_t *st = monitor_state_get();
		st->cry_confidence = cry_pct;
		st->cry_detected = confirmed;
		st->state = evaluate_system_state(st->state, st->env,
						  st->device_moved,
						  st->env_sensor_ok, confirmed);
		st->buzzer_on = baby_logic_buzzer_on(st->state,
						     k_uptime_get_32());
		monitor_state_unlock();

		/* One console line PER WINDOW while diagnosing recall (the
		 * model's frozen-test cry recall is 0.29, so watching the actual
		 * per-window CRY vote share tells us whether the gate or the
		 * model is the bottleneck). Downgrade to change-only prints
		 * once the threshold is tuned. */
		printk("[CRY] win cls=%s cry%%=%u -> %s\n",
		       (cls == CRY_PRED_CRY) ? "CRY" : "NONCRY",
		       (unsigned)cry_pct, confirmed ? "ALERT" : "-");

		/* One console line per confirmed-detection CHANGE. */
		if (confirmed != prev_detected) {
			prev_detected = confirmed;
			printk("[CRY] detected=%s conf=%u%%\n",
			       confirmed ? "YES" : "NO", (unsigned)cry_pct);
		}

		/* Non-overlapping windows for v1 (see file header): restart
		 * from a fresh window. Hop/overlap is a tuning option. */
		filled = 0;
	}
}

/* House style: static thread, no dynamic allocation (prio 7, 4096 stack).
 * CONFIG_THREAD_NAME=y publishes "cry_thread" as the runtime thread name. */
K_THREAD_DEFINE(cry_thread, 4096, cry_thread_fn, NULL, NULL, NULL, 7, 0, 0);

/* ----------------------------------------------------------------- start hook */
void cry_task_start(void)
{
	/* No-op by design: the cry thread is created at build time via
	 * K_THREAD_DEFINE() above and starts automatically at boot (it parks
	 * until the audio boot demo releases the mic path). The explicit call
	 * site in main() documents the bring-up order. */
}
