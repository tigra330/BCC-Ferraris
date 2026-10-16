/**
 * @file adc_continuous_sampler.h
 * @brief ADC continuous/DMA sampler for 1..8 ADC1 channels.
 *
 * Wraps the ESP-IDF adc_continuous (DMA) driver into a set-oriented API:
 * the reader task drains DMA frames, assembles one value per pattern
 * channel into a sample set and calls the user callback with
 * (raw[0..n-1], n, now_us).
 *
 * It also keeps a decimated ring of recent (A, B) pairs for periodic
 * re-tuning (see adc_continuous_sampler_snapshot); A/B are two of the set
 * indices, chosen at create time (the counting pair). ESP-only.
 *
 * Target: ESP32 classic, ADC1, 12-bit, 11 dB attenuation, DMA continuous
 * mode. All channels share one pattern table. A set is emitted as soon as
 * every channel has delivered a fresh value since the previous set — order
 * independent, because the ESP32 I2S DMA returns the 16-bit samples swapped
 * in pairs (with an odd channel count the pattern order is never seen).
 */

#ifndef ADC_CONTINUOUS_SAMPLER_H
#define ADC_CONTINUOUS_SAMPLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_SAMPLER_MAX_CHANNELS 8

// ─── Types ──────────────────────────────────────────────────────────────────

typedef struct adc_continuous_sampler adc_continuous_sampler_t;

/**
 * @brief Called for every complete sample set (one value per channel).
 *
 * raw[i] belongs to the i-th channel passed to create().  Runs in the
 * sampler's reader task context — keep it short (no blocking, no logging).
 * now_us is esp_timer_get_time() captured at frame read time.
 */
typedef void (*adc_sample_set_cb_t)(const int *raw, int n, uint32_t now_us, void *user);

// ─── Lifecycle ──────────────────────────────────────────────────────────────

/**
 * @brief Create a sampler (does not start sampling yet).
 *
 * Configures the ADC1 continuous/DMA driver for the channels at
 * sample_rate_hz (ESP32 minimum is 20 kHz — CONFIG_SOC_ADC_SAMPLE_FREQ_THRES_LOW).
 * The conversion rate is shared by all channels (per channel ~ rate / n).
 *
 * @param channels            ADC1 channels in pattern order (distinct)
 * @param n_channels          Number of channels (1..ADC_SAMPLER_MAX_CHANNELS)
 * @param ring_idx_a          Set index stored as "A" in the snapshot ring
 * @param ring_idx_b          Set index stored as "B" in the snapshot ring
 * @param sample_rate_hz      ADC conversion frequency (>= 20000)
 * @param snapshot_capacity    Ring size in pairs for re-tune snapshots (e.g. 3000)
 * @param snapshot_decimation Store every Nth pair into the ring (>=1; spreads
 *                            the ring across the time window at high rates)
 * @return Handle on success, NULL on failure.
 */
adc_continuous_sampler_t *adc_continuous_sampler_create(
    const adc_channel_t *channels, int n_channels,
    int ring_idx_a, int ring_idx_b,
    int sample_rate_hz,
    int snapshot_capacity,
    int snapshot_decimation);

/**
 * @brief Start the reader task and ADC conversions.
 *
 * @param s     Sampler handle
 * @param cb    Called per complete sample set
 * @param user  Opaque pointer passed to cb
 * @return ESP_OK on success.
 */
esp_err_t adc_continuous_sampler_start(adc_continuous_sampler_t *s,
                                        adc_sample_set_cb_t cb, void *user);

/**
 * @brief Stop the reader task and ADC conversions.
 *
 * Muss im selben Task wie adc_continuous_sampler_start() aufgerufen werden
 * (IDF-adc_lock ist ein Mutex mit Besitzer).
 */
esp_err_t adc_continuous_sampler_stop(adc_continuous_sampler_t *s);

/**
 * @brief Destroy the sampler and free all resources.
 */
void adc_continuous_sampler_destroy(adc_continuous_sampler_t *s);

/**
 * @brief Laufende Zaehler (32-bit, vom Reader-Task geschrieben, lockfrei lesbar).
 *
 * Zur Diagnose, an welcher Stelle Daten verloren gehen:
 * read -> bytes -> parsed -> (invalid | unknown_ch) -> sets.
 */
typedef struct {
    uint32_t reads_ok;       /**< adc_continuous_read mit Daten */
    uint32_t reads_timeout;  /**< adc_continuous_read ohne Daten (Timeout) */
    uint32_t reads_err;      /**< adc_continuous_read mit Fehler */
    uint32_t bytes;          /**< gelesene Bytes */
    uint32_t parsed;         /**< ausgewertete Einzelwerte */
    uint32_t invalid;        /**< davon als ungueltig markiert */
    uint32_t unknown_ch;     /**< davon Kanal nicht im Pattern */
    uint32_t last_channel;   /**< Kanal des zuletzt ausgewerteten Werts */
    uint32_t sets;           /**< vollstaendige Sample-Sets (Callback-Aufrufe) */
} adc_sampler_stats_t;

void adc_continuous_sampler_get_stats(const adc_continuous_sampler_t *s,
                                      adc_sampler_stats_t *out);

// ─── Re-Tune Snapshot ────────────────────────────────────────────────────────

/**
 * @brief Copy up to n recent sample pairs out of the ring (oldest-first).
 *
 * Thread-safe against the reader task. The ring holds the most recent
 * `snapshot_capacity` pairs (decimated). Used by the periodic re-tune task to
 * recompute Schmitt thresholds via schmitt_autotune_analyze.
 *
 * @param s      Sampler handle
 * @param out_a  Output buffer for channel A (n elements)
 * @param out_b  Output buffer for channel B (n elements)
 * @param n      Max pairs to copy
 * @return Number of pairs actually copied (0 if ring empty or not started).
 */
int adc_continuous_sampler_snapshot(adc_continuous_sampler_t *s,
                                     int16_t *out_a, int16_t *out_b, int n);

#ifdef __cplusplus
}
#endif

#endif // ADC_CONTINUOUS_SAMPLER_H