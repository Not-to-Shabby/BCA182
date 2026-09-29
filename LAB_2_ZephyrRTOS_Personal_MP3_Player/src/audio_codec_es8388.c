/**
 * @file audio_codec_es8388.c
 * @brief High-fidelity audio synthesizer supporting:
 *        1. STM32F407 12-bit Analog DAC on Pin PA4 (direct headphone/speaker output)
 *        2. On-board Everest Semiconductor ES8388 Stereo Codec via I2C2 + I2S3
 *           driving the 3.5mm Headphone Jack (CN3 / PJ-320A).
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "audio_codec_es8388.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>

/* -------------------------------------------------------------------------- */
/* 32-Point Pure Harmonic Sinusoidal Waveform Table (12-bit, 0 to 4095)       */
/* -------------------------------------------------------------------------- */
static const uint16_t SINE_32[32] = {
    2048, 2447, 2831, 3185, 3495, 3750, 3939, 4056,
    4095, 4056, 3939, 3750, 3495, 3185, 2831, 2447,
    2048, 1648, 1264,  910,  600,  345,  156,   39,
       0,   39,  156,  345,  600,  910, 1264, 1648
};

static volatile uint8_t s_sine_step = 0;
static volatile uint16_t s_dac_scaled_table[32];
static volatile bool s_dac_active = false;
static volatile uint8_t s_current_volume = 70;

/* -------------------------------------------------------------------------- */
/* Hardware Timer TIM4 ISR for Direct Digital Synthesis (DDS)                */
/* -------------------------------------------------------------------------- */
static void tim4_audio_isr(const void *arg)
{
    ARG_UNUSED(arg);

    if (TIM4->SR & TIM_SR_UIF) {
        TIM4->SR &= ~TIM_SR_UIF;

        if (s_dac_active) {
            uint16_t sample = s_dac_scaled_table[s_sine_step];
            /* 1. Output analog voltage to DAC1 (PA4) */
            DAC->DHR12R1 = sample;

            /* 2. Output digital sample to SPI3/I2S3 for ES8388 codec if ready */
            if (SPI3->SR & SPI_SR_TXE) {
                SPI3->DR = (uint16_t)(sample ^ 0x8000);
            }

            s_sine_step = (s_sine_step + 1) & 31;
        } else {
            DAC->DHR12R1 = 2048; /* Mid-rail quiescent bias */
        }
    }
}

/* -------------------------------------------------------------------------- */
/* I2C2 Low-Level Driver for ES8388 Codec Configuration                       */
/* PB10 = SCL, PB11 = SDA                                                     */
/* -------------------------------------------------------------------------- */
static void i2c2_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    /* Configure PB10 (SCL) and PB11 (SDA) as Open-Drain Outputs with Pull-Ups */
    GPIOB->MODER = (GPIOB->MODER & ~((3U << 20) | (3U << 22))) |
                   ((1U << 20) | (1U << 22)); /* Output mode */
    GPIOB->OTYPER |= (1U << 10) | (1U << 11);  /* Open drain */
    GPIOB->OSPEEDR |= (3U << 20) | (3U << 22);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~((3U << 20) | (3U << 22))) |
                   ((1U << 20) | (1U << 22));  /* Pull-up */

    GPIOB->BSRR = (1U << 10) | (1U << 11);     /* High idle */
}

static inline void i2c_delay(void)
{
    for (volatile int i = 0; i < 25; i++) {
        __NOP();
    }
}

static void i2c_start(void)
{
    GPIOB->BSRR = (1U << 10) | (1U << 11);
    i2c_delay();
    GPIOB->BSRR = (1U << (11 + 16)); /* SDA LOW while SCL is HIGH */
    i2c_delay();
    GPIOB->BSRR = (1U << (10 + 16)); /* SCL LOW */
    i2c_delay();
}

static void i2c_stop(void)
{
    GPIOB->BSRR = (1U << (11 + 16)); /* SDA LOW */
    i2c_delay();
    GPIOB->BSRR = (1U << 10);        /* SCL HIGH */
    i2c_delay();
    GPIOB->BSRR = (1U << 11);        /* SDA HIGH while SCL is HIGH */
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            GPIOB->BSRR = (1U << 11);
        } else {
            GPIOB->BSRR = (1U << (11 + 16));
        }
        i2c_delay();
        GPIOB->BSRR = (1U << 10); /* SCL HIGH */
        i2c_delay();
        GPIOB->BSRR = (1U << (10 + 16)); /* SCL LOW */
        i2c_delay();
    }

    /* Release SDA for ACK */
    GPIOB->BSRR = (1U << 11);
    i2c_delay();
    GPIOB->BSRR = (1U << 10); /* SCL HIGH for ACK clock */
    i2c_delay();
    bool ack = ((GPIOB->IDR & (1U << 11)) == 0);
    GPIOB->BSRR = (1U << (10 + 16)); /* SCL LOW */
    i2c_delay();
    return ack;
}

static void es8388_reg_write(uint8_t reg, uint8_t val)
{
    i2c_start();
    i2c_write_byte(0x20); /* 7-bit 0x10 << 1 | Write(0) */
    i2c_write_byte(reg);
    i2c_write_byte(val);
    i2c_stop();
}

/* -------------------------------------------------------------------------- */
/* ES8388 Codec Setup (Matching RT-Spark Official Board Driver)               */
/* -------------------------------------------------------------------------- */
static void es8388_codec_init(void)
{
    i2c2_gpio_init();
    k_msleep(10);

    /* ES8388 Initialization Sequence from drv_es8388.c */
    es8388_reg_write(0x19, 0x04); /* DACCONTROL3: mute during setup */
    es8388_reg_write(0x01, 0x50); /* CONTROL2 */
    es8388_reg_write(0x02, 0x00); /* CHIPPOWER: power up all */
    es8388_reg_write(0x08, 0x00); /* MASTERMODE: slave mode */
    es8388_reg_write(0x04, 0xC0); /* DACPOWER: disable DAC temporarily */
    es8388_reg_write(0x00, 0x12); /* CONTROL1: Play & Record mode */
    es8388_reg_write(0x17, 0x18); /* DACCONTROL1: 16-bit I2S format */
    es8388_reg_write(0x18, 0x02); /* DACCONTROL2: Single speed, ratio 256 */
    es8388_reg_write(0x26, 0x00); /* DACCONTROL16: audio on LIN1/RIN1 */
    es8388_reg_write(0x27, 0x9C); /* DACCONTROL17: L DAC to L mixer 0dB */
    es8388_reg_write(0x2A, 0x9C); /* DACCONTROL20: R DAC to R mixer 0dB */
    es8388_reg_write(0x2B, 0x80); /* DACCONTROL21: internal LRCK */
    es8388_reg_write(0x2D, 0x00); /* DACCONTROL23 */

    /* Headphone volume (0x1E = 0dB gain) */
    es8388_reg_write(0x2E, 0x1E); /* DACCONTROL24: LOUT1VOL (3.5mm Left) */
    es8388_reg_write(0x2F, 0x1E); /* DACCONTROL25: ROUT1VOL (3.5mm Right) */

    /* Power on DAC and LOUT1 / ROUT1 output amplifiers */
    es8388_reg_write(0x04, 0x3C); /* DACPOWER: Enable DAC and Lout/Rout */

    /* Un-mute DAC */
    es8388_reg_write(0x19, 0x00); /* DACCONTROL3: Un-mute */
}

/* -------------------------------------------------------------------------- */
/* I2S3 Configuration on STM32F407 (PC7: MCK, PA15: WS, PB3: SCK, PB5: SD)   */
/* -------------------------------------------------------------------------- */
static void i2s3_hw_init(void)
{
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;

    /* PA15 (I2S3_WS) -> Alternate Function 6 */
    GPIOA->MODER = (GPIOA->MODER & ~(3U << 30)) | (2U << 30);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFU << 28)) | (6U << 28);
    GPIOA->OSPEEDR |= (3U << 30);

    /* PB3 (I2S3_CK) and PB5 (I2S3_SD) -> Alternate Function 6 */
    GPIOB->MODER = (GPIOB->MODER & ~((3U << 6) | (3U << 10))) |
                   ((2U << 6) | (2U << 10));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFU << 12) | (0xFU << 20))) |
                    ((6U << 12) | (6U << 20));
    GPIOB->OSPEEDR |= ((3U << 6) | (3U << 10));

    /* PC7 (I2S3_MCK) -> Alternate Function 6 */
    GPIOC->MODER = (GPIOC->MODER & ~(3U << 14)) | (2U << 14);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFU << 28)) | (6U << 28);
    GPIOC->OSPEEDR |= (3U << 14);

    /* Configure I2S3 in Master Transmit Mode */
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1 |
                    SPI_I2SCFGR_I2SE;
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

    printk("[Audio_Codec] Initializing ES8388 Codec on 3.5mm Headphone Jack (CN3)...\n");
    es8388_codec_init();
    i2s3_hw_init();

    /* Initialize TIM4 for 32-sample Direct Digital Synthesis (DDS) */
    RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;
    TIM4->PSC = 83; /* 1 MHz count resolution */
    TIM4->ARR = 1000;
    TIM4->DIER |= TIM_DIER_UIE;
    TIM4->CR1 = TIM_CR1_CEN;

    /* Connect TIM4 interrupt in Zephyr */
    IRQ_CONNECT(TIM4_IRQn, 1, tim4_audio_isr, NULL, 0);
    irq_enable(TIM4_IRQn);

    /* Initialize scaled sine table */
    audio_hardware_dac_set_volume(s_current_volume);

    printk("[Audio_DAC] Dual-Output Analog Audio Synthesizer (PA4 + 3.5mm Jack) ready.\n");
}

void audio_hardware_dac_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_current_volume = volume_percent;

    /* Recompute scaled 32-sample table */
    for (int i = 0; i < 32; i++) {
        int32_t centered = (int32_t)SINE_32[i] - 2048;
        int32_t scaled = (centered * (int32_t)volume_percent) / 100;
        s_dac_scaled_table[i] = (uint16_t)(scaled + 2048);
    }

    /* Update digital volume in ES8388 codec */
    uint8_t es_vol = (uint8_t)((100 - volume_percent) * 192 / 100);
    es8388_reg_write(0x1A, es_vol); /* DAC L */
    es8388_reg_write(0x1B, es_vol); /* DAC R */
}

void audio_hardware_dac_set_tone(float note_period_ms, uint8_t volume_percent)
{
    if (note_period_ms <= 0.001f || volume_percent == 0) {
        audio_hardware_dac_stop();
        return;
    }

    audio_hardware_dac_set_volume(volume_percent);

    /* Calculate timer period for 32 samples per note period:
     * note_period_ms * 1000 us / 32 = period_us_per_sample
     */
    uint32_t step_period_us = (uint32_t)((note_period_ms * 1000.0f) / 32.0f + 0.5f);
    if (step_period_us < 10) {
        step_period_us = 10;
    }
    if (step_period_us > 5000) {
        step_period_us = 5000;
    }

    TIM4->ARR = step_period_us - 1;
    if (TIM4->CNT >= (step_period_us - 1)) {
        TIM4->CNT = 0;
    }

    s_dac_active = true;
}

void audio_hardware_dac_stop(void)
{
    s_dac_active = false;
    DAC->DHR12R1 = 2048;
}
