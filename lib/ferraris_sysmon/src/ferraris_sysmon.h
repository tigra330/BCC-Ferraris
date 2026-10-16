/**
 * @file ferraris_sysmon.h
 * @brief Robustheit, ESP-Huelle: Resetgrund, Start-/Absturzzaehler, abgesicherter
 *        Modus, Loop-Watchdog, OTA-Bestaetigung mit Rollback, Absturzbericht.
 *
 * Ablauf:
 *   setup():  ferraris_sysmon_boot()  ganz am Anfang (liest Resetgrund, schreibt
 *             Zaehler ins NVS, entscheidet abgesicherten Modus)
 *             ferraris_sysmon_start() am Ende (startet den Loop-Watchdog)
 *   loop():   ferraris_sysmon_loop(healthy) bei jedem Durchlauf
 *
 * OTA: Arduino bestaetigt eine neue Firmware sonst sofort beim Start. Hier wird
 * das verschoben (verifyRollbackLater), bis ferraris_health "gesund" meldet.
 * Stuerzt die neue Firmware vorher ab, schaltet der Bootloader beim naechsten
 * Start selbst zurueck; laeuft sie, wird aber nicht gesund, nach dem Timeout.
 *
 * Alle Funktionen ausser feed() nur aus setup()/loop() aufrufen.
 */

#ifndef FERRARIS_SYSMON_H
#define FERRARIS_SYSMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t loop_watchdog_ms;     /**< Loop haengt so lange -> Neustart (0 = aus) */
    uint32_t stable_ms;            /**< so lange gelaufen = Absturzserie beendet */
    uint32_t safe_mode_crashes;    /**< Abstuerze in Folge -> abgesicherter Modus (0 = nie) */
    uint32_t ota_min_ms;           /**< OTA: fruehestens bestaetigen */
    uint32_t ota_stable_ms;        /**< OTA: so lange ununterbrochen gesund */
    uint32_t ota_timeout_ms;       /**< OTA: nicht gesund bis dahin -> Rollback */
} ferraris_sysmon_config_t;

/** Gewollte Neustarts: Grund wird ueber den Reset gerettet (RTC-RAM). */
typedef enum {
    FERRARIS_RESTART_NONE = 0,
    FERRARIS_RESTART_USER,         /**< Webseite / API */
    FERRARIS_RESTART_OTA,          /**< nach Firmware-Update */
    FERRARIS_RESTART_OTA_ROLLBACK, /**< neue Firmware nicht gesund -> zurueck */
    FERRARIS_RESTART_LOOP_WDT,     /**< Loop-Watchdog */
} ferraris_restart_cause_t;

typedef struct {
    const char *reset_reason;      /**< Klartext, z. B. "Absturz", "Firmware-Update" */
    uint32_t boot_count;
    uint32_t crash_count;
    uint32_t crash_streak;
    bool     safe_mode;
    bool     coredump;             /**< Absturzbericht im Flash vorhanden */
    const char *ota_state;         /**< "ok", "pruefung", "zurueckgenommen" */
    uint32_t ota_left_s;           /**< bei "pruefung": Sekunden bis zum Rollback */
} ferraris_sysmon_info_t;

typedef struct {
    bool     present;
    uint32_t size;
    char     task[16];             /**< Task, in dem der Absturz passierte */
    uint32_t pc;                   /**< Programmzaehler */
    char     reason[96];           /**< Panic-Grund, falls gespeichert */
} ferraris_coredump_info_t;

/** @return true = abgesicherter Modus (Werkseinstellungen verwenden). */
bool ferraris_sysmon_boot(const ferraris_sysmon_config_t *cfg);

/** Loop-Watchdog-Task starten (Ende von setup()). */
void ferraris_sysmon_start(void);

/**
 * @brief Pro loop()-Durchlauf: Watchdog fuettern, Absturzserie nach stabiler
 *        Laufzeit zuruecksetzen, OTA-Bestaetigung (1x pro Sekunde).
 * @param healthy ADC liefert Messungen und Netz erreichbar
 */
void ferraris_sysmon_loop(bool healthy);

/** Watchdog fuettern, z. B. waehrend langer Uploads (aus dem loop()-Task). */
void ferraris_sysmon_feed(void);

/** Grund fuer den naechsten Neustart merken (vor esp_restart). */
void ferraris_sysmon_set_restart_cause(ferraris_restart_cause_t cause);

void ferraris_sysmon_get_info(ferraris_sysmon_info_t *out);

/** Heap: frei, Tiefstwert, groesster Block. */
void ferraris_sysmon_heap(uint32_t *free_b, uint32_t *min_b, uint32_t *block_b);

void ferraris_sysmon_coredump_info(ferraris_coredump_info_t *out);

/** Bericht lesen (offset/len innerhalb des Berichts). @return false bei Fehler. */
bool ferraris_sysmon_coredump_read(uint32_t offset, void *buf, size_t len);

bool ferraris_sysmon_coredump_erase(void);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_SYSMON_H
