/**
 * @file ferraris_status.h
 * @brief Status-Snapshot des Zaehlers + JSON-Formatierung (pure C, PC-testbar).
 *
 * main fuellt den Snapshot, das Netzwerk (HTTP /api/status, MQTT <prefix>/state)
 * gibt ihn als JSON aus. Beide nutzen denselben Formatter -> identisches Format.
 *
 * JSON (Beispiel, eine Zeile):
 *   {"uptime_s":12,"counting":true,
 *    "meter":{"k":150,"steps_import":8,"steps_export":0,"import_kwh":0.0133,
 *             "export_kwh":0.0000,"net_kwh":0.0133,"power_w":412.3},
 *    "schmitt":{"high":3450,"low":3300},"count_pair":["Q1","Q2"],
 *    "sensors":[{"name":"Q1","gpio":36,"raw":2790,"min":2601,"max":3690,"hw":"OK",
 *                "high":3450,"low":3300},...],
 *    "wifi":{"connected":true,"ssid":"heim","rssi":-61,"ip":"192.168.1.23",
 *            "ap":{"active":false,"ssid":"Ferraris-3A7F","ip":"192.168.4.1","clients":0}},
 *    "adc":{"sets_per_s":10000,"restarts":0,"errors":0,"timeouts":0},
 *    "health":{"reset":"Einschalten","boots":3,"crashes":0,"safe_mode":false,
 *              "heap_free":154000,"heap_min":121000,"heap_block":98000,
 *              "wifi_reconnects":1,"mqtt":"aus","coredump":false,"ota":"ok"},
 *    "fw":"2.1.0","host":"ferraris"}
 */

#ifndef FERRARIS_STATUS_H
#define FERRARIS_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FERRARIS_STATUS_MAX_SENSORS 8

/** Puffergroesse, die fuer das JSON sicher reicht. */
#define FERRARIS_STATUS_JSON_MAX 2048   /* 8 Sensoren, lange SSID, Health-Block */

typedef struct {
    const char *name;      /**< "Q1".. */
    int         gpio;
    int         raw;       /**< letzter Rohwert */
    int         run_min;   /**< Huelle min (Hintergrund) */
    int         run_max;   /**< Huelle max (Marke) */
    const char *hw;        /**< Ergebnis Selbsttest, z. B. "OK" */
    int         high;      /**< Schmitt-Schwelle HIGH dieses Eingangs */
    int         low;       /**< Schmitt-Schwelle LOW dieses Eingangs */
} ferraris_sensor_status_t;

typedef struct {
    uint32_t uptime_s;
    bool     counting;              /**< Zaehlung freigegeben (nach Auto-Tune) */

    int32_t  revs_per_kwh;
    int32_t  steps_import;          /**< Quadratur-Schritte Bezug */
    int32_t  steps_export;          /**< Quadratur-Schritte Einspeisung */
    float    import_kwh;
    float    export_kwh;
    float    net_kwh;
    float    power_w;               /**< + Bezug, - Einspeisung */

    int      threshold_high;        /**< Kanal A (aeltere Clients), je Eingang: sensors[].high */
    int      threshold_low;
    const char *count_a;            /**< Name Zaehlsensor A */
    const char *count_b;            /**< Name Zaehlsensor B */

    int      n_sensors;
    ferraris_sensor_status_t sensors[FERRARIS_STATUS_MAX_SENSORS];

    bool     wifi_connected;
    char     wifi_ssid[33];
    int      wifi_rssi;
    char     wifi_ip[16];
    bool     ap_active;             /**< Einrichtungs-Access-Point an */
    char     ap_ssid[33];
    char     ap_ip[16];
    int      ap_clients;

    uint32_t adc_sets_per_s;        /**< vollstaendige Sample-Sets pro Sekunde */
    uint32_t adc_restarts;          /**< Neustarts durch die ADC-Ueberwachung */
    uint32_t adc_errors;            /**< Lesefehler + ungueltige Werte + fremde Kanaele */
    uint32_t adc_timeouts;          /**< Lesevorgaenge ohne Daten */

    // Robustheit (ferraris_sysmon, ferraris_net)
    const char *reset_reason;       /**< Grund des letzten Starts, Klartext */
    uint32_t boot_count;            /**< Starts seit Flash-Loeschen */
    uint32_t crash_count;           /**< davon nach Absturz/Watchdog */
    bool     safe_mode;             /**< Werkseinstellungen nach Absturzserie */
    uint32_t heap_free;
    uint32_t heap_min;              /**< Tiefstwert seit Start */
    uint32_t heap_block;            /**< groesster freier Block */
    uint32_t wifi_reconnects;       /**< Verbindungsverluste im Betrieb */
    const char *mqtt_state;         /**< "aus", "verbunden", "getrennt" */
    bool     coredump;              /**< Absturzbericht im Flash */
    const char *ota_state;          /**< "ok", "pruefung", "zurueckgenommen" */

    const char *fw_version;
    const char *hostname;
} ferraris_status_t;

/**
 * @brief Snapshot als einzeiliges JSON formatieren.
 * @return Laenge ohne Nullterminator; < 0 oder >= len, wenn der Puffer zu klein war.
 */
int ferraris_status_to_json(const ferraris_status_t *st, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_STATUS_H
