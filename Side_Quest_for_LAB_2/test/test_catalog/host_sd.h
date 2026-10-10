#ifndef HOST_SD_H_
#define HOST_SD_H_

#include <stdbool.h>
#include <stddef.h>

/* Folder on the PC that stands in for the SD card, and whether the card "mounted". */
extern char g_sd_root[260];
extern bool g_sd_mounted;

/* Fault injection, modelled on what the real SDIO driver does after a bad transfer:
 *  g_sd_fail_reads  the next N reads fail, and the failed file keeps its error until reopened
 *  g_sd_stuck       every read and open fails until sd_card_recover() is called
 *  g_sd_recovers    how many times sd_card_recover() ran
 *  g_sd_recover_ok  what sd_card_recover() reports */
extern unsigned g_sd_fail_reads;
extern bool g_sd_stuck;
extern unsigned g_sd_recovers;
extern bool g_sd_recover_ok;

void host_rmtree(const char *abs_path);
void host_mkdir(const char *rel_path);
/* Writes @p size bytes at rel_path; the first bytes are "MThd" when size >= 4. */
void host_write_file(const char *rel_path, size_t size);
void host_write_bytes(const char *rel_path, const unsigned char *data, size_t size);

#endif /* HOST_SD_H_ */
