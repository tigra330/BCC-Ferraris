/**
 * @file ferraris_hwcheck.h
 * @brief Klassifikation des Lichtschranken-Selbsttests (pure C, PC-testbar).
 *
 * Die Messung (Pulls umschalten, ADC lesen) liegt in ferraris_hwdetect
 * (ESP-only); hier nur die Entscheidungsregeln, an ESP32-Hardware validiert:
 *
 *   - GPIO32/33 (interne Pulls ~45 kOhm): floating -> Pulls erreichen die
 *     Schienen (diff > 3800); Last (Phototransistor + Vorwiderstand) -> Pulls
 *     kommen nicht durch (diff < 1500); Kurzschluss -> dig-Kombination.
 *   - GPIO34..39 (ohne Pulls): Rausch-Scan, treibende Last = stabiler Wert,
 *     offener Pin = zittriger Wert.
 */

#ifndef FERRARIS_HWCHECK_H
#define FERRARIS_HWCHECK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FERRARIS_HW_OK = 0,      /**< Last erkannt (Lichtschranke angeschlossen) */
    FERRARIS_HW_OPEN,        /**< nichts angeschlossen (offen) */
    FERRARIS_HW_SHORT_GND,   /**< Kurzschluss nach GND */
    FERRARIS_HW_SHORT_VCC,   /**< Kurzschluss nach VCC */
    FERRARIS_HW_WEAK,        /**< unsicher / lose Leitung */
    FERRARIS_HW_ERROR        /**< Messfehler (ADC nicht lesbar) */
} ferraris_hw_status_t;

/** Spread-Grenze des Rausch-Scans: darunter gilt der Wert als stabil (Last). */
#define FERRARIS_HW_SPREAD_STABLE 120

/**
 * @brief Klassifikation fuer Pins mit internen Pulls (GPIO32/33).
 *
 * ADC ist primaer (32-Sample-Mittel, robust gegen 50-Hz-Brummen); die
 * einzelnen dig-Reads sind auf hochohmigen Knoten unsicher, deshalb werden
 * Kurzschluesse nur bei passendem ADC-Pegel bestaetigt. adc_* < 0 = Messfehler.
 */
ferraris_hw_status_t ferraris_hw_classify(int dig_up, int dig_dn,
                                          int adc_np, int adc_up, int adc_dn);

/**
 * @brief Klassifikation fuer Pins ohne Pulls (GPIO34..39) aus dem Rausch-Scan.
 *
 * @param mean   ADC-Mittelwert (< 0 = Messfehler)
 * @param spread max - min ueber den Scan (< 0 = Messfehler)
 */
ferraris_hw_status_t ferraris_hw_classify_noise(int mean, int spread);

/** Kurzname, z. B. "OK", "OFFEN", "KURZSCHLUSS-GND". */
const char *ferraris_hw_status_name(ferraris_hw_status_t s);

/** Deutsche Handlungsempfehlung zum Status. */
const char *ferraris_hw_status_advice(ferraris_hw_status_t s);

/**
 * @brief ADC1-Kanalnummer eines ESP32-GPIO (32..39).
 * @return Kanal 0..7 oder -1, wenn der GPIO kein ADC1-Pin ist.
 */
int ferraris_gpio_to_adc1_channel(int gpio);

/** true, wenn der GPIO interne Pull-Widerstaende hat (GPIO32/33 unter den ADC1-Pins). */
bool ferraris_gpio_has_pulls(int gpio);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_HWCHECK_H
