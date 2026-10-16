/**
 * @file ferraris_calib.h
 * @brief Automatisches Einmessen: Aufzeichnung aller Sensoren, Schwellen je
 *        Sensor und Wahl des Zaehlpaars (pure C, PC-testbar).
 *
 * Aufzeichnung (Hotpath-tauglich): je Zeitfenster ("Bucket") das MAXIMUM
 * jedes Sensors. Das Maximum ist fuer beide Fragen das Richtige: die Marke
 * ist ein Ausschlag nach oben, und die Oberkante des Ruhepegels ist genau
 * das, was LOW sicher uebersteigen muss. Ist der Puffer voll, werden je zwei
 * Buckets zusammengefasst (Breite verdoppelt) - so passt jede Dauer in einen
 * festen Puffer, mit der bestmoeglichen Aufloesung.
 *
 * Auswertung je Sensor:
 *   base     = Median (Ruhe)
 *   peak     = hoechster Wert, der mindestens 2 Buckets haelt (kein Ausreisser)
 *   rest_hi  = 99%-Wert aller Buckets unter (base+peak)/2  (Ruhe inkl. Schwankung)
 *   gap      = peak - rest_hi
 *   LOW      = rest_hi + 30 % gap,  HIGH = rest_hi + 60 % gap
 *
 * Zaehlpaar: fuer jedes Paar nutzbarer Sensoren wird die Quadratur auf der
 * Aufzeichnung nachgespielt. Gueltig ist ein Paar nur ohne ungueltige Spruenge
 * (beide Kanaele im selben Bucket) und ohne Gegenrichtung; das Paar mit dem
 * staerksten schwaecheren Signal gewinnt. A/B wird so orientiert, dass die
 * Drehung waehrend der Aufzeichnung als Bezug (vorwaerts) zaehlt - ausser
 * reverse ist gesetzt (es wurde eingespeist).
 */

#ifndef FERRARIS_CALIB_H
#define FERRARIS_CALIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FERRARIS_CALIB_MAX_SENSORS 8
#define FERRARIS_CALIB_MIN_GAP     80    /**< kleinerer Ausschlag = Sensor unbrauchbar */
#define FERRARIS_CALIB_JSON_MAX    1536

// ─── Aufzeichnung ────────────────────────────────────────────────────────────

typedef struct {
    uint16_t *buf;            /**< rows x n Werte (Bucket-Maxima) */
    uint32_t  cap_rows;       /**< gerade */
    uint8_t   n;
    uint32_t  bucket_sets;    /**< Sample-Sets je Bucket (verdoppelt sich) */
    uint32_t  in_bucket;      /**< Sets im laufenden Bucket */
    uint16_t  cur[FERRARIS_CALIB_MAX_SENSORS];
    volatile uint32_t rows;        /**< fertige Buckets */
    volatile uint32_t total_sets;  /**< alle gefuetterten Sets */
} ferraris_calib_rec_t;

/**
 * @param buf          Speicher fuer cap_values uint16
 * @param cap_values   Groesse des Puffers in Werten (wird auf ganze, gerade Zeilen gekuerzt)
 * @param bucket_sets  Start-Breite eines Buckets in Sample-Sets (>= 1)
 * @return false bei ungueltigen Parametern
 */
bool ferraris_calib_rec_init(ferraris_calib_rec_t *r, uint16_t *buf, uint32_t cap_values,
                             int n, uint32_t bucket_sets);

/** Ein Sample-Set (n Rohwerte). Hotpath: keine Allokation, kein I/O. */
void ferraris_calib_rec_feed(ferraris_calib_rec_t *r, const int *raw);

// ─── Auswertung ──────────────────────────────────────────────────────────────

typedef struct {
    bool usable;              /**< Ausschlag gross genug */
    int  base;                /**< Median */
    int  rest_hi;             /**< Oberkante Ruhe */
    int  peak;                /**< Markenspitze */
    int  gap;                 /**< peak - rest_hi */
    int  high, low;           /**< Vorschlag */
    int  passes;              /**< Markendurchgaenge mit diesen Schwellen */
} ferraris_calib_sensor_t;

typedef struct {
    int      n;
    uint32_t rows;
    float    bucket_ms;
    ferraris_calib_sensor_t s[FERRARIS_CALIB_MAX_SENSORS];
    bool     ok;              /**< Paar gefunden */
    int      count_a, count_b;   /**< Q-Nummer 1..n (0 = keins) */
    int      steps;           /**< Schritte des gewaehlten Paars waehrend der Aufzeichnung */
    float    revolutions;
    float    period_s;        /**< mittlere Umdrehungsdauer, 0 = unbekannt */
    int      pairs_checked, pairs_valid;
    const char *message;      /**< Klartext (Ergebnis oder Grund) */
} ferraris_calib_result_t;

/**
 * @param buf       Aufzeichnung (rows x n)
 * @param bucket_ms Breite eines Buckets in ms
 * @param reverse   waehrend der Aufzeichnung wurde eingespeist (Scheibe rueckwaerts)
 */
void ferraris_calib_analyze(const uint16_t *buf, uint32_t rows, int n, float bucket_ms,
                            bool reverse, ferraris_calib_result_t *out);

/** Ergebnis als JSON-Objekt. @return Laenge wie snprintf, < 0 bei Fehler. */
int ferraris_calib_result_to_json(const ferraris_calib_result_t *r, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_CALIB_H
