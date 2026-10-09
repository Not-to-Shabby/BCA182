/**
 * @file synth_dsp.h
 * @brief Send effects (reverb, chorus) and the mastering stage (compressor, look-ahead
 *        limiter) that sit behind the FM voices.
 *
 * Everything works on blocks of at most DSP_BLOCK_FRAMES frames and only ever touches CPU
 * memory, so it can run on the host in unit tests and in the PC render bench.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef SYNTH_DSP_H_
#define SYNTH_DSP_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest block the processing functions accept. */
#define DSP_BLOCK_FRAMES        32U

/** The limiter delays the signal by this many frames so that it can see peaks coming. */
#define DSP_LIMITER_DELAY       63U

/** Output never exceeds this fraction of full scale. */
#ifndef DSP_CEILING
#define DSP_CEILING             0.955f
#endif

/** Clears every delay line and filter state (reverb tail, chorus, limiter, compressor). */
void dsp_init(float sample_rate);
void dsp_clear(void);

/**
 * @brief Level of the reverb and chorus returns in percent (0 to 200, default 100).
 *        0 turns both off and dsp_effects_active() reports false, so the caller can skip
 *        building the send buses.
 */
void dsp_set_effects_level(uint8_t percent);
uint8_t dsp_get_effects_level(void);
bool dsp_effects_active(void);

/**
 * @brief Runs the reverb on @p rev_in and the chorus on @p cho_in and adds their stereo
 *        returns to @p acc_l / @p acc_r.
 *
 * @param rev_in Mono reverb send bus, in the same units as the dry mix.
 * @param cho_in Mono chorus send bus.
 * @param n      Frames, at most DSP_BLOCK_FRAMES.
 */
void dsp_effects_process(const int32_t *rev_in, const int32_t *cho_in,
                         int32_t *acc_l, int32_t *acc_r, size_t n);

/** Compressor on or off. The look-ahead limiter always runs. */
void dsp_set_compressor(bool on);
bool dsp_get_compressor(void);

/**
 * @brief Applies the master gain, the compressor and the limiter and writes 16-bit stereo.
 *
 * @param acc_l,acc_r Summed mix, full scale = +-32767.
 * @param out         Interleaved L/R output.
 * @param n           Frames, at most DSP_BLOCK_FRAMES.
 * @param master_q8   Master gain in 1/256 steps.
 */
void dsp_master_process(const int32_t *acc_l, const int32_t *acc_r, int16_t *out,
                        size_t n, uint32_t master_q8);

/** Largest compressor gain reduction since the previous call, in tenths of a dB. */
int dsp_take_reduction_db10(void);

/** Samples for which the limiter was pulling the gain down. */
uint32_t dsp_get_limiter_samples(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_DSP_H_ */
