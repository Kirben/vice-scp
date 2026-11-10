/* Instrument platform/file services; all image open/probe/cleanup is production. */
#include "vice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "diskimage.h"
#include "fsimage.h"
#include "fsimage-dxx.h"
#include "fsimage-gcr.h"
#include "fsimage-p64.h"
#include "flux-capture.h"
#include "realimage.h"
#include "log.h"
#include "zfile.h"
#include "archdep.h"
#include "util.h"
#ifdef _WIN32
#include <windows.h>
#endif

static int opens, writable, closes, active, foreign_calls, writable_fail;
static int allocations;
static disk_image_t image;

void lifecycle_reset(void)
{
    opens = writable = closes = foreign_calls = writable_fail = 0;
}
int lifecycle_metric(int n)
{
    switch (n) {
        case 0: return opens;
        case 1: return writable;
        case 2: return closes;
        case 3: return active;
        case 4: return foreign_calls;
        case 5: return allocations;
        default: return -1;
    }
}
void lifecycle_fail_writable(int value) { writable_fail = value; }
void *lib_malloc(size_t size) { void *p = malloc(size); if (!p) { abort(); } allocations++; return p; }
void *lib_calloc(size_t n, size_t size) { void *p = calloc(n, size); if (!p) { abort(); } allocations++; return p; }
void lib_free(void *p) { if (p) { allocations--; free(p); } }
char *lib_strdup(const char *s) { char *p = lib_malloc(strlen(s) + 1); strcpy(p, s); return p; }

FILE *zfile_fopen(const char *name, const char *mode)
{
    FILE *f;
    opens++;
    if (strchr(mode, '+')) { writable++; if (writable_fail) { return NULL; } }
#ifdef _WIN32
    wchar_t path[32768], modes[16];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, path, 32768)
        || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, modes, 16)) { return NULL; }
    f = _wfopen(path, modes);
#else
    f = fopen(name, mode);
#endif
    if (f) { active++; }
    return f;
}
int zfile_fclose(FILE *f) { closes++; active--; return fclose(f); }
int archdep_stat(const char *name, size_t *size, unsigned int *isdir)
{
    struct stat st;
    if (stat(name, &st)) { return -1; }
    *size = (size_t)st.st_size;
    *isdir = (st.st_mode & S_IFDIR) != 0;
    return 0;
}
off_t archdep_file_size(FILE *f)
{
    long pos = ftell(f), size;
    if (fseek(f, 0, SEEK_END)) { return -1; }
    size = ftell(f);
    if (fseek(f, pos, SEEK_SET)) { return -1; }
    return size;
}
int archdep_fseeko(FILE *f, off_t offset, int origin) { return fseek(f, (long)offset, origin); }
int util_fpread(FILE *f, void *buf, size_t count, long offset)
{
    return fseek(f, (long)offset, SEEK_SET) || fread(buf, 1, count, f) != count ? -1 : 0;
}
const char *util_get_extension(const char *s) { const char *p = strrchr(s, '.'); return p ? p + 1 : ""; }
int machine_drive_rom_check_loaded(unsigned int type) { return -1; }
int crt_probe(const char *name) { return 0; }
log_t log_open(const char *id) { return LOG_DEFAULT; }
int log_error(log_t log, const char *format, ...) { return 0; }
int log_message(log_t log, const char *format, ...) { return 0; }
int log_verbose(log_t log, const char *format, ...) { return 0; }

/* Other format engines must never be called for a native SCP. */
#define IMAGE_CALL(name) int name(const disk_image_t *i) { foreign_calls++; return -1; }
IMAGE_CALL(fsimage_read_p64_image)
IMAGE_CALL(fsimage_write_p64_image)
IMAGE_CALL(fsimage_read_gcr_image)
IMAGE_CALL(fsimage_read_dxx_image)
#define SECTOR_READ(name) int name(const disk_image_t *i, uint8_t *b, const disk_addr_t *a) { foreign_calls++; return -1; }
#define SECTOR_WRITE(name) int name(disk_image_t *i, const uint8_t *b, const disk_addr_t *a) { foreign_calls++; return -1; }
SECTOR_READ(fsimage_dxx_read_sector)
SECTOR_READ(fsimage_gcr_read_sector)
SECTOR_READ(fsimage_p64_read_sector)
SECTOR_WRITE(fsimage_dxx_write_sector)
SECTOR_WRITE(fsimage_gcr_write_sector)
SECTOR_WRITE(fsimage_p64_write_sector)
#define TRACK_WRITE(name) int name(disk_image_t *i, unsigned int h, const struct disk_track_s *r) { foreign_calls++; return -1; }
TRACK_WRITE(fsimage_dxx_write_half_track)
TRACK_WRITE(fsimage_gcr_write_half_track)
TRACK_WRITE(fsimage_p64_write_half_track)
SECTOR_READ(realimage_read_sector)
SECTOR_WRITE(realimage_write_sector)
int realimage_open(disk_image_t *i) { foreign_calls++; return -1; }
int realimage_close(disk_image_t *i) { foreign_calls++; return -1; }
void realimage_media_create(disk_image_t *i) { foreign_calls++; }
void realimage_media_destroy(disk_image_t *i) { foreign_calls++; }
void realimage_init(void) {}
void fsimage_create_init(void) {}
void fsimage_dxx_init(void) {}
void fsimage_gcr_init(void) {}
void fsimage_p64_init(void) {}
int fsimage_create(const char *name, unsigned int type) { return -1; }
int fsimage_create_dxm(const char *name, const char *label, unsigned int type) { return -1; }
int fsimage_create_dhd(const char *name, const char *label, unsigned int type) { return -1; }

int lifecycle_open(const char *name, int readonly, int probe)
{
    memset(&image, 0, sizeof(image));
    image.device = DISK_IMAGE_DEVICE_FS;
    image.read_only = readonly;
    disk_image_media_create(&image);
    disk_image_fsimage_name_set(&image, name);
    return probe ? fsimage_open_probe(&image) : disk_image_open(&image);
}
int lifecycle_type(void) { return image.type; }
int lifecycle_readonly(void) { return image.read_only; }
int lifecycle_capture(void) { return disk_image_flux_capture_get(&image) != NULL; }
int lifecycle_read_image(void) { return disk_image_read_image(&image); }
int lifecycle_close(void) { return disk_image_close(&image); }
void lifecycle_destroy(void) { disk_image_media_destroy(&image); }
int lifecycle_write(int which)
{
    uint8_t buf[256] = {0};
    disk_addr_t addr = {1, 0};
    /* Deliberately clear the advisory flag: SCP's format remains protected. */
    image.read_only = 0;
    switch (which) {
        case 0: return disk_image_write_sector(&image, buf, &addr);
        case 1: return disk_image_write_half_track(&image, 2, NULL);
        case 2: return disk_image_write_p64_image(&image);
        case 3: return fsimage_write_sector(&image, buf, &addr);
        default: return -2;
    }
}
