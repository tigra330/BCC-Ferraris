/**
 * @file ferraris_power.h
 * @brief Momentanleistung aus den Quadratur-Schritten (pure C, PC-testbar).
 *
 * Eine Umdrehung = 4 Quadratur-Schritte. Die Schritte liegen NICHT
 * gleichmaessig (die Marke passiert A und B kurz hintereinander, dann folgt
 * eine lange Pause), aber der Abstand zwischen Schritt n und Schritt n-4 ist
 * immer genau eine Umdrehung. Daraus:
 *
 *     P [W] = (3.6e6 J / k) / T_umdr [s] = 3.6e9 / (k * T_umdr [ms])
 *
 * Ohne neue Schritte faellt die Anzeige ab: ist seit dem letzten Schritt
 * schon laenger als eine Periode vergangen, dreht die Scheibe hoechstens so
 * schnell -> obere Schranke 3.6e9 / (k * seit_letztem_schritt).
 *
 * Richtungswechsel verwirft die Historie (neue Periode erst nach 4 Schritten
 * in der neuen Richtung). Vorzeichen: + = Bezug, - = Einspeisung.
 *
 * Thread-Safety wie im restlichen Projekt: ein Writer (DMA-Callback,
 * ferraris_power_step), ein Reader (ferraris_power_watts). Die Reader-Felder
 * sind 32-bit und einzeln atomar (Last-Good-Value, kein Mutex im Hotpath).
 */

#ifndef FERRARIS_POWER_H
#define FERRARIS_POWER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Nur Writer
    uint32_t step_ms[4];          /**< Zeitstempel der letzten 4 Schritte (Ring) */
    uint8_t  idx;                 /**< naechster Schreibplatz */
    uint8_t  n;                   /**< gueltige Eintraege (0..4) */
    int8_t   dir;                 /**< Richtung der Eintraege im Ring */

    // Writer -> Reader
    volatile uint32_t last_step_ms;  /**< Zeitpunkt des letzten Schritts */
    volatile int32_t  period_ms;     /**< letzte volle Umdrehung, Vorzeichen = Richtung; 0 = unbekannt */
} ferraris_power_t;

void ferraris_power_init(ferraris_power_t *p);

/**
 * @brief Einen Quadratur-Schritt melden (Hotpath, keine Allokation).
 *
 * @param dir    +1 Bezug, -1 Einspeisung, 0 wird ignoriert
 * @param now_ms monotone Zeit in ms (Ueberlauf nach 49 Tagen ist unkritisch,
 *               es werden nur Differenzen gebildet)
 */
void ferraris_power_step(ferraris_power_t *p, int dir, uint32_t now_ms);

/**
 * @brief Momentanleistung in Watt (+ Bezug, - Einspeisung, 0 = unbekannt).
 *
 * @param k_rev_per_kwh Zaehlerkonstante
 * @param now_ms        aktuelle Zeit (gleiche Basis wie step)
 * @param timeout_ms    ohne Schritt laenger als dies -> 0 (Stillstand)
 */
float ferraris_power_watts(const ferraris_power_t *p, int k_rev_per_kwh,
                           uint32_t now_ms, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_POWER_H
