/**
 * @file ferraris_log.h
 * @brief Minimales Log mit Laufzeit-Level (ersetzt ESP_LOGx).
 *
 * Warum nicht ESP_LOGx: der vorkompilierte Arduino-Core setzt
 * CONFIG_LOG_MAXIMUM_LEVEL=ERROR, d. h. ESP_LOGI/W werden weg-kompiliert.
 * Ausserdem muss der CSV-Modus alle Info-Ausgaben zur Laufzeit stummschalten
 * koennen. Pure C (nur stdio), laeuft auch nativ in den Unit-Tests.
 *
 * Format: "I tag: text\n" — eine Zeile pro Aufruf, in einem Stueck
 * ausgegeben (kein Zerreissen durch andere Tasks).
 *
 * Nicht im Hotpath (DMA-Callback) verwenden.
 */

#ifndef FERRARIS_LOG_H
#define FERRARIS_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLOG_NONE  = 0,
    FLOG_ERROR = 1,
    FLOG_WARN  = 2,
    FLOG_INFO  = 3,
} flog_level_t;

/** Laufzeit-Level setzen (Default FLOG_INFO). */
void ferraris_log_set_level(flog_level_t level);

flog_level_t ferraris_log_get_level(void);

void ferraris_log_write(flog_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define FLOGE(tag, fmt, ...) ferraris_log_write(FLOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define FLOGW(tag, fmt, ...) ferraris_log_write(FLOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define FLOGI(tag, fmt, ...) ferraris_log_write(FLOG_INFO,  tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_LOG_H
