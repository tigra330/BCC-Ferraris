# AGENTS.md — RML 2.0 BAScloud: Ferraris-Zähler-Ausleser (ESP32, Arduino)

Arduino-Port von `ferraris-pio` (ESP-IDF, `~/Dokumente/PlatformIO/Projects/ferraris-pio`),
erweitert um WLAN. Diese Datei beschreibt Funktionen, Architektur und Konventionen.

## Zweck

Ein ESP32 liest einen analogen **Ferraris-Stromzähler** (Drehscheibe mit Marke) optisch aus und
berechnet daraus Energie (kWh) und Leistung (W), inkl. Richtung (Bezug / Einspeisung). Ausgabe über
die Konsole (Text oder RML-CSV) und über **WLAN** (Dashboard + JSON, optional MQTT). Ohne WLAN
startet ein **Einrichtungs-Access-Point**, über den die WLAN-Zugangsdaten im Browser gesetzt werden.

Umfang gegenüber `ferraris-pio`: Phase 1 (ADC/DMA, Schmitt, Quadratur) + CSV-Logger + WLAN.
**Nicht portiert:** Kamera/CNN (Phase 2), D0-Ausgabe (Phase 3), `ferraris_autocal`,
Oneshot-Poll-Pfad. Statt `ferraris_persist` gibt es das einfachere `ferraris_counter_store`. Bei Bedarf aus `ferraris-pio` übernehmen.

## Build & Toolchain

- **PlatformIO**, Plattform pioarduino `61.04.00-RC1` mit `framework = arduino`
  = Arduino-ESP32 **4.0.0-RC1** auf **ESP-IDF 6.1**. Die IDF-Treiber (`adc_continuous`,
  `adc_oneshot`, `esp-mqtt`) werden direkt genutzt; keine externen `lib_deps` außer Unity (Tests).
- Board `esp32dev`, Flash **4 MB** DIO 40 MHz, zwei OTA-Slots je 1920 KB (Image aktuell ~1,26 MB).
- `pio` liegt unter `~/.platformio/penv/bin/pio` (nicht im PATH).

```sh
pio run                         # bauen (env esp32)
pio run -t upload               # flashen (Devkit, Auto-Reset per DTR/RTS)
pio device monitor              # Konsole 115200 Baud
pio test -e native              # Unit-Tests der pure-C-Logik auf dem PC
```

**Partitionstabelle** (`partitions_ferraris.csv`): `nvs` @0x9000 (0x6000), `phy_init` @0xf000,
`otadata` @0x10000 (0x2000), `app0`/`app1` (ota_0/ota_1) @0x20000/@0x200000 (je 0x1E0000),
`coredump` @0x3E0000. NVS-Lage wie in `ferraris-pio` → WLAN-Daten/Einstellungen überleben den
Umstieg von der alten 2-MB-Tabelle.

**`board_upload.arduino.boot_app0 = 0x10000`**: Der Arduino-Core flasht `boot_app0.bin` (otadata
→ app0) per Default nach 0xe000 — das wäre hier die letzte NVS-Seite. Die Option legt es auf
unser otadata; so startet nach jedem USB-Upload sicher app0 (auch wenn vorher per OTA app1 lief).
Das frühere Filterskript `tools/pio_no_boot_app0.py` ist damit entfallen.

**OTA** (`POST /api/ota`, Karte „Firmware-Update“ auf `/config`): Arduino `Update` schreibt
`firmware.bin` in den freien Slot, `Update.end(true)` prüft das Image und schaltet otadata erst
bei Erfolg um. Beim Flash-Schreiben können kurz ADC-Samples fehlen; Zählung läuft weiter.

**Rollback** (`ferraris_sysmon`, Logik in `ferraris_health`): Der vorkompilierte Core hat
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Wir überschreiben den schwachen Arduino-Hook
`verifyRollbackLater()` → `true` (Disassembly prüfen, falls der Linker sich ändert), damit
`initArduino()` die neue Firmware nicht sofort bestätigt. Bestätigt wird in
`ferraris_sysmon_loop()`, wenn sie nach ≥ 60 s Laufzeit 30 s **ununterbrochen gesund** war
(ADC-Sets/s > 0 **und** `ferraris_net_healthy()`: STA verbunden oder AP mit Gerät / ohne WLAN-Konfig;
MQTT zählt nicht). Sonst nach 5 min `esp_ota_mark_app_invalid_rollback_and_reboot()`. Stürzt sie
vorher ab, schaltet der Bootloader beim nächsten Start selbst zurück (PENDING_VERIFY → ABORTED).
Zeiten: `FERRARIS_OTA_VERIFY_MIN/STABLE/TIMEOUT_MS`. Ein USB-Upload (boot_app0) ist nie „pending“.

**Robustheit** (`ferraris_sysmon`):
- Resetgrund (`esp_reset_reason` + eigener Grund im `RTC_NOINIT`-RAM für gewollte Neustarts:
  Webseite, OTA, Rollback, Loop-Watchdog) → Status `health.reset`, Konsole `# Start Nr. …`.
- NVS-Namespace `ferrhlth`: `boots`, `crashes`, `streak` (Abstürze in Folge). Ein Schreibvorgang
  pro Start in `setup()` vor dem ADC, `streak` → 0 nach `FERRARIS_STABLE_UPTIME_MS` (60 s).
- **Abgesicherter Modus**: `streak ≥ FERRARIS_SAFE_MODE_CRASHES` (3) → `s_boot` = Defaults,
  gespeicherte Einstellungen bleiben auf `/config` sichtbar/editierbar. Absturz = Panic/WDT oder
  Loop-Watchdog, nicht Unterspannung/Einschalten.
- **Loop-Watchdog**: Task `loop_wdt` (Prio 2) startet neu, wenn `loop()` `FERRARIS_LOOP_WATCHDOG_MS`
  (30 s) keinen Herzschlag liefert. Lange Blocker müssen `ferraris_sysmon_feed()` rufen
  (OTA-Upload, Coredump-Download). Der Task-WDT des Cores überwacht nur Idle CPU0.
- **Zählerstand sichern**: `counter_loop()` speichert alle `FERRARIS_COUNTER_SAVE_INTERVAL_MS`
  (5 min), aber nur bei Änderung; `before_restart`-Hook in `ferraris_sysmon_config_t` sichert vor
  Webseiten-/OTA-Neustart und Rollback (nicht beim Loop-Watchdog: loop() hängt evtl. in einem
  Flash-Zugriff → NVS-Sperre). Ein- und Ausschalten per `FERRARIS_COUNTER_PERSIST_ENABLE`.
- Coredump (ELF im Flash): Existenz beim Start gecacht, Zusammenfassung (Task, PC, Panic-Grund)
  auf `/config`, Download/Löschen per API.

**Version** (`tools/pio_version.py`, post-Skript): `FERRARIS_FW_VERSION` = `git describe --tags
--dirty` ohne „v“ (`2.2.0`, `2.2.0-3-g1a2b3c4`, `…-dirty`), nur für `projenv` (sonst baut jeder
Commit das Framework neu). Fallback ohne Git in `src/main.cpp` — beim Release mitziehen.
Release: Fallback anpassen, committen, `git tag -a VX.Y.Z` (Schreibweise wie `V2.1.0`), pushen, auf GitHub ein Release zum Tag
mit `firmware.bin` (OTA), `firmware.factory.bin` (USB, leeres Board) und `firmware.elf`
(Coredump-Auswertung) anlegen.

**Ein Adapter:** Geflasht und überwacht wird über den USB-Seriell-Adapter des Boards (Auto-Reset
per DTR/RTS, Port automatisch). Den GPIO2-Break-Workaround mit zweitem Dongle (Env `box`) aus
`ferraris-pio` gibt es hier bewusst nicht mehr.

Vorkompilierte Core-Einstellungen (nicht änderbar ohne Hybrid-Build): `FREERTOS_HZ=1000`,
Task-WDT 5 s mit Panic (nur Idle-Task CPU0), `CONFIG_LOG_MAXIMUM_LEVEL=ERROR` (→ eigenes Log).

## Konfiguration

- **`include/ferraris_config.h`** ersetzt Kconfig. Alle Werte `#ifndef`-geschützt, überschreibbar per
  `build_flags = -DNAME=wert`. Schalter sind `0/1`-Makros (`#if`, nicht `#ifdef`).
- **WLAN-Zugangsdaten** werden über die Webseite `/wifi` gesetzt und im NVS gespeichert
  (Namespace `ferrnet`, Keys `ssid`/`pass`). Nur wenn dort nichts steht, gelten
  `FERRARIS_WIFI_SSID/PASSWORD` aus **`include/secrets.h`** (gitignored, Vorlage `secrets.example.h`).
  MQTT-Zugang (`FERRARIS_MQTT_URI/USER/PASSWORD`) aus `secrets.h` ist nur der Default.
- **Einstellungsseite `/config`** (`ferraris_settings` + `ferraris_settings_store`): k, Schmitt
  HIGH/LOW **je Eingang Q1..Q8** (NVS `hi1..hi8`/`lo1..lo8`, alte `thr_hi/thr_lo` werden beim Laden
  übernommen und beim Speichern entfernt), Hülle auto/fest (**sofort wirksam**), Anzahl Eingänge (2..8) und GPIO je Eingang,
  Zählpaar A/B, Hostname, MQTT
  (URI/Benutzer/Passwort/Präfix/Intervall), AP-/Admin-Passwort (**nach Neustart**). Gespeichert im
  NVS-Namespace `ferrcfg` (ein Key je Feld), beim Start Vorrang vor den Makros; ungültiger
  NVS-Stand → Fehlermeldung, Defaults. `s_boot` (Stand beim Start, an Sampler/WLAN/MQTT übergeben)
  vs. `s_settings` (gespeichert) in `main.cpp` → `restart_pending`. Die Makros in
  `ferraris_config.h` bleiben die Defaults („Standardwerte eintragen“ auf der Seite).

Wichtige Defaults: k = 150 U/kWh, 5 Eingänge (`FERRARIS_SENSOR_COUNT`), Q1..Q5 = GPIO36/39/34/35/32
(Q6..Q8 = 33/37/38), Zählpaar A=Q1 B=Q2, DMA 50 kHz,
Schmitt HIGH=3450 LOW=3300, Auto-Tune aus, CSV-Log an (250 ms), WLAN an, Host `ferraris`,
MQTT-Intervall 10 s, Topic-Präfix `ferraris`, AP `Ferraris-XXXX` mit Passwort `ferraris-setup`
(= auch Admin-Passwort, Benutzer `admin`).

## Hardware

- **2..8 Lichtschranken Q1..Qn** (Reflex-Phototransistor + Vorwiderstand) an **ADC1-Pins 32..39**,
  Anzahl und Pins per `/config` (Neustart). Die Marke läuft bei Bezug Q1→Q2→…
  **Nur ADC1** — ADC2 ist mit WLAN nicht nutzbar. Die DMA-Rate (50 kHz) teilt sich auf alle
  Eingänge (bei 8: ~6 kHz je Sensor).
- **Zählpaar A/B** (`FERRARIS_COUNT_A_Q`/`_B_Q`): A = Sensor, den die Marke bei Bezug zuerst erreicht.
- Signal: 12 bit, Dämpfung 12 dB, **nicht invertiert** (Marke = hoher Rohwert auf dieser HW).

## Datenfluss

```
ADC1 continuous/DMA (50 kHz gesamt, ~10 kHz je Kanal)
  └─ adc_sampler: Reader-Task (Prio 5, Core 1) setzt Sample-Sets raw[0..4] zusammen
       └─ dma_sample_callback(raw, n, now_us)                  [src/main.cpp]
            ├─ schmitt_logic_process(A), (B) → Flanken
            ├─ bei Flanke: quad_decoder_process → +1/-1/0
            │     └─ falls s_counting: ferraris_data_add_quad(dir, esp_timer-ms)
            ├─ falls s_collecting: autotune_collector_add()     (Start-Kalibrierung)
            └─ immer: ferraris_logger_feed() + feed_sensors()   (EMA/Hülle je Sensor)
  └─ Snapshot-Ring (dezimiert) der A/B-Paare → periodisches Re-Tune
ferraris_data ──► print_task (Text) | log_task (CSV) | loop(): HTTP + MQTT (ferraris_net)
```

### Boot-Ablauf (`setup()`)

1. Einstellungen laden (Defaults + NVS `ferrcfg`), Boot-Konfiguration ausgeben (Firmware,
   Arduino/IDF, Chip, Flash, PSRAM, k, Pins, Schwellen, WLAN).
2. `ferraris_data_create(k)`, `quad_decoder_create()`, `ferraris_logger_create()`, gespeicherten
   Zählerstand laden (`counter_restore`, Konsole `# Zaehlerstand geladen: …`).
3. Q-Pins auf ADC1-Kanäle abbilden, Duplikate prüfen (`static_assert` für die Default-Makros;
   Anzahl `s_n_sensors`, Pins `s_sensor_pins` und Zählpaar `s_idx_a/b` kommen aus den
   Einstellungen und sind ab dem Start fest).
4. **Hardware-Selbsttest** (`ferraris_hwdetect_run`) mit eigener Oneshot-Unit, danach freigeben.
5. Schmitt-Logik mit Defaults, Logger konfigurieren.
6. `ferraris_net_begin()` (kehrt sofort zurück) — **vor** dem ADC: der WLAN-Start kalibriert den
   Funkteil und kann eine bereits laufende I2S-ADC-Abtastung einfrieren lassen.
7. DMA-Sampler erstellen + starten; optional Auto-Tune (blockierend) + Re-Tune-Task, sonst sofort
   `s_counting = true`; `print_task` (nur ohne CSV-Log).
8. Bei CSV-Log: Log-Level auf ERROR (`ferraris_log` und `esp_log`), `log_task` starten.

`loop()` ruft `adc_watchdog()`, `calib_loop()`, `counter_loop()`, `ferraris_net_loop()` und `ferraris_sysmon_loop(gesund)`, dann
`delay(5)` (beim ersten Durchlauf `ferraris_sysmon_start()`). Die **ADC-Überwachung**
misst die Sample-Sets/s (Status-JSON `adc.sets_per_s`, Webseiten-Fußzeile) und startet den Sampler
neu, wenn `FERRARIS_ADC_STALL_MS` (3 s) lang keine Sets kommen (Meldung `# ADC-Abtastung liefert
keine Messungen …` mit Diagnosezählern, Zähler `adc.restarts`). Konstante Rohwerte sind kein
Kriterium (übersteuerter Sensor = legitim 4095).

**ESP32-I2S-DMA-Eigenheit:** Die Werte kommen paarweise vertauscht, nicht in Pattern-Reihenfolge.
Der Sampler setzt Sets daher reihenfolge-unabhängig zusammen (jeder Kanal einmal frisch → Set).
Mit der alten „letzter Pattern-Kanal schließt das Set“-Logik entstanden bei 5 Kanälen nie Sets.
`adc_continuous_stop()` muss im selben Task wie `start()` laufen (IDF-`adc_lock` ist ein Mutex).

## Bibliotheken (`lib/`)

Grundregel: **Handle-basierte APIs** (`x_create()` / `x_destroy()`, opake Structs) und
**Trennung von reiner Logik (pure C, PC-testbar) und ESP-Hülle**. Jede Bibliothek: `lib/<name>/src/`.

| Bibliothek | Art | Funktion |
|---|---|---|
| `quadrature_decoder` | pure C | Gray-Code-Automat S0=(0,0) S1=(1,0) S2=(1,1) S3=(0,1). Vorwärts +1, rückwärts −1, ±2 ungültig. 1 Umdrehung = 4 Schritte. Unverändert aus `ferraris-pio`. |
| `schmitt_logic` | pure C | `schmitt_trigger_logic` (Hysterese, `set_thresholds` erhält Zustand), `schmitt_trigger_autotune` (64-Bin-Histogramm, zwei Peaks, Schwellen im Tal), `autotune_collector` (dauerbasiert bis beide Populationen sichtbar). Unverändert. |
| `ferraris_logger_logic` | pure C | Tracker je Kanal: EMA (α=0.12), asymmetrische Hülle (0.002 / 1e-6, Mindestspanne 300), Markenzähler; eigene Quadratur fürs W-Feld. Unverändert, identisch im Simulator. |
| `ferraris_power` | pure C | **Neu.** Momentanleistung aus dem Abstand Schritt n ↔ n−4 (= genau eine Umdrehung, auch bei ungleichmäßigen Schritten): `P = 3.6e9 / (k · T_ms)`. Ohne neuen Schritt fällt die Anzeige als obere Schranke ab; Richtungswechsel setzt zurück. |
| `ferraris_data` | pure C | Zähler (Single-Writer, 32-bit atomar), `import/export/energy_kwh`, `get_power_watts(now_ms)` (Timeout 1 h). `set_quad(fwd, bwd)` (Laden/Zurücksetzen, loop()-Task) setzt nur einen Basiswert — die Hotpath-Zähler bleiben unangetastet, kein Rennen mit dem DMA-Callback. Ohne Kameraziffern → jetzt PC-testbar. |
| `ferraris_counter_store` | ESP (C++) | **Neu.** Zählerstand (Quadratur-Schritte, nicht kWh → k-Änderung rechnet um) als Blob `cnt` {version, fwd, bwd} im NVS-Namespace `ferrcnt`. In `native` per `lib_ignore`. |
| `ferraris_hwcheck` | pure C | Selbsttest-Klassifikation: `ferraris_hw_classify` (GPIO32/33 mit Pulls; floating ⇒ diff > 3800, Last ⇒ diff < 1500), `ferraris_hw_classify_noise` (GPIO34..39, Spread < 120 = stabil), GPIO→ADC1-Kanal. |
| `ferraris_status` | pure C | **Neu.** Status-Snapshot + JSON-Formatter (gemeinsam für HTTP und MQTT). |
| `ferraris_log` | pure C | **Neu.** `FLOGE/W/I(tag, …)` mit Laufzeit-Level. Ersetzt `ESP_LOGx`, weil der Core `ESP_LOGI/W` weg-kompiliert und der CSV-Modus stummschalten muss. |
| `adc_sampler` | ESP | `adc_continuous_sampler`: DMA für 1..8 ADC1-Kanäle, Callback pro Sample-Set, selbstheilend bei Verlusten, Snapshot-Ring. Reader auf Core 1 gepinnt, DMA-Pool 16 Frames (~20 ms) gegen WLAN-Latenzen. |
| `ferraris_logger` | ESP | CSV-Zeile formatieren (`format_row`, auch für HTTP), Live-Werte je Sensor (`get_sensor`), `log_task` auf UART0. Tracker laufen immer, der CSV-Strom nur nach `start()`. |
| `ferraris_hwdetect` | ESP | Messung des Selbsttests (Pulls umschalten, Oneshot-ADC). Blockiert den Boot nie. |
| `ferraris_settings` | pure C | **Neu.** Editierbare Einstellungen: `set_field` (Formularfeld → Wert, leeres Passwort = behalten), `validate` (Bereiche, A≠B, HIGH>LOW, Hostname, MQTT-URI/Präfix, Passwort 8..63), `needs_restart`, JSON ohne Passwörter. |
| `ferraris_settings_store` | ESP (C++) | **Neu.** NVS-Laden/-Speichern (`Preferences`, Namespace `ferrcfg`). In `native` per `lib_ignore`. |
| `ferraris_calib` | pure C | **Neu.** Automatisches Einmessen: Rekorder (Bucket-**Maximum** je Sensor, Hotpath-tauglich; voller Puffer → je 2 Buckets zusammenfassen, Breite ×2) und Auswertung (Median, Spitze ≥ 2 Buckets, Ruhe-Oberkante = 99 %-Wert unter der Mitte, LOW/HIGH = Ruhe + 30/60 % Ausschlag; Paarwahl per nachgespielter Quadratur: keine ungültigen/Gegenschritte, ≥ 4 Schritte, stärkster Ausschlag gewinnt; A/B so, dass die Aufzeichnung als Bezug zählt). |
| `ferraris_health` | pure C | **Neu.** OTA-Bestätigung (`ferraris_ota_verify_step`: WAIT/VALID/ROLLBACK), Absturzserie (`ferraris_crash_streak_next`, `ferraris_safe_mode`). |
| `ferraris_sysmon` | ESP (C++) | **Neu.** Resetgrund, Startzähler (NVS `ferrhlth`), abgesicherter Modus, Loop-Watchdog, OTA-Rollback (`verifyRollbackLater`-Override), Heap, Coredump. In `native` per `lib_ignore`. |
| `ferraris_netutil` | pure C | **Neu.** Prüfung der WLAN-Zugangsdaten (SSID 1..32, Passwort leer oder 8..63), JSON-String-Escaping für untrusted SSIDs. |
| `ferraris_net` | ESP (C++) | WLAN-Zustandsautomat (STA + Einrichtungs-AP, eigenes Wiederverbinden statt Arduino-Auto-Reconnect), Captive Portal (`DNSServer`), `WebServer`, NVS (`Preferences`), Leistungsverlauf 60 min, mDNS, esp-mqtt. Seiten in `ferraris_web_pages.h`. |

## WLAN und Webseiten

Ablauf (`wifi_step()` im loop-Task):
1. Zugangsdaten aus NVS (sonst `secrets.h`), Verbindung versuchen.
2. Keine SSID oder nach 20 s keine Verbindung → **Access Point** `Ferraris-<MAC4>` (WPA2) +
   Captive Portal (alle DNS-Anfragen → 192.168.4.1, fremde Hosts → Redirect auf `/wifi`).
3. Im AP-Modus alle 2 min neuer Verbindungsversuch — **nur wenn kein Gerät am AP hängt**, weil ein
   Versuch den Funkkanal wechselt und AP-Clients trennt. Nach erfolglosem Versuch wird die STA
   per `disconnect()` ruhiggestellt.
4. Verbunden → AP bleibt noch 60 s an (Einrichtungsseite zeigt neue IP), geht aus, sobald niemand
   mehr verbunden ist.
5. Verbindung im Betrieb verloren → 60 s lang neu verbinden, dann zusätzlich AP.

Endpunkte (`*` = aus dem Heimnetz mit HTTP-Basic-Auth `admin` / AP-Passwort, über den AP frei):

- `GET /` — Dashboard: Momentanleistung, Bezug/Einspeisung/Netto/Umdrehungen, Leistungsverlauf
  60 min (SVG, Fadenkreuz-Tooltip), Lichtschranken (Hüllenband min..max, Rohwert, Schwellen,
  Selbsttest-Status), WLAN-Status. Aktualisierung alle 2 s (Verlauf alle 10 s).
- `GET /wifi` `*` — WLAN-Einrichtung: Scan-Liste, SSID/Passwort, Speichern + Verbinden,
  Live-Status. Leeres Passwort bei gleicher SSID = gespeichertes behalten.
- `GET /api/status` — JSON, Format siehe `lib/ferraris_status/src/ferraris_status.h`.
- `GET /api/history` — `{"interval_s":10,"w":[…]}`, älteste zuerst, max. 360 Werte.
- `GET /api/csv` — aktuelle CSV-Zeile (gleiches Format wie UART0).
- `GET /api/scan` `*` — asynchroner Scan; `{"scanning":true}` bis das Ergebnis da ist.
- `POST /api/wifi` `*` — Formularfelder `ssid`, `password`; speichert im NVS und verbindet neu.
- `GET /config` `*` — Einstellungen (Abschnitte mit Badge „sofort“ / „nach Neustart“), Live-Vorschau
  der Schwellen je Eingang über seinem Rohwert, Neustart-Banner, Karte „Zählerstand“ (Werte,
  letzte Sicherung, „Zähler zurücksetzen“).
- `GET /api/config` `*` — `{"settings":{…},"defaults":{…},"restart_pending":bool,"autotune":bool,
  "max_sensors":8,"min_sensors":2}`; `settings.gpio` = alle 8 Pins, aktiv sind die ersten
  `sensor_count`. Passwörter nur als `mqtt_password_set`/`ap_password_set`.
- `POST /api/config` `*` `**` — Formularfelder wie im JSON (`gpio1`..`gpio8`, `mqtt_interval_s` in s,
  `ap_password2` = Wiederholung); Fehler `{"ok":false,"error":…,"field":…}`.
- `POST /api/restart` `*` `**` — MQTT `offline`, Neustart nach ~0,5 s (über `ferraris_sysmon_restart()`,
  sichert vorher den Zählerstand).
- `POST /api/counter/reset` `*` `**` — Zählerstand auf 0 und sofort speichern (Karte „Zählerstand“
  auf `/config`, mit Rückfrage). Status-JSON `meter.saved_s` = Sekunden, seit der gesicherte Stand
  zuletzt aktuell war (−1 = noch nie); `/api/config` liefert `counter_persist`.
- `GET /api/coredump` `*` — Absturzbericht roh (`esp-coredump info_corefile --core-format raw
  --core ferraris-coredump.bin .pio/build/esp32/firmware.elf`); `POST /api/coredump/delete` `*` `**`.
- Status-JSON `health`: `reset`, `boots`, `crashes`, `safe_mode`, `heap_free/min/block`,
  `wifi_reconnects`, `mqtt`, `coredump`, `ota` (`ok`/`pruefung`/`zurueckgenommen`); `adc.errors/timeouts`.
  Dashboard-Karte „System“.
- `GET /api/calib` `*` — Einmessung `{"state":"idle|running|done","elapsed_s","duration_s","reverse",
  "bucket_ms","result":{ok,message,count_a,count_b,steps,revolutions,period_s,sensors[…]}|null}`;
  `POST /api/calib/start` `*` `**` (`duration_s` 30..3600, `reverse=1` bei Einspeisung),
  `POST /api/calib/stop` `*` `**` (`mode=finish` jetzt auswerten | `cancel`). Übernehmen macht die
  Seite über das normale Formular (`/api/config`), nichts wird automatisch gespeichert.
- `POST /api/ota` `*` `**` — multipart, Feld `firmware` = `.pio/build/esp32/firmware.bin`
  (nicht `firmware.factory.bin`, erstes Byte muss 0xE9 sein); danach Neustart.
  `GET /api/config` liefert dazu `firmware.version/partition/ota_max`.
- `**` = zusätzlich Header `X-Ferraris: 1` (CSRF-Schutz: fremde Seiten können ihn nicht ohne
  CORS-Preflight setzen). `/api/wifi` hat diesen Schutz noch nicht.
- **Vorschau ohne Hardware:** `python3 tools/web_preview.py` → http://localhost:8080/ (liest die
  Seiten direkt aus dem Header, simulierte Werte).
- mDNS: `http://<FERRARIS_HOSTNAME>.local/`.
- MQTT (wenn `FERRARIS_MQTT_URI` gesetzt): `<prefix>/state` = Status-JSON alle
  `FERRARIS_MQTT_INTERVAL_MS`; `<prefix>/status` = `online`/`offline` (retained, Last Will).
- Achtung: aktives WLAN erhöht das ADC-Rauschen auf dem ESP32. Die Schwellen (Rauschboden bis ~3397)
  wurden **ohne** WLAN ermittelt — mit WLAN per CSV-Capture nachprüfen.

## Kalibrierung der Schwellen (Erfahrungswerte!)

- **Automatisch einmessen** (`/config`, `ferraris_calib`): zeichnet alle aktiven Sensoren auf
  (32-KB-Puffer nur während der Messung, Start-Auflösung ~5 ms), wertet im loop()-Task aus und
  schlägt Zählpaar + Schwellen vor. Feldtest 2026-10-16 (3 Sensoren, 8 min, ~79 s/Umdrehung):
  Ergebnis A = Q3, B = Q1, Q3 1761/1432, Q1 3139/3029 — deckt sich mit der Handauswertung.
  Grenze: Ist die Marke kürzer als ~2 Buckets (schnelle Scheibe + lange Messung), sind die
  Paare „ungültig“ → kürzer oder bei weniger Last messen.
- Montage-Erkenntnis: die Quadratur braucht **Überlappung** (Marke kurz unter beiden Sensoren).
  Abstand ≈ eine Markenbreite führte zu Schrittfolge +1 −1 −1 +1 (netto 0, Leistung 0 W).

- **Je Kanal einstellbar** (`/config`, Formularfelder `high1..8`/`low1..8`, Status `sensors[].high/low`).
  Regel: **Ruhepegel < LOW < HIGH < Marke**. Feldbefund 2026-10-16 (2 Sensoren, WLAN an): Ruhe
  A ≈ 2470–2515, B ≈ 2420–2475, Marke ≥ 3000; mit HIGH 2800 / LOW 2000 blieben beide Kanäle
  dauerhaft im Zustand „Marke“ (Ruhe nie < LOW) → keine Zählung. Ohne WLAN lag die Ruhe
  ~500–600 tiefer (A ≈ 1700–1970) → Schwellen mit WLAN einstellen.
- Default: **HIGH = 3450, LOW = 3300** (Hysterese 150) für alle Kanäle, aus 1-h-Capture:
  Rauschboden bis ~3397, Markenausschlag 3664–3700, Ruhe-Basis ~2600–2800. Eine zu niedrige
  HIGH-Schwelle (3250) ließ Rauschen durch beide Schwellen oszillieren → Fehltrigger und Unterzählung.
- **Auto-Tune ist standardmäßig AUS**: bei geringer Last (z. B. 1 Umdr./562 s) sieht ein 30-s-Fenster
  nur die Ruhe-Basis (unimodal) → Auto-Tune scheitert.
- Wenn an: Start-Auto-Tune (2–30 s), **jeder Kanal bekommt seine eigenen Schwellen** (ohne zwei
  Populationen behält der Kanal seine Werte); Re-Tune alle 10 min aus dem Snapshot-Ring (gegen
  LED-Alterung, Staub, Temperatur). Auto-Tune-Werte werden nicht ins NVS geschrieben.

## CSV-Log-Format (UART0, `FERRARIS_CSV_LOG_ENABLE`)

Keine Kopfzeile, eine Zeile alle `FERRARIS_CSV_LOG_INTERVAL_MS` (250 ms):

```
millis, la_raw, la_filt, la_min, la_max, la_tOn, la_tOff, la_state, la_count,
        ra_raw, ra_filt, ra_min, ra_max, ra_tOn, ra_tOff, ra_state, ra_count,
W, [Q1_raw, Q1_min, Q1_max, …, Qn_raw, Qn_min, Qn_max]
```

- Spalten 0–16 = RML-17-Spalten-Format, 17 = mittlere Leistung W seit Logger-Start, ab 18 je
  Q-Sensor raw/min/max.
- min/max-Spalten: mit `FERRARIS_ENVELOPE_AUTO=1` mitlaufende Hülle; **Default 0** = feste Werte
  `FERRARIS_ENVELOPE_FIXED_MIN/MAX` (0/4095), gilt auch für Webseite/JSON. Die Logik rechnet
  intern weiter (Simulator unverändert), nur die Ausgabe wird umgeschaltet.
- Polarität: `la_state = !mark` (RML-Konvention: 0 = Marke, 1 = Verweilzeit).
- Netzwerk-Meldungen (WLAN verbunden mit IP, Access Point aktiv) erscheinen immer, auch im
  CSV-Modus, als Kommentarzeile `# …` — der Simulator überspringt `#`-Zeilen. Danach pausiert
  der CSV-Strom `FERRARIS_CONSOLE_PAUSE_MS` (10 s), damit die Meldung lesbar bleibt; die Zählung
  läuft weiter (in der Pause fehlen nur Log-Zeilen, `millis` springt danach).
- Ohne CSV-Log gibt `print_task` `Zaehler: Verbrauch=… Einspeisung=… Netto=… … kWh … W` aus.

## Tests und PC-Simulator

- `test/test_logic/test_main.c` (Unity, `pio test -e native`): Quadratur, Schmitt, Auto-Tune,
  Collector, Logger-Logik, Leistung (inkl. Timer-Überlauf), Zählerdaten, HW-Klassifikation, JSON,
  WLAN-Zugangsdaten-Prüfung, JSON-Escaping, Einstellungen (Parser, Prüfung, Neustart-Felder, JSON).
  Die Env `native` ignoriert die ESP-Bibliotheken per `lib_ignore` — neue ESP-Libs dort eintragen.
- `tools/sim/ferraris_sim.c`: Standalone-Simulator, nutzt dieselben pure-C-Dateien:

```sh
cd tools/sim
gcc -Wall -Wextra -I../../lib/schmitt_logic/src -I../../lib/ferraris_logger_logic/src \
    -o ferraris_sim ferraris_sim.c ../../lib/schmitt_logic/src/schmitt_trigger_autotune.c \
    ../../lib/ferraris_logger_logic/src/ferraris_logger_logic.c -lm
./ferraris_sim synth_447.csv --use-fw-states --revs 150     # -> 8 Schritte = 2 Umdrehungen
```

- Liest Synthese-CSV (`sample,a,b`) oder echte RML-Captures (`--col-a 1 --col-b 9`).
  Optionen: `--autotune N`, `--high/--low`, per Kanal `--high-a/…`, `--invert`, `--fw-thresholds`,
  `--use-fw-states`, `--production`, `--revs N`, `--generate` (20-kHz-Signalgenerator), `--write-csv`.
- Erkenntnis: Das 4-Hz-Log ist aliasiert; die Quadratur braucht echte Abtastraten im kHz-Bereich
  (Marke ~1,7 ms Verweilzeit bei Volllast 63 A, ~8 U/s).

## Konventionen für neuen Code

- Sprache: C für Logik und Treiber, C++ nur für Arduino-APIs (`src/main.cpp`, `ferraris_net`).
  Kommentare/Logs Deutsch, ASCII-Umschreibung (`ae`, `oe`, `ue`) in Logs und den meisten Kommentaren.
- Logik, die auf dem PC testbar sein soll, in eigene pure-C-Bibliotheken ohne ESP-Includes; Tests
  in `test/test_logic` ergänzen. Firmware und Simulator nutzen dieselbe Datei.
- Hotpath (DMA-Callback, ~10 kHz): kein I/O, keine Mutexe, keine Allokation, kein Logging;
  Last-Good-Value über 32-bit-atomare Felder (Single-Writer/Single-Reader).
- Netzwerk nur im `loop()`-Task bzw. esp-mqtt-Task, nie im ADC-Pfad. NVS-Schreiben der
  WLAN-Daten nur dort (selten, auf Benutzeraktion).
- Webseiten: keine externen Ressourcen (AP hat kein Internet), untrusted Strings nur per
  `textContent`, Farben als CSS-Tokens mit Dark-Mode-Variante.
- Flash-Writes (NVS) nie im ADC-Pfad — über Notify an einen Writer-Task niedriger Priorität.
- Hardware-/Netzwerkfehler nicht den Boot blockieren lassen — laut warnen, weiterzählen.
- Features per Makro in `ferraris_config.h` schaltbar, Defaults dort mit *warum* dokumentieren.
- Task-Prioritäten: adc_reader 5 (Core 1), print/retune 3, loop_wdt 2, log 1, Arduino-loop 1.

## Offene Punkte

- Auf echter Hardware prüfen: DMA-Sampling + WLAN gleichzeitig (Sample-Verluste, ADC-Rauschen →
  Schwellen), HTTP/MQTT im Dauerbetrieb.
- Zählerstand: Bei Stromausfall/Absturz fehlen bis zu `FERRARIS_COUNTER_SAVE_INTERVAL_MS` (5 min).
  Optional: Stand auf den echten Zählerstand setzen (Eingabefeld statt nur Reset).
- Sicherheit: HTTP ohne TLS, Basic-Auth im Klartext im Heimnetz; Admin-Passwort = AP-Passwort
  (ändern über `/config` oder Default per `-DFERRARIS_AP_PASSWORD=…`). Passwort vergessen → nur
  Flash löschen hilft (kein Reset-Taster-Pfad). MQTT-Passwort liegt im NVS im Klartext.
- BAScloud-Anbindung konkretisieren (Broker/Topics oder HTTP-API); aktuell generisches MQTT-JSON.
- `ferraris_autocal`.
- Warnung „Schranke hängt“ (Kanal bleibt lange im Zustand Marke, Ruhepegel über LOW).
- Rollback, Loop-Watchdog und abgesicherter Modus auf echter Hardware auslösen und prüfen
  (z. B. Test-Firmware ohne WLAN-Verbindung bzw. mit absichtlichem Absturz).
