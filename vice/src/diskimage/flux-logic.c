/* Shared 1541 read electronics, extracted from rotation.c. GPL-2.0-or-later. */
#include "flux-logic.h"

uint32_t flux_logic_random(flux_logic_t *state)
{
    state->xorShift32 ^= state->xorShift32 << 13;
    state->xorShift32 ^= state->xorShift32 >> 17;
    return state->xorShift32 ^= state->xorShift32 << 5;
}

uint64_t flux_logic_limit(const flux_logic_t *rptr, uint64_t limit)
{
    if (limit <= 1) {
        return 1;
    }
    if (rptr->ue7_counter < 16 && (uint64_t)(16 - rptr->ue7_counter) < limit) {
        limit = 16 - rptr->ue7_counter;
    }
    if (rptr->filter_counter < 40 && (uint64_t)(40 - rptr->filter_counter) < limit) {
        limit = 40 - rptr->filter_counter;
    }
    if (rptr->fr_randcount > 0 && rptr->fr_randcount < limit) {
        limit = rptr->fr_randcount;
    }
    if (rptr->so_delay > 0 && (uint64_t)rptr->so_delay < limit) {
        limit = rptr->so_delay;
    }
    return limit;
}

void flux_logic_clock(flux_logic_t *rptr, flux_logic_output_t *out, uint64_t ToDo, int byte_ready)
{
    out->bit = -1;
    {
        /* so signal handling */
        if (rptr->so_delay) {
            rptr->so_delay -= ToDo;
            if (!rptr->so_delay) {
                out->ready_edge = 1;
                out->ready_level = 1;
            }
        }
    }
    /****************************************************************************************************************************************/
    {
        /* Clock logic */

        /* update UE7 before a possible reset of decoder to start with requested speedzone instead of todo value */
        rptr->ue7_counter += ToDo;
        /* 2.5 microseconds filter */
        rptr->filter_counter += (rptr->filter_counter < 40) ? ToDo : 0;
        if (((rptr->filter_counter >= 40) && (rptr->filter_state != rptr->filter_last_state))) {
            rptr->filter_last_state = rptr->filter_state;
            rptr->uf4_counter = 0;
            rptr->ue7_counter = rptr->ue7_dcba;
            rptr->fr_randcount = ((flux_logic_random(rptr) >> 16) % 31) + 289;
        } else {
            rptr->fr_randcount -= ToDo;
            if (!rptr->fr_randcount) {
                rptr->uf4_counter = 0;
                rptr->ue7_counter = rptr->ue7_dcba;
                rptr->fr_randcount = ((flux_logic_random(rptr) >> 16) % 367) + 33;
            }
        }

        /* Increment the pulse divider clock until the speed zone pulse divider clock threshold value is reached, which is:
        ** 16-(CurrentSpeedZone & 3), and each overflow, increment the pulse counter clock until the 4th pulse is reached
        */

        if (rptr->ue7_counter == 16) {
            rptr->ue7_counter = rptr->ue7_dcba;

            rptr->uf4_counter = (rptr->uf4_counter + 1) & 0xf;
            if ((rptr->uf4_counter & 3) == 2) {
                /****************************************************************************************************************************************/
                {
                    /* Decoder logic */

                    rptr->last_read_data = ((rptr->last_read_data << 1) & 0x3fe) | (((rptr->uf4_counter + 0x1c) >> 4) & 1);
                    out->bit = rptr->last_read_data & 1;

                    rptr->last_write_data <<= 1;

                    /* is sync? reset bit counter, don't move data, etc. */
                    if (rptr->last_read_data == 0x3ff) {
                        rptr->bit_counter = 0;
                    } else {
                        if (++rptr->bit_counter == 8) {
                            rptr->bit_counter = 0;

                            out->byte = (uint8_t) rptr->last_read_data;

                            /* tlr claims that the write register is loaded at every
                             * byte boundary, and since the bus is shared, it's reasonable
                             * to guess that it would be loaded with whatever was last read. */
                            rptr->last_write_data = out->byte;

                            /* BYTE READY signal if enabled */
                            if (byte_ready) {
                                rptr->so_delay = 16 - ((rptr->cycle_index + (ToDo - 1)) & 15);
                                if (rptr->so_delay < 10) {
                                    rptr->so_delay += 16;
                                }
                            }
                        }
                    }
                }
                /****************************************************************************************************************************************/
            }
        }
    }
    /****************************************************************************************************************************************/
}

int flux_logic_advance(flux_logic_t *rptr, flux_logic_output_t *out,
                        flux_playback_t *cursor, unsigned int *reference_phase,
                        uint64_t ticks, int byte_ready, flux_logic_sink_t sink, void *opaque)
{
    flux_event_t event;
    uint64_t span, cycles;
    unsigned int kind;
    int rc;
    if (!rptr || !out || !cursor || !reference_phase || *reference_phase >= 5 || !cursor->capture) {
        return FLUX_INVALID;
    }
    if (!(*cursor).state.motor) {
        return FLUX_OK;
    }
    if (ticks > UINT64_MAX - (*cursor).state.elapsed) {
        return FLUX_RANGE;
    }
    for (;;) {
        rc = flux_playback_next_event(&(*cursor), &event);
        if (rc != FLUX_OK) {
            return rc;
        }
        if (ticks == 0 && event.delta != 0) {
            return FLUX_OK;
        }
        span = event.delta < ticks ? event.delta : ticks;
        /* Avoid overflow for a full-width catch-up. Only a small bounded
           number of clocks can pass before the next circuit event. */
        cycles = span / 5 + (span % 5 + (*reference_phase)) / 5;
        if (cycles) {
            uint64_t limit = cycles > 40 ? 40 : (uint64_t)cycles;
            cycles = flux_logic_limit(rptr, limit);
            span = cycles * 5 - (*reference_phase);
        }
        rc = flux_playback_advance(&(*cursor), span, &kind);
        if (rc != FLUX_OK) {
            return rc;
        }
        if (cycles) {
            flux_logic_clock(rptr, out, cycles, byte_ready);
            rptr->cycle_index += (uint32_t)cycles;
            (*reference_phase) = 0;
        } else {
            (*reference_phase) += (unsigned int)span;
        }
        if (kind & FLUX_EVENT_PULSE) {
            rptr->filter_state ^= 1;
            rptr->filter_counter = 0;
        }
        ticks -= span;
        if (cycles && out->bit >= 0 && sink && sink(opaque, (unsigned int)out->bit)) {
            return FLUX_LOGIC_STOPPED;
        }
    }
}
