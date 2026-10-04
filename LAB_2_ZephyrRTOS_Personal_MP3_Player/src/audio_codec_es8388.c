/**
 * @file audio_codec_es8388.c
 * @brief High-fidelity audio synthesizer supporting:
 *        1. On-board Everest Semiconductor ES8388 Stereo Codec via I2C2 (PF0/PF1)
 *           and I2S3 (PC7: MCK, PA15: WS, PB3: CK, PB5: SD) driving the 3.5mm Headphone Jack (CN3).
 *        2. STM32F407 12-bit Analog DAC on Pin PA4.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "audio_codec_es8388.h"
#include "wav_player.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>

/* -------------------------------------------------------------------------- */
/* 32-Point Waveform Tables for Audio Synthesis (12-bit, 0 to 4095)           */
/* -------------------------------------------------------------------------- */
static const uint16_t SINE_32[32] = {
    2048, 2447, 2831, 3185, 3495, 3750, 3939, 4056,
    4095, 4056, 3939, 3750, 3495, 3185, 2831, 2447,
    2048, 1648, 1264,  910,  600,  345,  156,   39,
       0,   39,  156,  345,  600,  910, 1264, 1648
};

static const uint16_t TRIANGLE_32[32] = {
       0,  264,  528,  792, 1056, 1320, 1585, 1849,
    2113, 2377, 2641, 2906, 3170, 3434, 3698, 3962,
    3962, 3698, 3434, 3170, 2906, 2641, 2377, 2113,
    1849, 1585, 1320, 1056,  792,  528,  264,    0
};

static const uint16_t SAWTOOTH_32[32] = {
       0,  132,  264,  396,  528,  660,  792,  924,
    1056, 1188, 1320, 1453, 1585, 1717, 1849, 1981,
    2113, 2245, 2377, 2509, 2641, 2774, 2906, 3038,
    3170, 3302, 3434, 3566, 3698, 3830, 3962, 4095
};

static const uint16_t SQUARE_32[32] = {
    4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095,
    4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095,
       0,    0,    0,    0,    0,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0
};

static audio_waveform_t s_current_waveform = AUDIO_WAVE_SINE;
static volatile uint16_t s_dac_scaled_table[32];
static volatile bool s_dac_active = false;
static volatile uint8_t s_current_volume = 70;
static volatile uint32_t s_phase_acc = 0;
static volatile uint32_t s_phase_inc = 0;
static volatile bool s_channel_toggle = false;

/* Hardware live diagnostic records */
static audio_diagnostics_t s_diag = {
    .es_detected = false,
    .es_addr = 0x10,
    .reg04_readback = 0xFF,
    .reg04_verified = false,
    .pll_locked = false,
    .i2s_tx_samples = 0,
    .dac_active = false
};

static char s_diag_str[64] = "ES8388:PROBING";

#define AUDIO_DMA_WORDS 512
#define AUDIO_HALF_WORDS (AUDIO_DMA_WORDS / 2)
static uint16_t s_audio_dma[AUDIO_DMA_WORDS];
static uint16_t s_stage[2][AUDIO_HALF_WORDS];
static volatile uint8_t s_stage_ready[2];
static volatile uint32_t s_stage_misses;
static volatile bool s_dma_channel_toggle;

static uint16_t audio_next_word(void)
{
    if (wav_player_is_active()) {
        int16_t sample;
        if (wav_player_get_next_sample(&sample)) {
            int32_t value = (int32_t)sample / 16 + 2048;
            if (value < 0) value = 0;
            if (value > 4095) value = 4095;
            if (s_dma_channel_toggle) DAC->DHR12R1 = (uint16_t)value;
            s_dma_channel_toggle = !s_dma_channel_toggle;
            s_diag.i2s_tx_samples++;
            return (uint16_t)sample;
        }
        return 0;
    }

    if (!s_dac_active || s_phase_inc == 0) {
        DAC->DHR12R1 = 2048;
        return 0;
    }

    uint16_t sample = s_dac_scaled_table[(s_phase_acc >> 27) & 31];
    int16_t pcm = (int16_t)(((int32_t)sample - 2048) * 15);
    DAC->DHR12R1 = sample;
    if (s_dma_channel_toggle) s_phase_acc += s_phase_inc;
    s_dma_channel_toggle = !s_dma_channel_toggle;
    s_diag.i2s_tx_samples++;
    return (uint16_t)pcm;
}

static void audio_dma_fill(uint16_t offset, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        s_audio_dma[offset + i] = audio_next_word();
    }
}

/* Background producer: generates PCM into staging halves so the DMA ISR only
 * performs a short memory copy. This keeps the audio interrupt brief enough
 * that the interrupt-driven SDMMC driver can service its FIFO without overrun. */
static void audio_producer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        for (int half = 0; half < 2; half++) {
            if (s_stage_ready[half]) {
                continue;
            }
            for (uint16_t i = 0; i < AUDIO_HALF_WORDS; i++) {
                s_stage[half][i] = audio_next_word();
            }
            s_stage_ready[half] = 1;
        }
        k_msleep(1);
    }
}
/* Must outrank the MP3 reader so a long decode cannot starve the DMA staging buffers. */
K_THREAD_DEFINE(audio_producer, 2048, audio_producer_thread, NULL, NULL, NULL, 1, 0, 0);

static void dma1_stream5_isr(const void *arg)
{
    ARG_UNUSED(arg);
    uint32_t status = DMA1->HISR;
    uint32_t clear = 0;

    if (status & DMA_HISR_HTIF5) {
        if (s_stage_ready[0]) {
            memcpy(&s_audio_dma[0], s_stage[0], sizeof(s_stage[0]));
            s_stage_ready[0] = 0;
        } else {
            s_stage_misses++;
        }
        clear |= DMA_HIFCR_CHTIF5;
    }
    if (status & DMA_HISR_TCIF5) {
        if (s_stage_ready[1]) {
            memcpy(&s_audio_dma[AUDIO_HALF_WORDS], s_stage[1], sizeof(s_stage[1]));
            s_stage_ready[1] = 0;
        } else {
            s_stage_misses++;
        }
        clear |= DMA_HIFCR_CTCIF5;
    }
    if (status & (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)) {
        clear |= DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;
    }
    if (clear != 0) DMA1->HIFCR = clear;
}

uint32_t audio_stage_misses(void)
{
    return s_stage_misses;
}

static void audio_dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    DMA1_Stream5->CR &= ~DMA_SxCR_EN;
    while (DMA1_Stream5->CR & DMA_SxCR_EN) { }
    DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 |
                  DMA_HIFCR_CTEIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;

    audio_dma_fill(0, AUDIO_DMA_WORDS);
    DMA1_Stream5->PAR = (uint32_t)&SPI3->DR;
    DMA1_Stream5->M0AR = (uint32_t)s_audio_dma;
    DMA1_Stream5->NDTR = AUDIO_DMA_WORDS;
    DMA1_Stream5->FCR = 0;
    DMA1_Stream5->CR = DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       DMA_SxCR_DIR_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0 |
                       DMA_SxCR_HTIE | DMA_SxCR_TCIE;
    IRQ_CONNECT(DMA1_Stream5_IRQn, 0, dma1_stream5_isr, NULL, 0);
    irq_enable(DMA1_Stream5_IRQn);
    SPI3->CR2 |= SPI_CR2_TXDMAEN;
    DMA1_Stream5->CR |= DMA_SxCR_EN;
}

/* -------------------------------------------------------------------------- */
/* Direct Digital Synthesis (DDS) Interrupt via SPI3 / I2S3 TXE               */
/* -------------------------------------------------------------------------- */
static void spi3_i2s_isr(const void *arg)
{
    ARG_UNUSED(arg);

    if (SPI3->SR & SPI_SR_TXE) {
        if (wav_player_is_active()) {
            int16_t sample;
            if (wav_player_get_next_sample(&sample)) {
                SPI3->DR = (uint16_t)sample;
                int32_t value = (int32_t)sample / 16 + 2048;
                if (value < 0) value = 0;
                if (value > 4095) value = 4095;
                if (s_channel_toggle) DAC->DHR12R1 = (uint16_t)value;
                s_channel_toggle = !s_channel_toggle;
                s_diag.i2s_tx_samples++;
            } else {
                SPI3->DR = 0;
                DAC->DHR12R1 = 2048;
            }
        } else if (!s_dac_active || s_phase_inc == 0) {
            SPI3->DR = 0;
            DAC->DHR12R1 = 2048; /* Mid-rail bias */
        } else {
            /* 12-bit sine sample (0 to 4095) */
            uint16_t sample = s_dac_scaled_table[(s_phase_acc >> 27) & 31];

            /* Convert to signed 16-bit PCM for ES8388 stereo DAC */
            int16_t pcm = (int16_t)(((int32_t)sample - 2048) * 15);
            SPI3->DR = (uint16_t)pcm;

            /* Simultaneous Analog DAC on PA4 */
            DAC->DHR12R1 = sample;

            if (s_channel_toggle) {
                s_phase_acc += s_phase_inc;
            }
            s_channel_toggle = !s_channel_toggle;
            s_diag.i2s_tx_samples++;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* I2C2 Hardware Routing for ES8388 on RT-Thread Spark Board                  */
/* PF1 = I2C2_SCL (Pin 11 of MCU)                                             */
/* PF0 = I2C2_SDA (Pin 10 of MCU)                                             */
/* -------------------------------------------------------------------------- */
static void i2c2_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    /* Configure PF1 (SCL) and PF0 (SDA) as Open-Drain Outputs with Pull-Ups */
    GPIOF->MODER = (GPIOF->MODER & ~((3U << (0 * 2)) | (3U << (1 * 2)))) |
                   ((1U << (0 * 2)) | (1U << (1 * 2))); /* Output mode */
    GPIOF->OTYPER |= (1U << 0) | (1U << 1);             /* Open-drain */
    GPIOF->OSPEEDR |= (3U << (0 * 2)) | (3U << (1 * 2)); /* High speed */
    GPIOF->PUPDR = (GPIOF->PUPDR & ~((3U << (0 * 2)) | (3U << (1 * 2)))) |
                   ((1U << (0 * 2)) | (1U << (1 * 2))); /* Pull-up */

    GPIOF->BSRR = (1U << 0) | (1U << 1);                /* Idle HIGH */
}

static inline void i2c_delay(void)
{
    /* ~4.5 us delay at 168 MHz for reliable 100 kHz standard-mode I2C */
    for (volatile int i = 0; i < 200; i++) {
        __NOP();
    }
}

static void i2c_start(void)
{
    GPIOF->BSRR = (1U << 0) | (1U << 1);
    i2c_delay();
    GPIOF->BSRR = (1U << (0 + 16)); /* SDA LOW while SCL is HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << (1 + 16)); /* SCL LOW */
    i2c_delay();
}

static void i2c_stop(void)
{
    GPIOF->BSRR = (1U << (0 + 16)); /* SDA LOW */
    i2c_delay();
    GPIOF->BSRR = (1U << 1);        /* SCL HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << 0);        /* SDA HIGH while SCL is HIGH */
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            GPIOF->BSRR = (1U << 0); /* SDA HIGH */
        } else {
            GPIOF->BSRR = (1U << (0 + 16)); /* SDA LOW */
        }
        i2c_delay();
        GPIOF->BSRR = (1U << 1); /* SCL HIGH */
        i2c_delay();
        GPIOF->BSRR = (1U << (1 + 16)); /* SCL LOW */
        i2c_delay();
    }

    /* Release SDA for ACK */
    GPIOF->BSRR = (1U << 0);
    i2c_delay();
    GPIOF->BSRR = (1U << 1); /* SCL HIGH for ACK sample */
    i2c_delay();
    bool ack = ((GPIOF->IDR & (1U << 0)) == 0); /* Active LOW */
    GPIOF->BSRR = (1U << (1 + 16)); /* SCL LOW */
    i2c_delay();
    return ack;
}

static uint8_t i2c_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    GPIOF->BSRR = (1U << 0); /* Release / Float SDA */

    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        GPIOF->BSRR = (1U << 1); /* SCL HIGH */
        i2c_delay();
        if (GPIOF->IDR & (1U << 0)) {
            byte |= (1U << i);
        }
        GPIOF->BSRR = (1U << (1 + 16)); /* SCL LOW */
        i2c_delay();
    }

    /* Send ACK (LOW) or NACK (HIGH) */
    if (send_ack) {
        GPIOF->BSRR = (1U << (0 + 16)); /* ACK: pull SDA low */
    } else {
        GPIOF->BSRR = (1U << 0);        /* NACK: float SDA high */
    }
    i2c_delay();
    GPIOF->BSRR = (1U << 1); /* SCL HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << (1 + 16)); /* SCL LOW */
    GPIOF->BSRR = (1U << 0); /* Float SDA */
    i2c_delay();

    return byte;
}

static bool es8388_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t write_addr = (uint8_t)(s_diag.es_addr << 1);
    i2c_start();
    bool ack_addr = i2c_write_byte(write_addr);
    bool ack_reg  = i2c_write_byte(reg);
    bool ack_val  = i2c_write_byte(val);
    i2c_stop();
    return (ack_addr && ack_reg && ack_val);
}

static uint8_t es8388_reg_read(uint8_t reg)
{
    uint8_t write_addr = (uint8_t)(s_diag.es_addr << 1);
    uint8_t read_addr  = (uint8_t)((s_diag.es_addr << 1) | 0x01);

    i2c_start();
    if (!i2c_write_byte(write_addr)) {
        i2c_stop();
        return 0xFF;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return 0xFF;
    }

    /* Repeated start */
    i2c_start();
    if (!i2c_write_byte(read_addr)) {
        i2c_stop();
        return 0xFF;
    }

    uint8_t val = i2c_read_byte(false); /* Send NACK to end read */
    i2c_stop();
    return val;
}

/* -------------------------------------------------------------------------- */
/* ES8388 Complete Codec Configuration Matching RT-Thread drv_es8388.c        */
/* -------------------------------------------------------------------------- */
static void es8388_codec_init(void)
{
    i2c2_gpio_init();
    k_msleep(20);

    /* 1. Probe I2C address: 0x10 (write 0x20) or 0x11 (write 0x22) */
    i2c_start();
    bool ack10 = i2c_write_byte(0x20);
    i2c_stop();
    k_msleep(10);

    i2c_start();
    bool ack11 = i2c_write_byte(0x22);
    i2c_stop();
    k_msleep(10);

    if (ack10) {
        s_diag.es_addr = 0x10;
        s_diag.es_detected = true;
        printk("[ES8388] Codec ACK on I2C address 0x10!\n");
    } else if (ack11) {
        s_diag.es_addr = 0x11;
        s_diag.es_detected = true;
        printk("[ES8388] Codec ACK on I2C address 0x11!\n");
    } else {
        s_diag.es_addr = 0x10;
        s_diag.es_detected = false;
        printk("[ES8388] Notice: NACK on both 0x10 and 0x11 (check I2C bus)\n");
    }

    /* 2. Full ES8388 register configuration from drv_es8388.c */
    es8388_reg_write(0x19, 0x04); /* DACCONTROL3: mute during setup */
    es8388_reg_write(0x01, 0x50); /* CONTROL2: chip power mgmt */
    es8388_reg_write(0x02, 0x00); /* CHIPPOWER: normal all, power up all */
    es8388_reg_write(0x08, 0x00); /* MASTERMODE: slave mode */

    /* DAC setup */
    es8388_reg_write(0x04, 0xC0); /* DACPOWER: disable DAC temporarily */
    es8388_reg_write(0x00, 0x12); /* CONTROL1: Play & Record mode */
    es8388_reg_write(0x17, 0x18); /* DACCONTROL1: 16-bit I2S format */
    es8388_reg_write(0x18, 0x02); /* DACCONTROL2: Single speed, ratio 256 */
    es8388_reg_write(0x26, 0x00); /* DACCONTROL16: audio on LIN1/RIN1 */
    es8388_reg_write(0x27, 0x9C); /* DACCONTROL17: L DAC to L mixer enable 0dB */
    es8388_reg_write(0x2A, 0x9C); /* DACCONTROL20: R DAC to R mixer enable 0dB */
    es8388_reg_write(0x2B, 0x80); /* DACCONTROL21: internal LRCK */
    es8388_reg_write(0x2D, 0x00); /* DACCONTROL23: vroi=0 */

    /* Digital volume: 0dB */
    es8388_reg_write(0x1A, 0x00); /* DACCONTROL4: L Digital Vol 0dB */
    es8388_reg_write(0x1B, 0x00); /* DACCONTROL5: R Digital Vol 0dB */
    es8388_reg_write(0x04, 0x3C); /* DACPOWER: Enable DAC and Lout/Rout */

    /* ADC setup: required for internal clock generation when reg 0x2B = 0x80 */
    es8388_reg_write(0x03, 0xFF); /* ADCPOWER: power down */
    es8388_reg_write(0x09, 0xBB); /* ADCCONTROL1: MIC PGA gain */
    es8388_reg_write(0x0A, 0x00); /* ADCCONTROL2: LINSEL/RINSEL */
    es8388_reg_write(0x0B, 0x02); /* ADCCONTROL3 */
    es8388_reg_write(0x0C, 0x0D); /* ADCCONTROL4: 16-bit I2S format */
    es8388_reg_write(0x0D, 0x02); /* ADCCONTROL5: single speed, ratio 256 */
    es8388_reg_write(0x10, 0x00); /* ADCCONTROL8: 0dB */
    es8388_reg_write(0x11, 0x00); /* ADCCONTROL9: 0dB */
    es8388_reg_write(0x03, 0x09); /* ADCPOWER: Power on ADC */

    /* Headphone volume: LOUT1 / ROUT1 (+3.0 dB output boost) */
    es8388_reg_write(0x2E, 0x21); /* DACCONTROL24: LOUT1VOL (3.5mm Left) */
    es8388_reg_write(0x2F, 0x21); /* DACCONTROL25: ROUT1VOL (3.5mm Right) */

    /* Start State Machine */
    es8388_reg_write(0x02, 0xF0); /* CHIPPOWER: reset state machine */
    k_msleep(5);
    es8388_reg_write(0x02, 0x00); /* CHIPPOWER: start state machine */
    k_msleep(5);

    /* Final un-mute */
    es8388_reg_write(0x04, 0x3C); /* DACPOWER */
    es8388_reg_write(0x19, 0x00); /* DACCONTROL3: UNMUTE! */
    k_msleep(10);

    /* 3. Read back register 0x04 (DACPOWER) to verify communication */
    s_diag.reg04_readback = es8388_reg_read(0x04);
    s_diag.reg04_verified = (s_diag.reg04_readback == 0x3C);

    if (s_diag.reg04_verified) {
        printk("[ES8388] Verified: Reg 0x04 readback = 0x3C (Codec fully operational)!\n");
    } else {
        printk("[ES8388] Diagnostic: Reg 0x04 readback = 0x%02X\n", s_diag.reg04_readback);
    }
}

/* -------------------------------------------------------------------------- */
/* I2S3 Configuration on STM32F407 (PC7: MCK, PA15: WS, PB3: CK, PB5: SD)   */
/* -------------------------------------------------------------------------- */
static void i2s3_hw_init(void)
{
    /* 1. Enable peripheral clocks */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;

    /* Explicitly route I2S clock source to PLLI2S (RCC->CFGR bit 23 = 0) */
    RCC->CFGR &= ~RCC_CFGR_I2SSRC;

    /* 2. Configure PLLI2S for 44.1 kHz audio:
     *    HSE (8 MHz) / 8 * 271 / 2 = 135.5 MHz I2SxCLK
     */
    RCC->CR &= ~RCC_CR_PLLI2SON;
    RCC->PLLI2SCFGR = (271U << RCC_PLLI2SCFGR_PLLI2SN_Pos) | (2U << RCC_PLLI2SCFGR_PLLI2SR_Pos);
    RCC->CR |= RCC_CR_PLLI2SON;
    uint32_t timeout = 100000;
    while (!(RCC->CR & RCC_CR_PLLI2SRDY) && --timeout);
    s_diag.pll_locked = ((RCC->CR & RCC_CR_PLLI2SRDY) != 0);

    /* 3. Configure GPIO Alternate Function 6 for I2S3 pins:
     *    PC7:  I2S3_MCK (AF6)
     *    PA15: I2S3_WS  (AF6)
     *    PB3:  I2S3_CK  (AF6)
     *    PB5:  I2S3_SD  (AF6)
     */
    /* PC7: MCK */
    GPIOC->MODER = (GPIOC->MODER & ~(3U << (7 * 2))) | (2U << (7 * 2));
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFU << (7 * 4))) | (6U << (7 * 4));
    GPIOC->OSPEEDR |= (3U << (7 * 2));

    /* PA15: WS */
    GPIOA->MODER = (GPIOA->MODER & ~(3U << (15 * 2))) | (2U << (15 * 2));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFU << ((15 - 8) * 4))) | (6U << ((15 - 8) * 4));
    GPIOA->OSPEEDR |= (3U << (15 * 2));

    /* PB3: CK and PB5: SD */
    GPIOB->MODER = (GPIOB->MODER & ~((3U << (3 * 2)) | (3U << (5 * 2)))) |
                   ((2U << (3 * 2)) | (2U << (5 * 2)));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFU << (3 * 4)) | (0xFU << (5 * 4)))) |
                    ((6U << (3 * 4)) | (6U << (5 * 4)));
    GPIOB->OSPEEDR |= ((3U << (3 * 2)) | (3U << (5 * 2)));

    /* 4. Configure SPI3 in I2S Philips Standard Master Transmit Mode:
     *    MCKOE = 1, I2SDIV = 6 -> Fs = 44.108 kHz
     */
    SPI3->I2SCFGR = 0;
    SPI3->I2SPR = SPI_I2SPR_MCKOE | 6U;
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD |   /* I2S mode */
                    SPI_I2SCFGR_I2SCFG_1;  /* Master Transmit (10b) */
    SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;     /* Enable I2S peripheral */
        audio_dma_init();
}

/* -------------------------------------------------------------------------- */
/* Internal 12-Bit Analog DAC1 Setup on PA4                                   */
/* -------------------------------------------------------------------------- */
static void dac1_pa4_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;

    /* PA4 to Analog Mode (00b pull, 11b moder) */
    GPIOA->MODER |= (3U << (4 * 2));
    GPIOA->PUPDR &= ~(3U << (4 * 2));

    /* Enable DAC Channel 1 (PA4) */
    DAC->CR |= DAC_CR_EN1;
    DAC->DHR12R1 = 2048; /* Midpoint 1.65V bias */
}

/* -------------------------------------------------------------------------- */
/* Public API Implementation                                                  */
/* -------------------------------------------------------------------------- */
void audio_hardware_dac_init(void)
{
    printk("[Audio_DAC] Initializing STM32 12-bit Analog DAC on PA4...\n");
    dac1_pa4_init();

    printk("[Audio_Codec] Initializing ES8388 (I2C2 on PF0/PF1) & I2S3 on CN3...\n");
    es8388_codec_init();
    i2s3_hw_init();

    /* Initialize scaled sine table */
    audio_hardware_dac_set_volume(s_current_volume);

    printk("[Audio_DAC] Dual-Output Audio Synthesizer (PA4 + 3.5mm Jack CN3) active.\n");
}

static const uint16_t* get_waveform_raw_table(audio_waveform_t wave)
{
    switch (wave) {
        case AUDIO_WAVE_TRIANGLE: return TRIANGLE_32;
        case AUDIO_WAVE_SAWTOOTH: return SAWTOOTH_32;
        case AUDIO_WAVE_SQUARE:   return SQUARE_32;
        case AUDIO_WAVE_SINE:
        default:                  return SINE_32;
    }
}

void audio_hardware_dac_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_current_volume = volume_percent;

    const uint16_t *raw_table = get_waveform_raw_table(s_current_waveform);

    /* Recompute scaled 32-sample table */
    for (int i = 0; i < 32; i++) {
        int32_t centered = (int32_t)raw_table[i] - 2048;
        int32_t scaled = (centered * (int32_t)volume_percent) / 100;
        s_dac_scaled_table[i] = (uint16_t)(scaled + 2048);
    }

    /* Update digital volume in ES8388 codec */
    uint8_t es_vol = (uint8_t)((100 - volume_percent) * 192 / 100);
    es8388_reg_write(0x1A, es_vol); /* DAC L */
    es8388_reg_write(0x1B, es_vol); /* DAC R */
}

void audio_hardware_dac_set_waveform(audio_waveform_t wave)
{
    if (wave >= AUDIO_WAVE_COUNT) {
        wave = AUDIO_WAVE_SINE;
    }
    s_current_waveform = wave;
    audio_hardware_dac_set_volume(s_current_volume);
}

audio_waveform_t audio_hardware_dac_get_waveform(void)
{
    return s_current_waveform;
}

audio_waveform_t audio_hardware_dac_cycle_waveform(void)
{
    s_current_waveform = (audio_waveform_t)((s_current_waveform + 1) % AUDIO_WAVE_COUNT);
    audio_hardware_dac_set_volume(s_current_volume);
    return s_current_waveform;
}

const char* audio_hardware_dac_get_waveform_name(void)
{
    switch (s_current_waveform) {
        case AUDIO_WAVE_TRIANGLE: return "TRIANGLE";
        case AUDIO_WAVE_SAWTOOTH: return "SAWTOOTH";
        case AUDIO_WAVE_SQUARE:   return "SQUARE";
        case AUDIO_WAVE_SINE:
        default:                  return "SINE";
    }
}

void audio_hardware_dac_set_tone(float note_period_ms, uint8_t volume_percent)
{
    if (note_period_ms <= 0.001f || volume_percent == 0) {
        audio_hardware_dac_stop();
        return;
    }

    /* Only update volume table if it changed */
    if (volume_percent != s_current_volume) {
        audio_hardware_dac_set_volume(volume_percent);
    }

    /* DDS phase increment for 44.1 kHz stereo sample rate:
     * f_target = 1000.0 / note_period_ms
     * phase_inc = (f_target / 44100) * 2^32 = 97391549.0 / note_period_ms
     */
    s_phase_inc = (uint32_t)(97391549.0f / note_period_ms + 0.5f);
    s_dac_active = true;
    s_diag.dac_active = true;
}

void audio_hardware_dac_stop(void)
{
    s_dac_active = false;
    s_diag.dac_active = false;
    s_phase_inc = 0;
    DAC->DHR12R1 = 2048;
}

const audio_diagnostics_t* audio_get_diagnostics(void)
{
    return &s_diag;
}

const char *audio_hardware_dac_status(void)
{
    snprintf(s_diag_str, sizeof(s_diag_str), "ES:%s R04:%02X W:%s",
             s_diag.es_detected ? (s_diag.es_addr == 0x10 ? "0x10" : "0x11") : "NACK",
             s_diag.reg04_readback,
             audio_hardware_dac_get_waveform_name());
    return s_diag_str;
}
