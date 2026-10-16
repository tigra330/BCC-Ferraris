/**
 * @file quadrature_decoder.h
 * @brief Quadrature decoder — Gray-code state machine for rotation direction.
 *
 * Zero-dependency, portable C component. Uses a handle-based API so multiple
 * decoder instances can coexist independently.
 *
 * Gray-code states:
 *   S0: A=low,  B=low
 *   S1: A=high, B=low     — A leads B = forward
 *   S2: A=high, B=high
 *   S3: A=low,  B=high
 *
 * Forward rotation:  S0→S1→S2→S3→S0  (each transition +1)
 * Reverse rotation:  S0→S3→S2→S1→S0  (each transition -1)
 *
 * This gives 4x resolution: every state transition counts as one step.
 * Invalid jumps (±2, both channels changing simultaneously) are ignored.
 */

#ifndef QUADRATURE_DECODER_H
#define QUADRATURE_DECODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Types ──────────────────────────────────────────────────────────────────

/**
 * Gray-code states for two sensor channels.
 * State transitions determine direction unambiguously.
 */
typedef enum {
    QUAD_S0 = 0,   /**< A=low,  B=low */
    QUAD_S1 = 1,   /**< A=high, B=low */
    QUAD_S2 = 2,   /**< A=high, B=high */
    QUAD_S3 = 3,   /**< A=low,  B=high */
} quad_state_t;

/**
 * Direction result from a state transition.
 */
typedef enum {
    QUAD_DIR_NONE     =  0,   /**< No change or invalid jump */
    QUAD_DIR_FORWARD  =  1,   /**< Forward rotation (A leads B) */
    QUAD_DIR_BACKWARD = -1,   /**< Reverse rotation (B leads A) */
} quad_direction_t;

/**
 * Opaque decoder handle. Create with quad_decoder_create().
 */
typedef struct quad_decoder quad_decoder_t;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

/**
 * @brief Create a new quadrature decoder instance.
 *
 * Initial state is QUAD_S0 and counters are zero.
 *
 * @return Handle on success, NULL on allocation failure.
 */
quad_decoder_t *quad_decoder_create(void);

/**
 * @brief Destroy a decoder instance and free its memory.
 */
void quad_decoder_destroy(quad_decoder_t *dec);

/**
 * @brief Reset counters and state to initial values.
 */
void quad_decoder_reset(quad_decoder_t *dec);

// ─── Processing ─────────────────────────────────────────────────────────────

/**
 * @brief Process a new pair of sensor readings.
 *
 * Applies the Gray-code state machine and updates counters based on
 * the direction of the state transition.
 *
 * @param dec     Decoder handle
 * @param a_high  Current digital state of channel A
 * @param b_high  Current digital state of channel B
 * @return Direction of the transition, or QUAD_DIR_NONE if no change.
 */
quad_direction_t quad_decoder_process(quad_decoder_t *dec, bool a_high, bool b_high);

// ─── Readout ────────────────────────────────────────────────────────────────

/**
 * @brief Read the current forward and backward counters.
 */
void quad_decoder_get_counts(const quad_decoder_t *dec, int32_t *forward, int32_t *backward);

/**
 * @brief Get the current Gray-code state (for debugging).
 */
quad_state_t quad_decoder_get_state(const quad_decoder_t *dec);

/**
 * @brief Get the name of a state as a string (for debugging).
 *
 * Returns: "S0(A=0,B=0)", "S1(A=1,B=0)", "S2(A=1,B=1)", "S3(A=0,B=1)"
 */
const char *quad_state_name(quad_state_t state);

/**
 * @brief Compute the Gray-code state from two boolean channel values.
 *
 * This is a pure function (no side effects) useful for testing.
 */
quad_state_t quad_compute_state(bool a_high, bool b_high);

/**
 * @brief Compute the direction from a state transition.
 *
 * @return QUAD_DIR_FORWARD, QUAD_DIR_BACKWARD, or QUAD_DIR_NONE (invalid jump).
 */
quad_direction_t quad_compute_delta(quad_state_t prev, quad_state_t curr);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_DECODER_H