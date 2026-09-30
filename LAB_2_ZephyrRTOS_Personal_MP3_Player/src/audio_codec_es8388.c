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
#include <stdio.h>
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

static volatile uint16_t s_dac_scaled_table[32];
static volatile bool s_dac_active = false;
static volatile uint8_t s_current_volume = 70;
static volatile uint32_t s_phase_acc = 0;
static volatile uint32_t s_phase_inc = 0;
static volatile bool s_channel_toggle = false;
static uint8_t s_es_addr = 0x20; /* Probed write address */
static bool s_es_detected = false;
static bool s_pll_ready = false;
static char s_diag_str[48] = "ES8388:PENDING";

/* -------------------------------------------------------------------------- */
/* Direct Digital Synthesis (DDS) Interrupt via SPI3 / I2S3 TXE               */
/* -------------------------------------------------------------------------- */
static void spi3_i2s_isr(const void *arg)
{
    ARG_UNUSED(arg);

    while (SPI3->SR & SPI_SR_TXE) {
        if (!s_dac_active || s_phase_inc == 0) {
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
    for (volatile int i = 0; i < 40; i++) {
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

static bool es8388_reg_write(uint8_t reg, uint8_t val)
{
    i2c_start();
    bool ack_addr = i2c_write_byte(s_es_addr);
    bool ack_reg  = i2c_write_byte(reg);
    bool ack_val  = i2c_write_byte(val);
    i2c_stop();
    return (ack_addr && ack_reg && ack_val);
}

/* -------------------------------------------------------------------------- */
/* ES8388 Codec Setup (Matching RT-Spark Official Board Driver)               */
/* -------------------------------------------------------------------------- */
static void es8388_codec_init(void)
{
    i2c2_gpio_init();
    k_msleep(20);

    /* Probe I2C address: 0x20 (7-bit 0x10) or 0x22 (7-bit 0x11) */
    i2c_start();
    bool ack10 = i2c_write_byte(0x20);
    i2c_stop();
    k_msleep(5);

    i2c_start();
    bool ack11 = i2c_write_byte(0x22);
    i2c_stop();
    k_msleep(5);

    if (ack10) {
        s_es_addr = 0x20;
        s_es_detected = true;
        printk("[ES8388] Codec detected at I2C address 0x10!\n");
    } else if (ack11) {
        s_es_addr = 0x22;
        s_es_detected = true;
        printk("[ES8388] Codec detected at I2C address 0x11!\n");
    } else {
        s_es_addr = 0x20;
        s_es_detected = false;
        printk("[ES8388] Notice: Defaulting to I2C 0x10 (NACK)\n");
    }

    /* ES8388 Register Sequence from drv_es8388.c:
     * - Mute DAC during configuration
     * - Power up all blocks
     * - Configure 16-bit I2S format, single speed
     * - Route DAC Left/Right to LOUT1 / ROUT1 mixer
     * - Set headphone volume gain to 0dB (0x1E)
     * - Power on DAC output stage and unmute
     */
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

    /* Headphone volume: LOUT1 / ROUT1 */
    es8388_reg_write(0x2E, 0x21); /* DACCONTROL24: LOUT1VOL (3.5mm Left) */
    es8388_reg_write(0x2F, 0x21); /* DACCONTROL25: ROUT1VOL (3.5mm Right) */

    /* Power on DAC and LOUT1 / ROUT1 output amplifiers */
    es8388_reg_write(0x04, 0x3C); /* DACPOWER: Enable DAC and Lout/Rout */

    /* Un-mute DAC */
    es8388_reg_write(0x19, 0x00); /* DACCONTROL3: Un-mute */
}

/* -------------------------------------------------------------------------- */
/* I2S3 Configuration on STM32F407 (PC7: MCK, PA15: WS, PB3: CK, PB5: SD)   */
/* -------------------------------------------------------------------------- */
static void i2s3_hw_init(void)
{
    /* 1. Enable peripheral clocks */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;

    /* 2. Configure PLLI2S for 44.1 kHz audio:
     *    HSE (8 MHz) / 8 * 271 / 2 = 135.5 MHz I2SxCLK
     */
    RCC->CR &= ~RCC_CR_PLLI2SON;
    RCC->PLLI2SCFGR = (271U << RCC_PLLI2SCFGR_PLLI2SN_Pos) | (2U << RCC_PLLI2SCFGR_PLLI2SR_Pos);
    RCC->CR |= RCC_CR_PLLI2SON;
    uint32_t timeout = 100000;
    while (!(RCC->CR & RCC_CR_PLLI2SRDY) && --timeout);
    s_pll_ready = ((RCC->CR & RCC_CR_PLLI2SRDY) != 0);

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
    SPI3->CR2 |= SPI_CR2_TXEIE;            /* Enable TX Empty Interrupt */
    SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;     /* Enable I2S peripheral */

    /* 5. Connect SPI3 interrupt in Zephyr */
    IRQ_CONNECT(SPI3_IRQn, 1, spi3_i2s_isr, NULL, 0);
    irq_enable(SPI3_IRQn);
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

    /* DDS phase increment for 44.1 kHz stereo sample rate:
     * f_target = 1000.0 / note_period_ms
     * phase_inc = (f_target / 44100) * 2^32 = 97391549.0 / note_period_ms
     */
    s_phase_inc = (uint32_t)(97391549.0f / note_period_ms + 0.5f);
    s_dac_active = true;
}

void audio_hardware_dac_stop(void)
{
    s_dac_active = false;
    s_phase_inc = 0;
    DAC->DHR12R1 = 2048;
}

const char *audio_hardware_dac_status(void)
{
    snprintf(s_diag_str, sizeof(s_diag_str), "ES8388:%s PLL:%s",
             s_es_detected ? (s_es_addr == 0x20 ? "0x10" : "0x11") : "NACK",
             s_pll_ready ? "OK" : "FAIL");
    return s_diag_str;
}
