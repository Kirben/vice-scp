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
/* WD1770 byte stream for one DD revolution, including missing-clock marks. */
#define FLUX_MFM_DD_BYTES 6250U
#define FLUX_MFM_MAX_BYTES (4U * FLUX_MFM_DD_BYTES)
int flux_mfm_decode(const flux_track_t *track, unsigned int rev, unsigned int bytes, uint8_t *data, uint8_t *sync, int *phase_out);
int flux_mfm_write_track(const flux_capture_t *source, unsigned int slot, unsigned int bytes,
                         const uint8_t *data, const uint8_t *sync,
                         unsigned int rev, int phase, unsigned int first, unsigned int length,
                         flux_capture_t **replacement);
int flux_mfm_windows_aligned(const flux_capture_t *capture, unsigned int bytes);
int flux_mfm_probe(const flux_capture_t *capture, unsigned int slot, unsigned int bytes);
int flux_view_probe_8x50(const flux_capture_t *capture, unsigned int track);
int flux_view_probe(const flux_capture_t *capture, unsigned int slot, unsigned int cylinder,
                    unsigned int logical_track);
int flux_view_write_sector(flux_view_t *view, unsigned int track, unsigned int sector,
                           const uint8_t *data, flux_capture_t **replacement);
int flux_view_read_disk_id(flux_view_t *view, unsigned int track, uint8_t id[2]);
int flux_view_read(flux_view_t *view, unsigned int track, unsigned int sector, uint8_t *buffer);
#endif
