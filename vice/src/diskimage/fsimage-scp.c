/* Native SCP image lifecycle. Part of VICE; GPL-2.0-or-later. */
#include "vice.h"
#include "archdep.h"
#include "cbmdos.h"
#include "diskconstants.h"
#include "gcr.h"
#include "diskimage.h"
#include "fsimage.h"
#include "fsimage-scp.h"
#include "scp-reader.h"
#include "scp-writer.h"
#include "flux-view.h"
#include "lib.h"
#include "log.h"
#include "sha1.h"
#include "util.h"
#include "zfile.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifdef WINDOWS_COMPILE
#include <windows.h>
#endif

static int file_identity(FILE *file, uint8_t identity[20])
{
    uint8_t buffer[8192];
    SHA1_CTX context;
    size_t count;
    uint64_t total = 0;
    if (fseek(file, 0, SEEK_SET)) {
        return -1;
    }
    SHA1Init(&context);
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        total += count;
        if (total > SCP_FILE_MAX_BYTES) {
            return -1;
        }
        SHA1Update(&context, buffer, (uint32_t)count);
    }
    if (ferror(file)) {
        return -1;
    }
    SHA1Final(identity, &context);
    return 0;
}

/* Bound quantization to a quarter of the shortest encoded cell. At
   360 RPM the same cells are captured in five sixths of the time. */
static unsigned int scp_write_sample_limit(unsigned int flags, int rpm360)
{
    unsigned int cell_ns = 3250;
    if (flags & FLUX_CAPTURE_MFM) {
        cell_ns = (flags & FLUX_CAPTURE_MFM_ED) ? 500
            : (flags & FLUX_CAPTURE_MFM_HD) ? 1000 : 2000;
    } else if (flags & FLUX_CAPTURE_8X50) {
        cell_ns = 2166;
    }
    return rpm360 ? cell_ns * 5 / 24 : cell_ns / 4;
}

static int measure_scp_turns(const flux_capture_t *capture,
                             uint64_t turns[FLUX_CAPTURE_MAX_TRACKS], int scaled)
{
    unsigned int flags = flux_capture_flags(capture);
    unsigned int heads = (flags & FLUX_CAPTURE_DOUBLE_SIDED) ? 2 : 1;
    int wide = !!(flags & FLUX_CAPTURE_8X50);
    unsigned int cylinders = wide ? 77 : 35;
    unsigned int head;
    unsigned int cylinder;

    for (head = (flags & FLUX_CAPTURE_TOP_ONLY) ? 1 : 0; head < heads; ++head) {
        for (cylinder = 1; cylinder <= cylinders; ++cylinder) {
            unsigned int logical = cylinder + head * cylinders;
            unsigned int slot = wide ? (cylinder - 1) * 2 + head
                : head * FLUX_CAPTURE_TRACKS + (cylinder - 1) * 2;
            unsigned int zone = disk_image_speed_map(wide ? DISK_IMAGE_TYPE_D82 : DISK_IMAGE_TYPE_D64,
                                                      wide ? logical : cylinder);
            unsigned int cell = (16 - zone) * 20;
            const flux_track_t *track;

            if (wide) {
                cell = cell * 2 / 3;
            }
            track = flux_capture_track(capture, slot);
            if (scaled) {
                cell = (cell * 5 + 3) / 6;
            }
            turns[slot] = flux_track_measure_gcr_turn(track, logical, cell);
            if (turns[slot] < (scaled ? 12500000 : 15000000)
                || turns[slot] > (scaled ? 14200000 : 17000000)
                || flux_track_period(track) < turns[slot] * 2) {
                return 0;
            }
        }
    }
    return 1;
}

int fsimage_scp_prepare_embedded_write(disk_image_t *image, int scaled, int gcr_window_alignment)
{
    fsimage_t *fs = image->media.fsimage;
    uint64_t turns[FLUX_CAPTURE_MAX_TRACKS] = {0};
    unsigned int flags = flux_capture_flags(fs->flux);

    if (image->read_only || (flags & FLUX_CAPTURE_INDEXED)
        || flux_capture_revolutions(fs->flux) == 1) {
        return 0;
    }
    if (flags & FLUX_CAPTURE_MFM) {
        unsigned int bytes = FLUX_MFM_DD_BYTES << ((flags & FLUX_CAPTURE_MFM_ED) ? 2
            : (flags & FLUX_CAPTURE_MFM_HD) ? 1 : 0);
        return flux_mfm_windows_aligned(fs->flux, bytes) ? 0 : -1;
    }
    if (!measure_scp_turns(fs->flux, turns, scaled)) {
        return flux_capture_uses_360_clock(fs->flux) && !gcr_window_alignment ? -1 : 0;
    }
    fs->flux_scp_turns = lib_malloc(sizeof(turns));
    memcpy(fs->flux_scp_turns, turns, sizeof(turns));
    return 0;
}

void fsimage_scp_enable_write(disk_image_t *image, int multirev, scp_write_clock_t clock, int flip,
                              int gcr_window_alignment)
{
    fsimage_t *fs = image->media.fsimage;
    uint8_t header[16];
    uint8_t identity[20];
    uint8_t wrsp[SCP_WRSP_EXTENSION_SIZE];
    int has_wrsp = 0;
    FILE *plain;
    int enabled;
    unsigned int slot;
    unsigned int flags = flux_capture_flags(fs->flux);
    int rpm360 = flux_capture_uses_360_clock(fs->flux);
    int unindexed = !(flags & FLUX_CAPTURE_INDEXED);
    int measured = 0;
    int measure_period = clock == SCP_WRITE_MEASURE_CAPTURE_CLOCK || clock == SCP_WRITE_MEASURE_AUTO_CLOCK;
    int mfm = !!(flags & FLUX_CAPTURE_MFM);
    uint64_t turns[FLUX_CAPTURE_MAX_TRACKS] = {0};

    /* Multi-turn images retain a standard SCP checksum and require an
       explicit emulator opt-in. Single-turn images still require MODE. */
    if (!fs->fd
        || (rpm360 && clock == SCP_WRITE_300_ONLY)
        || (rpm360 && unindexed && multirev && !mfm && !measure_period)
        || (multirev != (flux_capture_revolutions(fs->flux) > 1))
        || fseek(fs->fd, 0, SEEK_SET)
        || fread(header, 1, sizeof(header), fs->fd) != sizeof(header)
        || (header[4] != SCP_DISK_TYPE_C64
            && header[4] != SCP_DISK_TYPE_GENERIC
            && !((header[4] == SCP_DISK_TYPE_PC_720K || header[4] == SCP_DISK_TYPE_PC_1440K)
                 && (flags & FLUX_CAPTURE_MFM)))
        || (!multirev && !(header[8] & 0x10)) || 25U * (header[11] + 1U) > scp_write_sample_limit(flags, rpm360)
        || (header[11] && flux_capture_sample_ticks(fs->flux) == 2)) {
        return;
    }
    if ((flags & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_8X50)) == FLUX_CAPTURE_DOUBLE_SIDED) {
        for (slot = 1; slot < FLUX_CAPTURE_MAX_TRACKS; slot += 2) {
            if (flux_capture_track(fs->flux, slot)) {
                return;
            }
        }
    }
    if (multirev && mfm && !(flags & FLUX_CAPTURE_INDEXED)) {
        unsigned int bytes = FLUX_MFM_DD_BYTES << ((flags & FLUX_CAPTURE_MFM_ED) ? 2
            : (flags & FLUX_CAPTURE_MFM_HD) ? 1 : 0);
        if (!flux_mfm_windows_aligned(fs->flux, bytes)) {
            return;
        }
    }
    if (unindexed && multirev && !mfm && measure_period) {
        flux_capture_t *flipped = NULL;
        if (flip && flux_capture_flip_gcr(fs->flux, &flipped) != FLUX_OK) {
            return;
        }
        measured = measure_scp_turns(flipped ? flipped : fs->flux, turns, rpm360 && clock == SCP_WRITE_MEASURE_AUTO_CLOCK);
        flux_capture_destroy(flipped);
        if (rpm360 && !measured && !gcr_window_alignment) {
            return;
        }
    }
    plain = fopen(fs->name, "r+b");
    if (!plain) {
        return;
    }
    enabled = !file_identity(plain, identity)
        && !file_identity(fs->fd, fs->flux_file_identity)
        && !memcmp(identity, fs->flux_file_identity, sizeof(identity));
    fclose(plain);
    if (!enabled) {
        return;
    }
    /* The known WRSP layout has one angular position per SCP table slot,
       independent of track file offsets. Unknown chunks may have offsets. */
    if (!fseek(fs->fd, 688, SEEK_SET) && fread(wrsp, 1, 4, fs->fd) == 4
        && !memcmp(wrsp, "EXTS", 4)) {
        uint8_t table[168 * 4];
        if (fread(wrsp + 4, 1, sizeof(wrsp) - 4, fs->fd) != sizeof(wrsp) - 4
            || util_le_buf_to_dword(wrsp + 4) != sizeof(wrsp) - 8
            || memcmp(wrsp + 8, "WRSP", 4)
            || util_le_buf_to_dword(wrsp + 12) != sizeof(wrsp) - 16
            || util_le_buf_to_dword(wrsp + 16)
            || fseek(fs->fd, 16, SEEK_SET)
            || fread(table, 1, sizeof(table), fs->fd) != sizeof(table)) {
            return;
        }
        for (slot = 0; slot < 168; ++slot) {
            uint32_t offset = util_le_buf_to_dword(table + slot * 4);
            if (offset && offset < 688 + sizeof(wrsp)) {
                return;
            }
        }
        has_wrsp = 1;
    }
    if (header[8] & 0x20) {
        uint64_t offset = fs->flux_scp_trailer_offset;
        long end;
        size_t size;
        size_t first;
        size_t skip;
        unsigned int i;
        uint8_t *trailer;
        uint8_t *footer;
        if (!offset || fseek(fs->fd, 0, SEEK_END) || (end = ftell(fs->fd)) < 0
            || (uint64_t)end < offset + 48 || (uint64_t)end - offset > 1024 * 1024
            || fseek(fs->fd, (long)offset, SEEK_SET)) {
            return;
        }
        size = (size_t)((uint64_t)end - offset);
        trailer = lib_malloc(size);
        if (fread(trailer, 1, size, fs->fd) != size || !scp_file_footer_writable(trailer, size, offset)) {
            lib_free(trailer);
            return;
        }
        first = size - 48;
        footer = trailer + first;
        for (i = 0; i < 24; i += 4) {
            uint32_t address = util_le_buf_to_dword(footer + i);
            if (address && address >= offset && address - offset < first) {
                first = (size_t)(address - offset);
            }
        }
        /* Drop any old MODE padding before the timestamp or first string.
           The staging writer reserves its own space for the last track. */
        for (skip = 0; skip < first && !trailer[skip]; ++skip) {}
        memmove(trailer, trailer + skip, size - skip);
        lib_free(fs->flux_scp_trailer);
        fs->flux_scp_trailer = trailer;
        fs->flux_scp_trailer_offset = offset + skip;
        fs->flux_scp_trailer_size = size - skip;
    }
    if (has_wrsp) {
        lib_free(fs->flux_scp_wrsp);
        fs->flux_scp_wrsp = lib_malloc(sizeof(wrsp));
        memcpy(fs->flux_scp_wrsp, wrsp, sizeof(wrsp));
    }
    if (measured) {
        lib_free(fs->flux_scp_turns);
        fs->flux_scp_turns = lib_malloc(sizeof(turns));
        memcpy(fs->flux_scp_turns, turns, sizeof(turns));
    }
    zfile_fclose(fs->fd);
    fs->fd = NULL;
    fs->flux_only = FSIMAGE_FLUX_FILE;
    memcpy(fs->flux_scp_range, header + 6, sizeof(fs->flux_scp_range));
    fs->flux_scp_flags = header[8] & 0x0f; /* Index, TPI, RPM and normalization. */
    fs->flux_scp_heads = header[10];
    fs->flux_scp_disk_type = header[4];
    image->read_only = 0;
}

uint64_t fsimage_scp_physical_turn(const disk_image_t *image, unsigned int slot)
{
    const fsimage_t *fs = image ? image->media.fsimage : NULL;
    return fs && fs->flux_scp_turns && slot < FLUX_CAPTURE_MAX_TRACKS ? fs->flux_scp_turns[slot] : 0;
}

int fsimage_scp_can_write_slot(const disk_image_t *image, unsigned int slot)
{
    const fsimage_t *fs = image ? image->media.fsimage : NULL;
    if (!fs) {
        return 0;
    }
    return (!fs->flux_scp_turns || fsimage_scp_physical_turn(image, slot))
        && flux_capture_slot_writable(fs->flux, slot)
        && (!fs->flux_scp_physical || (slot < FLUX_CAPTURE_TRACKS && !(slot & 1)));
}

void fsimage_scp_adopt_write(disk_image_t *image, flux_capture_t *capture)
{
    fsimage_t *fs = image->media.fsimage;
    int write_failed = fs->flux_write_failed;
    if (fs->flux_scp_physical) {
        unsigned int slot;
        for (slot = 0; slot < FLUX_CAPTURE_TRACKS; ++slot) {
            const flux_track_t *old = flux_capture_track(fs->flux, slot);
            const flux_track_t *new_track = flux_capture_track(capture, slot);
            size_t count = flux_track_word_count(new_track);
            if ((!old != !new_track) || flux_track_word_count(old) != count
                || (count && memcmp(flux_track_words(old), flux_track_words(new_track),
                                    count * sizeof(uint16_t)))) {
                fs->flux_scp_flipped_dirty[slot] = 1;
            }
        }
    }

    fsimage_scp_dispose(image);
    fs->flux_write_failed = write_failed;
    fs->flux = capture;
    fs->flux_dirty = 1;
}

void fsimage_scp_abort_write(disk_image_t *image)
{
    image->media.fsimage->flux_write_failed = 1;
}

static FILE *open_stage(const char *base, char **path)
{
    unsigned int attempt;

    for (attempt = 0; attempt < 64; ++attempt) {
        int fd;
        int error;
        char *name = attempt ? lib_msprintf("%s-%u", base, attempt) : lib_strdup(base);
        FILE *file;

        fd = open(name, O_RDWR | O_CREAT | O_EXCL
#ifdef O_BINARY
                  | O_BINARY
#endif
                  , 0600);
        if (fd < 0) {
            error = errno;
            lib_free(name);
            if (error == EEXIST) {
                continue;
            }
            break;
        }
        file = archdep_fdopen(fd, "w+b");
        if (file == NULL) {
            close(fd);
            archdep_remove(name);
            lib_free(name);
            return NULL;
        }
        *path = name;
        return file;
    }
    return NULL;
}

/* A checksum-valid side-1 GCR header distinguishes the interleaved
   1571 layout from older single-sided half-track captures with HEADS=0. */
static unsigned int detect_1571_step(const flux_capture_t *capture)
{
    unsigned int cylinder;
    if (flux_view_probe(capture, 35, 18, 53)) {
        return 2;
    }
    if (flux_view_probe(capture, 69, 18, 53)) {
        return 1;
    }
    if (flux_view_probe(capture, 1, 1, 36)) {
        return flux_view_probe(capture, 68, 18, 18) ? 1 : 2;
    }
    for (cylinder = 0; cylinder < 35; cylinder++) {
        if (flux_view_probe(capture, cylinder * 2 + 1, cylinder + 1, cylinder + 36)) {
            return 2;
        }
    }
    return 0;
}

static unsigned int detect_mfm(const flux_capture_t *capture)
{
    static const unsigned int density[] = {0, FLUX_CAPTURE_MFM_HD, FLUX_CAPTURE_MFM_ED};
    unsigned int rate;
    for (rate = 0; rate < sizeof(density) / sizeof(density[0]); rate++) {
        unsigned int bytes = FLUX_MFM_DD_BYTES << rate;
        if (flux_mfm_probe(capture, 78, bytes) || flux_mfm_probe(capture, 79, bytes)
            || flux_mfm_probe(capture, 0, bytes) || flux_mfm_probe(capture, 1, bytes)) {
            return FLUX_CAPTURE_MFM | density[rate];
        }
    }
    return 0;
}

static int write_stage(const fsimage_t *fs, FILE *file, uint8_t identity[20], int *layout_ok)
{
    flux_capture_t *physical = NULL;
    const flux_capture_t *capture = fs->flux;
    int result = FLUX_OK;

    if (fs->flux_scp_physical) {
        result = flux_capture_restore_top(fs->flux_scp_physical, fs->flux,
                                         fs->flux_scp_flipped_dirty, &physical);
        capture = physical;
    }
    if (!result) {
        result = scp_file_write_staged_wrsp(file, capture, fs->flux_scp_wrsp, fs->flux_scp_range);
    }

    if (!result && (fseek(file, 4, SEEK_SET)
        || fputc(fs->flux_scp_disk_type, file) == EOF
        || fseek(file, 8, SEEK_SET)
        || fputc(fs->flux_scp_flags | (flux_capture_revolutions(fs->flux) == 1 ? 0x90 : 0x80), file) == EOF
        || fseek(file, 10, SEEK_SET)
        || fputc(fs->flux_scp_heads, file) == EOF
        || (fs->flux_scp_trailer && scp_file_append_footer(file, fs->flux_scp_trailer,
                 fs->flux_scp_trailer_size, fs->flux_scp_trailer_offset) != FLUX_OK)
        || fflush(file)
        || file_identity(file, identity))) {
        result = -1;
    }
    if (!result && layout_ok) {
        unsigned int flags = flux_capture_flags(capture);
        unsigned int mfm = flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_MFM_HD | FLUX_CAPTURE_MFM_ED);
        int gcr1571 = (flags & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_8X50)) == FLUX_CAPTURE_DOUBLE_SIDED
            && flux_capture_has_second_bank(capture);
        if (mfm || gcr1571 || fs->flux_scp_trailer) {
            flux_capture_t *staged = NULL;
            uint8_t type;
            uint8_t heads;
            /* Keep a recovery file when the staged image loses its
               identifiable encoding or head layout. */
            *layout_ok = scp_file_read_typed(file, &staged, &type, &heads) == FLUX_OK
                && heads == fs->flux_scp_heads
                && (!mfm || detect_mfm(staged) == mfm)
                && (!gcr1571 || heads == 2 || detect_1571_step(staged) == 2);
            flux_capture_destroy(staged);
        }
    }
    flux_capture_destroy(physical);
    if (fclose(file)) {
        result = -1;
    }
    return result;
}

int fsimage_scp_flush(disk_image_t *image, char **recovery_path)
{
    fsimage_t *fs = image->media.fsimage;
    uint8_t identity[20];
    uint8_t staged_identity[20];
    char *base;
    char *stage = NULL;
    FILE *file;
    FILE *original;
    int same_directory;
    int result;
    int layout_ok = 1;

    if (recovery_path != NULL) {
        *recovery_path = NULL;
    }
    if (fs->flux_only == FSIMAGE_FLUX_EMBEDDED) {
        return 0;
    }
    if (!fs->flux_dirty) {
        if (fs->flux_write_failed) {
            log_error(LOG_DEFAULT, "SCP drive write failed without recoverable changes: %s.", fs->name);
            fs->flux_write_failed = 0;
        }
        return 0;
    }

    base = lib_msprintf("%s.vice-write", fs->name);
    file = open_stage(base, &stage);
    lib_free(base);
    same_directory = file != NULL;
    if (file != NULL && write_stage(fs, file, staged_identity, &layout_ok) != 0) {
        archdep_remove(stage);
        lib_free(stage);
        stage = NULL;
        file = NULL;
        same_directory = 0;
    }

    if (!same_directory) {
        base = lib_msprintf("%s/vice-scp-recovery.scp", archdep_user_config_path());
        file = open_stage(base, &stage);
        lib_free(base);
        if (file == NULL) {
            file = archdep_mkstemp_fd(&stage, "w+b");
        }
        if (file == NULL || write_stage(fs, file, staged_identity, NULL) != 0) {
            if (stage != NULL) {
                archdep_remove(stage);
                lib_free(stage);
            }
            log_error(LOG_DEFAULT, "Could not stage or recover SCP writes for %s.", fs->name);
            return -1;
        }
    }

    result = -1;
    if (same_directory && !fs->flux_write_failed && layout_ok) {
        original = fopen(fs->name, "r+b");
        if (original != NULL) {
            int unchanged = !file_identity(original, identity)
                && !memcmp(identity, fs->flux_file_identity, sizeof(identity));
            fclose(original);
            if (unchanged) {
#ifdef WINDOWS_COMPILE
                result = MoveFileExA(stage, fs->name,
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
                result = archdep_rename(stage, fs->name);
#endif
            }
        }
    }

    fs->flux_dirty = 0;
    fs->flux_write_failed = 0;
    if (result != 0) {
        log_error(LOG_DEFAULT, "SCP image was not replaced. Recover the saved image from %s.", stage);
        if (recovery_path != NULL) {
            *recovery_path = stage;
        } else {
            lib_free(stage);
        }
        return 1;
    }
    memcpy(fs->flux_file_identity, staged_identity, sizeof(staged_identity));
    lib_free(stage);
    return 0;
}

/* The SCP tag has no 8x50 identifier. Confirm the faster GCR clock
   and physical slot mapping with a checksum-valid sector. */
static int detect_8x50(const flux_capture_t *capture)
{
    return flux_view_probe_8x50(capture, 39) || flux_view_probe_8x50(capture, 77)
        || flux_view_probe_8x50(capture, 1);
}

int fsimage_scp_read(disk_image_t *image)
{
    flux_capture_t *capture = NULL;
    flux_view_t *view = NULL;
    fsimage_t *fsimage = image->media.fsimage;
    uint8_t disk_type = UINT8_MAX;
    uint8_t heads;
    unsigned int slot;
    unsigned int step = 0;
    int top_only;
    int rc = scp_file_read_metadata(fsimage->fd, &capture, &disk_type, &heads,
                                    &fsimage->flux_scp_trailer_offset);

    if (disk_type != UINT8_MAX) {
        image->type = DISK_IMAGE_TYPE_SCP;
    }
    if (rc != FLUX_OK) {
        return rc;
    }
    top_only = (flux_capture_flags(capture) & FLUX_CAPTURE_TOP_ONLY) != 0;
    if ((disk_type == SCP_DISK_TYPE_C64 || disk_type == SCP_DISK_TYPE_GENERIC)
        && (top_only ? (flux_view_probe_8x50(capture, 116) || flux_view_probe_8x50(capture, 154)
                        || flux_view_probe_8x50(capture, 78)) : detect_8x50(capture))) {
        int double_sided = top_only;
        if (!heads) {
            for (slot = 78; slot <= 154; slot++) {
                if (flux_view_probe_8x50(capture, slot)) {
                    double_sided = 1;
                    break;
                }
            }
        }
        flux_capture_mark_8x50(capture, double_sided);
    }
    if ((!heads || top_only) && disk_type != SCP_DISK_TYPE_C64
        && !(flux_capture_flags(capture) & FLUX_CAPTURE_8X50)) {
        unsigned int mfm = detect_mfm(capture);
        if (mfm) {
            flux_capture_mark_mfm(capture, mfm & ~FLUX_CAPTURE_MFM);
        }
    }
    if ((disk_type == SCP_DISK_TYPE_PC_720K || disk_type == SCP_DISK_TYPE_PC_1440K)
        && !(flux_capture_flags(capture) & FLUX_CAPTURE_MFM)) {
        flux_capture_destroy(capture);
        return FLUX_INVALID;
    }
    if (!(flux_capture_flags(capture) & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))) {
        if (top_only) {
            /* HEADS=2 uses odd interleaved slots, one per cylinder. */
            step = 2;
        } else if (!heads) {
            step = detect_1571_step(capture);
        }
        if (step && flux_capture_split_heads(capture, step) != FLUX_OK) {
            flux_capture_destroy(capture);
            return FLUX_INVALID;
        }
        if (!step) {
            for (slot = FLUX_CAPTURE_TRACKS; slot < FLUX_CAPTURE_MAX_TRACKS; slot++) {
                if (flux_capture_track(capture, slot)) {
                    flux_capture_destroy(capture);
                    return FLUX_INVALID;
                }
            }
        }
    }
    if (top_only && disk_type == SCP_DISK_TYPE_GENERIC
        && !(flux_capture_flags(capture) & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))
        && !flux_view_probe(capture, FLUX_CAPTURE_TRACKS + 34, 18, 53)
        && !flux_view_probe(capture, FLUX_CAPTURE_TRACKS + 68, 35, 70)
        && !flux_view_probe(capture, FLUX_CAPTURE_TRACKS, 1, 36)) {
        flux_capture_t *flipped = NULL;
        int valid = flux_capture_flip_gcr(capture, &flipped) == FLUX_OK
            && (flux_view_probe(flipped, 34, 18, 18) || flux_view_probe(flipped, 0, 1, 1));
        flux_capture_destroy(flipped);
        if (!valid) {
            flux_capture_destroy(capture);
            return FLUX_INVALID;
        }
    }
    if (!top_only && disk_type == SCP_DISK_TYPE_GENERIC && !(flux_capture_flags(capture) & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))) {
        uint8_t block[256];
        view = flux_view_create(capture);
        if (!view) {
            flux_capture_destroy(capture);
            return FLUX_NOMEM;
        }
        /* Greaseweazle's generic tag does not identify an encoding. Require
           a checksum-valid 1541 BAM or first directory sector to mount it. */
        if (flux_view_read(view, 18, 0, block) != CBMDOS_IPE_OK
            && flux_view_read(view, 18, 1, block) != CBMDOS_IPE_OK) {
            flux_view_destroy(view);
            flux_capture_destroy(capture);
            return FLUX_INVALID;
        }
    }
    fsimage_scp_dispose(image);
    fsimage->flux = capture;
    fsimage->flux_view = view;
    image->type = DISK_IMAGE_TYPE_SCP;
    image->read_only = 1;
    image->tracks = FLUX_CAPTURE_TRACKS / 2;
    image->max_half_tracks = FLUX_CAPTURE_TRACKS;
    if (flux_capture_flags(capture) & FLUX_CAPTURE_8X50) {
        image->tracks = (flux_capture_flags(capture) & FLUX_CAPTURE_DOUBLE_SIDED) ? 154 : 77;
        image->max_half_tracks = 154;
    }
    return FLUX_OK;
}

/* Choose a geometry for an empty SCP before the drive starts formatting. */
int fsimage_scp_prepare_blank(disk_image_t *image, unsigned int flags)
{
    fsimage_t *fs = image->media.fsimage;
    unsigned int slot;
    flux_capture_t *blank;
    if (!flags || image->read_only || fs->flux_only != FSIMAGE_FLUX_FILE
        || fs->flux_scp_disk_type != SCP_DISK_TYPE_C64) {
        return FLUX_OK;
    }
    for (slot = 0; slot < FLUX_CAPTURE_MAX_TRACKS; slot++) {
        if (flux_capture_track(fs->flux, slot)) {
            return FLUX_OK;
        }
    }
    blank = flux_capture_create(1, flags | flux_capture_flags(fs->flux));
    if (!blank) {
        return FLUX_NOMEM;
    }
    flux_capture_seal(blank);
    fsimage_scp_dispose(image);
    fs->flux = blank;
    if (flags & FLUX_CAPTURE_MFM) {
        fs->flux_scp_disk_type = SCP_DISK_TYPE_GENERIC;
        fs->flux_scp_heads = 0;
        image->tracks = 80;
        image->max_half_tracks = 160;
    } else if (flags & FLUX_CAPTURE_8X50) {
        image->tracks = (flags & FLUX_CAPTURE_DOUBLE_SIDED) ? 154 : 77;
        image->max_half_tracks = 154;
        fs->flux_scp_heads = (flags & FLUX_CAPTURE_DOUBLE_SIDED) ? 0 : 1;
    }
    return FLUX_OK;
}

/* Build the replacement before the interface processor writes the directory. */
int fsimage_scp_format_gcr(disk_image_t *image, unsigned int format, const uint8_t id[2])
{
    const flux_capture_t *source = disk_image_flux_capture_get(image);
    unsigned int flags = flux_capture_flags(source);
    int rpm360 = flux_capture_uses_360_clock(source);
    int measured = image->media.fsimage->flux_scp_turns != NULL;
    uint64_t duration = rpm360 ? FLUX_DEFAULT_360_TURN_TICKS
                               : FLUX_DEFAULT_TURN_TICKS;
    int wide = format == DISK_IMAGE_TYPE_D80 || format == DISK_IMAGE_TYPE_D82;
    unsigned int tracks = format == DISK_IMAGE_TYPE_D82 ? 154 : wide ? 77 : 35;
    uint64_t durations[FLUX_CAPTURE_MAX_REVS];
    unsigned int revolutions = flux_capture_revolutions(source);
    unsigned int rev;
    unsigned int sample = flux_capture_sample_ticks(source) / 2;
    flux_capture_t *capture;
    unsigned int track;
    int rc = FLUX_OK;

    if (image->read_only || (flags & FLUX_CAPTURE_TOP_ONLY) || wide != !!(flags & FLUX_CAPTURE_8X50)
        || (wide && (format == DISK_IMAGE_TYPE_D82) != !!(flags & FLUX_CAPTURE_DOUBLE_SIDED))) {
        return FLUX_INVALID;
    }
    capture = flux_capture_create(revolutions, flags | (measured ? 0 : FLUX_CAPTURE_INDEXED)
                                  | (rpm360 ? FLUX_CAPTURE_360_RPM : 0));
    if (!capture) {
        return FLUX_NOMEM;
    }
    for (track = 1; track <= tracks; track++) {
        unsigned int zone = disk_image_speed_map(format, track);
        unsigned int sectors = disk_image_sector_per_track(format, track);
        unsigned int length = disk_image_raw_track_size(format, track);
        unsigned int slot = wide ? ((track - 1) % 77) * 2 + (track > 77) : (track - 1) * 2;
        unsigned int numerator = (16 - zone) * (wide ? 20 : 10) * (rpm360 ? 5 : 1);
        unsigned int denominator = (wide ? 3 : 1) * (rpm360 ? 6 : 1);
        const flux_track_t *existing = flux_capture_track(source, slot);
        uint64_t period = measured ? fsimage_scp_physical_turn(image, slot) : duration;
        uint64_t total = 0;
        uint64_t span;
        uint64_t base;
        uint64_t previous = 0;
        uint64_t capacity;
        uint8_t *raw;
        uint16_t *words;
        unsigned int sector;
        unsigned int bit;
        unsigned int count = 0;
        gcr_header_t header = {0, (uint8_t)track, id[1], id[0]};

        if (!period || (measured && !existing)) {
            rc = FLUX_INVALID;
            break;
        }
        for (rev = 0; rev < revolutions; ++rev) {
            durations[rev] = existing ? flux_track_duration(existing, rev) : duration;
            if (!measured && (durations[rev] < 12000000 || durations[rev] > 18000000)) {
                rc = FLUX_INVALID;
                break;
            }
            total += durations[rev];
        }
        if (rc != FLUX_OK) {
            break;
        }
        capacity = (length * 8 + 256) * (measured ? (total - 1) / period + 1 : revolutions);
        if (capacity > FLUX_CAPTURE_MAX_WORDS) {
            rc = FLUX_LIMIT;
            break;
        }
        raw = lib_malloc(length);
        words = lib_malloc((size_t)capacity * sizeof(*words));
        memset(raw, 0x55, length);
        for (sector = 0; sector < sectors; sector++) {
            uint8_t data[256] = {0};
            if (sector == 0 && track == (wide ? HDR_TRACK_8050 : 18)) {
                memcpy(data + (wide ? HDR_ID_8050 : BAM_ID_1541), id, 2);
            }
            header.sector = (uint8_t)sector;
            gcr_convert_sector_to_GCR(data, raw + (sector * length) / sectors,
                                      &header, 8, 5, CBMDOS_FDC_ERR_OK);
        }
        rev = 0;
        for (base = 0; base < total; base += span) {
            /* A measured unindexed capture is continuous. Otherwise start
               a new format pattern at each recorded revolution boundary. */
            span = measured ? period : durations[rev++];
            for (bit = 0; bit < length * 8; bit++) {
                uint64_t offset = ((bit + 1) * numerator + denominator / 2) / denominator;
                if (!measured) {
                    offset = (offset * span + duration / 2) / duration;
                }
                if (offset * 2 > span || base + offset * 2 > total) {
                    break;
                }
                if (raw[bit / 8] & (0x80 >> (bit & 7))) {
                    uint64_t tick = (base / 2 + offset + sample / 2) / sample;
                    uint64_t delta;
                    if (tick <= previous) {
                        continue;
                    }
                    delta = tick - previous;
                    if (!(delta % 65536)) {
                        --delta;
                        --tick;
                    }
                    while (delta > 65535) {
                        words[count++] = 0;
                        delta -= 65536;
                    }
                    words[count++] = (uint16_t)delta;
                    previous = tick;
                }
            }
        }
        rc = flux_capture_add_track(capture, slot, durations, words, count, sample * 2);
        lib_free(words);
        lib_free(raw);
        if (rc != FLUX_OK) {
            break;
        }
    }
    if (rc == FLUX_OK) {
        rc = flux_capture_keep_unwritten(capture, source);
    }
    if (rc == FLUX_OK) {
        flux_capture_seal(capture);
        fsimage_scp_adopt_write(image, capture);
    } else {
        flux_capture_destroy(capture);
    }
    return rc;
}

int fsimage_scp_flip_gcr(disk_image_t *image)
{
    fsimage_t *fs = image->media.fsimage;
    flux_capture_t *flipped;
    int rc = flux_capture_flip_gcr(fs->flux, &flipped);
    if (rc != FLUX_OK) {
        return rc;
    }
    if (!image->read_only) {
        fs->flux_scp_physical = fs->flux;
        fs->flux = NULL;
    }
    fsimage_scp_dispose(image);
    fs->flux = flipped;
    return FLUX_OK;
}

void fsimage_scp_dispose(disk_image_t *image)
{
    fsimage_t *fsimage = image->media.fsimage;
    flux_view_destroy(fsimage->flux_view);
    fsimage->flux_view = NULL;
    flux_capture_destroy(fsimage->flux);
    fsimage->flux = NULL;
    fsimage->flux_dirty = 0;
    fsimage->flux_write_failed = 0;
}
