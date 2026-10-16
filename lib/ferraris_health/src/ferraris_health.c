/**
 * @file ferraris_health.c
 * @brief Entscheidungslogik fuer Robustheit (pure C).
 */

#include "ferraris_health.h"
#include <string.h>

void ferraris_ota_verify_init(ferraris_ota_verify_t *v, uint32_t min_ms, uint32_t stable_ms,
                              uint32_t timeout_ms)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->min_ms     = min_ms;
    v->stable_ms  = stable_ms;
    v->timeout_ms = timeout_ms;
}

ferraris_ota_verdict_t ferraris_ota_verify_step(ferraris_ota_verify_t *v, uint32_t uptime_ms,
                                                bool healthy)
{
    if (!v) return FERRARIS_OTA_WAIT;
    if (!healthy) {
        v->healthy_seen = false;          // muss UNUNTERBROCHEN gesund sein
    } else if (!v->healthy_seen) {
        v->healthy_seen     = true;
        v->healthy_since_ms = uptime_ms;
    }
    if (v->healthy_seen && uptime_ms >= v->min_ms &&
        uptime_ms - v->healthy_since_ms >= v->stable_ms) {
        return FERRARIS_OTA_VALID;
    }
    if (uptime_ms >= v->timeout_ms) return FERRARIS_OTA_ROLLBACK;
    return FERRARIS_OTA_WAIT;
}

uint32_t ferraris_crash_streak_next(uint32_t prev, bool crash_reset)
{
    if (!crash_reset) return 0;
    return prev < UINT32_MAX ? prev + 1 : prev;
}

bool ferraris_safe_mode(uint32_t crash_streak, uint32_t threshold)
{
    return threshold > 0 && crash_streak >= threshold;
}
