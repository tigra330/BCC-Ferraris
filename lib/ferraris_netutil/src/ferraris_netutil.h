/**
 * @file ferraris_netutil.h
 * @brief Hilfen fuer die Web-Schnittstelle (pure C, PC-testbar).
 */

#ifndef FERRARIS_NETUTIL_H
#define FERRARIS_NETUTIL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max. Laengen nach IEEE 802.11 / WPA2-PSK. */
#define FERRARIS_SSID_MAX     32
#define FERRARIS_PASSWORD_MAX 63

/**
 * @brief WLAN-Zugangsdaten pruefen.
 *
 * SSID: 1..32 Zeichen. Passwort: leer (offenes Netz) oder 8..63 Zeichen.
 * @param why optional: deutsche Fehlermeldung bei false
 */
bool ferraris_wifi_credentials_valid(const char *ssid, const char *password, const char **why);

/**
 * @brief String als JSON-String (mit Anfuehrungszeichen, escaped) schreiben.
 *
 * Steuerzeichen werden als \\u00XX ausgegeben, Bytes >= 0x80 unveraendert
 * (UTF-8 bleibt gueltig). Untrusted Daten (SSIDs aus dem Scan) sind damit sicher.
 * @return benoetigte Laenge ohne Nullterminator (wie snprintf), < 0 bei Fehler.
 */
int ferraris_json_string(char *buf, size_t len, const char *s);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_NETUTIL_H
