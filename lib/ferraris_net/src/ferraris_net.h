/**
 * @file ferraris_net.h
 * @brief WLAN-Anbindung: Station + Einrichtungs-AP, Webseiten, mDNS, optional MQTT.
 *
 * WLAN-Ablauf (alles im loop()-Task, ohne Einfluss auf die Zaehlung):
 *   1. Zugangsdaten aus NVS (Namespace "ferrnet"), sonst Defaults aus secrets.h.
 *   2. Verbindung versuchen. Klappt das nicht in sta_connect_timeout_ms (oder
 *      es gibt keine SSID), startet der Access Point "<Praefix><MAC4>" mit
 *      Captive Portal -> Einrichtungsseite /wifi.
 *   3. Im AP-Modus wird alle ap_retry_interval_ms neu verbunden, aber nur wenn
 *      kein Geraet am AP haengt (ein Versuch wechselt den Funkkanal).
 *   4. Nach erfolgreicher Verbindung bleibt der AP noch ap_off_delay_ms an
 *      (die Einrichtungsseite zeigt dann die neue IP) und geht aus, sobald
 *      niemand mehr verbunden ist.
 *   5. Geht das WLAN im Betrieb verloren: Wiederverbinden, nach
 *      sta_lost_to_ap_ms zusaetzlich AP an.
 *
 * HTTP (Port http_port):
 *   GET  /              Dashboard
 *   GET  /wifi          WLAN-Einrichtung          (*)
 *   GET  /api/status    Status-JSON (ferraris_status.h)
 *   GET  /api/history   Leistungsverlauf {"interval_s":10,"w":[...]} (60 min)
 *   GET  /api/csv       aktuelle CSV-Zeile
 *   GET  /api/scan      WLAN-Scan {"scanning":bool,"networks":[...]}   (*)
 *   POST /api/wifi      ssid, password -> NVS speichern + verbinden      (*)
 *   GET  /config        Einstellungen                                   (*)
 *   GET  /api/config    {"settings":{...},"defaults":{...},"restart_pending":bool,
 *                        "autotune":bool,"max_sensors":8,"min_sensors":2,
 *                        "firmware":{"version","partition","ota_max","ota_state",
 *                                    "ota_left_s","safe_mode","coredump":{...}}}
 *                       (ohne Passwoerter)                               (*)
 *   POST /api/config    Formularfelder wie ferraris_settings_set_field   (*)(**)
 *   POST /api/restart   Neustart nach ~0,5 s                             (*)(**)
 *   POST /api/ota       Firmware-Update: multipart, Feld "firmware" = firmware.bin;
 *                       schreibt in den freien OTA-Slot, dann Neustart     (*)(**)
 *   GET  /api/coredump  Absturzbericht (Rohformat) herunterladen          (*)
 *   GET  /api/calib     Einmessung: Status + Ergebnis                     (*)
 *   POST /api/calib/start  duration_s, reverse=0/1                         (*)(**)
 *   POST /api/calib/stop   mode=finish (jetzt auswerten) | cancel          (*)(**)
 *   POST /api/coredump/delete  Absturzbericht loeschen                     (*)(**)
 *   (*) aus dem Heimnetz nur mit HTTP-Anmeldung (admin_user/admin_password);
 *       ueber den AP ohne, dort schuetzt bereits das WPA2-Passwort.
 *   (**) zusaetzlich Header "X-Ferraris: 1" (gegen CSRF: fremde Seiten koennen
 *       ihn ohne CORS-Preflight nicht setzen, und den beantworten wir nicht).
 *
 * MQTT (nur wenn mqtt_uri gesetzt): <prefix>/state (JSON), <prefix>/status
 * (online/offline, retained, Last Will).
 */

#ifndef FERRARIS_NET_H
#define FERRARIS_NET_H

#include <stddef.h>
#include <stdint.h>
#include "ferraris_settings.h"
#include "ferraris_status.h"

/** Anbindung der Einstellungsseite an main (alle Aufrufe im loop()-Task). */
typedef struct {
    /** Aktuelle (gespeicherte) Werte und Firmware-Defaults aus ferraris_config.h. */
    void (*get)(ferraris_settings_t *current, ferraris_settings_t *defaults);
    /** Pruefen, im NVS speichern, sofort wirksame Felder anwenden.
     *  why/field: Fehlermeldung und Feldname bei false. */
    bool (*apply)(const ferraris_settings_t *s, const char **why, const char **field);
    /** Gespeicherte Werte, die erst nach einem Neustart wirken, weichen ab. */
    bool (*restart_pending)(void);
    bool autotune;                   /**< Auto-Tune an: Schwellen werden periodisch neu gesetzt */
} ferraris_settings_ops_t;

/** Automatisches Einmessen (alle Aufrufe im loop()-Task). */
typedef struct {
    /** Aufzeichnung starten. reverse: waehrend der Aufzeichnung wird eingespeist. */
    bool (*start)(uint32_t duration_s, bool reverse, const char **why);
    void (*finish)(void);            /**< vorzeitig auswerten */
    void (*cancel)(void);            /**< abbrechen, Ergebnis verwerfen */
    /** {"state":"idle|running|done","elapsed_s",...,"result":{...}|null}, Rueckgabe wie snprintf */
    int  (*status_json)(char *buf, size_t len);
} ferraris_calib_ops_t;

typedef struct {
    const char *default_ssid;        /**< falls im NVS nichts steht (secrets.h) */
    const char *default_password;
    const char *hostname;            /**< DHCP + mDNS (<hostname>.local) */
    uint16_t    http_port;

    const char *ap_ssid_prefix;      /**< + 4 Hex-Stellen der MAC */
    const char *ap_password;         /**< WPA2, 8..63 Zeichen */
    const char *admin_user;          /**< HTTP-Anmeldung fuer /wifi aus dem Heimnetz */
    const char *admin_password;
    uint32_t    sta_connect_timeout_ms;
    uint32_t    sta_lost_to_ap_ms;
    uint32_t    ap_retry_interval_ms;
    uint32_t    ap_off_delay_ms;

    const char *mqtt_uri;            /**< "" = kein MQTT */
    const char *mqtt_user;           /**< "" = ohne Anmeldung */
    const char *mqtt_password;
    const char *mqtt_topic_prefix;
    uint32_t    mqtt_interval_ms;

    /** Optional: nach wichtigen Konsolen-Meldungen (WLAN verbunden, AP aktiv)
     *  aufgerufen, um laufende Ausgaben (CSV-Strom) kurz anzuhalten. */
    void      (*console_pause_fn)(uint32_t ms);
    uint32_t    console_pause_ms;

    /** Optional: Einstellungsseite /config (NULL = Seite aus). */
    const ferraris_settings_ops_t *settings;
    /** Optional: automatisches Einmessen (NULL = aus). */
    const ferraris_calib_ops_t    *calib;
} ferraris_net_config_t;

/** Fuellt einen Status-Snapshot (WLAN-/AP-Felder ergaenzt ferraris_net selbst). */
typedef void (*ferraris_status_fn)(ferraris_status_t *out);

/** Schreibt die aktuelle CSV-Zeile, Rueckgabe wie snprintf. */
typedef int (*ferraris_csv_fn)(char *buf, size_t len);

/** WLAN/AP und Webserver starten. Kehrt sofort zurueck. */
void ferraris_net_begin(const ferraris_net_config_t *cfg,
                        ferraris_status_fn status_fn, ferraris_csv_fn csv_fn);

/** Aus loop() aufrufen: WLAN-Zustandsautomat, DNS, HTTP, Verlauf, MQTT. */
void ferraris_net_loop(void);

/**
 * @brief Netz "gesund" fuer die OTA-Bestaetigung: mit dem Heim-WLAN verbunden,
 *        oder Einrichtungs-AP aktiv mit Geraet / ohne konfiguriertes WLAN.
 *        MQTT zaehlt nicht (ein Broker-Ausfall soll kein Update zuruecknehmen).
 */
bool ferraris_net_healthy(void);

#endif // FERRARIS_NET_H
