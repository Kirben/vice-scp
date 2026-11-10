/* Read-only SCP file adapter. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_SCP_READER_H
#define VICE_SCP_READER_H
#include <stdio.h>
#include "flux-capture.h"

#define SCP_FILE_NOT_MATCHED 1
#define SCP_FILE_MAX_BYTES UINT64_C(1073741824)

/* Returns FLUX_OK, SCP_FILE_NOT_MATCHED, or a negative flux_result_t.
   On every failure *capture is NULL. A valid result is sealed and owned by
   the caller; file ownership is unchanged. The adapter never writes. */
int scp_file_read(FILE *file, flux_capture_t **capture);
#endif
