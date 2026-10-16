/**
 * @file ferraris_settings.c
 * @brief Editierbare Einstellungen: Feld-Parser, Pruefung, JSON (pure C).
 */

#include "ferraris_settings.h"
#include "ferraris_hwcheck.h"
#include "ferraris_netutil.h"
#include <errno.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Feld-Parser ─────────────────────────────────────────────────────────────

static bool fail(const char **why, const char *msg)
{
    if (why) *why = msg;
    return false;
}

static bool parse_long(const char *v, long *out)
{
    if (!v || !*v) return false;
    char *end = NULL;
    errno = 0;
    long x = strtol(v, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) end++;
    // Grenze weit ueber allen Feldbereichen: kein Ueberlauf beim int-Zuweisen.
    if (errno || !end || *end || x < -1000000000L || x > 1000000000L) return false;
    *out = x;
    return true;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!v) return false;
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on"))   { *out = true;  return true; }
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !*v) { *out = false; return true; }
    return false;
}

static bool copy_str(char *dst, size_t size, const char *v, const char **why, const char *too_long)
{
    size_t n = v ? strlen(v) : 0;
    if (n >= size) return fail(why, too_long);
    memcpy(dst, v ? v : "", n);
    dst[n] = '\0';
    return true;
}

bool ferraris_settings_set_field(ferraris_settings_t *s, const char *name, const char *value,
                                 const char **why)
{
    if (why) *why = NULL;
    if (!s || !name) return fail(why, "kein Feld");
    long x;

#define INT_FIELD(key, member, msg)                                  \
    if (!strcmp(name, key)) {                                        \
        if (!parse_long(value, &x)) return fail(why, msg);           \
        s->member = x;                                               \
        return true;                                                 \
    }
    INT_FIELD("k", revs_per_kwh, "k muss eine ganze Zahl sein")
    INT_FIELD("count_a", count_a, "Sensor A muss eine ganze Zahl sein")
    INT_FIELD("count_b", count_b, "Sensor B muss eine ganze Zahl sein")
    INT_FIELD("sensor_count", sensor_count, "Anzahl Eingaenge muss eine ganze Zahl sein")
#undef INT_FIELD

    // Je Eingang: "gpio1".."gpio8", "high1".."high8", "low1".."low8"
    static const struct {
        const char *prefix;
        size_t      off;      // Offset des int-Arrays in ferraris_settings_t
        const char *msg;
    } per_input[] = {
        { "gpio", offsetof(ferraris_settings_t, sensor_gpio),    "GPIO muss eine ganze Zahl sein" },
        { "high", offsetof(ferraris_settings_t, threshold_high), "HIGH-Schwelle muss eine ganze Zahl sein" },
        { "low",  offsetof(ferraris_settings_t, threshold_low),  "LOW-Schwelle muss eine ganze Zahl sein" },
    };
    for (size_t k = 0; k < sizeof(per_input) / sizeof(per_input[0]); k++) {
        size_t pl = strlen(per_input[k].prefix);
        if (strncmp(name, per_input[k].prefix, pl) || name[pl] < '1' ||
            name[pl] >= '1' + FERRARIS_SETTINGS_MAX_SENSORS || name[pl + 1]) {
            continue;
        }
        if (!parse_long(value, &x)) return fail(why, per_input[k].msg);
        int *arr = (int *)((char *)s + per_input[k].off);
        arr[name[pl] - '1'] = (int)x;
        return true;
    }

    // Aeltere API: eine Schwelle fuer alle Eingaenge
    if (!strcmp(name, "threshold_high") || !strcmp(name, "threshold_low")) {
        bool high = name[10] == 'h';
        if (!parse_long(value, &x)) {
            return fail(why, high ? "HIGH-Schwelle muss eine ganze Zahl sein"
                                  : "LOW-Schwelle muss eine ganze Zahl sein");
        }
        for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
            (high ? s->threshold_high : s->threshold_low)[i] = (int)x;
        }
        return true;
    }

    if (!strcmp(name, "mqtt_interval_s")) {
        if (!parse_long(value, &x) || x < 1 || x > 3600) {
            return fail(why, "MQTT-Intervall muss 1..3600 s sein");
        }
        s->mqtt_interval_ms = (uint32_t)x * 1000u;
        return true;
    }
    if (!strcmp(name, "envelope_auto")) {
        return parse_bool(value, &s->envelope_auto) || fail(why, "Huelle: ungueltiger Wert");
    }

#define STR_FIELD(key, member, msg) \
    if (!strcmp(name, key)) return copy_str(s->member, sizeof(s->member), value, why, msg);
    STR_FIELD("hostname", hostname, "Hostname laenger als 32 Zeichen")
    STR_FIELD("mqtt_uri", mqtt_uri, "MQTT-URI laenger als 127 Zeichen")
    STR_FIELD("mqtt_user", mqtt_user, "MQTT-Benutzer laenger als 64 Zeichen")
    STR_FIELD("mqtt_prefix", mqtt_prefix, "Topic-Praefix laenger als 64 Zeichen")
#undef STR_FIELD

    // Passwoerter: leer = unveraendert (das Formular kennt den alten Wert nicht).
    if (!strcmp(name, "mqtt_password")) {
        if (!value || !*value) return true;
        return copy_str(s->mqtt_password, sizeof(s->mqtt_password), value, why,
                        "MQTT-Passwort laenger als 64 Zeichen");
    }
    if (!strcmp(name, "ap_password")) {
        if (!value || !*value) return true;
        return copy_str(s->ap_password, sizeof(s->ap_password), value, why,
                        "Passwort laenger als 63 Zeichen");
    }
    return true;   // unbekannt -> ignorieren
}

// ─── Pruefung ────────────────────────────────────────────────────────────────

static bool printable(const char *p)
{
    for (; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static bool hostname_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || h[0] == '-' || h[n - 1] == '-') return false;
    for (const char *p = h; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool uri_ok(const char *u)
{
    if (!*u) return true;   // kein MQTT
    static const char *const schemes[] = { "mqtt://", "mqtts://", "ws://", "wss://" };
    bool scheme = false;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t n = strlen(schemes[i]);
        if (!strncmp(u, schemes[i], n) && u[n]) scheme = true;
    }
    if (!scheme) return false;
    for (const char *p = u; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static bool prefix_ok(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || t[0] == '/' || t[n - 1] == '/') return false;
    for (const char *p = t; *p; p++) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7f || *p == '+' || *p == '#') return false;
    }
    return true;
}

/** Eingaenge: Feldname/Meldung des ersten Fehlers, sonst NULL. */
static const char *check_inputs(const ferraris_settings_t *s, const char **field)
{
    static const char *const names[FERRARIS_SETTINGS_MAX_SENSORS] = {
        "gpio1", "gpio2", "gpio3", "gpio4", "gpio5", "gpio6", "gpio7", "gpio8",
    };
    static const char *const highs[FERRARIS_SETTINGS_MAX_SENSORS] = {
        "high1", "high2", "high3", "high4", "high5", "high6", "high7", "high8",
    };
    static const char *const lows[FERRARIS_SETTINGS_MAX_SENSORS] = {
        "low1", "low2", "low3", "low4", "low5", "low6", "low7", "low8",
    };
    if (s->sensor_count < FERRARIS_SETTINGS_MIN_SENSORS ||
        s->sensor_count > FERRARIS_SETTINGS_MAX_SENSORS) {
        *field = "sensor_count";
        return "Anzahl Eingaenge muss 2..8 sein";
    }
    for (int i = 0; i < s->sensor_count; i++) {
        if (ferraris_gpio_to_adc1_channel(s->sensor_gpio[i]) < 0) {
            *field = names[i];
            return "Eingang muss ein ADC1-Pin sein (GPIO32..39)";
        }
        for (int j = 0; j < i; j++) {
            if (s->sensor_gpio[j] == s->sensor_gpio[i]) {
                *field = names[i];
                return "GPIO wird schon von einem anderen Eingang benutzt";
            }
        }
    }
    for (int i = 0; i < s->sensor_count; i++) {
        if (s->threshold_high[i] < 1 || s->threshold_high[i] > 4095) {
            *field = highs[i];
            return "HIGH-Schwelle muss 1..4095 sein";
        }
        if (s->threshold_low[i] < 0 || s->threshold_low[i] > 4094) {
            *field = lows[i];
            return "LOW-Schwelle muss 0..4094 sein";
        }
        if (s->threshold_high[i] <= s->threshold_low[i]) {
            *field = highs[i];
            return "HIGH-Schwelle muss groesser als LOW sein";
        }
    }
    return NULL;
}

bool ferraris_settings_validate(const ferraris_settings_t *s, const char **why, const char **field)
{
    const char *msg = NULL, *f = NULL;
    size_t apl = s ? strlen(s->ap_password) : 0;
    const char *in_msg = NULL, *in_field = NULL;
    if (s) in_msg = check_inputs(s, &in_field);

#define CHECK(cond, name, text) else if (!(cond)) { f = name; msg = text; }
    if (!s) { msg = "keine Einstellungen"; }
    else if (in_msg) { msg = in_msg; f = in_field; }
    CHECK(s->revs_per_kwh >= 1 && s->revs_per_kwh <= 10000, "k", "k muss 1..10000 U/kWh sein")
    CHECK(s->count_a >= 1 && s->count_a <= s->sensor_count, "count_a",
          "Sensor A muss einer der aktiven Eingaenge sein")
    CHECK(s->count_b >= 1 && s->count_b <= s->sensor_count, "count_b",
          "Sensor B muss einer der aktiven Eingaenge sein")
    CHECK(s->count_a != s->count_b, "count_b", "Sensor A und B muessen verschieden sein")
    CHECK(hostname_ok(s->hostname), "hostname",
          "Hostname: 1..32 Zeichen a-z, 0-9 und '-' (nicht am Anfang/Ende)")
    CHECK(uri_ok(s->mqtt_uri), "mqtt_uri",
          "MQTT-URI muss mit mqtt://, mqtts://, ws:// oder wss:// beginnen (ohne Leerzeichen)")
    CHECK(printable(s->mqtt_user), "mqtt_user", "MQTT-Benutzer enthaelt Steuerzeichen")
    CHECK(printable(s->mqtt_password), "mqtt_password", "MQTT-Passwort enthaelt Steuerzeichen")
    CHECK(prefix_ok(s->mqtt_prefix), "mqtt_prefix",
          "Topic-Praefix: nicht leer, ohne Leerzeichen, '+', '#' und '/' am Anfang/Ende")
    CHECK(s->mqtt_interval_ms >= 1000 && s->mqtt_interval_ms <= 3600000u, "mqtt_interval_s",
          "MQTT-Intervall muss 1..3600 s sein")
    CHECK(apl >= 8 && apl <= FERRARIS_SETTINGS_APPASS_MAX, "ap_password",
          "Passwort muss 8..63 Zeichen haben")
    CHECK(printable(s->ap_password), "ap_password", "Passwort enthaelt Steuerzeichen")
#undef CHECK

    if (why) *why = msg;
    if (field) *field = f;
    return msg == NULL;
}

bool ferraris_settings_needs_restart(const ferraris_settings_t *boot, const ferraris_settings_t *now)
{
    if (!boot || !now) return false;
    if (boot->sensor_count != now->sensor_count) return true;
    for (int i = 0; i < now->sensor_count && i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        if (boot->sensor_gpio[i] != now->sensor_gpio[i]) return true;
    }
    return boot->count_a != now->count_a || boot->count_b != now->count_b ||
           strcmp(boot->hostname, now->hostname) || strcmp(boot->mqtt_uri, now->mqtt_uri) ||
           strcmp(boot->mqtt_user, now->mqtt_user) ||
           strcmp(boot->mqtt_password, now->mqtt_password) ||
           strcmp(boot->mqtt_prefix, now->mqtt_prefix) ||
           boot->mqtt_interval_ms != now->mqtt_interval_ms ||
           strcmp(boot->ap_password, now->ap_password);
}

// ─── JSON ────────────────────────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t len;
    int    pos;      // geschriebene Zeichen (auch ueber len hinaus gezaehlt)
} out_t;

static size_t room(const out_t *o)
{
    return (size_t)o->pos < o->len ? o->len - (size_t)o->pos : 0;
}

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (o->pos < 0) return;
    size_t r = room(o);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r ? o->buf + o->pos : NULL, r, fmt, ap);
    va_end(ap);
    o->pos = n < 0 ? -1 : o->pos + n;
}

static void put_str(out_t *o, const char *key, const char *s)
{
    put(o, "\"%s\":", key);
    if (o->pos < 0) return;
    size_t r = room(o);
    int n = ferraris_json_string(r ? o->buf + o->pos : NULL, r, s);
    o->pos = n < 0 ? -1 : o->pos + n;
}

int ferraris_settings_to_json(const ferraris_settings_t *s, char *buf, size_t len)
{
    if (!s || !buf || len == 0) return -1;
    out_t o = { buf, len, 0 };
    put(&o, "{\"sensor_count\":%d,\"gpio\":[", s->sensor_count);
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        put(&o, "%s%d", i ? "," : "", s->sensor_gpio[i]);
    }
    put(&o, "],\"high\":[");
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        put(&o, "%s%d", i ? "," : "", s->threshold_high[i]);
    }
    put(&o, "],\"low\":[");
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        put(&o, "%s%d", i ? "," : "", s->threshold_low[i]);
    }
    put(&o, "],\"k\":%ld,\"count_a\":%d,\"count_b\":%d,\"envelope_auto\":%s,",
        (long)s->revs_per_kwh, s->count_a, s->count_b, s->envelope_auto ? "true" : "false");
    put_str(&o, "hostname", s->hostname);
    put(&o, ",");
    put_str(&o, "mqtt_uri", s->mqtt_uri);
    put(&o, ",");
    put_str(&o, "mqtt_user", s->mqtt_user);
    put(&o, ",\"mqtt_password_set\":%s,", s->mqtt_password[0] ? "true" : "false");
    put_str(&o, "mqtt_prefix", s->mqtt_prefix);
    put(&o, ",\"mqtt_interval_s\":%lu,\"ap_password_set\":%s}",
        (unsigned long)(s->mqtt_interval_ms / 1000u), s->ap_password[0] ? "true" : "false");
    if (o.pos >= 0) buf[(size_t)o.pos < len ? (size_t)o.pos : len - 1] = '\0';
    return o.pos;
}
