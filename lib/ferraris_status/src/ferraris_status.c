/**
 * @file ferraris_status.c
 * @brief JSON-Formatierung des Status-Snapshots (pure C).
 */

#include "ferraris_status.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct {
    char  *buf;
    size_t len;
    int    pos;      // geschriebene Zeichen (auch ueber len hinaus gezaehlt)
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (o->pos < 0) return;
    size_t room = (size_t)o->pos < o->len ? o->len - (size_t)o->pos : 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(room ? o->buf + o->pos : NULL, room, fmt, ap);
    va_end(ap);
    o->pos = n < 0 ? -1 : o->pos + n;
}

// Namen kommen aus der Firmware (Konstanten), trotzdem JSON-sicher ausgeben.
static void put_str(out_t *o, const char *s)
{
    put(o, "\"");
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') put(o, "\\%c", *p);
        else if ((unsigned char)*p < 0x20) put(o, "\\u%04x", (unsigned char)*p);
        else put(o, "%c", *p);
    }
    put(o, "\"");
}

int ferraris_status_to_json(const ferraris_status_t *st, char *buf, size_t len)
{
    if (!st || !buf || len == 0) return -1;
    out_t o = { buf, len, 0 };

    put(&o, "{\"uptime_s\":%lu,\"counting\":%s,",
        (unsigned long)st->uptime_s, st->counting ? "true" : "false");
    put(&o, "\"meter\":{\"k\":%ld,\"steps_import\":%ld,\"steps_export\":%ld,"
            "\"import_kwh\":%.4f,\"export_kwh\":%.4f,\"net_kwh\":%.4f,\"power_w\":%.1f},",
        (long)st->revs_per_kwh, (long)st->steps_import, (long)st->steps_export,
        (double)st->import_kwh, (double)st->export_kwh, (double)st->net_kwh,
        (double)st->power_w);
    put(&o, "\"schmitt\":{\"high\":%d,\"low\":%d},\"count_pair\":[",
        st->threshold_high, st->threshold_low);
    put_str(&o, st->count_a);
    put(&o, ",");
    put_str(&o, st->count_b);
    put(&o, "],\"sensors\":[");

    int n = st->n_sensors;
    if (n < 0) n = 0;
    if (n > FERRARIS_STATUS_MAX_SENSORS) n = FERRARIS_STATUS_MAX_SENSORS;
    for (int i = 0; i < n; i++) {
        const ferraris_sensor_status_t *s = &st->sensors[i];
        put(&o, "%s{\"name\":", i ? "," : "");
        put_str(&o, s->name);
        put(&o, ",\"gpio\":%d,\"raw\":%d,\"min\":%d,\"max\":%d,\"hw\":",
            s->gpio, s->raw, s->run_min, s->run_max);
        put_str(&o, s->hw);
        put(&o, ",\"high\":%d,\"low\":%d}", s->high, s->low);
    }

    put(&o, "],\"wifi\":{\"connected\":%s,\"ssid\":", st->wifi_connected ? "true" : "false");
    put_str(&o, st->wifi_ssid);
    put(&o, ",\"rssi\":%d,\"ip\":", st->wifi_rssi);
    put_str(&o, st->wifi_ip);
    put(&o, ",\"ap\":{\"active\":%s,\"ssid\":", st->ap_active ? "true" : "false");
    put_str(&o, st->ap_ssid);
    put(&o, ",\"ip\":");
    put_str(&o, st->ap_ip);
    put(&o, ",\"clients\":%d}},\"adc\":{\"sets_per_s\":%lu,\"restarts\":%lu,\"errors\":%lu,"
            "\"timeouts\":%lu},\"health\":{\"reset\":",
        st->ap_clients, (unsigned long)st->adc_sets_per_s, (unsigned long)st->adc_restarts,
        (unsigned long)st->adc_errors, (unsigned long)st->adc_timeouts);
    put_str(&o, st->reset_reason);
    put(&o, ",\"boots\":%lu,\"crashes\":%lu,\"safe_mode\":%s,\"heap_free\":%lu,\"heap_min\":%lu,"
            "\"heap_block\":%lu,\"wifi_reconnects\":%lu,\"mqtt\":",
        (unsigned long)st->boot_count, (unsigned long)st->crash_count,
        st->safe_mode ? "true" : "false", (unsigned long)st->heap_free,
        (unsigned long)st->heap_min, (unsigned long)st->heap_block,
        (unsigned long)st->wifi_reconnects);
    put_str(&o, st->mqtt_state ? st->mqtt_state : "aus");
    put(&o, ",\"coredump\":%s,\"ota\":", st->coredump ? "true" : "false");
    put_str(&o, st->ota_state ? st->ota_state : "ok");
    put(&o, "},\"fw\":");
    put_str(&o, st->fw_version);
    put(&o, ",\"host\":");
    put_str(&o, st->hostname);
    put(&o, "}");

    return o.pos;
}
