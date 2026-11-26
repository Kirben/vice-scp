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
    size_t references;
    uint16_t *words;
    size_t count;
    uint32_t *window_words;
    uint32_t unit_ticks;
    uint32_t reflected_write_grid;
    uint64_t boundaries[FLUX_CAPTURE_MAX_REVS + 1];
    unsigned int revolutions;
    flux_checkpoint_t *checkpoints;
    size_t checkpoint_count;
};

struct flux_capture_s {
    flux_track_t *tracks[FLUX_CAPTURE_MAX_TRACKS];
    unsigned int revolutions;
    unsigned int flags;
    size_t words;
    int sealed;
};

flux_capture_t *flux_capture_create(unsigned int revolutions, unsigned int flags)
{
    flux_capture_t *capture;
    if (!revolutions || revolutions > FLUX_CAPTURE_MAX_REVS
        || (flags & ~FLUX_CAPTURE_FLAGS)) {
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
    if (track && --track->references == 0) {
        free(track->window_words);
        free(track->words);
        free(track->checkpoints);
        free(track);
    }
}

void flux_capture_destroy(flux_capture_t *capture)
{
    unsigned int i;
    if (capture) {
        for (i = 0; i < FLUX_CAPTURE_MAX_TRACKS; i++) {
            track_destroy(capture->tracks[i]);
        }
        free(capture);
    }
}

int flux_capture_add_track_partitioned(flux_capture_t *capture, unsigned int halftrack,
                                       const uint64_t *durations, const uint16_t *words,
                                       size_t count, uint32_t unit_ticks,
                                       const uint32_t *window_words)
{
    flux_track_t *track;
    unsigned int rev;
    size_t i;
    size_t checkpoint = 0;
    size_t next_word = 0;
    uint64_t time = 0;
    uint64_t delta;
    uint64_t next_time = 0;
    if (!capture || !capture->revolutions || capture->revolutions > FLUX_CAPTURE_MAX_REVS
        || capture->sealed || halftrack >= FLUX_CAPTURE_MAX_TRACKS
        || capture->tracks[halftrack] || !durations || !unit_ticks
        || (count && !words)) {
        return FLUX_INVALID;
    }
    if (count > FLUX_CAPTURE_MAX_WORDS - capture->words) {
        return FLUX_LIMIT;
    }
    if (window_words) {
        size_t remaining = count;
        for (rev = 0; rev < capture->revolutions; ++rev) {
            if (window_words[rev] > remaining) {
                return FLUX_INVALID;
            }
            remaining -= window_words[rev];
        }
        if (remaining) {
            return FLUX_INVALID;
        }
    }
    track = calloc(1, sizeof(*track));
    if (!track) {
        return FLUX_NOMEM;
    }
    track->references = 1;
    track->revolutions = capture->revolutions;
    track->unit_ticks = unit_ticks;
    track->count = count;
    if (window_words && track->revolutions > 1) {
        size_t bytes = track->revolutions * sizeof(*window_words);
        track->window_words = malloc(bytes);
        if (!track->window_words) {
            track_destroy(track);
            return FLUX_NOMEM;
        }
        memcpy(track->window_words, window_words, bytes);
    }
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

int flux_capture_add_track(flux_capture_t *capture, unsigned int halftrack,
                           const uint64_t *durations, const uint16_t *words,
                           size_t count, uint32_t unit_ticks)
{
    return flux_capture_add_track_partitioned(capture, halftrack, durations, words,
                                              count, unit_ticks, NULL);
}

const uint32_t *flux_track_window_words(const flux_track_t *track)
{
    return track->window_words;
}

void flux_capture_mark_8x50(flux_capture_t *capture, int double_sided)
{
    capture->flags |= FLUX_CAPTURE_8X50;
    if (double_sided) {
        capture->flags |= FLUX_CAPTURE_DOUBLE_SIDED;
    }
}

void flux_capture_mark_mfm(flux_capture_t *capture, unsigned int density)
{
    capture->flags |= FLUX_CAPTURE_MFM | density;
}

int flux_capture_split_heads(flux_capture_t *capture, unsigned int halftrack_step)
{
    flux_track_t *tracks[FLUX_CAPTURE_MAX_TRACKS] = {0};
    unsigned int i;
    unsigned int halftrack;
    for (i = 0; i < FLUX_CAPTURE_MAX_TRACKS; i++) {
        if (!capture->tracks[i]) {
            continue;
        }
        halftrack = (i / 2) * halftrack_step;
        if (halftrack >= FLUX_CAPTURE_TRACKS) {
            return FLUX_INVALID;
        }
        tracks[(i & 1) * FLUX_CAPTURE_TRACKS + halftrack] = capture->tracks[i];
    }
    memcpy(capture->tracks, tracks, sizeof(tracks));
    capture->flags |= FLUX_CAPTURE_DOUBLE_SIDED;
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

static int splice_contains(uint64_t position, uint64_t start, uint64_t end)
{
    if (start < end) {
        return position > start && position <= end;
    }
    return position > start || position <= end;
}

uint32_t flux_capture_sample_ticks(const flux_capture_t *capture)
{
    unsigned int slot;
    for (slot = 0; slot < FLUX_CAPTURE_MAX_TRACKS; ++slot) {
        const flux_track_t *track = flux_capture_track(capture, slot);
        if (track) {
            return track->unit_ticks;
        }
    }
    return 2;
}

/* Scale sample positions without rounding across the index boundary. SCP
   durations are 32-bit sample counts and the writable resolution is 25 ns. */
static uint64_t splice_scale(uint64_t position, uint64_t duration, uint64_t reference)
{
    return ((position / 2) * (duration / 2) / (reference / 2)) * 2;
}

/* NULL words counts the packed intervals before allocation. Keep a continuous
   stream across index boundaries so the first delta in a turn is preserved. */
static size_t splice_split(const uint64_t *pulses, size_t count, uint64_t limit)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (pulses[middle] <= limit) {
            low = middle + 1;
        }
        else {
            high = middle;
        }
    }
    return low;
}

typedef struct splice_region_s {
    uint64_t from;
    uint64_t to;
    uint64_t offset;
} splice_region_t;

/* Clip each adjacent occurrence of the write to this recorded window.
   A shifted write can start or finish in a different window. */
static unsigned int splice_window_regions(splice_region_t regions[4], unsigned int rev,
                                          unsigned int revolutions, uint64_t duration,
                                          uint64_t reference, uint64_t start, uint64_t end,
                                          const uint64_t *offsets)
{
    unsigned int count = 0;
    int relative;

    for (relative = -2; relative <= 1; ++relative) {
        unsigned int origin = (rev + revolutions + relative) % revolutions;
        uint64_t offset = offsets[origin];
        int64_t shift = (int64_t)offset;
        int64_t from;
        int64_t to;

        if (offset > reference / 2) {
            shift -= (int64_t)reference;
        }
        shift += relative * (int64_t)reference;
        from = (int64_t)start + shift;
        to = (int64_t)end + shift + (end <= start ? (int64_t)reference : 0);
        if (from >= (int64_t)reference || to <= 0) {
            continue;
        }
        if (from < 0) {
            from = 0;
        }
        if (to > (int64_t)reference) {
            to = (int64_t)reference;
        }
        regions[count].from = splice_scale((uint64_t)from, duration, reference);
        regions[count].to = splice_scale((uint64_t)to, duration, reference);
        regions[count].offset = offset;
        if (regions[count].from < regions[count].to) {
            ++count;
        }
    }
    return count;
}

static int splice_words(const flux_track_t *track, const uint64_t *durations,
                        unsigned int revolutions, uint64_t reference, uint64_t physical_period, const uint64_t *offsets,
                        int continuous, uint64_t start, uint64_t end, const uint64_t *pulses,
                        size_t count, uint16_t *words, size_t *length)
{
    uint64_t time = 0;
    uint64_t previous = 0;
    uint64_t base = 0;
    size_t word = 0;
    size_t used = 0;
    unsigned int rev = 0;
    unsigned int unit = track->unit_ticks;
    uint64_t total = flux_track_period(track);
    unsigned int grid = track->reflected_write_grid ? track->reflected_write_grid : unit;
    uint64_t phase = track->reflected_write_grid ? total % grid : 0;

    while (physical_period ? base < total : rev < revolutions) {
        uint64_t duration = physical_period ? physical_period : durations[rev++];
        uint64_t limit = base + duration;
        int last_window = physical_period && limit >= total;
        splice_region_t regions[4];
        unsigned int region = 0;
        unsigned int region_count = 1;
        uint64_t next = 0;
        uint64_t offset;
        size_t replacement = 0;
        size_t split;

        if (last_window) {
            limit = total;
        }
        if (continuous) {
            region_count = splice_window_regions(regions, rev - 1, revolutions, duration,
                                                  reference, start, end, offsets);
            for (region = 1; region < region_count; ++region) {
                if (regions[region - 1].to > regions[region].from) {
                    return FLUX_INVALID;
                }
            }
            region = 0;
        } else {
            offset = offsets ? offsets[rev - 1] : 0;
            regions[0].from = splice_scale((start + offset) % reference, duration, reference);
            regions[0].to = splice_scale((end + offset) % reference, duration, reference);
            if (!regions[0].to) {
                regions[0].to = duration;
            }
            regions[0].offset = offset;
        }
        offset = region_count ? regions[0].offset : 0;
        split = splice_split(pulses, count, reference - offset);

        for (;;) {
            uint64_t position;
            uint64_t delta;
            uint64_t overflow;
            uint64_t inserted = 0;
            while (!next && word < track->count) {
                uint16_t value = track->words[word];
                uint64_t edge = time + (uint64_t)(value ? value : 65536U) * unit;
                if (edge > limit && !last_window) {
                    break;
                }
                time = edge;
                ++word;
                if (value) {
                    unsigned int covered = 0;
                    while (edge <= limit && covered < region_count
                           && !splice_contains(edge - base, regions[covered].from, regions[covered].to)) {
                        ++covered;
                    }
                    if (edge > limit || covered == region_count) {
                        next = time;
                    }
                }
            }
            if (region < region_count && replacement == count) {
                ++region;
                if (region < region_count) {
                    offset = regions[region].offset;
                    replacement = 0;
                    split = splice_split(pulses, count, reference - offset);
                    continue;
                }
            }
            if (region < region_count && replacement < count) {
                size_t index = replacement + split;
                uint64_t shifted = (pulses[index < count ? index : index - count] + offset) % reference;
                inserted = base + splice_scale(shifted ? shifted : reference, duration, reference);
                /* Reflected physical samples lie at phase + n * grid.
                   Use this same grid for the emulated drive's readback. */
                inserted = inserted > phase
                    ? phase + (inserted - phase + (track->reflected_write_grid ? (grid - 1) / 2 : grid / 2)) / grid * grid
                    : phase;
                if (continuous && inserted > base + regions[region].to) {
                    replacement = count;
                    continue;
                }
                if (inserted > limit) {
                    if (physical_period) {
                        replacement = count;
                        continue;
                    }
                    if (inserted < grid) {
                        ++replacement;
                        continue;
                    }
                    inserted -= grid;
                }
                if (inserted <= base || !splice_contains(inserted - base, regions[region].from, regions[region].to)) {
                    ++replacement;
                    continue;
                }
            }
            if (region < region_count && replacement < count && (!next || inserted <= next)) {
                position = inserted;
                ++replacement;
                if (position == next) {
                    next = 0;
                }
            } else if (next) {
                position = next;
                next = 0;
            } else {
                break;
            }
            if (position <= previous || position <= base) {
                continue;
            }
            delta = (position - previous) / unit;
            /* Zero extends a gap. An edge on an exact overflow multiple
               moves one sample earlier, as in the single-turn writer. */
            if (!(delta % 65536)) {
                --delta;
                position -= unit;
            }
            overflow = delta / 65536;
            if (overflow >= FLUX_CAPTURE_MAX_WORDS - used) {
                return FLUX_LIMIT;
            }
            if (words) {
                memset(words + used, 0, (size_t)overflow * sizeof(*words));
                words[used + (size_t)overflow] = (uint16_t)delta;
            }
            used += (size_t)overflow + 1;
            previous = position;
        }
        base = limit;
    }
    *length = used;
    return FLUX_OK;
}

int flux_capture_slot_writable(const flux_capture_t *capture, unsigned int slot)
{
    unsigned int flags = flux_capture_flags(capture);
    if (slot >= FLUX_CAPTURE_MAX_TRACKS) {
        return 0;
    }
    if (!(flags & FLUX_CAPTURE_TOP_ONLY)) {
        return 1;
    }
    return (flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))
        ? (slot & 1) != 0 : slot >= FLUX_CAPTURE_TRACKS && !(slot & 1);
}

/* Track samples and seek checkpoints are immutable after publication. */
static int share_track(flux_capture_t *capture, unsigned int slot, flux_track_t *track)
{
    if (track->count > FLUX_CAPTURE_MAX_WORDS - capture->words) {
        return FLUX_LIMIT;
    }
    ++track->references;
    capture->tracks[slot] = track;
    capture->words += track->count;
    return FLUX_OK;
}

int flux_capture_keep_unwritten(flux_capture_t *capture, const flux_capture_t *source)
{
    unsigned int slot;

    if (!capture || capture->sealed || !flux_capture_is_sealed(source)
        || capture->revolutions != source->revolutions) {
        return FLUX_INVALID;
    }
    for (slot = 0; slot < FLUX_CAPTURE_MAX_TRACKS; ++slot) {
        if (!capture->tracks[slot] && source->tracks[slot]) {
            int rc = share_track(capture, slot, source->tracks[slot]);
            if (rc != FLUX_OK) {
                return rc;
            }
        }
    }
    return FLUX_OK;
}

static int splice_capture(const flux_capture_t *source, unsigned int halftrack,
                          uint64_t duration, uint64_t physical_period, const uint64_t *offsets,
                          int continuous, uint64_t start, uint64_t end,
                        const uint64_t *pulses, size_t count, flux_capture_t **result)
{
    const flux_track_t *target = flux_capture_track(source, halftrack);
    const flux_track_t blank = { .unit_ticks = flux_capture_sample_ticks(source) };
    flux_capture_t *copy;
    uint64_t durations[FLUX_CAPTURE_MAX_REVS];
    uint16_t *words;
    size_t i;
    size_t length;
    int rc;

    if (!result) {
        return FLUX_INVALID;
    }
    *result = NULL;
    if (!flux_capture_is_sealed(source) || !flux_capture_slot_writable(source, halftrack)
        || duration < 2 || (duration & 1) || duration / 2 > UINT32_MAX
        || start >= duration || !end || end > duration || (count && !pulses)
        || (target && (target->unit_ticks < 2 || (target->unit_ticks & 1)))) {
        return FLUX_INVALID;
    }
    if (count > FLUX_CAPTURE_MAX_WORDS) {
        return FLUX_LIMIT;
    }
    for (i = 0; i < count; ++i) {
        if (!pulses[i] || pulses[i] > duration || (pulses[i] & 1)
            || (i && pulses[i] <= pulses[i - 1])
            || !splice_contains(pulses[i], start, end)) {
            return FLUX_INVALID;
        }
    }
    for (i = 0; i < source->revolutions; ++i) {
        durations[i] = target ? flux_track_duration(target, (unsigned int)i) : duration;
        if ((durations[i] & 1) || durations[i] / 2 > UINT32_MAX
            || (offsets && offsets[i] >= duration)) {
            return FLUX_RANGE;
        }
    }
    if (!target) {
        target = &blank;
    }
    rc = splice_words(target, durations, source->revolutions, duration, physical_period, offsets,
                      continuous, start, end, pulses, count, NULL, &length);
    if (rc != FLUX_OK) {
        return rc;
    }
    if (length > FLUX_CAPTURE_MAX_WORDS - (source->words - target->count)) {
        return FLUX_LIMIT;
    }
    words = length ? malloc(length * sizeof(*words)) : NULL;
    if (length && !words) {
        return FLUX_NOMEM;
    }
    rc = splice_words(target, durations, source->revolutions, duration, physical_period, offsets,
                      continuous, start, end, pulses, count, words, &length);
    copy = rc == FLUX_OK ? flux_capture_create(source->revolutions, source->flags) : NULL;
    if (!copy) {
        free(words);
        return rc == FLUX_OK ? FLUX_NOMEM : rc;
    }
    for (i = 0; i < FLUX_CAPTURE_MAX_TRACKS; ++i) {
        if (i == halftrack) {
            rc = flux_capture_add_track(copy, (unsigned int)i, durations, words, length, target->unit_ticks);
            if (rc == FLUX_OK) {
                copy->tracks[i]->reflected_write_grid = target->reflected_write_grid;
            }
        } else if (source->tracks[i]) {
            rc = share_track(copy, (unsigned int)i, source->tracks[i]);
        }
        if (rc != FLUX_OK) {
            break;
        }
    }
    free(words);
    if (rc != FLUX_OK) {
        flux_capture_destroy(copy);
        return rc;
    }
    flux_capture_seal(copy);
    *result = copy;
    return FLUX_OK;
}

int flux_capture_splice(const flux_capture_t *source, unsigned int halftrack,
                        uint64_t duration, uint64_t start, uint64_t end,
                        const uint64_t *pulses, size_t count, flux_capture_t **result)
{
    return splice_capture(source, halftrack, duration, 0, NULL, 0, start, end, pulses, count, result);
}

int flux_capture_splice_shifted(const flux_capture_t *source, unsigned int halftrack,
                               uint64_t duration, uint64_t start, uint64_t end,
                               const uint64_t *pulses, size_t count, const uint64_t *offsets,
                               flux_capture_t **result)
{
    return splice_capture(source, halftrack, duration, 0, offsets, 0, start, end, pulses, count, result);
}

int flux_capture_splice_continuous(const flux_capture_t *source, unsigned int halftrack,
                                   uint64_t duration, uint64_t start, uint64_t end,
                                   const uint64_t *pulses, size_t count, const uint64_t *offsets,
                                   flux_capture_t **result)
{
    if (!offsets || flux_capture_revolutions(source) < 2) {
        return FLUX_INVALID;
    }
    return splice_capture(source, halftrack, duration, 0, offsets, 1, start, end, pulses, count, result);
}

int flux_capture_splice_unindexed(const flux_capture_t *source, unsigned int halftrack,
                                 uint64_t period, uint64_t start, uint64_t end,
                                 const uint64_t *pulses, size_t count, flux_capture_t **result)
{
    return splice_capture(source, halftrack, period, period, NULL, 0, start, end, pulses, count, result);
}

/* Pack a reflected track in two passes to bound allocations even for
   long gaps. Reflect the entire stream so adjacent captured turns stay
   adjacent, including transitions that straddle an index boundary. */
static int flip_track_words(const flux_track_t *track, uint64_t last_pulse,
                            size_t last_word, uint32_t unit, int resample, uint16_t *words,
                            size_t *length)
{
    uint64_t period = flux_track_period(track);
    uint64_t pulse = last_pulse;
    uint64_t previous = 0;
    int seam = pulse == period;
    size_t word = last_word;
    size_t used = 0;

    while (word || seam) {
        uint64_t position = word ? period - pulse : period;
        if (resample) {
            /* The playback grid can be finer than the physical grid.
               Existing physical samples land exactly back on their grid. */
            position = (position + unit / 2) / unit * unit;
            if (position > period) {
                position -= unit;
            }
        }
        if (position > previous) {
            uint64_t delta = (position - previous) / unit;
            size_t overflow;
            if (!delta) {
                return FLUX_INVALID;
            }
            /* A zero word extends an interval. Use the preceding sample
               when an edge lands on an exact overflow multiple. */
            if (!(delta % 65536)) {
                delta--;
                position -= unit;
            }
            if (delta / 65536 >= FLUX_CAPTURE_MAX_WORDS - used) {
                return FLUX_LIMIT;
            }
            overflow = (size_t)(delta / 65536);
            if (words) {
                memset(words + used, 0, overflow * sizeof(*words));
                words[used + overflow] = (uint16_t)delta;
            }
            used += overflow + 1;
            previous = position;
        }
        if (!word) {
            break;
        }
        pulse -= (uint64_t)track->words[--word] * track->unit_ticks;
        while (word && !track->words[word - 1]) {
            pulse -= UINT64_C(65536) * track->unit_ticks;
            word--;
        }
    }
    *length = used;
    return FLUX_OK;
}

static int add_reflected_track(flux_capture_t *copy, unsigned int slot,
                               const flux_track_t *track, uint32_t physical_unit)
{
    unsigned int rev;
    int rc;
    uint64_t durations[FLUX_CAPTURE_MAX_REVS];
    uint64_t pulse;
    uint64_t last_pulse = 0;
    flux_scan_t scan = {0};
    size_t last_word = 0;
    size_t length = 0;
    uint32_t unit;
    uint64_t remainder;
    uint16_t *words;
    for (rev = 0; rev < copy->revolutions; rev++) {
        durations[rev] = flux_track_duration(track, copy->revolutions - 1 - rev);
    }
    /* A capture's total duration and sample resolution can differ.
       Use their common divisor to retain reflected pulse positions. */
    unit = physical_unit ? physical_unit : track->unit_ticks;
    if (!physical_unit) {
        remainder = flux_track_period(track) % unit;
        while (remainder) {
            uint64_t next = unit % remainder;
            unit = (uint32_t)remainder;
            remainder = next;
        }
    }
    while (flux_track_next(track, &scan, &pulse)) {
        last_pulse = pulse;
        last_word = scan.word;
    }
    rc = flip_track_words(track, last_pulse, last_word, unit, physical_unit != 0, NULL, &length);
    if (rc != FLUX_OK) {
        return rc;
    }
    if (length > FLUX_CAPTURE_MAX_WORDS - copy->words) {
        return FLUX_LIMIT;
    }
    words = length ? malloc(length * sizeof(*words)) : NULL;
    if (length && !words) {
        return FLUX_NOMEM;
    }
    rc = flip_track_words(track, last_pulse, last_word, unit, physical_unit != 0, words, &length);
    if (rc == FLUX_OK) {
        rc = flux_capture_add_track(copy, slot, durations, words, length, unit);
        if (rc == FLUX_OK && !physical_unit && unit != track->unit_ticks) {
            copy->tracks[slot]->reflected_write_grid = track->unit_ticks;
        }
    }
    free(words);
    return rc;
}

int flux_capture_flip_gcr(const flux_capture_t *source, flux_capture_t **result)
{
    unsigned int flags = flux_capture_flags(source);
    unsigned int slot;
    flux_capture_t *copy;
    int rc = FLUX_OK;

    *result = NULL;
    if (!flux_capture_is_sealed(source)
        || !(flags & FLUX_CAPTURE_TOP_ONLY) || !(flags & FLUX_CAPTURE_DOUBLE_SIDED)
        || (flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))) {
        return FLUX_INVALID;
    }
    copy = flux_capture_create(source->revolutions,
                               flags & ~(FLUX_CAPTURE_TOP_ONLY | FLUX_CAPTURE_DOUBLE_SIDED));
    if (!copy) {
        return FLUX_NOMEM;
    }
    for (slot = 0; slot < FLUX_CAPTURE_TRACKS; slot++) {
        const flux_track_t *track = source->tracks[FLUX_CAPTURE_TRACKS + slot];
        if (!track) {
            continue;
        }
        rc = add_reflected_track(copy, slot, track, 0);
        if (rc != FLUX_OK) {
            break;
        }
    }
    if (rc != FLUX_OK) {
        flux_capture_destroy(copy);
        return rc;
    }
    flux_capture_seal(copy);
    *result = copy;
    return FLUX_OK;
}

int flux_capture_restore_top(const flux_capture_t *physical, const flux_capture_t *flipped,
                             const uint8_t *changed, flux_capture_t **result)
{
    unsigned int slot;
    flux_capture_t *copy = flux_capture_create(physical->revolutions, physical->flags);

    *result = NULL;
    if (!copy) {
        return FLUX_NOMEM;
    }
    for (slot = 0; slot < FLUX_CAPTURE_TRACKS; ++slot) {
        unsigned int target = FLUX_CAPTURE_TRACKS + slot;
        flux_track_t *track = changed[slot] ? flipped->tracks[slot] : physical->tracks[target];
        int rc;
        if (!track) {
            continue;
        }
        rc = changed[slot]
            ? add_reflected_track(copy, target, track, flux_capture_sample_ticks(physical))
            : share_track(copy, target, track);
        if (rc != FLUX_OK) {
            flux_capture_destroy(copy);
            return rc;
        }
    }
    flux_capture_seal(copy);
    *result = copy;
    return FLUX_OK;
}

int flux_capture_has_second_bank(const flux_capture_t *capture)
{
    unsigned int slot;
    for (slot = FLUX_CAPTURE_TRACKS; slot < FLUX_CAPTURE_MAX_TRACKS; slot++) {
        if (flux_capture_track(capture, slot)) {
            return 1;
        }
    }
    return 0;
}

unsigned int flux_capture_flags(const flux_capture_t *capture)
{
    return capture ? capture->flags : 0;
}

int flux_capture_uses_360_clock(const flux_capture_t *capture)
{
    unsigned int ht;
    unsigned int present = 0;
    unsigned int matching = 0;
    if (!capture || !capture->sealed) {
        return 0;
    }
    if (capture->flags & FLUX_CAPTURE_360_RPM) {
        return 1;
    }
    if (!(capture->flags & FLUX_CAPTURE_INDEXED)) {
        return 0;
    }

    /* Recover a missing RPM flag only from a disk-wide consensus. Allow
       approximately 160-175 ms per turn, comfortably clear of 300 RPM. */
    for (ht = 0; ht < FLUX_CAPTURE_MAX_TRACKS; ++ht) {
        const flux_track_t *track = capture->tracks[ht];
        unsigned int rev;
        if (!track) {
            continue;
        }
        ++present;
        for (rev = 0; rev < capture->revolutions; ++rev) {
            uint64_t duration = flux_track_duration(track, rev);
            if (duration < 12800000 || duration > 14000000) {
                break;
            }
        }
        if (rev == capture->revolutions) {
            ++matching;
        }
    }
    return matching >= 3 && matching * 4 >= present * 3;
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
    return capture && capture->sealed && halftrack < FLUX_CAPTURE_MAX_TRACKS
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

typedef void (*gcr_header_sink_t)(void *, unsigned int, uint16_t, uint64_t);

static void scan_gcr_headers(const flux_track_t *track, unsigned int cylinder, unsigned int cell,
                             gcr_header_sink_t sink, void *opaque)
{
    static const int8_t decode[32] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,8,0,1,-1,12,4,5,
        -1,-1,2,3,-1,15,6,7,-1,9,10,11,-1,13,14,-1
    };
    flux_scan_t scan = {0};
    uint8_t header[8];
    uint64_t pulse;
    uint64_t previous = 0;
    uint64_t start = 0;
    unsigned int ones = 0;
    unsigned int bits = 0;
    unsigned int symbol = 0;
    unsigned int high = 0;
    unsigned int nibble = 0;
    unsigned int active = 0;
    unsigned int i;

    if (!track || !cell || !cylinder || cylinder > 154) {
        return;
    }
    while (flux_track_next(track, &scan, &pulse)) {
        uint64_t steps = (pulse - previous + cell / 2) / cell;
        previous = pulse;
        if (!steps || steps > 8) {
            ones = active = 0;
            continue;
        }
        while (steps) {
            unsigned int bit = --steps == 0;
            if (bit) {
                if (++ones >= 10) {
                    ones = 10;
                    active = 0;
                    continue;
                }
            } else {
                if (ones >= 10) {
                    start = pulse - steps * cell;
                    active = 1;
                    bits = symbol = nibble = 0;
                }
                ones = 0;
            }
            if (!active) {
                continue;
            }
            symbol = (symbol << 1) | bit;
            if (++bits != 5) {
                continue;
            }
            if (decode[symbol] < 0) {
                active = 0;
                continue;
            }
            if (nibble & 1) {
                header[nibble / 2] = (uint8_t)(high << 4 | (unsigned int)decode[symbol]);
            } else {
                high = (unsigned int)decode[symbol];
            }
            bits = symbol = 0;
            if (++nibble != 16) {
                continue;
            }
            active = 0;
            i = header[2];
            if (header[0] == 8 && i < 29 && header[3] == cylinder
                && header[1] == (header[2] ^ header[3] ^ header[4] ^ header[5])) {
                uint16_t id = (uint16_t)(header[4] << 8 | header[5]);
                sink(opaque, i, id, start);
            }
        }
    }
}

typedef struct gcr_turn_measurement_s {
    uint64_t last[29];
    uint64_t intervals[29];
    uint16_t ids[29];
} gcr_turn_measurement_t;

static void measure_header(void *opaque, unsigned int sector, uint16_t id, uint64_t position)
{
    gcr_turn_measurement_t *state = opaque;
    uint64_t delta = position - state->last[sector];
    if (state->last[sector] && id == state->ids[sector] && delta >= 12000000 && delta <= 18000000) {
        state->intervals[sector] = delta;
    }
    state->last[sector] = position;
    state->ids[sector] = id;
}

uint64_t flux_track_measure_gcr_turn(const flux_track_t *track, unsigned int cylinder,
                                     unsigned int cell)
{
    gcr_turn_measurement_t state = {0};
    uint64_t values[29];
    uint64_t median;
    unsigned int i;
    unsigned int count = 0;
    unsigned int matching = 0;
    scan_gcr_headers(track, cylinder, cell, measure_header, &state);
    for (i = 0; i < 29; ++i) {
        unsigned int j = count;
        if (!state.intervals[i]) {
            continue;
        }
        while (j && values[j - 1] > state.intervals[i]) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = state.intervals[i];
        ++count;
    }
    if (count < 6) {
        return 0;
    }
    median = values[count / 2];
    for (i = 0; i < count; ++i) {
        if (values[i] + cell * 2 >= median && values[i] <= median + cell * 2) {
            ++matching;
        }
    }
    return matching >= 6 && matching * 4 >= count * 3 ? median & ~UINT64_C(1) : 0;
}

typedef struct gcr_window_headers_s {
    const flux_track_t *track;
    uint64_t position[FLUX_CAPTURE_MAX_REVS][29];
    uint16_t id[FLUX_CAPTURE_MAX_REVS][29];
    unsigned int rev;
} gcr_window_headers_t;

static void record_window_header(void *opaque, unsigned int sector, uint16_t id, uint64_t position)
{
    gcr_window_headers_t *state = opaque;
    uint64_t *entry;

    while (state->rev < state->track->revolutions
        && position >= flux_track_boundary(state->track, state->rev + 1)) {
        ++state->rev;
    }
    if (state->rev >= state->track->revolutions) {
        return;
    }
    entry = &state->position[state->rev][sector];
    if (*entry) {
        *entry = UINT64_MAX; /* An ambiguous duplicate cannot anchor a write. */
        return;
    }
    *entry = position - flux_track_boundary(state->track, state->rev) + 1;
    state->id[state->rev][sector] = id;
}

int flux_track_gcr_write_offsets(const flux_track_t *track, unsigned int logical_track,
                                 unsigned int cell, unsigned int reference_rev,
                                 uint64_t start, int required_sector, uint64_t *offsets)
{
    gcr_window_headers_t state = {0};
    uint64_t duration = flux_track_duration(track, reference_rev);
    uint64_t distance = duration;
    uint64_t reference;
    unsigned int sector;
    unsigned int anchor = 29;
    unsigned int rev;
    if (duration < 12000000 || duration > 18000000 || start >= duration) {
        return 0;
    }
    state.track = track;
    scan_gcr_headers(track, logical_track, cell, record_window_header, &state);
    for (sector = 0; sector < 29; ++sector) {
        uint64_t position = state.position[reference_rev][sector];
        uint64_t delta;
        if ((required_sector >= 0 && sector != (unsigned int)required_sector)
            || !position || position == UINT64_MAX) {
            continue;
        }
        delta = (start + duration - (position - 1)) % duration;
        if (delta < distance) {
            anchor = sector;
            distance = delta;
        }
    }
    if (anchor == 29 || distance > duration / 9) {
        return 0;
    }
    reference = state.position[reference_rev][anchor] - 1;
    for (rev = 0; rev < track->revolutions; ++rev) {
        uint64_t position = state.position[rev][anchor];
        uint64_t other = flux_track_duration(track, rev);
        uint64_t shift;
        if (other < 12000000 || other > 18000000 || !position || position == UINT64_MAX
            || state.id[rev][anchor] != state.id[reference_rev][anchor]) {
            return 0;
        }
        shift = (splice_scale(position - 1, duration, other) + duration - reference) % duration;
        if (shift > duration / 100 && shift < duration - duration / 100) {
            return 0;
        }
        offsets[rev] = (shift + 1) & ~UINT64_C(1);
        if (offsets[rev] == duration) {
            offsets[rev] = 0;
        }
    }
    return 1;
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
    size_t lo = 0;
    size_t hi;
    size_t mid;
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
