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

int fsimage_scp_prepare_embedded_write(disk_image_t *image, int scaled, int gcr_window_alignment)
{
    return FLUX_INVALID;
}

void fsimage_scp_enable_write(disk_image_t *image, int multirev, scp_write_clock_t clock, int flip,
                             int gcr_window_alignment)
{
}

uint64_t fsimage_scp_physical_turn(const disk_image_t *image, unsigned int slot)
{
    return 0;
}

int fsimage_scp_can_write_slot(const disk_image_t *image, unsigned int slot)
{
    return 0;
}

void fsimage_scp_adopt_write(disk_image_t *image, flux_capture_t *capture)
{
    flux_capture_destroy(capture);
}

void fsimage_scp_abort_write(disk_image_t *image)
{
}

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

int fsimage_scp_flush(disk_image_t *image, char **recovery_path)
{
    if (recovery_path != NULL) {
        *recovery_path = NULL;
    }
    return 0;
}

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
    return FLUX_INVALID;
}

/* Build the replacement before the interface processor writes the directory. */
int fsimage_scp_format_gcr(disk_image_t *image, unsigned int format, const uint8_t id[2])
{
    return FLUX_INVALID;
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
