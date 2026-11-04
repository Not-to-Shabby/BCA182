/**
 * @file ui_dashboard.c
 * @brief ST7789 240x240 baby-monitor dashboard (presentation layer).
 *
 * Laboratory 4 differential-redraw style: the static frame (header bar, card
 * outlines, footer disclaimer) is painted once by ui_dashboard_init(); every
 * ui_dashboard_update() call formats each field into a fixed-width string,
 * compares it against a cached mirror (s_last_*), and repaints only regions
 * whose content changed -- no flicker, no residue. The ALERT banner blinks
 * red/maroon on a 250 ms phase derived from the tick, mirroring the alert
 * strip behavior of Lab 4; a confirmed cry (cry_task) replaces the ALERT
 * banner text with "CRY DETECTED" under the same treatment and raises a CRY
 * flag in the footer row.
 *
 * All drawing happens on the caller's snapshot COPY (never the live state
 * pointer), so no lock is held while the relatively slow FSMC writes run.
 */

#include "ui_dashboard.h"
#include "lcd_st7789.h"
#include "baby_logic.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h> /* k_uptime_get_32() */

/* ------------------------------------------------------------------ layout */
/* 8x16 font: every character cell is 8 px wide, 16 px tall. */
#define CHAR_W      8U
#define CHAR_H      16U

#define BG          LCD_COLOR_BLACK
#define CARD_BORDER LCD_COLOR_DARKGREY

/* Header bar */
#define HEADER_Y2   23U

/* State banner strip (fill color = state color) */
#define BANNER_Y1   28U
#define BANNER_Y2   55U
#define BANNER_TEXT_Y 36U

/* MUTE countdown row */
#define MUTE_Y      62U

/* Sensor card: 5 rows, labels at x=16, values at x=80 */
#define CARD_Y1     84U
#define CARD_Y2     186U
#define LABEL_X     16U
#define VALUE_X     80U
#define ROW_Y(i)    (92U + (i) * 18U)

/* Footer card: uptime left, CRY + TEST flags right */
#define FOOT_Y1     192U
#define FOOT_Y2     218U
#define UP_Y        196U
#define CRY_X       144U
#define TEST_X      180U

/* Disclaimer footer line */
#define DISCL_Y     224U

#define MUTE_BLANK  "          " /* same width as "MUTE 99:59" */

/* ----------------------------------------------------- differential caches */
static char   s_last_banner[24];        /* last banner text, '' = never */
static bool   s_last_alert_phase;
static bool   s_last_cry_flag;
static char   s_last_mute[16];
static char   s_last_temp[12];
static char   s_last_hum[12];
static char   s_last_light[12];
static char   s_last_prox[12];
static char   s_last_accel[12];
static char   s_last_up[16];
static bool   s_last_test;

/* ----------------------------------------------------------- static frame */
void ui_dashboard_init(void)
{
	lcd_backlight_on();

	lcd_clear(BG);

	/* Header bar */
	lcd_fill_rect(0, 0, LCD_WIDTH - 1, HEADER_Y2, LCD_COLOR_NAVY);
	lcd_show_string(72, 4, "BABY MONITOR", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
	lcd_draw_line(0, 24, LCD_WIDTH - 1, 24, CARD_BORDER);

	/* Sensor card outline + row labels (values painted by update()) */
	lcd_draw_rect(8, CARD_Y1, 231, CARD_Y2, CARD_BORDER);
	lcd_show_string(LABEL_X, ROW_Y(0), "TEMP  :", LCD_COLOR_LIGHTGREY, BG);
	lcd_show_string(LABEL_X, ROW_Y(1), "HUM   :", LCD_COLOR_LIGHTGREY, BG);
	lcd_show_string(LABEL_X, ROW_Y(2), "LIGHT :", LCD_COLOR_LIGHTGREY, BG);
	lcd_show_string(LABEL_X, ROW_Y(3), "PROX  :", LCD_COLOR_LIGHTGREY, BG);
	lcd_show_string(LABEL_X, ROW_Y(4), "ACCEL :", LCD_COLOR_LIGHTGREY, BG);

	/* Footer card outline */
	lcd_draw_rect(8, FOOT_Y1, 231, FOOT_Y2, CARD_BORDER);

	/* Disclaimer: 23 chars * 8 px = 184 px, centered */
	lcd_draw_line(0, 222, LCD_WIDTH - 1, 222, CARD_BORDER);
	lcd_show_string(28, DISCL_Y, "prototype - not medical",
			LCD_COLOR_GRAY, BG);

	/* Reset caches so the first update() paints every dynamic region */
	s_last_banner[0] = '\0';
	s_last_alert_phase = false;
	s_last_cry_flag = false;
	s_last_mute[0] = '\0';
	s_last_temp[0] = '\0';
	s_last_hum[0] = '\0';
	s_last_light[0] = '\0';
	s_last_prox[0] = '\0';
	s_last_accel[0] = '\0';
	s_last_up[0] = '\0';
	s_last_test = false;
}

/* ------------------------------------------------------------ field paints */
/* Repaint a fixed-width field only when its text changed. */
static void paint_field(uint16_t x, uint16_t y, const char *text,
			uint16_t color, char *cache, size_t cache_size)
{
	if (strcmp(text, cache) != 0) {
		lcd_show_string(x, y, text, color, BG);
		/* snprintf: bounded copy with guaranteed NUL (no truncation
		 * warning; the field buffers are sized for the fixed formats). */
		snprintf(cache, cache_size, "%s", text);
	}
}

/* The banner text is formatted by the caller (it encodes the state AND the
 * cry flag); paint_banner only picks the colors, fills the strip and centers
 * the text. text must stay within 23 chars + NUL (the s_last_banner cache). */
static void paint_banner(baby_state_t state, const char *text,
			 bool alert_phase)
{
	uint16_t fill;
	uint16_t fg;

	switch (state) {
	case BABY_STATE_ALERT:
		fill = alert_phase ? LCD_COLOR_RED : LCD_COLOR_MAROON;
		fg = LCD_COLOR_WHITE;
		break;
	case BABY_STATE_FAULT:
		fill = LCD_COLOR_ORANGE; /* amber */
		fg = LCD_COLOR_WHITE;
		break;
	case BABY_STATE_NORMAL:
	default:
		fill = LCD_COLOR_GREEN;
		fg = LCD_COLOR_BLACK;
		break;
	}

	lcd_fill_rect(0, BANNER_Y1, LCD_WIDTH - 1, BANNER_Y2, fill);

	uint16_t x = (uint16_t)((LCD_WIDTH - strlen(text) * CHAR_W) / 2U);
	lcd_show_string(x, BANNER_TEXT_Y, text, fg, fill);
}

/* ------------------------------------------------------------ update pass */
void ui_dashboard_update(const monitor_state_t *s)
{
	if (s == NULL) {
		return;
	}

	char buf[24];
	const uint32_t now = k_uptime_get_32();
	const bool alert_phase = ((now / 250U) & 1U) != 0U; /* 250 ms phase */

	/* --- STATE banner (blinks only while ALERT) ---
	 * A confirmed cry takes over the ALERT banner text ("CRY DETECTED"
	 * instead of "STATE: ALERT"), same red/blink treatment; FAULT still
	 * outranks it and keeps the amber STATE banner. */
	if (s->state == BABY_STATE_ALERT && s->cry_detected) {
		snprintf(buf, sizeof(buf), "CRY DETECTED");
	} else {
		snprintf(buf, sizeof(buf), "STATE: %s",
			 baby_state_name(s->state));
	}
	if (strcmp(buf, s_last_banner) != 0 ||
	    (s->state == BABY_STATE_ALERT && alert_phase != s_last_alert_phase)) {
		paint_banner(s->state, buf, alert_phase);
		snprintf(s_last_banner, sizeof(s_last_banner), "%s", buf);
		s_last_alert_phase = alert_phase;
	}

	/* --- MUTE countdown (mm:ss while the mute window is open) --- */
	uint32_t rem_s = 0U;
	if ((int32_t)(s->mute_until_ms - now) > 0) {
		rem_s = (s->mute_until_ms - now + 999U) / 1000U;
	}
	if (rem_s > 5999U) {
		rem_s = 5999U; /* cap display at 99:59 */
	}
	if (rem_s > 0U) {
		snprintf(buf, sizeof(buf), "MUTE %2u:%02u",
			 (unsigned)(rem_s / 60U), (unsigned)(rem_s % 60U));
	} else {
		strcpy(buf, MUTE_BLANK);
	}
	paint_field(16, MUTE_Y, buf, LCD_COLOR_YELLOW,
		    s_last_mute, sizeof(s_last_mute));

	/* --- TEMP / HUM (AHT10) --- */
	if (s->aht_ok) {
		const bool neg = s->temp_x10 < 0;
		const uint32_t mag = neg ? (uint32_t)(-s->temp_x10)
					 : (uint32_t)s->temp_x10;
		snprintf(buf, sizeof(buf), "%c%2u.%1u C", neg ? '-' : ' ',
			 (unsigned)(mag / 10U), (unsigned)(mag % 10U));
	} else {
		strcpy(buf, "  --   ");
	}
	paint_field(VALUE_X, ROW_Y(0), buf, LCD_COLOR_CYAN,
		    s_last_temp, sizeof(s_last_temp));

	if (s->aht_ok) {
		snprintf(buf, sizeof(buf), "%2u.%1u %%",
			 (unsigned)(s->hum_x10 / 10U),
			 (unsigned)(s->hum_x10 % 10U));
	} else {
		strcpy(buf, "  --  ");
	}
	paint_field(VALUE_X, ROW_Y(1), buf, LCD_COLOR_CYAN,
		    s_last_hum, sizeof(s_last_hum));

	/* --- LIGHT / PROX (AP3216C) --- */
	if (s->aps_ok) {
		snprintf(buf, sizeof(buf), "%5u", (unsigned)s->lux);
	} else {
		strcpy(buf, "  ---");
	}
	paint_field(VALUE_X, ROW_Y(2), buf, LCD_COLOR_CYAN,
		    s_last_light, sizeof(s_last_light));

	if (s->aps_ok) {
		snprintf(buf, sizeof(buf), "%5u", (unsigned)s->proximity);
	} else {
		strcpy(buf, "  ---");
	}
	paint_field(VALUE_X, ROW_Y(3), buf, LCD_COLOR_CYAN,
		    s_last_prox, sizeof(s_last_prox));

	/* --- ACCEL (ICM20608) --- */
	if (s->imu_ok) {
		snprintf(buf, sizeof(buf), "%5ld mg", (long)s->accel_mg);
	} else {
		strcpy(buf, "  ---   ");
	}
	paint_field(VALUE_X, ROW_Y(4), buf, LCD_COLOR_CYAN,
		    s_last_accel, sizeof(s_last_accel));

	/* --- Footer: uptime seconds + CRY / TEST flags --- */
	snprintf(buf, sizeof(buf), "UP %6u s",
		 (unsigned)(now / 1000U));
	paint_field(16, UP_Y, buf, LCD_COLOR_YELLOW,
		    s_last_up, sizeof(s_last_up));

	const bool cry_flag = s->cry_detected;
	if (cry_flag != s_last_cry_flag) {
		lcd_show_string(CRY_X, UP_Y, cry_flag ? "CRY" : "   ",
				LCD_COLOR_RED, BG);
		s_last_cry_flag = cry_flag;
	}

	const bool test = s->test_mode;
	if (test != s_last_test) {
		lcd_show_string(TEST_X, UP_Y, test ? "TEST" : "    ",
				LCD_COLOR_YELLOW, BG);
		s_last_test = test;
	}
}
