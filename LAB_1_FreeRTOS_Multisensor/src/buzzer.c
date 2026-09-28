#include "buzzer.h"
#include "stm32f1xx_hal.h"

static TIM_HandleTypeDef htim2;
static bool buzzerOn = false;

bool Buzzer_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();

    /* PA2 = TIM2_CH3 (Alternate Function Push-Pull) */
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_2;
    g.Mode  = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);

    /* Dynamic prescaler to guarantee 1 MHz counter frequency regardless of clock tree */
    uint32_t timerClock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) { timerClock *= 2U; }
    uint32_t prescaler = (timerClock / 1000000U) - 1U;

    /* 1 MHz counter clock / (999 + 1) = 1 kHz audible tone */
    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = prescaler;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 999;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
    {
        return false;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = 500;                 /* 50 % duty cycle */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_3) != HAL_OK)
    {
        return false;
    }

    Buzzer_Set(false);
    return true;
}

void Buzzer_Set(bool on)
{
    if (on == buzzerOn) { return; }

    if (on) { (void)HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3); }
    else    { (void)HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3); }

    buzzerOn = on;
}
