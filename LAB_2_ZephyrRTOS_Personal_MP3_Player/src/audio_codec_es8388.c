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

static audio_waveform_t s_current_waveform = AUDIO_WAVE_SINE;
static volatile uint16_t s_dac_scaled_table[32];
static volatile bool s_dac_active = false;
static volatile uint8_t s_current_volume = 70;
static volatile uint32_t s_phase_acc = 0;
static volatile uint32_t s_phase_inc = 0;
static volatile bool s_channel_toggle = false;
static volatile uint16_t s_tone_gain = 0;
static volatile uint16_t s_tone_target_gain = 0;
static volatile uint16_t s_output_gain = 0;
static volatile int16_t s_last_pcm = 0;

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

/* -------------------------------------------------------------------------- */
/* Hardware Circular DMA Audio Streamer (DMA1 Stream 5 Channel 0 for SPI3_TX) */
/* -------------------------------------------------------------------------- */
#define I2S_DMA_BUFFER_SIZE 1024
static int16_t s_i2s_dma_buf[I2S_DMA_BUFFER_SIZE];

static void populate_audio_block(int16_t *dest, size_t count)
{
    bool wav_active = wav_player_is_active();
    bool tone_active = (s_phase_inc > 0 || s_tone_gain > s_tone_target_gain);
    uint16_t output_target = (wav_active || tone_active) ? 256U : 0U;

    for (size_t i = 0; i < count; i++) {
        if (s_output_gain < output_target) {
            s_output_gain = (s_output_gain + 8U > output_target) ?
                            output_target : s_output_gain + 8U;
        } else if (s_output_gain > output_target) {
            s_output_gain = (s_output_gain < output_target + 8U) ?
                            output_target : s_output_gain - 8U;
        }

        if (!s_dac_active) {
            dest[i] = 0;
            if (s_channel_toggle) {
                DAC->DHR12R1 = 2048; /* Mid-rail bias */
            }
            s_channel_toggle = !s_channel_toggle;
        } else if (wav_active) {
            int16_t out_sample;
            if (wav_player_get_next_sample(&out_sample)) {
                int16_t pcm = (int16_t)(((int32_t)out_sample * s_output_gain) / 256);
                s_last_pcm = pcm;
                dest[i] = pcm;

                if (s_channel_toggle) {
                    int32_t val12 = (int32_t)pcm / 16 + 2048;
                    if (val12 < 0) val12 = 0;
                    if (val12 > 4095) val12 = 4095;
                    DAC->DHR12R1 = (uint16_t)val12;
                }
                s_channel_toggle = !s_channel_toggle;
                s_diag.i2s_tx_samples++;
            } else {
                int16_t pcm = (int16_t)(((int32_t)s_last_pcm * s_output_gain) / 256);
                dest[i] = pcm;
                if (s_channel_toggle) {
                    DAC->DHR12R1 = (int32_t)pcm / 16 + 2048;
                }
                s_channel_toggle = !s_channel_toggle;
            }
        } else if (s_phase_inc > 0 || s_tone_gain > s_tone_target_gain) {
            if (s_tone_gain < s_tone_target_gain) {
                s_tone_gain = (s_tone_gain + 4U > s_tone_target_gain) ?
                              s_tone_target_gain : s_tone_gain + 4U;
            } else if (s_tone_gain > s_tone_target_gain) {
                s_tone_gain = (s_tone_gain < s_tone_target_gain + 4U) ?
                              s_tone_target_gain : s_tone_gain - 4U;
            }

            uint8_t table_index = (uint8_t)((s_phase_acc >> 27) & 31U);
            uint8_t next_index = (uint8_t)((table_index + 1U) & 31U);
            uint32_t fraction = (s_phase_acc >> 19) & 0xFFU;
            int32_t sample_delta = (int32_t)s_dac_scaled_table[next_index] -
                                   (int32_t)s_dac_scaled_table[table_index];
            uint16_t sample = (uint16_t)((int32_t)s_dac_scaled_table[table_index] +
                                         ((sample_delta * (int32_t)fraction) >> 8));

            int32_t centered = ((int32_t)sample - 2048) * s_tone_gain / 256;
            int16_t pcm = (int16_t)(centered * 15);
            pcm = (int16_t)(((int32_t)pcm * s_output_gain) / 256);
            s_last_pcm = pcm;
            dest[i] = pcm;

            if (s_channel_toggle) {
                DAC->DHR12R1 = (uint16_t)(centered + 2048);
                s_phase_acc += s_phase_inc;
            }
            s_channel_toggle = !s_channel_toggle;
            s_diag.i2s_tx_samples++;
        } else {
            int16_t pcm = (int16_t)(((int32_t)s_last_pcm * s_output_gain) / 256);
            dest[i] = pcm;
            if (s_channel_toggle) {
                DAC->DHR12R1 = (int32_t)pcm / 16 + 2048;
            }
            s_channel_toggle = !s_channel_toggle;
        }
    }
}

static void dma1_stream5_isr(const void *arg)
{
    ARG_UNUSED(arg);

    if (DMA1->HISR & DMA_HISR_HTIF5) {
        DMA1->HIFCR = DMA_HIFCR_CHTIF5;
        populate_audio_block(&s_i2s_dma_buf[0], I2S_DMA_BUFFER_SIZE / 2);
    }
    if (DMA1->HISR & DMA_HISR_TCIF5) {
        DMA1->HIFCR = DMA_HIFCR_CTCIF5;
        populate_audio_block(&s_i2s_dma_buf[I2S_DMA_BUFFER_SIZE / 2], I2S_DMA_BUFFER_SIZE / 2);
    }
    if (DMA1->HISR & DMA_HISR_TEIF5) {
        DMA1->HIFCR = DMA_HIFCR_CTEIF5;
    }
    if (DMA1->HISR & DMA_HISR_FEIF5) {
        DMA1->HIFCR = DMA_HIFCR_CFEIF5;
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

    /* Headphone volume: LOUT1 / ROUT1 (0 dB clean, unclipped output) */
    es8388_reg_write(0x2E, 0x1E); /* DACCONTROL24: LOUT1VOL (0dB) */
    es8388_reg_write(0x2F, 0x1E); /* DACCONTROL25: ROUT1VOL (0dB) */

    /* Start State Machine */
    es8388_reg_write(0x02, 0xF0); /* CHIPPOWER: reset state machine */
    k_msleep(5);
    es8388_reg_write(0x02, 0x00); /* CHIPPOWER: start state machine */
    k_msleep(5);

    /* Keep the codec muted until the STM32 I2S peripheral is running. */
    es8388_reg_write(0x04, 0x3C); /* DACPOWER */
    es8388_reg_write(0x19, 0x04); /* DACCONTROL3: mute */
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

    /* 2. Configure PLLI2S for 44.1 kHz audio with PLLM = 4 (8 MHz HSE / 4 = 2 MHz input):
     *    VCO = 2 MHz * 192 = 384 MHz (well within 192..432 MHz operating envelope)
     *    I2SxCLK = 384 MHz / 2 = 192 MHz
     *    I2SDIV = 8, ODD = 1 -> Total Div = 17
     *    Fs = 192 MHz / (256 * 17) = 44117.6 Hz (44.1 kHz exact target)
     */
    RCC->CR &= ~RCC_CR_PLLI2SON;
    RCC->PLLI2SCFGR = (192U << RCC_PLLI2SCFGR_PLLI2SN_Pos) | (2U << RCC_PLLI2SCFGR_PLLI2SR_Pos);
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
     *    MCKOE = 1, I2SDIV = 8, ODD = 1 -> Fs = 44.117 kHz
     */
    SPI3->I2SCFGR = 0;
    SPI3->I2SPR = SPI_I2SPR_MCKOE | SPI_I2SPR_ODD | 8U;
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD |   /* I2S mode */
                    SPI_I2SCFGR_I2SCFG_1;  /* Master Transmit (10b) */

    /* 5. Configure DMA1 Stream 5 Channel 0 for Hardware Circular SPI3_TX */
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;

    DMA1_Stream5->CR = 0;
    while (DMA1_Stream5->CR & DMA_SxCR_EN);

    /* Clear DMA1 Stream 5 interrupt flags */
    DMA1->HIFCR = DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5 |
                  DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;

    DMA1_Stream5->PAR = (uint32_t)&(SPI3->DR);
    DMA1_Stream5->M0AR = (uint32_t)s_i2s_dma_buf;
    DMA1_Stream5->NDTR = I2S_DMA_BUFFER_SIZE;
    DMA1_Stream5->FCR = 0;

    DMA1_Stream5->CR = (0U << DMA_SxCR_CHSEL_Pos) |     /* Channel 0: SPI3_TX */
                       DMA_SxCR_PL_1 | DMA_SxCR_PL_0 |   /* Very High Priority */
                       DMA_SxCR_MSIZE_0 |                /* 16-bit Memory */
                       DMA_SxCR_PSIZE_0 |                /* 16-bit Peripheral */
                       DMA_SxCR_MINC |                   /* Memory increment */
                       DMA_SxCR_CIRC |                   /* Circular double buffer */
                       DMA_SxCR_DIR_0 |                  /* Memory to Peripheral */
                       DMA_SxCR_TCIE |                   /* Transfer Complete Interrupt */
                       DMA_SxCR_HTIE;                    /* Half Transfer Interrupt */

    /* Pre-fill initial silence */
    memset(s_i2s_dma_buf, 0, sizeof(s_i2s_dma_buf));

    /* Connect and enable DMA1 Stream 5 interrupt in Zephyr */
    IRQ_CONNECT(DMA1_Stream5_IRQn, 0, dma1_stream5_isr, NULL, 0);
    irq_enable(DMA1_Stream5_IRQn);

    /* Enable DMA request on SPI3 and start stream */
    SPI3->CR2 |= SPI_CR2_TXDMAEN;
    DMA1_Stream5->CR |= DMA_SxCR_EN;
    SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;     /* Enable I2S peripheral */
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
    k_msleep(2);
    es8388_reg_write(0x19, 0x00); /* Unmute only after stable I2S clocks */

    /* Initialize scaled sine table */
    audio_hardware_dac_set_volume(s_current_volume);

    printk("[Audio_DAC] Dual-Output Audio Synthesizer (PA4 + 3.5mm Jack CN3) active.\n");
}

static const uint16_t* get_waveform_raw_table(audio_waveform_t wave)
{
    ARG_UNUSED(wave);
    return SINE_32;
}

void audio_hardware_dac_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_current_volume = volume_percent;

    const uint16_t *raw_table = get_waveform_raw_table(s_current_waveform);

    /* Recompute scaled 32-sample table */
    unsigned int irq_key = irq_lock();
    for (int i = 0; i < 32; i++) {
        int32_t centered = (int32_t)raw_table[i] - 2048;
        int32_t scaled = (centered * (int32_t)volume_percent) / 100;
        s_dac_scaled_table[i] = (uint16_t)(scaled + 2048);
    }
    irq_unlock(irq_key);

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
    s_current_waveform = AUDIO_WAVE_SINE;
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
    s_tone_target_gain = 256;
    s_diag.dac_active = true;
}

void audio_hardware_dac_stop(void)
{
    /* Fade the current tone instead of abruptly cutting the I2S waveform. */
    s_dac_active = true;
    s_diag.dac_active = false;
    s_phase_inc = 0;
    s_tone_target_gain = 0;
    s_channel_toggle = false;
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
