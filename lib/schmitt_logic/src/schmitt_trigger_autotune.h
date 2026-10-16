/**
 * @file schmitt_trigger_autotune.h
 * @brief Auto-tuning for Schmitt-Trigger thresholds.
 *
 * Analyzes raw ADC samples to find optimal HIGH/LOW thresholds
 * based on the bimodal distribution of Ferraris counter sensor signals.
 *
 * Algorithm:
 *   1. Downsample 12-bit ADC values into a coarse histogram (default 64 bins,
 *      each spanning 64 ADC units). This accumulates enough counts per bin
 *      to produce clear peaks even with few samples.
 *   2. Smooth with a moving-average kernel.
 *   3. Find two dominant peaks using suppress-first-peak method.
 *   4. Map bin indices back to ADC values and place thresholds
 *      at configurable ratios in the valley between peaks.
 *
 * This module is pure C with zero ESP-IDF dependencies —
 * it can be tested on PC and used in the simulator.
 */

#ifndef SCHMITT_TRIGGER_AUTOTUNE_H
#define SCHMITT_TRIGGER_AUTOTUNE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Configuration ──────────────────────────────────────────────────────────

#define SCHMITT_AUTOTUNE_ADC_BITS       12     /**< 12-bit ADC (0-4095) */
#define SCHMITT_AUTOTUNE_ADC_MAX        4095   /**< Maximum ADC value */
#define SCHMITT_AUTOTUNE_NUM_BINS       64     /**< Coarse histogram bins (4096/64) */
#define SCHMITT_AUTOTUNE_BIN_WIDTH      64     /**< ADC units per bin (4096/64) */
#define SCHMITT_AUTOTUNE_DEFAULT_SAMPLES 1000  /**< Default number of samples to analyze */
#define SCHMITT_AUTOTUNE_DEFAULT_SMOOTHING 3   /**< Default histogram smoothing window (bins) */
#define SCHMITT_AUTOTUNE_DEFAULT_MIN_SEP 6     /**< Default minimum peak separation (bins, ~384 ADC) */
#define SCHMITT_AUTOTUNE_DEFAULT_MIN_SEP_ADC (SCHMITT_AUTOTUNE_DEFAULT_MIN_SEP * SCHMITT_AUTOTUNE_BIN_WIDTH)

/**
 * @brief Configuration for auto-tuning algorithm.
 *
 * min_peak_separation is in COARSE BIN units (not ADC units).
 * A separation of 6 bins = 6 × 64 = 384 ADC units.
 */
typedef struct {
    int num_samples;           /**< Number of samples to collect (default 1000) */
    int smoothing_window;     /**< Histogram smoothing kernel width in bins (default 3) */
    int min_peak_separation;  /**< Minimum bin distance between peaks (default 6 bins ≈ 384 ADC) */
    float high_ratio;          /**< Threshold position in valley (default 0.6 = 60% toward high peak) */
    float low_ratio;           /**< Threshold position in valley (default 0.4 = 40% toward high peak) */
} schmitt_autotune_config_t;

/**
 * @brief Result of auto-tuning analysis.
 */
typedef struct {
    int threshold_high;      /**< Computed high threshold (ADC units, 0-4095) */
    int threshold_low;       /**< Computed low threshold (ADC units, 0-4095) */
    int peak_low;            /**< ADC value of the low-signal peak */
    int peak_high;           /**< ADC value of the high-signal peak */
    int peak_separation;     /**< Distance between peaks (ADC units) */
    bool valid;               /**< True if auto-tuning succeeded (two peaks found) */
} schmitt_autotune_result_t;

// ─── Functions ──────────────────────────────────────────────────────────────

/**
 * @brief Get default auto-tune configuration.
 */
schmitt_autotune_config_t schmitt_autotune_config_default(void);

/**
 * @brief Analyze raw ADC samples to find optimal Schmitt-Trigger thresholds.
 *
 * Uses coarse-histogram bimodal peak detection:
 *   1. Downsample ADC values into 64 coarse bins (64 ADC units each)
 *   2. Smooth with moving-average kernel
 *   3. Find two dominant peaks (suppress-first-peak method)
 *   4. Place thresholds at configurable ratios in the valley
 *
 * This function is pure C with no hardware dependencies.
 * It can be called with samples collected from any source (ADC, CSV file, etc.).
 *
 * @param samples      Array of raw ADC values (0-4095)
 * @param num_samples  Number of samples in the array
 * @param config       Auto-tune configuration (NULL for defaults)
 * @return Analysis result with computed thresholds.
 */
schmitt_autotune_result_t schmitt_autotune_analyze(
    const int16_t *samples,
    int num_samples,
    const schmitt_autotune_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // SCHMITT_TRIGGER_AUTOTUNE_H