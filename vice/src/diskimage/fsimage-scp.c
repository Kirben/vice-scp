/* Read-only native SCP image lifecycle. Part of VICE; GPL-2.0-or-later. */
#include "vice.h"
#include "diskimage.h"
#include "fsimage.h"
#include "fsimage-scp.h"
#include "scp-reader.h"
#include "flux-view.h"

int fsimage_scp_read(disk_image_t *image)
{
    flux_capture_t *capture = NULL;
    fsimage_t *fsimage = image->media.fsimage;
    int rc = scp_file_read(fsimage->fd, &capture);

    if (rc != FLUX_OK) {
        return rc;
    }
    fsimage_scp_dispose(image);
    fsimage->flux = capture;
    image->type = DISK_IMAGE_TYPE_SCP;
    image->read_only = 1;
    image->tracks = FLUX_CAPTURE_TRACKS / 2;
    image->max_half_tracks = FLUX_CAPTURE_TRACKS;
    return FLUX_OK;
}

void fsimage_scp_dispose(disk_image_t *image)
{
    fsimage_t *fsimage = image->media.fsimage;
    flux_view_destroy(fsimage->flux_view);
    fsimage->flux_view = NULL;
    flux_capture_destroy(fsimage->flux);
    fsimage->flux = NULL;
}
