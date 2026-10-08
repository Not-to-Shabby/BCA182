/* Host stand-in for the FatFs calls used by karaoke_catalog.c.
 * "SD:/..." paths map onto the folder named by g_sd_root (see host_sd.h). */
#ifndef STUB_FF_H_
#define STUB_FF_H_

#include <stdio.h>

typedef unsigned char BYTE;
typedef unsigned int UINT;
typedef unsigned long FSIZE_t;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_NO_FILE,
    FR_NO_PATH
} FRESULT;

#define FA_READ 0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10

typedef struct {
    FILE *fp;
    FSIZE_t objsize;
} FIL;

typedef struct {
    void *handle;
    char path[260];
} DIR;

typedef struct {
    FSIZE_t fsize;
    BYTE fattrib;
    char fname[256];
} FILINFO;

#define f_size(fp) ((fp)->objsize)

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_opendir(DIR *dp, const char *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

#endif /* STUB_FF_H_ */
