/**
 * @file ferraris_counter_store.cpp
 * @brief Zaehlerstand im NVS (Arduino Preferences), siehe Header.
 */

#include "ferraris_counter_store.h"

#include <Preferences.h>

static const char *NVS_NAMESPACE = "ferrcnt";
static const char *KEY           = "cnt";

typedef struct {
    uint32_t version;     // 1
    int32_t  forward;
    int32_t  backward;
} counter_blob_v1_t;

typedef struct {
    uint32_t       version;   // 2
    int32_t        forward;
    int32_t        backward;
    ferraris_day_t day;
} counter_blob_v2_t;

bool ferraris_counter_load(ferraris_counter_state_t *st)
{
    Preferences p;
    // Read-only-Open schlaegt fehl, solange der Namespace nie geschrieben wurde.
    if (!p.begin(NVS_NAMESPACE, true)) return false;
    // NVS prueft die CRC des Blobs selbst; hier nur Laenge, Version, Plausibilitaet.
    const size_t len = p.getBytesLength(KEY);
    counter_blob_v2_t b = {};
    bool ok = false;
    if (len == sizeof(counter_blob_v2_t)) {
        ok = p.getBytes(KEY, &b, sizeof(b)) == sizeof(b) && b.version == 2;
    } else if (len == sizeof(counter_blob_v1_t)) {
        counter_blob_v1_t v1 = {};
        ok = p.getBytes(KEY, &v1, sizeof(v1)) == sizeof(v1) && v1.version == 1;
        b.forward  = v1.forward;
        b.backward = v1.backward;
    }
    p.end();
    if (!ok || b.forward < 0 || b.backward < 0) return false;
    if (st) {
        st->forward   = b.forward;
        st->backward  = b.backward;
        st->day       = b.day;
        st->day_valid = b.version == 2;
    }
    return true;
}

bool ferraris_counter_save(const ferraris_counter_state_t *st)
{
    if (!st) return false;
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    const counter_blob_v2_t b = { 2, st->forward, st->backward, st->day };
    const bool ok = p.putBytes(KEY, &b, sizeof(b)) == sizeof(b);
    p.end();
    return ok;
}
