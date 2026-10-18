/**
 * @file ferraris_counter_store.h
 * @brief Zaehlerstand (Quadratur-Schritte Bezug/Einspeisung) und Tageswerte im
 *        NVS, Namespace "ferrcnt", ein Blob "cnt" (alle Werte zusammen konsistent).
 *
 * Gespeichert werden Schritte, nicht kWh: ein spaeter geaendertes k rechnet
 * auch den gespeicherten Stand um (wie im laufenden Betrieb).
 *
 * Blob-Version 1 (nur Gesamtstand) wird weiter gelesen; die Tageswerte
 * beginnen dann beim geladenen Stand.
 *
 * Nur aus setup() bzw. dem loop()-Task aufrufen (Flash-Writes nie im ADC-Pfad).
 * Verschleiss: ein Blob belegt 3 NVS-Eintraege; bei 5 min Intervall und
 * Dauerlast ~860 Eintraege/Tag -> jede der 6 NVS-Seiten wird gut einmal am Tag
 * geloescht, bei 100k Zyklen weit jenseits der Lebensdauer des Geraets.
 */

#ifndef FERRARIS_COUNTER_STORE_H
#define FERRARIS_COUNTER_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "ferraris_day.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t       forward;    /**< Gesamtschritte Bezug */
    int32_t       backward;   /**< Gesamtschritte Einspeisung */
    ferraris_day_t day;       /**< Tageswerte */
    bool          day_valid;  /**< day geladen (false: altes Format, day selbst starten) */
} ferraris_counter_state_t;

/** @return false, wenn nichts (Gueltiges) gespeichert ist; dann bleibt *st unveraendert. */
bool ferraris_counter_load(ferraris_counter_state_t *st);

/** @return false bei NVS-Fehler. */
bool ferraris_counter_save(const ferraris_counter_state_t *st);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_COUNTER_STORE_H
