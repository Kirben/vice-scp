/* Native SCP staging writer. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_SCP_WRITER_H
#define VICE_SCP_WRITER_H

#include <stdio.h>
#include "flux-capture.h"

#define SCP_WRSP_EXTENSION_SIZE (16U + 169U * 4U)

/** Serialize a sealed capture into a new, empty binary update stream.
 * The caller owns the stream and publishes it only on success.
 */
int scp_file_write_staged(FILE *file, const flux_capture_t *capture);
/** Write a staged image with its WRSP extension and track range. */
int scp_file_write_staged_wrsp(FILE *file, const flux_capture_t *capture, const uint8_t *extension,
                               const uint8_t *track_range);

/** Check whether the trailer can be relocated when an image is rewritten. */
int scp_file_footer_writable(const uint8_t *trailer, size_t size, uint64_t original_offset);
/** Append the trailer and adjust its offsets for the staged image. */
int scp_file_append_footer(FILE *file, const uint8_t *trailer, size_t size, uint64_t original_offset);

#endif
