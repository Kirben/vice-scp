/* Native flux input to the shared 1541 read electronics. GPL-2.0-or-later. */
#ifndef VICE_ROTATION_FLUX_H
#define VICE_ROTATION_FLUX_H

#include "../diskimage/flux-playback.h"
struct drive_s;

typedef struct rotation_flux_s {
    flux_playback_t playback;
    /* Elapsed native ticks since the last 16 MHz reference-clock edge (0..4).
       Head changes preserve this value; motor-off time freezes it. */
    unsigned int reference_phase;
    int fault; /* Latched by drive dispatch on exhausted/invalid clock state. */
} rotation_flux_t;

/* Internal drive adapter, not image attachment. The capture is borrowed and
   must remain alive. Binding does not reset the drive's electronics state. */
int rotation_flux_bind(rotation_flux_t *input, const flux_capture_t *capture,
                       unsigned int halftrack);
/* Discard all borrowed input pointers before the capture owner closes it. */
void rotation_flux_unbind(rotation_flux_t *input);
/* Drive lifecycle: rebase CPU time without moving the medium or decoder. */
void rotation_flux_rebase(struct drive_s *drive);
int rotation_flux_clock(struct drive_s *drive);
/* Advance exact native time; no I/O or allocation. At a reference-clock/flux
   tie, electronics clock first and input pulse second, as in the P64 reader.
   Multiple intervening pulses all reach the input filter, without rounding
   individual intervals to reference cycles. Captured turn boundaries are
   internal metadata, not an index input to the emulated 1541.
   The source is immutable even if the drive asserts its write gate: this
   adapter continues feeding the read circuit and never calls a writer.
   On a range error, stops at the last successfully completed span. */
int rotation_flux_advance(struct drive_s *drive, rotation_flux_t *input, uint64_t ticks);

#endif
