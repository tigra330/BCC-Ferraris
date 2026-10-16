/**
 * @file ferraris_data.h
 * @brief Gemeinsamer Zaehlerzustand: Quadratur-Zaehler, Energie, Leistung.
 *
 * Pure C (keine ESP-Abhaengigkeit, PC-testbar). Single-Writer (DMA-Callback
 * ruft add_quad), beliebig viele Reader (Print-Task, HTTP, MQTT). Alle
 * Reader-Felder sind 32-bit und einzeln atomar auf Xtensa — bewusstes
 * Last-Good-Value ohne Mutex im Hotpath.
 *
 *   energy_kwh = (fwd - bwd) / (4 * k)
 *   import_kwh = fwd / (4 * k),  export_kwh = bwd / (4 * k)
 *   power      = siehe ferraris_power.h (letzte volle Umdrehung)
 */

#ifndef FERRARIS_DATA_H
#define FERRARIS_DATA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ohne Quadratur-Schritt laenger als dies -> Leistung 0 (Stillstand).
 *  1 h: bei k=150 entspricht das < 7 W. */
#define FERRARIS_DATA_POWER_TIMEOUT_MS (3600UL * 1000UL)

typedef struct ferraris_data ferraris_data_t;

/**
 * @param revolutions_per_kwh Zaehlerkonstante k (> 0)
 * @return Handle oder NULL (Speicher / k ungueltig)
 */
ferraris_data_t *ferraris_data_create(int32_t revolutions_per_kwh);

void ferraris_data_destroy(ferraris_data_t *data);

/**
 * @brief Einen Quadratur-Schritt verbuchen (Hotpath: keine Sperren, kein I/O).
 *
 * @param direction +1 Bezug, -1 Einspeisung, 0 ignoriert
 * @param now_ms    monotone Zeit in ms
 */
void ferraris_data_add_quad(ferraris_data_t *data, int8_t direction, uint32_t now_ms);

/** Quadratur-Schritte lesen (4 Schritte = 1 Umdrehung). */
void ferraris_data_get_quad(const ferraris_data_t *data, int32_t *forward, int32_t *backward);

/**
 * @brief Zaehlerstand setzen: gespeicherten Stand nach dem Start uebernehmen
 *        oder zuruecksetzen (0, 0). Nicht aus dem Hotpath aufrufen.
 *
 * Die Hotpath-Zaehler bleiben unangetastet; gesetzt wird ein Basiswert
 * (Wert = Hotpath-Zaehler + Basis), den nur der Aufrufer schreibt. Ein
 * gleichzeitiger Schritt im DMA-Callback geht dabei nicht verloren. Leser
 * koennen waehrend des Setzens einmal einen Mischwert sehen (zwei 32-bit-Felder).
 */
void ferraris_data_set_quad(ferraris_data_t *data, int32_t forward, int32_t backward);

/** Netto-Energie in kWh (Bezug - Einspeisung). */
float ferraris_data_get_energy_kwh(const ferraris_data_t *data);

/** Bezug in kWh. */
float ferraris_data_get_import_kwh(const ferraris_data_t *data);

/** Einspeisung in kWh. */
float ferraris_data_get_export_kwh(const ferraris_data_t *data);

/** Momentanleistung in W (+ Bezug, - Einspeisung, 0 = unbekannt/Stillstand). */
float ferraris_data_get_power_watts(const ferraris_data_t *data, uint32_t now_ms);

int32_t ferraris_data_get_revs_per_kwh(const ferraris_data_t *data);

/**
 * @brief Zaehlerkonstante zur Laufzeit aendern (Webseite /config).
 *
 * Die Zaehler sind Quadratur-Schritte, kWh werden bei jedem Lesen aus k
 * berechnet -> auch bisherige Werte erscheinen danach mit dem neuen k.
 * Ein 32-bit-Schreibzugriff, sicher gegenueber parallelen Readern.
 * @return false bei k <= 0 (unveraendert)
 */
bool ferraris_data_set_revs_per_kwh(ferraris_data_t *data, int32_t revolutions_per_kwh);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_DATA_H
