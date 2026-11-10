/* Native flux input to the shared 1541 read electronics. GPL-2.0-or-later. */
#ifndef VICE_ROTATION_FLUX_H
#define VICE_ROTATION_FLUX_H

#include "../diskimage/flux-playback.h"
struct drive_s;

typedef struct rotation_flux_s {
    flux_playback_t playback;
    /* Time since the last 16 MHz edge: 12.5 ns ticks at captured speed
       (0..4), or 2.5 ns subticks with 6/5 compensation (0..24).
       Head changes preserve this value; motor-off time freezes it. */
    unsigned int reference_phase;
    unsigned int speed; /* 5: capture clock; 6: 360 RPM capture at 300 RPM. */
    unsigned int source_phase; /* 2.5 ns subticks within a scaled source tick. */
    int fault; /* Latched by drive dispatch on exhausted/invalid clock state. */
    unsigned int align_halftrack; /* 0, or an odd physical head position. */
    uint64_t write_start;
    uint64_t write_elapsed;
    uint64_t *write_pulses;
    size_t write_count;
    size_t write_capacity;
    int writing;
} rotation_flux_t;
/* Attachment policy: 0 auto from capture metadata, 1 captured clock. */
int drive_flux_speed_mode(unsigned int unit_index);
int drive_flux_align_halftrack(unsigned int unit_index);

static inline int rotation_flux_alignment_valid(unsigned int halftrack)
{
    return halftrack == 0 || (halftrack >= 3 && halftrack <= FLUX_CAPTURE_TRACKS + 1
                             && (halftrack & 1));
}

/* Convert the physical head position to the capture entry. */
static inline unsigned int rotation_flux_track(unsigned int head, unsigned int side, unsigned int alignment)
{
    return side * FLUX_CAPTURE_TRACKS + head - 2 - (head == alignment);
}

/* Internal drive adapter, not image attachment. The capture is borrowed and
   must remain alive. Binding does not reset the drive's electronics state. */
int rotation_flux_bind(rotation_flux_t *input, const flux_capture_t *capture,
                       unsigned int halftrack);
/* Discard all borrowed input pointers before the capture owner closes it. */
void rotation_flux_unbind(rotation_flux_t *input);
/* Drive lifecycle: rebase CPU time without moving the medium or decoder. */
void rotation_flux_rebase(struct drive_s *drive);
int rotation_flux_clock(struct drive_s *drive);
int rotation_flux_finish_write(struct drive_s *drive);
/* Advance exact native time; no I/O or allocation. At a reference-clock/flux
   tie, electronics clock first and input pulse second, as in the P64 reader.
   Multiple intervening pulses all reach the input filter, without rounding
   individual intervals to reference cycles. Captured turn boundaries are
   internal metadata, not an index input to the emulated 1541.
   On a range error, stops at the last successfully completed span. */
int rotation_flux_advance(struct drive_s *drive, rotation_flux_t *input, uint64_t ticks);

#endif
