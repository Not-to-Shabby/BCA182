/**
 * @file audio_codec_es8388.c
 * @brief High-fidelity audio synthesizer & I2S driver for Everest Semiconductor ES8388
 *        Stereo Codec on RT-Thread Spark Board driving the 3.5mm Headphone Jack (CN3).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "audio_codec_es8388.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>

static audio_diagnostics_t s_diag = {
    .es_detected = false,
    .es_addr = 0x10,
    .reg04_readback = 0xFF,
    .reg04_verified = false,
    .pll_locked = false,
    .i2s_tx_samples = 0,
    .dma_misses = 0,
    .peak_left = 0,
    .peak_right = 0
};

static volatile uint8_t s_current_volume = 75;
static audio_pcm_callback_t s_pcm_callback = NULL;

/* DMA and Staging Buffers */
static uint16_t s_audio_dma[AUDIO_DMA_WORDS];
static int16_t s_stage[2][AUDIO_HALF_WORDS];
static volatile uint8_t s_stage_ready[2];

/* -------------------------------------------------------------------------- */
/* I2C2 Bit-Bang Implementation on PF0 (SDA) and PF1 (SCL)                    */
/* -------------------------------------------------------------------------- */
static void i2c2_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    /* Configure PF1 (SCL) and PF0 (SDA) as Open-Drain Outputs with Pull-Ups */
    GPIOF->MODER = (GPIOF->MODER & ~((3U << (0 * 2)) | (3U << (1 * 2)))) |
                   ((1U << (0 * 2)) | (1U << (1 * 2)));
    GPIOF->OTYPER |= (1U << 0) | (1U << 1);
    GPIOF->OSPEEDR |= (3U << (0 * 2)) | (3U << (1 * 2));
    GPIOF->PUPDR = (GPIOF->PUPDR & ~((3U << (0 * 2)) | (3U << (1 * 2)))) |
                   ((1U << (0 * 2)) | (1U << (1 * 2)));

    GPIOF->BSRR = (1U << 0) | (1U << 1); /* Idle HIGH */
}

static inline void i2c_delay(void)
{
    for (volatile int i = 0; i < 200; i++) {
        __NOP();
    }
}

static void i2c_start(void)
{
    GPIOF->BSRR = (1U << 0) | (1U << 1);
    i2c_delay();
    GPIOF->BSRR = (1U << (0 + 16)); /* SDA LOW */
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
    GPIOF->BSRR = (1U << 0);        /* SDA HIGH */
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            GPIOF->BSRR = (1U << 0);
        } else {
            GPIOF->BSRR = (1U << (0 + 16));
        }
        i2c_delay();
        GPIOF->BSRR = (1U << 1);
        i2c_delay();
        GPIOF->BSRR = (1U << (1 + 16));
        i2c_delay();
    }

    /* ACK pulse */
    GPIOF->BSRR = (1U << 0);
    i2c_delay();
    GPIOF->BSRR = (1U << 1);
    i2c_delay();
    bool ack = ((GPIOF->IDR & (1U << 0)) == 0);
    GPIOF->BSRR = (1U << (1 + 16));
    i2c_delay();
    return ack;
}

static uint8_t i2c_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    GPIOF->BSRR = (1U << 0);

    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        GPIOF->BSRR = (1U << 1);
        i2c_delay();
        if (GPIOF->IDR & (1U << 0)) {
            byte |= (1U << i);
        }
        GPIOF->BSRR = (1U << (1 + 16));
        i2c_delay();
    }

    if (send_ack) {
        GPIOF->BSRR = (1U << (0 + 16));
    } else {
        GPIOF->BSRR = (1U << 0);
    }
    i2c_delay();
    GPIOF->BSRR = (1U << 1);
    i2c_delay();
    GPIOF->BSRR = (1U << (1 + 16));
    GPIOF->BSRR = (1U << 0);
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

    i2c_start();
    if (!i2c_write_byte(read_addr)) {
        i2c_stop();
        return 0xFF;
    }

    uint8_t val = i2c_read_byte(false);
    i2c_stop();
    return val;
}

static void es8388_codec_init(void)
{
    i2c2_gpio_init();
    k_msleep(20);

    /* Probe I2C address */
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
        printk("[ES8388] Codec ACK on I2C address 0x10\n");
    } else if (ack11) {
        s_diag.es_addr = 0x11;
        s_diag.es_detected = true;
        printk("[ES8388] Codec ACK on I2C address 0x11\n");
    } else {
        s_diag.es_addr = 0x10;
        s_diag.es_detected = false;
        printk("[ES8388] NACK on both 0x10 and 0x11\n");
    }

    /* ES8388 register configuration */
    es8388_reg_write(0x19, 0x04); /* Mute DAC during setup */
    es8388_reg_write(0x01, 0x50); /* Chip power mgmt */
    es8388_reg_write(0x02, 0x00); /* Normal all */
    es8388_reg_write(0x08, 0x00); /* Slave mode */

    /* DAC setup */
    es8388_reg_write(0x04, 0xC0); /* Disable DAC temporarily */
    es8388_reg_write(0x00, 0x12); /* Play & Record mode */
    es8388_reg_write(0x17, 0x18); /* 16-bit I2S format */
    es8388_reg_write(0x18, 0x02); /* Single speed, ratio 256 */
    es8388_reg_write(0x26, 0x00); /* Audio on LIN1/RIN1 */
    es8388_reg_write(0x27, 0x9C); /* L DAC to L mixer enable 0dB */
    es8388_reg_write(0x2A, 0x9C); /* R DAC to R mixer enable 0dB */
    es8388_reg_write(0x2B, 0x80); /* Internal LRCK */
    es8388_reg_write(0x2D, 0x00); /* vroi = 0 */

    /* Volume */
    es8388_reg_write(0x1A, 0x00); /* L Digital Vol 0dB */
    es8388_reg_write(0x1B, 0x00); /* R Digital Vol 0dB */
    es8388_reg_write(0x04, 0x3C); /* Enable DAC and Lout/Rout */

    /* ADC setup required for clock generation */
    es8388_reg_write(0x03, 0xFF);
    es8388_reg_write(0x09, 0xBB);
    es8388_reg_write(0x0A, 0x00);
    es8388_reg_write(0x0B, 0x02);
    es8388_reg_write(0x0C, 0x0D);
    es8388_reg_write(0x0D, 0x02);
    es8388_reg_write(0x10, 0x00);
    es8388_reg_write(0x11, 0x00);
    es8388_reg_write(0x03, 0x09);

    /* Headphone volume: LOUT1 / ROUT1 (0x00 = +3.0 dB maximum output boost) */
    es8388_reg_write(0x2E, 0x00);
    es8388_reg_write(0x2F, 0x00);

    /* Reset state machine */
    es8388_reg_write(0x02, 0xF0);
    k_msleep(5);
    es8388_reg_write(0x02, 0x00);
    k_msleep(5);

    /* Un-mute */
    es8388_reg_write(0x04, 0x3C);
    es8388_reg_write(0x19, 0x00);
    k_msleep(10);

    s_diag.reg04_readback = es8388_reg_read(0x04);
    s_diag.reg04_verified = (s_diag.reg04_readback == 0x3C);
}

/* -------------------------------------------------------------------------- */
/* I2S3 Configuration on STM32F407                                            */
/* PC7: MCK, PA15: WS, PB3: CK, PB5: SD                                       */
/* -------------------------------------------------------------------------- */
static void i2s3_hw_init(void)
{
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;

    RCC->CFGR &= ~RCC_CFGR_I2SSRC; /* PLLI2S source */

    /* PLLI2S for 44.1 kHz */
    RCC->CR &= ~RCC_CR_PLLI2SON;
    RCC->PLLI2SCFGR = (271U << RCC_PLLI2SCFGR_PLLI2SN_Pos) | (2U << RCC_PLLI2SCFGR_PLLI2SR_Pos);
    RCC->CR |= RCC_CR_PLLI2SON;
    uint32_t timeout = 100000;
    while (!(RCC->CR & RCC_CR_PLLI2SRDY) && --timeout);
    s_diag.pll_locked = ((RCC->CR & RCC_CR_PLLI2SRDY) != 0);

    /* Pinmux AF6 */
    /* PC7 - MCK */
    GPIOC->MODER &= ~(3U << (7 * 2));
    GPIOC->MODER |=  (2U << (7 * 2));
    GPIOC->AFR[0] &= ~(0x0FU << (7 * 4));
    GPIOC->AFR[0] |=  (0x06U << (7 * 4));
    GPIOC->OSPEEDR |= (3U << (7 * 2));

    /* PA15 - WS */
    GPIOA->MODER &= ~(3U << (15 * 2));
    GPIOA->MODER |=  (2U << (15 * 2));
    GPIOA->AFR[1] &= ~(0x0FU << ((15 - 8) * 4));
    GPIOA->AFR[1] |=  (0x06U << ((15 - 8) * 4));
    GPIOA->OSPEEDR |= (3U << (15 * 2));

    /* PB3 - CK */
    GPIOB->MODER &= ~(3U << (3 * 2));
    GPIOB->MODER |=  (2U << (3 * 2));
    GPIOB->AFR[0] &= ~(0x0FU << (3 * 4));
    GPIOB->AFR[0] |=  (0x06U << (3 * 4));
    GPIOB->OSPEEDR |= (3U << (3 * 2));

    /* PB5 - SD */
    GPIOB->MODER &= ~(3U << (5 * 2));
    GPIOB->MODER |=  (2U << (5 * 2));
    GPIOB->AFR[0] &= ~(0x0FU << (5 * 4));
    GPIOB->AFR[0] |=  (0x06U << (5 * 4));
    GPIOB->OSPEEDR |= (3U << (5 * 2));

    /* I2S3 Peripheral Setup: 44.1 kHz, 16-bit, MCK enabled, Master Transmit */
    SPI3->I2SCFGR = 0;
    SPI3->I2SPR   = SPI_I2SPR_MCKOE | 6U;
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1 | SPI_I2SCFGR_I2SE;
}

/* -------------------------------------------------------------------------- */
/* DMA1 Stream 5 & Audio Producer Thread                                      */
/* -------------------------------------------------------------------------- */
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
            s_diag.dma_misses++;
        }
        clear |= DMA_HIFCR_CHTIF5;
    }
    if (status & DMA_HISR_TCIF5) {
        if (s_stage_ready[1]) {
            memcpy(&s_audio_dma[AUDIO_HALF_WORDS], s_stage[1], sizeof(s_stage[1]));
            s_stage_ready[1] = 0;
        } else {
            s_diag.dma_misses++;
        }
        clear |= DMA_HIFCR_CTCIF5;
    }
    if (status & (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)) {
        clear |= DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;
    }
    if (clear != 0) DMA1->HIFCR = clear;
}

static void audio_producer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        for (int half = 0; half < 2; half++) {
            if (s_stage_ready[half]) {
                continue;
            }

            int16_t *buf = s_stage[half];
            if (s_pcm_callback) {
                s_pcm_callback(buf, AUDIO_HALF_WORDS);
            } else {
                memset(buf, 0, AUDIO_HALF_WORDS * sizeof(int16_t));
            }

            /* Apply digital master volume scaling and track peak values */
            int32_t vol = (int32_t)s_current_volume;
            int16_t max_l = 0;
            int16_t max_r = 0;

            for (uint16_t i = 0; i < AUDIO_HALF_WORDS; i += 2) {
                int32_t left = ((int32_t)buf[i] * vol) / 100;
                int32_t right = ((int32_t)buf[i + 1] * vol) / 100;

                buf[i]     = (int16_t)left;
                buf[i + 1] = (int16_t)right;

                /* Mirror analog audio to STM32 12-bit Analog DAC on Pin PA4 */
                int32_t dac_val = (left / 16) + 2048;
                if (dac_val < 0) dac_val = 0;
                if (dac_val > 4095) dac_val = 4095;
                DAC->DHR12R1 = (uint16_t)dac_val;

                int16_t abs_l = (int16_t)abs(left);
                int16_t abs_r = (int16_t)abs(right);
                if (abs_l > max_l) max_l = abs_l;
                if (abs_r > max_r) max_r = abs_r;
            }

            if (max_l > s_diag.peak_left) s_diag.peak_left = max_l;
            if (max_r > s_diag.peak_right) s_diag.peak_right = max_r;
            s_diag.i2s_tx_samples += AUDIO_HALF_WORDS;

            s_stage_ready[half] = 1;
        }
        k_msleep(1);
    }
}

K_THREAD_STACK_DEFINE(s_audio_producer_stack, 3072);
static struct k_thread s_audio_producer_thread_data;

static void audio_dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    DMA1_Stream5->CR &= ~DMA_SxCR_EN;
    while (DMA1_Stream5->CR & DMA_SxCR_EN) { }
    DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 |
                  DMA_HIFCR_CTEIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;

    memset(s_audio_dma, 0, sizeof(s_audio_dma));
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

static void dac1_pa4_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;

    /* PA4 to Analog Mode */
    GPIOA->MODER |= (3U << (4 * 2));
    GPIOA->PUPDR &= ~(3U << (4 * 2));

    /* Enable DAC Channel 1 (PA4) */
    DAC->CR |= DAC_CR_EN1;
    DAC->DHR12R1 = 2048; /* Midpoint 1.65V bias */
}

void audio_hardware_init(void)
{
    printk("[Audio] Initializing STM32 12-Bit Analog DAC on PA4...\n");
    dac1_pa4_init();

    printk("[Audio] Initializing ES8388 Codec & I2S3 DMA...\n");
    es8388_codec_init();
    i2s3_hw_init();
    audio_dma_init();
    k_thread_create(&s_audio_producer_thread_data, s_audio_producer_stack,
                    K_THREAD_STACK_SIZEOF(s_audio_producer_stack),
                    audio_producer_thread, NULL, NULL, NULL,
                    1, 0, K_NO_WAIT);
    printk("[Audio] Audio subsystem initialized successfully!\n");
}

void audio_set_pcm_callback(audio_pcm_callback_t cb)
{
    s_pcm_callback = cb;
}

void audio_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    s_current_volume = volume_percent;

    /* Map 0-100% to ES8388 analog headphone amplifier (0x00 is +3dB, 0x33 is -50dB) */
    uint8_t att = (uint8_t)(((100U - (uint32_t)volume_percent) * 0x33U) / 100U);
    es8388_reg_write(0x2E, att);
    es8388_reg_write(0x2F, att);
}

uint8_t audio_get_volume(void)
{
    return s_current_volume;
}

const audio_diagnostics_t* audio_get_diagnostics(void)
{
    return &s_diag;
}

void audio_reset_peak_meters(void)
{
    s_diag.peak_left = 0;
    s_diag.peak_right = 0;
}
