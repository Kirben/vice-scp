/* Native flux playback cursor. Part of VICE; GPL-2.0-or-later. */
#include "flux-playback.h"
#include <string.h>

/* floor(a*b/divisor), with a < divisor, without a nonportable 128-bit type.
   Used only on head changes, not in the event loop. */
static uint64_t map_phase(uint64_t a, uint64_t b, uint64_t divisor)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    unsigned int bit = 64;
    while (bit--) {
        quotient *= 2;
        if (remainder >= divisor - remainder) {
            remainder -= divisor - remainder;
            quotient++;
        } else {
            remainder *= 2;
        }
        if ((b >> bit) & 1) {
            if (remainder >= divisor - a) {
                remainder -= divisor - a;
                quotient++;
            } else {
                remainder += a;
            }
        }
    }
    return quotient;
}

static unsigned int revolution(const flux_playback_t *cursor)
{
    return (unsigned int)(cursor->state.turn % flux_capture_revolutions(cursor->capture));
}

static void seek(flux_playback_t *cursor)
{
    /* A measured edge exactly at the final capture endpoint belongs to the
       loop seam. Emit it after the turn boundary, once, before replaying the
       first interval. Do not synthesize an edge when the endpoint has none.
       Pending state is derived from turn/phase/inclusive, including restores. */
    if (cursor->track && cursor->state.turn && !revolution(cursor)
        && !cursor->state.phase && cursor->state.inclusive
        && flux_track_seek(cursor->track, flux_track_period(cursor->track), 1,
                           &cursor->scan, &cursor->pulse)
        && cursor->pulse == flux_track_period(cursor->track)) {
        cursor->pulse = 0;
        cursor->has_pulse = 2; /* Endpoint edge, followed by the first interval. */
        return;
    }
    cursor->has_pulse = cursor->track
        && flux_track_seek(cursor->track,
            flux_track_boundary(cursor->track, revolution(cursor)) + cursor->state.phase,
            cursor->state.inclusive, &cursor->scan, &cursor->pulse);
}

int flux_playback_bind(flux_playback_t *cursor, const flux_capture_t *capture,
                       unsigned int halftrack)
{
    flux_playback_state_t state;
    if (!cursor || !flux_capture_is_sealed(capture) || !flux_capture_revolutions(capture)
        || halftrack >= FLUX_CAPTURE_MAX_TRACKS) {
        return FLUX_INVALID;
    }
    memset(&state, 0, sizeof(state));
    state.halftrack = halftrack;
    state.motor = 1;
    state.inclusive = 1;
    state.duration = flux_track_duration(flux_capture_track(capture, halftrack), 0);
    if (!state.duration) {
        /* Fallback is in the acquisition clock domain, like real windows. */
        state.duration = flux_capture_uses_360_clock(capture)
            ? FLUX_DEFAULT_360_TURN_TICKS : FLUX_DEFAULT_TURN_TICKS;
    }
    return flux_playback_restore(cursor, capture, &state);
}

int flux_playback_restore(flux_playback_t *cursor, const flux_capture_t *capture,
                          const flux_playback_state_t *state)
{
    flux_playback_t replacement;
    uint64_t expected;
    if (!cursor || !flux_capture_is_sealed(capture) || !state || !flux_capture_revolutions(capture)
        || state->halftrack >= FLUX_CAPTURE_MAX_TRACKS || state->motor > 1
        || state->inclusive > 1 || !state->duration || state->phase >= state->duration) {
        return FLUX_INVALID;
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.capture = capture;
    replacement.state = *state;
    replacement.track = flux_capture_track(capture, state->halftrack);
    expected = flux_track_duration(replacement.track, revolution(&replacement));
    if (expected && expected != state->duration) {
        return FLUX_INVALID;
    }
    seek(&replacement);
    *cursor = replacement;
    return FLUX_OK;
}

int flux_playback_next_event(const flux_playback_t *cursor, flux_event_t *event)
{
    uint64_t relative;
    if (!cursor || !cursor->capture || !event) {
        return FLUX_INVALID;
    }
    event->delta = UINT64_MAX;
    event->kind = FLUX_EVENT_NONE;
    if (!cursor->state.motor) {
        return FLUX_OK;
    }
    event->delta = cursor->state.duration - cursor->state.phase;
    event->kind = FLUX_EVENT_TURN;
    if (revolution(cursor) + 1 == flux_capture_revolutions(cursor->capture)) {
        event->kind |= FLUX_EVENT_LOOP;
    }
    if (cursor->has_pulse) {
        relative = cursor->pulse - flux_track_boundary(cursor->track, revolution(cursor));
        if (relative >= cursor->state.phase && relative < cursor->state.duration) {
            event->delta = relative - cursor->state.phase;
            event->kind = FLUX_EVENT_PULSE;
        }
    }
    return FLUX_OK;
}

int flux_playback_advance(flux_playback_t *cursor, uint64_t ticks, unsigned int *kind)
{
    flux_event_t event;
    uint64_t next_phase = 0;
    unsigned int skipped = 0;
    if (!kind || flux_playback_next_event(cursor, &event) != FLUX_OK) {
        return FLUX_INVALID;
    }
    if (!cursor->state.motor) {
        *kind = FLUX_EVENT_NONE;
        return FLUX_OK;
    }
    if (ticks == event.delta && (event.kind & FLUX_EVENT_LOOP)
        && cursor->physical_turn && cursor->track) {
        next_phase = flux_track_period(cursor->track) % cursor->physical_turn;
        while (next_phase >= flux_track_duration(cursor->track, skipped)) {
            next_phase -= flux_track_duration(cursor->track, skipped++);
        }
    }
    if (ticks > event.delta || ticks > UINT64_MAX - cursor->state.elapsed
        || (ticks == event.delta && (event.kind & FLUX_EVENT_TURN)
            && cursor->state.turn >= UINT64_MAX - skipped)) {
        return FLUX_RANGE;
    }
    *kind = ticks == event.delta ? event.kind : FLUX_EVENT_NONE;
    cursor->state.phase += ticks;
    cursor->state.elapsed += ticks;
    if (ticks) {
        cursor->state.inclusive = 1;
    }
    if (*kind & FLUX_EVENT_TURN) {
        cursor->state.turn += 1 + skipped;
        cursor->state.phase = next_phase;
        cursor->state.inclusive = 1;
        if (cursor->track) {
            cursor->state.duration = flux_track_duration(cursor->track, revolution(cursor));

        }
        seek(cursor);
    } else if (*kind & FLUX_EVENT_PULSE) {
        cursor->state.inclusive = 0;
        if (cursor->has_pulse == 2) {
            seek(cursor);
        } else {
            cursor->has_pulse = flux_track_next(cursor->track, &cursor->scan, &cursor->pulse);
        }
    }
    return FLUX_OK;
}

int flux_playback_change_track(flux_playback_t *cursor, unsigned int halftrack)
{
    const flux_track_t *track;
    uint64_t duration;
    if (!cursor || !cursor->capture || halftrack >= FLUX_CAPTURE_MAX_TRACKS) {
        return FLUX_INVALID;
    }
    if (cursor->state.halftrack == halftrack) {
        return FLUX_OK;
    }
    track = flux_capture_track(cursor->capture, halftrack);
    duration = flux_track_duration(track, revolution(cursor));
    if (!duration) {
        duration = cursor->state.duration;
    }
    cursor->state.phase = map_phase(cursor->state.phase, duration, cursor->state.duration);
    cursor->state.duration = duration;
    cursor->state.halftrack = halftrack;
    cursor->state.inclusive = 1;
    cursor->track = track;
    seek(cursor);
    return FLUX_OK;
}

void flux_playback_set_motor(flux_playback_t *cursor, int motor)
{
    if (cursor) {
        cursor->state.motor = motor != 0;
    }
}
