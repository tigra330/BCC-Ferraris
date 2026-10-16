/**
 * @file ferraris_counter_store.cpp
 * @brief Zaehlerstand im NVS (Arduino Preferences), siehe Header.
 */

#include "ferraris_counter_store.h"

#include <Preferences.h>

static const char *NVS_NAMESPACE = "ferrcnt";
static const char *KEY           = "cnt";

#define COUNTER_VERSION 1u

typedef struct {
    uint32_t version;
    int32_t  forward;
    int32_t  backward;
} counter_blob_t;

bool ferraris_counter_load(int32_t *forward, int32_t *backward)
{
    Preferences p;
    // Read-only-Open schlaegt fehl, solange der Namespace nie geschrieben wurde.
    if (!p.begin(NVS_NAMESPACE, true)) return false;
    counter_blob_t b = {};
    // NVS prueft die CRC des Blobs selbst; hier nur Laenge, Version, Plausibilitaet.
    const bool ok = p.getBytesLength(KEY) == sizeof(b) &&
                    p.getBytes(KEY, &b, sizeof(b)) == sizeof(b) &&
                    b.version == COUNTER_VERSION && b.forward >= 0 && b.backward >= 0;
    p.end();
    if (!ok) return false;
    if (forward)  *forward  = b.forward;
    if (backward) *backward = b.backward;
    return true;
}

bool ferraris_counter_save(int32_t forward, int32_t backward)
{
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    const counter_blob_t b = { COUNTER_VERSION, forward, backward };
    const bool ok = p.putBytes(KEY, &b, sizeof(b)) == sizeof(b);
    p.end();
    return ok;
}
