/**
 * @file ferraris_config.h
 * @brief Projekt-Konfiguration (ersetzt die Kconfig-Menues von ferraris-pio).
 *
 * Jeder Wert ist mit #ifndef geschuetzt und kann per build_flags in
 * platformio.ini ueberschrieben werden, z. B.
 *     build_flags = -DFERRARIS_REVOLUTIONS_PER_KWH=75 -DFERRARIS_CSV_LOG_ENABLE=0
 * Die Kommentare dokumentieren, *warum* ein Default gewaehlt wurde.
 *
 * Eingaenge (Anzahl, GPIOs), k, Schmitt-Schwellen, Zaehlpaar, Huelle auto/fest,
 * Hostname, MQTT und das
 * AP-/Admin-Passwort sind hier nur DEFAULTS: ueber die Webseite /config
 * gespeicherte Werte (NVS "ferrcfg") haben beim Start Vorrang.
 */

#ifndef FERRARIS_CONFIG_H
#define FERRARIS_CONFIG_H

// ─── Zaehler ────────────────────────────────────────────────────────────────

/** Zaehlerkonstante k (Umdrehungen/kWh). Typisch 75, 150 oder 300; am RML-Zaehler 150. */
#ifndef FERRARIS_REVOLUTIONS_PER_KWH
#define FERRARIS_REVOLUTIONS_PER_KWH 150
#endif

// ─── Lichtschranken Q1..Q8 (ADC1-Pins 32..39, alle verschieden) ──────────────
// ADC1: 32=CH4 33=CH5 34=CH6 35=CH7 36=CH0 37=CH1 38=CH2 39=CH3.
// Die Marke laeuft bei Bezug Q1 -> Q2 -> ... Nur ADC1: ADC2 ist mit aktivem
// WLAN nicht nutzbar. Anzahl und Pins sind Defaults, aenderbar ueber /config.

/** Aktive Eingaenge Q1..Qn (2..8). 5 = Aufbau RML 2.0. Mehr Eingaenge teilen
 *  sich die DMA-Rate: bei 8 bleiben ~6 kHz je Sensor (Marke bei Volllast
 *  ~1,7 ms -> noch ~10 Samples). */
#ifndef FERRARIS_SENSOR_COUNT
#define FERRARIS_SENSOR_COUNT 5
#endif

#ifndef FERRARIS_Q1_GPIO
#define FERRARIS_Q1_GPIO 36
#endif
#ifndef FERRARIS_Q2_GPIO
#define FERRARIS_Q2_GPIO 39
#endif
#ifndef FERRARIS_Q3_GPIO
#define FERRARIS_Q3_GPIO 34
#endif
#ifndef FERRARIS_Q4_GPIO
#define FERRARIS_Q4_GPIO 35
#endif
#ifndef FERRARIS_Q5_GPIO
#define FERRARIS_Q5_GPIO 32
#endif
// Q6..Q8: die restlichen ADC1-Pins. GPIO37/38 sind auf den meisten
// ESP32-WROOM-Devkits nicht herausgefuehrt.
#ifndef FERRARIS_Q6_GPIO
#define FERRARIS_Q6_GPIO 33
#endif
#ifndef FERRARIS_Q7_GPIO
#define FERRARIS_Q7_GPIO 37
#endif
#ifndef FERRARIS_Q8_GPIO
#define FERRARIS_Q8_GPIO 38
#endif

/** Zaehlpaar: A = Sensor, den die Marke bei BEZUG zuerst erreicht (Q-Nummer 1..n). */
#ifndef FERRARIS_COUNT_A_Q
#define FERRARIS_COUNT_A_Q 1
#endif
/** Zaehlpaar: B (Q-Nummer 1..n, != A). */
#ifndef FERRARIS_COUNT_B_Q
#define FERRARIS_COUNT_B_Q 2
#endif

// ─── ADC continuous/DMA ─────────────────────────────────────────────────────

/** Wandlungsrate gesamt; geteilt durch 5 Kanaele -> ~10 kHz je Sensor. ESP32-Minimum 20000.
 *  Bei Volllast (63 A, ~8 U/s) verweilt die 5-Grad-Marke ~1,7 ms -> genug Headroom. */
#ifndef FERRARIS_ADC_DMA_SAMPLE_RATE_HZ
#define FERRARIS_ADC_DMA_SAMPLE_RATE_HZ 50000
#endif
/** Groesse des dezimierten A/B-Rings fuer Re-Tune (Paare). */
#ifndef FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY
#define FERRARIS_ADC_DMA_SNAPSHOT_CAPACITY 3000
#endif
/** Jedes N-te Sample-Set in den Ring (200 @ 10 kHz/Kanal -> 50 Hz -> 60 s Fenster). */
#ifndef FERRARIS_ADC_DMA_SNAPSHOT_DECIMATION
#define FERRARIS_ADC_DMA_SNAPSHOT_DECIMATION 200
#endif

// ─── Schmitt-Trigger ────────────────────────────────────────────────────────
// Aus 1-h-Capture (2026-07-14): Rauschboden bis ~3397, Markenausschlag
// 3664..3700, Ruhe-Basis ~2600..2800. HIGH=3450 liegt 53 ueber dem Rauschen;
// eine alte HIGH=3250 liess Rauschen durch beide Schwellen oszillieren ->
// Fehltrigger + Unterzaehlung. Ein globaler Wert passt fuer beide Kanaele.

#ifndef SCHMITT_TRIGGER_THRESHOLD_HIGH
#define SCHMITT_TRIGGER_THRESHOLD_HIGH 3450
#endif
#ifndef SCHMITT_TRIGGER_THRESHOLD_LOW
#define SCHMITT_TRIGGER_THRESHOLD_LOW 3300
#endif

/** Auto-Tune beim Start + periodisches Re-Tune. AUS: bei geringer Last
 *  (1 Umdr./562 s) sieht ein 30-s-Fenster nur die Ruhe-Basis (unimodal) ->
 *  Auto-Tune scheitert und blockiert den Start 30 s. */
#ifndef SCHMITT_AUTOTUNE_ENABLE
#define SCHMITT_AUTOTUNE_ENABLE 0
#endif
#ifndef SCHMITT_AUTOTUNE_SMOOTHING
#define SCHMITT_AUTOTUNE_SMOOTHING 3          /* Bins, ungerade */
#endif
#ifndef SCHMITT_AUTOTUNE_MIN_SEPARATION
#define SCHMITT_AUTOTUNE_MIN_SEPARATION 6     /* Bins (x64 ADC = 384) */
#endif
#ifndef FERRARIS_AUTOTUNE_MIN_TIME_MS
#define FERRARIS_AUTOTUNE_MIN_TIME_MS 2000
#endif
#ifndef FERRARIS_AUTOTUNE_MAX_TIME_MS
#define FERRARIS_AUTOTUNE_MAX_TIME_MS 30000
#endif
#ifndef FERRARIS_RETUNE_INTERVAL_MIN
#define FERRARIS_RETUNE_INTERVAL_MIN 10
#endif

// ─── Huelle min/max je Sensor (CSV-Spalten, Webseite) ────────────────────────

/** 1 = Huelle laeuft automatisch mit (EMA zum Extrem, langsames Vergessen).
 *  0 = feste Grenzen FERRARIS_ENVELOPE_FIXED_MIN/MAX. Default 0: bei der
 *  Inbetriebnahme sind feste Grenzen leichter zu deuten (voller ADC-Bereich). */
#ifndef FERRARIS_ENVELOPE_AUTO
#define FERRARIS_ENVELOPE_AUTO 0
#endif
#ifndef FERRARIS_ENVELOPE_FIXED_MIN
#define FERRARIS_ENVELOPE_FIXED_MIN 0
#endif
#ifndef FERRARIS_ENVELOPE_FIXED_MAX
#define FERRARIS_ENVELOPE_FIXED_MAX 4095
#endif

// ─── ADC-Ueberwachung ────────────────────────────────────────────────────────

/** Kommen so lange keine Sample-Sets, wird die DMA-Abtastung neu gestartet
 *  (Sicherheitsnetz, z. B. nach WLAN-Moduswechseln). 0 = aus. */
#ifndef FERRARIS_ADC_STALL_MS
#define FERRARIS_ADC_STALL_MS 3000
#endif

// ─── Robustheit (ferraris_sysmon) ────────────────────────────────────────────

/** loop() (WLAN, HTTP, MQTT) haengt so lange -> Neustart. Weit ueber dem
 *  laengsten legitimen Block (WebServer wartet bis 5 s auf Client-Daten;
 *  OTA-Upload fuettert den Watchdog je Paket). 0 = aus. */
#ifndef FERRARIS_LOOP_WATCHDOG_MS
#define FERRARIS_LOOP_WATCHDOG_MS 30000
#endif
/** So lange gelaufen = stabil: Absturzserie endet. */
#ifndef FERRARIS_STABLE_UPTIME_MS
#define FERRARIS_STABLE_UPTIME_MS 60000
#endif
/** So viele Abstuerze/Watchdog-Resets in Folge (ohne stabile Laufzeit) ->
 *  abgesicherter Modus mit Werkseinstellungen (gespeicherte Eingaenge oder
 *  Schwellen koennten die Ursache sein). 0 = nie. */
#ifndef FERRARIS_SAFE_MODE_CRASHES
#define FERRARIS_SAFE_MODE_CRASHES 3
#endif
/** OTA: neue Firmware fruehestens nach so viel Laufzeit bestaetigen ... */
#ifndef FERRARIS_OTA_VERIFY_MIN_MS
#define FERRARIS_OTA_VERIFY_MIN_MS 60000
#endif
/** ... wenn sie so lange ununterbrochen gesund war (ADC liefert, Netz erreichbar) ... */
#ifndef FERRARIS_OTA_VERIFY_STABLE_MS
#define FERRARIS_OTA_VERIFY_STABLE_MS 30000
#endif
/** ... sonst nach dieser Zeit zurueck zur vorigen Firmware. 5 min: genug fuer
 *  einen WLAN-Wiederverbindungszyklus, kurz genug fuer den Zaehlausfall. */
#ifndef FERRARIS_OTA_VERIFY_TIMEOUT_MS
#define FERRARIS_OTA_VERIFY_TIMEOUT_MS 300000
#endif

// ─── Konsolen-Ausgabe ───────────────────────────────────────────────────────

/** RML-CSV-Log auf UART0 (~4 Hz) statt Klartext. Schaltet die Info-Logs ab,
 *  damit der Strom sauber bleibt (Capture fuer den PC-Simulator). */
#ifndef FERRARIS_CSV_LOG_ENABLE
#define FERRARIS_CSV_LOG_ENABLE 1
#endif
#ifndef FERRARIS_CSV_LOG_INTERVAL_MS
#define FERRARIS_CSV_LOG_INTERVAL_MS 250
#endif
/** Nach "WLAN verbunden" / "Access Point aktiv" pausiert der CSV-Strom so
 *  lange, damit die Meldung (IP, Adresse) in der Konsole lesbar stehen bleibt.
 *  Die Zaehlung laeuft dabei weiter. 0 = keine Pause. */
#ifndef FERRARIS_CONSOLE_PAUSE_MS
#define FERRARIS_CONSOLE_PAUSE_MS 10000
#endif
/** Klartext-Ausgabe (nur ohne CSV-Log): Intervall der "Zaehler:"-Zeile. */
#ifndef FERRARIS_PRINT_INTERVAL_MS
#define FERRARIS_PRINT_INTERVAL_MS 1000
#endif

// ─── WLAN / HTTP / MQTT ─────────────────────────────────────────────────────
// WLAN-Zugangsdaten werden ueber die Webseite /wifi eingerichtet und im NVS
// gespeichert (Namespace "ferrnet"). Ist dort nichts gespeichert, gelten die
// Werte aus include/secrets.h. Ohne funktionierendes WLAN startet ein Access
// Point zur Einrichtung; die Zaehlung laeuft in jedem Fall weiter.

#ifndef FERRARIS_WIFI_ENABLE
#define FERRARIS_WIFI_ENABLE 1
#endif
/** Hostname fuer DHCP und mDNS (http://<hostname>.local/). */
#ifndef FERRARIS_HOSTNAME
#define FERRARIS_HOSTNAME "ferraris"
#endif
#ifndef FERRARIS_HTTP_PORT
#define FERRARIS_HTTP_PORT 80
#endif
/** MQTT-Publish-Intervall (nur wenn FERRARIS_MQTT_URI gesetzt ist). */
#ifndef FERRARIS_MQTT_INTERVAL_MS
#define FERRARIS_MQTT_INTERVAL_MS 10000
#endif
/** Topic-Praefix: <prefix>/state (JSON), <prefix>/status (online/offline, retained). */
#ifndef FERRARIS_MQTT_TOPIC_PREFIX
#define FERRARIS_MQTT_TOPIC_PREFIX "ferraris"
#endif

// ─── Einrichtungs-Access-Point ──────────────────────────────────────────────

/** AP-Name = Praefix + letzte 4 Hex-Stellen der MAC, z. B. "Ferraris-3A7F". */
#ifndef FERRARIS_AP_SSID_PREFIX
#define FERRARIS_AP_SSID_PREFIX "Ferraris-"
#endif
/** WPA2-Passwort des AP (8..63 Zeichen). Gleichzeitig Admin-Passwort fuer
 *  /wifi, wenn die Seite aus dem Heimnetz (nicht ueber den AP) aufgerufen wird. */
#ifndef FERRARIS_AP_PASSWORD
#define FERRARIS_AP_PASSWORD "ferraris-setup"
#endif
#ifndef FERRARIS_ADMIN_USER
#define FERRARIS_ADMIN_USER "admin"
#endif
/** So lange wartet der Start auf die WLAN-Verbindung, bevor der AP kommt. */
#ifndef FERRARIS_STA_CONNECT_TIMEOUT_MS
#define FERRARIS_STA_CONNECT_TIMEOUT_MS 20000
#endif
/** WLAN im Betrieb so lange weg -> AP einschalten. */
#ifndef FERRARIS_STA_LOST_TO_AP_MS
#define FERRARIS_STA_LOST_TO_AP_MS 60000
#endif
/** Im AP-Modus erneuter Verbindungsversuch, wenn kein Geraet am AP haengt.
 *  (Ein Versuch wechselt den Funkkanal und wuerde AP-Clients trennen.) */
#ifndef FERRARIS_AP_RETRY_INTERVAL_MS
#define FERRARIS_AP_RETRY_INTERVAL_MS 120000
#endif
/** Nach erfolgreicher Verbindung bleibt der AP noch so lange an (die
 *  Einrichtungsseite kann so die neue IP anzeigen), sofern niemand verbunden ist. */
#ifndef FERRARIS_AP_OFF_DELAY_MS
#define FERRARIS_AP_OFF_DELAY_MS 60000
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef FERRARIS_WIFI_SSID
#define FERRARIS_WIFI_SSID ""
#endif
#ifndef FERRARIS_WIFI_PASSWORD
#define FERRARIS_WIFI_PASSWORD ""
#endif
#ifndef FERRARIS_MQTT_URI
#define FERRARIS_MQTT_URI ""          /* z. B. "mqtt://broker.local:1883", leer = kein MQTT */
#endif
#ifndef FERRARIS_MQTT_USER
#define FERRARIS_MQTT_USER ""
#endif
#ifndef FERRARIS_MQTT_PASSWORD
#define FERRARIS_MQTT_PASSWORD ""
#endif

#endif // FERRARIS_CONFIG_H
