/* Native SCP image lifecycle. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_FSIMAGE_SCP_H
#define VICE_FSIMAGE_SCP_H

#include <stdint.h>

struct disk_image_s;
struct flux_capture_s;

/* FLUX_OK, SCP_FILE_NOT_MATCHED, or a negative flux_result_t.
   The fsimage owns the resulting capture. */
int fsimage_scp_read(struct disk_image_s *image);
int fsimage_scp_flip_gcr(struct disk_image_s *image);
int fsimage_scp_prepare_blank(struct disk_image_s *image, unsigned int flags);
int fsimage_scp_format_gcr(struct disk_image_s *image, unsigned int format, const uint8_t id[2]);
void fsimage_scp_dispose(struct disk_image_s *image);
typedef enum scp_write_clock_e {
    SCP_WRITE_300_ONLY,
    SCP_WRITE_CAPTURE_TIMING,
    SCP_WRITE_MEASURE_CAPTURE_CLOCK,
    SCP_WRITE_MEASURE_AUTO_CLOCK
} scp_write_clock_t;

void fsimage_scp_enable_write(struct disk_image_s *image, int multirev,
                             scp_write_clock_t clock, int flip,
                              int gcr_window_alignment);
int fsimage_scp_prepare_embedded_write(struct disk_image_s *image, int scaled,
                                      int gcr_window_alignment);
uint64_t fsimage_scp_physical_turn(const struct disk_image_s *image, unsigned int slot);
int fsimage_scp_can_write_slot(const struct disk_image_s *image, unsigned int slot);
void fsimage_scp_adopt_write(struct disk_image_s *image, struct flux_capture_s *capture);
void fsimage_scp_abort_write(struct disk_image_s *image);
/* Zero on publication or a clean image, one if changes were recovered to
   a separate file, or negative if changes could not be saved. The optional
   recovery path is owned by the caller. */
int fsimage_scp_flush(struct disk_image_s *image, char **recovery_path);

#endif
