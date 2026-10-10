/**
 * @file app_config.h
 * @brief System configuration and timing parameters for Side Quest Karaoke.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* Catalog index to play at boot; -1 picks a random song. Bench builds override it. */
#ifndef KARAOKE_BOOT_SONG
#define KARAOKE_BOOT_SONG           (-1)
#endif

/* Bench builds: load another random song every this many milliseconds, the way a thumb on the
 * UP/DOWN keys does; 0 turns it off. */
#ifndef KARAOKE_STRESS_SWITCH_MS
#define KARAOKE_STRESS_SWITCH_MS    0
#endif

#define APP_NAME                    "RT-Spark Karaoke Player"
#define APP_VERSION                 "1.0.0"

/* Thread Stack Sizes */
#define UI_THREAD_STACK_SIZE        4096
#define BUTTON_THREAD_STACK_SIZE    3072    /* loads songs, so it can end up restarting the SD driver */

/* Thread Priorities */
#define AUDIO_THREAD_PRIORITY       1
#define UI_THREAD_PRIORITY          4
#define BUTTON_THREAD_PRIORITY      3

/* Polling Periods */
#define UI_THREAD_PERIOD_MS         30
#define BUTTON_POLL_PERIOD_MS       20

#endif /* APP_CONFIG_H_ */
