/**
 * @file schmitt_trigger_logic.h
 * @brief Pure-C Schmitt-Trigger hysteresis logic — hardware-agnostic.
 *
 * This is the stateless-with-respect-to-the-ADC-backend core of the
 * Schmitt-Trigger: it takes a raw sample and applies hysteresis, returning
 * whether an edge occurred. It has zero ESP-IDF dependencies and is
 * testable on PC.
 *
 * The ADC backend (adc_oneshot in schmitt_trigger_adc.c, or adc_continuous
 * in adc_continuous_sampler.c) supplies raw values; this module owns the
 * hysteresis decision so both backends share identical edge behavior.
 *
 * Hysteresis rule (identical to the original schmitt_channel_read):
 *   - rising edge  : state was LOW  and raw >= threshold_high  -> state = HIGH
 *   - falling edge  : state was HIGH and raw <= threshold_low   -> state = LOW
 *   - dead band     : raw between the thresholds                -> no change
 */

#ifndef SCHMITT_TRIGGER_LOGIC_H
#define SCHMITT_TRIGGER_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Types ──────────────────────────────────────────────────────────────────

/**
 * @brief Pure-C Schmitt-Trigger state for one channel.
 *
 * Owns the two thresholds and the current digital state.
 * No ADC handle — callers feed raw samples in.
 */
typedef struct {
    int  threshold_high;   /**< ADC value for rising edge (low → high) */
    int  threshold_low;    /**< ADC value for falling edge (high → low) */
    bool current_state;    /**< Current digital state (true = above threshold_high) */
} schmitt_logic_t;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

/**
 * @brief Initialize a Schmitt-Trigger logic instance.
 *
 * State starts LOW (current_state = false).
 *
 * @param s              Logic instance
 * @param threshold_high Rising edge threshold
 * @param threshold_low  Falling edge threshold
 */
void schmitt_logic_init(schmitt_logic_t *s, int threshold_high, int threshold_low);

// ─── Processing ─────────────────────────────────────────────────────────────

/**
 * @brief Apply hysteresis to one raw sample.
 *
 * Updates current_state and returns whether an edge was detected.
 * Thread-safety: the caller must serialize access to one instance
 * (single sampling task / DMA callback owner).
 *
 * @param s    Logic instance
 * @param raw  Raw ADC value (0-4095 for 12-bit ADC)
 * @return true if the state changed (edge), false otherwise.
 */
bool schmitt_logic_process(schmitt_logic_t *s, int raw);

/**
 * @brief Get the current digital state without processing a sample.
 */
bool schmitt_logic_get_state(const schmitt_logic_t *s);

// ─── Runtime Configuration ───────────────────────────────────────────────────

/**
 * @brief Update the thresholds at runtime (used by auto-tuning).
 *
 * Does not change current_state.
 *
 * @param s              Logic instance
 * @param threshold_high New rising edge threshold
 * @param threshold_low  New falling edge threshold
 */
void schmitt_logic_set_thresholds(schmitt_logic_t *s, int threshold_high, int threshold_low);

#ifdef __cplusplus
}
#endif

#endif // SCHMITT_TRIGGER_LOGIC_H