/**
 * @file ferraris_power.c
 * @brief Momentanleistung aus den Quadratur-Schritten (pure C).
 */

#include "ferraris_power.h"
#include <string.h>

void ferraris_power_init(ferraris_power_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void ferraris_power_step(ferraris_power_t *p, int dir, uint32_t now_ms)
{
    if (!p || dir == 0) return;
    dir = dir > 0 ? 1 : -1;

    if (dir != p->dir) {
        // Richtungswechsel: alte Zeitstempel gehoeren zu einer anderen Drehung.
        p->dir = (int8_t)dir;
        p->n   = 0;
        p->idx = 0;
        p->period_ms = 0;
    }

    if (p->n == 4) {
        // step_ms[idx] ist der Schritt von vor genau 4 Schritten = 1 Umdrehung.
        uint32_t period = now_ms - p->step_ms[p->idx];
        if (period == 0) period = 1;
        if (period > (uint32_t)INT32_MAX) period = (uint32_t)INT32_MAX;
        p->period_ms = dir * (int32_t)period;
    }

    p->step_ms[p->idx] = now_ms;
    p->idx = (uint8_t)((p->idx + 1) & 3);
    if (p->n < 4) p->n++;
    p->last_step_ms = now_ms;
}

float ferraris_power_watts(const ferraris_power_t *p, int k_rev_per_kwh,
                           uint32_t now_ms, uint32_t timeout_ms)
{
    if (!p || k_rev_per_kwh <= 0) return 0.0f;

    int32_t period = p->period_ms;
    if (period == 0) return 0.0f;

    uint32_t since = now_ms - p->last_step_ms;
    if (timeout_ms > 0 && since > timeout_ms) return 0.0f;

    uint32_t abs_period = (uint32_t)(period < 0 ? -period : period);
    // Laenger als eine Periode kein Schritt -> Scheibe ist langsamer geworden.
    uint32_t eff = since > abs_period ? since : abs_period;

    float w = 3.6e9f / ((float)k_rev_per_kwh * (float)eff);
    return period < 0 ? -w : w;
}
