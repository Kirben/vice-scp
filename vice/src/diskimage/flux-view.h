/* Independent, read-only DOS sector view. GPL-2.0-or-later. */
#ifndef VICE_FLUX_VIEW_H
#define VICE_FLUX_VIEW_H
#include "flux-capture.h"
typedef struct flux_view_s flux_view_t;
/* Borrows a sealed capture. Destroy before its owner releases the capture. */
flux_view_t *flux_view_create(const flux_capture_t *capture);
void flux_view_destroy(flux_view_t *view);
/* CBMDOS status. First valid header/data checksum in captured order wins.
   One extra loop permits boundary-spanning blocks. No live drive is used.
   Each window is limited to 400 ms; a skipped tail breaks decoding continuity.
   This bounded DOS view may fail on nonstandard/protected directories even
   though the native image is valid and its full timing remains playable. */
int flux_view_read(flux_view_t *view, unsigned int track, unsigned int sector, uint8_t *buffer);
#endif
