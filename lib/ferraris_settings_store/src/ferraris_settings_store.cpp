/**
 * @file ferraris_settings_store.cpp
 * @brief Einstellungen im NVS (Arduino Preferences).
 */

#include "ferraris_settings_store.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

static const char *NVS_NAMESPACE = "ferrcfg";

// Keys max. 15 Zeichen (NVS-Grenze).
static void load_str(Preferences &p, const char *key, char *dst, size_t size, bool *any)
{
    if (!p.isKey(key)) return;
    String v = p.getString(key, "");
    if (v.length() >= size) return;   // passt nicht (z. B. alte Version) -> Default behalten
    memcpy(dst, v.c_str(), v.length() + 1);
    *any = true;
}

bool ferraris_settings_load(ferraris_settings_t *s)
{
    Preferences p;
    // Read-only-Open schlaegt fehl, solange der Namespace nie geschrieben wurde.
    if (!s || !p.begin(NVS_NAMESPACE, true)) return false;
    bool any = false;
    char key[8];
    if (p.isKey("n_sens"))  { s->sensor_count     = p.getInt("n_sens"); any = true; }
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        snprintf(key, sizeof(key), "gpio%d", i + 1);
        if (p.isKey(key)) { s->sensor_gpio[i] = p.getInt(key); any = true; }
    }
    if (p.isKey("k"))       { s->revs_per_kwh     = p.getInt("k");    any = true; }
    // Schwellen je Eingang; aeltere Firmware hatte eine fuer alle (thr_hi/thr_lo).
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        snprintf(key, sizeof(key), "hi%d", i + 1);
        if (p.isKey(key))           { s->threshold_high[i] = p.getInt(key); any = true; }
        else if (p.isKey("thr_hi")) { s->threshold_high[i] = p.getInt("thr_hi"); any = true; }
        snprintf(key, sizeof(key), "lo%d", i + 1);
        if (p.isKey(key))           { s->threshold_low[i] = p.getInt(key); any = true; }
        else if (p.isKey("thr_lo")) { s->threshold_low[i] = p.getInt("thr_lo"); any = true; }
    }
    if (p.isKey("cnt_a"))   { s->count_a          = p.getInt("cnt_a"); any = true; }
    if (p.isKey("cnt_b"))   { s->count_b          = p.getInt("cnt_b"); any = true; }
    if (p.isKey("env_auto")){ s->envelope_auto    = p.getBool("env_auto"); any = true; }
    if (p.isKey("mqtt_ivl")){ s->mqtt_interval_ms = p.getUInt("mqtt_ivl"); any = true; }
    load_str(p, "host", s->hostname, sizeof(s->hostname), &any);
    load_str(p, "mqtt_uri", s->mqtt_uri, sizeof(s->mqtt_uri), &any);
    load_str(p, "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user), &any);
    load_str(p, "mqtt_pass", s->mqtt_password, sizeof(s->mqtt_password), &any);
    load_str(p, "mqtt_prefix", s->mqtt_prefix, sizeof(s->mqtt_prefix), &any);
    load_str(p, "ap_pass", s->ap_password, sizeof(s->ap_password), &any);
    p.end();
    return any;
}

bool ferraris_settings_save(const ferraris_settings_t *s)
{
    Preferences p;
    if (!s || !p.begin(NVS_NAMESPACE, false)) return false;
    bool ok = p.putInt("n_sens", s->sensor_count) > 0;
    char key[8];
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        snprintf(key, sizeof(key), "gpio%d", i + 1);
        ok &= p.putInt(key, s->sensor_gpio[i]) > 0;
    }
    ok &= p.putInt("k", s->revs_per_kwh) > 0;
    for (int i = 0; i < FERRARIS_SETTINGS_MAX_SENSORS; i++) {
        snprintf(key, sizeof(key), "hi%d", i + 1);
        ok &= p.putInt(key, s->threshold_high[i]) > 0;
        snprintf(key, sizeof(key), "lo%d", i + 1);
        ok &= p.putInt(key, s->threshold_low[i]) > 0;
    }
    // Alte gemeinsame Schwelle: ab jetzt je Eingang (remove() loggt bei fehlendem Key).
    if (p.isKey("thr_hi")) p.remove("thr_hi");
    if (p.isKey("thr_lo")) p.remove("thr_lo");
    ok &= p.putInt("cnt_a", s->count_a) > 0;
    ok &= p.putInt("cnt_b", s->count_b) > 0;
    ok &= p.putBool("env_auto", s->envelope_auto) > 0;
    ok &= p.putUInt("mqtt_ivl", s->mqtt_interval_ms) > 0;
    ok &= p.putString("host", s->hostname) > 0;
    // Leere Strings schreiben 0 Bytes -> nicht als Fehler werten.
    p.putString("mqtt_uri", s->mqtt_uri);
    p.putString("mqtt_user", s->mqtt_user);
    p.putString("mqtt_pass", s->mqtt_password);
    ok &= p.putString("mqtt_prefix", s->mqtt_prefix) > 0;
    ok &= p.putString("ap_pass", s->ap_password) > 0;
    p.end();
    return ok;
}
