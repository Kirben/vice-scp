/* Read-only SCP file adapter. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_SCP_READER_H
#define VICE_SCP_READER_H
#include <stdio.h>
#include "flux-capture.h"

#define SCP_FILE_NOT_MATCHED 1
#define SCP_FILE_MAX_BYTES UINT64_C(1073741824)
#define SCP_DISK_TYPE_C64 0x00
#define SCP_DISK_TYPE_PC_720K 0x31
#define SCP_DISK_TYPE_PC_1440K 0x33
#define SCP_DISK_TYPE_GENERIC 0x80

/** Returns FLUX_OK, SCP_FILE_NOT_MATCHED, or a negative flux_result_t.
   On every failure *capture is NULL. A valid result is sealed and owned by
   the caller; file ownership is unchanged. The adapter never writes. */
int scp_file_read(FILE *file, flux_capture_t **capture);
/** Opt in to generic flux. A generic result needs independent format
   identification before it is mounted as a C64 disk. */
int scp_file_read_metadata(FILE *file, flux_capture_t **capture, uint8_t *disk_type,
                           uint8_t *heads, uint64_t *trailer);
/** Read a capture and return its SCP disk type and head selector. */
int scp_file_read_typed(FILE *file, flux_capture_t **capture, uint8_t *disk_type, uint8_t *heads);
#endif
