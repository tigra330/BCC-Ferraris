/**
 * @file ferraris_day.c
 * @brief Tageswerte und Vortag (pure C), siehe Header.
 */

#include "ferraris_day.h"
#include <string.h>

int32_t ferraris_day_id(int year, int month, int mday)
{
    return (int32_t)year * 10000 + (int32_t)month * 100 + (int32_t)mday;
}

void ferraris_day_start(ferraris_day_t *d, int32_t fwd, int32_t bwd)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->base_fwd = fwd;
    d->base_bwd = bwd;
}

void ferraris_day_get(const ferraris_day_t *d, int32_t fwd, int32_t bwd,
                      int32_t *day_fwd, int32_t *day_bwd)
{
    int32_t f = d ? fwd - d->base_fwd : 0;
    int32_t b = d ? bwd - d->base_bwd : 0;
    if (day_fwd) *day_fwd = f > 0 ? f : 0;
    if (day_bwd) *day_bwd = b > 0 ? b : 0;
}

bool ferraris_day_update(ferraris_day_t *d, int32_t today, int32_t fwd, int32_t bwd)
{
    if (!d || today <= 0 || today == d->day_id) return false;
    if (d->day_id != 0) {
        // Tageswechsel (auch nach laengerer Pause oder verstellter Uhr):
        // bisherigen Tag abschliessen, neuer Tag beginnt beim aktuellen Stand.
        d->prev_id = d->day_id;
        ferraris_day_get(d, fwd, bwd, &d->prev_fwd, &d->prev_bwd);
        d->base_fwd = fwd;
        d->base_bwd = bwd;
    }
    // day_id == 0: bisher ohne Uhrzeit gezaehlt -> gehoert zum heutigen Tag.
    d->day_id = today;
    return true;
}

void ferraris_day_shift(ferraris_day_t *d, int32_t delta_fwd, int32_t delta_bwd)
{
    if (!d) return;
    d->base_fwd += delta_fwd;
    d->base_bwd += delta_bwd;
}
