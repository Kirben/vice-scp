/* Private 1541 read circuit and strict GCR blocks for disk tools. GPL-2.0-or-later. */
#include "vice.h"
#include <stdlib.h>
#include <string.h>
#include "cbmdos.h"
#include "flux-logic.h"
#include "flux-view.h"

#define VIEW_TRACKS 42U
#define VIEW_SECTORS 21U
#define VIEW_MAX_WINDOW UINT64_C(32000000)
#define VIEW_HEADER_DISTANCE 4000U

typedef struct view_track_s {
    uint32_t valid;
    int decoded;
    uint8_t error[VIEW_SECTORS];
    uint8_t data[VIEW_SECTORS][256];
} view_track_t;
struct flux_view_s {
    const flux_capture_t *capture;
    view_track_t tracks[VIEW_TRACKS];
};
typedef struct block_reader_s {
    view_track_t *cache;
    unsigned int track, sectors, ones, sync, active, code, bits, nibble, byte;
    unsigned int length, kind, pending, pending_sector, distance, data_sector;
    uint8_t block[260];
} block_reader_t;

static const int8_t decode[32] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,8,0,1,-1,12,4,5,
    -1,-1,2,3,-1,15,6,7,-1,9,10,11,-1,13,14,-1
};

static void error_set(block_reader_t *reader, unsigned int sector, uint8_t error)
{
    if (sector < reader->sectors && !(reader->cache->valid & (1U << sector))) {
        reader->cache->error[sector] = error;
    }
}

static void bad_symbol(block_reader_t *r)
{
    if (r->kind == 7) {
        error_set(r, r->data_sector, CBMDOS_IPE_READ_ERROR_GCR);
    } else if (r->pending) {
        error_set(r, r->pending_sector, CBMDOS_IPE_READ_ERROR_GCR);
    }
    r->pending = r->active = 0;
}

static int sector_bit(void *opaque, unsigned int bit)
{
    block_reader_t *r = opaque;
    unsigned int i, check;
    int value;
    if (r->pending && ++r->distance > VIEW_HEADER_DISTANCE) { r->pending = 0; }
    if (bit) {
        if (r->ones < 10) { r->ones++; }
        if (r->ones == 10) {
            r->sync = 1;
            r->active = 0;
            return 0;
        }
    } else {
        r->ones = 0;
        if (r->sync) {
            r->sync = 0;
            r->active = 1;
            r->bits = r->code = r->nibble = r->byte = r->length = r->kind = 0;
            for (i = 0; i < r->sectors; i++) {
                if (r->cache->error[i] == CBMDOS_IPE_READ_ERROR_SYNC) {
                    r->cache->error[i] = CBMDOS_IPE_READ_ERROR_BNF;
                }
            }
        }
    }
    if (!r->active) { return 0; }
    r->code = (r->code << 1) | bit;
    if (++r->bits != 5) { return 0; }
    value = decode[r->code];
    r->code = r->bits = 0;
    if (value < 0) { bad_symbol(r); return 0; }
    r->byte = (r->byte << 4) | (unsigned int)value;
    if (++r->nibble != 2) { return 0; }
    r->nibble = 0;
    r->block[r->length++] = (uint8_t)r->byte;
    r->byte = 0;
    if (r->length == 1) {
        r->kind = r->block[0];
        if (r->kind == 8) {
            r->pending = 0;
        } else if (r->kind == 7 && r->pending) {
            r->data_sector = r->pending_sector;
            r->pending = 0;
        } else {
            if (r->pending) { error_set(r, r->pending_sector, CBMDOS_IPE_READ_ERROR_DATA); }
            r->pending = r->active = 0;
        }
    }
    if (r->kind == 8 && r->length == 8) {
        if (r->block[3] == r->track && r->block[2] < r->sectors) {
            if (r->block[1] == (r->block[2] ^ r->block[3] ^ r->block[4] ^ r->block[5])) {
                r->pending = 1;
                r->pending_sector = r->block[2];
                r->distance = 0;
                error_set(r, r->pending_sector, CBMDOS_IPE_READ_ERROR_DATA);
            } else {
                error_set(r, r->block[2], CBMDOS_IPE_READ_ERROR_BCHK);
            }
        }
        r->active = 0;
    } else if (r->kind == 7 && r->length == 260) {
        check = 0;
        for (i = 1; i <= 256; i++) { check ^= r->block[i]; }
        if (check == r->block[257]) {
            if (!(r->cache->valid & (1U << r->data_sector))) {
                memcpy(r->cache->data[r->data_sector], r->block + 1, 256);
                r->cache->error[r->data_sector] = CBMDOS_IPE_OK;
                r->cache->valid |= 1U << r->data_sector;
            }
        } else {
            error_set(r, r->data_sector, CBMDOS_IPE_READ_ERROR_CHK);
        }
        r->active = 0;
    }
    return r->cache->valid == (1U << r->sectors) - 1;
}

static void circuit_reset(flux_logic_t *state, flux_logic_output_t *out,
                           block_reader_t *reader, unsigned int zone)
{
    memset(state, 0, sizeof(*state));
    memset(out, 0, sizeof(*out));
    state->ue7_dcba = zone;
    state->xorShift32 = 0x1234abcd;
    reader->pending = reader->active = reader->ones = reader->sync = 0;
}

static void decode_track(flux_view_t *view, unsigned int track, unsigned int sectors)
{
    view_track_t *cache = &view->tracks[track - 1];
    const flux_track_t *raw = flux_capture_track(view->capture, (track - 1) * 2);
    flux_playback_t cursor;
    flux_logic_t logic;
    flux_logic_output_t out;
    block_reader_t reader = {0};
    flux_scan_t scan;
    uint64_t pulse, duration, window;
    unsigned int turn, phase = 0, source_phase = 0, zone = (track < 31) + (track < 25) + (track < 18);
    int rc, truncated, scaled = !!(flux_capture_flags(view->capture) & FLUX_CAPTURE_360_RPM);
    memset(cache->error, CBMDOS_IPE_READ_ERROR_BNF, sizeof(cache->error));
    cache->decoded = 1;
    if (!raw || !flux_track_seek(raw, 0, 1, &scan, &pulse)) { return; }
    memset(cache->error, CBMDOS_IPE_READ_ERROR_SYNC, sizeof(cache->error));
    reader.cache = cache;
    reader.track = track;
    reader.sectors = sectors;
    circuit_reset(&logic, &out, &reader, zone);
    if (flux_playback_bind(&cursor, view->capture, (track - 1) * 2) != FLUX_OK) { return; }
    for (turn = 0; turn <= flux_capture_revolutions(view->capture); turn++) {
        duration = cursor.state.duration - cursor.state.phase;
        if (scaled) {
            /* Round only the request boundary; the adapter carries the exact
               fraction into the next window. Keep the same wall-time limit. */
            truncated = duration > (VIEW_MAX_WINDOW * 5 + source_phase) / 6;
            window = truncated ? VIEW_MAX_WINDOW : (duration * 6 - source_phase + 4) / 5;
            rc = flux_logic_advance_360(&logic, &out, &cursor, &phase, &source_phase,
                                        window, 0, sector_bit, &reader);
        } else {
            truncated = duration > VIEW_MAX_WINDOW;
            window = truncated ? VIEW_MAX_WINDOW : duration;
            rc = flux_logic_advance(&logic, &out, &cursor, &phase, window, 0, sector_bit, &reader);
        }
        if (rc == FLUX_LOGIC_STOPPED || rc < 0) { break; }
        if (truncated) {
            /* Never concatenate a decoded block across an unobserved tail. */
            flux_playback_state_t next = {0};
            next.turn = cursor.state.turn + 1;
            next.halftrack = cursor.state.halftrack;
            next.motor = next.inclusive = 1;
            next.duration = flux_track_duration(raw, (unsigned int)(next.turn % flux_capture_revolutions(view->capture)));
            if (flux_playback_restore(&cursor, view->capture, &next) != FLUX_OK) { break; }
            phase = source_phase = 0;
            circuit_reset(&logic, &out, &reader, zone);
        }
    }
}

flux_view_t *flux_view_create(const flux_capture_t *capture)
{
    flux_view_t *view;
    if (!flux_capture_is_sealed(capture)) { return NULL; }
    view = calloc(1, sizeof(*view));
    if (view) { view->capture = capture; }
    return view;
}
void flux_view_destroy(flux_view_t *view) { free(view); }

int flux_view_read(flux_view_t *view, unsigned int track, unsigned int sector, uint8_t *buffer)
{
    unsigned int sectors;
    view_track_t *cache;
    if (!view || !buffer) { return CBMDOS_IPE_NOT_READY; }
    if (!track || track > VIEW_TRACKS) { return CBMDOS_IPE_ILLEGAL_TRACK_OR_SECTOR; }
    sectors = track < 18 ? 21 : track < 25 ? 19 : track < 31 ? 18 : 17;
    if (sector >= sectors) { return CBMDOS_IPE_ILLEGAL_TRACK_OR_SECTOR; }
    cache = &view->tracks[track - 1];
    if (!cache->decoded) { decode_track(view, track, sectors); }
    if (cache->error[sector] == CBMDOS_IPE_OK) { memcpy(buffer, cache->data[sector], 256); }
    return cache->error[sector];
}
