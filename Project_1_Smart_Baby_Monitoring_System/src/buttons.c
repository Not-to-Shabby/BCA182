/**
 * @file buttons.c
 * @brief Debounced KEY0/KEY1 (PC0/PC1) handling: acknowledge-alert and
 *        test-alert toggle for the Smart Baby Monitoring System.
 *
 * Pin setup and active-LOW read modeled on Laboratory 4 service_mode.c
 * (raw GPIOC registers, pull-ups enabled). Debounce is time-stamped rather
 * than poll-counted because this module is driven from the UI thread's
 * 500 ms cycle: a raw level held stable for DEBOUNCE_MS becomes the new
 * stable level, and a press is the stable level's falling edge.
 */

#include "buttons.h"
#include "baby_logic.h"
#include "monitor_state.h"

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>

/* KEY0 = PC0, KEY1 = PC1 on the RT-Spark board, both active-LOW. */
#define KEY0_BIT 0U
#define KEY1_BIT 1U

/* A raw level must hold this long (ms) before it is accepted as stable. */
#define DEBOUNCE_MS 50U

/* Per-key debounce bookkeeping. stable starts HIGH (= released). */
typedef struct {
	bool stable;         /* accepted (debounced) level, true = released */
	bool last_raw;       /* previous raw sample                         */
	uint32_t raw_change_ms; /* k_uptime_get_32() of last raw transition  */
} key_t;

static key_t s_key0 = { true, true, 0U };
static key_t s_key1 = { true, true, 0U };

void buttons_init(void)
{
	/* PC0 (KEY0) and PC1 (KEY1): inputs with pull-up, active LOW
	 * (Laboratory 4 service_mode.c register recipe). */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
	(void)RCC->AHB1ENR; /* settle after clock enable */
	GPIOC->MODER &= ~((3U << (KEY0_BIT * 2)) | (3U << (KEY1_BIT * 2)));
	GPIOC->PUPDR = (GPIOC->PUPDR &
			~((3U << (KEY0_BIT * 2)) | (3U << (KEY1_BIT * 2)))) |
		       ((1U << (KEY0_BIT * 2)) | (1U << (KEY1_BIT * 2)));

	s_key0.stable = true;
	s_key0.last_raw = true;
	s_key0.raw_change_ms = 0U;
	s_key1.stable = true;
	s_key1.last_raw = true;
	s_key1.raw_change_ms = 0U;
}

/* Debounce one key. Returns true exactly once per confirmed press
 * (falling edge of the debounced level). */
static bool key_update(key_t *k, bool raw_now, uint32_t now_ms)
{
	if (raw_now != k->last_raw) {
		/* Raw bounce: (re)start the stability window. */
		k->last_raw = raw_now;
		k->raw_change_ms = now_ms;
	}
	if ((raw_now != k->stable) &&
	    ((now_ms - k->raw_change_ms) >= DEBOUNCE_MS)) {
		k->stable = raw_now;
		if (!k->stable) {
			return true; /* confirmed press (falling edge) */
		}
	}
	return false;
}

static void on_key0_press(void)
{
	/* Caregiver acknowledge: refresh the logic mute window and mirror it
	 * into the shared snapshot for the dashboard MUTE row. */
	uint32_t now = k_uptime_get_32();

	monitor_state_lock();
	baby_logic_acknowledge_for(now, ALERT_MUTE_MS);
	monitor_state_get()->mute_until_ms = now + ALERT_MUTE_MS;
	monitor_state_unlock();

	printk("[UI] Alert acknowledged (mute %u s)\n",
	       (unsigned)(ALERT_MUTE_MS / 1000U));
}

static void on_key1_press(void)
{
	/* test_mode is a UI-level demo alert: it only exercises the alarm
	 * outputs (buzzer + LED tint) so the presentation layer can be
	 * demonstrated without a fault. It never claims real cry detection
	 * and is deliberately invisible to baby_logic / the sensor task. */
	bool on;

	monitor_state_lock();
	monitor_state_get()->test_mode = !monitor_state_get()->test_mode;
	on = monitor_state_get()->test_mode;
	monitor_state_unlock();

	printk("[UI] Test alert %s\n", on ? "ON" : "OFF");
}

void buttons_poll(uint32_t now_ms)
{
	/* true from IDR = level high = released (active-LOW keys). */
	const bool raw0 = (GPIOC->IDR & (1U << KEY0_BIT)) != 0U;
	const bool raw1 = (GPIOC->IDR & (1U << KEY1_BIT)) != 0U;

	if (key_update(&s_key0, raw0, now_ms)) {
		on_key0_press();
	}
	if (key_update(&s_key1, raw1, now_ms)) {
		on_key1_press();
	}
}
