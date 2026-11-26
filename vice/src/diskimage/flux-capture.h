/* Native immutable flux captures. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_FLUX_CAPTURE_H
#define VICE_FLUX_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define FLUX_CAPTURE_TRACKS 84U /* Half-tracks per head. */
#define FLUX_CAPTURE_MAX_TRACKS (2U * FLUX_CAPTURE_TRACKS)
#define FLUX_CAPTURE_MAX_REVS 255U
/* Five ED revolutions at 168 slots can use up to 168 million words. */
#define FLUX_CAPTURE_MAX_WORDS 180000000U
#define FLUX_CAPTURE_CHECKPOINT_WORDS 1024U
/* One tick is 12.5 ns: 25 ns capture intervals and 16 MHz logic are exact. */
#define FLUX_TICKS_PER_SECOND UINT64_C(80000000)
#define FLUX_DEFAULT_TURN_TICKS UINT64_C(16000000)
#define FLUX_DEFAULT_360_TURN_TICKS UINT64_C(13333332)
#define FLUX_CAPTURE_INDEXED 1U
#define FLUX_CAPTURE_360_RPM 2U /* Capture device spindle, not target drive RPM. */
#define FLUX_CAPTURE_DOUBLE_SIDED 4U
#define FLUX_CAPTURE_MFM 8U
#define FLUX_CAPTURE_MFM_HD 16U
#define FLUX_CAPTURE_MFM_ED 32U
#define FLUX_CAPTURE_8X50 64U /* 77 cylinders, SCP interleaved head slots. */
#define FLUX_CAPTURE_TOP_ONLY 128U /* Physical head 1 is the sole captured head. */
#define FLUX_CAPTURE_FLAGS (FLUX_CAPTURE_INDEXED | FLUX_CAPTURE_360_RPM | FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_MFM | FLUX_CAPTURE_MFM_HD | FLUX_CAPTURE_MFM_ED | FLUX_CAPTURE_8X50 | FLUX_CAPTURE_TOP_ONLY)

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
/* Source window lengths retain the original partition of the continuous words. */
int flux_capture_add_track_partitioned(flux_capture_t *capture, unsigned int halftrack,
                                       const uint64_t *durations, const uint16_t *words,
                                       size_t count, uint32_t unit_ticks,
                                       const uint32_t *window_words);
/* Keep the source tracks that a replacement capture did not write. */
int flux_capture_keep_unwritten(flux_capture_t *capture, const flux_capture_t *source);
int flux_capture_seal(flux_capture_t *capture);
/* Normalize interleaved SCP heads before publishing a newly parsed capture. */
void flux_capture_mark_8x50(flux_capture_t *capture, int double_sided);
void flux_capture_mark_mfm(flux_capture_t *capture, unsigned int density);
int flux_capture_split_heads(flux_capture_t *capture, unsigned int halftrack_step);
/* Reflect the recorded pulse stream and expose physical head 1 as head 0. */
int flux_capture_flip_gcr(const flux_capture_t *source, flux_capture_t **result);
int flux_capture_restore_top(const flux_capture_t *physical, const flux_capture_t *flipped,
                             const uint8_t *changed, flux_capture_t **result);
void flux_capture_destroy(flux_capture_t *capture);
unsigned int flux_capture_revolutions(const flux_capture_t *capture);
int flux_capture_slot_writable(const flux_capture_t *capture, unsigned int slot);
int flux_capture_has_second_bank(const flux_capture_t *capture);
unsigned int flux_capture_flags(const flux_capture_t *capture);
/* Playback clock policy. Raw flags and durations are unchanged. */
int flux_capture_uses_360_clock(const flux_capture_t *capture);
int flux_capture_is_sealed(const flux_capture_t *capture);
size_t flux_capture_words(const flux_capture_t *capture);
const flux_track_t *flux_capture_track(const flux_capture_t *capture, unsigned int track);
uint64_t flux_track_boundary(const flux_track_t *track, unsigned int boundary);
uint64_t flux_track_duration(const flux_track_t *track, unsigned int revolution);
uint64_t flux_track_period(const flux_track_t *track);
/* Estimate the physical turn from repeated checksum-valid GCR headers in
   continuous flux. Zero means there is insufficient consistent evidence. */
int flux_track_gcr_write_offsets(const flux_track_t *track, unsigned int logical_track,
                                 unsigned int cell, unsigned int reference_rev,
                                 uint64_t start, int required_sector, uint64_t *offsets);
uint64_t flux_track_measure_gcr_turn(const flux_track_t *track, unsigned int cylinder,
                                     unsigned int cell);
/* Immutable serialization view for provider-neutral snapshots. */
size_t flux_track_word_count(const flux_track_t *track);
const uint32_t *flux_track_window_words(const flux_track_t *track);
uint32_t flux_capture_sample_ticks(const flux_capture_t *capture);
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

/* Splice a capture across all revolutions at an existing or missing half-track.
   Positions are native ticks in (0, duration], sorted ascending.
   The circular write window is (start, end], with start in [0, period) and
   end in (0, period]. Equal endpoints select a whole turn. The source stays
   sealed and unchanged, including on error. */
int flux_capture_splice_shifted(const flux_capture_t *source, unsigned int halftrack,
                               uint64_t duration, uint64_t start, uint64_t end,
                               const uint64_t *pulses, size_t count, const uint64_t *offsets,
                               flux_capture_t **result);
/* An aligned write that wraps continues into the next captured window. */
int flux_capture_splice_continuous(const flux_capture_t *source, unsigned int halftrack,
                                   uint64_t duration, uint64_t start, uint64_t end,
                                   const uint64_t *pulses, size_t count, const uint64_t *offsets,
                                   flux_capture_t **result);
int flux_capture_splice_unindexed(const flux_capture_t *source, unsigned int halftrack,
                                 uint64_t period, uint64_t start, uint64_t end,
                                 const uint64_t *pulses, size_t count, flux_capture_t **result);
int flux_capture_splice(const flux_capture_t *source, unsigned int halftrack,
                        uint64_t duration, uint64_t start, uint64_t end,
                        const uint64_t *pulses, size_t count, flux_capture_t **result);

#endif
