#include "dht22.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

/* DHT22 single-wire protocol on PA1 */
#define DHT_PORT  GPIOA
#define DHT_PIN   GPIO_PIN_1

static uint32_t cyclesPerUs = 72U;

static void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    cyclesPerUs = SystemCoreClock / 1000000U;
    if (cyclesPerUs == 0U) { cyclesPerUs = 1U; }
}

static void DelayUs(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * cyclesPerUs;
    uint32_t guard = us * 20U;

    for (;;)
    {
        uint32_t elapsed = DWT->CYCCNT - start;
        if (elapsed >= ticks) { break; }
        if (guard-- == 0U)    { break; }
    }
}

static void PinAsOutput(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin   = DHT_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT_PORT, &g);
}

static void PinAsInput(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin  = DHT_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(DHT_PORT, &g);
}

static int WaitLevel(uint32_t level, uint32_t timeoutUs)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t limit = timeoutUs * cyclesPerUs;
    uint32_t guard = timeoutUs * 20U;

    for (;;)
    {
        uint32_t elapsed = DWT->CYCCNT - start;
        uint32_t pin = ((DHT_PORT->IDR & DHT_PIN) != 0U) ? 1U : 0U;

        if (pin == level)    { return (int)(elapsed / cyclesPerUs); }
        if (elapsed > limit) { return -1; }
        if (guard-- == 0U)   { return -1; }
    }
}

void DHT22_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    PinAsInput();
    DWT_Init();
}

bool DHT22_Read(Dht22Reading *out)
{
    uint8_t data[5] = {0};
    bool ok = false;

    /* Start signal: drive line low for >1 ms */
    PinAsOutput();
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
    DelayUs(1200);

    /* Time-critical sensor handshake (~5 ms) */
    taskENTER_CRITICAL();

    /* Release line and configure pull-up input */
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
    PinAsInput();

    /* Sensor response: low ~80 us, high ~80 us, then first data bit starts */
    if (WaitLevel(0U, 200U) < 0) goto done;
    if (WaitLevel(1U, 200U) < 0) goto done;
    if (WaitLevel(0U, 200U) < 0) goto done;

    /* 40 bits of serial data */
    for (int i = 0; i < 40; i++)
    {
        if (WaitLevel(1U, 100U) < 0) goto done;
        int highUs = WaitLevel(0U, 120U);
        if (highUs < 0) goto done;

        data[i / 8] = (uint8_t)(data[i / 8] << 1);
        if (highUs > 40) { data[i / 8] |= 1U; }
    }

    /* Checksum verification */
    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) == data[4])
    {
        int16_t t = (int16_t)(((data[2] & 0x7FU) << 8) | data[3]);
        if (data[2] & 0x80U) { t = (int16_t)-t; }

        out->hum_x10  = (uint16_t)((data[0] << 8) | data[1]);
        out->temp_x10 = t;
        ok = true;
    }

done:
    taskEXIT_CRITICAL();
    return ok;
}
