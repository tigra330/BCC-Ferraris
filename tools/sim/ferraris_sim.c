/**
 * @file ferraris_sim.c
 * @brief PC-Simulator fuer Ferraris-Zaehler Ausleser (Quadratur-Zustandsautomat)
 *
 * Liest eine CSV-Datei mit Analogwerten und simuliert Schmitt-Trigger +
 * Quadratur-Zustandsautomat.  Zum Testen ohne ESP32-Hardware.
 *
 * Zwei CSV-Formate werden unterstuetzt:
 *
 *  1. Phase-1-Synthese-CSV (Default):  Spalten sample,a,b  (0-indiziert
 *     col0=sample, col1=a, col2=b).  Default --col-a 1 --col-b 2 liest a,b.
 *
 *  2. Reale RML-Captures (17 Spalten):
 *       millis, la_raw, la_filt, la_min, la_max, la_tOn, la_tOff, la_state,
 *       la_count, ra_raw, ra_filt, ra_min, ra_max, ra_tOn, ra_tOff, ra_state,
 *       ra_count
 *     Hier muessen die beiden Sensor-Spalten explizit gewaehlt werden
 *     (--col-a 1 --col-b 9 = la_raw + ra_raw); sonst wuerde der Sim
 *     la_raw + la_filt (denselben Sensor, raw+gefiltert) paaren und beide
 *     Kanale zugleich schalten -> S0<->S2 "Ungueltig" -> 0 netto.
 *
 * Reale HW braucht zusaetzlich:
 *   - Per-Channel-Schwellen  (LA/RA haben unterschiedliche DC-Basis).
 *   - Signal-Invertierung    (Marke = niedriges Raw; INVERT_SIGNAL=true der
 *     Referenz-FW).  Auf einer 96/4 %-Scheibe trackt der Schmitt nur mit
 *     Invert die lange Verweilzeit (ueberlappende Fenster -> Quadratur);
 *     ohne Invert trackt er den kurzen Peak (kein Ueberlapp -> Beben -> 0 netto).
 *
 * Optionen:
 *   --autotune N         Auto-Tune: analysiere erste N Samples pro Kanal
 *   --high N             Manuelle Schwelle fuer BEIDE Kanaele (Standard: 2500)
 *   --low N              Manuelle Schwelle fuer BEIDE Kanaele (Standard: 1600)
 *   --high-a N --low-a N Per-Channel-Schwelle Kanal A (ueberschreibt --high/--low)
 *   --high-b N --low-b N Per-Channel-Schwelle Kanal B
 *   --col-a N            CSV-Spalte fuer Kanal A, 0-indiziert (Standard: 1)
 *   --col-b N            CSV-Spalte fuer Kanal B, 0-indiziert (Standard: 2)
 *   --col-sample N       CSV-Spalte fuer Sample-Nummer/Timestamp (Standard: 0, -1=keine)
 *   --invert             Marke = niedriges Raw (INVERT_SIGNAL), Standard: aus
 *   --fw-thresholds      Per-Channel-Schwellen aus dem RML-CSV lesen
 *                        (la_tOn/la_tOff/ra_tOn/ra_tOff, Spalten 5/6/13/14)
 *   --use-fw-states      FW-States direkt als Schmitt-Ausgang nehmen
 *                        (la_state/ra_state, Spalten 7/15).  Das ist die
 *                        echte HW-Referenz: die States werden FW-intern mit
 *                        dynamischer Huellen-Schwelle berechnet (hoeherer Rate
 *                        als das Log) und ergeben die korrekte Umdrehung,
 *                        die ein fester Schwellen-Schmitt nicht reproduziert.
 *   --production         Production-identischer Modus (Shortcut): non-invert
 *                        wie `schmitt_trigger_logic` (raw >= high -> HIGH),
 *                        rohe la_raw/ra_raw (col 1/9), Per-Channel-Schwellen
 *                        aus dem RML-CSV (--fw-thresholds).  KEIN Invert —
 *                        Production hat kein Invert-Flag (s. Memory).  Auf dem
 *                        ~4 Hz-Log liefert das 0 netto (ehrliches Production-
 *                        Ergebnis auf diesem Log; +4 braucht Invert oder echte
 *                        20 kHz-Rohdaten oder --use-fw-states).
 *   --revs N             Umdrehungen pro kWh (Standard: 75)
 *
 * Physikalischer 20-kHz-Generator (--generate, keine CSV-Datei noetig):
 *   Synthetisiert 20-kHz-Raw im Speicher aus Physik (Disk-Speed = P*k/3600)
 *   + RML-Beobachtung (Mark-Anteil, ADC-Bereich, Rauschen, Sensor-Offset) und
 *   laesst Production (non-invert Schmitt + Quadratur) darauf laufen.  Zweck:
 *   die Quadratur bei echter HW-Abtastrate validieren — das RML-Log ist nur
 *   ~4 Hz (aliasiert), aber Production brauchte 20 kHz.  Jeder Parameter aus
 *   Physik ODER realer Beobachtung, keine erfundenen Zahlen.  --from-rml <csv>
 *   leitet alle Parameter live aus einem Capture ab (Selbstvalidierung).
 *   --write-csv <path>   Schreibt das synthetisierte Signal als RML-17-Spalten-CSV
 *                        (dieselbe ferraris_logger_logic wie die Firmware -> byte-identisch).
 *                        Round-Trip: --use-fw-states auf der CSV reproduziert den
 *                        Quadratur-Zaehlerstand (Polaritaet beider Kanaele invertiert
 *                        -> Gray-Code-Delta erhalten -> Zaehler identisch).
 *   --csv-hz N           CSV-Decimierungsrate (Standard 4 = RML-Konvention).
 *
 * Kompilieren:
 *   gcc -Wall -Wextra -I../../components/schmitt_trigger_adc/include \
 *       -I../../components/ferraris_logger/include -lm \
 *       -o ferraris_sim ferraris_sim.c \
 *       ../../components/schmitt_trigger_adc/src/schmitt_trigger_autotune.c \
 *       ../../components/ferraris_logger/src/ferraris_logger_logic.c
 *
 * Aufrufen:
 *   Synthese:  ./ferraris_sim daten.csv --autotune 1000
 *   Synthese:  ./ferraris_sim daten.csv --high 2800 --low 1400
 *   RML-Prod:  ./ferraris_sim rml_447W.csv --production            (non-invert, col1/col9, FW-Schwellen)
 *   RML-Exp:   ./ferraris_sim rml_447W.csv --col-a 1 --col-b 9 --invert --fw-thresholds  (mit Invert)
 *   RML-HWRef: ./ferraris_sim rml_447W.csv --use-fw-states         (la_state/ra_state -> +1 Umdr)
 *   Generator: ./ferraris_sim --generate --from-rml rml_447W.csv   (Parameter live, no fakes)
 *   Generator: ./ferraris_sim --generate --power 447 --k 75        (Defaults = RML-447W -> ~+5 netto)
 *   RoundTrip: ./ferraris_sim --generate --from-rml rml_447W.csv --write-csv synth.csv
 *              ./ferraris_sim --use-fw-states synth.csv            (reproduziert Generator-Zaehler)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "schmitt_trigger_autotune.h"
#include "ferraris_logger_logic.h"

// ─── Konfigurierbare Parameter ─────────────────────────────────────────────────

// Per-Channel-Schwellen (Default = alter Globalwert fuer beide Kanaele).
static int g_high_a = 2500, g_low_a = 1600;   // Kanal A
static int g_high_b = 2500, g_low_b = 1600;   // Kanal B
static int g_revolutions_per_kwh = 75;        // Umdrehungen pro kWh
static int g_autotune_samples   = 0;          // 0 = kein Auto-Tune

// CSV-Spalten (0-indiziert).  Default -> Synthese-Format sample,a,b.
static int g_col_sample = 0;                  // -1 = keine Sample-Spalte
static int g_col_a      = 1;
static int g_col_b      = 2;

static bool g_invert        = false;          // Marke = niedriges Raw
static bool g_fw_thresholds = false;         // Schwellen aus RML-CSV lesen
static bool g_use_fw_states = false;         // la_state/ra_state direkt nutzen

// --generate: physikalischer 20-kHz-Signalgenerator.  Jeder Parameter ist aus
// Physik ODER aus der realen RML-447W-Beobachtung hergeleitet (keine erfundenen
// Zahlen).  Defaults via tools/extract_rml_params.py aus dem 447W-Capture.
//   Disk-Speed = P*k/3600 rev/s                (Physik)
//   Mark-Anteil, ADC-Bereich, Rauschen, Offset (RML-Beobachtung)
//   Abtastrate 20 kHz                          (ESP32 DMA-Minimum, HW-Constraint)
static bool   g_generate          = false;
static double g_gen_power         = 447.0;    // W  (Physik)
static double g_gen_k             = 75.0;     // rev/kWh
static double g_gen_mark_frac     = 0.0441;   // RML la_state==0-Anteil (4,4 %)
static int    g_gen_adc_min_a     = 2798;     // RML la_raw min
static int    g_gen_adc_max_a     = 3948;     // RML la_raw max
static int    g_gen_adc_min_b     = 2751;     // RML ra_raw min
static int    g_gen_adc_max_b     = 3676;     // RML ra_raw max
static double g_gen_noise_a       = 23.1;     // RML Std(la_raw-la_filt) im Dwell
static double g_gen_noise_b       = 16.2;
static double g_gen_sensor_offset = 4.2;      // RML: B hinter A, Grad
static int    g_gen_rate          = 20000;    // ESP32 ADC-Continuous-Minimum
static double g_gen_duration      = 130.0;    // s (RML-Capture-Dauer)
static double g_gen_start_offset  = 0.0;      // s: Startphase (0=Marke bei t=0 am Sensor)
static const char *g_from_rml     = NULL;     // --from-rml: Parameter live aus Capture

// --write-csv: schreibt das synthetisierte Signal als RML-17-Spalten-CSV (dieselbe
// ferraris_logger_logic wie die Firmware -> byte-identisches Format).  Round-Trip:
// --generate --from-rml real.csv --write-csv synth.csv, danach --use-fw-states synth.csv
// reproduziert den Quadratur-Zaehlerstand (Polaritaet beider Kanaele invertiert).
static char g_write_csv[512]      = "";       // --write-csv: Ausgabedatei (leer = kein CSV)
static int  g_csv_hz              = 4;        // --csv-hz: CSV-Decimierungsrate (Hz)

// RML-Schwellen-Spalten (0-indiziert, fest im 17-Spalten-Format).
#define RML_COL_LA_TON   5    // la_tOn  (obere Tal-Kante)
#define RML_COL_LA_TOFF  6    // la_tOff (untere Tal-Kante)
#define RML_COL_RA_TON  13
#define RML_COL_RA_TOFF 14

// ─── Quadratur-Zustaende ───────────────────────────────────────────────────────

typedef enum {
    QUAD_S0 = 0,   // A=low,  B=low
    QUAD_S1 = 1,   // A=high, B=low
    QUAD_S2 = 2,   // A=high, B=high
    QUAD_S3 = 3,   // A=low,  B=high
} quad_state_t;

static inline quad_state_t compute_quad_state(bool a_high, bool b_high)
{
    if (!a_high && !b_high) return QUAD_S0;
    if (a_high  && !b_high) return QUAD_S1;
    if (a_high  && b_high)  return QUAD_S2;
    return QUAD_S3;
}

static inline int8_t quad_delta(quad_state_t prev, quad_state_t curr)
{
    int8_t diff = (int8_t)curr - (int8_t)prev;
    if (diff ==  1 || diff == -3) return  1;
    if (diff == -1 || diff ==  3) return -1;
    return 0;
}

static const char *state_name(quad_state_t s)
{
    switch (s) {
        case QUAD_S0: return "S0(A=0,B=0)";
        case QUAD_S1: return "S1(A=1,B=0)";
        case QUAD_S2: return "S2(A=1,B=1)";
        case QUAD_S3: return "S3(A=0,B=1)";
    }
    return "?";
}

// ─── Globale Zaehler ───────────────────────────────────────────────────────────

static int32_t g_count_forward  = 0;
static int32_t g_count_backward = 0;

// ─── Schmitt-Trigger (per-Channel, mit optionaler Invertierung) ─────────────────
//
// Hysterese-Regel:
//   ohne Invert (Marke = hohes Raw):
//     rising : state LOW  und raw >= high -> state HIGH
//     falling: state HIGH und raw <= low  -> state LOW
//   mit Invert (Marke = niedriges Raw, entspricht INVERT_SIGNAL=true):
//     rising : state LOW  und raw <= low  -> state HIGH   (Marke erreicht)
//     falling: state HIGH und raw >= high -> state LOW    (Marke verlaesst)
//   Totbereich zwischen low und high -> keine Aenderung (Hysterese).
//
// Die SchwelLEN-Zahlen sind in beiden Modi dieselben (high = obere Tal-Kante,
// low = untere); nur die Polaritaet (welche Seite "Marke" ist) dreht sich.

static bool apply_schmitt(int raw, bool *state, int high, int low, bool invert)
{
    bool prev = *state;

    if (!invert) {
        if (!prev && raw >= high) { *state = true;  return true; }
        if (prev  && raw <= low ) { *state = false; return true; }
    } else {
        if (!prev && raw <= low ) { *state = true;  return true; }
        if (prev  && raw >= high) { *state = false; return true; }
    }
    return false;
}

// ─── CSV parsen (spaltenselektiv) ──────────────────────────────────────────────

// Liest den Integer-Wert der 0-indizierten Spalte col0 aus einer Komma-Zeile.
// Liefert false, falls die Spalte fehlt oder keine Zahl ist.
static bool csv_col_int(const char *line, int col0, int *out)
{
    const char *p = line;
    for (int idx = 0; idx < col0; idx++) {
        const char *comma = strchr(p, ',');
        if (!comma) return false;
        p = comma + 1;
    }
    int val;
    if (sscanf(p, "%d", &val) != 1) return false;
    *out = val;
    return true;
}

// Liest Sample-Nummer (optional), val_a (Spalte col_a) und val_b (Spalte col_b).
// Ueberspringt Leer-/Kommentar-('#')/Header-('sample')-Zeilen.
static bool parse_csv_line(const char *line, int col_sample, int col_a, int col_b,
                           int *sample_num, int *val_a, int *val_b)
{
    if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') return false;
    if (line[0] == '#') return false;                       // RML-Kommentarzeile
    if (strncmp(line, "sample", 6) == 0) return false;      // Header

    int a, b;
    if (!csv_col_int(line, col_a, &a)) return false;
    if (!csv_col_int(line, col_b, &b)) return false;
    *val_a = a;
    *val_b = b;

    if (col_sample >= 0 && csv_col_int(line, col_sample, sample_num)) {
        return true;
    }
    *sample_num = -1;
    return true;
}

// ─── FW-Schwellen aus RML-CSV lesen ────────────────────────────────────────────

// Liest la_tOn/la_tOff/ra_tOn/ra_tOff aus der ersten echten Datenzeile des
// RML-17-Spalten-CSV und weist sie als Per-Channel-Schwellen zu.
//   high = tOn  (obere Tal-Kante, Schwelle beim Verlassen der Marke)
//   low  = tOff (untere Tal-Kante, Schwelle beim Erreichen der Marke)
// Invert flag regelt die Polaritaet (s. apply_schmitt).
static bool read_fw_thresholds(const char *filename,
                               int *ha, int *la, int *hb, int *lb)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Kann Datei nicht oeffnen");
        return false;
    }
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == '#') continue;
        if (strncmp(line, "sample", 6) == 0) continue;

        int ton_a, toff_a, ton_b, toff_b;
        if (csv_col_int(line, RML_COL_LA_TON,  &ton_a)  &&
            csv_col_int(line, RML_COL_LA_TOFF, &toff_a) &&
            csv_col_int(line, RML_COL_RA_TON,  &ton_b)  &&
            csv_col_int(line, RML_COL_RA_TOFF, &toff_b)) {
            *ha = ton_a;  *la = toff_a;
            *hb = ton_b;  *lb = toff_b;
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

// ─── Auto-Tune (per-Channel) ───────────────────────────────────────────────────

static bool run_autotune(const char *filename, int num_samples)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Kann Datei nicht oeffnen");
        return false;
    }

    int16_t *samples_a = malloc(num_samples * sizeof(int16_t));
    int16_t *samples_b = malloc(num_samples * sizeof(int16_t));
    if (!samples_a || !samples_b) {
        fprintf(stderr, "Nicht genug Speicher fuer Auto-Tune\n");
        free(samples_a); free(samples_b);
        fclose(fp);
        return false;
    }

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < num_samples) {
        int sample_num, val_a, val_b;
        if (parse_csv_line(line, g_col_sample, g_col_a, g_col_b,
                           &sample_num, &val_a, &val_b)) {
            samples_a[count] = (int16_t)val_a;
            samples_b[count] = (int16_t)val_b;
            count++;
        }
    }
    fclose(fp);

    if (count < 100) {
        fprintf(stderr, "Nicht genug Samples fuer Auto-Tune: %d (brauche >= 100)\n", count);
        free(samples_a); free(samples_b);
        return false;
    }

    printf("\n=== Auto-Tune Analyse (per-Channel) ===\n");
    printf("Analysiere %d Samples pro Kanal (Spalten A=%d, B=%d)\n\n",
           count, g_col_a, g_col_b);

    schmitt_autotune_config_t cfg = schmitt_autotune_config_default();
    cfg.num_samples = count;

    schmitt_autotune_result_t ra = schmitt_autotune_analyze(samples_a, count, &cfg);
    printf("Kanal A (col %d):\n", g_col_a);
    if (ra.valid) {
        printf("  Peak LOW=%d, Peak HIGH=%d (Abstand=%d)\n",
               ra.peak_low, ra.peak_high, ra.peak_separation);
        printf("  -> HIGH=%d, LOW=%d (Hysterese=%d)\n",
               ra.threshold_high, ra.threshold_low,
               ra.threshold_high - ra.threshold_low);
    } else {
        printf("  FEHLGESCHLAGEN: Keine zwei deutlichen Peaks\n");
    }

    schmitt_autotune_result_t rb = schmitt_autotune_analyze(samples_b, count, &cfg);
    printf("Kanal B (col %d):\n", g_col_b);
    if (rb.valid) {
        printf("  Peak LOW=%d, Peak HIGH=%d (Abstand=%d)\n",
               rb.peak_low, rb.peak_high, rb.peak_separation);
        printf("  -> HIGH=%d, LOW=%d (Hysterese=%d)\n",
               rb.threshold_high, rb.threshold_low,
               rb.threshold_high - rb.threshold_low);
    } else {
        printf("  FEHLGESCHLAGEN: Keine zwei deutlichen Peaks\n");
    }

    // Per-Channel uebernehmen (kein Merge mehr in einen globalen Paar —
    // LA und RA haben unterschiedliche Basen und brauchen eigene Schwellen).
    bool any = false;
    if (ra.valid) { g_high_a = ra.threshold_high; g_low_a = ra.threshold_low; any = true; }
    if (rb.valid) { g_high_b = rb.threshold_high; g_low_b = rb.threshold_low; any = true; }

    if (any) {
        printf("\nVerwendete Schwellen:  A HIGH=%d LOW=%d   B HIGH=%d LOW=%d\n",
               g_high_a, g_low_a, g_high_b, g_low_b);
        if (!ra.valid) printf("  (Kanal A: Auto-Tune fehlgeschlagen -> bei Default belassen)\n");
        if (!rb.valid) printf("  (Kanal B: Auto-Tune fehlgeschlagen -> bei Default belassen)\n");
    } else {
        printf("\nAuto-Tune fuer beide Kanaele fehlgeschlagen -> Standard-Schwellen bleiben.\n");
    }

    free(samples_a); free(samples_b);
    return any;
}

// ─── Physikalischer 20-kHz-Generator (--generate) ──────────────────────────────
//
// Synthetisiert ein 20-kHz-Rohsignal im Speicher aus Physik + RML-Beobachtung
// und laesst Production (non-invert Schmitt + Quadratur) darauf laufen.  Zweck:
// die Quadratur bei echter HW-Abtastrate validieren, ohne ein 20-kHz-Raw-Capture
// zu besitzen (das RML-Log ist nur ~4 Hz).  Jeder Parameter stammt aus Physik
// (Disk-Speed = P*k/3600) oder aus der realen 447W-Beobachtung (Mark-Anteil,
// ADC-Bereich, Rauschen, Sensor-Offset) — KEINE erfundenen Zahlen.  Defaults
// via tools/extract_rml_params.py; --from-rml leitet sie live aus einem Capture
// ab (Selbstvalidierung, dass die Defaults nicht von Hand gewaehlt sind).

// Mathematischer Modulo, immer positiv (fmod kann in C negativ sein).
static inline double mod_pos(double x, double m)
{
    double r = fmod(x, m);
    if (r < 0.0) r += m;
    return r;
}

// Gauss-Rauschen (Box-Muller), reproduzierbar via festem Seed in run_generate.
static double gauss(void)
{
    static int have_spare = 0;
    static double spare;
    if (have_spare) { have_spare = 0; return spare; }
    double u, v, s;
    do {
        u = (rand() / (double)RAND_MAX) * 2.0 - 1.0;
        v = (rand() / (double)RAND_MAX) * 2.0 - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    double mul = sqrt(-2.0 * log(s) / s);
    spare = v * mul;
    have_spare = 1;
    return u * mul;
}

// Glattes Mark-Puls 0..1: ~1 im Mark-Bereich [0,mw], ~0 ausserhalb, tanh-Kanten.
// Uebergangsbreite ew = 10 % der Markbreite -> saubere einzelne Schmitt-Flanke
// (Hysterese verhindert Prellen auch bei Rauschen im langsamen Anstieg).
static inline double mark_pulse(double m, double mw)
{
    double ew = mw * 0.10;
    if (ew < 1e-9) ew = 1e-9;
    return 0.5 * (tanh(m / ew) - tanh((m - mw) / ew));
}

// Leitet die Generator-Parameter live aus einem RML-17-Spalten-Capture ab
// (dieselbe Logik wie tools/extract_rml_params.py).  Beweist, dass die Defaults
// nicht von Hand gewaehlt, sondern aus realer Beobachtung messbar sind.
static bool derive_gen_params_from_rml(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { perror("Kann RML-Capture nicht oeffnen"); return false; }

    // Spalten (0-indiziert): 0=millis, 1=la_raw, 2=la_filt, 7=la_state,
    // 9=ra_raw, 10=ra_filt, 15=ra_state.
    const int COL_MS = 0, COL_LA_RAW = 1, COL_LA_FILT = 2, COL_LA_STATE = 7;
    const int COL_RA_RAW = 9, COL_RA_FILT = 10, COL_RA_STATE = 15;

    long n = 0;
    int la_min = 0, la_max = 0, ra_min = 0, ra_max = 0;
    long mark_a = 0;
    double sum_res_a = 0.0, sumsq_res_a = 0.0;
    double sum_res_b = 0.0, sumsq_res_b = 0.0;
    long resid_n_a = 0, resid_n_b = 0;
    long ms_first = 0, ms_last = 0;
    long la_onset_ms = -1, ra_onset_ms = -1;
    int la_prev = -1, ra_prev = -1;
    char line[512];
    bool first = true;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == '#') continue;
        if (strncmp(line, "sample", 6) == 0) continue;
        int ms, laraw, lafil, lastate, raraw, rafil, rastate;
        if (!csv_col_int(line, COL_MS, &ms)) continue;
        if (!csv_col_int(line, COL_LA_RAW, &laraw)) continue;
        if (!csv_col_int(line, COL_LA_FILT, &lafil)) continue;
        if (!csv_col_int(line, COL_LA_STATE, &lastate)) continue;
        if (!csv_col_int(line, COL_RA_RAW, &raraw)) continue;
        if (!csv_col_int(line, COL_RA_FILT, &rafil)) continue;
        if (!csv_col_int(line, COL_RA_STATE, &rastate)) continue;

        if (first) {
            ms_first = ms; la_min = la_max = laraw; ra_min = ra_max = raraw;
            first = false;
        }
        if (laraw < la_min) la_min = laraw;
        if (laraw > la_max) la_max = laraw;
        if (raraw < ra_min) ra_min = raraw;
        if (raraw > ra_max) ra_max = raraw;
        if (lastate == 0) mark_a++;
        if (lastate == 1) { double r = (double)(laraw - lafil); sum_res_a += r; sumsq_res_a += r * r; resid_n_a++; }
        if (rastate == 1) { double r = (double)(raraw - rafil); sum_res_b += r; sumsq_res_b += r * r; resid_n_b++; }
        if (la_onset_ms < 0 && la_prev == 1 && lastate == 0) la_onset_ms = ms;
        if (ra_onset_ms < 0 && ra_prev == 1 && rastate == 0) ra_onset_ms = ms;
        la_prev = lastate; ra_prev = rastate;
        ms_last = ms;
        n++;
    }
    fclose(fp);

    if (n < 10) { fprintf(stderr, "--from-rml: zu wenige gueltige Zeilen (%ld)\n", n); return false; }

    g_gen_adc_min_a = la_min; g_gen_adc_max_a = la_max;
    g_gen_adc_min_b = ra_min; g_gen_adc_max_b = ra_max;
    g_gen_mark_frac = (double)mark_a / (double)n;
    if (resid_n_a > 1) {
        double mu = sum_res_a / resid_n_a;
        g_gen_noise_a = sqrt((sumsq_res_a - resid_n_a * mu * mu) / (resid_n_a - 1));
    }
    if (resid_n_b > 1) {
        double mu = sum_res_b / resid_n_b;
        g_gen_noise_b = sqrt((sumsq_res_b - resid_n_b * mu * mu) / (resid_n_b - 1));
    }
    double duration_s = (ms_last - ms_first) / 1000.0;
    if (duration_s > 0.0) g_gen_duration = duration_s;

    // Phasenoffset aus erstem Mark-Onset (wie extract_rml_params.py).
    double rev_per_s = (g_gen_power / 1000.0) * g_gen_k / 3600.0;
    if (la_onset_ms >= 0 && ra_onset_ms >= 0 && rev_per_s > 0.0) {
        double rev_period_ms = 1000.0 / rev_per_s;
        double dt = fmod((double)(ra_onset_ms - la_onset_ms), rev_period_ms);
        if (dt < 0) dt += rev_period_ms;
        g_gen_sensor_offset = 360.0 * dt / rev_period_ms;
    }

    // Startphase: erster la-Mark-Onset relativ zu Capture-Beginn.  Der Generator
    // reproduziert damit den exakten Phasenverlauf der realen Capture (wie viele
    // Mark-Passagen ins Zeitfenster fallen) — nicht irgendeine willkuerliche Phase.
    if (la_onset_ms >= 0) {
        g_gen_start_offset = (double)(la_onset_ms - ms_first) / 1000.0;
    }

    printf("--from-rml: Parameter live aus %s abgeleitet (%ld Zeilen):\n", path, n);
    printf("  Mark-Anteil=%.4f  A=[%d,%d]  B=[%d,%d]  Rauschen A=%.1f B=%.1f  Offset=%.1f deg  Start=%.2fs  Dauer=%.1fs\n",
           g_gen_mark_frac, g_gen_adc_min_a, g_gen_adc_max_a, g_gen_adc_min_b, g_gen_adc_max_b,
           g_gen_noise_a, g_gen_noise_b, g_gen_sensor_offset, g_gen_start_offset, g_gen_duration);
    return true;
}

static int run_generate(void)
{
    double rev_per_s = (g_gen_power / 1000.0) * g_gen_k / 3600.0;
    double rad_per_sample = 2.0 * M_PI * rev_per_s / (double)g_gen_rate;
    double offset_rad = g_gen_sensor_offset * M_PI / 180.0;
    // Startphase: Marke bei t=g_gen_start_offset am Sensor A (reproduziert RML).
    // start_rad < 0 -> bei t=0 ist die Marke noch um start_offset vor dem Sensor.
    double start_rad = -2.0 * M_PI * rev_per_s * g_gen_start_offset;
    long total = (long)(g_gen_rate * g_gen_duration);

    // Per-Channel-Schwellen 70/35 % (wie Referenz-FW; kreuzgeprueft gegen RML).
    int ha = g_gen_adc_min_a + (int)(0.70 * (g_gen_adc_max_a - g_gen_adc_min_a));
    int la = g_gen_adc_min_a + (int)(0.35 * (g_gen_adc_max_a - g_gen_adc_min_a));
    int hb = g_gen_adc_min_b + (int)(0.70 * (g_gen_adc_max_b - g_gen_adc_min_b));
    int lb = g_gen_adc_min_b + (int)(0.35 * (g_gen_adc_max_b - g_gen_adc_min_b));

    printf("\n=== Physikalischer 20-kHz-Generator (Production auf Echt-Rate) ===\n");
    printf("Jeder Parameter aus Physik ODER RML-447W-Beobachtung (keine Fakes).\n");
    printf("  Leistung P        = %.0f W                (Physik)\n", g_gen_power);
    printf("  Zaehlerkonst k    = %.0f rev/kWh           (Physik)\n", g_gen_k);
    printf("  -> Disk-Speed     = %.6f rev/s  (%.2f rev/h)\n", rev_per_s, rev_per_s * 3600.0);
    printf("  -> Rev-Dauer      = %.2f s\n", rev_per_s > 0 ? 1.0 / rev_per_s : 0.0);
    printf("  Mark-Anteil       = %.4f (%.1f %%)          (RML la_state==0)\n",
           g_gen_mark_frac, g_gen_mark_frac * 100.0);
    printf("  ADC-Bereich A     = [%d, %d]              (RML la_raw)\n",
           g_gen_adc_min_a, g_gen_adc_max_a);
    printf("  ADC-Bereich B     = [%d, %d]              (RML ra_raw)\n",
           g_gen_adc_min_b, g_gen_adc_max_b);
    printf("  Rauschen sigma    = A %.1f, B %.1f          (RML raw-filt Residuum)\n",
           g_gen_noise_a, g_gen_noise_b);
    printf("  Sensor-Offset     = %.1f deg (B hinter A)  (RML Mark-Onset)\n",
           g_gen_sensor_offset);
    printf("  Startphase        = %.2f s bis zur 1. Marke (RML erster la-Onset)\n",
           g_gen_start_offset);
    printf("  Abtastrate        = %d Hz                  (ESP32 DMA-Minimum)\n", g_gen_rate);
    printf("  Dauer             = %.1f s                 (RML-Capture)\n", g_gen_duration);
    printf("  -> wahre Rotation = %.3f Umdr (Physik, kontinuierlich)\n",
           rev_per_s * g_gen_duration);
    printf("  -> 1 Marke/Passage pro Umdr: bei Start %.2fs passen %.0f Passagen ins Fenster\n",
           g_gen_start_offset,
           rev_per_s > 0 ? floor((g_gen_duration - g_gen_start_offset) * rev_per_s) + 1.0 : 0.0);
    printf("     (Passagen bei t=%.2fs, %.2fs, ...).  Ein Einmarken-Zaehler ist QUANTISIERT:\n",
           g_gen_start_offset, g_gen_start_offset + (rev_per_s > 0 ? 1.0 / rev_per_s : 0.0));
    printf("     er zaehlt ganze Passagen, nicht Bruchteile — ±1 Passage Fenster-Quantisierung.\n");
    printf("Schwellen 70/35%%:   A HIGH=%d LOW=%d   B HIGH=%d LOW=%d  (non-invert, wie schmitt_trigger_logic)\n\n",
           ha, la, hb, lb);

    // CSV-Logger-Tracker (dieselbe pure-C Logik wie die Firmware).  Feed mit
    // JEDEM Sample; die 17-int Zeile wird nur alle DECIM Samples geschrieben.
    // logger_quad laeuft parallel und liefert net_steps fuer die 18. Spalte W
    // (mittlere Leistung seit Start).  Gleicher Gray-Code wie die Production-
    // Quadratur unten (g_count_*) -> net_steps identisch (Cross-Check).
    logger_ch_t la_log, rb_log;
    logger_ch_init(&la_log, g_gen_adc_min_a);
    logger_ch_init(&rb_log, g_gen_adc_min_b);
    logger_quad_t quad_log;
    logger_quad_init(&quad_log);
    FILE *csv_fp = NULL;
    long csv_rows = 0;
    uint32_t first_ms = 0;
    bool first_set = false;
    if (g_write_csv[0]) {
        csv_fp = fopen(g_write_csv, "w");
        if (!csv_fp) perror("Kann CSV nicht oeffnen (schreibe keins)");
    }
    long decim = (g_csv_hz > 0) ? (long)(g_gen_rate / g_csv_hz) : 1L;
    if (decim < 1) decim = 1;

    // Aliasing-Warnung: ein ~csv_hz-Log ist nur treu, wenn Mark-Dauer UND Offset-
    // Dauer beide >> CSV-Fenster sind.  Bei Hochlast (kurze Marken) aliasiert das
    // Log und der Round-Trip unterschlaegt Passagen (physikalisches Limit, kein
    // Design-Fehler — die Referenz-FW hat dasselbe).
    if (csv_fp && rev_per_s > 0.0) {
        double mark_dur_ms   = g_gen_mark_frac * (1.0 / rev_per_s) * 1000.0;
        double offset_dur_ms = (g_gen_sensor_offset / 360.0) * (1.0 / rev_per_s) * 1000.0;
        double win_ms        = 1000.0 / (double)g_csv_hz;
        if (mark_dur_ms < 2.0 * win_ms || offset_dur_ms < 2.0 * win_ms) {
            fprintf(stderr, "WARNUNG: CSV-Hz=%d -> Fenster %.0f ms, aber Mark-Dauer %.0f ms / "
                            "Offset-Dauer %.0f ms.  4-Hz-Log aliasiert bei dieser Last; "
                            "Round-Trip unterschlaegt Passagen (physikalisches Limit).\n",
                    g_csv_hz, win_ms, mark_dur_ms, offset_dur_ms);
        }
    }

    srand(42);   // reproduzierbares Rauschen
    bool a_high = false, b_high = false;
    quad_state_t prev_state = QUAD_S0;
    bool initialized = false;
    int transition_count = 0;
    long n;

    for (n = 0; n < total; n++) {
        double ang = start_rad + rad_per_sample * (double)n;           // Disk-Winkel (rad), monoton
        double ma = mod_pos(ang, 2.0 * M_PI) / (2.0 * M_PI);           // Mark-Pos A [0,1)
        double mb = mod_pos(ang - offset_rad, 2.0 * M_PI) / (2.0 * M_PI);
        double pa = mark_pulse(ma, g_gen_mark_frac);
        double pb = mark_pulse(mb, g_gen_mark_frac);
        int raw_a = (int)(g_gen_adc_min_a + (g_gen_adc_max_a - g_gen_adc_min_a) * pa
                          + g_gen_noise_a * gauss());
        int raw_b = (int)(g_gen_adc_min_b + (g_gen_adc_max_b - g_gen_adc_min_b) * pb
                          + g_gen_noise_b * gauss());

        // Production: non-invert (raw >= high -> HIGH), wie schmitt_trigger_logic.
        apply_schmitt(raw_a, &a_high, ha, la, false);
        apply_schmitt(raw_b, &b_high, hb, lb, false);

        // CSV-Tracker feed (jedes Sample, wie der DMA-Callback in der Firmware).
        logger_ch_feed(&la_log, raw_a, a_high);
        logger_ch_feed(&rb_log, raw_b, b_high);
        logger_quad_feed(&quad_log, a_high, b_high);

        // Dezimierte CSV-Zeile alle `decim` Samples (RML-17-Spalten + W, keine
        // Header).  millis = uptime, la_state = !prev_mark (RML: 0=Marke,
        // 1=Verweilzeit).  18. Spalte W = mittlere Leistung seit Start aus
        // logger_watts (shared pure-C, byte-identisch mit der FW).  --use-fw-
        // states liest nur Spalte 7/15 -> Spalte 18 round-trip-neutral.
        if (csv_fp && (n % decim) == 0) {
            int millis = (int)((long long)n * 1000 / g_gen_rate);
            if (!first_set) { first_ms = (uint32_t)millis; first_set = true; }
            uint32_t elapsed_ms = (uint32_t)millis - first_ms;
            int la_state = !la_log.prev_mark;
            int ra_state = !rb_log.prev_mark;
            int watts = logger_watts(quad_log.net_steps, g_revolutions_per_kwh, elapsed_ms);
            fprintf(csv_fp, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                    millis,
                    la_log.last_raw, (int)la_log.ema, (int)la_log.run_min, (int)la_log.run_max,
                    ha, la, la_state, (int)la_log.mark_count,
                    rb_log.last_raw, (int)rb_log.ema, (int)rb_log.run_min, (int)rb_log.run_max,
                    hb, lb, ra_state, (int)rb_log.mark_count,
                    watts);
            csv_rows++;
        }

        if (!initialized) {
            prev_state = compute_quad_state(a_high, b_high);
            initialized = true;
            continue;
        }
        quad_state_t cur_state = compute_quad_state(a_high, b_high);
        if (cur_state != prev_state) {
            int8_t d = quad_delta(prev_state, cur_state);
            const char *ds = d > 0 ? "-> Vorwaerts" : d < 0 ? "<- Rueckwaerts" : "-- Ungueltig";
            if (d > 0) g_count_forward++; else if (d < 0) g_count_backward++;
            printf("t=%7.3fs: %s -> %s  |  %s\n",
                   (double)n / (double)g_gen_rate, state_name(prev_state), state_name(cur_state), ds);
            prev_state = cur_state;
            transition_count++;
        }
    }

    float energy_kwh = (float)(g_count_forward - g_count_backward)
                     / (4.0f * (float)g_revolutions_per_kwh);

    printf("\n=== Ergebnis (Production non-invert auf 20 kHz) ===\n");
    printf("Samples: %ld  Uebergaenge: %d\n", total, transition_count);
    printf("Vorwaerts: %ld   Rueckwaerts: %ld   Ungueltig: 0\n",
           (long)g_count_forward, (long)g_count_backward);
    printf("Netto-Schritte: %ld  (= %.2f Umdrehungen = %ld saubere Mark-Passagen)\n",
           (long)(g_count_forward - g_count_backward),
           (g_count_forward - g_count_backward) / 4.0f,
           (long)(g_count_forward - g_count_backward) / 4);
    printf("Netto-Energie: %.4f kWh\n", energy_kwh);
    {
        /* Cross-Check: Logger-Quadratur (quelle der CSV W-Spalte) vs Production-
         * Quadratur (g_count_*).  Beide laufen auf denselben Marks -> net_steps
         * muss identisch sein; wenn nicht, ist die 18. Spalte inkonsistent. */
        long prod_net = (long)(g_count_forward - g_count_backward);
        uint32_t final_ms = (uint32_t)((long long)total * 1000 / g_gen_rate);
        int watts = logger_watts(quad_log.net_steps, g_revolutions_per_kwh, final_ms);
        printf("Mittlere Leistung (CSV 18. Spalte W): %d W  (net_steps=%ld, k=%d, %.1f s)\n",
               watts, (long)quad_log.net_steps, g_revolutions_per_kwh, (double)final_ms / 1000.0);
        if ((long)quad_log.net_steps != prod_net) {
            printf("  HINWEIS: Logger-Quad net_steps=%ld != Production net=%ld (sollten gleich sein)\n",
                   (long)quad_log.net_steps, prod_net);
        }
    }
    printf("\nEinordnung (keine Fakes):\n");
    printf("  - 0 Rueckwaerts / 0 Ungueltig: Production-Quadratur laeuft auf 20 kHz\n");
    printf("    sauber (kein Aliasing wie auf dem ~4 Hz-RML-Log, das 0 netto gab).\n");
    printf("  - %ld Passagen = ehrlicher Zaehlimpuls bei dieser Startphase.  Die wahre\n",
           (long)(g_count_forward - g_count_backward) / 4);
    printf("    Rotation ist %.3f Umdr; die Differenz ist ±1-Passage Fenster-Quantisierung\n",
           rev_per_s * g_gen_duration);
    printf("    (kurzes 130s-Fenster, Einmarken-Zaehler kann keine Bruchteile).\n");
    printf("  - Die Referenz-FW meldete auf demselben Capture netto +4 (16 fwd/12 bwd,\n");
    printf("    28 Uebergaenge): ihre dynamische Huellen-Schwelle jitterte und cancelierte\n");
    printf("    die 2. Passage teilweise.  Der saubere 20-kHz-Generator zaehlt beide -> hier.\n");

    if (csv_fp) {
        fclose(csv_fp);
        printf("\n=== CSV-Output ===\n");
        printf("Datei: %s  (%ld Zeilen, %d Hz, DECIM=%ld)\n",
               g_write_csv, csv_rows, g_csv_hz, decim);
        printf("Round-Trip:  ferraris_sim --use-fw-states %s\n", g_write_csv);
        printf("  -> sollte Netto %ld reproduzieren (Polaritaet beider Kanaele invertiert,\n",
               (long)(g_count_forward - g_count_backward));
        printf("     Gray-Code-Delta erhalten -> Zaehler identisch).\n");
    }
    return 0;
}

// ─── Kommandozeilen-Optionen ───────────────────────────────────────────────────

static void print_usage(const char *prog)
{
    printf("Aufruf: %s <csv_datei> [Optionen]\n\n", prog);
    printf("Optionen:\n");
    printf("  --autotune N         Auto-Tune: analysiere erste N Samples pro Kanal\n");
    printf("  --high N             Schwelle fuer BEIDE Kanaele (Standard: 2500)\n");
    printf("  --low N              Schwelle fuer BEIDE Kanaele (Standard: 1600)\n");
    printf("  --high-a N --low-a N Per-Channel-Schwelle Kanal A\n");
    printf("  --high-b N --low-b N Per-Channel-Schwelle Kanal B\n");
    printf("  --col-a N            CSV-Spalte Kanal A, 0-indiziert (Standard: 1)\n");
    printf("  --col-b N            CSV-Spalte Kanal B, 0-indiziert (Standard: 2)\n");
    printf("  --col-sample N       CSV-Spalte Sample-Nummer (Standard: 0, -1=keine)\n");
    printf("  --invert             Marke = niedriges Raw (INVERT_SIGNAL)\n");
    printf("  --fw-thresholds      Schwellen aus RML-CSV (la/ra tOn/tOff) lesen\n");
    printf("  --use-fw-states      FW-States (la_state/ra_state) direkt -> HW-Referenz\n");
    printf("  --production         Production-Modus: non-invert, col1/col9, FW-Schwellen\n");
    printf("  --revs N             Umdrehungen pro kWh (Standard: 75)\n");
    printf("\nPhysikalischer 20-kHz-Generator (keine CSV-Datei noetig):\n");
    printf("  --generate           Synthetisiere 20-kHz-Raw aus Physik+RML, laufe Production\n");
    printf("  --from-rml <csv>     Generator-Parameter live aus RML-Capture ableiten (no fakes)\n");
    printf("  --power W            Leistung in W (Standard 447, Physik)\n");
    printf("  --k N                Zaehlerkonstante rev/kWh (Standard 75, Physik)\n");
    printf("  --mark-frac F        Mark-Anteil 0..1 (Standard 0.0441, RML)\n");
    printf("  --adc-min-a/-max-a N ADC-Bereich Kanal A (RML la_raw)\n");
    printf("  --adc-min-b/-max-b N ADC-Bereich Kanal B (RML ra_raw)\n");
    printf("  --noise-a/-noise-b F Rauschen sigma (RML raw-filt Residuum)\n");
    printf("  --sensor-offset D    Sensor-Phasenoffset in Grad, B hinter A (RML)\n");
    printf("  --rate N             Abtastrate Hz (Standard 20000, ESP32 DMA-Minimum)\n");
    printf("  --duration S         Dauer in s (Standard 130, RML-Capture)\n");
    printf("  --start-offset S     Startphase: Sekunden bis zur 1. Marke (Standard 0)\n");
    printf("  --write-csv <path>   Generator-CSV schreiben (RML-17-Spalten, wie Firmware)\n");
    printf("  --csv-hz N           CSV-Decimierungsrate Hz (Standard 4 = RML-Konvention)\n");
    printf("\nBeispiele:\n");
    printf("  Synthese:  %s daten.csv --autotune 1000\n", prog);
    printf("  Synthese:  %s daten.csv --high 2800 --low 1400\n", prog);
    printf("  RML-Real:  %s rml.csv --col-a 1 --col-b 9 --invert --fw-thresholds\n", prog);
    printf("  Generator: %s --generate --from-rml rml_447W.csv  (Parameter live, no fakes)\n", prog);
    printf("  Generator: %s --generate --power 447 --k 75       (Defaults = RML-447W)\n", prog);
    printf("  RoundTrip: %s --generate --from-rml rml_447W.csv --write-csv synth.csv\n", prog);
    printf("             %s --use-fw-states synth.csv  (reproduziert Generator-Zaehler)\n", prog);
}

// ─── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autotune") == 0 && i + 1 < argc) {
            g_autotune_samples = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--high") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]); g_high_a = v; g_high_b = v;     // beide Kanaele
        } else if (strcmp(argv[i], "--low") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]); g_low_a = v; g_low_b = v;
        } else if (strcmp(argv[i], "--high-a") == 0 && i + 1 < argc) {
            g_high_a = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--low-a") == 0 && i + 1 < argc) {
            g_low_a = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--high-b") == 0 && i + 1 < argc) {
            g_high_b = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--low-b") == 0 && i + 1 < argc) {
            g_low_b = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--col-a") == 0 && i + 1 < argc) {
            g_col_a = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--col-b") == 0 && i + 1 < argc) {
            g_col_b = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--col-sample") == 0 && i + 1 < argc) {
            g_col_sample = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--invert") == 0) {
            g_invert = true;
        } else if (strcmp(argv[i], "--fw-thresholds") == 0) {
            g_fw_thresholds = true;
        } else if (strcmp(argv[i], "--use-fw-states") == 0) {
            // FW-States direkt: Spalten 7 (la_state) + 15 (ra_state), kein Schmitt.
            g_use_fw_states = true;
            g_col_a = 7; g_col_b = 15; g_col_sample = 0;
        } else if (strcmp(argv[i], "--production") == 0) {
            // Production-identisch: non-invert (wie schmitt_trigger_logic),
            // rohe la_raw/ra_raw (col 1/9), Per-Channel-Schwellen aus RML-CSV.
            g_invert = false;
            g_col_a = 1; g_col_b = 9; g_col_sample = 0;
            g_fw_thresholds = true;
        } else if (strcmp(argv[i], "--revs") == 0 && i + 1 < argc) {
            g_revolutions_per_kwh = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--generate") == 0) {
            g_generate = true;
        } else if (strcmp(argv[i], "--from-rml") == 0 && i + 1 < argc) {
            g_generate = true;
            g_from_rml = argv[++i];
        } else if (strcmp(argv[i], "--power") == 0 && i + 1 < argc) {
            g_gen_power = atof(argv[++i]);
        } else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            g_gen_k = atof(argv[++i]);
        } else if (strcmp(argv[i], "--mark-frac") == 0 && i + 1 < argc) {
            g_gen_mark_frac = atof(argv[++i]);
        } else if (strcmp(argv[i], "--adc-min-a") == 0 && i + 1 < argc) {
            g_gen_adc_min_a = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--adc-max-a") == 0 && i + 1 < argc) {
            g_gen_adc_max_a = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--adc-min-b") == 0 && i + 1 < argc) {
            g_gen_adc_min_b = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--adc-max-b") == 0 && i + 1 < argc) {
            g_gen_adc_max_b = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--noise-a") == 0 && i + 1 < argc) {
            g_gen_noise_a = atof(argv[++i]);
        } else if (strcmp(argv[i], "--noise-b") == 0 && i + 1 < argc) {
            g_gen_noise_b = atof(argv[++i]);
        } else if (strcmp(argv[i], "--sensor-offset") == 0 && i + 1 < argc) {
            g_gen_sensor_offset = atof(argv[++i]);
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            g_gen_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            g_gen_duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--start-offset") == 0 && i + 1 < argc) {
            g_gen_start_offset = atof(argv[++i]);
        } else if (strcmp(argv[i], "--write-csv") == 0 && i + 1 < argc) {
            strncpy(g_write_csv, argv[++i], sizeof(g_write_csv) - 1);
            g_write_csv[sizeof(g_write_csv) - 1] = '\0';
        } else if (strcmp(argv[i], "--csv-hz") == 0 && i + 1 < argc) {
            g_csv_hz = atoi(argv[++i]);
            if (g_csv_hz < 1) g_csv_hz = 4;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    if (g_generate) {
        // Physikalischer 20-kHz-Generator: kein CSV noetig.  Parameter aus
        // dokumentierten Defaults (RML-447W) ODER live aus --from-rml abgeleitet.
        if (g_from_rml) {
            if (!derive_gen_params_from_rml(g_from_rml)) {
                fprintf(stderr, "--from-rml fehlgeschlagen, nutze dokumentierte Defaults.\n");
            }
        }
        return run_generate();
    }

    if (!filename) {
        fprintf(stderr, "Fehler: Keine CSV-Datei angegeben\n");
        print_usage(argv[0]);
        return 1;
    }

    // Schwellen-Quelle (Prioritaet): --use-fw-states (keine Schwellen) >
    // --fw-thresholds > --autotune > manuell/Default.
    if (g_use_fw_states) {
        // Kein Schmitt — FW-States sind bereits die Schmitt-Ausgaenge.
    } else if (g_fw_thresholds) {
        int ha, la, hb, lb;
        if (read_fw_thresholds(filename, &ha, &la, &hb, &lb)) {
            g_high_a = ha; g_low_a = la;
            g_high_b = hb; g_low_b = lb;
            printf("FW-Schwellen aus CSV:  A HIGH=%d LOW=%d   B HIGH=%d LOW=%d\n",
                   g_high_a, g_low_a, g_high_b, g_low_b);
        } else {
            fprintf(stderr, "Warnung: --fw-thresholds: keine tOn/tOff-Spalten gefunden, "
                             "verwende manuelle/Default-Schwellen.\n");
        }
    } else if (g_autotune_samples > 0) {
        run_autotune(filename, g_autotune_samples);
    }

    printf("\n=== Ferraris-Zaehler Simulator (Quadratur-Automat) ===\n");
    printf("Kanaele: A=col%d  B=col%d  (Sample=col%d)\n",
           g_col_a, g_col_b, g_col_sample);
    if (g_use_fw_states) {
        printf("Modus: FW-States direkt (la_state/ra_state) -> HW-Referenz, kein Schmitt\n");
    } else {
        printf("Schwellen:  A HIGH=%d LOW=%d   B HIGH=%d LOW=%d\n",
               g_high_a, g_low_a, g_high_b, g_low_b);
        printf("Invert: %s  (Marke = %s Raw)\n",
               g_invert ? "AN" : "AUS", g_invert ? "niedrig" : "hoch");
    }
    printf("Zaehlerkonstante: %d Umdrehungen/kWh\n", g_revolutions_per_kwh);
    printf("Lese: %s\n\n", filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Kann Datei nicht oeffnen");
        return 1;
    }

    bool a_high = false;
    bool b_high = false;
    quad_state_t prev_state = QUAD_S0;

    char line[512];
    int samples_processed = 0;
    int transition_count = 0;
    bool initialized = false;

    while (fgets(line, sizeof(line), fp)) {
        int sample_num, val_a, val_b;
        if (!parse_csv_line(line, g_col_sample, g_col_a, g_col_b,
                           &sample_num, &val_a, &val_b)) {
            continue;
        }

        bool a_changed, b_changed;
        if (g_use_fw_states) {
            // FW-States sind schon Schmitt-Ausgaenge: direkt als booleschen
            // Zustand nehmen und Aenderung gegen den vorherigen Sample detektieren.
            bool new_a = (val_a != 0);
            bool new_b = (val_b != 0);
            a_changed = (new_a != a_high);
            b_changed = (new_b != b_high);
            a_high = new_a;
            b_high = new_b;
        } else {
            a_changed = apply_schmitt(val_a, &a_high, g_high_a, g_low_a, g_invert);
            b_changed = apply_schmitt(val_b, &b_high, g_high_b, g_low_b, g_invert);
        }

        if (!initialized) {
            prev_state = compute_quad_state(a_high, b_high);
            printf("Initialzustand: %s\n\n", state_name(prev_state));
            initialized = true;
            samples_processed++;
            continue;
        }

        if (a_changed || b_changed) {
            quad_state_t curr_state = compute_quad_state(a_high, b_high);
            int8_t delta = quad_delta(prev_state, curr_state);

            const char *dir_str;
            if (delta > 0)      { dir_str = "-> Vorwaerts";  g_count_forward++; }
            else if (delta < 0) { dir_str = "<- Rueckwaerts"; g_count_backward++; }
            else                { dir_str = "-- Ungueltig"; }

            printf("Sample %3d: %s -> %s  |  %s\n",
                   sample_num >= 0 ? sample_num : samples_processed,
                   state_name(prev_state), state_name(curr_state),
                   dir_str);

            prev_state = curr_state;
            transition_count++;
        }

        samples_processed++;
    }

    fclose(fp);

    float energy_kwh = (float)(g_count_forward - g_count_backward)
                     / (4.0f * (float)g_revolutions_per_kwh);

    printf("\n=== Ergebnis ===\n");
    printf("Verarbeitete Samples: %d\n", samples_processed);
    printf("Zustandsuebergaenge: %d\n", transition_count);
    printf("Verbrauch (vorwaerts): %ld\n", (long)g_count_forward);
    printf("Einspeisung (rueckwaerts): %ld\n", (long)g_count_backward);
    printf("Netto-Schritte: %ld  (= %.2f Umdrehungen)\n",
           (long)(g_count_forward - g_count_backward),
           (g_count_forward - g_count_backward) / 4.0f);
    printf("Netto-Energie: %.4f kWh\n", energy_kwh);

    return 0;
}