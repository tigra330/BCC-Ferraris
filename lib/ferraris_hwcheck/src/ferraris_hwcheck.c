/**
 * @file ferraris_hwcheck.c
 * @brief Klassifikation des Lichtschranken-Selbsttests (pure C).
 */

#include "ferraris_hwcheck.h"

ferraris_hw_status_t ferraris_hw_classify(int dig_up, int dig_dn,
                                          int adc_np, int adc_up, int adc_dn)
{
    (void)adc_np;
    const int diff = adc_up - adc_dn;

    if (adc_up < 0 || adc_dn < 0)                       return FERRARIS_HW_ERROR;
    if (dig_up == 0 && dig_dn == 0 && adc_up < 300)     return FERRARIS_HW_SHORT_GND;
    if (dig_up == 1 && dig_dn == 1 && adc_dn > 3850)    return FERRARIS_HW_SHORT_VCC;
    if (diff > 3800)                                    return FERRARIS_HW_OPEN;
    if (diff < 1500)                                    return FERRARIS_HW_OK;
    return FERRARIS_HW_WEAK;
}

ferraris_hw_status_t ferraris_hw_classify_noise(int mean, int spread)
{
    if (mean < 0 || spread < 0)               return FERRARIS_HW_ERROR;
    if (spread >= FERRARIS_HW_SPREAD_STABLE)  return FERRARIS_HW_OPEN;
    if (mean < 100)                           return FERRARIS_HW_SHORT_GND;
    if (mean > 4000)                          return FERRARIS_HW_SHORT_VCC;
    return FERRARIS_HW_OK;
}

const char *ferraris_hw_status_name(ferraris_hw_status_t s)
{
    switch (s) {
        case FERRARIS_HW_OK:        return "OK";
        case FERRARIS_HW_OPEN:      return "OFFEN";
        case FERRARIS_HW_SHORT_GND: return "KURZSCHLUSS-GND";
        case FERRARIS_HW_SHORT_VCC: return "KURZSCHLUSS-VCC";
        case FERRARIS_HW_WEAK:      return "UNSICHER";
        case FERRARIS_HW_ERROR:     return "MESSFEHLER";
    }
    return "?";
}

const char *ferraris_hw_status_advice(ferraris_hw_status_t s)
{
    switch (s) {
        case FERRARIS_HW_OK:        return "Lichtschranke erkannt - alles in Ordnung";
        case FERRARIS_HW_OPEN:      return "NICHTS angeschlossen - Lichtschranke fehlt oder Leitung unterbrochen. Phototransistor + Vorwiderstand pruefen.";
        case FERRARIS_HW_SHORT_GND: return "Kurzschluss nach GND - Verbinder/Leitung pruefen (Signal schlaegt nach Masse).";
        case FERRARIS_HW_SHORT_VCC: return "Kurzschluss nach VCC - Verbinder/Leitung pruefen (Signal schlaegt nach 3V3).";
        case FERRARIS_HW_WEAK:      return "unsicher - hochohmig, evtl. lose Leitung oder defekter Pull. Kontakt pruefen.";
        case FERRARIS_HW_ERROR:     return "Messfehler - ADC nicht lesbar, Hardware/Treiber pruefen.";
    }
    return "?";
}

int ferraris_gpio_to_adc1_channel(int gpio)
{
    static const int map[8] = {
        4, 5, 6, 7,   // GPIO32..35
        0, 1, 2, 3,   // GPIO36..39
    };
    if (gpio < 32 || gpio > 39) return -1;
    return map[gpio - 32];
}

bool ferraris_gpio_has_pulls(int gpio)
{
    return gpio == 32 || gpio == 33;
}
