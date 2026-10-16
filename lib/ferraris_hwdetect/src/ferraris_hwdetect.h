/**
 * @file ferraris_hwdetect.h
 * @brief Hardware-Selbsttest der Ferraris-Lichtschranken beim Boot (ESP-only).
 *
 * Legt eine eigene adc_oneshot-Unit fuer ADC1 an, testet jeden Pin und gibt
 * die Unit wieder frei, damit der DMA-Sampler ADC1 danach uebernehmen kann.
 * Die Entscheidungsregeln liegen in ferraris_hwcheck (pure C).
 *
 * Blockiert den Boot nie — bei Fehlern wird nur laut gewarnt.
 */

#ifndef FERRARIS_HWDETECT_H
#define FERRARIS_HWDETECT_H

#include "ferraris_hwcheck.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Alle Lichtschranken-Pins testen und das Ergebnis loggen.
 *
 * @param pins    ADC1-GPIOs (32..39)
 * @param names   Anzeigenamen je Pin (z. B. "Q1")
 * @param n       Anzahl Pins
 * @param status  optional (NULL erlaubt): Ergebnis je Pin
 * @return Anzahl Sensoren mit Status OK.
 */
int ferraris_hwdetect_run(const int *pins, const char *const *names, int n,
                          ferraris_hw_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_HWDETECT_H
