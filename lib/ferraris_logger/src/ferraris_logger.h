/**
 * @file ferraris_logger.h
 * @brief ESP-Huelle um den pure-C Tracker: RML-CSV-Log auf UART0 + Live-Werte.
 *
 * Der Per-Channel-Tracker (EMA + asymmetrische Huelle + Mark-Passagen) lebt in
 * ferraris_logger_logic (pure C, PC + ESP identisch). Diese Huelle haelt die
 * Tracker-Instanzen, liest die Schmitt-Schwellen live ueber Pointer und
 * formatiert die CSV-Zeile:
 *
 *   millis, la_raw, la_filt, la_min, la_max, la_tOn, la_tOff, la_state, la_count,
 *           ra_raw, ra_filt, ra_min, ra_max, ra_tOn, ra_tOff, ra_state, ra_count,
 *   W, [Q1_raw, Q1_min, Q1_max, ..., Qn_raw, Qn_min, Qn_max]
 *
 * Die Tracker laufen immer (auch ohne CSV-Strom), damit HTTP/MQTT Rohwerte und
 * Huellen der Sensoren zeigen koennen. Der CSV-Strom auf UART0 startet erst
 * mit ferraris_logger_start().
 *
 * Polaritaet: gespeist wird der PRODUCTION-Schmitt-State (true = Marke);
 * la_state = !prev_mark (RML-Konvention: 0 = Marke, 1 = Verweilzeit).
 *
 * Thread-Safety: feed() im DMA-Callback (Single-Writer), Formatierung in
 * log_task/HTTP (Reader). 32-bit/float-Felder sind auf Xtensa atomar;
 * bewusstes Last-Good-Value ohne Mutex im Hotpath.
 */

#ifndef FERRARIS_LOGGER_H
#define FERRARIS_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "schmitt_trigger_logic.h"
#include "ferraris_logger_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max. Anzahl zusaetzlich geloggter Sensoren (Q1..Qn). */
#define FERRARIS_LOGGER_MAX_SENSORS 8

/** Puffergroesse, die fuer eine vollstaendige CSV-Zeile sicher reicht. */
#define FERRARIS_LOGGER_ROW_MAX (64 + 18 * 12 + FERRARIS_LOGGER_MAX_SENSORS * 3 * 6)

typedef struct ferraris_logger ferraris_logger_t;

/**
 * @brief Logger anlegen (ohne CSV-Task). Tracker starten auf ADC-Mitte 2048.
 * @param interval_ms CSV-Intervall (0 -> 250 ms = 4 Hz)
 */
ferraris_logger_t *ferraris_logger_create(uint32_t interval_ms);

/** Live-Schmitt-Logik fuer die Spalten la_tOn/la_tOff (stabile Pointer, NULL erlaubt). */
void ferraris_logger_set_thresholds(ferraris_logger_t *l,
                                    const schmitt_logic_t *la,
                                    const schmitt_logic_t *rb);

/** Zaehlerkonstante k fuer die W-Spalte (muss zur Production-Konstante passen). */
void ferraris_logger_set_meter_constant(ferraris_logger_t *l, int k_rev_per_kwh);

/**
 * @brief Huelle (min/max-Spalten) automatisch oder fest.
 *
 * auto_envelope = true: mitlaufende Huelle aus ferraris_logger_logic (Default).
 * false: alle min/max-Ausgaben (CSV, get_sensor) liefern fixed_min/fixed_max.
 * Die Tracker rechnen intern weiter, nur die Ausgabe wird umgeschaltet.
 */
void ferraris_logger_set_envelope(ferraris_logger_t *l, bool auto_envelope,
                                  int fixed_min, int fixed_max);

/** Anzahl Q-Sensoren fuer die Zusatzspalten (vor dem ersten feed setzen). */
void ferraris_logger_set_sensor_count(ferraris_logger_t *l, int n);

/** Zaehlpaar einspeisen (Hotpath: kein I/O, keine Sperren). */
void ferraris_logger_feed(ferraris_logger_t *l, int raw_a, int raw_b,
                          bool mark_a, bool mark_b);

/** Rohwerte aller Q-Sensoren einspeisen, raw[i] gehoert zu Q(i+1) (Hotpath). */
void ferraris_logger_feed_sensors(ferraris_logger_t *l, const int *raw, int n);

/**
 * @brief Aktuelle CSV-Zeile (ohne Zeilenende) in buf formatieren.
 * @return Laenge der Zeile, < 0 bei Fehler.
 */
int ferraris_logger_format_row(ferraris_logger_t *l, char *buf, size_t len);

/**
 * @brief Live-Werte eines Q-Sensors (Index 0 = Q1).
 * @return false, wenn i ausserhalb der Sensoranzahl liegt.
 */
bool ferraris_logger_get_sensor(const ferraris_logger_t *l, int i,
                                int *raw, int *run_min, int *run_max);

/**
 * @brief CSV-Ausgabe fuer ms Millisekunden anhalten (z. B. damit eine
 *        Netzwerk-Meldung lesbar bleibt). Tracker laufen weiter. Thread-sicher.
 */
void ferraris_logger_pause(ferraris_logger_t *l, uint32_t ms);

/** CSV-Task starten: ab jetzt alle interval_ms eine Zeile auf UART0. */
esp_err_t ferraris_logger_start(ferraris_logger_t *l);

/** Task stoppen und Logger freigeben. */
void ferraris_logger_destroy(ferraris_logger_t *l);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_LOGGER_H
