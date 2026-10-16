/**
 * @file autotune_collector.h
 * @brief Duration-based raw-sample collector for auto-tuning.
 *
 * Replaces the fixed sample-count collection (CONFIG_SCHMITT_AUTOTUNE_SAMPLES
 * read in a tight loop) with a duration-based policy:
 *   - collect until BOTH signal populations (mark / no-mark) are visible in
 *     the 64-bin coarse histogram, OR
 *   - until min_time_ms has elapsed (ensures statistical mass even for fast
 *     signals), whichever is later, but
 *   - never longer than max_time_ms (hard cap so slow/low-load signals do
 *     not block startup forever).
 *
 * This fixes the low-load case where a fixed 2 s window barely sees one
 * population and auto-tuning fails: here the window grows until both
 * populations are actually observed.
 *
 * Decimation: the collector stores only every `decimation`-th fed sample.
 * At 20 kHz DMA with decimation=200 the effective collection rate is 100 Hz,
 * so capacity=3000 spans 30 s in ~6 KB/channel. PC tests use decimation=1.
 *
 * Pure C, zero ESP-IDF dependencies. Testable on PC.
 */

#ifndef AUTOTUNE_COLLECTOR_H
#define AUTOTUNE_COLLECTOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Types ──────────────────────────────────────────────────────────────────

/**
 * @brief Duration-based sample collector for two ADC channels.
 *
 * Collects decimated (raw_a, raw_b) pairs until the auto-tune readiness
 * condition is met. Buffers are owned by the collector.
 */
typedef struct {
    int16_t *buf_a;             /**< Channel A raw samples (capacity elements) */
    int16_t *buf_b;             /**< Channel B raw samples (capacity elements) */
    int      capacity;          /**< Max samples stored per channel */
    int      count;             /**< Samples stored so far (per channel) */
    int      decimation;        /**< Store 1 sample per this many fed samples (>=1) */
    int      feed_counter;      /**< Calls since last stored sample */
    uint32_t start_ms;          /**< Timestamp of first fed sample */
    bool     started;           /**< Has the first sample been seen? */
    uint32_t min_time_ms;       /**< Minimum collection time before ready */
    uint32_t max_time_ms;       /**< Hard cap on collection time */
    int      min_separation_bins; /**< Min bin distance between the two populations */
    float    min_bin_fraction;   /**< Second-population count must be >= this fraction of the max bin */
} autotune_collector_t;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

/**
 * @brief Initialize a collector and allocate its buffers.
 *
 * After init, check `c->buf_a != NULL` to confirm allocation succeeded.
 *
 * @param c                  Collector instance
 * @param capacity           Max samples stored per channel (e.g. 3000)
 * @param decimation         Store 1 sample per this many fed samples (>=1; 1 = store all)
 * @param min_time_ms        Minimum collection time before ready (statistical mass)
 * @param max_time_ms        Hard cap on collection time
 * @param min_separation_bins Min bin distance between populations (coarse bins, 64 ADC each)
 * @param min_bin_fraction   Second-population count threshold as fraction of max bin (e.g. 0.05)
 */
void autotune_collector_init(autotune_collector_t *c, int capacity, int decimation,
                              uint32_t min_time_ms, uint32_t max_time_ms,
                              int min_separation_bins, float min_bin_fraction);

/**
 * @brief Free the collector's buffers.
 */
void autotune_collector_destroy(autotune_collector_t *c);

// ─── Collection ─────────────────────────────────────────────────────────────

/**
 * @brief Feed one sample pair (applies decimation internally).
 *
 * @param c      Collector instance
 * @param a      Channel A raw ADC value
 * @param b      Channel B raw ADC value
 * @param now_ms Monotonic timestamp in ms (e.g. esp_timer_get_time()/1000)
 */
void autotune_collector_add(autotune_collector_t *c, int16_t a, int16_t b, uint32_t now_ms);

/**
 * @brief Is the collector ready for auto-tune analysis?
 *
 * Ready when:
 *   - max_time_ms has elapsed (hard cap), OR
 *   - min_time_ms has elapsed AND both channels show both populations.
 *
 * @param c      Collector instance
 * @param now_ms Current monotonic timestamp in ms
 * @return true if schmitt_autotune_analyze() can be run on c->buf_a / c->buf_b.
 */
bool autotune_collector_ready(const autotune_collector_t *c, uint32_t now_ms);

// ─── Population Check ───────────────────────────────────────────────────────

/**
 * @brief Check whether a sample set shows two distinct populations.
 *
 * Builds the 64-bin coarse histogram (same binning as schmitt_autotune),
 * finds the dominant bin, suppresses the region around it, and checks for a
 * second bin that is at least `min_bin_fraction` of the dominant count and at
 * least `min_separation_bins` away. Pure C, no allocations.
 *
 * @param samples            Array of raw ADC values (0-4095)
 * @param n                  Number of samples
 * @param min_separation_bins Min bin distance between the two populations
 * @param min_bin_fraction   Second-population count threshold (fraction of max bin)
 * @return true if two populations are present.
 */
bool autotune_collector_both_populations(const int16_t *samples, int n,
                                          int min_separation_bins, float min_bin_fraction);

#ifdef __cplusplus
}
#endif

#endif // AUTOTUNE_COLLECTOR_H