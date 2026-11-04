/**
 * @file buzzer.c
 * @brief TIM3 CH3 (PB0) PWM alarm buzzer for the Smart Baby Monitoring System.
 *
 * Raw-register port of the Laboratory 2 Personal MP3 Player audio engine
 * (src/audio_engine.c: hw_pwm_init / hw_set_tone / hw_stop_tone), reduced to
 * one fixed alarm tone. Same hardware facts as Lab 2:
 *   - APB1 timer clock = 84 MHz (168 MHz SYSCLK, APB1 div 4, x2 timer mult)
 *   - PSC = 83 -> 1 MHz counter (1 us per count)
 *   - PWM Mode 1, preload enabled, counter runs continuously; silence is
 *     achieved by driving the compare value to 0 (output stays low).
 */

#include "buzzer.h"

#include <stm32f4xx.h>

/* Fixed alarm tone: 2 kHz. At 1 MHz counting, period = 500 us -> ARR = 499.
 * 50 % duty gives the maximum fundamental acoustic output (Lab 2 finding). */
#define BUZZER_TONE_HZ        2000U
#define BUZZER_TICKS_PER_US   1U
#define BUZZER_PERIOD_US      (1000000U / BUZZER_TONE_HZ)
#define BUZZER_ARR            ((BUZZER_PERIOD_US * BUZZER_TICKS_PER_US) - 1U)
#define BUZZER_DUTY           (BUZZER_ARR / 2U)

void buzzer_init(void)
{
	/* 1. Enable AHB1 clock for GPIOB and APB1 clock for TIM3 */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
	RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;

	/* 2. PB0 = TIM3_CH3 (onboard buzzer): alternate function 2, push-pull */
	GPIOB->MODER = (GPIOB->MODER & ~(3U << (0 * 2))) | (2U << (0 * 2));
	GPIOB->OTYPER &= ~(1U << 0);
	GPIOB->OSPEEDR |= (3U << (0 * 2));
	GPIOB->PUPDR &= ~(3U << (0 * 2));
	GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFU << (0 * 4))) | (2U << (0 * 4));

	/* 3. Timebase: 1 MHz count, 2 kHz period, output idle OFF */
	TIM3->PSC = 83;              /* 84 MHz / (83 + 1) = 1 MHz            */
	TIM3->ARR = BUZZER_ARR;      /* 2 kHz alarm tone period              */
	TIM3->CCR3 = 0;              /* silent: compare 0 -> output low      */

	/* PWM Mode 1 on Channel 3 with output compare preload */
	TIM3->CCMR2 = (TIM3->CCMR2 & ~(7U << 4)) | (6U << 4) | TIM_CCMR2_OC3PE;

	/* Enable CH3 output (active-high polarity is the reset default) */
	TIM3->CCER |= TIM_CCER_CC3E;

	/* Force PSC/ARR into the shadow registers immediately, clear the update
	 * flag the UG event raises, then run the counter (Lab 2 sequencing). */
	TIM3->EGR = TIM_EGR_UG;
	TIM3->SR &= ~TIM_SR_UIF;
	TIM3->CR1 = TIM_CR1_CEN;
}

void buzzer_set(bool on)
{
	/* Cheap repeated calls: one compare-register store per invocation. */
	TIM3->CCR3 = on ? BUZZER_DUTY : 0U;
}
