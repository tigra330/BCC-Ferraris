/**
 * @file ferraris_day.h
 * @brief Tageswerte (Bezug/Einspeisung seit 0:00 Uhr) und Vortag (pure C, PC-testbar).
 *
 * Gemerkt wird der Gesamtstand (Quadratur-Schritte) zu Tagesbeginn; der
 * Tageswert ist die Differenz zum aktuellen Gesamtstand. Der Tag ist als
 * JJJJMMTT kodiert (lokale Zeit, kommt per NTP). Solange keine Uhrzeit
 * bekannt ist (day_id 0), zaehlt der Tag ab dem Start; die erste gueltige
 * Uhrzeit ordnet ihn dem aktuellen Datum zu.
 *
 * Nur aus einem Task schreiben (loop()), nie aus dem Hotpath. Alle Felder
 * int32 -> direkt als NVS-Blob speicherbar (ferraris_counter_store).
 */

#ifndef FERRARIS_DAY_H
#define FERRARIS_DAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t day_id;     /**< laufender Tag JJJJMMTT, 0 = Datum noch unbekannt */
    int32_t base_fwd;   /**< Gesamtschritte Bezug zu Tagesbeginn */
    int32_t base_bwd;   /**< Gesamtschritte Einspeisung zu Tagesbeginn */
    int32_t prev_id;    /**< letzter abgeschlossener Tag JJJJMMTT, 0 = keiner */
    int32_t prev_fwd;   /**< Schritte Bezug an jenem Tag */
    int32_t prev_bwd;   /**< Schritte Einspeisung an jenem Tag */
} ferraris_day_t;

/** JJJJMMTT aus Jahr (z. B. 2026), Monat 1..12, Tag 1..31. */
int32_t ferraris_day_id(int year, int month, int mday);

/** Neuer Tag ohne Vorgeschichte: zaehlt ab dem aktuellen Gesamtstand. */
void ferraris_day_start(ferraris_day_t *d, int32_t fwd, int32_t bwd);

/**
 * @brief Datum pruefen, bei Tageswechsel den Tag abschliessen.
 *
 * @param today JJJJMMTT (lokal), <= 0 = Uhrzeit unbekannt -> keine Aenderung
 * @return true, wenn sich der Zustand geaendert hat (-> speichern)
 */
bool ferraris_day_update(ferraris_day_t *d, int32_t today, int32_t fwd, int32_t bwd);

/** Schritte des laufenden Tages (nie negativ). */
void ferraris_day_get(const ferraris_day_t *d, int32_t fwd, int32_t bwd,
                      int32_t *day_fwd, int32_t *day_bwd);

/**
 * @brief Gesamtstand wurde gesetzt (Zaehlerstand eingegeben / zurueckgesetzt):
 *        Basis mitverschieben, damit der Tageswert unveraendert bleibt.
 */
void ferraris_day_shift(ferraris_day_t *d, int32_t delta_fwd, int32_t delta_bwd);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_DAY_H
