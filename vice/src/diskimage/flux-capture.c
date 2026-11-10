/* Native immutable flux captures. Part of VICE; GPL-2.0-or-later. */
#include "flux-capture.h"
#include <stdlib.h>
#include <string.h>

typedef struct flux_checkpoint_s {
    size_t word;
    uint64_t time;
    size_t next_word;
    uint64_t next_time;
} flux_checkpoint_t;

struct flux_track_s {
    uint16_t *words;
    size_t count;
    uint32_t unit_ticks;
    uint64_t boundaries[FLUX_CAPTURE_MAX_REVS + 1];
    unsigned int revolutions;
    flux_checkpoint_t *checkpoints;
    size_t checkpoint_count;
};

struct flux_capture_s {
    flux_track_t *tracks[FLUX_CAPTURE_TRACKS];
    unsigned int revolutions;
    unsigned int flags;
    size_t words;
    int sealed;
};

flux_capture_t *flux_capture_create(unsigned int revolutions, unsigned int flags)
{
    flux_capture_t *capture;
    if (!revolutions || revolutions > FLUX_CAPTURE_MAX_REVS
        || (flags & ~FLUX_CAPTURE_INDEXED)) {
        return NULL;
    }
    capture = calloc(1, sizeof(*capture));
    if (capture) {
        capture->revolutions = revolutions;
        capture->flags = flags;
    }
    return capture;
}

static void track_destroy(flux_track_t *track)
{
    if (track) {
        free(track->words);
        free(track->checkpoints);
        free(track);
    }
}

void flux_capture_destroy(flux_capture_t *capture)
{
    unsigned int i;
    if (capture) {
        for (i = 0; i < FLUX_CAPTURE_TRACKS; i++) {
            track_destroy(capture->tracks[i]);
        }
        free(capture);
    }
}

int flux_capture_add_track(flux_capture_t *capture, unsigned int halftrack,
                           const uint64_t *durations, const uint16_t *words,
                           size_t count, uint32_t unit_ticks)
{
    flux_track_t *track;
    unsigned int rev;
    size_t i, checkpoint = 0, next_word = 0;
    uint64_t time = 0, delta, next_time = 0;
    if (!capture || !capture->revolutions || capture->revolutions > FLUX_CAPTURE_MAX_REVS
        || capture->sealed || halftrack >= FLUX_CAPTURE_TRACKS
        || capture->tracks[halftrack] || !durations || !unit_ticks
        || (count && !words)) {
        return FLUX_INVALID;
    }
    if (count > FLUX_CAPTURE_MAX_WORDS - capture->words) {
        return FLUX_LIMIT;
    }
    track = calloc(1, sizeof(*track));
    if (!track) {
        return FLUX_NOMEM;
    }
    track->revolutions = capture->revolutions;
    track->unit_ticks = unit_ticks;
    track->count = count;
    for (rev = 0; rev < track->revolutions; rev++) {
        if (!durations[rev] || durations[rev] > UINT64_MAX - track->boundaries[rev]) {
            track_destroy(track);
            return FLUX_INVALID;
        }
        track->boundaries[rev + 1] = track->boundaries[rev] + durations[rev];
    }
    if (count) {
        track->checkpoint_count = count / FLUX_CAPTURE_CHECKPOINT_WORDS + 1;
        track->checkpoints = calloc(track->checkpoint_count, sizeof(*track->checkpoints));
        track->words = malloc(count * sizeof(*track->words));
    }
    if (count && (!track->checkpoints || !track->words)) {
        track_destroy(track);
        return FLUX_NOMEM;
    }
    for (i = 0; i < count; i++) {
        if (!(i % FLUX_CAPTURE_CHECKPOINT_WORDS)) {
            track->checkpoints[checkpoint].word = i;
            track->checkpoints[checkpoint++].time = time;
        }
        delta = (uint64_t)(words[i] ? words[i] : 65536U) * unit_ticks;
        if (delta > UINT64_MAX - time) {
            track_destroy(track);
            return FLUX_INVALID;
        }
        time += delta;
        track->words[i] = words[i];
    }
    /* The final checkpoint represents the stream end, including empty tracks. */
    if (count && !(count % FLUX_CAPTURE_CHECKPOINT_WORDS)) {
        track->checkpoints[checkpoint].word = count;
        track->checkpoints[checkpoint].time = time;
    }
    /* Also index the next real edge, so a long overflow-only region never
       requires a linear scan in the playback loop. */
    for (i = count; i > 0; i--) {
        if (words[i - 1]) {
            next_word = i;
            next_time = time;
        }
        if (!((i - 1) % FLUX_CAPTURE_CHECKPOINT_WORDS)) {
            checkpoint = (i - 1) / FLUX_CAPTURE_CHECKPOINT_WORDS;
            track->checkpoints[checkpoint].next_word = next_word;
            track->checkpoints[checkpoint].next_time = next_time;
        }
        time -= (uint64_t)(words[i - 1] ? words[i - 1] : 65536U) * unit_ticks;
    }
    capture->tracks[halftrack] = track;
    capture->words += count;
    return FLUX_OK;
}

int flux_capture_seal(flux_capture_t *capture)
{
    if (!capture) {
        return FLUX_INVALID;
    }
    capture->sealed = 1;
    return FLUX_OK;
}

unsigned int flux_capture_revolutions(const flux_capture_t *capture)
{
    return capture ? capture->revolutions : 0;
}

unsigned int flux_capture_flags(const flux_capture_t *capture)
{
    return capture ? capture->flags : 0;
}

int flux_capture_is_sealed(const flux_capture_t *capture)
{
    return capture && capture->sealed;
}

size_t flux_capture_words(const flux_capture_t *capture)
{
    return capture ? capture->words : 0;
}

const flux_track_t *flux_capture_track(const flux_capture_t *capture, unsigned int halftrack)
{
    return capture && capture->sealed && halftrack < FLUX_CAPTURE_TRACKS
        ? capture->tracks[halftrack] : NULL;
}

uint64_t flux_track_boundary(const flux_track_t *track, unsigned int boundary)
{
    return track && boundary <= track->revolutions ? track->boundaries[boundary] : 0;
}

uint64_t flux_track_duration(const flux_track_t *track, unsigned int revolution)
{
    return track && revolution < track->revolutions
        ? track->boundaries[revolution + 1] - track->boundaries[revolution] : 0;
}

uint64_t flux_track_period(const flux_track_t *track)
{
    return track ? track->boundaries[track->revolutions] : 0;
}

int flux_track_next(const flux_track_t *track, flux_scan_t *scan, uint64_t *pulse)
{
    uint16_t word;
    if (!track || !scan || !pulse) {
        return 0;
    }
    while (scan->word < track->count) {
        if (!(scan->word % FLUX_CAPTURE_CHECKPOINT_WORDS)) {
            const flux_checkpoint_t *checkpoint = &track->checkpoints[scan->word / FLUX_CAPTURE_CHECKPOINT_WORDS];
            if (!checkpoint->next_word || checkpoint->next_time > flux_track_period(track)) {
                scan->word = track->count;
                return 0;
            }
            scan->word = checkpoint->next_word;
            scan->time = checkpoint->next_time;
            *pulse = scan->time;
            return 1;
        }
        word = track->words[scan->word++];
        {
            uint64_t delta = (uint64_t)(word ? word : 65536U) * track->unit_ticks;
            if (delta > UINT64_MAX - scan->time) {
                scan->word = track->count;
                return 0;
            }
            scan->time += delta;
        }
        if (scan->time > flux_track_period(track)) {
            scan->word = track->count;
            return 0;
        }
        if (word) {
            *pulse = scan->time;
            return 1;
        }
    }
    return 0;
}

int flux_track_seek(const flux_track_t *track, uint64_t time, int inclusive,
                    flux_scan_t *scan, uint64_t *pulse)
{
    size_t lo = 0, hi, mid;
    if (!track || !scan || !pulse || !track->count || time > flux_track_period(track)) {
        return 0;
    }
    hi = track->checkpoint_count;
    /* Strictly before the target: a pulse at a checkpoint time must not be
       skipped when seeking inclusively to an exact index/head position. */
    while (lo + 1 < hi) {
        mid = lo + (hi - lo) / 2;
        if (track->checkpoints[mid].time < time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    scan->word = track->checkpoints[lo].word;
    scan->time = track->checkpoints[lo].time;
    while (flux_track_next(track, scan, pulse)) {
        if (*pulse > time || (inclusive && *pulse == time)) {
            return 1;
        }
    }
    return 0;
}

size_t flux_track_word_count(const flux_track_t *track)
{
    return track ? track->count : 0;
}

uint32_t flux_track_unit_ticks(const flux_track_t *track)
{
    return track ? track->unit_ticks : 0;
}

const uint16_t *flux_track_words(const flux_track_t *track)
{
    return track ? track->words : NULL;
}
