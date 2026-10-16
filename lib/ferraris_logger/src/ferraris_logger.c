/**
 * @file ferraris_logger.c
 * @brief ESP-Huelle des RML-CSV-Loggers (log_task, printf auf UART0).
 */

#include "ferraris_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

struct ferraris_logger {
    logger_ch_t            la;              // Kanal A Tracker (shared logic)
    logger_ch_t            rb;              // Kanal B Tracker
    logger_quad_t          quad;            // Quadratur auf dem Mark-PAAR (W-Spalte)
    logger_ch_t            q[FERRARIS_LOGGER_MAX_SENSORS];  // Tracker je Q-Sensor
    int                    n_q;             // Anzahl Q-Sensoren
    const schmitt_logic_t *la_logic;        // live Schwellen A
    const schmitt_logic_t *rb_logic;        // live Schwellen B
    uint32_t               interval_ms;
    int                    k_rev_per_kwh;
    uint32_t               start_ms;        // Bezug fuer die W-Spalte ("seit Start")
    volatile uint32_t      pause_until_ms;  // CSV-Ausgabe bis dahin angehalten
    bool                   auto_envelope;   // false -> feste min/max ausgeben
    int                    fixed_min;
    int                    fixed_max;
    volatile bool          running;
    TaskHandle_t           task;
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ─── Zeile formatieren ───────────────────────────────────────────────────────

static int env_min(const ferraris_logger_t *l, const logger_ch_t *c)
{
    return l->auto_envelope ? (int)c->run_min : l->fixed_min;
}

static int env_max(const ferraris_logger_t *l, const logger_ch_t *c)
{
    return l->auto_envelope ? (int)c->run_max : l->fixed_max;
}

int ferraris_logger_format_row(ferraris_logger_t *l, char *buf, size_t len)
{
    if (!l || !buf || len == 0) return -1;

    uint32_t millis = now_ms();
    uint32_t elapsed_ms = millis - l->start_ms;

    const logger_ch_t *a = &l->la;
    const logger_ch_t *b = &l->rb;
    int la_tOn  = l->la_logic ? l->la_logic->threshold_high : 0;
    int la_tOff = l->la_logic ? l->la_logic->threshold_low  : 0;
    int ra_tOn  = l->rb_logic ? l->rb_logic->threshold_high : 0;
    int ra_tOff = l->rb_logic ? l->rb_logic->threshold_low  : 0;

    // 18. Spalte: mittlere Leistung seit Start aus der Logger-Quadratur
    // (logger_watts, identisch zum PC-Simulator).
    int watts = logger_watts(l->quad.net_steps, l->k_rev_per_kwh, elapsed_ms);

    int n = snprintf(buf, len,
                     "%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                     (unsigned long)millis,
                     a->last_raw, (int)a->ema, env_min(l, a), env_max(l, a),
                     la_tOn, la_tOff, !a->prev_mark, (int)a->mark_count,
                     b->last_raw, (int)b->ema, env_min(l, b), env_max(l, b),
                     ra_tOn, ra_tOff, !b->prev_mark, (int)b->mark_count,
                     watts);
    // Spalten 19+: je Q-Sensor raw,min,max (Huelle = Marken-/Hintergrund-Pegel).
    for (int i = 0; i < l->n_q && n > 0 && (size_t)n < len; i++) {
        n += snprintf(buf + n, len - (size_t)n, ",%d,%d,%d",
                      l->q[i].last_raw, env_min(l, &l->q[i]), env_max(l, &l->q[i]));
    }
    return n;
}

bool ferraris_logger_get_sensor(const ferraris_logger_t *l, int i,
                                int *raw, int *run_min, int *run_max)
{
    if (!l || i < 0 || i >= l->n_q) return false;
    if (raw)     *raw     = l->q[i].last_raw;
    if (run_min) *run_min = env_min(l, &l->q[i]);
    if (run_max) *run_max = env_max(l, &l->q[i]);
    return true;
}

// ─── log_task ────────────────────────────────────────────────────────────────

static void log_task(void *arg)
{
    ferraris_logger_t *l = (ferraris_logger_t *)arg;
    const TickType_t delay = pdMS_TO_TICKS(l->interval_ms);
    char line[FERRARIS_LOGGER_ROW_MAX];

    while (l->running) {
        bool paused = (int32_t)(l->pause_until_ms - now_ms()) > 0;
        if (!paused && ferraris_logger_format_row(l, line, sizeof(line)) > 0) {
            // Ganze Zeile in einem printf -> kein Zerreissen durch andere Ausgaben.
            printf("%s\n", line);
        }
        vTaskDelay(delay);
    }

    l->task = NULL;
    vTaskDelete(NULL);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

ferraris_logger_t *ferraris_logger_create(uint32_t interval_ms)
{
    ferraris_logger_t *l = calloc(1, sizeof(*l));
    if (!l) return NULL;

    l->interval_ms   = interval_ms ? interval_ms : 250;
    l->k_rev_per_kwh = 75;   // Default; main setzt FERRARIS_REVOLUTIONS_PER_KWH
    l->start_ms      = now_ms();
    l->auto_envelope = true;
    l->fixed_min     = 0;
    l->fixed_max     = 4095;

    // Tracker auf ADC-Mitte; die Huelle konvergiert mit den ersten Samples.
    logger_ch_init(&l->la, 2048);
    logger_ch_init(&l->rb, 2048);
    logger_quad_init(&l->quad);
    for (int i = 0; i < FERRARIS_LOGGER_MAX_SENSORS; i++) {
        logger_ch_init(&l->q[i], 2048);
    }
    return l;
}

void ferraris_logger_set_thresholds(ferraris_logger_t *l,
                                    const schmitt_logic_t *la,
                                    const schmitt_logic_t *rb)
{
    if (!l) return;
    l->la_logic = la;
    l->rb_logic = rb;
}

void ferraris_logger_set_meter_constant(ferraris_logger_t *l, int k_rev_per_kwh)
{
    if (l && k_rev_per_kwh > 0) l->k_rev_per_kwh = k_rev_per_kwh;
}

void ferraris_logger_set_envelope(ferraris_logger_t *l, bool auto_envelope,
                                  int fixed_min, int fixed_max)
{
    if (!l) return;
    l->auto_envelope = auto_envelope;
    l->fixed_min     = fixed_min;
    l->fixed_max     = fixed_max;
}

void ferraris_logger_set_sensor_count(ferraris_logger_t *l, int n)
{
    if (!l) return;
    if (n < 0) n = 0;
    if (n > FERRARIS_LOGGER_MAX_SENSORS) n = FERRARIS_LOGGER_MAX_SENSORS;
    l->n_q = n;
}

void ferraris_logger_feed(ferraris_logger_t *l, int raw_a, int raw_b,
                          bool mark_a, bool mark_b)
{
    if (!l) return;
    logger_ch_feed(&l->la, raw_a, mark_a);
    logger_ch_feed(&l->rb, raw_b, mark_b);
    // Eigene Quadratur fuer die W-Spalte: gleicher Gray-Code wie
    // quadrature_decoder, damit der Logger ohne ferraris_data auskommt.
    logger_quad_feed(&l->quad, mark_a, mark_b);
}

void ferraris_logger_feed_sensors(ferraris_logger_t *l, const int *raw, int n)
{
    if (!l || !raw) return;
    if (n > l->n_q) n = l->n_q;
    for (int i = 0; i < n; i++) {
        // Nur Rohwert/Huelle interessieren; Markenzaehlung je Q wird nicht genutzt.
        logger_ch_feed(&l->q[i], raw[i], false);
    }
}

void ferraris_logger_pause(ferraris_logger_t *l, uint32_t ms)
{
    if (l) l->pause_until_ms = now_ms() + ms;
}

esp_err_t ferraris_logger_start(ferraris_logger_t *l)
{
    if (!l) return ESP_ERR_INVALID_ARG;
    if (l->running) return ESP_OK;

    l->running = true;
    BaseType_t ok = xTaskCreate(log_task, "ferraris_log", 4096, l,
                                tskIDLE_PRIORITY + 1, &l->task);
    if (ok != pdPASS) {
        l->running = false;
        l->task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ferraris_logger_destroy(ferraris_logger_t *l)
{
    if (!l) return;
    // Task sanft beenden: er sieht running=false nach spaetestens einem
    // Intervall und loescht sich selbst (kein Delete mitten im printf).
    l->running = false;
    if (l->task) {
        vTaskDelay(pdMS_TO_TICKS(l->interval_ms) + pdMS_TO_TICKS(10));
    }
    free(l);
}
