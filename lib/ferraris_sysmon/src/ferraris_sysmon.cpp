/**
 * @file ferraris_sysmon.cpp
 * @brief Robustheit, ESP-Huelle (siehe ferraris_sysmon.h).
 */

#include "ferraris_sysmon.h"

#include <Arduino.h>
#include <Preferences.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ferraris_health.h"
#include "ferraris_log.h"

static const char *TAG = "sysmon";
static const char *NVS_NAMESPACE = "ferrhlth";

// Ueberlebt Software-Reset und Absturz, nicht aber Einschalten -> Magic.
#define RTC_MAGIC 0x46455252u   // "FERR"
typedef struct {
    uint32_t magic;
    uint32_t cause;
} rtc_note_t;
static RTC_NOINIT_ATTR rtc_note_t s_rtc;

static ferraris_sysmon_config_t s_cfg;
static ferraris_sysmon_info_t   s_info;
static volatile uint32_t        s_heartbeat_ms;   // Writer: loop()-Task, Reader: Watchdog-Task
static bool                     s_watchdog_on;
static bool                     s_streak_cleared;
static bool                     s_ota_pending;
static ferraris_ota_verify_t    s_ota;
static uint32_t                 s_last_ota_step;

/**
 * Arduino-Hook: die OTA-Bestaetigung NICHT schon in initArduino() erledigen,
 * sondern erst, wenn ferraris_sysmon_loop() "gesund" gesehen hat.
 */
extern "C" bool verifyRollbackLater()
{
    return true;
}

// ─── Start ───────────────────────────────────────────────────────────────────

static const char *restart_cause_text(uint32_t cause)
{
    switch (cause) {
        case FERRARIS_RESTART_USER:         return "Neustart (Webseite)";
        case FERRARIS_RESTART_OTA:          return "Firmware-Update";
        case FERRARIS_RESTART_OTA_ROLLBACK: return "Update zurueckgenommen";
        case FERRARIS_RESTART_LOOP_WDT:     return "Loop-Watchdog";
        default:                            return "Software-Neustart";
    }
}

static const char *reset_text(esp_reset_reason_t r, uint32_t cause)
{
    switch (r) {
        case ESP_RST_POWERON:   return "Einschalten";
        case ESP_RST_EXT:       return "Reset-Pin";
        case ESP_RST_SW:        return restart_cause_text(cause);
        case ESP_RST_PANIC:     return "Absturz";
        case ESP_RST_INT_WDT:   return "Interrupt-Watchdog";
        case ESP_RST_TASK_WDT:  return "Task-Watchdog";
        case ESP_RST_WDT:       return "Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep-Sleep";
        case ESP_RST_BROWNOUT:  return "Unterspannung";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unbekannt";
    }
}

static bool is_crash(esp_reset_reason_t r, uint32_t cause)
{
    // Unterspannung zaehlt nicht: dagegen helfen keine Werkseinstellungen.
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || (r == ESP_RST_SW && cause == FERRARIS_RESTART_LOOP_WDT);
}

bool ferraris_sysmon_boot(const ferraris_sysmon_config_t *cfg)
{
    s_cfg = *cfg;
    memset(&s_info, 0, sizeof(s_info));

    const esp_reset_reason_t r = esp_reset_reason();
    const uint32_t cause = (s_rtc.magic == RTC_MAGIC) ? s_rtc.cause : FERRARIS_RESTART_NONE;
    s_rtc.magic = RTC_MAGIC;
    s_rtc.cause = FERRARIS_RESTART_NONE;
    const bool crash = is_crash(r, cause);
    s_info.reset_reason = reset_text(r, cause);

    // Zaehler: ein Schreibvorgang pro Start (vor dem ADC-Start, nie im Hotpath).
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        s_info.boot_count   = p.getUInt("boots", 0) + 1;
        s_info.crash_count  = p.getUInt("crashes", 0) + (crash ? 1 : 0);
        s_info.crash_streak = ferraris_crash_streak_next(p.getUInt("streak", 0), crash);
        p.putUInt("boots", s_info.boot_count);
        p.putUInt("crashes", s_info.crash_count);
        p.putUInt("streak", s_info.crash_streak);
        p.end();
    } else {
        FLOGW(TAG, "NVS fuer Startzaehler nicht verfuegbar");
    }
    s_info.safe_mode = ferraris_safe_mode(s_info.crash_streak, s_cfg.safe_mode_crashes);

    // Absturzbericht und Rollback-Zustand aendern sich nur ueber einen Neustart.
    s_info.coredump = esp_core_dump_image_check() == ESP_OK;

    esp_ota_img_states_t state;
    const esp_partition_t *run = esp_ota_get_running_partition();
    s_ota_pending = run && esp_ota_get_state_partition(run, &state) == ESP_OK &&
                    state == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_ota_pending) {
        ferraris_ota_verify_init(&s_ota, s_cfg.ota_min_ms, s_cfg.ota_stable_ms, s_cfg.ota_timeout_ms);
        s_info.ota_state = "pruefung";
    } else {
        s_info.ota_state = esp_ota_get_last_invalid_partition() ? "zurueckgenommen" : "ok";
    }

    // Immer sichtbar (auch im CSV-Modus), als '#'-Kommentarzeile.
    printf("# Start Nr. %lu: %s%s%s\n", (unsigned long)s_info.boot_count, s_info.reset_reason,
           s_info.coredump ? ", Absturzbericht vorhanden" : "",
           s_ota_pending ? ", neue Firmware wird geprueft" : "");
    if (s_info.safe_mode) {
        printf("# ABGESICHERTER MODUS: %lu Abstuerze in Folge - starte mit Werkseinstellungen\n",
               (unsigned long)s_info.crash_streak);
    }
    return s_info.safe_mode;
}

// ─── Loop-Watchdog ───────────────────────────────────────────────────────────

void ferraris_sysmon_set_restart_cause(ferraris_restart_cause_t cause)
{
    s_rtc.magic = RTC_MAGIC;
    s_rtc.cause = (uint32_t)cause;
}

void ferraris_sysmon_restart(void)
{
    if (s_cfg.before_restart) s_cfg.before_restart();
    fflush(stdout);
    esp_restart();
}

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t idle = millis() - s_heartbeat_ms;
        if (idle > s_cfg.loop_watchdog_ms) {
            printf("# Loop-Watchdog: loop() haengt seit %lu ms - Neustart\n", (unsigned long)idle);
            ferraris_sysmon_set_restart_cause(FERRARIS_RESTART_LOOP_WDT);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

void ferraris_sysmon_start(void)
{
    s_heartbeat_ms = millis();
    if (!s_cfg.loop_watchdog_ms || s_watchdog_on) return;
    // Prio 2: ueber loop() (1), damit ein Endlos-Loop dort ihn nicht aushungert.
    if (xTaskCreate(watchdog_task, "loop_wdt", 2560, NULL, 2, NULL) == pdPASS) {
        s_watchdog_on = true;
    } else {
        FLOGE(TAG, "Loop-Watchdog konnte nicht gestartet werden");
    }
}

void ferraris_sysmon_feed(void)
{
    s_heartbeat_ms = millis();
}

// ─── Laufend ─────────────────────────────────────────────────────────────────

void ferraris_sysmon_loop(bool healthy)
{
    const uint32_t now = millis();
    s_heartbeat_ms = now;

    if (!s_streak_cleared && now >= s_cfg.stable_ms) {
        s_streak_cleared = true;
        if (s_info.crash_streak) {
            Preferences p;
            if (p.begin(NVS_NAMESPACE, false)) {
                p.putUInt("streak", 0);
                p.end();
            }
            FLOGI(TAG, "Stabil seit %lu s - Absturzserie zurueckgesetzt", (unsigned long)(now / 1000));
        }
    }

    if (!s_ota_pending || now - s_last_ota_step < 1000) return;
    s_last_ota_step = now;
    s_info.ota_left_s = now < s_cfg.ota_timeout_ms ? (s_cfg.ota_timeout_ms - now) / 1000 : 0;

    switch (ferraris_ota_verify_step(&s_ota, now, healthy)) {
        case FERRARIS_OTA_VALID:
            s_ota_pending = false;
            s_info.ota_state = "ok";
            esp_ota_mark_app_valid_cancel_rollback();
            printf("# Neue Firmware bestaetigt (gesund nach %lu s)\n", (unsigned long)(now / 1000));
            break;
        case FERRARIS_OTA_ROLLBACK: {
            printf("# Neue Firmware nach %lu s nicht gesund (ADC/Netz) - schalte zurueck\n",
                   (unsigned long)(now / 1000));
            ferraris_sysmon_set_restart_cause(FERRARIS_RESTART_OTA_ROLLBACK);
            if (s_cfg.before_restart) s_cfg.before_restart();
            fflush(stdout);
            // Kehrt nur zurueck, wenn es keine gueltige Vorgaenger-Firmware gibt.
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            printf("# Rollback nicht moeglich (%s) - behalte diese Firmware\n", esp_err_to_name(err));
            esp_ota_mark_app_valid_cancel_rollback();
            s_ota_pending = false;
            s_info.ota_state = "ok";
            break;
        }
        default:
            break;
    }
}

// ─── Abfragen ────────────────────────────────────────────────────────────────

void ferraris_sysmon_get_info(ferraris_sysmon_info_t *out)
{
    if (out) *out = s_info;
}

void ferraris_sysmon_heap(uint32_t *free_b, uint32_t *min_b, uint32_t *block_b)
{
    if (free_b)  *free_b  = esp_get_free_heap_size();
    if (min_b)   *min_b   = esp_get_minimum_free_heap_size();
    if (block_b) *block_b = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void ferraris_sysmon_coredump_info(ferraris_coredump_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    size_t addr = 0, size = 0;
    if (!s_info.coredump || esp_core_dump_image_get(&addr, &size) != ESP_OK) return;
    out->present = true;
    out->size    = size;
    esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)malloc(sizeof(*sum));
    if (sum) {
        if (esp_core_dump_get_summary(sum) == ESP_OK) {
            strlcpy(out->task, sum->exc_task, sizeof(out->task));
            out->pc = sum->exc_pc;
        }
        free(sum);
    }
    if (esp_core_dump_get_panic_reason(out->reason, sizeof(out->reason)) != ESP_OK) {
        out->reason[0] = '\0';
    }
}

bool ferraris_sysmon_coredump_read(uint32_t offset, void *buf, size_t len)
{
    size_t addr = 0, size = 0;
    if (!s_info.coredump || esp_core_dump_image_get(&addr, &size) != ESP_OK) return false;
    if (offset > size || len > size - offset) return false;
    return esp_flash_read(NULL, buf, addr + offset, len) == ESP_OK;
}

bool ferraris_sysmon_coredump_erase(void)
{
    if (esp_core_dump_image_erase() != ESP_OK) return false;
    s_info.coredump = false;
    return true;
}
