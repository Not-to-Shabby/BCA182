/* Host implementation of the FatFs stand-in: maps "SD:/..." onto g_sd_root. */

#define DIR HOST_DIR
#include <dirent.h>
#undef DIR

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "ff.h"
#include "host_sd.h"
#include "sd_card_reader.h"

char g_sd_root[260] = "sd_mock_tmp";
bool g_sd_mounted = true;

bool sd_card_is_mounted(void)
{
    return g_sd_mounted;
}

static void map_path(const char *fs_path, char *out, size_t n)
{
    const char *p = fs_path;
    if (strncmp(p, "SD:", 3) == 0) {
        p += 3;
    }
    snprintf(out, n, "%s%s", g_sd_root, p);
}

static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

void host_mkdir(const char *rel_path)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_sd_root, rel_path);
    make_dir(path);
}

void host_write_bytes(const char *rel_path, const unsigned char *data, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_sd_root, rel_path);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

void host_write_file(const char *rel_path, size_t size)
{
    unsigned char *data = calloc(size > 0 ? size : 1, 1);
    if (size >= 4) {
        memcpy(data, "MThd", 4);
    }
    host_write_bytes(rel_path, data, size);
    free(data);
}

void host_rmtree(const char *abs_path)
{
    HOST_DIR *d = opendir(abs_path);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", abs_path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            host_rmtree(child);
        } else {
            remove(child);
        }
    }
    closedir(d);
    rmdir(abs_path);
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode)
{
    (void)mode;
    char host[512];
    map_path(path, host, sizeof(host));
    fp->fp = fopen(host, "rb");
    if (fp->fp == NULL) {
        return FR_NO_FILE;
    }
    fseek(fp->fp, 0, SEEK_END);
    fp->objsize = (FSIZE_t)ftell(fp->fp);
    fseek(fp->fp, 0, SEEK_SET);
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    if (fp->fp != NULL) {
        fclose(fp->fp);
        fp->fp = NULL;
    }
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br)
{
    *br = (UINT)fread(buf, 1, btr, fp->fp);
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    return fseek(fp->fp, (long)ofs, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_opendir(DIR *dp, const char *path)
{
    map_path(path, dp->path, sizeof(dp->path));
    dp->handle = opendir(dp->path);
    return dp->handle != NULL ? FR_OK : FR_NO_PATH;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    struct dirent *e;
    while ((e = readdir((HOST_DIR *)dp->handle)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", dp->path, e->d_name);
        struct stat st;
        fno->fattrib = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? AM_DIR : 0;
        snprintf(fno->fname, sizeof(fno->fname), "%s", e->d_name);
        return FR_OK;
    }
    fno->fname[0] = '\0';
    return FR_OK;
}

FRESULT f_closedir(DIR *dp)
{
    if (dp->handle != NULL) {
        closedir((HOST_DIR *)dp->handle);
        dp->handle = NULL;
    }
    return FR_OK;
}
