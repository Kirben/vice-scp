/* Compile the real rotation implementation twice: upstream and modified.
   Input injection represents the physical read-amplifier transition only;
   clocks, filtering, RNG, sync and byte-ready remain production code. */
#include "vice.h"
#include <stdlib.h>
#include <string.h>
#include "drive.h"
#include "log.h"
#include "rotation-flux.h"
#include NATIVE_ROTATION_SOURCE

#ifdef NATIVE_REFERENCE_ONLY
#define READ_STATE(p) (p)
#else
#define READ_STATE(p) (&(p)->logic)
#endif

struct diskunit_context_s *diskunit_context[NUM_DISK_UNITS];
static diskunit_context_t unit;
static drive_t drive;
static TP64Image image;
static CLOCK clock_value;
static int initialized;
#ifndef NATIVE_REFERENCE_ONLY
static rotation_flux_t input;
#endif

void *lib_malloc(size_t size) { return malloc(size); }
void *lib_realloc(void *p, size_t size) { return realloc(p, size); }
void lib_free(void *p) { free(p); }
int log_error(log_t log, const char *format, ...) { (void)log; (void)format; return 0; }

static uint32_t scramble(uint32_t *value)
{
    *value ^= *value << 13;
    *value ^= *value >> 17;
    return *value ^= *value << 5;
}

void electronics_init(uint32_t seed, unsigned int zone, int reading)
{
    rotation_t *r = &rotation[0];
    if (initialized) { P64ImageDestroy(&image); }
    memset(&unit, 0, sizeof(unit));
    memset(&drive, 0, sizeof(drive));
    memset(rotation, 0, sizeof(rotation));
    clock_value = 0;
    unit.mynumber = 0;
    unit.clk_ptr = &clock_value;
    unit.drives[0] = &drive;
    drive.diskunit = &unit;
    diskunit_context[0] = &unit;
    drive.current_half_track = 2;
    drive.p64 = &image;
    drive.byte_ready_active = BRA_BYTE_READY | BRA_MOTOR_ON;
    drive.read_write_mode = reading;
    drive.GCR_write_value = (uint8_t)seed;
    P64ImageCreate(&image);
    rotation_init(0, 0);
    rotation_reset(&drive);
    rotation_speed_zone_set(zone, 0);
    if (seed) {
        r->accum = scramble(&seed);
        READ_STATE(r)->last_read_data = scramble(&seed) & 1023;
        READ_STATE(r)->last_write_data = (uint8_t)scramble(&seed);
        READ_STATE(r)->bit_counter = scramble(&seed) % 8;
        READ_STATE(r)->ue7_counter = scramble(&seed) % 16;
        READ_STATE(r)->uf4_counter = scramble(&seed) % 16;
        READ_STATE(r)->fr_randcount = scramble(&seed) % 400;
        READ_STATE(r)->filter_counter = scramble(&seed) % 41;
        READ_STATE(r)->filter_state = scramble(&seed) & 1;
        READ_STATE(r)->filter_last_state = scramble(&seed) & 1;
        READ_STATE(r)->so_delay = scramble(&seed) % 32;
        READ_STATE(r)->cycle_index = scramble(&seed);
        r->seed = scramble(&seed);
        READ_STATE(r)->xorShift32 = scramble(&seed);
    }
#ifndef NATIVE_REFERENCE_ONLY
    memset(&input, 0, sizeof(input));
#endif
    initialized = 1;
}

void electronics_finish(void)
{
    if (initialized) { P64ImageDestroy(&image); initialized = 0; }
#ifndef NATIVE_REFERENCE_ONLY
    rotation_flux_unbind(&input);
#endif
}
void electronics_add_p64(uint32_t position, uint32_t strength)
{
    P64PulseStreamAddPulse(&image.PulseStreams[0][2], position, strength);
}
void electronics_p64_clocks(uint64_t clocks) { rotation_1541_p64(&drive, clocks); }
void electronics_cpu_clocks(uint64_t clocks, unsigned int requested)
{
    clock_value += clocks;
    drive.req_ref_cycles = requested;
    rotation_1541_p64_cycle(&drive);
}
void electronics_pulse(void)
{
    READ_STATE(&rotation[0])->filter_state ^= 1;
    READ_STATE(&rotation[0])->filter_counter = 0;
}
void electronics_ack(void) { drive.byte_ready_edge = drive.byte_ready_level = 0; }
void electronics_zone(unsigned int zone) { rotation_speed_zone_set(zone, 0); }
void electronics_mode(int reading) { drive.read_write_mode = reading; }

unsigned int electronics_state(uint64_t *out)
{
    rotation_t *r = &rotation[0];
    PP64PulseStream stream = &image.PulseStreams[0][2];
    int32_t at = stream->UsedFirst;
    uint64_t hash = UINT64_C(14695981039346656037), count = 0;
    while (at >= 0) {
        hash = (hash ^ stream->Pulses[at].Position) * UINT64_C(1099511628211);
        hash = (hash ^ stream->Pulses[at].Strength) * UINT64_C(1099511628211);
        count++;
        at = stream->Pulses[at].Next;
    }
    {
        uint64_t fields[] = {r->accum, r->rotation_last_clk, READ_STATE(r)->last_read_data,
            READ_STATE(r)->last_write_data, READ_STATE(r)->bit_counter, r->zero_count, r->frequency,
            r->speed_zone, READ_STATE(r)->ue7_dcba, READ_STATE(r)->ue7_counter, READ_STATE(r)->uf4_counter,
            READ_STATE(r)->fr_randcount, READ_STATE(r)->filter_counter, READ_STATE(r)->filter_state, READ_STATE(r)->filter_last_state,
            r->write_flux, READ_STATE(r)->so_delay, READ_STATE(r)->cycle_index, r->ref_advance,
            r->PulseHeadPosition, r->seed, READ_STATE(r)->xorShift32, drive.GCR_read,
            drive.GCR_write_value, drive.byte_ready_edge, drive.byte_ready_level,
            drive.req_ref_cycles, drive.read_write_mode, count, hash,
            (uint64_t)(int64_t)stream->CurrentIndex, drive.P64_dirty};
        memcpy(out, fields, sizeof(fields));
        return sizeof(fields) / sizeof(fields[0]);
    }
}

#ifndef NATIVE_REFERENCE_ONLY
int electronics_bind(const flux_capture_t *capture, unsigned int halftrack)
{
    return rotation_flux_bind(&input, capture, halftrack);
}
int electronics_advance(uint64_t ticks) { return rotation_flux_advance(&drive, &input, ticks); }
void electronics_drive_bind(void) { drive.flux = &input; rotation_flux_rebase(&drive); }
int electronics_drive_clock(uint64_t clocks, unsigned int requested)
{
    clock_value += clocks;
    drive.req_ref_cycles = requested;
    rotation_rotate_disk(&drive);
    return input.fault ? FLUX_RANGE : FLUX_OK;
}
void electronics_drive_motor(int motor)
{
    rotation_rotate_disk(&drive);
    drive.byte_ready_active = (drive.byte_ready_active & ~BRA_MOTOR_ON) | (motor ? BRA_MOTOR_ON : 0);
    if (motor) { rotation_begins(&drive); }
}
void electronics_drive_reset(void) { rotation_reset(&drive); }
void electronics_wobble(int rpm, int amplitude)
{
    drive.rpm = rpm;
    drive.wobble_amplitude = amplitude;
    drive.wobble_frequency = 12345;
}
void electronics_unbind(void) { rotation_flux_unbind(&input); }
int electronics_step(unsigned int halftrack) { return flux_playback_change_track(&input.playback, halftrack); }
unsigned int electronics_rate(void) { return (input.speed << 8) | input.source_phase; }
void electronics_motor(int motor) { flux_playback_set_motor(&input.playback, motor); }
void electronics_flux_state(flux_playback_state_t *state, unsigned int *phase)
{
    *state = input.playback.state;
    *phase = input.reference_phase;
}
int electronics_restore(const flux_capture_t *capture, const flux_playback_state_t *state, unsigned int phase)
{
    int rc;
    if (phase >= 5) { return FLUX_INVALID; }
    rc = flux_playback_restore(&input.playback, capture, state);
    if (!rc) { input.reference_phase = phase; }
    return rc;
}
int electronics_invalid(unsigned int which)
{
    rotation_flux_t broken = input;
    switch (which) {
        case 0: return rotation_flux_advance(NULL, &input, 1);
        case 1: return rotation_flux_advance(&drive, NULL, 1);
        case 2: broken.reference_phase = 5; break;
        case 3: broken.playback.capture = NULL; break;
        case 4: broken.source_phase = 6; break;
        case 5: broken.speed = 7; break;
        case 6: broken.reference_phase = 25; break;
        default: return rotation_flux_bind(NULL, NULL, 0);
    }
    return rotation_flux_advance(&drive, &broken, 1);
}
#endif
