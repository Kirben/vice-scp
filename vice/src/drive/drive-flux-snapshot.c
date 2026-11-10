/* Native captures and playback state, independent of SCP/USB. GPL-2.0-or-later.
   FLUXu:d 1.0: embed byte, canonical capture SHA-1 (20 bytes), cursor turn,
   phase, duration, elapsed (qwords), halftrack/motor/inclusive/reference phase
   (bytes). If embedded: revolutions/flags (dwords), then 84 track descriptors:
   presence byte, unit ticks/count (dwords), per-turn durations (qwords), and
   packed interval words. DRIVE 2.1 makes this module mandatory.
   Without embedding, the exact matching capture must already be attached. */
#include "vice.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "attach.h"
#include "drive.h"
#include "drive-flux-snapshot.h"
#include "rotation-flux.h"
#include "snapshot.h"
#include "sha1.h"
#include "log.h"

static void hash_integer(SHA1_CTX *hash, uint64_t value)
{
    uint8_t bytes[8];
    unsigned int i;
    for (i = 0; i < 8; i++) { bytes[i] = (uint8_t)(value >> (i * 8)); }
    SHA1Update(hash, bytes, 8);
}

static void capture_identity(const flux_capture_t *capture, uint8_t identity[20])
{
    SHA1_CTX hash;
    unsigned int halftrack, rev;
    size_t i, n, j, count;
    uint8_t buffer[8192];
    SHA1Init(&hash);
    hash_integer(&hash, flux_capture_revolutions(capture));
    hash_integer(&hash, flux_capture_flags(capture));
    for (halftrack = 0; halftrack < FLUX_CAPTURE_TRACKS; halftrack++) {
        const flux_track_t *track = flux_capture_track(capture, halftrack);
        const uint16_t *words = flux_track_words(track);
        hash_integer(&hash, track != NULL);
        if (!track) { continue; }
        count = flux_track_word_count(track);
        hash_integer(&hash, flux_track_unit_ticks(track));
        hash_integer(&hash, count);
        for (rev = 0; rev < flux_capture_revolutions(capture); rev++) {
            hash_integer(&hash, flux_track_duration(track, rev));
        }
        for (i = 0; i < count; i += n) {
            n = count - i < sizeof(buffer) / 2 ? count - i : sizeof(buffer) / 2;
            for (j = 0; j < n; j++) {
                buffer[j * 2] = (uint8_t)words[i + j];
                buffer[j * 2 + 1] = (uint8_t)(words[i + j] >> 8);
            }
            SHA1Update(&hash, buffer, (uint32_t)(n * 2));
        }
    }
    SHA1Final(identity, &hash);
}

static int capture_write(snapshot_module_t *m, const flux_capture_t *capture)
{
    unsigned int halftrack, rev;
    if (SMW_DW(m, flux_capture_revolutions(capture)) < 0
        || SMW_DW(m, flux_capture_flags(capture)) < 0) { return -1; }
    for (halftrack = 0; halftrack < FLUX_CAPTURE_TRACKS; halftrack++) {
        const flux_track_t *track = flux_capture_track(capture, halftrack);
        size_t count = flux_track_word_count(track);
        if (SMW_B(m, track != NULL) < 0) { return -1; }
        if (!track) { continue; }
        if (SMW_DW(m, flux_track_unit_ticks(track)) < 0
            || SMW_DW(m, (uint32_t)count) < 0) { return -1; }
        for (rev = 0; rev < flux_capture_revolutions(capture); rev++) {
            if (SMW_QW(m, flux_track_duration(track, rev)) < 0) { return -1; }
        }
        if (count && SMW_WA(m, flux_track_words(track), (unsigned int)count) < 0) { return -1; }
    }
    return 0;
}

static flux_capture_t *capture_read(snapshot_module_t *m)
{
    uint32_t revs, flags, unit, count;
    unsigned int halftrack, rev;
    uint8_t present;
    uint64_t durations[FLUX_CAPTURE_MAX_REVS];
    uint16_t *words = NULL;
    flux_capture_t *capture;
    if (SMR_DW(m, &revs) < 0 || !revs || revs > FLUX_CAPTURE_MAX_REVS
        || SMR_DW(m, &flags) < 0 || (flags & ~FLUX_CAPTURE_INDEXED)) { return NULL; }
    capture = flux_capture_create(revs, flags);
    if (!capture) { return NULL; }
    for (halftrack = 0; halftrack < FLUX_CAPTURE_TRACKS; halftrack++) {
        if (SMR_B(m, &present) < 0 || present > 1) { goto fail; }
        if (!present) { continue; }
        if (SMR_DW(m, &unit) < 0 || !unit || SMR_DW(m, &count) < 0
            || count > FLUX_CAPTURE_MAX_WORDS - flux_capture_words(capture)) { goto fail; }
        for (rev = 0; rev < revs; rev++) {
            if (SMR_QW(m, &durations[rev]) < 0 || !durations[rev]) { goto fail; }
        }
        if (count) {
            words = malloc((size_t)count * sizeof(*words));
            if (!words || SMR_WA(m, words, count) < 0) { goto fail; }
        }
        if (flux_capture_add_track(capture, halftrack, durations, words, count, unit) != FLUX_OK) { goto fail; }
        free(words);
        words = NULL;
    }
    if (flux_capture_seal(capture) != FLUX_OK) { goto fail; }
    return capture;
fail:
    free(words);
    flux_capture_destroy(capture);
    return NULL;
}

int drive_flux_snapshot_validate(const drive_t *drive, unsigned int halftrack)
{
    return halftrack >= 2 && halftrack <= FLUX_CAPTURE_TRACKS + 1
        && (drive->diskunit->type == DRIVE_TYPE_1541 || drive->diskunit->type == DRIVE_TYPE_1541II)
        && drive->diskunit->clock_frequency == 1 && drive->read_only == 1
        && drive->snap_speed_zone <= 3 && drive->snap_ue7_dcba <= 3
        && drive->snap_ue7_counter <= 15 && drive->snap_uf4_counter <= 15
        && drive->snap_bit_counter >= 0 && drive->snap_bit_counter <= 7
        && drive->snap_last_read_data >= 0 && drive->snap_last_read_data <= 1023
        && drive->snap_filter_counter <= 40 && drive->snap_filter_state <= 1
        && drive->snap_filter_last_state <= 1 && drive->snap_so_delay <= 31
        && drive->snap_ref_advance <= 15 && drive->snap_req_ref_cycles <= 15 ? 0 : -1;
}

int drive_flux_snapshot_write(snapshot_t *s, drive_t *drive,
                                unsigned int unit, unsigned int number, int embed)
{
    char name[16];
    uint8_t identity[20];
    snapshot_module_t *m;
    const rotation_flux_t *input = drive->flux;
    const flux_playback_state_t *state;
    int rc;
    if (!input || input->fault || !flux_capture_is_sealed(input->playback.capture)) { return -1; }
    state = &input->playback.state;
    capture_identity(input->playback.capture, identity);
    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_create(s, name, 1, 0);
    if (!m) { return -1; }
    rc = SMW_B(m, embed != 0) < 0 || SMW_BA(m, identity, sizeof(identity)) < 0
        || SMW_QW(m, state->turn) < 0 || SMW_QW(m, state->phase) < 0
        || SMW_QW(m, state->duration) < 0 || SMW_QW(m, state->elapsed) < 0
        || SMW_B(m, state->halftrack) < 0 || SMW_B(m, !!(drive->byte_ready_active & BRA_MOTOR_ON)) < 0
        || SMW_B(m, state->inclusive) < 0 || SMW_B(m, input->reference_phase) < 0;
    if (!rc && embed) { rc = capture_write(m, input->playback.capture); }
    if (snapshot_module_close(m) < 0) { rc = -1; }
    return rc ? -1 : 0;
}

int drive_flux_snapshot_read(snapshot_t *s, drive_t *drive,
                               unsigned int unit, unsigned int number, unsigned int halftrack)
{
    char name[16];
    uint8_t major, minor, embed, ht, motor, inclusive, phase, expected[20], actual[20];
    snapshot_module_t *m;
    flux_capture_t *owned = NULL;
    const flux_capture_t *capture;
    rotation_flux_t restored = {0};
    flux_playback_state_t state = {0};
    int rc = -1;

    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_open(s, name, &major, &minor);
    if (!m) { return -1; } /* Mandatory when DRIVE 2.1 selects native flux. */
    if (major != 1 || minor != 0 || SMR_B(m, &embed) < 0 || embed > 1
        || SMR_BA(m, expected, sizeof(expected)) < 0
        || SMR_QW(m, &state.turn) < 0 || SMR_QW(m, &state.phase) < 0
        || SMR_QW(m, &state.duration) < 0 || SMR_QW(m, &state.elapsed) < 0
        || SMR_B(m, &ht) < 0 || SMR_B(m, &motor) < 0
        || SMR_B(m, &inclusive) < 0 || SMR_B(m, &phase) < 0 || phase >= 5) { goto done; }
    state.halftrack = ht;
    state.motor = motor;
    state.inclusive = inclusive;
    if (halftrack != (unsigned int)ht + 2 || motor != !!(drive->byte_ready_active & BRA_MOTOR_ON)
        || drive_flux_snapshot_validate(drive, halftrack) < 0) { goto done; }
    if (embed) {
        owned = capture_read(m);
        if (!owned) { goto done; }
        capture = owned;
    } else {
        if (!drive->flux) { goto done; }
        capture = drive->flux->playback.capture;
    }
    capture_identity(capture, actual);
    if (memcmp(expected, actual, sizeof(actual))
        || flux_playback_restore(&restored.playback, capture, &state) != FLUX_OK) { goto done; }
    restored.reference_phase = phase;
    if (snapshot_module_close(m) < 0) { m = NULL; goto done; }
    m = NULL;
    if (embed) {
        if (file_system_attach_flux_capture(unit, number, owned) < 0) { goto done; }
        owned = NULL;
    }
    *drive->flux = restored;
    rc = 0;
done:
    if (m && snapshot_module_close(m) < 0) { rc = -1; }
    flux_capture_destroy(owned);
    if (rc < 0) {
        snapshot_set_error(SNAPSHOT_MODULE_INCOMPATIBLE);
        log_error(drive->diskunit->log, "Invalid native snapshot state or missing/mismatched attached capture.");
    }
    return rc;
}
