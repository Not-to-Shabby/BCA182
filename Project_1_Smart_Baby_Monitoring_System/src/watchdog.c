/**
 * @file watchdog.c
 * @brief IWDG watchdog + per-thread liveness supervision.
 *
 * The freeze observed on hardware (non-deterministic, at the first cry
 * window compute / net gate wake, ~7-8 s after boot) wedges the whole
 * system with no fault dump. The IWDG converts that wedge into a board
 * reset: the system self-recovers, and repeated resets in the boot log
 * are themselves the diagnostic signature of the race.
 *
 * Feed discipline: the audio capture thread (prio 5, ~8 ms cadence) calls
 * watchdog_feed_check() every loop. The feed is WITHHELD unless every
 * registered thread advanced its progress counter since the last feed, so
 * a wedged cry/net/sensor/ui thread starves the feed and triggers the
 * reset instead of being masked by a healthy audio path.
 */
#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#define WDT_FEED_WINDOW_MS 4000u
#define WDT_TIMEOUT_S      16u
#define WDT_MAX_THREADS    8u

static const struct device *s_wdt;
static int s_wdt_channel = -1;
static bool s_wdt_ok;

struct wdt_slot {
	const char *name;
	volatile uint32_t progress;
	volatile uint32_t last_seen;
	bool used;
};

static struct wdt_slot s_slots[WDT_MAX_THREADS];
static uint32_t s_last_feed_ms;
static uint32_t s_last_hb;

int watchdog_register_thread(const char *name)
{
	for (int i = 0; i < WDT_MAX_THREADS; i++) {
		if (!s_slots[i].used) {
			s_slots[i].used = true;
			s_slots[i].name = name;
			s_slots[i].progress = 0;
			s_slots[i].last_seen = 0;
			return i;
		}
	}
	return -1;
}

void watchdog_progress(int slot)
{
	if (slot >= 0 && slot < WDT_MAX_THREADS && s_slots[slot].used) {
		s_slots[slot].progress++;
	}
}

void watchdog_init(void)
{
	s_wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg));
	if (!device_is_ready(s_wdt)) {
		s_wdt = NULL;
		printk("[WDT] no watchdog device (running unwatched)\n");
		return;
	}

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0, .max = WDT_TIMEOUT_S * 1000u },
		.callback = NULL, /* IWDG: no early warning, reset on expiry */
		.flags = WDT_FLAG_RESET_SOC,
	};

	s_wdt_channel = wdt_install_timeout(s_wdt, &cfg);
	if (s_wdt_channel < 0) {
		s_wdt = NULL;
		printk("[WDT] install failed (%d) -- running unwatched\n",
		       s_wdt_channel);
		return;
	}
	if (wdt_setup(s_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		s_wdt = NULL;
		printk("[WDT] setup failed -- running unwatched\n");
		return;
	}

	s_wdt_ok = true;
	s_last_feed_ms = k_uptime_get_32();
	printk("[WDT] IWDG armed (%u s timeout, %d channels)\n",
	       (unsigned)WDT_TIMEOUT_S, s_wdt_channel);
}

void watchdog_feed_check(void)
{
	if (!s_wdt_ok) {
		return;
	}

	const uint32_t now = k_uptime_get_32();

	/* Liveness: every registered thread must have progressed since the
	 * previous feed check (checked at most once per feed window). */
	if ((now - s_last_hb) >= WDT_FEED_WINDOW_MS) {
		s_last_hb = now;
		for (int i = 0; i < WDT_MAX_THREADS; i++) {
			if (!s_slots[i].used) {
				continue;
			}
			if (s_slots[i].progress == s_slots[i].last_seen) {
				printk("[WDT] thread '%s' wedged -- withholding "
				       "feed (reset in <=%u s)\n",
				       s_slots[i].name,
				       (unsigned)WDT_TIMEOUT_S);
				return; /* starving the feed -> IWDG reset */
			}
			s_slots[i].last_seen = s_slots[i].progress;
		}
		(void)s_last_feed_ms;
	}

	wdt_feed(s_wdt, s_wdt_channel);
}
