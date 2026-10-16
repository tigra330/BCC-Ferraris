/**
 * @file ferraris_net.cpp
 * @brief WLAN-Anbindung: Station + Einrichtungs-AP, Webseiten, mDNS, optional MQTT.
 */

#include "ferraris_net.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "esp_ota_ops.h"
#include "ferraris_calib.h"
#include "ferraris_log.h"
#include "ferraris_netutil.h"
#include "ferraris_sysmon.h"
#include "ferraris_web_pages.h"
#include "mqtt_client.h"

static const char *TAG = "net";

static const char *NVS_NAMESPACE = "ferrnet";

static ferraris_net_config_t s_cfg;
static ferraris_status_fn    s_status_fn;
static ferraris_csv_fn       s_csv_fn;
static WebServer            *s_server;
static DNSServer             s_dns;

// ─── WLAN-Zustand (nur loop()-Task) ──────────────────────────────────────────

static char     s_sta_ssid[FERRARIS_SSID_MAX + 1];
static char     s_sta_pass[FERRARIS_PASSWORD_MAX + 1];
static char     s_ap_ssid[FERRARIS_SSID_MAX + 1];
static bool     s_ap_on;
static bool     s_attempting;        // Verbindungsversuch laeuft
static uint32_t s_attempt_ms;        // Start des letzten Versuchs
static bool     s_was_connected;
static uint32_t s_connected_ms;      // seit wann verbunden
static uint32_t s_lost_ms;           // seit wann getrennt (0 = nie verbunden gewesen)
static bool     s_mdns_up;
static uint32_t s_wifi_reconnects;   // Verbindungsverluste im Betrieb
static bool     s_apply_pending;     // neue Zugangsdaten -> neu verbinden
static uint32_t s_apply_at;
static bool     s_restart_pending;   // POST /api/restart -> nach Antwort neu starten
static uint32_t s_restart_at;

// Firmware-Update (POST /api/ota), nur loop()-Task
static bool     s_ota_active;        // Update.begin() lief, Daten werden geschrieben
static bool     s_ota_started;       // Upload-Teil der Anfrage gesehen
static String   s_ota_error;         // erste Fehlermeldung des Uploads
static size_t   s_ota_bytes;

// ─── Leistungsverlauf: 60 min in 10-s-Schritten ──────────────────────────────

#define HISTORY_INTERVAL_MS 10000
#define HISTORY_LEN         360
static float    s_hist[HISTORY_LEN];
static int      s_hist_head, s_hist_count;
static uint32_t s_hist_last_ms;

// ─── MQTT ────────────────────────────────────────────────────────────────────

static esp_mqtt_client_handle_t s_mqtt;
static volatile bool            s_mqtt_connected;
static uint32_t                 s_last_publish_ms;
static char                     s_topic_state[96];
static char                     s_topic_status[96];

/**
 * @brief Wichtige Netzwerk-Meldung IMMER auf der Konsole ausgeben.
 *
 * Auch im CSV-Modus (dort sind die Info-Logs stumm). Als Kommentarzeile mit
 * '#', die der PC-Simulator (RML-Konvention) beim Einlesen ueberspringt.
 */
static void console_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void console_note(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("# %s\n", line);
}

/** Konsolen-Ausgaben kurz anhalten, damit die letzte Meldung lesbar bleibt. */
static void console_pause()
{
    if (s_cfg.console_pause_fn && s_cfg.console_pause_ms) {
        s_cfg.console_pause_fn(s_cfg.console_pause_ms);
    }
}

// ─── Zugangsdaten (NVS) ──────────────────────────────────────────────────────

static void load_credentials()
{
    Preferences p;
    String ssid, pass;
    // Read-only-Open schlaegt fehl, solange der Namespace nie geschrieben wurde.
    if (p.begin(NVS_NAMESPACE, true)) {
        ssid = p.getString("ssid", "");
        pass = p.getString("pass", "");
        p.end();
    }
    const char *src = "NVS";
    if (ssid.isEmpty()) {
        ssid = s_cfg.default_ssid ? s_cfg.default_ssid : "";
        pass = s_cfg.default_password ? s_cfg.default_password : "";
        src  = ssid.isEmpty() ? "keine" : "secrets.h";
    }
    strlcpy(s_sta_ssid, ssid.c_str(), sizeof(s_sta_ssid));
    strlcpy(s_sta_pass, pass.c_str(), sizeof(s_sta_pass));
    FLOGI(TAG, "WLAN-Zugangsdaten: %s%s%s%s", src, s_sta_ssid[0] ? " (SSID \"" : "",
          s_sta_ssid, s_sta_ssid[0] ? "\")" : "");
}

static bool save_credentials(const char *ssid, const char *pass)
{
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putString("ssid", ssid) > 0;
    p.putString("pass", pass);   // leeres Passwort (offenes Netz) schreibt 0 Bytes
    p.end();
    return ok;
}

// ─── Station / Access Point ──────────────────────────────────────────────────

static void sta_begin()
{
    if (!s_sta_ssid[0]) return;
    FLOGI(TAG, "WLAN: verbinde mit \"%s\" ...", s_sta_ssid);
    WiFi.begin(s_sta_ssid, s_sta_pass[0] ? s_sta_pass : nullptr);
    s_attempting = true;
    s_attempt_ms = millis();
}

static void mdns_start()
{
    if (s_mdns_up) return;
    if (MDNS.begin(s_cfg.hostname)) {
        MDNS.addService("http", "tcp", s_cfg.http_port);
        s_mdns_up = true;
        FLOGI(TAG, "mDNS: http://%s.local/", s_cfg.hostname);
    } else {
        FLOGW(TAG, "mDNS-Start fehlgeschlagen");
    }
}

static void ap_start()
{
    if (s_ap_on) return;
    WiFi.mode(WIFI_AP_STA);
    const char *pw = s_cfg.ap_password;
    if (pw && strlen(pw) < 8) {
        FLOGW(TAG, "AP-Passwort kuerzer als 8 Zeichen - AP laeuft OFFEN");
        pw = nullptr;
    }
    if (!WiFi.softAP(s_ap_ssid, pw)) {
        FLOGE(TAG, "Access Point konnte nicht gestartet werden");
        return;
    }
    s_ap_on = true;
    s_dns.start(53, "*", WiFi.softAPIP());   // Captive Portal: jede Domain -> wir
    mdns_start();
    console_note("Access Point \"%s\" aktiv (Passwort: %s) -> http://%s/wifi", s_ap_ssid,
                 pw ? pw : "keins", WiFi.softAPIP().toString().c_str());
    console_pause();
}

static void ap_stop()
{
    if (!s_ap_on) return;
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap_on = false;
    FLOGI(TAG, "Access Point aus");
}

static int ap_clients()
{
    return s_ap_on ? WiFi.softAPgetStationNum() : 0;
}

// ─── MQTT ────────────────────────────────────────────────────────────────────

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            esp_mqtt_client_publish(s_mqtt, s_topic_status, "online", 0, 1, 1);
            FLOGI(TAG, "MQTT verbunden");
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_mqtt_connected) FLOGW(TAG, "MQTT getrennt - neuer Versuch automatisch");
            s_mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            FLOGW(TAG, "MQTT-Fehler");
            break;
        default:
            break;
    }
}

static void mqtt_start()
{
    if (s_mqtt || !s_cfg.mqtt_uri || !s_cfg.mqtt_uri[0]) return;

    snprintf(s_topic_state, sizeof(s_topic_state), "%s/state", s_cfg.mqtt_topic_prefix);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/status", s_cfg.mqtt_topic_prefix);

    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri = s_cfg.mqtt_uri;
    mc.credentials.client_id = s_cfg.hostname;
    if (s_cfg.mqtt_user && s_cfg.mqtt_user[0]) {
        mc.credentials.username = s_cfg.mqtt_user;
        mc.credentials.authentication.password = s_cfg.mqtt_password;
    }
    mc.session.last_will.topic  = s_topic_status;
    mc.session.last_will.msg    = "offline";
    mc.session.last_will.qos    = 1;
    mc.session.last_will.retain = 1;

    s_mqtt = esp_mqtt_client_init(&mc);
    if (!s_mqtt) {
        FLOGE(TAG, "MQTT-Client konnte nicht angelegt werden");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        FLOGE(TAG, "MQTT-Start fehlgeschlagen");
        return;
    }
    FLOGI(TAG, "MQTT: %s, Topics %s / %s alle %lu ms", s_cfg.mqtt_uri, s_topic_state,
          s_topic_status, (unsigned long)s_cfg.mqtt_interval_ms);
}

// ─── Status ──────────────────────────────────────────────────────────────────

static void fill_status(ferraris_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (s_status_fn) s_status_fn(st);
    st->wifi_connected = WiFi.isConnected();
    strlcpy(st->wifi_ssid, s_sta_ssid, sizeof(st->wifi_ssid));
    if (st->wifi_connected) {
        st->wifi_rssi = WiFi.RSSI();
        strlcpy(st->wifi_ip, WiFi.localIP().toString().c_str(), sizeof(st->wifi_ip));
    }
    st->ap_active = s_ap_on;
    strlcpy(st->ap_ssid, s_ap_ssid, sizeof(st->ap_ssid));
    st->wifi_reconnects = s_wifi_reconnects;
    st->mqtt_state = !s_mqtt ? "aus" : (s_mqtt_connected ? "verbunden" : "getrennt");
    if (s_ap_on) {
        strlcpy(st->ap_ip, WiFi.softAPIP().toString().c_str(), sizeof(st->ap_ip));
        st->ap_clients = ap_clients();
    }
    if (!st->hostname) st->hostname = s_cfg.hostname;
}

static int status_json(char *buf, size_t len)
{
    ferraris_status_t st;
    fill_status(&st);
    return ferraris_status_to_json(&st, buf, len);
}

static void history_sample()
{
    uint32_t now = millis();
    if (s_hist_count && now - s_hist_last_ms < HISTORY_INTERVAL_MS) return;
    s_hist_last_ms = now;
    ferraris_status_t st;
    memset(&st, 0, sizeof(st));
    if (s_status_fn) s_status_fn(&st);
    s_hist[s_hist_head] = st.power_w;
    s_hist_head = (s_hist_head + 1) % HISTORY_LEN;
    if (s_hist_count < HISTORY_LEN) s_hist_count++;
}

static void mqtt_publish_if_due()
{
    if (!s_mqtt || !s_mqtt_connected) return;
    uint32_t now = millis();
    if (now - s_last_publish_ms < s_cfg.mqtt_interval_ms) return;
    s_last_publish_ms = now;

    static char json[FERRARIS_STATUS_JSON_MAX];
    int n = status_json(json, sizeof(json));
    if (n > 0 && n < (int)sizeof(json)) {
        esp_mqtt_client_publish(s_mqtt, s_topic_state, json, n, 0, 0);
    }
}

// ─── HTTP ────────────────────────────────────────────────────────────────────

static bool via_ap()
{
    return s_ap_on && s_server->client().localIP() == WiFi.softAPIP();
}

/** Einrichtung: ueber den AP frei, aus dem Heimnetz nur mit Anmeldung. */
static bool admin_ok()
{
    if (via_ap()) return true;
    if (s_server->authenticate(s_cfg.admin_user, s_cfg.admin_password)) return true;
    s_server->requestAuthentication(BASIC_AUTH, "Ferraris-Zaehler",
                                    "Anmeldung erforderlich (Benutzer admin, Passwort = AP-Passwort)");
    return false;
}

static void send_json(int code, const String &body)
{
    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(code, "application/json", body);
}

static void handle_index()
{
    s_server->send(200, "text/html; charset=utf-8", FERRARIS_PAGE_INDEX);
}

static void handle_wifi_page()
{
    if (!admin_ok()) return;
    s_server->send(200, "text/html; charset=utf-8", FERRARIS_PAGE_WIFI);
}

static void handle_status()
{
    static char json[FERRARIS_STATUS_JSON_MAX];   // nur loop()-Task
    int n = status_json(json, sizeof(json));
    if (n < 0 || n >= (int)sizeof(json)) {
        s_server->send(500, "text/plain", "status zu gross");
        return;
    }
    send_json(200, json);
}

static void handle_history()
{
    String out;
    out.reserve(32 + s_hist_count * 8);
    out += "{\"interval_s\":";
    out += HISTORY_INTERVAL_MS / 1000;
    out += ",\"w\":[";
    int start = (s_hist_head - s_hist_count + HISTORY_LEN) % HISTORY_LEN;
    for (int i = 0; i < s_hist_count; i++) {
        if (i) out += ',';
        out += String(s_hist[(start + i) % HISTORY_LEN], 1);
    }
    out += "]}";
    send_json(200, out);
}

static void handle_csv()
{
    char row[512];
    int n = s_csv_fn ? s_csv_fn(row, sizeof(row)) : -1;
    if (n < 0) {
        s_server->send(503, "text/plain", "kein Logger");
        return;
    }
    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(200, "text/plain", String(row) + "\n");
}

static void append_json_string(String &out, const char *s)
{
    char buf[2 * 6 * FERRARIS_SSID_MAX + 3];   // worst case: jedes Byte \u00XX
    ferraris_json_string(buf, sizeof(buf), s);
    out += buf;
}

static void handle_scan()
{
    if (!admin_ok()) return;

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true);    // asynchron, Ergebnis beim naechsten Aufruf
        n = WIFI_SCAN_RUNNING;
    }
    if (n == WIFI_SCAN_RUNNING) {
        send_json(200, "{\"scanning\":true,\"networks\":[]}");
        return;
    }

    // Gleiche SSID (mehrere Access Points) nur einmal, staerkstes Signal zuerst.
    int order[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;   // versteckte Netze
        int dup = -1;
        for (int j = 0; j < m; j++) {
            if (WiFi.SSID(order[j]) == WiFi.SSID(i)) { dup = j; break; }
        }
        if (dup < 0) order[m++] = i;
        else if (WiFi.RSSI(i) > WiFi.RSSI(order[dup])) order[dup] = i;
    }
    for (int a = 1; a < m; a++) {
        for (int b = a; b > 0 && WiFi.RSSI(order[b]) > WiFi.RSSI(order[b - 1]); b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    }

    String out = "{\"scanning\":false,\"networks\":[";
    for (int k = 0; k < m; k++) {
        int i = order[k];
        if (k) out += ',';
        out += "{\"ssid\":";
        append_json_string(out, WiFi.SSID(i).c_str());
        out += ",\"rssi\":";
        out += WiFi.RSSI(i);
        out += ",\"secure\":";
        out += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
        out += '}';
    }
    out += "]}";
    WiFi.scanDelete();
    send_json(200, out);
}

static void handle_wifi_save()
{
    if (!admin_ok()) return;

    String ssid = s_server->arg("ssid");
    String pass = s_server->arg("password");
    ssid.trim();
    // Leeres Passwort bei unveraenderter SSID = gespeichertes behalten.
    if (pass.isEmpty() && ssid == s_sta_ssid) pass = s_sta_pass;

    const char *why = nullptr;
    if (!ferraris_wifi_credentials_valid(ssid.c_str(), pass.c_str(), &why)) {
        String out = "{\"ok\":false,\"error\":";
        append_json_string(out, why);
        out += '}';
        send_json(400, out);
        return;
    }
    if (!save_credentials(ssid.c_str(), pass.c_str())) {
        send_json(500, "{\"ok\":false,\"error\":\"Speichern im NVS fehlgeschlagen\"}");
        return;
    }
    strlcpy(s_sta_ssid, ssid.c_str(), sizeof(s_sta_ssid));
    strlcpy(s_sta_pass, pass.c_str(), sizeof(s_sta_pass));
    FLOGI(TAG, "Neue WLAN-Zugangsdaten gespeichert (SSID \"%s\")", s_sta_ssid);

    send_json(200, "{\"ok\":true}");
    // Erst antworten, dann umschalten (die Antwort soll den Client noch erreichen).
    s_apply_pending = true;
    s_apply_at = millis() + 500;
}

// ─── Einstellungen (/config) ─────────────────────────────────────────────────

/** Wie admin_write_ok, aber ohne Antwort (fuer den Upload-Handler, der vor
 *  dem eigentlichen Handler laeuft und nichts senden darf). */
static bool admin_write_allowed()
{
    return (via_ap() || s_server->authenticate(s_cfg.admin_user, s_cfg.admin_password)) &&
           s_server->header("X-Ferraris") == "1";
}

/** Schreibende Einstellungs-Endpunkte: Anmeldung + eigener Header (CSRF). */
static bool admin_write_ok()
{
    if (!admin_ok()) return false;
    if (s_server->header("X-Ferraris") != "1") {
        send_json(403, "{\"ok\":false,\"error\":\"Header X-Ferraris fehlt\"}");
        return false;
    }
    return true;
}

static void handle_config_page()
{
    if (!admin_ok()) return;
    s_server->send(200, "text/html; charset=utf-8", FERRARIS_PAGE_CONFIG);
}

static void handle_config_get()
{
    if (!admin_ok()) return;
    static ferraris_settings_t cur, def;          // nur loop()-Task
    static char buf[FERRARIS_SETTINGS_JSON_MAX];
    s_cfg.settings->get(&cur, &def);

    String out;
    out.reserve(2 * FERRARIS_SETTINGS_JSON_MAX);
    out += "{\"settings\":";
    int n = ferraris_settings_to_json(&cur, buf, sizeof(buf));
    out += (n > 0 && n < (int)sizeof(buf)) ? buf : "null";
    out += ",\"defaults\":";
    n = ferraris_settings_to_json(&def, buf, sizeof(buf));
    out += (n > 0 && n < (int)sizeof(buf)) ? buf : "null";
    out += ",\"restart_pending\":";
    out += s_cfg.settings->restart_pending() ? "true" : "false";
    out += ",\"autotune\":";
    out += s_cfg.settings->autotune ? "true" : "false";
    out += ",\"max_sensors\":";
    out += FERRARIS_SETTINGS_MAX_SENSORS;
    out += ",\"min_sensors\":";
    out += FERRARIS_SETTINGS_MIN_SENSORS;

    ferraris_status_t st;
    memset(&st, 0, sizeof(st));
    if (s_status_fn) s_status_fn(&st);
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    out += ",\"firmware\":{\"version\":";
    append_json_string(out, st.fw_version ? st.fw_version : "");
    out += ",\"partition\":";
    append_json_string(out, run ? run->label : "");
    out += ",\"ota_max\":";
    out += next ? (unsigned long)next->size : 0UL;

    ferraris_sysmon_info_t si;
    ferraris_sysmon_get_info(&si);
    out += ",\"ota_state\":";
    append_json_string(out, si.ota_state);
    out += ",\"ota_left_s\":";
    out += (unsigned long)si.ota_left_s;
    out += ",\"safe_mode\":";
    out += si.safe_mode ? "true" : "false";

    ferraris_coredump_info_t cd;
    ferraris_sysmon_coredump_info(&cd);
    out += ",\"coredump\":{\"present\":";
    out += cd.present ? "true" : "false";
    out += ",\"size\":";
    out += (unsigned long)cd.size;
    out += ",\"task\":";
    append_json_string(out, cd.task);
    out += ",\"pc\":";
    char pc[12];
    snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)cd.pc);
    append_json_string(out, cd.present ? pc : "");
    out += ",\"reason\":";
    append_json_string(out, cd.reason);
    out += "}}}";
    send_json(200, out);
}

static void config_error(int code, const char *why, const char *field)
{
    String out = "{\"ok\":false,\"error\":";
    append_json_string(out, why ? why : "Fehler");
    if (field) {
        out += ",\"field\":";
        append_json_string(out, field);
    }
    out += '}';
    send_json(code, out);
}

static void handle_config_save()
{
    if (!admin_write_ok()) return;
    static ferraris_settings_t s, def;            // nur loop()-Task
    s_cfg.settings->get(&s, &def);

    for (int i = 0; i < s_server->args(); i++) {
        const char *why = nullptr;
        String name = s_server->argName(i);
        if (!ferraris_settings_set_field(&s, name.c_str(), s_server->arg(i).c_str(), &why)) {
            config_error(400, why, name.c_str());
            return;
        }
    }
    String pw = s_server->arg("ap_password");
    if (!pw.isEmpty() && pw != s_server->arg("ap_password2")) {
        config_error(400, "Passwoerter stimmen nicht ueberein", "ap_password2");
        return;
    }

    const char *why = nullptr, *field = nullptr;
    if (!s_cfg.settings->apply(&s, &why, &field)) {
        config_error(400, why, field);
        return;
    }
    FLOGI(TAG, "Einstellungen gespeichert");
    String out = "{\"ok\":true,\"restart_pending\":";
    out += s_cfg.settings->restart_pending() ? "true" : "false";
    out += '}';
    send_json(200, out);
}

/** Neustart nach der laufenden Antwort (die soll den Client noch erreichen). */
static void schedule_restart(ferraris_restart_cause_t cause, const char *why)
{
    console_note("Neustart: %s", why);
    ferraris_sysmon_set_restart_cause(cause);
    if (s_mqtt && s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt, s_topic_status, "offline", 0, 1, 1);
    }
    s_restart_pending = true;
    s_restart_at = millis() + 500;
}

static void handle_restart()
{
    if (!admin_write_ok()) return;
    send_json(200, "{\"ok\":true}");
    schedule_restart(FERRARIS_RESTART_USER, "auf Anforderung der Webseite");
}

static void ota_fail(const String &why)
{
    if (s_ota_error.isEmpty()) s_ota_error = why;
    if (s_ota_active) Update.abort();
    s_ota_active = false;
}

/**
 * @brief Upload-Teil von POST /api/ota (multipart, Feld "firmware").
 *
 * Schreibt direkt in den freien OTA-Slot. Die Zaehlung laeuft weiter; beim
 * Flash-Schreiben koennen kurz Samples fehlen (der Sampler faengt sich).
 * Update.end(true) prueft das Image (Magic, Pruefsumme) und schaltet
 * otadata erst bei Erfolg um -> ein abgebrochener Upload aendert nichts.
 */
static void handle_ota_upload()
{
    HTTPUpload &up = s_server->upload();
    switch (up.status) {
        case UPLOAD_FILE_START:
            s_ota_started = true;
            s_ota_error   = "";
            s_ota_bytes   = 0;
            s_ota_active  = false;
            if (!admin_write_allowed()) {
                s_ota_error = "nicht angemeldet";   // Antwort sendet handle_ota_done
                return;
            }
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                ota_fail(String("Update-Start fehlgeschlagen: ") + Update.errorString());
                return;
            }
            s_ota_active = true;
            console_note("Firmware-Update: empfange \"%s\" ...", up.filename.c_str());
            break;
        case UPLOAD_FILE_WRITE:
            ferraris_sysmon_feed();      // Upload blockiert loop() bis zu ~30 s
            if (!s_ota_active) return;
            if (s_ota_bytes == 0 && up.currentSize > 0 && up.buf[0] != 0xE9) {
                ota_fail("Keine ESP32-App (firmware.bin verwenden, nicht factory.bin)");
                return;
            }
            if (Update.write(up.buf, up.currentSize) != up.currentSize) {
                ota_fail(String("Schreiben fehlgeschlagen: ") + Update.errorString());
                return;
            }
            s_ota_bytes += up.currentSize;
            break;
        case UPLOAD_FILE_END:
            if (!s_ota_active) return;
            s_ota_active = false;
            if (!Update.end(true)) {
                ota_fail(String("Image ungueltig: ") + Update.errorString());
            }
            break;
        case UPLOAD_FILE_ABORTED:
            ota_fail("Upload abgebrochen");
            break;
        default:
            break;
    }
}

static void handle_ota_done()
{
    const bool started = s_ota_started;
    s_ota_started = false;
    if (!admin_write_ok()) {      // sendet 401/403
        if (s_ota_active) Update.abort();
        s_ota_active = false;
        return;
    }
    if (!started) {
        config_error(400, "keine Datei im Feld firmware", nullptr);
        return;
    }
    if (!s_ota_error.isEmpty() || !Update.isFinished()) {
        console_note("Firmware-Update fehlgeschlagen: %s",
                     s_ota_error.isEmpty() ? "unvollstaendig" : s_ota_error.c_str());
        config_error(400, s_ota_error.isEmpty() ? "Update unvollstaendig" : s_ota_error.c_str(),
                     nullptr);
        return;
    }
    String out = "{\"ok\":true,\"bytes\":";
    out += (unsigned long)s_ota_bytes;
    out += '}';
    send_json(200, out);
    schedule_restart(FERRARIS_RESTART_OTA, "Firmware-Update installiert");
}

// ─── Absturzbericht ──────────────────────────────────────────────────────────

/** Coredump herunterladen (Rohformat, auswerten mit esp-coredump + firmware.elf). */
static void handle_coredump_get()
{
    if (!admin_ok()) return;
    ferraris_coredump_info_t cd;
    ferraris_sysmon_coredump_info(&cd);
    if (!cd.present) {
        s_server->send(404, "text/plain", "kein Absturzbericht");
        return;
    }
    s_server->sendHeader("Content-Disposition", "attachment; filename=\"ferraris-coredump.bin\"");
    s_server->sendHeader("Cache-Control", "no-store");
    s_server->setContentLength(cd.size);
    s_server->send(200, "application/octet-stream", "");
    static uint8_t buf[1024];                      // nur loop()-Task
    for (uint32_t off = 0; off < cd.size; off += sizeof(buf)) {
        size_t n = cd.size - off < sizeof(buf) ? cd.size - off : sizeof(buf);
        if (!ferraris_sysmon_coredump_read(off, buf, n)) break;
        s_server->sendContent((const char *)buf, n);
        ferraris_sysmon_feed();
    }
}

static void handle_coredump_delete()
{
    if (!admin_write_ok()) return;
    if (!ferraris_sysmon_coredump_erase()) {
        config_error(500, "Loeschen fehlgeschlagen", nullptr);
        return;
    }
    console_note("Absturzbericht geloescht");
    send_json(200, "{\"ok\":true}");
}

// ─── Automatisches Einmessen ─────────────────────────────────────────────────

static void handle_calib_get()
{
    if (!admin_ok()) return;
    static char buf[FERRARIS_CALIB_JSON_MAX + 256];   // nur loop()-Task
    int n = s_cfg.calib->status_json(buf, sizeof(buf));
    if (n < 0 || n >= (int)sizeof(buf)) {
        s_server->send(500, "text/plain", "Einmess-Status zu gross");
        return;
    }
    send_json(200, buf);
}

static void handle_calib_start()
{
    if (!admin_write_ok()) return;
    long dur = s_server->arg("duration_s").toInt();
    bool rev = s_server->arg("reverse") == "1";
    const char *why = nullptr;
    if (!s_cfg.calib->start(dur > 0 ? (uint32_t)dur : 0, rev, &why)) {
        config_error(400, why, "duration_s");
        return;
    }
    send_json(200, "{\"ok\":true}");
}

static void handle_calib_stop()
{
    if (!admin_write_ok()) return;
    if (s_server->arg("mode") == "cancel") s_cfg.calib->cancel();
    else s_cfg.calib->finish();
    send_json(200, "{\"ok\":true}");
}

static void handle_not_found()
{
    // Captive Portal: Anfragen an fremde Hosts (Connectivity-Checks von
    // Android/iOS/Windows) auf die Einrichtungsseite umleiten.
    if (via_ap() && s_server->hostHeader() != WiFi.softAPIP().toString()) {
        s_server->sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/wifi", true);
        s_server->send(302, "text/plain", "");
        return;
    }
    s_server->send(404, "text/plain", "nicht gefunden");
}

// ─── WLAN-Zustandsautomat ────────────────────────────────────────────────────

static void wifi_step()
{
    const uint32_t now = millis();
    const bool connected = WiFi.isConnected();

    if (s_apply_pending && (int32_t)(now - s_apply_at) >= 0) {
        s_apply_pending = false;
        WiFi.disconnect(false, false);
        s_was_connected = false;
        s_lost_ms = 0;
        sta_begin();
        return;
    }

    if (connected) {
        if (!s_was_connected) {
            s_was_connected = true;
            s_attempting    = false;
            s_connected_ms  = now;
            s_lost_ms       = 0;
            String ip = WiFi.localIP().toString();
            console_note("WLAN verbunden: \"%s\", IP %s, RSSI %d dBm -> http://%s/ bzw. http://%s.local/",
                         s_sta_ssid, ip.c_str(), WiFi.RSSI(), ip.c_str(), s_cfg.hostname);
            console_pause();
            mdns_start();
            mqtt_start();
        }
        if (s_ap_on && now - s_connected_ms > s_cfg.ap_off_delay_ms && ap_clients() == 0) {
            ap_stop();
        }
        return;
    }

    if (s_was_connected) {
        s_was_connected = false;
        s_wifi_reconnects++;
        s_lost_ms = now ? now : 1;
        FLOGW(TAG, "WLAN-Verbindung verloren - verbinde neu");
        sta_begin();
        return;
    }

    if (s_attempting && now - s_attempt_ms > s_cfg.sta_connect_timeout_ms) {
        s_attempting = false;
        WiFi.disconnect(false, false);   // STA ruhig stellen, damit der AP-Kanal stabil bleibt
        if (s_lost_ms && now - s_lost_ms < s_cfg.sta_lost_to_ap_ms) {
            sta_begin();                 // im Betrieb verloren: erst noch weiter versuchen
        } else if (!s_ap_on) {
            FLOGW(TAG, "WLAN \"%s\" nicht erreichbar - starte Einrichtungs-AP", s_sta_ssid);
            ap_start();
        }
        return;
    }

    if (!s_attempting && s_sta_ssid[0] && ap_clients() == 0 &&
        now - s_attempt_ms > s_cfg.ap_retry_interval_ms) {
        sta_begin();
    }
}

// ─── API ─────────────────────────────────────────────────────────────────────

void ferraris_net_begin(const ferraris_net_config_t *cfg,
                        ferraris_status_fn status_fn, ferraris_csv_fn csv_fn)
{
    s_cfg       = *cfg;
    s_status_fn = status_fn;
    s_csv_fn    = csv_fn;
    if (s_cfg.mqtt_interval_ms == 0) s_cfg.mqtt_interval_ms = 10000;

    uint64_t mac = ESP.getEfuseMac();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", s_cfg.ap_ssid_prefix,
             (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));

    load_credentials();

    WiFi.persistent(false);          // Arduino soll nichts selbst ins NVS schreiben
    WiFi.setAutoReconnect(false);    // Wiederverbinden steuert wifi_step()
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_cfg.hostname);
    WiFi.setSleep(false);            // Netzbetrieb: Antwortzeit vor Stromsparen

    s_server = new WebServer(s_cfg.http_port);
    s_server->on("/", HTTP_GET, handle_index);
    s_server->on("/wifi", HTTP_GET, handle_wifi_page);
    s_server->on("/api/status", HTTP_GET, handle_status);
    s_server->on("/api/history", HTTP_GET, handle_history);
    s_server->on("/api/csv", HTTP_GET, handle_csv);
    s_server->on("/api/scan", HTTP_GET, handle_scan);
    s_server->on("/api/wifi", HTTP_POST, handle_wifi_save);
    if (s_cfg.settings) {
        s_server->on("/config", HTTP_GET, handle_config_page);
        s_server->on("/api/config", HTTP_GET, handle_config_get);
        s_server->on("/api/config", HTTP_POST, handle_config_save);
        s_server->on("/api/restart", HTTP_POST, handle_restart);
        s_server->on("/api/ota", HTTP_POST, handle_ota_done, handle_ota_upload);
        s_server->on("/api/coredump", HTTP_GET, handle_coredump_get);
        s_server->on("/api/coredump/delete", HTTP_POST, handle_coredump_delete);
        if (s_cfg.calib) {
            s_server->on("/api/calib", HTTP_GET, handle_calib_get);
            s_server->on("/api/calib/start", HTTP_POST, handle_calib_start);
            s_server->on("/api/calib/stop", HTTP_POST, handle_calib_stop);
        }
    }
    s_server->onNotFound(handle_not_found);
    const char *headers[] = { "Host", "X-Ferraris" };
    s_server->collectHeaders(headers, 2);
    s_server->begin();

    if (s_sta_ssid[0]) {
        sta_begin();
    } else {
        FLOGW(TAG, "Kein WLAN konfiguriert - starte Einrichtungs-AP");
        ap_start();
    }
}

bool ferraris_net_healthy(void)
{
    if (!s_server) return false;
    if (WiFi.isConnected()) return true;
    // Ohne Heimnetz zaehlt der Access Point: mit verbundenem Geraet oder wenn
    // noch gar kein WLAN eingerichtet ist (dann ist der AP der Normalfall).
    return s_ap_on && (ap_clients() > 0 || !s_sta_ssid[0]);
}

void ferraris_net_loop(void)
{
    if (!s_server) return;
    if (s_restart_pending && (int32_t)(millis() - s_restart_at) >= 0) {
        ESP.restart();
    }
    wifi_step();
    if (s_ap_on) s_dns.processNextRequest();
    s_server->handleClient();
    history_sample();
    mqtt_publish_if_due();
}
