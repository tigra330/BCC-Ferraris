/**
 * @file secrets.example.h
 * @brief Vorlage fuer Zugangsdaten. Kopieren nach include/secrets.h und anpassen:
 *
 *     cp include/secrets.example.h include/secrets.h
 *
 * secrets.h ist in .gitignore eingetragen. Die WLAN-Werte sind nur Defaults: was ueber
 * die Webseite /wifi eingerichtet wurde (NVS), hat Vorrang.
 */

#ifndef FERRARIS_SECRETS_H
#define FERRARIS_SECRETS_H

#define FERRARIS_WIFI_SSID     "mein-wlan"
#define FERRARIS_WIFI_PASSWORD "geheim"

/* Optional: MQTT. Leer lassen = kein MQTT. */
#define FERRARIS_MQTT_URI      ""   /* z. B. "mqtt://192.168.1.10:1883" */
#define FERRARIS_MQTT_USER     ""
#define FERRARIS_MQTT_PASSWORD ""

#endif // FERRARIS_SECRETS_H
