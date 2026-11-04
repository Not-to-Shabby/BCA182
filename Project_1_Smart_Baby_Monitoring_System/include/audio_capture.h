/**
 * @file audio_capture.h
 * @brief P2 - Audio capture path for the Smart Baby Monitoring System.
 *
 * Captures the onboard electret microphone (M1 -> MIC_P/MIC_N differential ->
 * ES8388 LINPUT1/RINPUT1) whose ADCDATA leaves the codec on pin 8 and lands
 * on the STM32 I2S3_ext slave receiver:
 *
 *   ES8388 ADCDATA --> PB4 (I2S3_ext_SD, AF6, de-JTAG'd) --> I2S3ext (slave RX)
 *     --> DMA1 Stream 2 Channel 2 (circular, 512-word L,R buffer, HT/TC IRQ)
 *     --> capture thread (prio 5): L word of each L,R pair -> mono ring buffer
 *
 * The I2S3_ext receiver shares the WS/CK of the I2S3 master TX (audio output
 * path in audio_codec_es8388.c), so the whole system runs at ~16 kHz
 * (16 039 Hz). The DMA buffer lives in plain SRAM (CCM 0x10000000 is not
 * DMA-reachable on the STM32F407).
 */

#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the audio capture path.
 *
 * Calls audio_hardware_dac_init() (ES8388 codec + I2S3 master TX + TX DMA
 * engine, shared 16 kHz frame plan), then configures PB4 as I2S3_ext_SD
 * (AF6, releasing the default JTAG NJTRST function), the I2S3_ext peripheral
 * as 16-bit Philips slave receiver with RX DMA requests, and the DMA1
 * Stream 2 Channel 2 circular transfer with its half/full-transfer IRQ.
 */
void audio_capture_init(void);

/**
 * @brief Idempotently enable the capture path (DMA stream + I2S3_ext).
 *        Safe to call repeatedly; also acts as the readiness flag that
 *        releases the capture thread.
 */
void audio_capture_start(void);

/**
 * @brief Live microphone level, aggregated over the last ~1 s window.
 *
 * RMS and peak of the captured mono samples (full-scale int16). Silence with
 * the default +18 dB PGA sits in the low hundreds; speech near the board
 * pushes the RMS into the thousands. Snapshotted by the capture thread, so
 * both outputs are a consistent pair.
 */
void audio_capture_get_level(uint32_t *rms, int16_t *peak);

/**
 * @brief True once the one-shot boot loopback demo has finished (completed
 *        or aborted). The cry classifier waits on this before it starts
 *        draining the ring and reusing the demo scratch buffer.
 */
bool audio_capture_demo_done(void);

/**
 * @brief Drain the OLDEST mono samples from the capture ring (consumer side).
 *
 * The capture thread is the only producer; this function is the only
 * consumer. Copies min(ring_count, max_samples) oldest samples into dst and
 * advances past them, returning how many were copied. Safe against the
 * pre-empting capture thread (whole drain runs inside an IRQ lock).
 */
uint32_t audio_capture_ring_read(int16_t *dst, uint32_t max_samples);

/**
 * @brief Loan the 32078-sample demo record buffer as classification scratch.
 *
 * POST-DEMO LIFETIME CONTRACT: once audio_capture_demo_done() is true the
 * capture path never writes this buffer again (the demo freezes its write
 * index at the full length on both the completed and aborted paths), so the
 * ~10 s boot demo leaves this 64 KB allocation idle forever. The cry task
 * reuses it as its 1 s (16000-sample) window buffer, avoiding a +32 KB RAM
 * cost. Not valid for concurrent use with the demo (poll demo_done() first).
 */
int16_t *audio_capture_scratch_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H_ */
