/* Native immutable flux captures. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_FLUX_CAPTURE_H
#define VICE_FLUX_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define FLUX_CAPTURE_TRACKS 84U
#define FLUX_CAPTURE_MAX_REVS 255U
#define FLUX_CAPTURE_MAX_WORDS 32000000U
#define FLUX_CAPTURE_CHECKPOINT_WORDS 1024U
/* One tick is 12.5 ns: 25 ns capture intervals and 16 MHz logic are exact. */
#define FLUX_TICKS_PER_SECOND UINT64_C(80000000)
#define FLUX_DEFAULT_TURN_TICKS UINT64_C(16000000)
#define FLUX_CAPTURE_INDEXED 1U

typedef enum flux_result_e {
    FLUX_OK = 0,
    FLUX_INVALID = -1,
    FLUX_NOMEM = -2,
    FLUX_LIMIT = -3,
    FLUX_IO = -4,
    FLUX_RANGE = -5
} flux_result_t;

typedef struct flux_capture_s flux_capture_t;
typedef struct flux_track_s flux_track_t;

/* Builder copies host-endian packed intervals. A zero word extends the next
   transition by 65536 units; it is never a transition itself. This encoding
   is a provider-neutral storage contract, not a file/transport interface.
   Each turn duration is already expressed in native ticks. */
flux_capture_t *flux_capture_create(unsigned int revolutions, unsigned int flags);
int flux_capture_add_track(flux_capture_t *capture, unsigned int track,
                           const uint64_t *durations, const uint16_t *words,
                           size_t count, uint32_t unit_ticks);
int flux_capture_seal(flux_capture_t *capture);
void flux_capture_destroy(flux_capture_t *capture);
unsigned int flux_capture_revolutions(const flux_capture_t *capture);
unsigned int flux_capture_flags(const flux_capture_t *capture);
int flux_capture_is_sealed(const flux_capture_t *capture);
size_t flux_capture_words(const flux_capture_t *capture);
const flux_track_t *flux_capture_track(const flux_capture_t *capture, unsigned int track);
uint64_t flux_track_boundary(const flux_track_t *track, unsigned int boundary);
uint64_t flux_track_duration(const flux_track_t *track, unsigned int revolution);
uint64_t flux_track_period(const flux_track_t *track);
/* Immutable serialization view for provider-neutral snapshots. */
size_t flux_track_word_count(const flux_track_t *track);
uint32_t flux_track_unit_ticks(const flux_track_t *track);
const uint16_t *flux_track_words(const flux_track_t *track);

/* Decoder cursor uses word offsets and cumulative time, not allocated pulses.
   seek is bounded by one checkpoint block plus the next overflow interval.
   A real edge exactly at the capture endpoint is included; later samples
   are excluded. Playback assigns that endpoint edge to the loop seam. */
typedef struct flux_scan_s {
    size_t word;
    uint64_t time;
} flux_scan_t;
int flux_track_seek(const flux_track_t *track, uint64_t time, int inclusive,
                    flux_scan_t *scan, uint64_t *pulse);
int flux_track_next(const flux_track_t *track, flux_scan_t *scan, uint64_t *pulse);

#endif
