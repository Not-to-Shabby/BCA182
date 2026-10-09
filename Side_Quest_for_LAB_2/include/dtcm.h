/**
 * @file dtcm.h
 * @brief Placement attribute for CPU-only data in the 64 KB core-coupled RAM at 0x10000000.
 *        The DMA controllers cannot reach that region, so never use it for buffers that the
 *        SD card driver or the I2S DMA touch. On the host it expands to nothing.
 */

#ifndef DTCM_H_
#define DTCM_H_

#if defined(__arm__)
#define DTCM_BSS __attribute__((section(".dtcm_bss")))
#else
#define DTCM_BSS
#endif

#endif /* DTCM_H_ */
