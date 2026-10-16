/**
 * @file autotune_collector.c
 * @brief Duration-based raw-sample collector for auto-tuning.
 *
 * Pure C, no ESP-IDF dependencies. Testable on PC. Reuses the 64-bin coarse
 * histogram binning from schmitt_trigger_autotune (constants in the header).
 */

#include "autotune_collector.h"
#include "schmitt_trigger_autotune.h"   // NUM_BINS / BIN_WIDTH / ADC_MAX constants
#include <stdlib.h>
#include <string.h>

// ─── Lifecycle ──────────────────────────────────────────────────────────────

void autotune_collector_init(autotune_collector_t *c, int capacity, int decimation,
                              uint32_t min_time_ms, uint32_t max_time_ms,
                              int min_separation_bins, float min_bin_fraction)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));

    if (capacity < 1) capacity = 1;
    if (decimation < 1) decimation = 1;
    if (max_time_ms < min_time_ms) max_time_ms = min_time_ms;

    c->capacity            = capacity;
    c->decimation          = decimation;
    c->min_time_ms         = min_time_ms;
    c->max_time_ms         = max_time_ms;
    c->min_separation_bins = min_separation_bins;
    c->min_bin_fraction    = min_bin_fraction;
    c->count               = 0;
    c->feed_counter        = 0;
    c->started             = false;

    c->buf_a = (int16_t *)malloc(capacity * sizeof(int16_t));
    c->buf_b = (int16_t *)malloc(capacity * sizeof(int16_t));
    if (!c->buf_a || !c->buf_b) {
        // Allocation failed — caller must check buf_a != NULL.
        free(c->buf_a); c->buf_a = NULL;
        free(c->buf_b); c->buf_b = NULL;
        c->capacity = 0;
    }
}

void autotune_collector_destroy(autotune_collector_t *c)
{
    if (!c) return;
    free(c->buf_a);
    free(c->buf_b);
    c->buf_a = NULL;
    c->buf_b = NULL;
    c->capacity = 0;
    c->count = 0;
}

// ─── Collection ─────────────────────────────────────────────────────────────

void autotune_collector_add(autotune_collector_t *c, int16_t a, int16_t b, uint32_t now_ms)
{
    if (!c || !c->buf_a || !c->buf_b) return;

    if (!c->started) {
        c->start_ms = now_ms;
        c->started = true;
    }

    // Decimation: store only every `decimation`-th fed sample.
    c->feed_counter++;
    if (c->decimation > 1 && (c->feed_counter % c->decimation) != 0) {
        return;
    }

    if (c->count < c->capacity) {
        c->buf_a[c->count] = a;
        c->buf_b[c->count] = b;
        c->count++;
    }
    // If full, drop the sample — caller sizes capacity + decimation so the
    // buffer spans max_time_ms without filling first.
}

bool autotune_collector_ready(const autotune_collector_t *c, uint32_t now_ms)
{
    if (!c || !c->started || c->count == 0) return false;

    uint32_t elapsed = now_ms - c->start_ms;

    // Hard cap: never block longer than max_time_ms.
    if (elapsed >= c->max_time_ms) return true;

    // Need at least min_time_ms of statistical mass before declaring ready.
    if (elapsed < c->min_time_ms) return false;

    // Between min and max: ready once BOTH channels show both populations.
    return autotune_collector_both_populations(c->buf_a, c->count,
                                                c->min_separation_bins, c->min_bin_fraction)
        && autotune_collector_both_populations(c->buf_b, c->count,
                                                c->min_separation_bins, c->min_bin_fraction);
}

// ─── Population Check ───────────────────────────────────────────────────────

bool autotune_collector_both_populations(const int16_t *samples, int n,
                                          int min_separation_bins, float min_bin_fraction)
{
    if (!samples || n <= 0) return false;

    // Build the 64-bin coarse histogram (same binning as schmitt_autotune).
    int32_t hist[SCHMITT_AUTOTUNE_NUM_BINS];
    memset(hist, 0, sizeof(hist));

    int used = 0;
    for (int i = 0; i < n; i++) {
        int v = samples[i];
        if (v >= 0 && v <= SCHMITT_AUTOTUNE_ADC_MAX) {
            int bin = v / SCHMITT_AUTOTUNE_BIN_WIDTH;
            if (bin >= SCHMITT_AUTOTUNE_NUM_BINS) bin = SCHMITT_AUTOTUNE_NUM_BINS - 1;
            hist[bin]++;
            used++;
        }
    }
    if (used == 0) return false;

    // Find the dominant bin.
    int32_t max_count = 0;
    int max_bin = 0;
    for (int i = 0; i < SCHMITT_AUTOTUNE_NUM_BINS; i++) {
        if (hist[i] > max_count) {
            max_count = hist[i];
            max_bin = i;
        }
    }
    if (max_count == 0) return false;

    // Suppress the region around the dominant bin and find the second max.
    int radius = min_separation_bins / 2;
    if (radius < 1) radius = 1;
    int suppress_lo = max_bin - radius;
    int suppress_hi = max_bin + radius;
    if (suppress_lo < 0) suppress_lo = 0;
    if (suppress_hi >= SCHMITT_AUTOTUNE_NUM_BINS) suppress_hi = SCHMITT_AUTOTUNE_NUM_BINS - 1;

    int32_t second_max = 0;
    int second_bin = -1;
    for (int i = 0; i < SCHMITT_AUTOTUNE_NUM_BINS; i++) {
        if (i >= suppress_lo && i <= suppress_hi) continue;
        if (hist[i] > second_max) {
            second_max = hist[i];
            second_bin = i;
        }
    }
    if (second_bin < 0) return false;

    // Second population must be large enough and far enough away.
    int32_t threshold = (int32_t)(min_bin_fraction * (float)max_count);
    if (second_max < threshold) return false;
    int sep = second_bin - max_bin;
    if (sep < 0) sep = -sep;
    if (sep < min_separation_bins) return false;

    return true;
}