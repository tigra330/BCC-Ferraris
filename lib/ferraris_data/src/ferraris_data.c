/**
 * @file ferraris_data.c
 * @brief Gemeinsamer Zaehlerzustand (pure C).
 */

#include "ferraris_data.h"
#include "ferraris_power.h"
#include <stdlib.h>

struct ferraris_data {
    volatile int32_t  quad_forward;   // Single-Writer, 32-bit atomar
    volatile int32_t  quad_backward;
    volatile int32_t  base_forward;    // Writer: loop()-Task (set_quad), Wert = quad + base
    volatile int32_t  base_backward;
    ferraris_power_t  power;
    volatile int32_t  revolutions_per_kwh;   // Writer: loop()-Task (/config)
};

ferraris_data_t *ferraris_data_create(int32_t revolutions_per_kwh)
{
    if (revolutions_per_kwh <= 0) return NULL;
    ferraris_data_t *data = calloc(1, sizeof(*data));
    if (!data) return NULL;
    data->revolutions_per_kwh = revolutions_per_kwh;
    ferraris_power_init(&data->power);
    return data;
}

void ferraris_data_destroy(ferraris_data_t *data)
{
    free(data);
}

void ferraris_data_add_quad(ferraris_data_t *data, int8_t direction, uint32_t now_ms)
{
    if (!data || direction == 0) return;
    if (direction > 0) {
        data->quad_forward++;
    } else {
        data->quad_backward++;
    }
    ferraris_power_step(&data->power, direction, now_ms);
}

void ferraris_data_get_quad(const ferraris_data_t *data, int32_t *forward, int32_t *backward)
{
    if (forward)  *forward  = data ? data->quad_forward  + data->base_forward  : 0;
    if (backward) *backward = data ? data->quad_backward + data->base_backward : 0;
}

void ferraris_data_set_quad(ferraris_data_t *data, int32_t forward, int32_t backward)
{
    if (!data) return;
    data->base_forward  = forward  - data->quad_forward;
    data->base_backward = backward - data->quad_backward;
}

static float steps_to_kwh(const ferraris_data_t *data, int32_t steps)
{
    return (float)steps / (4.0f * (float)data->revolutions_per_kwh);
}

float ferraris_data_get_energy_kwh(const ferraris_data_t *data)
{
    if (!data) return 0.0f;
    int32_t fwd, bwd;
    ferraris_data_get_quad(data, &fwd, &bwd);
    return steps_to_kwh(data, fwd - bwd);
}

float ferraris_data_get_import_kwh(const ferraris_data_t *data)
{
    if (!data) return 0.0f;
    int32_t fwd;
    ferraris_data_get_quad(data, &fwd, NULL);
    return steps_to_kwh(data, fwd);
}

float ferraris_data_get_export_kwh(const ferraris_data_t *data)
{
    if (!data) return 0.0f;
    int32_t bwd;
    ferraris_data_get_quad(data, NULL, &bwd);
    return steps_to_kwh(data, bwd);
}

float ferraris_data_get_power_watts(const ferraris_data_t *data, uint32_t now_ms)
{
    if (!data) return 0.0f;
    return ferraris_power_watts(&data->power, data->revolutions_per_kwh, now_ms,
                                FERRARIS_DATA_POWER_TIMEOUT_MS);
}

int32_t ferraris_data_get_revs_per_kwh(const ferraris_data_t *data)
{
    return data ? data->revolutions_per_kwh : 0;
}

bool ferraris_data_set_revs_per_kwh(ferraris_data_t *data, int32_t revolutions_per_kwh)
{
    if (!data || revolutions_per_kwh <= 0) return false;
    data->revolutions_per_kwh = revolutions_per_kwh;
    return true;
}
