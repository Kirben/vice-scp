/* Native flux playback cursor. Part of VICE; GPL-2.0-or-later. */
#ifndef VICE_FLUX_PLAYBACK_H
#define VICE_FLUX_PLAYBACK_H
#include "flux-capture.h"

#define FLUX_EVENT_NONE 0U
#define FLUX_EVENT_PULSE 1U
#define FLUX_EVENT_TURN 2U
#define FLUX_EVENT_LOOP 4U

typedef struct flux_event_s {
    uint64_t delta;
    unsigned int kind;
} flux_event_t;

/* Defined integer state for snapshot adapters; pointers/scans are rebuilt. */
typedef struct flux_playback_state_s {
    uint64_t turn;
    uint64_t phase;
    uint64_t duration;
    uint64_t elapsed;
    unsigned int halftrack;
    unsigned int motor;
    unsigned int inclusive;
} flux_playback_state_t;

typedef struct flux_playback_s {
    const flux_capture_t *capture;
    const flux_track_t *track;
    flux_playback_state_t state;
    flux_scan_t scan;
    uint64_t pulse;
    uint64_t physical_turn; /* Optional continuous angle at the acquisition seam. */
    int has_pulse; /* 0: none, 1: interval edge, 2: measured capture-end edge. */
} flux_playback_t;

/* capture must be sealed and remain alive while a cursor borrows it. */
int flux_playback_bind(flux_playback_t *cursor, const flux_capture_t *capture,
                       unsigned int halftrack);
int flux_playback_next_event(const flux_playback_t *cursor, flux_event_t *event);
/* Advances at most to the next event. On success, kind is the event consumed
   or NONE for a partial advance. Overstepping/overflow never changes state.
   Motor-off time does not advance capture phase. */
int flux_playback_advance(flux_playback_t *cursor, uint64_t ticks, unsigned int *kind);
int flux_playback_change_track(flux_playback_t *cursor, unsigned int halftrack);
void flux_playback_set_motor(flux_playback_t *cursor, int motor);
int flux_playback_restore(flux_playback_t *cursor, const flux_capture_t *capture,
                          const flux_playback_state_t *state);
#endif
