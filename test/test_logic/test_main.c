/**
 * @file test_main.c
 * @brief Unit-Tests der pure-C-Logik (pio test -e native).
 */

#include <string.h>
#include <unity.h>

#include "autotune_collector.h"
#include "ferraris_calib.h"
#include "ferraris_data.h"
#include "ferraris_health.h"
#include "ferraris_hwcheck.h"
#include "ferraris_logger_logic.h"
#include "ferraris_netutil.h"
#include "ferraris_power.h"
#include "ferraris_settings.h"
#include "ferraris_status.h"
#include "quadrature_decoder.h"
#include "schmitt_trigger_autotune.h"
#include "schmitt_trigger_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ─── Quadratur ───────────────────────────────────────────────────────────────

// Marke laeuft A -> B: (0,0) (1,0) (1,1) (0,1) (0,0) = 4 Schritte vorwaerts.
static const bool FWD_A[] = { 1, 1, 0, 0 };
static const bool FWD_B[] = { 0, 1, 1, 0 };

static void test_quad_forward_revolution(void)
{
    quad_decoder_t *d = quad_decoder_create();
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(QUAD_DIR_FORWARD, quad_decoder_process(d, FWD_A[i], FWD_B[i]));
    }
    int32_t f, b;
    quad_decoder_get_counts(d, &f, &b);
    TEST_ASSERT_EQUAL(4, f);
    TEST_ASSERT_EQUAL(0, b);
    quad_decoder_destroy(d);
}

static void test_quad_backward_revolution(void)
{
    quad_decoder_t *d = quad_decoder_create();
    for (int i = 3; i >= 0; i--) {
        int j = (i + 3) & 3;   // rueckwaerts: S0 -> S3 -> S2 -> S1 -> S0
        TEST_ASSERT_EQUAL(QUAD_DIR_BACKWARD, quad_decoder_process(d, FWD_A[j], FWD_B[j]));
    }
    int32_t f, b;
    quad_decoder_get_counts(d, &f, &b);
    TEST_ASSERT_EQUAL(0, f);
    TEST_ASSERT_EQUAL(4, b);
    quad_decoder_destroy(d);
}

static void test_quad_invalid_jump_ignored(void)
{
    quad_decoder_t *d = quad_decoder_create();
    TEST_ASSERT_EQUAL(QUAD_DIR_NONE, quad_decoder_process(d, true, true));   // S0 -> S2
    TEST_ASSERT_EQUAL(QUAD_DIR_NONE, quad_decoder_process(d, true, true));   // keine Aenderung
    int32_t f, b;
    quad_decoder_get_counts(d, &f, &b);
    TEST_ASSERT_EQUAL(0, f + b);
    quad_decoder_destroy(d);
}

// ─── Schmitt-Trigger ─────────────────────────────────────────────────────────

static void test_schmitt_hysteresis(void)
{
    schmitt_logic_t s;
    schmitt_logic_init(&s, 3450, 3300);
    TEST_ASSERT_FALSE(schmitt_logic_process(&s, 3400));    // tote Zone
    TEST_ASSERT_TRUE(schmitt_logic_process(&s, 3450));     // steigend bei >= HIGH
    TEST_ASSERT_TRUE(schmitt_logic_get_state(&s));
    TEST_ASSERT_FALSE(schmitt_logic_process(&s, 3301));    // tote Zone haelt HIGH
    TEST_ASSERT_TRUE(schmitt_logic_process(&s, 3300));     // fallend bei <= LOW
    TEST_ASSERT_FALSE(schmitt_logic_get_state(&s));
}

static void test_schmitt_set_thresholds_keeps_state(void)
{
    schmitt_logic_t s;
    schmitt_logic_init(&s, 3450, 3300);
    schmitt_logic_process(&s, 3700);
    schmitt_logic_set_thresholds(&s, 3800, 3600);
    TEST_ASSERT_TRUE(schmitt_logic_get_state(&s));
}

// ─── Auto-Tune + Collector ───────────────────────────────────────────────────

#define N_BI 1000
static int16_t s_bimodal[N_BI];

static void fill_bimodal(void)
{
    // 90 % Ruhe-Basis um 2700, 10 % Marke um 3680 (Erfahrungswerte RML).
    for (int i = 0; i < N_BI; i++) {
        s_bimodal[i] = (int16_t)((i % 10 == 0) ? 3680 + (i % 7) * 5 : 2700 + (i % 11) * 5);
    }
}

static void test_autotune_bimodal(void)
{
    fill_bimodal();
    schmitt_autotune_result_t r = schmitt_autotune_analyze(s_bimodal, N_BI, NULL);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_GREATER_THAN(r.threshold_low, r.threshold_high);
    TEST_ASSERT_GREATER_THAN(2750, r.threshold_low);
    TEST_ASSERT_LESS_THAN(3680, r.threshold_high);
}

static void test_autotune_unimodal_fails(void)
{
    int16_t flat[500];
    for (int i = 0; i < 500; i++) flat[i] = (int16_t)(2700 + (i % 11) * 5);
    schmitt_autotune_result_t r = schmitt_autotune_analyze(flat, 500, NULL);
    TEST_ASSERT_FALSE(r.valid);
}

static void test_collector_populations_and_timing(void)
{
    fill_bimodal();
    TEST_ASSERT_TRUE(autotune_collector_both_populations(s_bimodal, N_BI, 6, 0.05f));

    autotune_collector_t c;
    autotune_collector_init(&c, N_BI, 1, 2000, 30000, 6, 0.05f);
    TEST_ASSERT_NOT_NULL(c.buf_a);
    for (int i = 0; i < N_BI; i++) {
        autotune_collector_add(&c, s_bimodal[i], s_bimodal[i], (uint32_t)i);
    }
    TEST_ASSERT_FALSE(autotune_collector_ready(&c, 1999));   // Mindestzeit
    TEST_ASSERT_TRUE(autotune_collector_ready(&c, 2000));
    autotune_collector_destroy(&c);

    // Unimodal: erst nach der Maximalzeit bereit.
    autotune_collector_init(&c, 100, 1, 2000, 30000, 6, 0.05f);
    for (int i = 0; i < 100; i++) autotune_collector_add(&c, 2700, 2700, 0);
    TEST_ASSERT_FALSE(autotune_collector_ready(&c, 29999));
    TEST_ASSERT_TRUE(autotune_collector_ready(&c, 30000));
    autotune_collector_destroy(&c);
}

// ─── Logger-Logik ────────────────────────────────────────────────────────────

static void test_logger_watts(void)
{
    // k=150: 1 Umdrehung (4 Schritte) in 24 s = 1/150 kWh / 24 s = 1000 W.
    TEST_ASSERT_EQUAL(1000, logger_watts(4, 150, 24000));
    TEST_ASSERT_EQUAL(-1000, logger_watts(-4, 150, 24000));
    TEST_ASSERT_EQUAL(0, logger_watts(4, 150, 0));
    TEST_ASSERT_EQUAL(0, logger_watts(4, 0, 1000));
}

static void test_logger_quad_and_marks(void)
{
    logger_quad_t q;
    logger_quad_init(&q);
    logger_quad_feed(&q, false, false);
    for (int i = 0; i < 4; i++) logger_quad_feed(&q, FWD_A[i], FWD_B[i]);
    TEST_ASSERT_EQUAL(4, q.net_steps);

    logger_ch_t c;
    logger_ch_init(&c, 2700);
    TEST_ASSERT_EQUAL_FLOAT(LOGGER_CALIB_MIN_RANGE, c.run_max - c.run_min);
    logger_ch_feed(&c, 3700, true);
    logger_ch_feed(&c, 3700, true);
    logger_ch_feed(&c, 2700, false);
    logger_ch_feed(&c, 3700, true);
    TEST_ASSERT_EQUAL(2, c.mark_count);   // zwei steigende Flanken
    TEST_ASSERT_EQUAL(3700, c.last_raw);
}

// ─── Leistung ────────────────────────────────────────────────────────────────

static void test_power_needs_full_revolution(void)
{
    ferraris_power_t p;
    ferraris_power_init(&p);
    for (int i = 0; i < 4; i++) ferraris_power_step(&p, 1, 1000u * (uint32_t)i);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ferraris_power_watts(&p, 150, 3000, 0));
}

static void test_power_uneven_steps(void)
{
    // Marke passiert A/B schnell (4 Schritte in 100 ms), dann 23.9 s Pause:
    // Umdrehung = 24 s -> 1000 W bei k=150.
    ferraris_power_t p;
    ferraris_power_init(&p);
    const uint32_t off[4] = { 0, 30, 60, 100 };
    for (int rev = 0; rev < 3; rev++) {
        for (int i = 0; i < 4; i++) ferraris_power_step(&p, 1, (uint32_t)rev * 24000u + off[i]);
    }
    uint32_t last = 2u * 24000u + 100u;
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, ferraris_power_watts(&p, 150, last + 10000, 0));
    // Laenger als eine Periode kein Schritt -> obere Schranke faellt ab.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 500.0f, ferraris_power_watts(&p, 150, last + 48000, 0));
    // Timeout -> 0
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ferraris_power_watts(&p, 150, last + 60001, 60000));
}

static void test_power_direction_change(void)
{
    ferraris_power_t p;
    ferraris_power_init(&p);
    for (int i = 0; i < 5; i++) ferraris_power_step(&p, 1, 6000u * (uint32_t)i);
    TEST_ASSERT_GREATER_THAN(0, (int)ferraris_power_watts(&p, 150, 24000, 0));
    ferraris_power_step(&p, -1, 25000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ferraris_power_watts(&p, 150, 25000, 0));
    for (int i = 1; i <= 4; i++) ferraris_power_step(&p, -1, 25000u + 6000u * (uint32_t)i);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -1000.0f, ferraris_power_watts(&p, 150, 49000, 0));
}

static void test_power_timer_wrap(void)
{
    ferraris_power_t p;
    ferraris_power_init(&p);
    uint32_t t0 = 0xFFFFFFFFu - 30000u;
    for (int i = 0; i < 5; i++) ferraris_power_step(&p, 1, t0 + 6000u * (uint32_t)i);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, ferraris_power_watts(&p, 150, t0 + 24000u, 0));
}

// ─── Zaehlerdaten ────────────────────────────────────────────────────────────

static void test_data_energy(void)
{
    TEST_ASSERT_NULL(ferraris_data_create(0));
    ferraris_data_t *d = ferraris_data_create(150);
    for (int i = 0; i < 600; i++) ferraris_data_add_quad(d, 1, (uint32_t)i);   // 150 Umdr.
    for (int i = 0; i < 60; i++)  ferraris_data_add_quad(d, -1, (uint32_t)i);  // 15 Umdr.
    ferraris_data_add_quad(d, 0, 0);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, ferraris_data_get_import_kwh(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, ferraris_data_get_export_kwh(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.9f, ferraris_data_get_energy_kwh(d));
    int32_t f, b;
    ferraris_data_get_quad(d, &f, &b);
    TEST_ASSERT_EQUAL(600, f);
    TEST_ASSERT_EQUAL(60, b);
    ferraris_data_destroy(d);
}

static void test_data_set_quad(void)
{
    ferraris_data_t *d = ferraris_data_create(150);
    for (int i = 0; i < 10; i++) ferraris_data_add_quad(d, 1, (uint32_t)i);
    ferraris_data_add_quad(d, -1, 10);
    // Gespeicherten Stand uebernehmen: weitere Schritte zaehlen darauf weiter.
    ferraris_data_set_quad(d, 6000, 300);
    ferraris_data_add_quad(d, 1, 11);
    ferraris_data_add_quad(d, -1, 12);
    int32_t f, b;
    ferraris_data_get_quad(d, &f, &b);
    TEST_ASSERT_EQUAL(6001, f);
    TEST_ASSERT_EQUAL(301, b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6001.0f / 600.0f, ferraris_data_get_import_kwh(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 301.0f / 600.0f, ferraris_data_get_export_kwh(d));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5700.0f / 600.0f, ferraris_data_get_energy_kwh(d));
    // Zuruecksetzen
    ferraris_data_set_quad(d, 0, 0);
    ferraris_data_get_quad(d, &f, &b);
    TEST_ASSERT_EQUAL(0, f);
    TEST_ASSERT_EQUAL(0, b);
    ferraris_data_add_quad(d, 1, 13);
    ferraris_data_get_quad(d, &f, &b);
    TEST_ASSERT_EQUAL(1, f);
    ferraris_data_set_quad(NULL, 1, 1);   // kein Absturz
    ferraris_data_destroy(d);
}

// ─── HW-Klassifikation ───────────────────────────────────────────────────────

static void test_hw_classify(void)
{
    TEST_ASSERT_EQUAL(FERRARIS_HW_OPEN,      ferraris_hw_classify(1, 0, 2000, 4095, 0));
    TEST_ASSERT_EQUAL(FERRARIS_HW_OK,        ferraris_hw_classify(1, 0, 2700, 2900, 2500));
    TEST_ASSERT_EQUAL(FERRARIS_HW_SHORT_GND, ferraris_hw_classify(0, 0, 0, 10, 0));
    TEST_ASSERT_EQUAL(FERRARIS_HW_SHORT_VCC, ferraris_hw_classify(1, 1, 4095, 4095, 4000));
    TEST_ASSERT_EQUAL(FERRARIS_HW_WEAK,      ferraris_hw_classify(1, 0, 2000, 3500, 1000));
    TEST_ASSERT_EQUAL(FERRARIS_HW_ERROR,     ferraris_hw_classify(0, 0, 0, -1, 0));

    TEST_ASSERT_EQUAL(FERRARIS_HW_OK,    ferraris_hw_classify_noise(2700, 30));
    TEST_ASSERT_EQUAL(FERRARIS_HW_OPEN,  ferraris_hw_classify_noise(1500, 900));
    TEST_ASSERT_EQUAL(FERRARIS_HW_ERROR, ferraris_hw_classify_noise(-1, -1));

    TEST_ASSERT_EQUAL(4, ferraris_gpio_to_adc1_channel(32));
    TEST_ASSERT_EQUAL(0, ferraris_gpio_to_adc1_channel(36));
    TEST_ASSERT_EQUAL(3, ferraris_gpio_to_adc1_channel(39));
    TEST_ASSERT_EQUAL(-1, ferraris_gpio_to_adc1_channel(25));
    TEST_ASSERT_TRUE(ferraris_gpio_has_pulls(33));
    TEST_ASSERT_FALSE(ferraris_gpio_has_pulls(34));
}

// ─── Status-JSON ─────────────────────────────────────────────────────────────

static void test_status_json(void)
{
    ferraris_status_t st;
    memset(&st, 0, sizeof(st));
    st.uptime_s = 12;
    st.counting = true;
    st.revs_per_kwh = 150;
    st.steps_import = 8;
    st.import_kwh = 0.0133f;
    st.net_kwh = 0.0133f;
    st.power_w = -412.34f;
    st.saved_age_s = 42;
    st.threshold_high = 3450;
    st.threshold_low = 3300;
    st.count_a = "Q1";
    st.count_b = "Q2";
    st.n_sensors = 1;
    st.sensors[0] = (ferraris_sensor_status_t){ "Q\"1", 36, 2790, 2601, 3690, "OK", 2900, 2700 };
    strcpy(st.wifi_ip, "192.168.1.23");
    strcpy(st.wifi_ssid, "heim");
    st.wifi_connected = true;
    st.ap_active = true;
    strcpy(st.ap_ssid, "Ferraris-3A7F");
    strcpy(st.ap_ip, "192.168.4.1");
    st.ap_clients = 1;
    st.fw_version = "2.0.0";
    st.adc_sets_per_s = 10000;
    st.adc_restarts = 2;
    st.adc_errors = 5;
    st.adc_timeouts = 7;
    st.hostname = "ferraris";
    st.reset_reason = "Absturz";
    st.boot_count = 12;
    st.crash_count = 1;
    st.heap_free = 150000;
    st.heap_min = 120000;
    st.heap_block = 90000;
    st.wifi_reconnects = 3;
    st.mqtt_state = "verbunden";
    st.coredump = true;

    char buf[FERRARIS_STATUS_JSON_MAX];
    int n = ferraris_status_to_json(&st, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"power_w\":-412.3,\"saved_s\":42}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"count_pair\":[\"Q1\",\"Q2\"]"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Q\\\"1\""));   // Escape
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hw\":\"OK\",\"high\":2900,\"low\":2700}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"wifi\":{\"connected\":true,\"ssid\":\"heim\",\"rssi\":0,\"ip\":\"192.168.1.23\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ap\":{\"active\":true,\"ssid\":\"Ferraris-3A7F\",\"ip\":\"192.168.4.1\",\"clients\":1}}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"adc\":{\"sets_per_s\":10000,\"restarts\":2,\"errors\":5,\"timeouts\":7},"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"health\":{\"reset\":\"Absturz\",\"boots\":12,\"crashes\":1,"
                                     "\"safe_mode\":false,\"heap_free\":150000,\"heap_min\":120000,"
                                     "\"heap_block\":90000,\"wifi_reconnects\":3,\"mqtt\":\"verbunden\","
                                     "\"coredump\":true,\"ota\":\"ok\"},\"fw\":\"2.0.0\",\"host\":\"ferraris\"}"));
    TEST_ASSERT_EQUAL('}', buf[n - 1]);

    // Zu kleiner Puffer: Rueckgabe >= len, Puffer bleibt terminiert.
    char small[32];
    int m = ferraris_status_to_json(&st, small, sizeof(small));
    TEST_ASSERT_EQUAL(n, m);
    TEST_ASSERT_EQUAL(31, (int)strlen(small));
}

// ─── Netz-Hilfen ─────────────────────────────────────────────────────────────

static void test_wifi_credentials(void)
{
    const char *why = NULL;
    TEST_ASSERT_TRUE(ferraris_wifi_credentials_valid("heim", "12345678", &why));
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_TRUE(ferraris_wifi_credentials_valid("offen", "", NULL));
    TEST_ASSERT_FALSE(ferraris_wifi_credentials_valid("", "12345678", &why));
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_FALSE(ferraris_wifi_credentials_valid("heim", "1234567", NULL));
    TEST_ASSERT_FALSE(ferraris_wifi_credentials_valid("123456789012345678901234567890123", "", NULL));
    char pw64[65];
    memset(pw64, 'x', 64);
    pw64[64] = '\0';
    TEST_ASSERT_FALSE(ferraris_wifi_credentials_valid("heim", pw64, NULL));
}

static void test_json_string(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL(4, ferraris_json_string(buf, sizeof(buf), "ab"));
    TEST_ASSERT_EQUAL_STRING("\"ab\"", buf);
    ferraris_json_string(buf, sizeof(buf), "a\"b\\c\n");
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u000a\"", buf);
    ferraris_json_string(buf, sizeof(buf), "M\xc3\xbcller");   // UTF-8 unveraendert
    TEST_ASSERT_EQUAL_STRING("\"M\xc3\xbcller\"", buf);
    char small[4];
    TEST_ASSERT_EQUAL(8, ferraris_json_string(small, sizeof(small), "abcdef"));
    TEST_ASSERT_EQUAL_STRING("\"ab", small);
}

// ─── Einstellungen (/config) ─────────────────────────────────────────────────

static ferraris_settings_t settings_sample(void)
{
    ferraris_settings_t s;
    memset(&s, 0, sizeof(s));
    static const int pins[FERRARIS_SETTINGS_MAX_SENSORS] = { 36, 39, 34, 35, 32, 33, 37, 38 };
    s.sensor_count = 5;
    memcpy(s.sensor_gpio, pins, sizeof(pins));
    s.revs_per_kwh = 150;
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        s.threshold_high[i] = 3450;
        s.threshold_low[i] = 3300;
    }
    s.count_a = 1;
    s.count_b = 2;
    s.mqtt_interval_ms = 10000;
    strcpy(s.hostname, "ferraris");
    strcpy(s.mqtt_prefix, "ferraris");
    strcpy(s.ap_password, "ferraris-setup");
    return s;
}

static void test_settings_set_field(void)
{
    ferraris_settings_t s = settings_sample();
    const char *why = NULL;
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "k", "75", &why));
    TEST_ASSERT_EQUAL(75, s.revs_per_kwh);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "mqtt_interval_s", "30", NULL));
    TEST_ASSERT_EQUAL_UINT32(30000, s.mqtt_interval_ms);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "envelope_auto", "1", NULL));
    TEST_ASSERT_TRUE(s.envelope_auto);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "envelope_auto", "0", NULL));
    TEST_ASSERT_FALSE(s.envelope_auto);

    // Ungueltige Zahlen, Ueberlauf, zu lange Strings
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "k", "12a", &why));
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "k", "", NULL));
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "threshold_high", "99999999999", NULL));
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "high1", "abc", NULL));

    // Schwellen je Eingang ...
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "high2", "2900", NULL));
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "low2", "2700", NULL));
    TEST_ASSERT_EQUAL(2900, s.threshold_high[1]);
    TEST_ASSERT_EQUAL(2700, s.threshold_low[1]);
    TEST_ASSERT_EQUAL(3450, s.threshold_high[0]);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "high9", "1", NULL));   // unbekannt -> ignoriert
    // ... und die aeltere API setzt alle
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "threshold_low", "3000", NULL));
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) TEST_ASSERT_EQUAL(3000, s.threshold_low[i]);
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "mqtt_interval_s", "0", NULL));
    TEST_ASSERT_EQUAL(75, s.revs_per_kwh);
    char longhost[40];
    memset(longhost, 'a', 39);
    longhost[39] = '\0';
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "hostname", longhost, NULL));
    TEST_ASSERT_EQUAL_STRING("ferraris", s.hostname);

    // Leeres Passwort behaelt den alten Wert, unbekannte Felder werden ignoriert
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "ap_password", "", NULL));
    TEST_ASSERT_EQUAL_STRING("ferraris-setup", s.ap_password);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "ap_password", "neues-passwort", NULL));
    TEST_ASSERT_EQUAL_STRING("neues-passwort", s.ap_password);
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "ap_password2", "egal", NULL));
}

static void test_settings_validate(void)
{
    ferraris_settings_t s = settings_sample();
    const char *why = NULL, *field = NULL;
    TEST_ASSERT_TRUE(ferraris_settings_validate(&s, &why, &field));
    TEST_ASSERT_NULL(why);

    // Schwellen je Eingang: Fehler am betroffenen Eingang
    ferraris_settings_t t = s;
    t.threshold_low[1] = 3450;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, &why, &field));
    TEST_ASSERT_EQUAL_STRING("high2", field);
    t = s; t.threshold_high[2] = 4096;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("high3", field);
    t = s; t.threshold_low[0] = -1;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("low1", field);
    // Inaktiver Eingang (Q8 bei 5 aktiven) wird nicht geprueft
    t = s; t.threshold_low[7] = 4000; t.threshold_high[7] = 10;
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));
    // Je Kanal verschieden ist erlaubt
    t = s; t.threshold_high[0] = 2900; t.threshold_low[0] = 2700;
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));

    t = s; t.count_b = 1;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("count_b", field);
    t = s; t.count_a = 6;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));
    t = s; t.revs_per_kwh = 0;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));

    t = s; strcpy(t.hostname, "-ferraris");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));
    t = s; strcpy(t.hostname, "zaehler keller");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));
    t = s; strcpy(t.hostname, "Zaehler-2");
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));

    t = s; strcpy(t.mqtt_uri, "mqtt://broker.local:1883");
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));
    strcpy(t.mqtt_uri, "http://broker");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("mqtt_uri", field);
    strcpy(t.mqtt_uri, "mqtt://");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));

    t = s; strcpy(t.mqtt_prefix, "haus/zaehler");
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));
    strcpy(t.mqtt_prefix, "haus/#");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));
    strcpy(t.mqtt_prefix, "haus/");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));

    t = s; strcpy(t.ap_password, "kurz");
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("ap_password", field);
}

static void test_settings_inputs(void)
{
    ferraris_settings_t s = settings_sample();
    const char *field = NULL;
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "sensor_count", "3", NULL));
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "gpio3", "33", NULL));
    TEST_ASSERT_EQUAL(33, s.sensor_gpio[2]);
    TEST_ASSERT_TRUE(ferraris_settings_validate(&s, NULL, NULL));
    TEST_ASSERT_FALSE(ferraris_settings_set_field(&s, "gpio1", "x", NULL));
    TEST_ASSERT_TRUE(ferraris_settings_set_field(&s, "gpio9", "1", NULL));   // unbekannt -> ignoriert

    // Doppelter Pin unter den aktiven Eingaengen -> Fehler am spaeteren Feld
    ferraris_settings_t t = s;
    t.sensor_gpio[2] = 36;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("gpio3", field);
    // ... aber egal, wenn der Eingang nicht aktiv ist
    t.sensor_count = 2;
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));

    // Kein ADC1-Pin
    t = s; t.sensor_gpio[1] = 25;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("gpio2", field);

    // Anzahl 2..8, Zaehlpaar unter den aktiven Eingaengen
    t = s; t.sensor_count = 1;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("sensor_count", field);
    t.sensor_count = 9;
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, NULL));
    t = settings_sample(); t.sensor_count = 8;   // alle 8 ADC1-Pins
    TEST_ASSERT_TRUE(ferraris_settings_validate(&t, NULL, NULL));
    t = s; t.count_b = 4;          // nur 3 aktiv
    TEST_ASSERT_FALSE(ferraris_settings_validate(&t, NULL, &field));
    TEST_ASSERT_EQUAL_STRING("count_b", field);

    // Neustart: Anzahl oder aktive Pins geaendert, inaktive Pins egal
    ferraris_settings_t boot = s;
    t = s; t.sensor_gpio[7] = 39;
    TEST_ASSERT_FALSE(ferraris_settings_needs_restart(&boot, &t));
    t.sensor_gpio[0] = 39;
    TEST_ASSERT_TRUE(ferraris_settings_needs_restart(&boot, &t));
    t = s; t.sensor_count = 4;
    TEST_ASSERT_TRUE(ferraris_settings_needs_restart(&boot, &t));
}

static void test_settings_restart_and_json(void)
{
    ferraris_settings_t boot = settings_sample();
    ferraris_settings_t now = boot;
    now.revs_per_kwh = 75;           // sofort wirksam
    now.threshold_high[0] = 3500;       // Schwellen wirken sofort
    now.envelope_auto = true;
    TEST_ASSERT_FALSE(ferraris_settings_needs_restart(&boot, &now));
    now.count_b = 3;
    TEST_ASSERT_TRUE(ferraris_settings_needs_restart(&boot, &now));
    now = boot;
    strcpy(now.mqtt_password, "geheim");
    TEST_ASSERT_TRUE(ferraris_settings_needs_restart(&boot, &now));

    strcpy(now.mqtt_user, "u\"1");
    char buf[FERRARIS_SETTINGS_JSON_MAX];
    int n = ferraris_settings_to_json(&now, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\"sensor_count\":5,\"gpio\":[36,39,34,35,32,33,37,38],"
                                     "\"high\":[3450,3450,3450,3450,3450,3450,3450,3450],"
                                     "\"low\":[3300,3300,3300,3300,3300,3300,3300,3300],\"k\":150,"
                                     "\"count_a\":1,\"count_b\":2,\"envelope_auto\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt_user\":\"u\\\"1\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt_password_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt_interval_s\":10,\"ap_password_set\":true}"));
    TEST_ASSERT_NULL(strstr(buf, "geheim"));            // Passwoerter nie im JSON
    TEST_ASSERT_NULL(strstr(buf, "ferraris-setup"));

    char small[16];
    TEST_ASSERT_EQUAL(n, ferraris_settings_to_json(&now, small, sizeof(small)));
    TEST_ASSERT_EQUAL(15, (int)strlen(small));
}

// ─── Robustheit ──────────────────────────────────────────────────────────────

static void test_ota_verify(void)
{
    ferraris_ota_verify_t v;
    ferraris_ota_verify_init(&v, 60000, 30000, 300000);
    // Gesund ab 10 s: bestaetigt fruehestens bei 60 s (min) und 40 s (stabil)
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 5000, false));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 10000, true));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 39000, true));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 59000, true));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_VALID, ferraris_ota_verify_step(&v, 60000, true));

    // Unterbrechung setzt die Stabilitaetszeit zurueck
    ferraris_ota_verify_init(&v, 60000, 30000, 300000);
    ferraris_ota_verify_step(&v, 1000, true);
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 70000, false));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 71000, true));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 100000, true));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_VALID, ferraris_ota_verify_step(&v, 101000, true));

    // Nie gesund -> Rollback nach Timeout
    ferraris_ota_verify_init(&v, 60000, 30000, 300000);
    TEST_ASSERT_EQUAL(FERRARIS_OTA_WAIT, ferraris_ota_verify_step(&v, 299000, false));
    TEST_ASSERT_EQUAL(FERRARIS_OTA_ROLLBACK, ferraris_ota_verify_step(&v, 300000, false));
    // Kurz vor dem Timeout gesund geworden, aber nicht lange genug -> trotzdem Rollback
    ferraris_ota_verify_init(&v, 60000, 30000, 300000);
    ferraris_ota_verify_step(&v, 290000, true);
    TEST_ASSERT_EQUAL(FERRARIS_OTA_ROLLBACK, ferraris_ota_verify_step(&v, 300000, true));
}

static void test_crash_streak(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, ferraris_crash_streak_next(0, true));
    TEST_ASSERT_EQUAL_UINT32(3, ferraris_crash_streak_next(2, true));
    TEST_ASSERT_EQUAL_UINT32(0, ferraris_crash_streak_next(5, false));   // Einschalten beendet Serie
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, ferraris_crash_streak_next(UINT32_MAX, true));
    TEST_ASSERT_FALSE(ferraris_safe_mode(2, 3));
    TEST_ASSERT_TRUE(ferraris_safe_mode(3, 3));
    TEST_ASSERT_FALSE(ferraris_safe_mode(100, 0));                      // 0 = nie
}

// ─── Automatisches Einmessen ─────────────────────────────────────────────────

static void test_calib_recorder(void)
{
    static uint16_t buf[2 * 8];          // 8 Zeilen fuer 2 Sensoren
    ferraris_calib_rec_t r;
    TEST_ASSERT_TRUE(ferraris_calib_rec_init(&r, buf, 16, 2, 2));
    TEST_ASSERT_EQUAL_UINT32(8, r.cap_rows);
    // 16 Sets -> 8 Buckets a 2 Sets, Maximum je Bucket
    for (int k = 0; k < 16; k++) {
        int raw[2] = { k, 100 - k };
        ferraris_calib_rec_feed(&r, raw);
    }
    TEST_ASSERT_EQUAL_UINT32(8, r.rows);
    TEST_ASSERT_EQUAL_UINT16(1, buf[0]);      // max(0,1)
    TEST_ASSERT_EQUAL_UINT16(100, buf[1]);    // max(100,99)
    TEST_ASSERT_EQUAL_UINT16(15, buf[14]);
    // Naechster Bucket: Puffer voll -> zusammenfassen, Breite verdoppelt
    int raw[2] = { 4095, 0 };
    ferraris_calib_rec_feed(&r, raw);
    ferraris_calib_rec_feed(&r, raw);
    TEST_ASSERT_EQUAL_UINT32(4, r.bucket_sets);
    TEST_ASSERT_EQUAL_UINT32(5, r.rows);       // 4 zusammengefasste + 1 neuer
    TEST_ASSERT_EQUAL_UINT16(3, buf[0]);       // max(Bucket 0, Bucket 1)
    TEST_ASSERT_EQUAL_UINT16(100, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(4095, buf[8]);
    TEST_ASSERT_EQUAL_UINT32(18, r.total_sets);
}

/**
 * Synthetische Scheibe wie im Feld: Marke laeuft Q2 -> Q3 -> Q1, Nachbarn
 * ueberlappen sich, Q2 und Q1 nicht. Q3 hat den groessten Ausschlag.
 */
static uint32_t calib_synth(uint16_t *buf, uint32_t rows, int offset_q1, bool marks)
{
    static const int base[3] = { 2650, 3100, 400 };
    static const int amp[3]  = { 400, 300, 1600 };
    const int off[3] = { offset_q1, 0, 25 };   // Q1, Q2, Q3 (Zeilen nach Q2)
    uint32_t seed = 1;
    for (uint32_t r = 0; r < rows; r++) {
        for (int i = 0; i < 3; i++) {
            seed = seed * 1103515245u + 12345u;
            int noise = (int)((seed >> 16) % 61) - 30;
            int ph = (int)((r + 400 - (uint32_t)off[i]) % 400);   // Periode 400 Zeilen
            int v = base[i] + noise + ((marks && ph >= 100 && ph < 140) ? amp[i] : 0);
            buf[r * 3 + (uint32_t)i] = (uint16_t)v;
        }
    }
    return rows;
}

static void test_calib_analyze(void)
{
    static uint16_t buf[3 * 2000];
    ferraris_calib_result_t r;

    calib_synth(buf, 2000, 50, true);
    ferraris_calib_analyze(buf, 2000, 3, 100.0f, false, &r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(3, r.count_a);            // Q3 vor Q1 -> Bezug vorwaerts
    TEST_ASSERT_EQUAL(1, r.count_b);
    TEST_ASSERT_TRUE(r.steps >= 16);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 40.0f, r.period_s);   // 400 Zeilen x 100 ms
    for (int i = 0; i < 3; i++) {
        // Ruhe < LOW < HIGH < Marke
        TEST_ASSERT_TRUE(r.s[i].usable);
        TEST_ASSERT_TRUE(r.s[i].rest_hi < r.s[i].low);
        TEST_ASSERT_TRUE(r.s[i].low < r.s[i].high);
        TEST_ASSERT_TRUE(r.s[i].high < r.s[i].peak);
        TEST_ASSERT_EQUAL(5, r.s[i].passes);
    }
    TEST_ASSERT_TRUE(r.pairs_valid >= 1);

    // Einspeisung waehrend der Aufzeichnung -> A/B getauscht
    ferraris_calib_analyze(buf, 2000, 3, 100.0f, true, &r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(1, r.count_a);
    TEST_ASSERT_EQUAL(3, r.count_b);

    // Q1 ueberlappt nicht mehr mit Q3 -> einziges gueltiges Paar ist Q2/Q3
    calib_synth(buf, 2000, 120, true);
    ferraris_calib_analyze(buf, 2000, 3, 100.0f, false, &r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(2, r.count_a);
    TEST_ASSERT_EQUAL(3, r.count_b);

    // Keine Marken -> kein Sensor nutzbar, Meldung
    calib_synth(buf, 2000, 50, false);
    ferraris_calib_analyze(buf, 2000, 3, 100.0f, false, &r);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(r.s[0].usable);
    TEST_ASSERT_NOT_NULL(r.message);

    // Zu kurz fuer eine Umdrehung
    calib_synth(buf, 2000, 50, true);
    ferraris_calib_analyze(buf, 150, 3, 100.0f, false, &r);
    TEST_ASSERT_FALSE(r.ok);

    // JSON
    calib_synth(buf, 2000, 50, true);
    ferraris_calib_analyze(buf, 2000, 3, 100.0f, false, &r);
    char js[FERRARIS_CALIB_JSON_MAX];
    int n = ferraris_calib_result_to_json(&r, js, sizeof(js));
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(js));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"count_a\":3,\"count_b\":1"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quad_forward_revolution);
    RUN_TEST(test_quad_backward_revolution);
    RUN_TEST(test_quad_invalid_jump_ignored);
    RUN_TEST(test_schmitt_hysteresis);
    RUN_TEST(test_schmitt_set_thresholds_keeps_state);
    RUN_TEST(test_autotune_bimodal);
    RUN_TEST(test_autotune_unimodal_fails);
    RUN_TEST(test_collector_populations_and_timing);
    RUN_TEST(test_logger_watts);
    RUN_TEST(test_logger_quad_and_marks);
    RUN_TEST(test_power_needs_full_revolution);
    RUN_TEST(test_power_uneven_steps);
    RUN_TEST(test_power_direction_change);
    RUN_TEST(test_power_timer_wrap);
    RUN_TEST(test_data_energy);
    RUN_TEST(test_data_set_quad);
    RUN_TEST(test_hw_classify);
    RUN_TEST(test_status_json);
    RUN_TEST(test_wifi_credentials);
    RUN_TEST(test_json_string);
    RUN_TEST(test_settings_set_field);
    RUN_TEST(test_settings_validate);
    RUN_TEST(test_settings_inputs);
    RUN_TEST(test_settings_restart_and_json);
    RUN_TEST(test_ota_verify);
    RUN_TEST(test_crash_streak);
    RUN_TEST(test_calib_recorder);
    RUN_TEST(test_calib_analyze);
    return UNITY_END();
}
