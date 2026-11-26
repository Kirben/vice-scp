/* Private 1541 read circuit and strict GCR blocks for disk tools. GPL-2.0-or-later. */
#include "vice.h"
#include <stdlib.h>
#include <string.h>
#include "cbmdos.h"
#include "diskimage.h"
#include "flux-logic.h"
#include "flux-view.h"
#include "gcr.h"

#define VIEW_TRACKS 42U
#define VIEW_SECTORS 29U
#define VIEW_MAX_WINDOW UINT64_C(32000000)
#define VIEW_HEADER_DISTANCE 4000U
#define VIEW_DATA_BLOCK_BYTES 258U /* Marker, 256 data bytes, and checksum. */

typedef struct view_track_s {
    uint32_t valid;
    int decoded;
    uint8_t id[2];
    uint64_t first_bit[VIEW_SECTORS];
    uint64_t last_bit[VIEW_SECTORS];
    uint8_t error[VIEW_SECTORS];
    uint8_t data[VIEW_SECTORS][256];
} view_track_t;
struct flux_view_s {
    const flux_capture_t *capture;
    view_track_t tracks[];
};
typedef struct block_reader_s {
    view_track_t *cache;
    const flux_playback_t *cursor;
    uint64_t block_start;
    unsigned int track;
    unsigned int sectors;
    unsigned int ones;
    unsigned int sync;
    unsigned int active;
    unsigned int code;
    unsigned int bits;
    unsigned int nibble;
    unsigned int byte;
    unsigned int length;
    unsigned int kind;
    unsigned int pending;
    unsigned int pending_sector;
    unsigned int distance;
    unsigned int data_sector;
    uint8_t pending_id[2];
    uint8_t block[VIEW_DATA_BLOCK_BYTES];
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
    unsigned int i;
    unsigned int check;
    int value;
    if (r->pending && ++r->distance > VIEW_HEADER_DISTANCE) {
        r->pending = 0;
    }
    if (bit) {
        if (r->ones < 10) {
            r->ones++;
        }
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
            r->block_start = r->cursor->state.elapsed;
            r->bits = r->code = r->nibble = r->byte = r->length = r->kind = 0;
            for (i = 0; i < r->sectors; i++) {
                if (r->cache->error[i] == CBMDOS_IPE_READ_ERROR_SYNC) {
                    r->cache->error[i] = CBMDOS_IPE_READ_ERROR_BNF;
                }
            }
        }
    }
    if (!r->active) {
        return 0;
    }
    r->code = (r->code << 1) | bit;
    if (++r->bits != 5) {
        return 0;
    }
    value = decode[r->code];
    r->code = r->bits = 0;
    if (value < 0) {
        bad_symbol(r);
        return 0;
    }
    r->byte = (r->byte << 4) | (unsigned int)value;
    if (++r->nibble != 2) {
        return 0;
    }
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
            if (r->pending) {
                error_set(r, r->pending_sector, CBMDOS_IPE_READ_ERROR_DATA);
            }
            r->pending = r->active = 0;
        }
    }
    if (r->kind == 8 && r->length == 8) {
        if (r->block[3] == r->track && r->block[2] < r->sectors) {
            if (r->block[1] == (r->block[2] ^ r->block[3] ^ r->block[4] ^ r->block[5])) {
                r->pending = 1;
                r->pending_sector = r->block[2];
                r->pending_id[0] = r->block[5];
                r->pending_id[1] = r->block[4];
                r->distance = 0;
                error_set(r, r->pending_sector, CBMDOS_IPE_READ_ERROR_DATA);
            } else {
                error_set(r, r->block[2], CBMDOS_IPE_READ_ERROR_BCHK);
            }
        }
        r->active = 0;
    } else if (r->kind == 7 && r->length == VIEW_DATA_BLOCK_BYTES) {
        /* The two following pad bytes carry no sector data or checksum. */
        check = 0;
        for (i = 1; i <= 256; i++) {
            check ^= r->block[i];
        }
        if (check == r->block[257]) {
            if (!(r->cache->valid & (1U << r->data_sector))) {
                if (!r->cache->valid) {
                    memcpy(r->cache->id, r->pending_id, sizeof(r->cache->id));
                }
                r->cache->first_bit[r->data_sector] = r->block_start;
                r->cache->last_bit[r->data_sector] = r->cursor->state.elapsed;
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

static void decode_track(const flux_capture_t *capture, view_track_t *cache,
                         unsigned int slot, unsigned int cylinder, unsigned int track,
                         unsigned int sectors, int wide)
{
    const flux_track_t *raw = flux_capture_track(capture, slot);
    flux_playback_t cursor;
    flux_logic_t logic;
    flux_logic_output_t out;
    block_reader_t reader = {0};
    flux_scan_t scan;
    uint64_t pulse;
    uint64_t duration;
    uint64_t window;
    unsigned int turn;
    unsigned int phase = 0;
    unsigned int source_phase = 0;
    unsigned int zone = disk_image_speed_map(wide ? DISK_IMAGE_TYPE_D82 : DISK_IMAGE_TYPE_D64,
                                             wide ? track : cylinder);
    int rpm360 = flux_capture_uses_360_clock(capture);
    /* The 8x50 GCR clock is 1.5 times the 1541 clock. */
    unsigned int numerator = wide ? (rpm360 ? 9 : 3) : 6;
    unsigned int denominator = rpm360 ? 5 : 2;
    int rc;
    int truncated;
    memset(cache->error, CBMDOS_IPE_READ_ERROR_BNF, sizeof(cache->error));
    cache->decoded = sectors;
    if (!raw || !flux_track_seek(raw, 0, 1, &scan, &pulse)) {
        return;
    }
    memset(cache->error, CBMDOS_IPE_READ_ERROR_SYNC, sizeof(cache->error));
    reader.cache = cache;
    reader.cursor = &cursor;
    reader.track = track;
    reader.sectors = sectors;
    circuit_reset(&logic, &out, &reader, zone);
    if (flux_playback_bind(&cursor, capture, slot) != FLUX_OK) {
        return;
    }
    for (turn = 0; turn <= flux_capture_revolutions(capture); turn++) {
        duration = cursor.state.duration - cursor.state.phase;
        if (wide || rpm360) {
            /* Round only the request boundary; the adapter carries the exact
               fraction into the next window. Keep the same wall-time limit. */
            truncated = duration > (VIEW_MAX_WINDOW * denominator + source_phase) / numerator;
            window = truncated ? VIEW_MAX_WINDOW
                : (duration * numerator - source_phase + denominator - 1) / denominator;
            rc = flux_logic_advance_scaled(&logic, &out, &cursor, &phase, &source_phase,
                                           window, 0, sector_bit, &reader, numerator, denominator);
        } else {
            truncated = duration > VIEW_MAX_WINDOW;
            window = truncated ? VIEW_MAX_WINDOW : duration;
            rc = flux_logic_advance(&logic, &out, &cursor, &phase, window, 0, sector_bit, &reader);
        }
        if (rc == FLUX_LOGIC_STOPPED || rc < 0) {
            break;
        }
        if (truncated) {
            /* Never concatenate a decoded block across an unobserved tail. */
            flux_playback_state_t next = {0};
            next.turn = cursor.state.turn + 1;
            next.halftrack = cursor.state.halftrack;
            next.motor = next.inclusive = 1;
            next.duration = flux_track_duration(raw, (unsigned int)(next.turn % flux_capture_revolutions(capture)));
            if (flux_playback_restore(&cursor, capture, &next) != FLUX_OK) {
                break;
            }
            phase = source_phase = 0;
            circuit_reset(&logic, &out, &reader, zone);
        }
    }
}

int flux_view_probe(const flux_capture_t *capture, unsigned int slot, unsigned int cylinder,
                    unsigned int logical_track)
{
    view_track_t cache;
    unsigned int sectors = cylinder < 18 ? 21 : cylinder < 25 ? 19 : cylinder < 31 ? 18 : 17;
    cache.valid = 0;
    decode_track(capture, &cache, slot, cylinder, logical_track, sectors, 0);
    return cache.valid != 0;
}

int flux_view_probe_8x50(const flux_capture_t *capture, unsigned int track)
{
    view_track_t cache = {0};
    unsigned int slot = ((track - 1) % 77) * 2 + (track > 77);
    decode_track(capture, &cache, slot, 0, track, disk_image_sector_per_track(DISK_IMAGE_TYPE_D82, track), 1);
    return cache.valid != 0;
}

flux_view_t *flux_view_create(const flux_capture_t *capture)
{
    flux_view_t *view;
    unsigned int tracks;

    if (!flux_capture_is_sealed(capture)) {
        return NULL;
    }
    tracks = (flux_capture_flags(capture) & FLUX_CAPTURE_8X50) ? 154 : 70;
    view = calloc(1, sizeof(*view) + tracks * sizeof(*view->tracks));
    if (view) {
        view->capture = capture;
    }
    return view;
}
void flux_view_destroy(flux_view_t *view)
{
    free(view);
}

static view_track_t *view_track(flux_view_t *view, unsigned int track, unsigned int sector)
{
    unsigned int sectors;
    unsigned int flags;
    unsigned int tracks;
    unsigned int slot;
    unsigned int cylinder;
    int double_sided;
    int wide;
    view_track_t *cache;
    if (!view) {
        return NULL;
    }
    flags = flux_capture_flags(view->capture);
    wide = (flags & FLUX_CAPTURE_8X50) != 0;
    double_sided = (flags & (FLUX_CAPTURE_DOUBLE_SIDED | FLUX_CAPTURE_MFM)) == FLUX_CAPTURE_DOUBLE_SIDED;
    tracks = wide ? ((flags & FLUX_CAPTURE_DOUBLE_SIDED) ? 154 : 77) : (double_sided ? 70 : VIEW_TRACKS);
    if (!track || track > tracks) {
        return NULL;
    }
    slot = wide ? ((track - 1) % 77) * 2 + (track > 77) : (track - 1) * 2;
    cylinder = !wide && double_sided ? (track - 1) % 35 + 1 : track;
    if (!wide && double_sided) {
        slot = (cylinder - 1) * 2 + (track > 35 ? FLUX_CAPTURE_TRACKS : 0);
    }
    sectors = disk_image_sector_per_track(wide ? DISK_IMAGE_TYPE_D82 : DISK_IMAGE_TYPE_D64, cylinder);
    /* DOS 1 adds sector 19 on tracks 18 through 24. Scan for it on request. */
    if (!wide && sector == 19 && track >= 18 && track < 25) {
        sectors = 20;
    }
    if (sector >= sectors) {
        return NULL;
    }
    cache = &view->tracks[track - 1];
    if (cache->decoded < sectors) {
        cache->valid = 0;
        decode_track(view->capture, cache, slot, cylinder, track, sectors, wide);
    }
    return cache;
}

int flux_view_read_disk_id(flux_view_t *view, unsigned int track, uint8_t id[2])
{
    view_track_t *cache = view_track(view, track, 0);
    if (!cache || !cache->valid) {
        return -1;
    }
    memcpy(id, cache->id, sizeof(cache->id));
    return 0;
}

int flux_view_read(flux_view_t *view, unsigned int track, unsigned int sector, uint8_t *buffer)
{
    view_track_t *cache;
    if (!view || !buffer) {
        return CBMDOS_IPE_NOT_READY;
    }
    cache = view_track(view, track, sector);
    if (!cache) {
        return CBMDOS_IPE_ILLEGAL_TRACK_OR_SECTOR;
    }
    if (!(flux_capture_flags(view->capture) & FLUX_CAPTURE_8X50)
        && sector == 19 && track >= 18 && track < 25
        && cache->error[sector] == CBMDOS_IPE_READ_ERROR_BNF) {
        return CBMDOS_IPE_ILLEGAL_TRACK_OR_SECTOR;
    }
    if (cache->error[sector] == CBMDOS_IPE_OK) {
        memcpy(buffer, cache->data[sector], 256);
    }
    return cache->error[sector];
}

static int pulse_compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int flux_view_write_sector(flux_view_t *view, unsigned int track, unsigned int sector,
                           const uint8_t *data, flux_capture_t **replacement)
{
    enum flux_view_data_layout_e {
        DATA_BITS = VIEW_DATA_BLOCK_BYTES * 10,
        GCR_DATA_OFFSET = 20
    };
    uint8_t gcr[SECTOR_GCR_SIZE_WITH_HEADER + 10];
    uint8_t check[256];
    uint64_t pulses[DATA_BITS];
    uint64_t first;
    uint64_t last;
    uint64_t duration;
    uint64_t start;
    uint64_t end;
    uint64_t span;
    uint64_t physical = 0;
    uint64_t offsets[FLUX_CAPTURE_MAX_REVS];
    unsigned int flags;
    unsigned int slot;
    unsigned int zone;
    unsigned int cell;
    unsigned int delay;
    unsigned int i;
    unsigned int count = 0;
    unsigned int rev = 0;
    const flux_track_t *raw;
    flux_view_t *verified;
    view_track_t *cache;
    gcr_header_t header = {0};
    int wide;
    int rc;
    int unindexed;

    *replacement = NULL;
    if (!view) {
        return FLUX_INVALID;
    }
    flags = flux_capture_flags(view->capture);
    wide = (flags & FLUX_CAPTURE_8X50) != 0;
    cache = view_track(view, track, sector);
    if (!cache || !(cache->valid & (1U << sector))) {
        return FLUX_INVALID;
    }
    if (memcmp(cache->data[sector], data, sizeof(check)) == 0) {
        return FLUX_OK;
    }
    slot = wide ? ((track - 1) % 77) * 2 + (track > 77) : (track - 1) * 2;
    raw = flux_capture_track(view->capture, slot);
    duration = flux_track_duration(raw, 0);
    zone = disk_image_speed_map(wide ? DISK_IMAGE_TYPE_D82 : DISK_IMAGE_TYPE_D64, track);
    cell = (16 - zone) * 20;
    delay = (40 + 2 * (16 - zone)) * 5;
    if (wide) {
        cell = cell * 2 / 3;
        delay = delay * 2 / 3;
    }

    if (flux_capture_uses_360_clock(view->capture)) {
        cell = (cell * 5 + 3) / 6;
        delay = (delay * 5 + 3) / 6;
    }

    first = cache->first_bit[sector];
    last = cache->last_bit[sector];
    unindexed = !(flags & FLUX_CAPTURE_INDEXED) && flux_capture_revolutions(view->capture) > 1;
    if (unindexed) {
        int rpm360 = flux_capture_uses_360_clock(view->capture);
        physical = flux_track_measure_gcr_turn(raw, track, cell);
        if (physical < (rpm360 ? 12500000 : 15000000)
            || physical > (rpm360 ? 14200000 : 17000000)
            || flux_track_period(raw) < physical * 2) {
            physical = 0;
        }
    }
    if (physical) {
        uint64_t angle = (first % flux_track_period(raw)) % physical;

        duration = physical;
        last = angle + (last - first);
        first = angle;
    } else if (flux_capture_revolutions(view->capture) > 1) {
        uint64_t period = flux_track_period(raw);
        uint64_t phase = first % period;
        while (phase >= flux_track_boundary(raw, rev + 1)) {
            ++rev;
        }
        duration = flux_track_duration(raw, rev);
        last = phase - flux_track_boundary(raw, rev) + (last - first);
        first = phase - flux_track_boundary(raw, rev);
    }
    if (last <= first || last - first + cell >= duration) {
        return FLUX_INVALID;
    }
    span = last - first;
    if (span < (DATA_BITS - 1U) * (cell - 4U)
        || span > (DATA_BITS - 1U) * (cell + 4U)) {
        return FLUX_INVALID;
    }

    /* Remove the read circuit's pulse filter and counter latency.
       Bound the splice halfway between the adjacent recorded bit cells. */
    first += duration - delay;
    last += duration - delay;
    start = ((first - cell / 2) % duration) & ~UINT64_C(1);
    end = ((last + cell / 2) % duration) & ~UINT64_C(1);
    if (!end) {
        end = duration;
    }
    gcr_convert_sector_to_GCR(data, gcr, &header, 0, 5, CBMDOS_FDC_ERR_OK);
    for (i = 0; i < DATA_BITS; i++) {
        if (gcr[GCR_DATA_OFFSET + i / 8] & (0x80 >> (i & 7))) {
            uint64_t tick = first + (span * i) / (DATA_BITS - 1);
            tick = (tick % duration) & ~UINT64_C(1);
            pulses[count++] = tick ? tick : duration;
        }
    }
    qsort(pulses, count, sizeof(*pulses), pulse_compare);
    if (physical) {
        rc = flux_capture_splice_unindexed(view->capture, slot, duration, start, end, pulses, count, replacement);
    } else if (unindexed) {
        if (!flux_track_gcr_write_offsets(raw, track, cell, rev, start, (int)sector, offsets)) {
            return FLUX_INVALID;
        }
        rc = flux_capture_splice_shifted(view->capture, slot, duration, start, end,
                                         pulses, count, offsets, replacement);
    } else {
        rc = flux_capture_splice(view->capture, slot, duration, start, end, pulses, count, replacement);
    }
    if (rc != FLUX_OK) {
        return rc;
    }
    verified = flux_view_create(*replacement);
    rc = flux_view_read(verified, track, sector, check);
    flux_view_destroy(verified);
    if (rc != CBMDOS_IPE_OK || memcmp(data, check, sizeof(check))) {
        flux_capture_destroy(*replacement);
        *replacement = NULL;
        return FLUX_INVALID;
    }
    return FLUX_OK;
}

/* Recover a nominal DD byte clock from captured MFM transitions. Keep the
   recorded bytes and missing-clock marks for the WD1770 to interpret. */
int flux_mfm_decode(const flux_track_t *track, unsigned int rev, unsigned int bytes, uint8_t *data, uint8_t *sync, int *phase_out)
{
    unsigned int cells = bytes * 16;
    uint8_t bits[FLUX_MFM_MAX_BYTES * 2] = {0};
    flux_scan_t scan;
    uint64_t start = flux_track_boundary(track, rev);
    uint64_t duration = flux_track_duration(track, rev);
    uint64_t pulse;
    int64_t clock;
    int64_t period;
    int64_t error;
    int64_t interval;
    int64_t steps;
    uint64_t position = 0;
    unsigned int i;
    unsigned int cell;
    unsigned int phase = 0;
    unsigned int mark = 0;
    unsigned int bit;
    uint16_t word = 0;

    if (phase_out) {
        *phase_out = -1;
    }
    if (!bytes || bytes > FLUX_MFM_MAX_BYTES || duration < 12000000 || duration > 18000000
        || !flux_track_seek(track, start, 0, &scan, &pulse)) {
        return 0;
    }
    period = (int64_t)duration;
    clock = 0;
    do {
        if (pulse - start > duration) {
            break;
        }
        interval = (int64_t)((pulse - start) * cells) - clock;
        steps = (interval + period / 2) / period;
        if (steps <= 0) {
            continue;
        }
        position += (uint64_t)steps;
        cell = (unsigned int)(position % cells);
        bits[cell >> 3] |= (uint8_t)(0x80 >> (cell & 7));
        error = interval - steps * period;
        clock += steps * period + error / 2;
        if (steps <= 4) {
            period += error / (steps * 16);
            if (period < (int64_t)duration * 9 / 10) {
                period = (int64_t)duration * 9 / 10;
            }
            if (period > (int64_t)duration * 11 / 10) {
                period = (int64_t)duration * 11 / 10;
            }
        }
    } while (flux_track_next(track, &scan, &pulse));

    /* Three consecutive missing-clock A1 words identify the byte phase. */
    for (i = 0; i < cells + 47; i++) {
        cell = i % cells;
        word = (uint16_t)((word << 1) | !!(bits[cell >> 3] & (0x80 >> (cell & 7))));
        if (word == 0x4489) {
            if (mark && i == mark + 32) {
                phase = (i + 1) & 15;
                break;
            }
            if (!mark || i != mark + 16) {
                mark = i;
            }
        }
    }
    if (i == cells + 47) {
        return 0;
    }

    if (phase_out) {
        *phase_out = (int)phase;
    }
    memset(sync, 0, (bytes + 7) / 8);
    for (i = 0; i < bytes; i++) {
        word = 0;
        for (bit = 0; bit < 16; bit++) {
            cell = (phase + i * 16 + bit) % cells;
            word = (uint16_t)((word << 1) | !!(bits[cell >> 3] & (0x80 >> (cell & 7))));
        }
        data[i] = 0;
        for (bit = 0; bit < 8; bit++) {
            data[i] = (uint8_t)((data[i] << 1) | ((word >> (14 - bit * 2)) & 1));
        }
        if (word == 0x4489 || word == 0x5224) {
            sync[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
    }
    return 1;
}

static int mfm_window_offsets(const flux_capture_t *capture, unsigned int slot,
                              unsigned int bytes, unsigned int reference_rev, uint64_t *offsets);

int flux_mfm_write_track(const flux_capture_t *source, unsigned int slot, unsigned int bytes,
                         const uint8_t *data, const uint8_t *sync,
                         unsigned int rev, int phase, unsigned int first, unsigned int length,
                         flux_capture_t **replacement)
{
    const flux_track_t *track = flux_capture_track(source, slot);
    uint64_t duration = track ? flux_track_duration(track, rev)
        : flux_capture_uses_360_clock(source) ? FLUX_DEFAULT_360_TURN_TICKS : FLUX_DEFAULT_TURN_TICKS;
    uint64_t *pulses;
    unsigned int i;
    unsigned int bit;
    unsigned int previous;
    unsigned int cells = bytes * 16;
    size_t count = 0;
    uint64_t start = 0;
    uint64_t end = duration;
    uint64_t offsets[FLUX_CAPTURE_MAX_REVS];
    unsigned int first_cell;
    unsigned int limit;
    unsigned int j;
    int rc;

    *replacement = NULL;
    if (!bytes || bytes > FLUX_MFM_MAX_BYTES || slot >= FLUX_CAPTURE_MAX_TRACKS
        || first >= bytes || !length || length > bytes - first
        || duration < 12000000 || duration > 18000000 || (duration & 1)) {
        return FLUX_INVALID;
    }
    if (!track || phase < 0 || length == bytes) {
        /* A complete revolution is controller-generated from the index.
           Supersede any offsets from the previous track contents. */
        phase = 0;
        first = 0;
        length = bytes;
    }
    first_cell = (unsigned int)phase + first * 16;
    limit = length * 16;
    if (length < bytes) {
        /* The first clock cell after the region depends on its last bit. */
        uint64_t half = duration / (2 * cells);
        start = ((first_cell * duration / cells + duration - half) % duration) & ~UINT64_C(1);
        end = (((first_cell + limit) * duration / cells + half) % duration) & ~UINT64_C(1);
        if (!end) {
            end = duration;
        }
        ++limit;
    }
    pulses = malloc(limit * sizeof(*pulses));
    if (!pulses) {
        return FLUX_NOMEM;
    }
    previous = data[(first + bytes - 1) % bytes] & 1;
    for (j = 0; j * 16 < limit; ++j) {
        unsigned int word = 0;

        i = (first + j) % bytes;
        if (sync[i >> 3] & (0x80 >> (i & 7))) {
            if (data[i] == 0xa1) {
                word = 0x4489;
            }
            else if (data[i] == 0xc2) {
                word = 0x5224;
            }
            else {
                free(pulses);
                return FLUX_INVALID;
            }
        } else {
            for (bit = 0; bit < 8; bit++) {
                unsigned int value = (data[i] >> (7 - bit)) & 1;
                word = (word << 2) | ((!previous && !value) << 1) | value;
                previous = value;
            }
        }
        previous = data[i] & 1;
        for (bit = 0; bit < 16 && j * 16 + bit < limit; ++bit) {
            if (word & (0x8000 >> bit)) {
                uint64_t index = (first_cell + j * 16 + bit) % cells;
                uint64_t tick = ((index * duration + cells) / (2 * cells)) * 2;
                pulses[count++] = tick ? tick : duration;
            }
        }
    }
    qsort(pulses, count, sizeof(*pulses), pulse_compare);
    if (length < bytes && track && flux_track_word_count(track) && flux_capture_revolutions(source) > 1
        && !(flux_capture_flags(source) & FLUX_CAPTURE_INDEXED)) {
        rc = mfm_window_offsets(source, slot, bytes, rev, offsets)
            ? flux_capture_splice_shifted(source, slot, duration, start, end, pulses, count, offsets, replacement)
            : FLUX_INVALID;
    } else {
        rc = flux_capture_splice(source, slot, duration, start, end, pulses, count, replacement);
    }
    free(pulses);
    return rc;
}

/* Collect checksum-valid address marks and their circular byte positions. */
static unsigned int mfm_address_positions(const uint8_t *data, const uint8_t *sync,
                                          unsigned int bytes, unsigned int slot, unsigned int positions[256])
{
    unsigned int i;
    unsigned int found = 0;
    for (i = 0; i < bytes; ++i) {
        unsigned int j;
        unsigned int crc = 0xffff;
        unsigned int sector;
        if (!(sync[i >> 3] & (0x80 >> (i & 7))) || data[i] != 0xa1) {
            continue;
        }
        if (data[(i + 1) % bytes] != 0xa1 || data[(i + 2) % bytes] != 0xa1
            || data[(i + 3) % bytes] != 0xfe) {
            continue;
        }
        for (j = 0; j < 10; ++j) {
            unsigned int bit;
            crc ^= data[(i + j) % bytes] << 8;
            for (bit = 0; bit < 8; ++bit) {
                crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
            }
        }
        if (crc) {
            continue;
        }
        if (data[(i + 4) % bytes] != slot / 2 || data[(i + 5) % bytes] != (slot & 1)) {
            return 0;
        }
        sector = data[(i + 6) % bytes];
        if (positions[sector]) {
            return 0;
        }
        positions[sector] = i + 1;
        ++found;
    }
    return found;
}

static int mfm_window_offsets(const flux_capture_t *capture, unsigned int slot,
                              unsigned int bytes, unsigned int reference_rev, uint64_t *offsets)
{
    uint8_t data[FLUX_MFM_MAX_BYTES];
    uint8_t sync[(FLUX_MFM_MAX_BYTES + 7) / 8];
    const flux_track_t *track = flux_capture_track(capture, slot);
    unsigned int reference[256] = {0};
    unsigned int rev;
    unsigned int cells = bytes * 16;
    uint64_t duration = flux_track_duration(track, reference_rev);
    int reference_phase;

    if (!flux_mfm_decode(track, reference_rev, bytes, data, sync, &reference_phase)
        || mfm_address_positions(data, sync, bytes, slot, reference) < 3) {
        return 0;
    }
    for (rev = 0; rev < flux_capture_revolutions(capture); ++rev) {
        unsigned int positions[256] = {0};
        unsigned int sector;
        unsigned int shift = cells;
        int phase;
        if (!flux_mfm_decode(track, rev, bytes, data, sync, &phase)
            || mfm_address_positions(data, sync, bytes, slot, positions) < 3) {
            return 0;
        }
        for (sector = 0; sector < 256; ++sector) {
            unsigned int a = reference[sector];
            unsigned int b = positions[sector];
            unsigned int delta;
            unsigned int distance;
            if (!a != !b) {
                return 0;
            }
            if (!a) {
                continue;
            }
            delta = (cells + b * 16 + phase - (a * 16 + reference_phase)) % cells;
            if (shift == cells) {
                shift = delta;
            }
            distance = delta > shift ? delta - shift : shift - delta;
            if (distance > 2 && distance < cells - 2) {
                return 0;
            }
        }
        offsets[rev] = (((uint64_t)shift * duration + cells) / (2 * cells) * 2) % duration;
    }
    return 1;
}

int flux_mfm_windows_aligned(const flux_capture_t *capture, unsigned int bytes)
{
    unsigned int slot;
    unsigned int checked = 0;
    uint64_t offsets[FLUX_CAPTURE_MAX_REVS];

    if (flux_capture_revolutions(capture) < 2 || !bytes || bytes > FLUX_MFM_MAX_BYTES) {
        return 0;
    }
    for (slot = 0; slot < FLUX_CAPTURE_MAX_TRACKS; ++slot) {
        if (!flux_track_word_count(flux_capture_track(capture, slot))) {
            continue;
        }
        if (!mfm_window_offsets(capture, slot, bytes, 0, offsets)) {
            return 0;
        }
        ++checked;
    }
    return checked != 0;
}

int flux_mfm_probe(const flux_capture_t *capture, unsigned int slot, unsigned int bytes)
{
    uint8_t data[FLUX_MFM_MAX_BYTES];
    uint8_t sync[(FLUX_MFM_MAX_BYTES + 7) / 8];
    unsigned int sectors = bytes == FLUX_MFM_MAX_BYTES ? 20 : 10;
    unsigned int size_code = bytes == FLUX_MFM_DD_BYTES ? 2 : 3;
    unsigned int i;
    unsigned int j;
    unsigned int bit;
    uint16_t crc;
    if (!flux_mfm_decode(flux_capture_track(capture, slot), 0, bytes, data, sync, NULL)) {
        return 0;
    }
    for (i = 0; i + 10 <= bytes; i++) {
        if (data[i] != 0xa1 || data[i+1] != 0xa1 || data[i+2] != 0xa1
            || data[i+3] != 0xfe || data[i+4] != slot / 2
            || data[i+5] != (slot & 1) || data[i+6] != sectors
            || data[i+7] != size_code || !(sync[i >> 3] & (0x80 >> (i & 7)))) {
            continue;
        }
        crc = 0xffff;
        for (j = i; j < i + 10; j++) {
            crc ^= (uint16_t)data[j] << 8;
            for (bit = 0; bit < 8; bit++) {
                crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
            }
        }
        if (!crc) {
            return 1;
        }
    }
    return 0;
}
