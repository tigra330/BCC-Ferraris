/**
 * @file schmitt_trigger_autotune.c
 * @brief Auto-tuning implementation — coarse histogram bimodal peak detection.
 *
 * Uses a 64-bin coarse histogram (64 ADC units per bin) to accumulate
 * enough counts per bin for reliable peak detection even with few samples.
 * Pure C, no ESP-IDF dependencies. Testable on PC.
 */

#include "schmitt_trigger_autotune.h"
#include <stdlib.h>
#include <string.h>

// ─── Default Config ─────────────────────────────────────────────────────────

schmitt_autotune_config_t schmitt_autotune_config_default(void)
{
    schmitt_autotune_config_t config = {
        .num_samples          = SCHMITT_AUTOTUNE_DEFAULT_SAMPLES,
        .smoothing_window     = SCHMITT_AUTOTUNE_DEFAULT_SMOOTHING,
        .min_peak_separation  = SCHMITT_AUTOTUNE_DEFAULT_MIN_SEP,
        .high_ratio           = 0.6f,
        .low_ratio            = 0.4f,
    };
    return config;
}

// ─── Internal: Build Coarse Histogram ──────────────────────────────────────

/**
 * @brief Build a coarse histogram from raw ADC samples.
 *
 * Maps 12-bit ADC values (0-4095) into NUM_BINS coarse bins,
 * each spanning BIN_WIDTH (64) ADC units. This accumulates
 * many samples per bin, making peak detection reliable even
 * with just 200-300 samples.
 *
 * @param samples      Array of ADC values (0-4095)
 * @param num_samples  Number of samples
 * @param histogram    Output histogram (must be NUM_BINS elements)
 */
static void build_histogram(const int16_t *samples, int num_samples,
                             int32_t *histogram)
{
    memset(histogram, 0, SCHMITT_AUTOTUNE_NUM_BINS * sizeof(int32_t));

    for (int i = 0; i < num_samples; i++) {
        int val = samples[i];
        if (val >= 0 && val <= SCHMITT_AUTOTUNE_ADC_MAX) {
            int bin = val / SCHMITT_AUTOTUNE_BIN_WIDTH;
            if (bin >= SCHMITT_AUTOTUNE_NUM_BINS)
                bin = SCHMITT_AUTOTUNE_NUM_BINS - 1;
            histogram[bin]++;
        }
    }
}

// ─── Internal: Smooth Histogram ──────────────────────────────────────────────

/**
 * @brief Apply moving-average smoothing to a histogram.
 *
 * @param histogram  Input histogram (NUM_BINS elements)
 * @param smoothed   Output smoothed histogram (NUM_BINS elements)
 * @param window     Smoothing kernel width in bins (must be odd, will be rounded)
 */
static void smooth_histogram(const int32_t *histogram, int32_t *smoothed,
                               int window)
{
    // Ensure window is odd
    if (window < 1) window = 1;
    if (window % 2 == 0) window++;
    int half = window / 2;

    for (int i = 0; i < SCHMITT_AUTOTUNE_NUM_BINS; i++) {
        int32_t sum = 0;
        int count = 0;
        for (int j = i - half; j <= i + half; j++) {
            if (j >= 0 && j < SCHMITT_AUTOTUNE_NUM_BINS) {
                sum += histogram[j];
                count++;
            }
        }
        smoothed[i] = sum / count;
    }
}

// ─── Internal: Find Peaks ───────────────────────────────────────────────────

static int refine_peak(const int32_t *smoothed, int initial_bin);

/**
 * @brief Find the two dominant peaks in a smoothed coarse histogram.
 *
 * Strategy:
 *   1. Find the global maximum (strongest peak)
 *   2. Suppress the region around peak 1 (zero out min_separation/2 bins)
 *   3. Find the global maximum of the suppressed histogram = peak 2
 *   4. Refine both peaks to local maxima
 *   5. Map bin indices back to ADC values
 *   6. Assign low/high based on ADC value
 *
 * @param smoothed         Smoothed coarse histogram (NUM_BINS elements)
 * @param min_separation   Minimum distance between peaks (in coarse bins)
 * @param out_peak_low     Output: ADC value of the low peak
 * @param out_peak_high    Output: ADC value of the high peak
 * @return true if two peaks found, false if unimodal or too close
 */
static bool find_two_peaks(const int32_t *smoothed, int min_separation,
                           int *out_peak_low, int *out_peak_high)
{
    // Step 1: Find global maximum
    int32_t max_count = 0;
    int max_bin = 0;
    for (int i = 0; i < SCHMITT_AUTOTUNE_NUM_BINS; i++) {
        if (smoothed[i] > max_count) {
            max_count = smoothed[i];
            max_bin = i;
        }
    }

    if (max_count == 0) return false;  // Empty histogram

    // Step 2: Suppress the region around peak 1
    int32_t suppressed[SCHMITT_AUTOTUNE_NUM_BINS];
    memcpy(suppressed, smoothed, sizeof(suppressed));

    int suppress_radius = min_separation / 2;
    if (suppress_radius < 1) suppress_radius = 1;
    int suppress_lo = max_bin - suppress_radius;
    int suppress_hi = max_bin + suppress_radius;
    if (suppress_lo < 0) suppress_lo = 0;
    if (suppress_hi >= SCHMITT_AUTOTUNE_NUM_BINS) suppress_hi = SCHMITT_AUTOTUNE_NUM_BINS - 1;

    for (int i = suppress_lo; i <= suppress_hi; i++) {
        suppressed[i] = 0;
    }

    // Step 3: Find the global maximum of the suppressed histogram
    int32_t second_max = 0;
    int second_bin = -1;
    for (int i = 0; i < SCHMITT_AUTOTUNE_NUM_BINS; i++) {
        if (suppressed[i] > second_max) {
            second_max = suppressed[i];
            second_bin = i;
        }
    }

    if (second_bin < 0 || second_max == 0) return false;

    // Step 4: Refine both peaks to local maxima
    int peak1_bin = refine_peak(smoothed, max_bin);
    int peak2_bin = refine_peak(smoothed, second_bin);

    // Verify minimum separation
    int separation = abs(peak1_bin - peak2_bin);
    if (separation < min_separation) return false;

    // Step 5: Map bin indices back to ADC values (center of bin)
    int peak1_adc = peak1_bin * SCHMITT_AUTOTUNE_BIN_WIDTH + SCHMITT_AUTOTUNE_BIN_WIDTH / 2;
    int peak2_adc = peak2_bin * SCHMITT_AUTOTUNE_BIN_WIDTH + SCHMITT_AUTOTUNE_BIN_WIDTH / 2;

    // Step 6: Assign low/high based on ADC value
    if (peak1_adc < peak2_adc) {
        *out_peak_low = peak1_adc;
        *out_peak_high = peak2_adc;
    } else {
        *out_peak_low = peak2_adc;
        *out_peak_high = peak1_adc;
    }

    return true;
}

/**
 * @brief Refine a peak location by finding the local maximum near a bin.
 *
 * Walks left and right from the initial bin to find the true peak
 * in the coarse histogram.
 */
static int refine_peak(const int32_t *smoothed, int initial_bin)
{
    int best_bin = initial_bin;
    int32_t best_count = smoothed[initial_bin];

    // Search ±5 bins for a better local maximum
    int lo = initial_bin - 5;
    int hi = initial_bin + 5;
    if (lo < 0) lo = 0;
    if (hi >= SCHMITT_AUTOTUNE_NUM_BINS) hi = SCHMITT_AUTOTUNE_NUM_BINS - 1;

    for (int i = lo; i <= hi; i++) {
        if (smoothed[i] > best_count) {
            best_count = smoothed[i];
            best_bin = i;
        }
    }

    return best_bin;
}

// ─── Public API ──────────────────────────────────────────────────────────────

schmitt_autotune_result_t schmitt_autotune_analyze(
    const int16_t *samples,
    int num_samples,
    const schmitt_autotune_config_t *config)
{
    schmitt_autotune_result_t result = {0};
    result.valid = false;

    if (!samples || num_samples <= 0) return result;

    // Use defaults if no config provided
    schmitt_autotune_config_t cfg = config ? *config : schmitt_autotune_config_default();

    // Step 1: Build coarse histogram
    int32_t histogram[SCHMITT_AUTOTUNE_NUM_BINS];
    build_histogram(samples, num_samples, histogram);

    // Step 2: Smooth histogram
    int32_t smoothed[SCHMITT_AUTOTUNE_NUM_BINS];
    smooth_histogram(histogram, smoothed, cfg.smoothing_window);

    // Step 3: Find two peaks (in coarse bin space)
    int peak_low_adc, peak_high_adc;
    if (!find_two_peaks(smoothed, cfg.min_peak_separation, &peak_low_adc, &peak_high_adc)) {
        result.valid = false;
        result.peak_low = peak_low_adc;
        result.peak_high = peak_high_adc;
        result.peak_separation = 0;
        result.threshold_high = 0;
        result.threshold_low = 0;
        return result;
    }

    // Step 4: Compute thresholds from peak positions
    int separation = peak_high_adc - peak_low_adc;
    result.peak_low = peak_low_adc;
    result.peak_high = peak_high_adc;
    result.peak_separation = separation;

    // Place thresholds in the valley between peaks
    result.threshold_high = (int)(peak_low_adc + separation * cfg.high_ratio + 0.5f);
    result.threshold_low  = (int)(peak_low_adc + separation * cfg.low_ratio + 0.5f);

    // Clamp to valid ADC range
    if (result.threshold_high > SCHMITT_AUTOTUNE_ADC_MAX)
        result.threshold_high = SCHMITT_AUTOTUNE_ADC_MAX;
    if (result.threshold_low < 0)
        result.threshold_low = 0;

    // Sanity check: HIGH must be > LOW
    if (result.threshold_high <= result.threshold_low) {
        result.valid = false;
        return result;
    }

    result.valid = true;
    return result;
}