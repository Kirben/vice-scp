/* Shared 1541 read circuit, with no drive or image ownership. GPL-2.0-or-later. */
#ifndef VICE_FLUX_LOGIC_H
#define VICE_FLUX_LOGIC_H
#include "flux-playback.h"

typedef struct flux_logic_s {
    unsigned int last_read_data;
    uint8_t last_write_data;
    int bit_counter;
    int ue7_dcba;
    int ue7_counter;
    int uf4_counter;
    uint32_t fr_randcount;
    int filter_counter;
    int filter_state;
    int filter_last_state;
    int so_delay;
    uint32_t cycle_index;
    uint32_t xorShift32;
} flux_logic_t;

typedef struct flux_logic_output_s {
    uint8_t byte;
    unsigned int ready_edge;
    unsigned int ready_level;
    int bit; /* -1 if this clock span did not sample a bit. */
} flux_logic_output_t;

#define FLUX_LOGIC_STOPPED 1
typedef int (*flux_logic_sink_t)(void *opaque, unsigned int bit);
uint32_t flux_logic_random(flux_logic_t *state);
uint64_t flux_logic_limit(const flux_logic_t *state, uint64_t limit);
void flux_logic_clock(flux_logic_t *state, flux_logic_output_t *out,
                       uint64_t clocks, int byte_ready);
/* Exact native timeline, clock before pulse on ties. Optional sink sees each
   sampled bit and may stop with FLUX_LOGIC_STOPPED. No allocation or I/O. */
int flux_logic_advance(flux_logic_t *state, flux_logic_output_t *out,
                        flux_playback_t *cursor, unsigned int *reference_phase,
                        uint64_t ticks, int byte_ready, flux_logic_sink_t sink, void *opaque);
/* 360 RPM acquisition played at nominal 300 RPM: exact 6/5 physical time.
   Capture intervals remain immutable. In this path reference_phase is in
   2.5 ns subticks (0..24); source_phase is elapsed subticks within one source
   tick (0..5). Carry both across calls, motor stops, head changes and saves.
   Input ticks are physical 12.5 ns ticks. Calls beyond UINT64_MAX/5 reject. */
int flux_logic_advance_scaled(flux_logic_t *state, flux_logic_output_t *out,
                              flux_playback_t *cursor, unsigned int *reference_phase,
                              unsigned int *source_phase, uint64_t ticks, int byte_ready,
                              flux_logic_sink_t sink, void *opaque, unsigned int numerator,
                              unsigned int denominator);
int flux_logic_advance_360(flux_logic_t *state, flux_logic_output_t *out,
                          flux_playback_t *cursor, unsigned int *reference_phase,
                          unsigned int *source_phase, uint64_t ticks, int byte_ready,
                          flux_logic_sink_t sink, void *opaque);
#endif
