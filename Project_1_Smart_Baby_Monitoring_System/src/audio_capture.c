/**
 * @file audio_capture.c
 * @brief P2 - Audio capture path: ES8388 ADC -> I2S3_ext slave RX on PB4 ->
 *        DMA1 Stream 2 Channel 2 circular double-half buffer -> mono ring.
 *
 * Capture data flow:
 *   - The onboard electret mic (differential MIC_P/MIC_N into ES8388
 *     LINPUT1/RINPUT1) is digitized by the codec ADC; ADCDATA is driven on
 *     pin 8 to the MCU net I2S3_ext_SD = PB4.
 *   - PB4 defaults to the JTAG NJTRST function (AF0); it is re-muxed to AF7
 *     (I2S3_ext_SD / SPI3_MISO function) by clearing the JTAG role via MODER.
 *   - I2S3ext is the receive-only extension of SPI3 (RM0090 section 28.5);
 *     it shares WS/CK with the I2S3 master TX and runs as 16-bit Philips
 *     slave receiver at the shared ~16 kHz frame rate.
 *   - DMA1 Stream 2 Channel 2 (per ST AN4031 request map) moves 16-bit words
 *     from I2S3ext->DR into a 512-word circular buffer (256 L,R pairs per
 *     half). Stream 2 is a LOW stream: its ISR flags live in DMA1->LISR and
 *     are cleared via DMA1->LIFCR (NOT the HISR/HIFCR used by the TX Stream 5).
 *   - The capture thread (prio 5) takes the L word of each L,R pair (the
 *     differential mic yields L == R) and pushes it into a mono ring buffer
 *     for the future ML task.
 *
 * A one-shot loopback demo runs once ~5 s after boot: records 2 s of mono
 * audio into a static SRAM buffer, prints stats, then plays it back through
 * the ES8388 DAC (headphone jack) by duplicating each mono sample into L and
 * R words via the codec driver's PCM feed API (audio_pcm_write()). After the
 * demo, live capture continues into the ring buffer with a 10 s status line.
 *
 * All DMA-accessible buffers are plain static SRAM (no CCM: 0x10000000 is not
 * reachable by the DMA controller, no section attributes used).
 */

#include "audio_capture.h"

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>

#include "audio_codec_es8388.h"

/* ------------------------------------------------------------------ timing */
/* Shared I2S frame plan: PLLI2S N=271/R=2 -> 135.5 MHz, I2SDIV=16, ODD=1
 * -> Fs = 135.5 MHz / (256 * 33) = 16039 Hz (+0.24% vs 16 kHz). */
#define AUDIO_FS_HZ             16039u
#define AUDIO_RECORD_SECONDS    2u
#define AUDIO_RECORD_SAMPLES    (AUDIO_FS_HZ * AUDIO_RECORD_SECONDS) /* 32078 */
#define AUDIO_RING_SECONDS      1u
#define AUDIO_RING_SAMPLES      (AUDIO_FS_HZ * AUDIO_RING_SECONDS)   /* 16039 */
#define AUDIO_STAT_PERIOD_MS    10000u
#define AUDIO_LEVEL_PERIOD_MS   2000u  /* live mic meter cadence */

/* ------------------------------------------------------------------- DMA RX */
#define AUDIO_RX_DMA_WORDS      512u                      /* L,R interleaved */
#define AUDIO_RX_HALF_WORDS     (AUDIO_RX_DMA_WORDS / 2u) /* 256 words = 128 L,R pairs */
#define AUDIO_CHUNK_MONO        128u                      /* mono samples per feed chunk */

/* DMA buffers in plain main SRAM: CCM (0x10000000) is NOT DMA-reachable. */
static uint16_t s_rx_dma_buf[AUDIO_RX_DMA_WORDS];  /* I2S3ext RX, circular */
static int16_t  s_record_buf[AUDIO_RECORD_SAMPLES]; /* 2 s mono demo buffer */
static int16_t  s_ring_buf[AUDIO_RING_SAMPLES];     /* 1 s mono ML ring */
static int16_t  s_stereo_chunk[2u * AUDIO_CHUNK_MONO]; /* L,R staging for TX feed */

/* ------------------------------------------------------------ capture state */
/* ISR-side state (DMA1 Stream 2 half/full transfer) */
static K_SEM_DEFINE(s_half_sem, 0, K_SEM_MAX_LIMIT);
static volatile uint8_t  s_isr_half;         /* buffer half completed most recently */
static volatile uint32_t s_completed_halves; /* total HT/TC completions */
static volatile uint32_t s_ovr_count;        /* I2S3ext SR OVR (codec FIFO overrun) */
static volatile uint32_t s_dma_err_count;    /* DMA TEIF/DMEIF/FEIF */

/* Thread-side state */
static volatile bool s_rx_started;
static uint32_t s_consumed_halves;
static uint32_t s_half_missed;   /* DMA lapped the consumer (service overrun) */
static uint32_t s_rx_underruns;  /* no half-buffer for 100 ms (RX stalled) */
static uint32_t s_halves_serviced;
static uint32_t s_record_pos;
static uint32_t s_ring_head;
static uint32_t s_ring_count;
static uint32_t s_ring_overwrites;

/* Live mic level (running ~1 s window, sampled in consume_latest_half and
 * snapshotted by the capture thread; proves the onboard electret is alive
 * and is the seed of the future cry/VAD feature path). */
static uint64_t s_lvl_sum_sq;
static uint64_t s_lvl_sum_sq_r; /* right slot: channel-swap diagnostic */
static uint32_t s_lvl_count;
static int16_t  s_lvl_peak;
static uint32_t s_level_rms = 1; /* 1 avoids /0 in a log scale */
static uint32_t s_level_rms_r = 1;
static int16_t  s_level_peak;

/* --------------------------------------------------------------- RX hardware */
static void rx_gpio_init(void)
{
    /* De-JTAG PB4: it defaults to NJTRST (AF0). On the STM32F407 the
     * I2S3ext_SD function lives on **AF7** (also AF5) -- AF6 on this family
     * is I2S2ext_SD/SAI1! The F42x/43x tables that name PB4 AF6 as
     * I2S3ext_SD do NOT apply to the F405/407; muxing PB4 to AF6 silently
     * feeds the I2S3_ext receiver a disconnected internal line (all-zero
     * captures). stm32f4xx_hal_gpio_ex.h: GPIO_AF7_I2S3ext = 0x07. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    (void)RCC->AHB1ENR; /* settle after clock enable */

    GPIOB->MODER = (GPIOB->MODER & ~(3U << (4 * 2))) | (2U << (4 * 2));     /* AF mode */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFU << (4 * 4))) | (7U << (4 * 4)); /* AF7 */
    GPIOB->OSPEEDR |= (3U << (4 * 2));                                      /* high speed */
    GPIOB->PUPDR &= ~(3U << (4 * 2));                                       /* no pull */
}

static void dma1_stream2_isr(const void *arg)
{
    ARG_UNUSED(arg);
    uint32_t status = DMA1->LISR; /* Stream 2 = LOW stream: flags in LISR */
    uint32_t clear = 0;

    if (status & DMA_LISR_HTIF2) {
        s_isr_half = 0u; /* first half (words 0..255) just filled */
        s_completed_halves++;
        k_sem_give(&s_half_sem);
        clear |= DMA_LIFCR_CHTIF2;
    }
    if (status & DMA_LISR_TCIF2) {
        s_isr_half = 1u; /* second half (words 256..511) just filled */
        s_completed_halves++;
        k_sem_give(&s_half_sem);
        clear |= DMA_LIFCR_CTCIF2;
    }
    if (status & (DMA_LISR_TEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_FEIF2)) {
        s_dma_err_count++;
        clear |= DMA_LIFCR_CTEIF2 | DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CFEIF2;
    }
    if (clear != 0u) {
        DMA1->LIFCR = clear;
    }

    /* Codec-side ADC FIFO overrun (data already lost): clear via SR read
     * followed by DR read (RM0090 OVR sequence). */
    if ((I2S3ext->SR & SPI_SR_OVR) != 0u) {
        s_ovr_count++;
        (void)I2S3ext->SR;
        (void)I2S3ext->DR;
    }
}

static void rx_dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN; /* already on from TX init; idempotent */
    (void)RCC->AHB1ENR;

    /* Disable + drain before (re)configuring the stream */
    DMA1_Stream2->CR &= ~DMA_SxCR_EN;
    while ((DMA1_Stream2->CR & DMA_SxCR_EN) != 0u) { }
    DMA1->LIFCR = DMA_LIFCR_CFEIF2 | DMA_LIFCR_CDMEIF2 |
                  DMA_LIFCR_CTEIF2 | DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTCIF2;

    DMA1_Stream2->PAR  = (uint32_t)&I2S3ext->DR;
    DMA1_Stream2->M0AR = (uint32_t)s_rx_dma_buf;
    DMA1_Stream2->NDTR = AUDIO_RX_DMA_WORDS;
    DMA1_Stream2->FCR  = 0;
    DMA1_Stream2->CR   = DMA_SxCR_CHSEL_1 |   /* Channel 2: I2S3_EXT_RX (AN4031) */
                         DMA_SxCR_PL_1 |      /* high priority */
                         DMA_SxCR_MINC |
                         DMA_SxCR_CIRC |
                         DMA_SxCR_PSIZE_0 |   /* peripheral: 16-bit */
                         DMA_SxCR_MSIZE_0 |   /* memory:     16-bit */
                         DMA_SxCR_HTIE |
                         DMA_SxCR_TCIE;       /* DIR=00: peripheral-to-memory */

    IRQ_CONNECT(DMA1_Stream2_IRQn, 0, dma1_stream2_isr, NULL, 0);
    irq_enable(DMA1_Stream2_IRQn);
}

/* -------------------------------------------------------------- ring buffer */
static void ring_push(int16_t sample)
{
    s_ring_buf[s_ring_head] = sample;
    s_ring_head++;
    if (s_ring_head >= AUDIO_RING_SAMPLES) {
        s_ring_head = 0;
    }
    if (s_ring_count < AUDIO_RING_SAMPLES) {
        s_ring_count++;
    } else {
        s_ring_overwrites++; /* full: oldest slot was overwritten */
    }
}

/* Take the L word of each L,R pair in the half the DMA just filled
 * (differential mic => L == R) into the mono ring + demo record buffer. */
static void consume_latest_half(void)
{
    const uint32_t completed = s_completed_halves;
    const uint32_t avail = completed - s_consumed_halves;
    if (avail == 0u) {
        return; /* stale wakeup token (burst drain), nothing new to read */
    }
    if (avail > 1u) {
        s_half_missed += (avail - 1u); /* DMA lapped the consumer */
    }
    s_consumed_halves = completed;

    const uint32_t offset = (s_isr_half != 0u) ? AUDIO_RX_HALF_WORDS : 0u;
    for (uint32_t i = 0; i < AUDIO_RX_HALF_WORDS; i += 2u) {
        const int16_t left = (int16_t)s_rx_dma_buf[offset + i];
        /* The differential mic signal lands in the R slot on this board
         * (L rms ~66 vs R rms ~440 with the ALC running): take R as mono. */
        const int16_t right = (int16_t)s_rx_dma_buf[offset + i + 1];
        const int16_t mag = (right < 0) ? (int16_t)(-right) : right;

        s_lvl_sum_sq += (uint64_t)((int32_t)left * left);
        s_lvl_sum_sq_r += (uint64_t)((int32_t)right * right);
        if (mag > s_lvl_peak) {
            s_lvl_peak = mag;
        }
        s_lvl_count++;

        ring_push(right);
        if (s_record_pos < AUDIO_RECORD_SAMPLES) {
            s_record_buf[s_record_pos++] = right;
        }
    }
    s_halves_serviced++;

    /* Snapshot the finished ~1 s window (single-writer thread). */
    if (s_lvl_count >= AUDIO_FS_HZ) {
        s_level_rms = (uint32_t)sqrtf((double)s_lvl_sum_sq /
                                      (double)s_lvl_count);
        s_level_rms_r = (uint32_t)sqrtf((double)s_lvl_sum_sq_r /
                                        (double)s_lvl_count);
        s_level_peak = s_lvl_peak;
        s_lvl_sum_sq = 0;
        s_lvl_sum_sq_r = 0;
        s_lvl_count = 0;
        s_lvl_peak = 0;
    }
}

void audio_capture_get_level(uint32_t *rms, int16_t *peak)
{
    if (rms != NULL) {
        *rms = s_level_rms;
    }
    if (peak != NULL) {
        *peak = s_level_peak;
    }
}

/* ------------------------------------------------------------ loopback demo */
static void run_loopback_demo(void)
{
    /* One-shot demo ~5 s after boot (main.c's s_hw_ready is file-local, so
     * readiness is signaled through audio_capture_start() + this delay). */
    k_sleep(K_SECONDS(5));

    /* Flush the pre-demo backlog: RX has been running since init, so the
     * semaphore holds stale tokens (~5 s of old audio). Drain them, resync
     * the half tracking, and zero the miss counter so the 2 s recording
     * starts on FRESH audio and the stats stay honest. */
    while (k_sem_take(&s_half_sem, K_NO_WAIT) == 0) {
    }
    s_consumed_halves = s_completed_halves;
    s_half_missed = 0U;

    /* Record 2 s of mono audio */
    s_record_pos = 0;
    printk("[AUDIO] demo: recording 2 s ...\n");
    while (s_record_pos < AUDIO_RECORD_SAMPLES) {
        if (k_sem_take(&s_half_sem, K_MSEC(500)) != 0) {
            printk("[AUDIO] demo: RX stalled, aborting record (%u samples)\n",
                   (unsigned)s_record_pos);
            return;
        }
        consume_latest_half();
    }
    printk("[AUDIO] demo: captured %u samples, ovr=%u, missed_halves=%u\n",
           (unsigned)s_record_pos, (unsigned)s_ovr_count, (unsigned)s_half_missed);

    /* Play it back: duplicate each mono sample into an L,R word pair and feed
     * the I2S3 TX staging path through the codec driver's PCM API. */
    printk("[AUDIO] demo: playing back ...\n");
    audio_pcm_start();
    uint32_t sent = 0;
    while (sent < s_record_pos) {
        uint32_t n = s_record_pos - sent;
        if (n > AUDIO_CHUNK_MONO) {
            n = AUDIO_CHUNK_MONO;
        }
        for (uint32_t i = 0; i < n; i++) {
            s_stereo_chunk[2u * i]       = s_record_buf[sent + i]; /* L */
            s_stereo_chunk[2u * i + 1u]  = s_record_buf[sent + i]; /* R */
        }
        const uint32_t need = 2u * n;
        uint32_t fed = 0;
        while (fed < need) {
            /* keep live capture serviced while waiting for TX FIFO space */
            if (k_sem_take(&s_half_sem, K_NO_WAIT) == 0) {
                consume_latest_half();
            }
            fed += audio_pcm_write(&s_stereo_chunk[fed], need - fed);
            if (fed < need) {
                k_msleep(1); /* staging FIFO full: wait for DMA drain */
            }
        }
        sent += n;
    }
    while (audio_pcm_buffered() > 0u) {
        if (k_sem_take(&s_half_sem, K_NO_WAIT) == 0) {
            consume_latest_half();
        }
        k_msleep(1);
    }
    audio_pcm_stop();
    printk("[AUDIO] demo: playback done, underruns=%u\n", (unsigned)s_rx_underruns);

    /* Mute + power down the DAC outputs after the demo: a monitor must not
     * radiate audio into its own microphone (headphone-jack-to-mic loop). */
    es8388_dac_mute_powerdown();
}

/* ------------------------------------------------------------------- thread */
static void audio_capture_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Park until main() has run audio_capture_start() (same gating pattern as
     * main.c's wait_hw_ready, which is file-local there). */
    while (!s_rx_started) {
        k_sleep(K_MSEC(10));
    }

    run_loopback_demo();

    /* Continuous capture into the mono ring buffer for the future ML task.
     * RX underrun (consumer starved by the producer) cannot happen with a
     * blocking take; a take timeout means the RX path stopped producing. */
    uint32_t next_stat = k_uptime_get_32() + AUDIO_STAT_PERIOD_MS;
    uint32_t next_level = k_uptime_get_32() + AUDIO_LEVEL_PERIOD_MS;
    for (;;) {
        if (k_sem_take(&s_half_sem, K_MSEC(100)) == 0) {
            consume_latest_half();
        } else {
            s_rx_underruns++;
        }

        const uint32_t now = k_uptime_get_32();
        if ((int32_t)(now - next_level) >= 0) {
            printk("[AUDIO] mic L rms=%-6u R rms=%-6u peak=%-6d\n",
                   (unsigned)s_level_rms, (unsigned)s_level_rms_r,
                   (int)s_level_peak);
            next_level = now + AUDIO_LEVEL_PERIOD_MS;
        }
        if ((int32_t)(now - next_stat) >= 0) {
            printk("[AUDIO] ring: %u s buffered, ovr=%u\n",
                   (unsigned)(s_ring_count / AUDIO_FS_HZ), (unsigned)s_ovr_count);
            next_stat = now + AUDIO_STAT_PERIOD_MS;
        }
    }
}

/* Static thread definition (house style: K_THREAD_DEFINE, prio 5, 2048 stack) */
K_THREAD_DEFINE(audio_capture_thread, 2048, audio_capture_thread_fn,
                NULL, NULL, NULL, 5, 0, 0);

/* ---------------------------------------------------------------- public API */
void audio_capture_init(void)
{
    /* Codec + I2S3 master TX + TX DMA engine (sets the shared 16 kHz frame
     * plan, ES8388 ADC/DAC register config incl. PGA and digital unmute). */
    audio_hardware_dac_init();
    es8388_adc_debug_readback();

    rx_gpio_init();
    printk("[AUDIO] PB4 de-JTAG'd to I2S3_ext_SD (AF7, high speed, no pull)\n");

    /* Synchronized full-duplex startup per stm32f4xx_hal_i2s_ex.c
     * (HAL_I2SEx_TransmitReceive_DMA, lines 455 & 660-666):
     *   1. Pause Master SPI3 (I2SE=0) while configuring the slave receiver so
     *      I2S3ext does not start mid-word with a bit-shift offset.
     *   2. Configure I2S3ext as 16-bit Philips Slave RX (I2SCFG=01b) with
     *      I2SPR=2U (HAL convention) and RXDMAEN.
     *   3. Enable RX DMA Stream 2, then enable I2S3ext FIRST, then enable
     *      Master SPI3 SECOND so both blocks lock onto bit 0 of frame 0. */
    SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;

    I2S3ext->I2SCFGR = 0;
    I2S3ext->I2SPR = 2U;
    I2S3ext->I2SCFGR = SPI_I2SCFGR_I2SMOD |   /* I2S mode */
                       SPI_I2SCFGR_I2SCFG_0;  /* 01b: Slave Receive */
    (void)I2S3ext->DR;
    (void)I2S3ext->SR;

    rx_dma_init();
    I2S3ext->CR2 |= SPI_CR2_RXDMAEN;
    DMA1_Stream2->CR |= DMA_SxCR_EN;

    I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;     /* Slave ext enabled FIRST */
    SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;        /* Master I2S3 enabled SECOND */

    printk("[AUDIO] I2S3ext slave RX armed (DMA1 S2 Ch2 circular, 512-word HT/TC)\n");
}

void audio_capture_start(void)
{
    if (s_rx_started) {
        return; /* idempotent */
    }
    /* Defensive re-enable in case a future caller re-inits between the two */
    if ((DMA1_Stream2->CR & DMA_SxCR_EN) == 0u) {
        DMA1_Stream2->CR |= DMA_SxCR_EN;
    }
    if ((I2S3ext->I2SCFGR & SPI_I2SCFGR_I2SE) == 0u) {
        I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;
    }
    s_rx_started = true; /* releases the capture thread */
    printk("[AUDIO] capture path live (16 kHz mono via I2S3ext RX on PB4)\n");
}
