/**
 * @file ferraris_health.h
 * @brief Entscheidungslogik fuer Robustheit (pure C, PC-testbar).
 *
 * - OTA-Bestaetigung: eine frisch per OTA installierte Firmware muss sich
 *   bewaehren (gesund = ADC liefert Messungen und Netz erreichbar), sonst
 *   wird auf die vorige Firmware zurueckgeschaltet.
 * - Absturzserie: zaehlt Starts nach Absturz/Watchdog, ohne dass das Geraet
 *   stabil lief; ab einer Schwelle startet es im abgesicherten Modus
 *   (Werkseinstellungen statt gespeicherter Einstellungen).
 *
 * Die ESP-Huelle (ferraris_sysmon) liefert Zeit, Gesundheit und Resetgrund.
 */

#ifndef FERRARIS_HEALTH_H
#define FERRARIS_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FERRARIS_OTA_WAIT = 0,     /**< noch nicht entschieden */
    FERRARIS_OTA_VALID,        /**< bestaetigen (Rollback abbrechen) */
    FERRARIS_OTA_ROLLBACK,     /**< zurueck zur vorigen Firmware */
} ferraris_ota_verdict_t;

typedef struct {
    uint32_t min_ms;           /**< fruehestens nach so viel Laufzeit bestaetigen */
    uint32_t stable_ms;        /**< so lange ununterbrochen gesund */
    uint32_t timeout_ms;       /**< bis dahin nicht bestaetigt -> Rollback */
    bool     healthy_seen;
    uint32_t healthy_since_ms;
} ferraris_ota_verify_t;

void ferraris_ota_verify_init(ferraris_ota_verify_t *v, uint32_t min_ms, uint32_t stable_ms,
                              uint32_t timeout_ms);

/**
 * @brief Einen Pruefschritt ausfuehren (z. B. 1x pro Sekunde).
 * @param uptime_ms Laufzeit seit Start
 * @param healthy   jetzt gesund (ADC liefert Sets und Netz erreichbar)
 */
ferraris_ota_verdict_t ferraris_ota_verify_step(ferraris_ota_verify_t *v, uint32_t uptime_ms,
                                                bool healthy);

/**
 * @brief Absturzserie fortschreiben.
 * @param prev        bisheriger Zaehler (aus dem NVS)
 * @param crash_reset dieser Start kam von Absturz/Watchdog
 * @return neuer Zaehler: prev + 1 nach Absturz, sonst 0 (Einschalten,
 *         Reset-Taste, gewollter Neustart beenden eine Serie)
 */
uint32_t ferraris_crash_streak_next(uint32_t prev, bool crash_reset);

/** true, wenn der Zaehler den abgesicherten Modus ausloest (threshold 0 = nie). */
bool ferraris_safe_mode(uint32_t crash_streak, uint32_t threshold);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_HEALTH_H
