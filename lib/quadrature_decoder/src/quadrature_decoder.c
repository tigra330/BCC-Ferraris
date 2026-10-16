/**
 * @file quadrature_decoder.c
 * @brief Quadrature decoder implementation — Gray-code state machine.
 */

#include "quadrature_decoder.h"
#include <stdlib.h>
#include <string.h>

// ─── Decoder State ──────────────────────────────────────────────────────────

struct quad_decoder {
    quad_state_t state;       /**< Current Gray-code state */
    int32_t      forward;     /**< Forward step counter */
    int32_t      backward;    /**< Backward step counter */
};

// ─── Lifecycle ──────────────────────────────────────────────────────────────

quad_decoder_t *quad_decoder_create(void)
{
    quad_decoder_t *dec = calloc(1, sizeof(quad_decoder_t));
    if (dec) {
        dec->state = QUAD_S0;
        dec->forward = 0;
        dec->backward = 0;
    }
    return dec;
}

void quad_decoder_destroy(quad_decoder_t *dec)
{
    free(dec);
}

void quad_decoder_reset(quad_decoder_t *dec)
{
    if (!dec) return;
    dec->state = QUAD_S0;
    dec->forward = 0;
    dec->backward = 0;
}

// ─── Processing ─────────────────────────────────────────────────────────────

quad_direction_t quad_decoder_process(quad_decoder_t *dec, bool a_high, bool b_high)
{
    if (!dec) return QUAD_DIR_NONE;

    quad_state_t new_state = quad_compute_state(a_high, b_high);

    if (new_state == dec->state) {
        return QUAD_DIR_NONE;   // No change
    }

    quad_direction_t dir = quad_compute_delta(dec->state, new_state);

    if (dir == QUAD_DIR_FORWARD) {
        dec->forward++;
    } else if (dir == QUAD_DIR_BACKWARD) {
        dec->backward++;
    }
    // QUAD_DIR_NONE: invalid jump — counters stay unchanged

    dec->state = new_state;
    return dir;
}

// ─── Readout ────────────────────────────────────────────────────────────────

void quad_decoder_get_counts(const quad_decoder_t *dec, int32_t *forward, int32_t *backward)
{
    if (!dec) {
        if (forward)  *forward = 0;
        if (backward) *backward = 0;
        return;
    }
    if (forward)  *forward = dec->forward;
    if (backward) *backward = dec->backward;
}

quad_state_t quad_decoder_get_state(const quad_decoder_t *dec)
{
    return dec ? dec->state : QUAD_S0;
}

// ─── Pure Functions ──────────────────────────────────────────────────────────

quad_state_t quad_compute_state(bool a_high, bool b_high)
{
    if (!a_high && !b_high) return QUAD_S0;
    if (a_high  && !b_high) return QUAD_S1;
    if (a_high  && b_high)  return QUAD_S2;
    /* !a_high && b_high */ return QUAD_S3;
}

quad_direction_t quad_compute_delta(quad_state_t prev, quad_state_t curr)
{
    int8_t diff = (int8_t)curr - (int8_t)prev;

    if (diff ==  1 || diff == -3) return QUAD_DIR_FORWARD;
    if (diff == -1 || diff ==  3) return QUAD_DIR_BACKWARD;

    // diff == 0: no change (shouldn't reach here)
    // diff == 2 or -2: invalid jump (both channels changed simultaneously)
    return QUAD_DIR_NONE;
}

const char *quad_state_name(quad_state_t state)
{
    switch (state) {
        case QUAD_S0: return "S0(A=0,B=0)";
        case QUAD_S1: return "S1(A=1,B=0)";
        case QUAD_S2: return "S2(A=1,B=1)";
        case QUAD_S3: return "S3(A=0,B=1)";
    }
    return "?";
}