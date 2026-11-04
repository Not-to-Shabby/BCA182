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

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H_ */
