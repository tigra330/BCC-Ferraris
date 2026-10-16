/**
 * @file ferraris_settings.h
 * @brief Zur Laufzeit editierbare Einstellungen (Webseite /config), pure C.
 *
 * Die Defaults kommen aus ferraris_config.h (main fuellt sie), gespeicherte
 * Werte aus dem NVS (ferraris_settings_store) ueberschreiben sie beim Start.
 * Diese Datei kennt weder NVS noch HTTP: sie parst einzelne Formularfelder,
 * prueft den ganzen Satz und formatiert ihn als JSON -> PC-testbar.
 *
 * Wirksamkeit (siehe ferraris_settings_needs_restart):
 *   sofort       k, Schmitt HIGH/LOW je Eingang, Huelle auto/fest
 *   Neustart     Eingaenge (Anzahl, GPIOs), Zaehlpaar A/B, Hostname, MQTT,
 *                AP-/Admin-Passwort
 *                (werden nur beim Start an Sampler bzw. WLAN/MQTT uebergeben)
 */

#ifndef FERRARIS_SETTINGS_H
#define FERRARIS_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FERRARIS_SETTINGS_MAX_SENSORS  8    /**< Q1..Q8: ADC1 hat genau 8 Pins (GPIO32..39) */
#define FERRARIS_SETTINGS_MIN_SENSORS  2    /**< Quadratur braucht ein Paar */
#define FERRARIS_SETTINGS_HOST_MAX     32
#define FERRARIS_SETTINGS_URI_MAX      127
#define FERRARIS_SETTINGS_CRED_MAX     64   /**< MQTT-Benutzer/-Passwort */
#define FERRARIS_SETTINGS_PREFIX_MAX   64
#define FERRARIS_SETTINGS_APPASS_MAX   63   /**< WPA2-PSK */

/** Puffergroesse, die fuer ferraris_settings_to_json sicher reicht. */
#define FERRARIS_SETTINGS_JSON_MAX 1024

typedef struct {
    // Eingaenge
    int      sensor_count;          /**< aktive Lichtschranken Q1..Qn, 2..8 */
    int      sensor_gpio[FERRARIS_SETTINGS_MAX_SENSORS];  /**< ADC1-GPIO je Q, aktive verschieden */

    // Zaehler / Lichtschranken
    int32_t  revs_per_kwh;          /**< k, 1..10000 */
    /** Schmitt-Schwellen je Eingang Q1..Q8 (wirksam fuer das Zaehlpaar; die
     *  Werte haengen am Sensor, ein neues Paar nimmt seine eigenen mit). */
    int      threshold_high[FERRARIS_SETTINGS_MAX_SENSORS];   /**< 1..4095, > low */
    int      threshold_low[FERRARIS_SETTINGS_MAX_SENSORS];    /**< 0..4094 */
    int      count_a;               /**< Q-Nummer 1..sensor_count */
    int      count_b;               /**< Q-Nummer 1..sensor_count, != count_a */
    bool     envelope_auto;         /**< min/max mitlaufend statt fest */

    // Netzwerk
    char     hostname[FERRARIS_SETTINGS_HOST_MAX + 1];
    char     mqtt_uri[FERRARIS_SETTINGS_URI_MAX + 1];      /**< "" = kein MQTT */
    char     mqtt_user[FERRARIS_SETTINGS_CRED_MAX + 1];
    char     mqtt_password[FERRARIS_SETTINGS_CRED_MAX + 1];
    char     mqtt_prefix[FERRARIS_SETTINGS_PREFIX_MAX + 1];
    uint32_t mqtt_interval_ms;      /**< 1 s .. 1 h */
    char     ap_password[FERRARIS_SETTINGS_APPASS_MAX + 1]; /**< = Admin-Passwort, 8..63 */
} ferraris_settings_t;

/**
 * @brief Ein Formularfeld uebernehmen (Name wie im JSON, Wert als Text).
 *
 * Zahlen muessen vollstaendig gueltig sein ("12a" -> Fehler), Bool-Felder
 * akzeptieren 1/0/true/false/on/off. "mqtt_interval_s" ist in Sekunden,
 * "gpio1".."gpio8" sind die Pins von Q1..Q8, "high1".."high8" / "low1".."low8"
 * ihre Schmitt-Schwellen. "threshold_high"/"threshold_low" setzen alle Eingaenge
 * auf einmal (aeltere API).
 * Leere Passwortfelder ("mqtt_password", "ap_password") behalten den alten
 * Wert. Unbekannte Felder werden ignoriert (true), damit das Formular
 * zusaetzliche Felder (z. B. Passwort-Wiederholung) senden darf.
 * Die Bereichspruefung des ganzen Satzes macht ferraris_settings_validate.
 *
 * @param why optional: deutsche Fehlermeldung bei false
 */
bool ferraris_settings_set_field(ferraris_settings_t *s, const char *name, const char *value,
                                 const char **why);

/**
 * @brief Ganzen Satz pruefen (Bereiche, A != B, HIGH > LOW, Hostname, Topic ...).
 *
 * Eingaenge: nur Q1..Q<sensor_count> muessen ADC1-Pins (32..39) und
 * verschieden sein; die uebrigen GPIO-Felder bleiben gespeichert, werden aber
 * nicht geprueft. Das gilt auch fuer die Schwellen (aktive: HIGH > LOW).
 * Das Zaehlpaar muss unter den aktiven Eingaengen liegen.
 * @param why   optional: deutsche Fehlermeldung bei false
 * @param field optional: Name des fehlerhaften Felds bei false
 */
bool ferraris_settings_validate(const ferraris_settings_t *s, const char **why, const char **field);

/** true, wenn sich Felder unterscheiden, die erst nach einem Neustart wirken. */
bool ferraris_settings_needs_restart(const ferraris_settings_t *boot, const ferraris_settings_t *now);

/**
 * @brief Satz als JSON-Objekt. Passwoerter werden NIE ausgegeben, nur
 *        "mqtt_password_set"/"ap_password_set".
 * @return Laenge ohne Nullterminator (wie snprintf), < 0 bei Fehler.
 */
int ferraris_settings_to_json(const ferraris_settings_t *s, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_SETTINGS_H
