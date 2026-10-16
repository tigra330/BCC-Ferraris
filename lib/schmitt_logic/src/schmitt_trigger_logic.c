/**
 * @file schmitt_trigger_logic.c
 * @brief Pure-C Schmitt-Trigger hysteresis logic implementation.
 *
 * No ESP-IDF dependencies. Compiles on PC and on target.
 */

#include "schmitt_trigger_logic.h"

// ─── Lifecycle ──────────────────────────────────────────────────────────────

void schmitt_logic_init(schmitt_logic_t *s, int threshold_high, int threshold_low)
{
    if (!s) return;
    s->threshold_high = threshold_high;
    s->threshold_low  = threshold_low;
    s->current_state  = false;  // Start in LOW state
}

// ─── Processing ─────────────────────────────────────────────────────────────

bool schmitt_logic_process(schmitt_logic_t *s, int raw)
{
    if (!s) return false;

    bool prev = s->current_state;
    bool edge = false;

    // Rising edge: was LOW, now at/above the high threshold
    if (!prev && raw >= s->threshold_high) {
        s->current_state = true;
        edge = true;
    }
    // Falling edge: was HIGH, now at/below the low threshold
    else if (prev && raw <= s->threshold_low) {
        s->current_state = false;
        edge = true;
    }
    // Dead band between thresholds → no change (hysteresis suppresses chatter)

    return edge;
}

bool schmitt_logic_get_state(const schmitt_logic_t *s)
{
    return s ? s->current_state : false;
}

// ─── Runtime Configuration ───────────────────────────────────────────────────

void schmitt_logic_set_thresholds(schmitt_logic_t *s, int threshold_high, int threshold_low)
{
    if (!s) return;
    s->threshold_high = threshold_high;
    s->threshold_low  = threshold_low;
}