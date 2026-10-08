#ifndef HOST_SD_H_
#define HOST_SD_H_

#include <stdbool.h>
#include <stddef.h>

/* Folder on the PC that stands in for the SD card, and whether the card "mounted". */
extern char g_sd_root[260];
extern bool g_sd_mounted;

void host_rmtree(const char *abs_path);
void host_mkdir(const char *rel_path);
/* Writes @p size bytes at rel_path; the first bytes are "MThd" when size >= 4. */
void host_write_file(const char *rel_path, size_t size);
void host_write_bytes(const char *rel_path, const unsigned char *data, size_t size);

#endif /* HOST_SD_H_ */
