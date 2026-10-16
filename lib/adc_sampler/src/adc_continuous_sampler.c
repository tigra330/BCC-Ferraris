/**
 * @file adc_continuous_sampler.c
 * @brief ADC continuous/DMA sampler for 1..8 ADC1 channels (ESP-only).
 *
 * Wraps the ESP-IDF adc_continuous (DMA) driver. A reader task drains DMA
 * frames via adc_continuous_read(), assembles one value per pattern channel
 * into a sample set by a self-healing state machine, and invokes the user
 * callback per set. A decimated ring of recent (A, B) pairs (two chosen set
 * indices) is kept for periodic re-tuning.
 *
 * Target: ESP32 classic, ADC1, 12-bit, ADC_ATTEN_DB_12, DMA continuous.
 * Sets werden reihenfolge-unabhaengig zusammengesetzt (jeder Kanal einmal
 * frisch), weil die ESP32-I2S-DMA die Werte paarweise vertauscht liefert.
 * Minimum sample rate is CONFIG_SOC_ADC_SAMPLE_FREQ_THRES_LOW (20 kHz).
 */

#include "adc_continuous_sampler.h"

#include "esp_adc/adc_continuous.h"
#include "ferraris_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "adc_sampler";

// Frame = CONV_PAIRS_PER_FRAME × SOC_ADC_DIGI_DATA_BYTES_PER_CONV bytes.
// With N channels a frame is generally not set-aligned; the reader keeps
// the assembly state across frames, so that does not matter.
#define CONV_PAIRS_PER_FRAME 64

// Reader task config.
#define READER_STACK_WORDS  4096
#define READER_PRIORITY     5
// Core 1 (APP_CPU): der WLAN-Stack laeuft auf Core 0 mit hoher Prioritaet und
// wuerde den Reader sonst ausbremsen. Prio 5 liegt ueber Arduinos loopTask (1).
#define READER_CORE         1
// DMA-Pool in Frames: puffert ~20 ms bei 50 kHz, falls der Reader kurz
// verdraengt wird (WLAN-Interrupts, Flash-Zugriffe).
#define POOL_FRAMES         16
#define READ_TIMEOUT_MS     100   // short so stop() is responsive

// ─── Sampler State ───────────────────────────────────────────────────────────

struct adc_continuous_sampler {
    // Driver
    adc_continuous_handle_t  handle;
    adc_channel_t            channels[ADC_SAMPLER_MAX_CHANNELS];
    int                      n_channels;
    int8_t                   ch_to_idx[SOC_ADC_MAX_CHANNEL_NUM];  // ADC channel -> set index, -1 = unused
    int                      ring_idx_a;
    int                      ring_idx_b;
    int                      sample_rate_hz;

    // DMA read buffers
    uint8_t                 *read_buf;       // conv_frame_size bytes
    int                      conv_frame_size;
    adc_continuous_data_t  *parsed;         // parsed_capacity entries
    int                      parsed_capacity;

    // Reader task
    TaskHandle_t             reader_task;
    bool                     running;
    adc_sample_set_cb_t      cb;
    void                    *user;
    SemaphoreHandle_t        done_sem;   // reader signals before self-delete

    // Decimated snapshot ring (mutex-protected)
    SemaphoreHandle_t        ring_mutex;
    int16_t                 *ring_a;
    int16_t                 *ring_b;
    int                      ring_capacity;
    int                      ring_head;     // next write slot
    int                      ring_count;    // pairs in ring (<= ring_capacity)
    int                      ring_decimation;
    int                      ring_feed_counter;

    // Statistik (Single-Writer: Reader-Task)
    volatile adc_sampler_stats_t stats;
};

// ─── Reader Task ──────────────────────────────────────────────────────────────

static void reader_task(void *arg)
{
    adc_continuous_sampler_t *s = (adc_continuous_sampler_t *)arg;

    // Set assembly state — kept across frames so a dropped/invalid sample
    // cannot permanently desync the set (a set is only emitted complete).
    const int      n    = s->n_channels;
    const uint32_t full = (1u << n) - 1u;
    uint32_t       seen = 0;
    int            raw[ADC_SAMPLER_MAX_CHANNELS] = {0};

    while (s->running) {
        uint32_t out_len = 0;
        esp_err_t ret = adc_continuous_read(s->handle, s->read_buf,
                                             (uint32_t)s->conv_frame_size,
                                             &out_len, READ_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
            // No data within the timeout — loop and recheck `running`.
            s->stats.reads_timeout++;
            continue;
        }
        if (ret != ESP_OK) {
            s->stats.reads_err++;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (out_len == 0) continue;
        s->stats.reads_ok++;
        s->stats.bytes += out_len;

        // Frame timestamp: shared by all sets in this frame. A frame spans a
        // few ms, well within Schmitt/quad tolerance (they care about sample
        // order, not absolute time).
        uint32_t now_us = (uint32_t)esp_timer_get_time();

        uint32_t num_parsed = 0;
        ret = adc_continuous_parse_data(s->handle, s->read_buf, out_len,
                                         s->parsed, &num_parsed);
        if (ret != ESP_OK) {
            s->stats.reads_err++;
            continue;
        }
        s->stats.parsed += num_parsed;

        for (uint32_t i = 0; i < num_parsed; i++) {
            const adc_continuous_data_t *d = &s->parsed[i];
            s->stats.last_channel = (uint32_t)d->channel;
            if (!d->valid) {          // drop invalid, state machine self-heals
                s->stats.invalid++;
                continue;
            }

            int idx = (unsigned)d->channel < SOC_ADC_MAX_CHANNEL_NUM ? s->ch_to_idx[d->channel] : -1;
            if (idx < 0) {            // unknown channel — ignore
                s->stats.unknown_ch++;
                continue;
            }

            // Reihenfolge-unabhaengig: ein Set ist fertig, sobald jeder Kanal
            // seit dem letzten Set einen frischen Wert hat. Die ESP32-I2S-DMA
            // liefert die 16-bit-Werte paarweise vertauscht (bei ungerader
            // Kanalzahl wandert die Phase) -> auf die Pattern-Reihenfolge ist
            // kein Verlass. Kommt ein Kanal doppelt, gilt der neuere Wert.
            raw[idx] = (int)d->raw_data;
            seen |= 1u << idx;

            if (seen == full) {
                // Push to decimated ring (under mutex, quick).
                s->ring_feed_counter++;
                if (s->ring_decimation <= 1 ||
                    (s->ring_feed_counter % s->ring_decimation) == 0) {
                    if (s->ring_mutex && xSemaphoreTake(s->ring_mutex, 0) == pdTRUE) {
                        s->ring_a[s->ring_head] = (int16_t)raw[s->ring_idx_a];
                        s->ring_b[s->ring_head] = (int16_t)raw[s->ring_idx_b];
                        s->ring_head = (s->ring_head + 1) % s->ring_capacity;
                        if (s->ring_count < s->ring_capacity) s->ring_count++;
                        xSemaphoreGive(s->ring_mutex);
                    }
                }
                s->stats.sets++;
                // Invoke user callback outside the mutex.
                if (s->cb) s->cb(raw, n, now_us, s->user);
                seen = 0;
            }
        }
    }

    // Nur melden und beenden. adc_continuous_stop() ruft stop() im Task des
    // Aufrufers: adc_continuous_start() nimmt intern einen Mutex (adc_lock),
    // den nur derselbe Task wieder freigeben darf (sonst FreeRTOS-Assert in
    // xTaskPriorityDisinherit).
    if (s->done_sem) xSemaphoreGive(s->done_sem);
    vTaskDelete(NULL);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

adc_continuous_sampler_t *adc_continuous_sampler_create(
    const adc_channel_t *channels, int n_channels,
    int ring_idx_a, int ring_idx_b,
    int sample_rate_hz,
    int snapshot_capacity,
    int snapshot_decimation)
{
    if (!channels || n_channels < 1 || n_channels > ADC_SAMPLER_MAX_CHANNELS ||
        ring_idx_a < 0 || ring_idx_a >= n_channels ||
        ring_idx_b < 0 || ring_idx_b >= n_channels) {
        FLOGE(TAG, "invalid channel config (n=%d, ring A=%d B=%d)",
                 n_channels, ring_idx_a, ring_idx_b);
        return NULL;
    }
    if (snapshot_capacity < 1) snapshot_capacity = 1;
    if (snapshot_decimation < 1) snapshot_decimation = 1;
    if (sample_rate_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW) {
        FLOGE(TAG, "sample_rate_hz %d below ESP32 minimum %d",
                 sample_rate_hz, SOC_ADC_SAMPLE_FREQ_THRES_LOW);
        return NULL;
    }

    adc_continuous_sampler_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    memset(s->ch_to_idx, -1, sizeof(s->ch_to_idx));
    for (int i = 0; i < n_channels; i++) {
        if ((unsigned)channels[i] >= SOC_ADC_MAX_CHANNEL_NUM || s->ch_to_idx[channels[i]] >= 0) {
            FLOGE(TAG, "channel %d invalid or duplicate", channels[i]);
            free(s);
            return NULL;
        }
        s->channels[i] = channels[i];
        s->ch_to_idx[channels[i]] = (int8_t)i;
    }
    s->n_channels = n_channels;
    s->ring_idx_a = ring_idx_a;
    s->ring_idx_b = ring_idx_b;
    s->sample_rate_hz = sample_rate_hz;
    s->conv_frame_size = SOC_ADC_DIGI_DATA_BYTES_PER_CONV * CONV_PAIRS_PER_FRAME;
    s->parsed_capacity = s->conv_frame_size / SOC_ADC_DIGI_RESULT_BYTES;
    s->ring_capacity = snapshot_capacity;
    s->ring_decimation = snapshot_decimation;
    s->running = false;

    // Ring buffers + mutex.
    s->ring_a = malloc(snapshot_capacity * sizeof(int16_t));
    s->ring_b = malloc(snapshot_capacity * sizeof(int16_t));
    s->ring_mutex = xSemaphoreCreateMutex();
    s->done_sem = xSemaphoreCreateBinary();
    if (!s->ring_a || !s->ring_b || !s->ring_mutex || !s->done_sem) {
        FLOGE(TAG, "ring/sem allocation failed");
        goto fail;
    }

    // DMA read buffer + parsed array.
    s->read_buf = malloc((size_t)s->conv_frame_size);
    s->parsed = malloc((size_t)s->parsed_capacity * sizeof(adc_continuous_data_t));
    if (!s->read_buf || !s->parsed) {
        FLOGE(TAG, "DMA buffer allocation failed");
        goto fail;
    }

    // ADC continuous handle. conv_frame_size must be a multiple of
    // SOC_ADC_DIGI_DATA_BYTES_PER_CONV (it is, by construction). The pool
    // holds a few frames so the reader task can lag briefly without overflow.
    adc_continuous_handle_cfg_t hdl_cfg = {
        .max_store_buf_size = (uint32_t)s->conv_frame_size * POOL_FRAMES,
        .conv_frame_size    = (uint32_t)s->conv_frame_size,
        .flags.flush_pool   = 1,
    };
    esp_err_t ret = adc_continuous_new_handle(&hdl_cfg, &s->handle);
    if (ret != ESP_OK) {
        FLOGE(TAG, "adc_continuous_new_handle: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Pattern: n ADC1 channels, 12-bit, 11 dB atten, converted in order.
    adc_digi_pattern_config_t pattern[ADC_SAMPLER_MAX_CHANNELS];
    for (int i = 0; i < n_channels; i++) {
        pattern[i].atten     = ADC_ATTEN_DB_12;
        pattern[i].channel   = channels[i];
        pattern[i].unit      = ADC_UNIT_1;
        pattern[i].bit_width = ADC_BITWIDTH_12;
    }

    adc_continuous_config_t cfg = {
        .pattern_num    = (uint32_t)n_channels,
        .adc_pattern    = pattern,
        .sample_freq_hz = (uint32_t)sample_rate_hz,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,  // deprecated, in IDF 6.1 noch gueltig
    };
    ret = adc_continuous_config(s->handle, &cfg);
    if (ret != ESP_OK) {
        FLOGE(TAG, "adc_continuous_config: %s", esp_err_to_name(ret));
        adc_continuous_deinit(s->handle);
        s->handle = NULL;
        goto fail;
    }

    FLOGI(TAG, "created: %d ch @ %d Hz (~%d Hz/ch), frame=%d B, ring=%d (A=idx%d B=idx%d) decim=%d",
             n_channels, sample_rate_hz, sample_rate_hz / n_channels, s->conv_frame_size,
             snapshot_capacity, ring_idx_a, ring_idx_b, snapshot_decimation);
    return s;

fail:
    adc_continuous_sampler_destroy(s);
    return NULL;
}

esp_err_t adc_continuous_sampler_start(adc_continuous_sampler_t *s,
                                        adc_sample_set_cb_t cb, void *user)
{
    if (!s || !s->handle) return ESP_ERR_INVALID_ARG;
    if (s->running) return ESP_ERR_INVALID_STATE;

    // Reset ring state for a clean snapshot window.
    s->ring_head = 0;
    s->ring_count = 0;
    s->ring_feed_counter = 0;
    s->cb = cb;
    s->user = user;
    s->running = true;

    // Drain any stale done token so stop() waits for THIS run's signal.
    if (s->done_sem) {
        while (xSemaphoreTake(s->done_sem, 0) == pdTRUE) { /* drain */ }
    }

    esp_err_t ret = adc_continuous_start(s->handle);
    if (ret != ESP_OK) {
        FLOGE(TAG, "adc_continuous_start: %s", esp_err_to_name(ret));
        s->running = false;
        return ret;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(reader_task, "adc_reader", READER_STACK_WORDS,
                                             s, READER_PRIORITY, &s->reader_task, READER_CORE);
    if (ok != pdPASS) {
        FLOGE(TAG, "reader task create failed");
        adc_continuous_stop(s->handle);
        s->running = false;
        return ESP_FAIL;
    }

    FLOGI(TAG, "started @ %d Hz", s->sample_rate_hz);
    return ESP_OK;
}

esp_err_t adc_continuous_sampler_stop(adc_continuous_sampler_t *s)
{
    if (!s || !s->handle) return ESP_ERR_INVALID_ARG;
    if (!s->running) return ESP_OK;

    s->running = false;
    // Reader task notices `running == false` within READ_TIMEOUT_MS, signals
    // done_sem, and self-deletes. Wait for that signal, then stop conversions
    // here (same task as adc_continuous_start -> adc_lock release is legal).
    // stop() muss deshalb im selben Task wie start() aufgerufen werden.
    if (s->reader_task && s->done_sem) {
        xSemaphoreTake(s->done_sem, pdMS_TO_TICKS(READ_TIMEOUT_MS + 200));
        s->reader_task = NULL;
    }
    adc_continuous_stop(s->handle);
    FLOGI(TAG, "stopped");
    return ESP_OK;
}

void adc_continuous_sampler_destroy(adc_continuous_sampler_t *s)
{
    if (!s) return;
    if (s->running) adc_continuous_sampler_stop(s);
    if (s->handle) {
        adc_continuous_deinit(s->handle);
        s->handle = NULL;
    }
    free(s->read_buf);
    free(s->parsed);
    free(s->ring_a);
    free(s->ring_b);
    if (s->ring_mutex) {
        vSemaphoreDelete(s->ring_mutex);
        s->ring_mutex = NULL;
    }
    if (s->done_sem) {
        vSemaphoreDelete(s->done_sem);
        s->done_sem = NULL;
    }
    free(s);
}

void adc_continuous_sampler_get_stats(const adc_continuous_sampler_t *s,
                                      adc_sampler_stats_t *out)
{
    if (!out) return;
    if (!s) {
        memset(out, 0, sizeof(*out));
        return;
    }
    // Feldweise kopieren (jedes 32-bit-Feld fuer sich atomar).
    out->reads_ok      = s->stats.reads_ok;
    out->reads_timeout = s->stats.reads_timeout;
    out->reads_err     = s->stats.reads_err;
    out->bytes         = s->stats.bytes;
    out->parsed        = s->stats.parsed;
    out->invalid       = s->stats.invalid;
    out->unknown_ch    = s->stats.unknown_ch;
    out->last_channel  = s->stats.last_channel;
    out->sets          = s->stats.sets;
}

// ─── Snapshot ─────────────────────────────────────────────────────────────────

int adc_continuous_sampler_snapshot(adc_continuous_sampler_t *s,
                                     int16_t *out_a, int16_t *out_b, int n)
{
    if (!s || !out_a || !out_b || n <= 0) return 0;
    if (!s->ring_mutex) return 0;

    int copied = 0;
    if (xSemaphoreTake(s->ring_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int count = s->ring_count;
        int take = (n < count) ? n : count;
        if (take > 0) {
            // Oldest-first: if the ring hasn't wrapped, oldest is index 0;
            // if it has wrapped, oldest is at `head` (the next slot to write).
            int start;
            if (count < s->ring_capacity) {
                start = 0;  // not wrapped yet, oldest at 0
            } else {
                start = s->ring_head;  // wrapped, oldest is the next-to-overwrite slot
            }
            for (int i = 0; i < take; i++) {
                int idx = (start + i) % s->ring_capacity;
                out_a[i] = s->ring_a[idx];
                out_b[i] = s->ring_b[idx];
            }
            copied = take;
        }
        xSemaphoreGive(s->ring_mutex);
    }
    return copied;
}