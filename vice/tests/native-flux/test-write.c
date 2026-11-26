/* Native write-splice and staged SCP round-trip checks. GPL-2.0-or-later. */
#include "flux-capture.h"
#include "scp-reader.h"
#include "scp-writer.h"

#include <assert.h>
#include <string.h>

static void check_track(const flux_capture_t *capture, unsigned int ht,
                        const uint16_t *expected, size_t count)
{
    const flux_track_t *track = flux_capture_track(capture, ht);
    assert(track);
    assert(flux_track_unit_ticks(track) == 2);
    assert(flux_track_duration(track, 0) == 1000);
    assert(flux_track_word_count(track) == count);
    assert(!memcmp(flux_track_words(track), expected, count * sizeof(*expected)));
}

static void check_multi_turn(void)
{
    const uint64_t durations[] = {1000, 1200};
    const uint64_t replacement[] = {50, 950};
    const uint16_t initial[] = {50, 50, 100, 200, 100, 60, 60, 120, 240, 120};
    const uint16_t expected[] = {25, 75, 100, 275, 55, 90, 120, 330};
    flux_capture_t *source = flux_capture_create(2, FLUX_CAPTURE_INDEXED);
    flux_capture_t *written = NULL;
    flux_capture_t *parsed = NULL;
    FILE *stage = tmpfile();

    assert(source && stage);
    assert(flux_capture_add_track(source, 0, durations, initial, 10, 2) == FLUX_OK);
    assert(flux_capture_add_track(source, 2, durations, initial, 10, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice(source, 2, durations[0], 700, 150,
                               replacement, 2, &written) == FLUX_OK);
    assert(scp_file_write_staged(stage, written) == FLUX_OK);
    assert(scp_file_read(stage, &parsed) == FLUX_OK);
    assert(flux_capture_revolutions(parsed) == 2);
    assert(flux_track_duration(flux_capture_track(parsed, 2), 1) == durations[1]);
    check_track(parsed, 2, expected, 8);
    check_track(parsed, 0, initial, 10);
    assert(!fclose(stage));
    flux_capture_destroy(parsed);
    flux_capture_destroy(written);
    flux_capture_destroy(source);
}

static void check_unindexed_turn(void)
{
    const uint64_t durations[] = {700, 700, 700};
    const uint64_t pulse = 400;
    const uint16_t initial[] = {50, 150, 250, 100, 50, 100, 325, 45};
    const uint16_t expected[] = {50, 150, 250, 100, 50, 100, 300, 70};
    flux_capture_t *source = flux_capture_create(3, FLUX_CAPTURE_360_RPM);
    flux_capture_t *written = NULL;
    const flux_track_t *track;

    assert(source);
    assert(flux_capture_add_track(source, 0, durations, initial, 8, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice_unindexed(source, 0, 800, 300, 500, &pulse, 1, &written) == FLUX_OK);
    assert(flux_capture_revolutions(written) == 3);
    track = flux_capture_track(written, 0);
    assert(flux_track_duration(track, 0) == 700);
    assert(flux_track_duration(track, 2) == 700);
    assert(flux_track_word_count(track) == 8);
    assert(!memcmp(flux_track_words(track), expected, sizeof(expected)));
    flux_capture_destroy(written);
    flux_capture_destroy(source);
}

static void check_shifted_turn(void)
{
    const uint64_t durations[] = {1000, 1000};
    const uint64_t offsets[] = {0, 200};
    const uint64_t pulses[] = {50, 950};
    const uint16_t initial[] = {50, 50, 100, 100, 150, 100, 50, 100, 100, 150};
    const uint16_t expected[] = {25, 75, 100, 100, 175, 100, 50, 75, 100, 150};
    flux_capture_t *source = flux_capture_create(2, 0);
    flux_capture_t *written = NULL;

    assert(source);
    assert(flux_capture_add_track(source, 0, durations, initial, 10, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice_shifted(source, 0, 1000, 700, 150,
                                     pulses, 2, offsets, &written) == FLUX_OK);
    check_track(written, 0, expected, 10);
    flux_capture_destroy(written);
    flux_capture_destroy(source);
}

static void check_shifted_boundaries(void)
{
    const uint64_t durations[] = {1000, 1000, 1000};
    const uint64_t offsets[] = {0, 50, 950};
    const uint64_t pulses[] = {920, 960};
    const uint16_t expected[] = {460, 20, 505, 20, 430, 20};
    const uint64_t wrap_offsets[] = {0, 950};
    const uint64_t wrap_pulses[] = {10, 960};
    const uint16_t wrap_expected[] = {480, 25, 450, 25};
    flux_capture_t *source = flux_capture_create(3, 0);
    flux_capture_t *written = NULL;

    assert(source);
    assert(flux_capture_add_track(source, 0, durations, NULL, 0, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice_continuous(source, 0, 1000, 900, 980, pulses, 2,
                                         offsets, &written) == FLUX_OK);
    check_track(written, 0, expected, 6);
    flux_capture_destroy(written);
    flux_capture_destroy(source);

    source = flux_capture_create(2, 0);
    assert(source);
    assert(flux_capture_add_track(source, 0, durations, NULL, 0, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice_continuous(source, 0, 1000, 940, 20, wrap_pulses, 2,
                                         wrap_offsets, &written) == FLUX_OK);
    check_track(written, 0, wrap_expected, 4);
    flux_capture_destroy(written);
    flux_capture_destroy(source);
}

static void check_reflected_write_grid(void)
{
    const uint64_t duration = 1006;
    const uint64_t pulse = 748;
    const uint16_t original[] = {10, 10, 10, 10};
    const uint16_t expected[] = {10, 10, 12, 8};
    const uint16_t playback[] = {343, 32, 48, 40};
    const uint8_t changed[FLUX_CAPTURE_TRACKS] = {1};
    unsigned int slot = FLUX_CAPTURE_TRACKS;
    flux_capture_t *source = flux_capture_create(1, FLUX_CAPTURE_TOP_ONLY | FLUX_CAPTURE_DOUBLE_SIDED);
    flux_capture_t *flipped = NULL;
    flux_capture_t *written = NULL;
    flux_capture_t *physical = NULL;
    flux_capture_t *remounted = NULL;
    const flux_track_t *track;

    assert(source);
    assert(flux_capture_add_track(source, slot, &duration, original, 4, 8) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_flip_gcr(source, &flipped) == FLUX_OK);
    assert(flux_capture_splice(flipped, 0, duration, 700, 830, &pulse, 1, &written) == FLUX_OK);
    track = flux_capture_track(written, 0);
    assert(flux_track_word_count(track) == 4);
    assert(!memcmp(flux_track_words(track), playback, sizeof(playback)));
    assert(flux_capture_restore_top(source, written, changed, &physical) == FLUX_OK);
    track = flux_capture_track(physical, slot);
    assert(flux_track_unit_ticks(track) == 8 && flux_track_duration(track, 0) == duration);
    assert(flux_track_word_count(track) == 4);
    assert(!memcmp(flux_track_words(track), expected, sizeof(expected)));
    assert(flux_capture_flip_gcr(physical, &remounted) == FLUX_OK);
    track = flux_capture_track(remounted, 0);
    assert(flux_track_word_count(track) == 4);
    assert(!memcmp(flux_track_words(track), playback, sizeof(playback)));
    flux_capture_destroy(remounted);
    flux_capture_destroy(physical);
    flux_capture_destroy(written);
    flux_capture_destroy(flipped);
    flux_capture_destroy(source);
}

int main(void)
{
    const uint64_t duration = 1000;
    const uint16_t initial[] = {50, 50, 100, 200, 100};
    const uint16_t expected[] = {25, 75, 100, 275};
    const uint64_t replacement[] = {50, 950};
    const uint64_t outside[] = {400};
    flux_capture_t *source = flux_capture_create(1, FLUX_CAPTURE_360_RPM);
    flux_capture_t *spliced = NULL;
    flux_capture_t *readback = NULL;
    FILE *file;

    assert(source);
    assert(flux_capture_add_track(source, 0, &duration, initial,
                                  FLUX_CAPTURE_MAX_WORDS + 1U, 2) == FLUX_LIMIT);
    assert(flux_capture_add_track(source, 0, &duration, initial, 5, 2) == FLUX_OK);
    assert(flux_capture_add_track(source, 3, &duration, initial, 5, 2) == FLUX_OK);
    assert(flux_capture_seal(source) == FLUX_OK);
    assert(flux_capture_splice(source, 0, duration, 700, 150, outside, 1, &spliced) == FLUX_INVALID);
    assert(!spliced);
    assert(flux_capture_splice(source, 0, duration, 700, 150, replacement, 2, &spliced) == FLUX_OK);
    check_track(source, 0, initial, 5);
    check_track(spliced, 0, expected, 4);
    check_track(spliced, 3, initial, 5);
    assert(flux_capture_track(spliced, 3) == flux_capture_track(source, 3));
    flux_capture_destroy(source);
    check_track(spliced, 3, initial, 5);
    assert(flux_capture_splice(spliced, 2, duration, 0, duration, replacement, 2, &readback) == FLUX_OK);
    assert(!flux_capture_track(spliced, 2));
    check_track(readback, 2, (const uint16_t[]){25, 450}, 2);
    check_track(readback, 0, expected, 4);
    flux_capture_destroy(spliced);
    spliced = readback;
    readback = NULL;

    file = tmpfile();
    assert(file);
    assert(scp_file_write_staged(file, spliced) == FLUX_OK);
    assert(scp_file_write_staged(file, spliced) == FLUX_INVALID);
    assert(scp_file_read(file, &readback) == FLUX_OK);
    assert(flux_capture_flags(readback) == FLUX_CAPTURE_360_RPM);
    check_track(readback, 0, expected, 4);
    check_track(readback, 3, initial, 5);
    check_track(readback, 2, (const uint16_t[]){25, 450}, 2);
    assert(!fclose(file));
    flux_capture_destroy(readback);
    flux_capture_destroy(spliced);

    check_multi_turn();
    check_unindexed_turn();
    check_shifted_turn();
    check_shifted_boundaries();
    check_reflected_write_grid();
    puts("Native splice and SCP staging round trip passed.");
    return 0;
}
