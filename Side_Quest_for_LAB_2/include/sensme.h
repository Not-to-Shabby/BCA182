/**
 * @file sensme.h
 * @brief Sony SensMe acoustic mood classification engine and 8MB SPI NOR Flash database manager.
 *
 * Provides:
 *   1. 12 SensMe mood channels (Morning, Daytime, Evening, Night, Relax, Mellow,
 *      Upbeat, Emotional, Lounge, Dance, Extreme, Acoustic).
 *   2. Fast O(1) 4-microsecond mood lookup directly from onboard W25Q64 SPI NOR Flash.
 *   3. Self-programming sync from SD:/sensme.bin into NOR Flash on boot.
 *   4. Channel filtering for random play and song browsing.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef SENSME_H_
#define SENSME_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSME_MAGIC            0x534E4553U  /**< "SENS" in little-endian ASCII (bytes: 'S', 'E', 'N', 'S') */
#define SENSME_VERSION          1U
#define SENSME_ENTRY_BYTES      4U
#define SENSME_HEADER_BYTES     64U
#define SENSME_FLASH_ADDR       0x080000U   /**< 512 KB offset in W25Q64 NOR Flash (avoids sector 0 OTP lock) */
#define SENSME_SD_PATH          "SD:/sensme.bin"

typedef enum {
    SENSME_CH_ALL = 0,      /**< All Songs / Unfiltered */
    SENSME_CH_MORNING = 1,  /**< Morning: Major key, gentle tempo, waking up */
    SENSME_CH_DAYTIME = 2,  /**< Daytime: Bright, lively pop */
    SENSME_CH_EVENING = 3,  /**< Evening: Warm acoustic, relaxing sunset */
    SENSME_CH_NIGHT = 4,    /**< Night: Slow, minor, moody, after-dark */
    SENSME_CH_RELAX = 5,    /**< Relax: Lowest energy, ambient, soft */
    SENSME_CH_MELLOW = 6,   /**< Mellow: Smooth ballad, moderate energy */
    SENSME_CH_UPBEAT = 7,   /**< Upbeat: High positive valence, cheerful, happy */
    SENSME_CH_EMOTIONAL = 8,/**< Emotional: Deep feeling, minor key, strings/flute */
    SENSME_CH_LOUNGE = 9,   /**< Lounge: Sophisticated jazz harmony, chill */
    SENSME_CH_DANCE = 10,   /**< Dance: Driving rhythm, 4-on-the-floor beat, high BPM */
    SENSME_CH_EXTREME = 11, /**< Extreme: Maximum intensity, fast rock/metal */
    SENSME_CH_ACOUSTIC = 12,/**< Acoustic: Natural piano/guitar/woodwinds */
    SENSME_CH_COUNT = 13
} sensme_channel_t;

typedef struct {
    uint8_t valence;        /**< 0 (Melancholic/Minor) to 255 (Euphoric/Major), 128 = Neutral */
    uint8_t energy;         /**< 0 (Calm/Ambient) to 255 (Intense/Extreme) */
    uint8_t channel;        /**< SensMe Channel ID (1..12) */
    uint8_t bpm;            /**< Song tempo in BPM (40..240) */
} sensme_data_t;

/**
 * @brief Initialize W25Q64 NOR Flash and SensMe engine.
 *        If SD:/sensme.bin exists on the MicroSD card and differs from NOR Flash,
 *        automatically flashes and verifies it in ~1.5 seconds.
 * @return true if SensMe database is ready in NOR Flash.
 */
bool sensme_init(void);

/**
 * @brief Returns whether the SensMe database is available in NOR Flash.
 */
bool sensme_is_ready(void);

/**
 * @brief Returns total songs indexed in the SensMe database.
 */
uint32_t sensme_get_song_count(void);

/**
 * @brief Fast O(1) 4-microsecond lookup of a song's mood coordinates by catalog index.
 * @param catalog_index 0-based index in the song catalog.
 * @param out Destination struct for valence, energy, channel, and BPM.
 * @return true if index was valid and read succeeded.
 */
bool sensme_get_song_data(uint32_t catalog_index, sensme_data_t *out);

/**
 * @brief Get human-readable name of a SensMe channel (e.g. "Energetic", "Relax").
 */
const char *sensme_channel_name(uint8_t channel_id);

/**
 * @brief Get representative 16-bit RGB565 LCD display color for a SensMe channel.
 */
uint16_t sensme_channel_color(uint8_t channel_id);

/**
 * @brief Returns how many songs in the catalog belong to @p channel_id.
 */
uint16_t sensme_get_channel_song_count(uint8_t channel_id);

/**
 * @brief Set active mood channel filter for Random Play and Song Browser (0 = All).
 */
void sensme_set_active_channel(uint8_t channel);

/**
 * @brief Get active mood channel filter.
 */
uint8_t sensme_get_active_channel(void);

/**
 * @brief Check if @p catalog_index matches the currently active mood channel filter.
 */
bool sensme_matches_active_channel(uint32_t catalog_index);

#ifdef __cplusplus
}
#endif

#endif /* SENSME_H_ */
