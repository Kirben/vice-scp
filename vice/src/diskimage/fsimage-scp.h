/* Read-only native SCP image lifecycle. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_FSIMAGE_SCP_H
#define VICE_FSIMAGE_SCP_H

struct disk_image_s;

/* FLUX_OK, SCP_FILE_NOT_MATCHED, or a negative flux_result_t.
   Call with a read-only file handle. A recognized invalid SCP must not be
   offered to other image probes. The fsimage owns the resulting capture. */
int fsimage_scp_read(struct disk_image_s *image);
void fsimage_scp_dispose(struct disk_image_s *image);

#endif
