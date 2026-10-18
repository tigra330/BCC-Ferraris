/**
 * @file main.cpp
 * @brief RML 2.0 BAScloud - Ferraris-Zaehler-Ausleser (Arduino-ESP32)
 *
 * Liest einen Ferraris-Zaehler optisch ueber Reflex-Lichtschranken aus:
 *
 *   ADC1 continuous/DMA (50 kHz, ~10 kHz je Sensor)
 *     └─ dma_sample_callback(raw[0..4])            [adc_reader-Task, Core 1]
 *          ├─ Schmitt A/B -> Flanke -> Quadratur -> ferraris_data (+/- Schritt)
 *          ├─ Auto-Tune-Collector (nur waehrend der Start-Kalibrierung)
 *          └─ Logger-Tracker (EMA/Huelle je Sensor, jedes Sample)
 *   ferraris_data ──► print_task (Klartext) | log_task (CSV) | HTTP | MQTT
 *
 * Konfiguration: include/ferraris_config.h (Defaults), Zugangsdaten: include/secrets.h.
 * Ueber die Webseite /config geaenderte Werte liegen im NVS (ferraris_settings_store)
 * und haben beim Start Vorrang. Der Zaehlerstand liegt ebenfalls im NVS
 * (ferraris_counter_store): zyklisch und vor geplanten Neustarts gesichert.
 * Tageswerte (ferraris_day) wechseln um 0:00 Uhr lokal (Uhrzeit per NTP).
 */

#include <Arduino.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ferraris_config.h"

#include "adc_continuous_sampler.h"
#include "autotune_collector.h"
#include "ferraris_calib.h"
#include "ferraris_counter_store.h"
#include "ferraris_data.h"
#include "ferraris_day.h"
#include "ferraris_hwdetect.h"
#include "ferraris_log.h"
#include "ferraris_logger.h"
#include "ferraris_net.h"
#include "ferraris_settings.h"
#include "ferraris_settings_store.h"
#include "ferraris_status.h"
#include "ferraris_sysmon.h"
#include "quadrature_decoder.h"
#include "schmitt_trigger_autotune.h"
#include "schmitt_trigger_logic.h"

// Normalerweise aus Git (tools/pio_version.py: git describe), dies ist der
// Fallback ohne Git. Beim Taggen eines Releases mit anpassen.
#ifndef FERRARIS_FW_VERSION
#define FERRARIS_FW_VERSION "2.2.0"
#endif

static const char *TAG = "ferraris";

// ─── Lichtschranken Q1..Qn ──────────────────────────────────────────────────
// Alle aktiven Q-Sensoren werden abgetastet und geloggt; zwei davon (Zaehlpaar
// A/B) gehen als Lichtschranke A/B in die Quadratur. Anzahl, Pins und Paar:
// Defaults aus ferraris_config.h, aenderbar ueber /config (ab Start fest).

#define MAX_SENSORS FERRARIS_SETTINGS_MAX_SENSORS

static_assert(FERRARIS_SENSOR_COUNT >= FERRARIS_SETTINGS_MIN_SENSORS && FERRARIS_SENSOR_COUNT <= MAX_SENSORS,
              "FERRARIS_SENSOR_COUNT muss 2..8 sein");
static_assert(FERRARIS_COUNT_A_Q >= 1 && FERRARIS_COUNT_A_Q <= FERRARIS_SENSOR_COUNT,
              "FERRARIS_COUNT_A_Q muss 1..FERRARIS_SENSOR_COUNT sein");
static_assert(FERRARIS_COUNT_B_Q >= 1 && FERRARIS_COUNT_B_Q <= FERRARIS_SENSOR_COUNT,
              "FERRARIS_COUNT_B_Q muss 1..FERRARIS_SENSOR_COUNT sein");
static_assert(FERRARIS_COUNT_A_Q != FERRARIS_COUNT_B_Q,
              "Zaehlsensoren A und B muessen verschiedene Q-Sensoren sein (FERRARIS_COUNT_A_Q/B_Q)");
static_assert(SCHMITT_TRIGGER_THRESHOLD_HIGH > SCHMITT_TRIGGER_THRESHOLD_LOW,
              "Schmitt: HIGH muss groesser als LOW sein");

static const int s_default_pins[MAX_SENSORS] = {
    FERRARIS_Q1_GPIO, FERRARIS_Q2_GPIO, FERRARIS_Q3_GPIO, FERRARIS_Q4_GPIO,
    FERRARIS_Q5_GPIO, FERRARIS_Q6_GPIO, FERRARIS_Q7_GPIO, FERRARIS_Q8_GPIO,
};
static const char *const s_sensor_names[MAX_SENSORS] = { "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7", "Q8" };
static int s_n_sensors;                    // aktive Eingaenge (ab Start fest)
static int s_sensor_pins[MAX_SENSORS];     // GPIO je Q (ab Start fest)

// ─── Zustand ─────────────────────────────────────────────────────────────────

static ferraris_data_t          *s_data;
static quad_decoder_t           *s_decoder;
static ferraris_logger_t        *s_logger;
static adc_continuous_sampler_t *s_sampler;
static schmitt_logic_t           s_logic_a, s_logic_b;
static volatile bool             s_counting;    // Quadratur -> Zaehler freigegeben
static volatile bool             s_collecting;  // Start-Auto-Tune sammelt
static autotune_collector_t      s_collector;
static ferraris_hw_status_t      s_hw_status[MAX_SENSORS];
static uint32_t                  s_adc_rate;      // Sample-Sets/s (letzte Sekunde)
static uint32_t                  s_adc_restarts;

// Automatisches Einmessen (/config): Aufzeichnung im DMA-Callback, Rest im loop()-Task.
#define CALIB_BUF_VALUES 16384            // 32 KB, nur waehrend der Einmessung belegt
enum { CALIB_IDLE = 0, CALIB_RUNNING, CALIB_DONE };
static ferraris_calib_rec_t              s_calib_rec;
static ferraris_calib_rec_t *volatile    s_calib_feed;   // != NULL -> Callback zeichnet auf
static int                               s_calib_state;
static uint16_t                         *s_calib_buf;
static uint32_t                          s_calib_start_ms, s_calib_duration_ms, s_calib_elapsed_ms;
static bool                              s_calib_reverse;
static ferraris_calib_result_t           s_calib_result;

// Tageswerte (Basis zu Tagesbeginn, Vortag). Schreiber: loop()-Task.
static ferraris_day_t            s_day;

// Zaehlerstand im NVS: zuletzt gesicherter Stand und wann er zuletzt dem
// aktuellen entsprach (Speichern oder Pruefung ohne Aenderung). Nur loop()-Task.
static ferraris_counter_state_t  s_saved;
static uint32_t                  s_saved_ms;
static bool                      s_saved_known;   // NVS-Stand bekannt (geladen/geschrieben)

// Einstellungen: Defaults (ferraris_config.h), Stand beim Start (an Sampler,
// WLAN und MQTT uebergeben) und aktuell gespeicherter Stand (/config).
static ferraris_settings_t       s_defaults, s_boot, s_settings;
static bool                      s_settings_from_nvs;
static bool                      s_safe_mode;     // Absturzserie -> Werkseinstellungen
static int                       s_idx_a, s_idx_b;   // Zaehlpaar (Index 0..n-1), ab Start fest

/**
 * @brief Q-Pins auf ADC1-Kanaele abbilden und auf Duplikate pruefen.
 * @return false bei ungueltigem/doppeltem Pin (dann kein Sampling moeglich).
 */
static bool sensor_channels(adc_channel_t out[MAX_SENSORS])
{
    for (int i = 0; i < s_n_sensors; i++) {
        int ch = ferraris_gpio_to_adc1_channel(s_sensor_pins[i]);
        if (ch < 0) {
            FLOGE(TAG, "%s: GPIO%d ist kein ADC1-Pin (32..39)", s_sensor_names[i], s_sensor_pins[i]);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (s_sensor_pins[j] == s_sensor_pins[i]) {
                FLOGE(TAG, "%s und %s nutzen beide GPIO%d", s_sensor_names[j],
                      s_sensor_names[i], s_sensor_pins[i]);
                return false;
            }
        }
        out[i] = (adc_channel_t)ch;
    }
    return true;
}

// ─── Hotpath: DMA-Callback ───────────────────────────────────────────────────

/**
 * @brief Pro Sample-Set (~10 kHz): Schmitt + Quadratur + Tracker.
 *
 * Laeuft im adc_reader-Task. Kein I/O, keine Sperren, keine Allokation.
 * Die Quadratur wird immer gespeist (bleibt synchron), gezaehlt wird erst
 * nach der Start-Kalibrierung (s_counting).
 */
static void dma_sample_callback(const int *raw, int n, uint32_t now_us, void *user)
{
    (void)user;
    const int raw_a = raw[s_idx_a];
    const int raw_b = raw[s_idx_b];
    bool a_edge = schmitt_logic_process(&s_logic_a, raw_a);
    bool b_edge = schmitt_logic_process(&s_logic_b, raw_b);
    bool a_high = schmitt_logic_get_state(&s_logic_a);
    bool b_high = schmitt_logic_get_state(&s_logic_b);

    if (a_edge || b_edge) {
        quad_direction_t dir = quad_decoder_process(s_decoder, a_high, b_high);
        if (s_counting && dir != QUAD_DIR_NONE) {
            // Eigene ms-Zeit statt now_us/1000: now_us laeuft nach 71 min ueber,
            // die Leistungsmessung braucht Differenzen ueber Stunden.
            ferraris_data_add_quad(s_data, (int8_t)dir,
                                   (uint32_t)(esp_timer_get_time() / 1000));
        }
    }

    if (s_collecting) {
        autotune_collector_add(&s_collector, (int16_t)raw_a, (int16_t)raw_b, now_us / 1000);
    }

    ferraris_calib_rec_t *cr = s_calib_feed;
    if (cr) ferraris_calib_rec_feed(cr, raw);

    // Tracker mit JEDEM Sample (EMA braucht kontinuierlichen Feed).
    // Production-State = Schmitt-Ausgang (true = Marke, nicht invertiert).
    ferraris_logger_feed(s_logger, raw_a, raw_b, a_high, b_high);
    ferraris_logger_feed_sensors(s_logger, raw, n);
}

// ─── Einstellungen ───────────────────────────────────────────────────────────

static void settings_defaults(ferraris_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->sensor_count     = FERRARIS_SENSOR_COUNT;
    memcpy(s->sensor_gpio, s_default_pins, sizeof(s->sensor_gpio));
    s->revs_per_kwh     = FERRARIS_REVOLUTIONS_PER_KWH;
    for (int i = 0; i < MAX_SENSORS; i++) {
        s->threshold_high[i] = SCHMITT_TRIGGER_THRESHOLD_HIGH;
        s->threshold_low[i]  = SCHMITT_TRIGGER_THRESHOLD_LOW;
    }
    s->count_a          = FERRARIS_COUNT_A_Q;
    s->count_b          = FERRARIS_COUNT_B_Q;
    s->envelope_auto    = FERRARIS_ENVELOPE_AUTO;
    s->mqtt_interval_ms = FERRARIS_MQTT_INTERVAL_MS;
    strlcpy(s->hostname, FERRARIS_HOSTNAME, sizeof(s->hostname));
    strlcpy(s->mqtt_uri, FERRARIS_MQTT_URI, sizeof(s->mqtt_uri));
    strlcpy(s->mqtt_user, FERRARIS_MQTT_USER, sizeof(s->mqtt_user));
    strlcpy(s->mqtt_password, FERRARIS_MQTT_PASSWORD, sizeof(s->mqtt_password));
    strlcpy(s->mqtt_prefix, FERRARIS_MQTT_TOPIC_PREFIX, sizeof(s->mqtt_prefix));
    strlcpy(s->ap_password, FERRARIS_AP_PASSWORD, sizeof(s->ap_password));
}

/**
 * @brief Defaults + NVS. Ungueltiger NVS-Stand -> laut warnen, Defaults nutzen.
 *
 * Abgesicherter Modus: gespeicherte Werte werden geladen (die Webseite zeigt
 * und bearbeitet sie), aber NICHT angewendet - gestartet wird mit Defaults.
 */
static void settings_load(void)
{
    settings_defaults(&s_defaults);
    s_settings = s_defaults;
    s_settings_from_nvs = ferraris_settings_load(&s_settings);
    const char *why = nullptr, *field = nullptr;
    if (s_settings_from_nvs && !ferraris_settings_validate(&s_settings, &why, &field)) {
        FLOGE(TAG, "Gespeicherte Einstellungen ungueltig (%s: %s) - nutze Defaults",
              field ? field : "?", why ? why : "?");
        s_settings = s_defaults;
        s_settings_from_nvs = false;
    }
    s_boot  = s_safe_mode ? s_defaults : s_settings;
    s_n_sensors = s_boot.sensor_count;
    memcpy(s_sensor_pins, s_boot.sensor_gpio, sizeof(s_sensor_pins));
    s_idx_a = s_boot.count_a - 1;
    s_idx_b = s_boot.count_b - 1;
}

/**
 * @brief Schwellen ohne ungueltigen Zwischenzustand setzen.
 *
 * Der DMA-Callback liest parallel. Erst auf das weiteste Band (max HIGH,
 * min LOW), dann auf die Zielwerte: nach jedem einzelnen 32-bit-Schreiben gilt
 * HIGH > LOW, eine Schwelle wandert nie ueber die andere.
 */
static void set_thresholds_live(schmitt_logic_t *l, int high, int low)
{
    schmitt_logic_set_thresholds(l, max(high, l->threshold_high), min(low, l->threshold_low));
    schmitt_logic_set_thresholds(l, high, low);
}

#if FERRARIS_WIFI_ENABLE   // nur die Webseite /config aendert Einstellungen

static void settings_get(ferraris_settings_t *current, ferraris_settings_t *defaults)
{
    *current  = s_settings;
    *defaults = s_defaults;
}

static bool settings_apply(const ferraris_settings_t *s, const char **why, const char **field)
{
    if (!ferraris_settings_validate(s, why, field)) return false;
    if (!ferraris_settings_save(s)) {
        if (why) *why = "Speichern im NVS fehlgeschlagen";
        if (field) *field = nullptr;
        return false;
    }
    s_settings = *s;
    s_settings_from_nvs = true;

    // Sofort wirksam. Alles andere (Eingaenge, Zaehlpaar, Hostname, MQTT, Passwort) nach Neustart.
    ferraris_data_set_revs_per_kwh(s_data, s->revs_per_kwh);
    ferraris_logger_set_meter_constant(s_logger, s->revs_per_kwh);
    // Schwellen des laufenden Zaehlpaars (ein geaendertes Paar gilt erst nach Neustart).
    set_thresholds_live(&s_logic_a, s->threshold_high[s_idx_a], s->threshold_low[s_idx_a]);
    set_thresholds_live(&s_logic_b, s->threshold_high[s_idx_b], s->threshold_low[s_idx_b]);
    ferraris_logger_set_envelope(s_logger, s->envelope_auto,
                                 FERRARIS_ENVELOPE_FIXED_MIN, FERRARIS_ENVELOPE_FIXED_MAX);
    FLOGI(TAG, "Einstellungen: k=%ld, Schmitt A %d/%d B %d/%d, Huelle %s%s", (long)s->revs_per_kwh,
          s_logic_a.threshold_high, s_logic_a.threshold_low, s_logic_b.threshold_high,
          s_logic_b.threshold_low, s->envelope_auto ? "auto" : "fest",
          ferraris_settings_needs_restart(&s_boot, &s_settings) ? " - Rest nach Neustart" : "");
    return true;
}

static bool settings_restart_pending(void)
{
    return ferraris_settings_needs_restart(&s_boot, &s_settings);
}

#endif // FERRARIS_WIFI_ENABLE

// ─── Zaehlerstand speichern ──────────────────────────────────────────────────

/** Aktueller Stand (Gesamt + Tag) zum Speichern. */
static void counter_state(ferraris_counter_state_t *st)
{
    ferraris_data_get_quad(s_data, &st->forward, &st->backward);
    st->day       = s_day;
    st->day_valid = true;
}

/** setup(): gesicherten Stand uebernehmen (vor dem Start der Zaehlung). */
static void counter_restore(void)
{
    ferraris_day_start(&s_day, 0, 0);
#if FERRARIS_COUNTER_PERSIST_ENABLE
    ferraris_counter_state_t st = {};
    if (!ferraris_counter_load(&st)) {
        printf("# Kein gespeicherter Zaehlerstand - beginne bei 0\n");
        return;
    }
    ferraris_data_set_quad(s_data, st.forward, st.backward);
    // Altes Format ohne Tageswerte: der Tag beginnt beim geladenen Stand.
    if (st.day_valid) s_day = st.day;
    else ferraris_day_start(&s_day, st.forward, st.backward);
    s_saved       = st;
    s_saved_ms    = millis();
    s_saved_known = st.day_valid;
    // Immer sichtbar (auch im CSV-Modus), als '#'-Kommentarzeile.
    printf("# Zaehlerstand geladen: Bezug %.3f kWh, Einspeisung %.3f kWh (%ld/%ld Schritte)\n",
           ferraris_data_steps_to_kwh(s_boot.revs_per_kwh, st.forward),
           ferraris_data_steps_to_kwh(s_boot.revs_per_kwh, st.backward),
           (long)st.forward, (long)st.backward);
#endif
}

/**
 * @brief Stand ins NVS schreiben, falls er sich seit dem letzten Mal geaendert
 *        hat (loop()-Task). Nie aus dem ADC-Pfad.
 */
static bool counter_save(void)
{
#if FERRARIS_COUNTER_PERSIST_ENABLE
    if (!s_data) return false;
    ferraris_counter_state_t cur;
    counter_state(&cur);
    if (!s_saved_known || cur.forward != s_saved.forward || cur.backward != s_saved.backward ||
        memcmp(&cur.day, &s_saved.day, sizeof(cur.day)) != 0) {
        if (!ferraris_counter_save(&cur)) {
            FLOGE(TAG, "Zaehlerstand konnte nicht gespeichert werden (NVS)");
            return false;
        }
        s_saved       = cur;
        s_saved_known = true;
    }
    s_saved_ms = millis();   // gesicherter Stand ist (wieder) aktuell
    return true;
#else
    return false;
#endif
}

/** loop(): zyklisch sichern (schreibt nur bei Aenderung). */
static void counter_loop(void)
{
#if FERRARIS_COUNTER_PERSIST_ENABLE
    if (FERRARIS_COUNTER_SAVE_INTERVAL_MS == 0 || !s_counting) return;
    if (millis() - s_saved_ms >= FERRARIS_COUNTER_SAVE_INTERVAL_MS) counter_save();
#endif
}

/** loop(): einmal je Sekunde das Datum pruefen; Tageswechsel sofort sichern. */
static void day_loop(void)
{
    static uint32_t last_ms;
    if (!s_data || millis() - last_ms < 1000) return;
    last_ms = millis();
    const time_t now = time(nullptr);
    if (now < 1700000000) return;            // noch keine Uhrzeit (NTP)
    struct tm tm;
    localtime_r(&now, &tm);
    const int32_t today = ferraris_day_id(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    const int32_t before = s_day.day_id;
    int32_t fwd, bwd;
    ferraris_data_get_quad(s_data, &fwd, &bwd);
    if (!ferraris_day_update(&s_day, today, fwd, bwd)) return;
    if (before) {
        const int32_t k = ferraris_data_get_revs_per_kwh(s_data);
        printf("# Tageswechsel: %ld Bezug %.3f kWh, Einspeisung %.3f kWh\n", (long)s_day.prev_id,
               ferraris_data_steps_to_kwh(k, s_day.prev_fwd),
               ferraris_data_steps_to_kwh(k, s_day.prev_bwd));
    }
    counter_save();
}

/** ferraris_sysmon: vor Neustart (Webseite, Firmware-Update, Rollback). */
static void counter_before_restart(void)
{
#if FERRARIS_COUNTER_PERSIST_ENABLE
    if (!s_data) return;
    if (counter_save()) {
        printf("# Zaehlerstand gesichert: Bezug %.3f kWh, Einspeisung %.3f kWh\n",
               (double)ferraris_data_get_import_kwh(s_data),
               (double)ferraris_data_get_export_kwh(s_data));
    }
#endif
}

#if FERRARIS_WIFI_ENABLE   // gesetzt wird nur ueber die Webseite
/** Zaehlerstand auf den echten Stand setzen (0/0 = zuruecksetzen). Tageswerte bleiben. */
static bool counter_set(double import_kwh, double export_kwh, const char **why)
{
    static char msg[96];
    if (!s_data) { *why = "Zaehler nicht initialisiert"; return false; }
    const int32_t k = ferraris_data_get_revs_per_kwh(s_data);
    int32_t new_fwd, new_bwd;
    if (!ferraris_data_kwh_to_steps(k, import_kwh, &new_fwd) ||
        !ferraris_data_kwh_to_steps(k, export_kwh, &new_bwd)) {
        snprintf(msg, sizeof(msg), "Wert zu gross (hoechstens %.0f kWh bei k = %ld)",
                 ferraris_data_steps_to_kwh(k, (int32_t)FERRARIS_DATA_MAX_SET_STEPS), (long)k);
        *why = msg;
        return false;
    }
    int32_t old_fwd, old_bwd;
    ferraris_data_get_quad(s_data, &old_fwd, &old_bwd);
    ferraris_data_set_quad(s_data, new_fwd, new_bwd);
    ferraris_day_shift(&s_day, new_fwd - old_fwd, new_bwd - old_bwd);
#if FERRARIS_COUNTER_PERSIST_ENABLE
    if (!counter_save()) {
        *why = "Gesetzt, aber Speichern im NVS fehlgeschlagen";
        return false;
    }
#endif
    return true;
}
#endif

// ─── Automatisches Einmessen ─────────────────────────────────────────────────

/** Aufzeichnung beenden; der Callback sieht den NULL-Zeiger spaetestens beim naechsten Set. */
static void calib_detach(void)
{
    s_calib_feed = NULL;
    vTaskDelay(pdMS_TO_TICKS(5));    // laufenden feed()-Aufruf sicher abwarten
    s_calib_elapsed_ms = millis() - s_calib_start_ms;
}

#if FERRARIS_WIFI_ENABLE   // gestartet wird nur ueber die Webseite
static bool calib_start(uint32_t duration_s, bool reverse, const char **why)
{
    if (s_calib_state == CALIB_RUNNING) { *why = "Einmessung laeuft bereits"; return false; }
    if (!s_sampler) { *why = "ADC-Abtastung laeuft nicht"; return false; }
    if (duration_s < 30 || duration_s > 3600) { *why = "Dauer muss 30..3600 s sein"; return false; }
    if (!s_calib_buf) s_calib_buf = (uint16_t *)malloc(CALIB_BUF_VALUES * sizeof(uint16_t));
    if (!s_calib_buf) { *why = "Kein Speicher fuer die Aufzeichnung"; return false; }

    // Start-Aufloesung ~5 ms; der Rekorder vergroebert sich bei Bedarf selbst.
    uint32_t sets_per_s = s_adc_rate ? s_adc_rate : FERRARIS_ADC_DMA_SAMPLE_RATE_HZ / s_n_sensors;
    uint32_t bucket_sets = sets_per_s / 200;
    if (!ferraris_calib_rec_init(&s_calib_rec, s_calib_buf, CALIB_BUF_VALUES, s_n_sensors,
                                 bucket_sets ? bucket_sets : 1)) {
        *why = "Aufzeichnung konnte nicht vorbereitet werden";
        return false;
    }
    memset(&s_calib_result, 0, sizeof(s_calib_result));
    s_calib_reverse     = reverse;
    s_calib_duration_ms = duration_s * 1000u;
    s_calib_start_ms    = millis();
    s_calib_elapsed_ms  = 0;
    s_calib_state       = CALIB_RUNNING;
    s_calib_feed        = &s_calib_rec;   // zuletzt: ab hier zeichnet der Callback auf
    FLOGI(TAG, "Einmessung gestartet: %lu s, %d Sensoren%s", (unsigned long)duration_s,
          s_n_sensors, reverse ? ", Einspeisung" : "");
    return true;
}
#endif

static void calib_finish(void)
{
    if (s_calib_state != CALIB_RUNNING) return;
    calib_detach();
    const ferraris_calib_rec_t *r = &s_calib_rec;
    // Bucket-Breite aus der tatsaechlichen Set-Rate (Sets je Bucket x ms je Set).
    float bucket_ms = r->total_sets ? (float)r->bucket_sets * (float)s_calib_elapsed_ms /
                                          (float)r->total_sets : 0.0f;
    ferraris_calib_analyze(s_calib_buf, r->rows, r->n, bucket_ms, s_calib_reverse, &s_calib_result);
    free(s_calib_buf);
    s_calib_buf   = NULL;
    s_calib_state = CALIB_DONE;
    // Immer sichtbar (auch im CSV-Modus), als '#'-Kommentarzeile.
    if (s_calib_result.ok) {
        printf("# Einmessung: Paar A=Q%d B=Q%d, %.1f Umdrehungen, Periode %.1f s\n",
               s_calib_result.count_a, s_calib_result.count_b,
               (double)s_calib_result.revolutions, (double)s_calib_result.period_s);
    } else {
        printf("# Einmessung ohne Ergebnis: %s\n", s_calib_result.message);
    }
}

#if FERRARIS_WIFI_ENABLE
static void calib_cancel(void)
{
    if (s_calib_state == CALIB_RUNNING) calib_detach();
    free(s_calib_buf);
    s_calib_buf   = NULL;
    s_calib_state = CALIB_IDLE;
}

#endif

/** loop(): Einmessung nach Ablauf der Dauer auswerten. */
static void calib_loop(void)
{
    if (s_calib_state == CALIB_RUNNING && millis() - s_calib_start_ms >= s_calib_duration_ms) {
        calib_finish();
    }
}

#if FERRARIS_WIFI_ENABLE

static int calib_status_json(char *buf, size_t len)
{
    static const char *const names[] = { "idle", "running", "done" };
    const bool run = s_calib_state == CALIB_RUNNING;
    const uint32_t elapsed = run ? millis() - s_calib_start_ms : s_calib_elapsed_ms;
    const ferraris_calib_rec_t *r = &s_calib_rec;
    float bucket_ms = (run && r->total_sets) ? (float)r->bucket_sets * (float)elapsed / (float)r->total_sets
                                             : s_calib_result.bucket_ms;
    int n = snprintf(buf, len, "{\"state\":\"%s\",\"elapsed_s\":%lu,\"duration_s\":%lu,"
                               "\"reverse\":%s,\"bucket_ms\":%.1f,\"result\":",
                     names[s_calib_state], (unsigned long)(elapsed / 1000),
                     (unsigned long)(s_calib_duration_ms / 1000), s_calib_reverse ? "true" : "false",
                     (double)bucket_ms);
    if (n < 0 || (size_t)n >= len) return n;
    int m;
    if (s_calib_state == CALIB_DONE) {
        m = ferraris_calib_result_to_json(&s_calib_result, buf + n, len - (size_t)n);
    } else {
        m = snprintf(buf + n, len - (size_t)n, "null");
    }
    if (m < 0 || (size_t)(n + m) >= len) return n + (m < 0 ? 0 : m);
    n += m;
    return n + snprintf(buf + n, len - (size_t)n, "}");
}
#endif

// ─── Auto-Tune + Re-Tune (nur mit SCHMITT_AUTOTUNE_ENABLE) ───────────────────
#if SCHMITT_AUTOTUNE_ENABLE

#define AUTOTUNE_MIN_BIN_FRACTION 0.05f   // zweite Population >= 5 % des Haupt-Bins

static schmitt_autotune_config_t autotune_config(int n)
{
    schmitt_autotune_config_t cfg = schmitt_autotune_config_default();
    cfg.num_samples         = n;
    cfg.smoothing_window    = SCHMITT_AUTOTUNE_SMOOTHING;
    cfg.min_peak_separation = SCHMITT_AUTOTUNE_MIN_SEPARATION;
    return cfg;
}

/** Ergebnis je Kanal uebernehmen; ein Kanal ohne zwei Populationen behaelt seine Schwellen. */
static void apply_tuned(schmitt_logic_t *l, const char *ch, const schmitt_autotune_result_t *r,
                        const char *src)
{
    if (!r->valid) {
        FLOGW(TAG, "%s %s: keine zwei Populationen - Schwellen bleiben %d/%d", src, ch,
              l->threshold_high, l->threshold_low);
        return;
    }
    set_thresholds_live(l, r->threshold_high, r->threshold_low);
    FLOGI(TAG, "%s %s: HIGH=%d LOW=%d (Hyst=%d)", src, ch, r->threshold_high, r->threshold_low,
          r->threshold_high - r->threshold_low);
}

/**
 * @brief Dauerbasiertes Start-Auto-Tune (blockiert bis beide Populationen
 *        sichtbar sind, hoechstens FERRARIS_AUTOTUNE_MAX_TIME_MS).
 *
 * Jeder Kanal bekommt seine eigenen Schwellen (Sensoren altern verschieden).
 */
static void autotune_calibrate(void)
{
    autotune_collector_init(&s_collector, FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY,
                            FERRARIS_ADC_DMA_SNAPSHOT_DECIMATION,
                            FERRARIS_AUTOTUNE_MIN_TIME_MS, FERRARIS_AUTOTUNE_MAX_TIME_MS,
                            SCHMITT_AUTOTUNE_MIN_SEPARATION, AUTOTUNE_MIN_BIN_FRACTION);
    if (!s_collector.buf_a) {
        FLOGW(TAG, "Auto-Tune: kein Speicher - Default-Schwellen, Zaehlung an");
        s_counting = true;
        return;
    }

    FLOGI(TAG, "Auto-Tune: sammle (min %d ms, max %d ms) ...",
          FERRARIS_AUTOTUNE_MIN_TIME_MS, FERRARIS_AUTOTUNE_MAX_TIME_MS);
    s_collecting = true;
    const uint32_t start = millis();
    while (!autotune_collector_ready(&s_collector, (uint32_t)(esp_timer_get_time() / 1000))) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (millis() - start > FERRARIS_AUTOTUNE_MAX_TIME_MS + 1000) break;  // Sicherheitsnetz
    }
    s_collecting = false;

    const int n = s_collector.count;
    schmitt_autotune_config_t cfg = autotune_config(n);
    schmitt_autotune_result_t ra = schmitt_autotune_analyze(s_collector.buf_a, n, &cfg);
    schmitt_autotune_result_t rb = schmitt_autotune_analyze(s_collector.buf_b, n, &cfg);
    FLOGI(TAG, "Auto-Tune A: %s (Peaks %d/%d, HIGH=%d LOW=%d), %d Samples",
          ra.valid ? "OK" : "FEHLER", ra.peak_low, ra.peak_high, ra.threshold_high,
          ra.threshold_low, n);
    FLOGI(TAG, "Auto-Tune B: %s (Peaks %d/%d, HIGH=%d LOW=%d)",
          rb.valid ? "OK" : "FEHLER", rb.peak_low, rb.peak_high, rb.threshold_high,
          rb.threshold_low);

    apply_tuned(&s_logic_a, "A", &ra, "Auto-Tune");
    apply_tuned(&s_logic_b, "B", &rb, "Auto-Tune");

    autotune_collector_destroy(&s_collector);
    // Der Decoder lief die ganze Zeit mit, kein Reset noetig.
    s_counting = true;
    FLOGI(TAG, "Zaehlung freigegeben");
}

/**
 * @brief Periodisches Re-Tune aus dem Snapshot-Ring (LED-Alterung, Staub,
 *        Temperatur). Pausiert die Zaehlung nicht: set_thresholds erhaelt den
 *        Zustand, eine verschobene Schwelle klassifiziert nur das naechste Sample.
 */
static void retune_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS((TickType_t)FERRARIS_RETUNE_INTERVAL_MIN * 60 * 1000);
    int16_t *snap_a = (int16_t *)malloc(FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY * sizeof(int16_t));
    int16_t *snap_b = (int16_t *)malloc(FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY * sizeof(int16_t));
    if (!snap_a || !snap_b) {
        FLOGE(TAG, "Re-Tune: kein Speicher - Task beendet");
        free(snap_a);
        free(snap_b);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(interval);
        int n = adc_continuous_sampler_snapshot(s_sampler, snap_a, snap_b,
                                                FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY);
        if (n < 32) {
            FLOGW(TAG, "Re-Tune: nur %d Samples im Ring - uebersprungen", n);
            continue;
        }
        schmitt_autotune_config_t cfg = autotune_config(n);
        schmitt_autotune_result_t ra = schmitt_autotune_analyze(snap_a, n, &cfg);
        schmitt_autotune_result_t rb = schmitt_autotune_analyze(snap_b, n, &cfg);
        apply_tuned(&s_logic_a, "A", &ra, "Re-Tune");
        apply_tuned(&s_logic_b, "B", &rb, "Re-Tune");
    }
}

#endif // SCHMITT_AUTOTUNE_ENABLE

// ─── Ausgabe ─────────────────────────────────────────────────────────────────

#if !FERRARIS_CSV_LOG_ENABLE
/** Klartext-Zaehlerstand, nur bei Aenderung. */
static void print_task(void *arg)
{
    (void)arg;
    int32_t last_fwd = 0, last_bwd = 0;
    for (;;) {
        int32_t fwd, bwd;
        ferraris_data_get_quad(s_data, &fwd, &bwd);
        if (fwd != last_fwd || bwd != last_bwd) {
            float watts = ferraris_data_get_power_watts(s_data, (uint32_t)(esp_timer_get_time() / 1000));
            int32_t day_fwd, day_bwd;
            ferraris_day_get(&s_day, fwd, bwd, &day_fwd, &day_bwd);
            const int32_t k = ferraris_data_get_revs_per_kwh(s_data);
            printf("Zaehler: Verbrauch=%ld  Einspeisung=%ld  Netto=%ld  "
                   "(delta_fwd=%ld delta_bwd=%ld)  %.4f kWh  %.0f W  "
                   "Tagesbezug %.3f kWh  Tageslieferung %.3f kWh\n",
                   (long)fwd, (long)bwd, (long)(fwd - bwd),
                   (long)(fwd - last_fwd), (long)(bwd - last_bwd),
                   ferraris_data_steps_to_kwh(k, fwd - bwd), (double)watts,
                   ferraris_data_steps_to_kwh(k, day_fwd), ferraris_data_steps_to_kwh(k, day_bwd));
            last_fwd = fwd;
            last_bwd = bwd;
        }
        vTaskDelay(pdMS_TO_TICKS(FERRARIS_PRINT_INTERVAL_MS));
    }
}
#endif

/** Status-Snapshot fuer HTTP/MQTT (laeuft im loop()-Task). */
static void fill_status(ferraris_status_t *st)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    st->uptime_s       = now / 1000;
    st->counting       = s_counting;
    st->revs_per_kwh   = ferraris_data_get_revs_per_kwh(s_data);
    ferraris_data_get_quad(s_data, &st->steps_import, &st->steps_export);
    const int32_t k    = st->revs_per_kwh;
    st->import_kwh     = ferraris_data_steps_to_kwh(k, st->steps_import);
    st->export_kwh     = ferraris_data_steps_to_kwh(k, st->steps_export);
    st->net_kwh        = ferraris_data_steps_to_kwh(k, st->steps_import - st->steps_export);
    int32_t day_fwd, day_bwd;
    ferraris_day_get(&s_day, st->steps_import, st->steps_export, &day_fwd, &day_bwd);
    st->day_id          = s_day.day_id;
    st->day_import_kwh  = ferraris_data_steps_to_kwh(k, day_fwd);
    st->day_export_kwh  = ferraris_data_steps_to_kwh(k, day_bwd);
    st->prev_day_id     = s_day.prev_id;
    st->prev_import_kwh = ferraris_data_steps_to_kwh(k, s_day.prev_fwd);
    st->prev_export_kwh = ferraris_data_steps_to_kwh(k, s_day.prev_bwd);
    st->power_w        = ferraris_data_get_power_watts(s_data, now);
    st->saved_age_s    = s_saved_known ? (int32_t)((millis() - s_saved_ms) / 1000) : -1;
    st->threshold_high = s_logic_a.threshold_high;
    st->threshold_low  = s_logic_a.threshold_low;
    st->count_a        = s_sensor_names[s_idx_a];
    st->count_b        = s_sensor_names[s_idx_b];
    st->n_sensors      = s_n_sensors;
    st->adc_sets_per_s = s_adc_rate;
    st->adc_restarts   = s_adc_restarts;
    if (s_sampler) {
        adc_sampler_stats_t as;
        adc_continuous_sampler_get_stats(s_sampler, &as);
        st->adc_errors   = as.reads_err + as.invalid + as.unknown_ch;
        st->adc_timeouts = as.reads_timeout;
    }
    ferraris_sysmon_info_t si;
    ferraris_sysmon_get_info(&si);
    st->reset_reason = si.reset_reason;
    st->boot_count   = si.boot_count;
    st->crash_count  = si.crash_count;
    st->safe_mode    = si.safe_mode;
    st->coredump     = si.coredump;
    st->ota_state    = si.ota_state;
    ferraris_sysmon_heap(&st->heap_free, &st->heap_min, &st->heap_block);
    st->fw_version     = FERRARIS_FW_VERSION;
    st->hostname       = s_boot.hostname;
    for (int i = 0; i < s_n_sensors; i++) {
        ferraris_sensor_status_t *s = &st->sensors[i];
        s->name = s_sensor_names[i];
        s->gpio = s_sensor_pins[i];
        s->hw   = ferraris_hw_status_name(s_hw_status[i]);
        ferraris_logger_get_sensor(s_logger, i, &s->raw, &s->run_min, &s->run_max);
        // Zaehlpaar: die tatsaechlich wirksamen (ggf. von Auto-Tune gesetzten) Werte.
        const schmitt_logic_t *l = i == s_idx_a ? &s_logic_a : (i == s_idx_b ? &s_logic_b : NULL);
        s->high = l ? l->threshold_high : s_settings.threshold_high[i];
        s->low  = l ? l->threshold_low  : s_settings.threshold_low[i];
    }
}

static void console_pause(uint32_t ms)
{
    ferraris_logger_pause(s_logger, ms);
}

static int csv_row(char *buf, size_t len)
{
    return ferraris_logger_format_row(s_logger, buf, len);
}

static void print_boot_config(void)
{
    FLOGI(TAG, "=== Ferraris-Zaehler Ausleser (RML 2.0 BAScloud) ===");
    FLOGI(TAG, "Firmware: %s  (Build %s %s)", FERRARIS_FW_VERSION, __DATE__, __TIME__);
    FLOGI(TAG, "Arduino : %s   ESP-IDF: %s", ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion());
    FLOGI(TAG, "Chip    : %s rev %d, %d Kern(e), Flash %lu KB", ESP.getChipModel(),
          (int)ESP.getChipRevision(), (int)ESP.getChipCores(),
          (unsigned long)(ESP.getFlashChipSize() / 1024));
    if (ESP.getPsramSize() > 0) {
        FLOGI(TAG, "PSRAM   : %lu KB", (unsigned long)(ESP.getPsramSize() / 1024));
    } else {
        FLOGI(TAG, "PSRAM   : nicht gefunden - OK (wird nicht gebraucht)");
    }
    FLOGI(TAG, "Einstellg.: %s", s_safe_mode ? "WERKSEINSTELLUNGEN (abgesicherter Modus)"
                                 : s_settings_from_nvs ? "aus NVS (Webseite /config)" : "Defaults");
    FLOGI(TAG, "Zaehlerkonstante: %ld Umdrehungen/kWh", (long)s_boot.revs_per_kwh);
#if FERRARIS_COUNTER_PERSIST_ENABLE
    FLOGI(TAG, "Zaehlerst.: NVS, alle %lu s (bei Aenderung) und vor geplanten Neustarts",
          (unsigned long)(FERRARIS_COUNTER_SAVE_INTERVAL_MS / 1000));
#else
    FLOGI(TAG, "Zaehlerst.: nicht gespeichert (beginnt nach jedem Start bei 0)");
#endif
    FLOGI(TAG, "ADC     : continuous/DMA %d Hz (~%d Hz je Sensor), Snapshot %d / Decim %d",
          FERRARIS_ADC_DMA_SAMPLE_RATE_HZ, FERRARIS_ADC_DMA_SAMPLE_RATE_HZ / s_n_sensors,
          FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY, FERRARIS_ADC_DMA_SNAPSHOT_DECIMATION);
    char pins[MAX_SENSORS * 4 + 1] = "";
    for (int i = 0, pos = 0; i < s_n_sensors; i++) {
        pos += snprintf(pins + pos, sizeof(pins) - pos, "%s%d", i ? "/" : "", s_sensor_pins[i]);
    }
    FLOGI(TAG, "Sensoren: Q1..Q%d = GPIO%s, Zaehlung A=Q%d B=Q%d", s_n_sensors, pins,
          s_boot.count_a, s_boot.count_b);
    FLOGI(TAG, "Schmitt : A=Q%d HIGH=%d LOW=%d, B=Q%d HIGH=%d LOW=%d, Auto-Tune %s",
          s_boot.count_a, s_boot.threshold_high[s_idx_a], s_boot.threshold_low[s_idx_a],
          s_boot.count_b, s_boot.threshold_high[s_idx_b], s_boot.threshold_low[s_idx_b],
          SCHMITT_AUTOTUNE_ENABLE ? "an" : "aus");
    if (s_boot.envelope_auto) {
        FLOGI(TAG, "Huelle  : automatisch (min/max laufen mit)");
    } else {
        FLOGI(TAG, "Huelle  : fest %d..%d", FERRARIS_ENVELOPE_FIXED_MIN, FERRARIS_ENVELOPE_FIXED_MAX);
    }
    FLOGI(TAG, "Quadratur: 4x (Gray-Code)");
#if FERRARIS_CSV_LOG_ENABLE
    FLOGI(TAG, "Ausgabe : CSV 18 + 3x%d Spalten @ %d ms auf UART0", s_n_sensors,
          FERRARIS_CSV_LOG_INTERVAL_MS);
#else
    FLOGI(TAG, "Ausgabe : Zaehler-Text auf UART0 (%d ms)", FERRARIS_PRINT_INTERVAL_MS);
#endif
#if FERRARIS_WIFI_ENABLE
    FLOGI(TAG, "WLAN    : Host %s, Einrichtungs-AP %sXXXX, MQTT %s", s_boot.hostname,
          FERRARIS_AP_SSID_PREFIX, s_boot.mqtt_uri[0] ? s_boot.mqtt_uri : "aus");
    FLOGI(TAG, "Uhrzeit : NTP %s, TZ %s (Tageswerte ab 0:00 Uhr)", FERRARIS_NTP_SERVER, FERRARIS_TIMEZONE);
#else
    FLOGI(TAG, "WLAN    : aus");
    FLOGI(TAG, "Uhrzeit : keine (ohne WLAN) - Tageswerte zaehlen ab dem Start");
#endif
}

/**
 * @brief ADC-Ueberwachung (loop-Task, 1x pro Sekunde).
 *
 * Misst die Sample-Sets pro Sekunde und startet den Sampler neu, wenn keine
 * Sets mehr kommen. Unveraenderte Rohwerte sind KEIN Kriterium: ein
 * uebersteuerter Sensor steht legitim konstant auf 4095. Die Zaehlung pausiert
 * beim Neustart nur Millisekunden; der Quadratur-Zustand bleibt erhalten.
 * Muss im selben Task laufen wie setup() (Sampler-Start, adc_lock-Besitzer).
 */
static void adc_watchdog(void)
{
    static uint32_t last_check, last_sets, stuck_since;
    if (!s_sampler) return;

    const uint32_t now = millis();
    if (now - last_check < 1000) return;
    last_check = now;

    adc_sampler_stats_t st;
    adc_continuous_sampler_get_stats(s_sampler, &st);
    s_adc_rate = st.sets - last_sets;
    last_sets  = st.sets;

    if (s_adc_rate > 0 || FERRARIS_ADC_STALL_MS == 0) {
        stuck_since = 0;
        return;
    }
    if (!stuck_since) stuck_since = now;
    if (now - stuck_since < FERRARIS_ADC_STALL_MS) return;

    stuck_since = 0;
    s_adc_restarts++;
    // Immer sichtbar (auch im CSV-Modus), als '#'-Kommentarzeile.
    printf("# ADC-Abtastung liefert keine Messungen - Neustart Nr. %lu\n"
           "#   reads ok=%lu timeout=%lu err=%lu bytes=%lu parsed=%lu invalid=%lu "
           "unknown_ch=%lu last_ch=%lu sets=%lu\n",
           (unsigned long)s_adc_restarts,
           (unsigned long)st.reads_ok, (unsigned long)st.reads_timeout,
           (unsigned long)st.reads_err, (unsigned long)st.bytes, (unsigned long)st.parsed,
           (unsigned long)st.invalid, (unsigned long)st.unknown_ch,
           (unsigned long)st.last_channel, (unsigned long)st.sets);
    adc_continuous_sampler_stop(s_sampler);
    esp_err_t err = adc_continuous_sampler_start(s_sampler, dma_sample_callback, NULL);
    if (err != ESP_OK) {
        printf("# ADC-Neustart fehlgeschlagen: %s\n", esp_err_to_name(err));
    }
}

// ─── Arduino ─────────────────────────────────────────────────────────────────

void setup()
{
    Serial.begin(115200);
    ferraris_log_set_level(FLOG_INFO);

    // Zuerst: Resetgrund, Startzaehler, abgesicherter Modus (bestimmt die Einstellungen).
    const ferraris_sysmon_config_t sysmon_cfg = {
        .loop_watchdog_ms  = FERRARIS_LOOP_WATCHDOG_MS,
        .stable_ms         = FERRARIS_STABLE_UPTIME_MS,
        .safe_mode_crashes = FERRARIS_SAFE_MODE_CRASHES,
        .ota_min_ms        = FERRARIS_OTA_VERIFY_MIN_MS,
        .ota_stable_ms     = FERRARIS_OTA_VERIFY_STABLE_MS,
        .ota_timeout_ms    = FERRARIS_OTA_VERIFY_TIMEOUT_MS,
        .before_restart    = counter_before_restart,
    };
    s_safe_mode = ferraris_sysmon_boot(&sysmon_cfg);
    settings_load();
    print_boot_config();

    s_data    = ferraris_data_create(s_boot.revs_per_kwh);
    s_decoder = quad_decoder_create();
    s_logger  = ferraris_logger_create(FERRARIS_CSV_LOG_INTERVAL_MS);
    if (!s_data || !s_decoder || !s_logger) {
        FLOGE(TAG, "Kein Speicher fuer Zaehler/Decoder/Logger - Abbruch");
        return;
    }
    // Zeitzone schon jetzt: die Tageswerte nutzen localtime(), NTP kommt spaeter.
    setenv("TZ", FERRARIS_TIMEZONE, 1);
    tzset();
    counter_restore();

    adc_channel_t sensor_ch[MAX_SENSORS];
    if (!sensor_channels(sensor_ch)) return;

    // Selbsttest VOR dem DMA-Sampler: eigene oneshot-Unit, danach freigegeben.
    ferraris_hwdetect_run(s_sensor_pins, s_sensor_names, s_n_sensors, s_hw_status);

    schmitt_logic_init(&s_logic_a, s_boot.threshold_high[s_idx_a], s_boot.threshold_low[s_idx_a]);
    schmitt_logic_init(&s_logic_b, s_boot.threshold_high[s_idx_b], s_boot.threshold_low[s_idx_b]);

    // Logger vor dem Sampler fertig konfigurieren, damit der Callback ab
    // Sample 0 vollstaendig speist (Huelle konvergiert schon beim Auto-Tune).
    ferraris_logger_set_thresholds(s_logger, &s_logic_a, &s_logic_b);
    ferraris_logger_set_meter_constant(s_logger, s_boot.revs_per_kwh);
    ferraris_logger_set_sensor_count(s_logger, s_n_sensors);
    ferraris_logger_set_envelope(s_logger, s_boot.envelope_auto,
                                 FERRARIS_ENVELOPE_FIXED_MIN, FERRARIS_ENVELOPE_FIXED_MAX);

    // WLAN VOR dem ADC starten: der WLAN-Start kalibriert den Funkteil und kann
    // eine schon laufende I2S-ADC-Abtastung (ESP32) einfrieren lassen.
#if FERRARIS_WIFI_ENABLE
    static const ferraris_calib_ops_t calib_ops = {
        .start       = calib_start,
        .finish      = calib_finish,
        .cancel      = calib_cancel,
        .status_json = calib_status_json,
    };
    static const ferraris_settings_ops_t settings_ops = {
        .get             = settings_get,
        .apply           = settings_apply,
        .restart_pending = settings_restart_pending,
        .autotune        = SCHMITT_AUTOTUNE_ENABLE != 0,
    };
    const ferraris_net_config_t net_cfg = {
        .default_ssid           = FERRARIS_WIFI_SSID,
        .default_password       = FERRARIS_WIFI_PASSWORD,
        .hostname               = s_boot.hostname,
        .http_port              = FERRARIS_HTTP_PORT,
        .ap_ssid_prefix         = FERRARIS_AP_SSID_PREFIX,
        .ap_password            = s_boot.ap_password,
        .admin_user             = FERRARIS_ADMIN_USER,
        .admin_password         = s_boot.ap_password,
        .sta_connect_timeout_ms = FERRARIS_STA_CONNECT_TIMEOUT_MS,
        .sta_lost_to_ap_ms      = FERRARIS_STA_LOST_TO_AP_MS,
        .ap_retry_interval_ms   = FERRARIS_AP_RETRY_INTERVAL_MS,
        .ap_off_delay_ms        = FERRARIS_AP_OFF_DELAY_MS,
        .ntp_server             = FERRARIS_NTP_SERVER,
        .timezone               = FERRARIS_TIMEZONE,
        .mqtt_uri          = s_boot.mqtt_uri,
        .mqtt_user         = s_boot.mqtt_user,
        .mqtt_password     = s_boot.mqtt_password,
        .mqtt_topic_prefix = s_boot.mqtt_prefix,
        .mqtt_interval_ms  = s_boot.mqtt_interval_ms,
        .console_pause_fn  = console_pause,
        .console_pause_ms  = FERRARIS_CONSOLE_PAUSE_MS,
        .settings          = &settings_ops,
        .calib             = &calib_ops,
        .counter_set_fn    = counter_set,
        .counter_persist   = FERRARIS_COUNTER_PERSIST_ENABLE != 0,
    };
    ferraris_net_begin(&net_cfg, fill_status, csv_row);
#endif

    s_sampler = adc_continuous_sampler_create(sensor_ch, s_n_sensors, s_idx_a, s_idx_b,
                                              FERRARIS_ADC_DMA_SAMPLE_RATE_HZ,
                                              FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY,
                                              FERRARIS_ADC_DMA_SNAPSHOT_DECIMATION);
    if (!s_sampler) {
        FLOGE(TAG, "DMA-Sampler konnte nicht angelegt werden - keine Zaehlung");
        return;
    }
    esp_err_t err = adc_continuous_sampler_start(s_sampler, dma_sample_callback, NULL);
    if (err != ESP_OK) {
        FLOGE(TAG, "DMA-Sampler-Start fehlgeschlagen: %s", esp_err_to_name(err));
        return;
    }

#if SCHMITT_AUTOTUNE_ENABLE
    autotune_calibrate();
    xTaskCreate(retune_task, "retune", 4096, NULL, 3, NULL);
#else
    s_counting = true;
    FLOGI(TAG, "Zaehlung freigegeben (Auto-Tune aus, Default-Schwellen)");
#endif

#if !FERRARIS_CSV_LOG_ENABLE
    xTaskCreate(print_task, "print", 3072, NULL, 3, NULL);
#endif


#if FERRARIS_CSV_LOG_ENABLE
    // Alle Boot-Logs sind raus. Ab jetzt nur noch Fehler, damit der CSV-Strom
    // sauber bleibt (auch IDF-/Arduino-Logs der WLAN-Treiber).
    ferraris_log_set_level(FLOG_ERROR);
    esp_log_level_set("*", ESP_LOG_NONE);
    if (ferraris_logger_start(s_logger) != ESP_OK) {
        FLOGE(TAG, "CSV-Logger-Task konnte nicht gestartet werden");
    }
#endif
}

void loop()
{
    static bool sysmon_started;
    if (!sysmon_started) {
        // Erst hier: setup() kann bei Init-Fehlern frueh enden, loop() laeuft trotzdem.
        sysmon_started = true;
        ferraris_sysmon_start();
    }
    adc_watchdog();
    calib_loop();
    counter_loop();
    day_loop();
#if FERRARIS_WIFI_ENABLE
    ferraris_net_loop();
    const bool net_ok = ferraris_net_healthy();
#else
    const bool net_ok = true;
#endif
    // Gesund = Messungen kommen und (falls WLAN an) die Webseite ist erreichbar.
    ferraris_sysmon_loop(s_adc_rate > 0 && net_ok);
    delay(5);
}
