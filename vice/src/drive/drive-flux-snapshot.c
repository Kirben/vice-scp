/* Native captures and playback state, independent of SCP/USB. GPL-2.0-or-later.
   FLUXu:d 1.0: embed byte, canonical capture SHA-1 (20 bytes), cursor turn,
   phase, duration, elapsed (qwords), halftrack/motor/inclusive/reference phase
   (bytes). Version 1.1 records speed and fractional source phase (bytes)
   immediately before any embedded capture; the reference phase uses 2.5 ns
   units when speed is 6. Version 1.2 adds the alignment position after
   the speed fields. If embedded: revolutions/flags (dwords), then 84 track descriptors:
   presence byte, unit ticks/count (dwords), per-turn durations (qwords), and
   packed interval words. DRIVE 2.1 makes this module mandatory.
   Version 1.3 describes writable media. It always includes speed and alignment
   and stores any active write splice before the optional embedded capture.
   Version 1.4 adds 1571 head selection and double-sided captures.
   Version 1.5 adds writable 1571 media and its active write splice.
   External captures must match both content and write protection. */
#include "vice.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "attach.h"
#include "drive.h"
#include "driveimage.h"
#include "diskimage.h"
#include "drive-flux-snapshot.h"
#include "rotation-flux.h"
#include "snapshot.h"
#include "sha1.h"
#include "log.h"
#include "lib.h"

static void hash_integer(SHA1_CTX *hash, uint64_t value)
{
    uint8_t bytes[8];
    unsigned int i;
    for (i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(value >> (i * 8));
    }
    SHA1Update(hash, bytes, 8);
}

static void capture_identity(const flux_capture_t *capture, uint8_t identity[20])
{
    SHA1_CTX hash;
    unsigned int halftrack;
    unsigned int rev;
    size_t i;
    size_t n;
    size_t j;
    size_t count;
    unsigned int tracks = (flux_capture_flags(capture) & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))
        ? FLUX_CAPTURE_MAX_TRACKS : FLUX_CAPTURE_TRACKS;
    uint8_t buffer[8192];
    SHA1Init(&hash);
    hash_integer(&hash, flux_capture_revolutions(capture));
    hash_integer(&hash, flux_capture_flags(capture));
    for (halftrack = 0; halftrack < tracks; halftrack++) {
        const flux_track_t *track = flux_capture_track(capture, halftrack);
        const uint16_t *words = flux_track_words(track);
        hash_integer(&hash, track != NULL);
        if (!track) {
            continue;
        }
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

/* Keep the existing little-endian word encoding, using block I/O for
   captures large enough to make per-word snapshot calls expensive. */
static int capture_words_write(snapshot_module_t *m, const uint16_t *words, size_t count)
{
    uint8_t buffer[8192];
    size_t i;
    size_t j;
    size_t n;
    for (i = 0; i < count; i += n) {
        n = count - i < sizeof(buffer) / 2 ? count - i : sizeof(buffer) / 2;
        for (j = 0; j < n; j++) {
            buffer[2 * j] = (uint8_t)words[i + j];
            buffer[2 * j + 1] = (uint8_t)(words[i + j] >> 8);
        }
        if (SMW_BA(m, buffer, (unsigned int)(2 * n)) < 0) {
            return -1;
        }
    }
    return 0;
}

static int capture_words_read(snapshot_module_t *m, uint16_t *words, size_t count)
{
    uint8_t buffer[8192];
    size_t i;
    size_t j;
    size_t n;
    for (i = 0; i < count; i += n) {
        n = count - i < sizeof(buffer) / 2 ? count - i : sizeof(buffer) / 2;
        if (SMR_BA(m, buffer, (unsigned int)(2 * n)) < 0) {
            return -1;
        }
        for (j = 0; j < n; j++) {
            words[i + j] = (uint16_t)(buffer[2 * j] | ((unsigned int)buffer[2 * j + 1] << 8));
        }
    }
    return 0;
}

static int capture_write(snapshot_module_t *m, const flux_capture_t *capture)
{
    unsigned int halftrack;
    unsigned int rev;
    unsigned int tracks = (flux_capture_flags(capture) & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))
        ? FLUX_CAPTURE_MAX_TRACKS : FLUX_CAPTURE_TRACKS;
    if (SMW_DW(m, flux_capture_revolutions(capture)) < 0
        || SMW_DW(m, flux_capture_flags(capture)) < 0) {
        return -1;
    }
    for (halftrack = 0; halftrack < tracks; halftrack++) {
        const flux_track_t *track = flux_capture_track(capture, halftrack);
        size_t count = flux_track_word_count(track);
        if (SMW_B(m, track != NULL) < 0) {
            return -1;
        }
        if (!track) {
            continue;
        }
        if (SMW_DW(m, flux_track_unit_ticks(track)) < 0
            || SMW_DW(m, (uint32_t)count) < 0) {
            return -1;
        }
        for (rev = 0; rev < flux_capture_revolutions(capture); rev++) {
            if (SMW_QW(m, flux_track_duration(track, rev)) < 0) {
                return -1;
            }
        }
        if (capture_words_write(m, flux_track_words(track), count) < 0) {
            return -1;
        }
    }
    return 0;
}

static flux_capture_t *capture_read(snapshot_module_t *m, unsigned int minor, int mfm)
{
    uint32_t revs;
    uint32_t flags;
    uint32_t unit;
    uint32_t count;
    unsigned int halftrack;
    unsigned int rev;
    unsigned int tracks;
    uint8_t present;
    uint64_t durations[FLUX_CAPTURE_MAX_REVS];
    uint16_t *words = NULL;
    flux_capture_t *capture;
    if (SMR_DW(m, &revs) < 0 || !revs || revs > FLUX_CAPTURE_MAX_REVS
        || SMR_DW(m, &flags) < 0 || (flags & ~FLUX_CAPTURE_FLAGS)) {
        return NULL;
    }
    if (!!(flags & FLUX_CAPTURE_MFM) != mfm
        || ((mfm || minor < 4) && (flags & FLUX_CAPTURE_DOUBLE_SIDED))
        || ((flags & FLUX_CAPTURE_TOP_ONLY)
            && !(flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_DOUBLE_SIDED)))) {
        return NULL;
    }
    tracks = (flags & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50)) ? FLUX_CAPTURE_MAX_TRACKS : FLUX_CAPTURE_TRACKS;
    capture = flux_capture_create(revs, flags);
    if (!capture) {
        return NULL;
    }
    for (halftrack = 0; halftrack < tracks; halftrack++) {
        if (SMR_B(m, &present) < 0 || present > 1) {
            goto fail;
        }
        if (!present) {
            continue;
        }
        if (flags & FLUX_CAPTURE_TOP_ONLY) {
            /* Sector controllers retain interleaved slots. The 1571
               playback circuit stores head 1 in its own half-track bank. */
            if ((flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_8X50))
                ? !(halftrack & 1)
                : (halftrack < FLUX_CAPTURE_TRACKS || (halftrack & 1))) {
                goto fail;
            }
        }
        if (SMR_DW(m, &unit) < 0 || !unit || SMR_DW(m, &count) < 0
            || count > FLUX_CAPTURE_MAX_WORDS - flux_capture_words(capture)) {
            goto fail;
        }
        for (rev = 0; rev < revs; rev++) {
            if (SMR_QW(m, &durations[rev]) < 0 || !durations[rev]) {
                goto fail;
            }
        }
        if (count) {
            words = malloc((size_t)count * sizeof(*words));
            if (!words || capture_words_read(m, words, count) < 0) {
                goto fail;
            }
        }
        if (flux_capture_add_track(capture, halftrack, durations, words, count, unit) != FLUX_OK) {
            goto fail;
        }
        free(words);
        words = NULL;
    }
    if (flux_capture_seal(capture) != FLUX_OK) {
        goto fail;
    }
    return capture;
fail:
    free(words);
    flux_capture_destroy(capture);
    return NULL;
}

static int has_1571_clock_switch(unsigned int type)
{
    return type == DRIVE_TYPE_1570 || type == DRIVE_TYPE_1571 || type == DRIVE_TYPE_1571CR;
}

int drive_flux_snapshot_validate(const drive_t *drive, unsigned int halftrack)
{
    return halftrack >= 2 && halftrack <= FLUX_CAPTURE_TRACKS + 1
        && drive->diskunit->type != DRIVE_TYPE_1581
        && drive_check_image_format(DISK_IMAGE_TYPE_SCP, drive->diskunit->mynumber) == 0
        && (has_1571_clock_switch(drive->diskunit->type)
            ? (drive->diskunit->clock_frequency == 1 || drive->diskunit->clock_frequency == 2)
            : drive->diskunit->clock_frequency == (drive->diskunit->type == DRIVE_TYPE_1551 ? 2 : 1))
        && (unsigned int)drive->read_only <= 1
        && (drive->read_only || drive_check_scp_write(drive->diskunit->type, 0))
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
    size_t i;
    int rc;
    int rate_state;
    int writable;
    int is_1571;
    if (!input || input->fault || !drive->image
        || (drive->image->read_only && !drive->read_only)
        || (drive->read_only && input->writing)
        || !flux_capture_is_sealed(input->playback.capture)) {
        return -1;
    }
    writable = !drive->image->read_only;
    is_1571 = has_1571_clock_switch(drive->diskunit->type);
    state = &input->playback.state;
    rate_state = flux_capture_uses_360_clock(input->playback.capture);
    if ((input->speed != 5 && input->speed != 6)
        || (input->speed == 6 && !rate_state)
        || input->reference_phase >= (input->speed == 6 ? 25U : 5U)
        || input->source_phase >= (input->speed == 6 ? 6U : 1U)) {
        return -1;
    }
    capture_identity(input->playback.capture, identity);
    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_create(s, name, 1,
                               is_1571 ? (writable ? 5 : 4) : writable ? 3 : input->align_halftrack ? 2 : rate_state);
    if (!m) {
        return -1;
    }
    rc = SMW_B(m, embed != 0) < 0 || SMW_BA(m, identity, sizeof(identity)) < 0
        || SMW_QW(m, state->turn) < 0 || SMW_QW(m, state->phase) < 0
        || SMW_QW(m, state->duration) < 0 || SMW_QW(m, state->elapsed) < 0
        || SMW_B(m, state->halftrack) < 0 || SMW_B(m, !!(drive->byte_ready_active & BRA_MOTOR_ON)) < 0
        || SMW_B(m, state->inclusive) < 0 || SMW_B(m, input->reference_phase) < 0;
    if (!rc && (is_1571 || writable || rate_state || input->align_halftrack)) {
        rc = SMW_B(m, input->speed) < 0 || SMW_B(m, input->source_phase) < 0;
    }
    if (!rc && (is_1571 || writable || input->align_halftrack)) {
        rc = SMW_B(m, input->align_halftrack) < 0;
    }
    if (!rc && writable) {
        rc = SMW_B(m, input->writing) < 0;
        if (!rc && input->writing) {
            rc = SMW_QW(m, input->write_start) < 0
                || SMW_QW(m, input->write_elapsed) < 0
                || SMW_DW(m, (uint32_t)input->write_count) < 0;
            for (i = 0; !rc && i < input->write_count; ++i) {
                rc = SMW_QW(m, input->write_pulses[i]) < 0;
            }
        }
    }
    if (!rc && embed) {
        rc = capture_write(m, input->playback.capture);
    }
    if (snapshot_module_close(m) < 0) {
        rc = -1;
    }
    return rc ? -1 : 0;
}

int drive_flux_snapshot_read(snapshot_t *s, drive_t *drive,
                               unsigned int unit, unsigned int number, unsigned int halftrack, unsigned int side)
{
    char name[16];
    uint8_t major;
    uint8_t minor;
    uint8_t embed;
    uint8_t ht;
    uint8_t motor;
    uint8_t inclusive;
    uint8_t phase;
    uint8_t speed = 5;
    uint8_t source_phase = 0;
    int read_only = drive->read_only;
    uint8_t expected[20];
    uint8_t actual[20];
    uint8_t alignment = 0;
    uint8_t writing;
    int writable;
    uint32_t count;
    uint32_t i;
    snapshot_module_t *m;
    flux_capture_t *owned = NULL;
    const flux_capture_t *capture;
    rotation_flux_t restored = {0};
    flux_playback_state_t state = {0};
    int rc = -1;

    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_open(s, name, &major, &minor);
    if (!m) { /* Mandatory when DRIVE 2.1 selects native flux. */
        return -1;
    }
    if (major != 1 || minor > 5 || SMR_B(m, &embed) < 0 || embed > 1
        || SMR_BA(m, expected, sizeof(expected)) < 0
        || SMR_QW(m, &state.turn) < 0 || SMR_QW(m, &state.phase) < 0
        || SMR_QW(m, &state.duration) < 0 || SMR_QW(m, &state.elapsed) < 0
        || SMR_B(m, &ht) < 0 || SMR_B(m, &motor) < 0
        || SMR_B(m, &inclusive) < 0 || SMR_B(m, &phase) < 0) {
        goto done;
    }
    if (minor && (SMR_B(m, &speed) < 0 || SMR_B(m, &source_phase) < 0)) {
        goto done;
    }
    if ((speed != 5 && speed != 6) || phase >= (speed == 6 ? 25U : 5U)
        || source_phase >= (speed == 6 ? 6U : 1U)) {
        goto done;
    }
    if (minor >= 2 && (SMR_B(m, &alignment) < 0
                      || !rotation_flux_alignment_valid(alignment))) {
        goto done;
    }
    state.halftrack = ht;
    state.motor = motor;
    state.inclusive = inclusive;
    writable = minor == 3 || minor == 5;
    if (drive_flux_snapshot_validate(drive, halftrack) < 0
        || (!writable && !read_only)
        || (minor >= 4) != has_1571_clock_switch(drive->diskunit->type)
        || rotation_flux_track(halftrack, side, alignment) != ht
        || motor != !!(drive->byte_ready_active & BRA_MOTOR_ON)) {
        goto done;
    }
    if (writable) {
        if (SMR_B(m, &writing) < 0 || writing > 1 || (writing && read_only)) {
            goto done;
        }
        restored.writing = writing;
        if (writing) {
            if (!motor || SMR_QW(m, &restored.write_start) < 0
                || SMR_QW(m, &restored.write_elapsed) < 0
                || SMR_DW(m, &count) < 0 || count > FLUX_CAPTURE_MAX_WORDS) {
                goto done;
            }
            if (count) {
                restored.write_pulses = lib_malloc((size_t)count * sizeof(*restored.write_pulses));
            }
            restored.write_capacity = restored.write_count = count;
            for (i = 0; i < count; ++i) {
                if (SMR_QW(m, &restored.write_pulses[i]) < 0
                    || restored.write_pulses[i] > restored.write_elapsed
                    || restored.write_pulses[i] <= (i ? restored.write_pulses[i - 1] : 0)) {
                    goto done;
                }
            }
        }
    }
    if (embed) {
        owned = capture_read(m, minor, 0);
        if (!owned) {
            goto done;
        }
        capture = owned;
    } else {
        if (!drive->flux || !drive->image
            || drive->image->read_only == writable) {
            goto done;
        }
        capture = drive->flux->playback.capture;
    }
    capture_identity(capture, actual);
    if ((speed == 6 && !flux_capture_uses_360_clock(capture))
        || (writable && !drive_check_scp_write(drive->diskunit->type, flux_capture_flags(capture)))
        || memcmp(expected, actual, sizeof(actual))
        || flux_playback_restore(&restored.playback, capture, &state) != FLUX_OK) {
        goto done;
    }
    if (restored.writing) {
        const flux_track_t *track = flux_capture_track(capture, state.halftrack);
        uint64_t longest = state.duration;
        unsigned int rev;
        for (rev = 0; rev < flux_capture_revolutions(capture); ++rev) {
            uint64_t duration = flux_track_duration(track, rev);
            if (duration > longest) {
                longest = duration;
            }
        }
        if (restored.write_start >= longest) {
            goto done;
        }
    }
    restored.reference_phase = phase;
    restored.speed = speed;
    restored.source_phase = source_phase;
    restored.align_halftrack = alignment;
    if (snapshot_module_close(m) < 0) {
        m = NULL;
        goto done;
    }
    m = NULL;
    if (embed) {
        if (file_system_attach_flux_capture(unit, number, owned, !writable, speed == 6) < 0) {
            goto done;
        }
        owned = NULL;
    }
    lib_free(drive->flux->write_pulses);
    *drive->flux = restored;
    drive->read_only = read_only;
    restored.write_pulses = NULL;
    rc = 0;
done:
    if (m && snapshot_module_close(m) < 0) {
        rc = -1;
    }
    flux_capture_destroy(owned);
    lib_free(restored.write_pulses);
    if (rc < 0) {
        snapshot_set_error(SNAPSHOT_MODULE_INCOMPATIBLE);
        log_error(drive->diskunit->log, "Invalid native snapshot state or missing/mismatched attached capture.");
    }
    return rc;
}

/* FLUX 2.0 stores the read-only MFM medium. The controller and FDD modules
   separately carry controller, mechanical position and byte stream. */
/* Sector controllers have no live flux cursor. Store only the capture
   identity and, optionally, the capture itself. */
static int readonly_capture_valid(const flux_capture_t *capture, int mfm)
{
    unsigned int flags = flux_capture_flags(capture);
    return flux_capture_is_sealed(capture)
        && (flags & (FLUX_CAPTURE_MFM | FLUX_CAPTURE_DOUBLE_SIDED))
           == (mfm ? FLUX_CAPTURE_MFM : (flags & FLUX_CAPTURE_8X50) ? flags & FLUX_CAPTURE_DOUBLE_SIDED : 0);
}

int drive_sector_flux_snapshot_write(snapshot_t *s, const drive_t *drive,
                                      unsigned int unit, unsigned int number, int embed, int mfm)
{
    char name[16];
    uint8_t identity[20];
    const flux_capture_t *capture = disk_image_flux_capture_get(drive->image);
    snapshot_module_t *m;
    int rc;

    if (!drive->image || drive->read_only != drive->image->read_only || !readonly_capture_valid(capture, mfm)) {
        return -1;
    }
    capture_identity(capture, identity);
    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_create(s, name, mfm ? 2 : 3, !drive->read_only);
    if (!m) {
        return -1;
    }
    rc = SMW_B(m, embed != 0) < 0 || SMW_BA(m, identity, sizeof(identity)) < 0
        || (embed && capture_write(m, capture) < 0);
    return snapshot_module_close(m) < 0 || rc ? -1 : 0;
}

int drive_sector_flux_snapshot_read(snapshot_t *s, const drive_t *drive,
                                     unsigned int unit, unsigned int number, int mfm,
                                     int expected_embed, flux_capture_t **embedded)
{
    char name[16];
    uint8_t major;
    uint8_t minor;
    uint8_t embed;
    uint8_t expected[20];
    uint8_t actual[20];
    snapshot_module_t *m;
    flux_capture_t *owned = NULL;
    const flux_capture_t *capture;

    *embedded = NULL;
    snprintf(name, sizeof(name), "FLUX%u:%u", unit, number);
    m = snapshot_module_open(s, name, &major, &minor);
    if (!m) {
        goto fail;
    }
    if (major != (mfm ? 2 : 3) || minor != (unsigned int)!drive->read_only
        || SMR_B(m, &embed) < 0 || embed > 1
        || (expected_embed >= 0 && embed != expected_embed)
        || SMR_BA(m, expected, sizeof(expected)) < 0) {
        goto fail;
    }
    if (embed) {
        owned = capture_read(m, mfm ? 0 : 4, mfm);
        capture = owned;
    } else {
        capture = disk_image_flux_capture_get(drive->image);
        if (!drive->image || drive->image->read_only != drive->read_only) {
            goto fail;
        }
    }
    if (!readonly_capture_valid(capture, mfm)) {
        goto fail;
    }
    capture_identity(capture, actual);
    if (memcmp(expected, actual, sizeof(actual))) {
        goto fail;
    }
    if (snapshot_module_close(m) < 0) {
        m = NULL;
        goto fail;
    }
    *embedded = owned;
    return 0;

fail:
    if (m) {
        snapshot_module_close(m);
    }
    flux_capture_destroy(owned);
    snapshot_set_error(SNAPSHOT_MODULE_INCOMPATIBLE);
    log_error(drive->diskunit->log, "Invalid SCP snapshot or missing/mismatched capture on unit %u mechanism %u.",
              unit, number);
    return -1;
}
