/**
 * @file ferraris_hwdetect.c
 * @brief Hardware-Selbsttest der Ferraris-Lichtschranken (ESP-only).
 *
 * Pro Sensor-Pin:
 *  - GPIO32/33: Pull-Up/Pull-Down/Float + 32-Sample-ADC -> ferraris_hw_classify().
 *  - GPIO34..39 (nur Eingang, OHNE interne Pulls): Rausch-Scan ueber 64
 *    Samples -> ferraris_hw_classify_noise().
 */

#include "ferraris_hwdetect.h"
#include "ferraris_log.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hwdetect";

typedef struct {
    ferraris_hw_status_t status;
    int dig_up, dig_dn;          // digitale Pegel bei Pull-Up / Pull-Down
    int adc_np, adc_up, adc_dn;  // ADC-Mittel ohne / Pull-Up / Pull-Down
} pull_result_t;

static int avg_n(adc_oneshot_unit_handle_t h, adc_channel_t ch, int n)
{
    int32_t s = 0;
    int ok = 0, v;
    for (int i = 0; i < n; i++) {
        if (adc_oneshot_read(h, ch, &v) == ESP_OK) { s += v; ok++; }
    }
    return ok > 0 ? (int)(s / ok) : -1;
}

static pull_result_t test_with_pulls(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                     gpio_num_t pin)
{
    pull_result_t r = {0};
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);     vTaskDelay(pdMS_TO_TICKS(80));
    r.dig_up = gpio_get_level(pin);
    r.adc_up = avg_n(h, ch, 32);

    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);   vTaskDelay(pdMS_TO_TICKS(80));
    r.dig_dn = gpio_get_level(pin);
    r.adc_dn = avg_n(h, ch, 32);

    gpio_set_pull_mode(pin, GPIO_FLOATING);        vTaskDelay(pdMS_TO_TICKS(80));
    r.adc_np = avg_n(h, ch, 32);

    r.status = ferraris_hw_classify(r.dig_up, r.dig_dn, r.adc_np, r.adc_up, r.adc_dn);
    return r;
}

static ferraris_hw_status_t test_pin(adc_oneshot_unit_handle_t h, const char *label,
                                     int pin, adc_channel_t ch)
{
    ferraris_hw_status_t st;
    if (ferraris_gpio_has_pulls(pin)) {
        pull_result_t r = test_with_pulls(h, ch, (gpio_num_t)pin);
        FLOGI(TAG, "Sensor %s (GPIO%d/CH%d): dig[UP=%d DN=%d] ADC[NP=%d UP=%d DN=%d] diff=%d -> %s",
              label, pin, (int)ch, r.dig_up, r.dig_dn, r.adc_np, r.adc_up, r.adc_dn,
              r.adc_up - r.adc_dn, ferraris_hw_status_name(r.status));
        // Pulls wieder freigeben, damit der Sampler den Pin unbeeinflusst liest.
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
        st = r.status;
    } else {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        int32_t s = 0;
        int ok = 0, v, mn = 4095, mx = 0;
        for (int i = 0; i < 64; i++) {
            if (adc_oneshot_read(h, ch, &v) == ESP_OK) {
                s += v; ok++;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        }
        int mean   = ok > 0 ? (int)(s / ok) : -1;
        int spread = ok > 0 ? mx - mn : -1;
        st = ferraris_hw_classify_noise(mean, spread);
        FLOGI(TAG, "Sensor %s (GPIO%d/CH%d, ohne Pulls): ADC mean=%d spread=%d (min=%d max=%d) -> %s",
              label, pin, (int)ch, mean, spread, mn, mx, ferraris_hw_status_name(st));
    }
    FLOGI(TAG, "  -> %s", ferraris_hw_status_advice(st));
    return st;
}

int ferraris_hwdetect_run(const int *pins, const char *const *names, int n,
                          ferraris_hw_status_t *status)
{
    FLOGI(TAG, "=== Selbsttest Lichtschranken (%d Sensoren) ===", n);

    adc_oneshot_unit_handle_t h;
    adc_oneshot_unit_init_cfg_t ic = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&ic, &h) != ESP_OK) {
        FLOGE(TAG, "ADC-Unit konnte nicht angelegt werden - Selbsttest abgebrochen");
        if (status) {
            for (int i = 0; i < n; i++) status[i] = FERRARIS_HW_ERROR;
        }
        return 0;
    }
    adc_oneshot_chan_cfg_t cc = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (int c = (int)ADC_CHANNEL_0; c <= (int)ADC_CHANNEL_7; c++) {
        adc_oneshot_config_channel(h, (adc_channel_t)c, &cc);
    }

    int ok = 0;
    for (int i = 0; i < n; i++) {
        ferraris_hw_status_t st;
        int ch = ferraris_gpio_to_adc1_channel(pins[i]);
        if (ch < 0) {
            FLOGE(TAG, "Sensor %s: GPIO%d ist kein ADC1-Pin (32..39)", names[i], pins[i]);
            st = FERRARIS_HW_ERROR;
        } else {
            st = test_pin(h, names[i], pins[i], (adc_channel_t)ch);
        }
        if (status) status[i] = st;
        if (st == FERRARIS_HW_OK) ok++;
    }
    adc_oneshot_del_unit(h);

    if (ok == n) {
        FLOGI(TAG, "Gesamt: %d/%d Lichtschranken OK - bereit", ok, n);
    } else {
        FLOGW(TAG, "WARNUNG: nur %d/%d Lichtschranken OK - Verdrahtung pruefen.", ok, n);
        FLOGW(TAG, "  (Boot wird nicht blockiert - Zaehlung laeuft, Werte aber evtl. unzuverlaessig.)");
    }
    return ok;
}
