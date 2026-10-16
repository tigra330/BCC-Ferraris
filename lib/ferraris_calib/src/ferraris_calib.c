/**
 * @file ferraris_calib.c
 * @brief Automatisches Einmessen (pure C), siehe ferraris_calib.h.
 */

#include "ferraris_calib.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ─── Aufzeichnung ────────────────────────────────────────────────────────────

bool ferraris_calib_rec_init(ferraris_calib_rec_t *r, uint16_t *buf, uint32_t cap_values,
                             int n, uint32_t bucket_sets)
{
    if (!r || !buf || n < 1 || n > FERRARIS_CALIB_MAX_SENSORS || bucket_sets < 1) return false;
    memset(r, 0, sizeof(*r));
    r->buf         = buf;
    r->n           = (uint8_t)n;
    r->cap_rows    = (cap_values / (uint32_t)n) & ~1u;
    r->bucket_sets = bucket_sets;
    return r->cap_rows >= 4;
}

/** Puffer voll: je zwei Buckets zu einem (Maximum), Breite verdoppeln. */
static void compact(ferraris_calib_rec_t *r)
{
    const uint32_t half = r->cap_rows / 2;
    const int n = r->n;
    for (uint32_t k = 0; k < half; k++) {
        const uint16_t *a = r->buf + (2 * k) * n;
        const uint16_t *b = a + n;
        uint16_t *dst = r->buf + k * n;
        for (int i = 0; i < n; i++) dst[i] = a[i] > b[i] ? a[i] : b[i];
    }
    r->rows = half;
    r->bucket_sets *= 2;
}

void ferraris_calib_rec_feed(ferraris_calib_rec_t *r, const int *raw)
{
    if (!r || !r->buf || !raw) return;
    for (int i = 0; i < r->n; i++) {
        int v = raw[i];
        if (v < 0) v = 0;
        if (v > 4095) v = 4095;
        if (r->in_bucket == 0 || v > r->cur[i]) r->cur[i] = (uint16_t)v;
    }
    r->total_sets++;
    if (++r->in_bucket < r->bucket_sets) return;

    if (r->rows >= r->cap_rows) compact(r);
    memcpy(r->buf + r->rows * r->n, r->cur, r->n * sizeof(uint16_t));
    r->rows++;
    r->in_bucket = 0;
}

// ─── Auswertung ──────────────────────────────────────────────────────────────

#define HIST_SHIFT 3                      // 8er-Bins
#define HIST_BINS  (4096 >> HIST_SHIFT)

static uint16_t val(const uint16_t *buf, int n, uint32_t row, int i)
{
    return buf[row * (uint32_t)n + (uint32_t)i];
}

/** Wert, unter dem der Anteil p der Werte liegt (untere Bin-Kante). */
static int percentile(const uint32_t *h, uint32_t total, float p)
{
    if (!total) return 0;
    uint32_t want = (uint32_t)(p * (float)(total - 1)), acc = 0;
    for (int b = 0; b < HIST_BINS; b++) {
        acc += h[b];
        if (acc > want) return b << HIST_SHIFT;
    }
    return 4095;
}

typedef struct {
    int  high, low;
    bool on;
} schmitt_t;

static bool schmitt_step(schmitt_t *s, int v)
{
    bool prev = s->on;
    if (!s->on && v >= s->high) s->on = true;
    else if (s->on && v <= s->low) s->on = false;
    return s->on != prev;
}

static void analyze_sensor(const uint16_t *buf, uint32_t rows, int n, int i,
                           ferraris_calib_sensor_t *o)
{
    static uint32_t h[HIST_BINS];   // nur ein Aufrufer (loop-Task bzw. Test)
    memset(o, 0, sizeof(*o));

    memset(h, 0, sizeof(h));
    for (uint32_t r = 0; r < rows; r++) h[val(buf, n, r, i) >> HIST_SHIFT]++;
    o->base = percentile(h, rows, 0.5f);

    // Spitze, die mindestens 2 Buckets haelt: ein einzelner Ausreisser zaehlt nicht.
    int peak = rows ? val(buf, n, 0, i) : 0;
    if (rows >= 2) {
        peak = 0;
        for (uint32_t r = 0; r + 1 < rows; r++) {
            int a = val(buf, n, r, i), b = val(buf, n, r + 1, i);
            int m = a < b ? a : b;
            if (m > peak) peak = m;
        }
    }
    o->peak = peak;

    // Ruhe = alles unter der Mitte; Oberkante = 99%-Wert (inkl. langsamer Schwankung)
    const int mid = (o->base + peak) / 2;
    memset(h, 0, sizeof(h));
    uint32_t cnt = 0;
    int rest_max = 0;
    for (uint32_t r = 0; r < rows; r++) {
        int v = val(buf, n, r, i);
        if (v >= mid) continue;
        h[v >> HIST_SHIFT]++;
        cnt++;
        if (v > rest_max) rest_max = v;
    }
    int rest_hi = cnt ? percentile(h, cnt, 0.99f) + (1 << HIST_SHIFT) - 1 : o->base;
    if (cnt && rest_hi > rest_max) rest_hi = rest_max;
    o->rest_hi = rest_hi;

    o->gap    = peak - rest_hi;
    o->usable = o->gap >= FERRARIS_CALIB_MIN_GAP;
    o->low    = rest_hi + o->gap * 3 / 10;
    o->high   = rest_hi + o->gap * 6 / 10;
    if (o->high <= o->low) o->high = o->low + 1;

    if (o->usable) {
        schmitt_t s = { o->high, o->low, false };
        s.on = rows && val(buf, n, 0, i) >= o->high;
        for (uint32_t r = 1; r < rows; r++) {
            if (schmitt_step(&s, val(buf, n, r, i)) && s.on) o->passes++;
        }
    }
}

typedef struct {
    int      fwd, bwd, invalid;
    uint32_t min_spacing;     // kleinster Abstand zweier Schritte in Buckets
} pair_sim_t;

/** Gray-Code wie quadrature_decoder: S0=(0,0) S1=(1,0) S2=(1,1) S3=(0,1). */
static int gray(bool a, bool b)
{
    return a ? (b ? 2 : 1) : (b ? 3 : 0);
}

static void simulate_pair(const uint16_t *buf, uint32_t rows, int n, int ia, int ib,
                          const ferraris_calib_sensor_t *sa, const ferraris_calib_sensor_t *sb,
                          pair_sim_t *o)
{
    memset(o, 0, sizeof(*o));
    o->min_spacing = UINT32_MAX;
    if (rows < 2) return;
    schmitt_t a = { sa->high, sa->low, val(buf, n, 0, ia) >= sa->high };
    schmitt_t b = { sb->high, sb->low, val(buf, n, 0, ib) >= sb->high };
    int state = gray(a.on, b.on);
    int64_t last = -1;
    for (uint32_t r = 1; r < rows; r++) {
        bool ca = schmitt_step(&a, val(buf, n, r, ia));
        bool cb = schmitt_step(&b, val(buf, n, r, ib));
        if (!ca && !cb) continue;
        int next = gray(a.on, b.on);
        switch ((next - state) & 3) {
            case 1: o->fwd++; break;
            case 3: o->bwd++; break;
            default: o->invalid++; break;   // beide im selben Bucket: Reihenfolge unbekannt
        }
        if (last >= 0 && (uint32_t)(r - last) < o->min_spacing) o->min_spacing = (uint32_t)(r - last);
        last = r;
        state = next;
    }
}

/** Mittlere Zeit zwischen steigenden Flanken eines Sensors (= eine Umdrehung). */
static float mean_period_rows(const uint16_t *buf, uint32_t rows, int n, int i,
                              const ferraris_calib_sensor_t *s)
{
    schmitt_t sc = { s->high, s->low, rows && val(buf, n, 0, i) >= s->high };
    int64_t first = -1, last = -1;
    int edges = 0;
    for (uint32_t r = 1; r < rows; r++) {
        if (schmitt_step(&sc, val(buf, n, r, i)) && sc.on) {
            if (first < 0) first = r;
            last = r;
            edges++;
        }
    }
    return edges >= 2 ? (float)(last - first) / (float)(edges - 1) : 0.0f;
}

void ferraris_calib_analyze(const uint16_t *buf, uint32_t rows, int n, float bucket_ms,
                            bool reverse, ferraris_calib_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->n = n;
    out->rows = rows;
    out->bucket_ms = bucket_ms;
    if (!buf || n < 1 || n > FERRARIS_CALIB_MAX_SENSORS || rows < 8) {
        out->message = "Zu wenig aufgezeichnet";
        return;
    }

    int usable = 0;
    for (int i = 0; i < n; i++) {
        analyze_sensor(buf, rows, n, i, &out->s[i]);
        if (out->s[i].usable) usable++;
    }
    if (usable == 0) {
        out->message = "Kein Sensor zeigt einen deutlichen Markenausschlag. Dreht sich die Scheibe? "
                       "Laenger einmessen oder Sensoren ausrichten.";
        return;
    }
    if (usable < 2) {
        out->message = "Nur ein Sensor zeigt einen deutlichen Ausschlag. Fuer die Drehrichtung "
                       "werden zwei gebraucht.";
        return;
    }

    long best_score = -1;
    int best_a = -1, best_b = -1, best_steps = 0, max_steps = 0, max_invalid = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (!out->s[i].usable || !out->s[j].usable) continue;
            pair_sim_t p;
            simulate_pair(buf, rows, n, i, j, &out->s[i], &out->s[j], &p);
            out->pairs_checked++;
            int steps = p.fwd > p.bwd ? p.fwd : p.bwd;
            if (steps > max_steps) max_steps = steps;
            if (p.invalid > max_invalid) max_invalid = p.invalid;
            if (p.invalid || (p.fwd && p.bwd) || steps < 4) continue;
            out->pairs_valid++;

            int g = out->s[i].gap < out->s[j].gap ? out->s[i].gap : out->s[j].gap;
            uint32_t sp = p.min_spacing > 99 ? 99 : p.min_spacing;
            long score = (long)g * 100 + (long)sp;
            if (score <= best_score) continue;
            best_score = score;
            best_steps = steps;
            // Orientierung: Drehung waehrend der Aufzeichnung = Bezug (vorwaerts), ausser reverse.
            bool i_first = (p.fwd > 0) != reverse;
            best_a = i_first ? i : j;
            best_b = i_first ? j : i;
        }
    }

    if (best_a < 0) {
        if (max_steps < 4) {
            out->message = "Zu wenige Umdrehungen gesehen. Laenger einmessen (mindestens 2 Umdrehungen).";
        } else if (max_invalid) {
            out->message = "Die Marke ist kuerzer als die Aufloesung der Aufzeichnung. Kuerzer oder "
                           "bei geringerer Last einmessen.";
        } else {
            out->message = "Kein Sensorpaar mit sauberer Reihenfolge. Benachbarte Sensoren muessen sich "
                           "ueberlappen (Marke kurz unter beiden).";
        }
        return;
    }

    out->ok          = true;
    out->count_a     = best_a + 1;
    out->count_b     = best_b + 1;
    out->steps       = best_steps;
    out->revolutions = (float)best_steps / 4.0f;
    out->period_s    = mean_period_rows(buf, rows, n, best_a, &out->s[best_a]) * bucket_ms / 1000.0f;
    out->message     = best_steps >= 8 ? "Zaehlpaar und Schwellen gefunden."
                                       : "Nur eine Umdrehung gesehen - Ergebnis unsicher, besser laenger einmessen.";
}

// ─── JSON ────────────────────────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t len;
    int    pos;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (o->pos < 0) return;
    size_t room = (size_t)o->pos < o->len ? o->len - (size_t)o->pos : 0;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(room ? o->buf + o->pos : NULL, room, fmt, ap);
    va_end(ap);
    o->pos = k < 0 ? -1 : o->pos + k;
}

int ferraris_calib_result_to_json(const ferraris_calib_result_t *r, char *buf, size_t len)
{
    if (!r || !buf || !len) return -1;
    out_t o = { buf, len, 0 };
    // Meldungen sind Firmware-Konstanten ohne Anfuehrungszeichen/Steuerzeichen.
    put(&o, "{\"ok\":%s,\"message\":\"%s\",\"rows\":%lu,\"bucket_ms\":%.1f,\"count_a\":%d,"
            "\"count_b\":%d,\"steps\":%d,\"revolutions\":%.2f,\"period_s\":%.1f,"
            "\"pairs_checked\":%d,\"pairs_valid\":%d,\"sensors\":[",
        r->ok ? "true" : "false", r->message ? r->message : "", (unsigned long)r->rows,
        (double)r->bucket_ms, r->count_a, r->count_b, r->steps, (double)r->revolutions,
        (double)r->period_s, r->pairs_checked, r->pairs_valid);
    for (int i = 0; i < r->n && i < FERRARIS_CALIB_MAX_SENSORS; i++) {
        const ferraris_calib_sensor_t *s = &r->s[i];
        put(&o, "%s{\"usable\":%s,\"base\":%d,\"rest_hi\":%d,\"peak\":%d,\"gap\":%d,\"high\":%d,"
                "\"low\":%d,\"passes\":%d}",
            i ? "," : "", s->usable ? "true" : "false", s->base, s->rest_hi, s->peak, s->gap,
            s->high, s->low, s->passes);
    }
    put(&o, "]}");
    return o.pos;
}
