/**
 * @file ferraris_logger_logic.c
 * @brief Pure-C Per-Channel-Tracker: EMA + asymmetrische Huelle + Mark-Passagen.
 *
 * Siehe ferraris_logger_logic.h fuer die Konstanten und die Polaritaets-Regel.
 * Diese Datei hat KEINE ESP-IDF-Abhaengigkeit (nur stdint/stdbool) und wird
 * identisch auf PC (gcc fuer Sim + Tests) und ESP32 (idf) kompiliert — das ist
 * der Format-Vertrag, der den CSV-Round-Trip wohldefiniert macht.
 */

#include "ferraris_logger_logic.h"

// ─── Lifecycle ──────────────────────────────────────────────────────────────

void logger_ch_init(logger_ch_t *c, int seed_raw)
{
    if (!c) return;
    c->last_raw   = seed_raw;
    c->ema        = (float)seed_raw;
    c->run_min    = (float)seed_raw;
    c->run_max    = (float)seed_raw;
    c->mark_count = 0;
    c->prev_mark  = false;

    // MIN_RANGE erzwingen: aus seed±CALIB_MIN_RANGE/2 auseinanderdruecken,
    // damit die Huelle ab Start eine gueltige Spanne hat (wie RML nach Boot).
    float half = (float)LOGGER_CALIB_MIN_RANGE * 0.5f;
    c->run_min -= half;
    c->run_max += half;
}

// ─── Processing ─────────────────────────────────────────────────────────────

void logger_ch_feed(logger_ch_t *c, int raw, bool mark_state)
{
    if (!c) return;

    c->last_raw = raw;

    // EMA (Tiefpass, RML FILTER_ALPHA=0.12).
    c->ema += LOGGER_FILTER_ALPHA * ((float)raw - c->ema);

    // Asymmetrische Huelle (RML runningMin/runningMax):
    //   zum Extrem hin schnell ziehen (TOWARD), in Gegenrichtung langsam
    //   vergessen (DRIFT).  So folgt die Huelle Drift/Staub/LED-Alterung,
    //   ohne von kurzem Rauschen weggezogen zu werden.
    if ((float)raw < c->run_min) {
        c->run_min += LOGGER_CALIB_ALPHA_TOWARD * ((float)raw - c->run_min);
    } else {
        c->run_min += LOGGER_CALIB_ALPHA_DRIFT * ((float)raw - c->run_min);
    }
    if ((float)raw > c->run_max) {
        c->run_max += LOGGER_CALIB_ALPHA_TOWARD * ((float)raw - c->run_max);
    } else {
        c->run_max += LOGGER_CALIB_ALPHA_DRIFT * ((float)raw - c->run_max);
    }

    // MIN_RANGE halten: Huelle darf nie unter CALIB_MIN_RANGE schrumpfen
    // (verhindert Schwellen-Kollaps bei ruhigem Signal).
    float range = c->run_max - c->run_min;
    if (range < (float)LOGGER_CALIB_MIN_RANGE) {
        float mid    = 0.5f * (c->run_min + c->run_max);
        float half   = (float)LOGGER_CALIB_MIN_RANGE * 0.5f;
        c->run_min = mid - half;
        c->run_max = mid + half;
    }

    // Mark-Passagen: steigende Flanke des Production-State (0->1 = Marke
    // betreten).  la_count = Passagen, semantisch wie RML-Peakcount.
    if (!c->prev_mark && mark_state) {
        c->mark_count++;
    }
    c->prev_mark = mark_state;
}

// ─── Quadratur + Leistung (fuer 18. Spalte W) ────────────────────────────────

void logger_quad_init(logger_quad_t *q)
{
    if (!q) return;
    q->state        = -1;
    q->net_steps    = 0;
    q->initialized = false;
}

/**
 * @brief Gray-Code-Zustand aus dem Mark-PAAR (a,b): S0=00 S1=10 S2=11 S3=01.
 *
 * Indizierung: idx = a ? (b ? 2 : 1) : (b ? 3 : 0).  Das ist die Gray-Code-
 * Reihenfolge (pro Schritt aendert sich genau ein Bit), nicht die rohe
 * Binaer-Indizes (a<<1|b) — mit der wuerde Vorwaerts/Rueckwaerts vertauscht.
 */
static inline int quad_index(bool a, bool b)
{
    return a ? (b ? 2 : 1) : (b ? 3 : 0);
}

void logger_quad_feed(logger_quad_t *q, bool a, bool b)
{
    if (!q) return;

    int cur = quad_index(a, b);

    if (!q->initialized) {
        q->state        = (int8_t)cur;
        q->initialized  = true;
        return;
    }
    if (cur == q->state) {
        return;   /* kein Uebergang */
    }

    /* delta = (cur - prev) mod 4, positiv in [0..3]:
     *   1 -> Vorwaerts (+1),  3 -> Rueckwaerts (-1, denn 3 == -1 mod 4),
     *   2 -> ungueltig (S0<->S2 / S1<->S3), ignorieren. */
    int delta = (cur - (int)q->state + 4) & 3;
    if (delta == 1) {
        q->net_steps++;
    } else if (delta == 3) {
        q->net_steps--;
    }
    /* delta == 2: ungueltig, net_steps unveraendert */

    q->state = (int8_t)cur;
}

int logger_watts(int32_t net_steps, int k_rev_per_kwh, uint32_t elapsed_ms)
{
    /* Guards: keine Zeit, keine Konstante, keine Umdrehung -> 0 (zu wenig Daten).
     * Vermeidet zusaetzlich Division durch Null. */
    if (k_rev_per_kwh <= 0 || elapsed_ms == 0 || net_steps == 0) {
        return 0;
    }

    /* W = (net_steps/4) / k * (3600 / (elapsed_ms/1000)) * 1000
     *   = net_steps * 900000000 / (k * elapsed_ms)
     * Double-Zwischenergebnis verhindert int32-Ueberlauf (9e8 * net_steps).
     * (int)-Cast schneidet gegen Null ab (wie RML-Integer-Spalten). */
    double w = (double)net_steps * 900000000.0
             / ((double)k_rev_per_kwh * (double)elapsed_ms);
    return (int)w;
}