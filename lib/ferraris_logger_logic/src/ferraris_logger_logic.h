/**
 * @file ferraris_logger_logic.h
 * @brief Pure-C Per-Channel-Tracker fuer das RML-17-Spalten CSV-Log.
 *
 * Haellt pro Kanal kontinuierlich die Groessen vor, die das RML-Event-Log
 * pro Zeile braucht: EMA-gefilterter Raw-Wert, asymmetrische Huelle
 * (runningMin/runningMax) und die Marken-Passagen-Anzahl.  Rein in C, ohne
 * jegliche ESP-IDF-Abhaengigkeit — identisch auf PC (Sim/Tests) und ESP32
 * (Firmware) verwendbar, was das CSV-Format in beiden Welten byte-identisch
 * macht (Voraussetzung fuer den Round-Trip).
 *
 * Gespeist wird der Tracker aus dem 20-kHz-DMA-Callback (Firmware) bzw. dem
 * Generator-Loop (Sim) mit JEDEM Sample.  Die Ausgabe-Zeile wird dagegen
 * nur alle ~250 ms (4 Hz) von einem separaten log_task / Generator-Dezim
 * aus den aktuellen Tracker-Feldern gelesen (Last-Good-Value, kein Mutex im
 * Hotpath).
 *
 * Konstanten (alle aus der RML-Referenz-FW, s. Memory ferraris-rml-reference):
 *   FILTER_ALPHA          = 0.12     EMA-Filterkoeffizient
 *   CALIB_ALPHA_TOWARD    = 0.002    Huelle: schnell zum Extrem ziehen
 *   CALIB_ALPHA_DRIFT     = 1e-6     Huelle: langsam vergessen (Gegenrichtung)
 *   CALIB_MIN_RANGE       = 300      Mindest-Spanne min..max halten
 *
 * Polaritaet: feed bekommt den PRODUCTION-Schmitt-State (true = Marke, da
 * non-invert auf dieser HW).  Die CSV-Spalte la_state wird vom Aufrufer als
 * !prev_mark gebildet (RML-Konvention: state=0 = Marke, state=1 = Verweilzeit).
 * mark_count zaehlt steigende Flanken des Production-State = Mark-Passagen.
 *
 * Leistung (W): zusaetzlich betreibt der Tracker eine eigene Gray-Code-
 * Quadratur (logger_quad_t) auf dem PAAR (mark_a, mark_b) und zaehlt net_steps
 * (Vorwaerts minus Rueckwaerts).  net_revs = net_steps/4.  logger_watts() leitet
 * daraus die mittlere Leistung seit Start ab:
 *   W = (net_steps/4) / k * (3600 / elapsed_s) * 1000
 *     = net_steps * 9e8 / (k * elapsed_ms)
 * Warum eine *eigene* Quadratur im Logger?  Die Firmware-Logger-Komponente hat
 * keinen Zugriff auf den Production-Zaehler in ferraris_data; sie re-derives
 * die Quadratur aus den Marks, die sie ohnehin jeden Sample bekommt (gleicher
 * Gray-Code wie quadrature_decoder → identisches net_steps).  Der Sim speist
 * dasselbe logger_quad, sodass die 18. CSV-Spalte (W) in FW und Sim byte-
 * identisch entsteht.  Physik-Hinweis: ein Einmarken-Zaehler quantisiert
 * Energie auf ganze Umdrehungen → W ist bei kurzem Fenster grob (±1 Umdr);
 * das ist Fundamentalphysik, kein Bug (siehe Memory ferraris-rml-reference).
 */

#ifndef FERRARIS_LOGGER_LOGIC_H
#define FERRARIS_LOGGER_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Konstanten (RML-Referenz-FW) ─────────────────────────────────────────────

#define LOGGER_FILTER_ALPHA       0.12f
#define LOGGER_CALIB_ALPHA_TOWARD 0.002f
#define LOGGER_CALIB_ALPHA_DRIFT  1e-6f
#define LOGGER_CALIB_MIN_RANGE    300

// ─── Types ──────────────────────────────────────────────────────────────────

/**
 * @brief Per-Channel-Tracker (alle Felder public — log_task liest direkt).
 *
 * Writer: der Sample-Feed-Pfad (DMA-Callback / Generator-Loop), single-writer.
 * Reader: der log_task / Generator-Dezim, single-reader.  32-bit/float-Felder
 * sind auf Xtensa atomar; bewusstes Last-Good-Value ohne Mutex (4-Hz-Diagnose-
 * CSV, Vorbild RML).
 */
typedef struct {
    int     last_raw;     /**< Letzter gefuetterter Raw-Wert (Spalte la_raw) */
    float   ema;          /**< EMA ueber Raw (Spalte la_filt), ALPHA=0.12 */
    float   run_min;      /**< Asymm. Huelle min (Spalte la_min) */
    float   run_max;      /**< Asymm. Huelle max (Spalte la_max) */
    int32_t mark_count;   /**< Mark-Passagen = steigende Flanken production-state (la_count) */
    bool    prev_mark;    /**< Letzter production-state (true=Marke); la_state = !prev_mark */
} logger_ch_t;

/**
 * @brief Gray-Code-Quadratur auf dem Mark-PAAR (a,b) fuer die W-Spalte.
 *
 * Selbst-contained (spiegelt quadrature_decoder, aber ohne Dep, damit der
 * Logger 0 ESP-IDF-Abhaengigkeit behaelt).  Zustand S0=(0,0) S1=(1,0) S2=(1,1)
 * S3=(0,1); Vorwaerts = +1 mod 4, Rueckwaerts = -1 mod 4, ±2 = ungueltig.
 * net_revs = net_steps / 4 (1 Umdr = 4 Gray-Code-Uebergaenge bei Einmarken-Scheibe).
 */
typedef struct {
    int8_t  state;        /**< Gray-Code-Zustand 0..3, -1 vor erstem Sample */
    int32_t net_steps;    /**< Vorwaerts minus Rueckwaerts (net_revs = net_steps/4) */
    bool    initialized;  /**< wahr ab erstem Sample (erstes Sample legt Startzustand) */
} logger_quad_t;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

/**
 * @brief Tracker initialisieren: EMA + Huelle auf seed_raw, MIN_RANGE erzwingen.
 *
 * ema = seed_raw; run_min = run_max = seed_raw, dann auseinandergezogen auf
 * Spanne >= CALIB_MIN_RANGE (seed-150 / seed+150).  prev_mark=false, count=0.
 */
void logger_ch_init(logger_ch_t *c, int seed_raw);

// ─── Processing ─────────────────────────────────────────────────────────────

/**
 * @brief Ein Sample in den Tracker einspeisen (JEDES Sample, ~20 kHz).
 *
 * Aktualisiert last_raw, EMA, asymmetrische Huelle (min/max) und inkrementiert
 * mark_count bei steigender Flanke des Production-State (0->1 = Marke betreten).
 * Kein I/O, keine Sperren — hotpath-tauglich.  NULL-sicher.
 *
 * @param c          Tracker (oder NULL -> no-op)
 * @param raw        Raw-ADC-Wert
 * @param mark_state Production-Schmitt-State (true = Marke present)
 */
void logger_ch_feed(logger_ch_t *c, int raw, bool mark_state);

// ─── Quadratur + Leistung (fuer 18. Spalte W) ────────────────────────────────

/**
 * @brief Quadratur-Tracker initialisieren (state=-1, net_steps=0, vor Start).
 */
void logger_quad_init(logger_quad_t *q);

/**
 * @brief Ein Mark-PAAR in die Quadratur einspeisen (JEDES Sample, wie ch_feed).
 *
 * Bestimmt den Gray-Code-Zustand aus (a,b), vergleicht mit dem Vorgaenger und
 * inkrementiert/dekrementiert net_steps bei gueltigem +1/-1-Uebergang.
 * ±2-Uebergaenge (S0<->S2, S1<->S3) werden verworfen (ungueltig).  NULL-sicher.
 *
 * @param q Tracker (oder NULL -> no-op)
 * @param a  Production-Schmitt-State Kanal A (true = Marke)
 * @param b  Production-Schmitt-State Kanal B (true = Marke)
 */
void logger_quad_feed(logger_quad_t *q, bool a, bool b);

/**
 * @brief Mittlere Leistung [W] seit Start aus dem Quadratur-Netto.
 *
 * W = (net_steps/4) / k_rev_per_kwh * (3600 / elapsed_s) * 1000
 *   = net_steps * 900000000 / (k_rev_per_kwh * elapsed_ms)   (Ganzzahl-Schnitt)
 *
 * Cumulative average seit erstem Sample — bei konstanter Last konvergiert das
 * gegen die angelegte Leistung (modulo ±1-Umdrehungs-Quantisierung, die mit
 * laengerem Fenster schrumpft).  Guards: k<=0, elapsed_ms==0 oder net_steps==0
 * -> 0 (zu wenig Daten).  Negatives net_steps -> negatives W (Einspeisung).
 *
 * @param net_steps       Quadratur-Netto (Vorwaerts - Rueckwaerts)
 * @param k_rev_per_kwh   Zaehlerkonstante (Umdrehungen pro kWh, z.B. 75)
 * @param elapsed_ms      Millisekunden seit erstem Sample
 * @return                Leistung in Watt (int, gegen Null abgeschnitten)
 */
int logger_watts(int32_t net_steps, int k_rev_per_kwh, uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_LOGGER_LOGIC_H