/**
 * @file net_task.c
 * @brief STUB — filled in by the P4 connectivity agent. Deliberately
 *        RAM-neutral: no thread, no stack, no networking CONFIG yet.
 *
 * IMPLEMENTATION NOTES for the P4 agent (main SRAM is 99.6% full!):
 *  - The net + mqtt thread stacks must live in the 64 KB DTCM via the
 *    raw-buffer pattern (K_THREAD_STACK_DEFINE carries its own section
 *    attribute and CANNOT be wrapped in a custom-section attribute):
 *      static uint8_t __attribute__((section(".dtcm_noinit"), aligned(32)))
 *              net_stack[2048 + 64];
 *      k_thread_create(&cb, (k_thread_stack_t *)net_stack, 2048, ...);
 *  - Free ~32 KB of main SRAM first by moving the mono ring (and/or the
 *    demo record buffer, shrunk to 16000 samples) to DTCM — both are
 *    CPU-access-only (never DMA'd).
 *  - The networking CONFIG block goes into zephyr/prj.conf together with
 *    the DTCM migration in one landing (see net_task.h RAM contract).
 *  - PB4/SPI2 note: keep &spi2 unclaimed in the overlay; the RW007 driver
 *    (LAB_3 rw007_wifi.c) owns SPI2 registers and holds PB12 high.
 */
#include "net_task.h"

void net_task_start(void)
{
}

bool net_task_is_online(void)
{
	return false;
}

const char *net_task_get_ip(void)
{
	return "0.0.0.0";
}
