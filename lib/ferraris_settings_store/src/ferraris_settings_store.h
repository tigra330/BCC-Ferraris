/**
 * @file ferraris_settings_store.h
 * @brief Einstellungen im NVS (Namespace "ferrcfg", getrennt von den WLAN-Daten).
 *
 * Nur aus dem loop()-Task bzw. setup() aufrufen (Flash-Writes nie im ADC-Pfad).
 * Jedes Feld ist ein eigener Key: fehlende Keys behalten den uebergebenen
 * Default, neue Felder in spaeteren Versionen brauchen keine Migration.
 */

#ifndef FERRARIS_SETTINGS_STORE_H
#define FERRARIS_SETTINGS_STORE_H

#include "ferraris_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Gespeicherte Werte ueber @p s legen (s enthaelt vorher die Defaults).
 * @return true, wenn mindestens ein Wert aus dem NVS kam.
 */
bool ferraris_settings_load(ferraris_settings_t *s);

/** Alle Felder speichern. @return false bei NVS-Fehler. */
bool ferraris_settings_save(const ferraris_settings_t *s);

#ifdef __cplusplus
}
#endif

#endif // FERRARIS_SETTINGS_STORE_H
