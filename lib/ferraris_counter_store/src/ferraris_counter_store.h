/**
 * @file ferraris_counter_store.h
 * @brief Zaehlerstand (Quadratur-Schritte Bezug/Einspeisung) im NVS,
 *        Namespace "ferrcnt", ein Blob "cnt" (beide Werte zusammen konsistent).
 *
 * Gespeichert werden Schritte, nicht kWh: ein spaeter geaendertes k rechnet
 * auch den gespeicherten Stand um (wie im laufenden Betrieb).
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

#ifdef __cplusplus
extern "C" {
#endif

/** @return false, wenn nichts (Gueltiges) gespeichert ist; dann bleiben die Ausgaben unveraendert. */
bool ferraris_counter_load(int32_t *forward, int32_t *backward);

/** @return false bei NVS-Fehler. */
bool ferraris_counter_save(int32_t forward, int32_t backward);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_COUNTER_STORE_H
