# RML 2.0 BAScloud – Ferraris-Zähler-Ausleser

Ein ESP32 liest einen analogen Ferraris-Stromzähler (Drehscheibe mit roter Marke) optisch über
Reflex-Lichtschranken aus. Daraus berechnet er Energie (kWh) und Momentanleistung (W), getrennt
nach **Bezug** und **Einspeisung**. Die Werte erscheinen auf einer Webseite im WLAN, als JSON und
optional per MQTT.

## Zugangsdaten und Passwörter

| Wofür | Benutzer | Passwort (Werkseinstellung) | Ändern |
|---|---|---|---|
| Einrichtungs-WLAN (Access Point) `Ferraris-XXXX` | – | `ferraris-setup` | Webseite `/config` → „Zugang“ |
| Anmeldung an `/wifi` und `/config` aus dem Heimnetz | `admin` | `ferraris-setup` (dasselbe wie oben) | Webseite `/config` → „Zugang“ |
| Heim-WLAN | – | wird bei der Einrichtung eingegeben | Webseite `/wifi` |
| MQTT-Broker | – | leer (kein MQTT) | Webseite `/config` → „Netzwerk und MQTT“ |

- `XXXX` sind die letzten vier Hex-Stellen der MAC-Adresse, z. B. `Ferraris-3A7F`.
- Das Admin-Passwort und das Passwort des Access Points sind **dasselbe**. Über den Access Point
  selbst ist keine Anmeldung nötig, dort schützt schon das WLAN-Passwort.
- Wurde das Passwort über `/config` geändert, gilt die Tabelle nicht mehr. **Neues Passwort
  notieren:** Wer es vergisst, kann den Speicher nur noch per USB löschen (siehe
  [Zurücksetzen](#zurücksetzen)).
- Die Werkseinstellung lässt sich auch beim Bauen ändern: `build_flags = -DFERRARIS_AP_PASSWORD=\"…\"`
  (8..63 Zeichen).
- Die Verbindung läuft über HTTP ohne Verschlüsselung. Die Anmeldung geht im Heimnetz also im
  Klartext über die Leitung. Bitte kein Passwort verwenden, das auch woanders benutzt wird.

## Inbetriebnahme

1. Gerät mit Strom versorgen. Ohne eingerichtetes WLAN startet nach wenigen Sekunden der
   Access Point `Ferraris-XXXX`.
2. Mit Handy oder Laptop in dieses WLAN gehen (Passwort `ferraris-setup`). Die Einrichtungsseite
   öffnet sich meist von selbst, sonst im Browser **http://192.168.4.1/wifi** aufrufen.
3. Heim-WLAN aus der Liste wählen, Passwort eingeben, „Speichern und verbinden“.
4. Die Seite zeigt danach die neue IP-Adresse. Der Zähler ist dann im Heimnetz unter
   **http://ferraris.local/** oder `http://<IP>/` erreichbar. Der Access Point schaltet sich
   etwa eine Minute später ab.
5. Unter **Einstellungen** (`/config`) die Zählerkonstante k vom Typenschild eintragen
   (Angabe „U/kWh“, Werkseinstellung 150) und das Passwort ändern.

Die Konsole (USB, 115200 Baud) zeigt beim Verbinden immer eine Zeile `# WLAN verbunden: … IP …`
bzw. `# Access Point … aktiv`.

## Funktionen

### Messung

- 2 bis 8 Lichtschranken Q1..Qn tasten die Scheibe ab. Werkseinstellung: 5 Eingänge, je rund
  10 000 Mal pro Sekunde (die Abtastrate von 50 000/s teilt sich auf alle Eingänge auf).
- Zwei davon bilden das **Zählpaar A/B**. Aus der Reihenfolge, in der die Marke A und B
  erreicht, erkennt der Zähler die Drehrichtung: Bezug oder Einspeisung. Eine Umdrehung zählt als
  vier Schritte. Störimpulse auf nur einem Sensor heben sich dadurch im Netto-Wert auf.
- Eine Schmitt-Schaltung mit zwei Schwellen je Lichtschranke (HIGH/LOW, Werkseinstellung
  3450/3300) entscheidet, ob gerade die Marke unter dem Sensor ist. Der Abstand der Schwellen
  unterdrückt Rauschen.
- **Energie** = Umdrehungen ÷ k. **Leistung** = aus der Dauer der letzten vollen Umdrehung.
  Ohne Drehung sinkt die Anzeige langsam ab und steht nach einer Stunde auf 0.
- Beim Start prüft ein Selbsttest jeden Sensoreingang (OK / UNSICHER / OFFEN). Das Ergebnis
  steht auf dem Dashboard.
- Eine Überwachung startet die Abtastung neu, falls 3 Sekunden lang keine Messwerte kommen.

> **Hinweis:** Die kWh-Werte zählen ab dem letzten Start. Nach einem Neustart oder Stromausfall
> beginnen sie wieder bei 0. Der Zählerstand wird noch nicht gespeichert.

### Webseiten

| Adresse | Inhalt | Anmeldung |
|---|---|---|
| `/` | Dashboard: Leistung, Bezug/Einspeisung/Netto, Umdrehungen, Verlauf der letzten 60 min, Lichtschranken mit Rohwert und Schwellen, WLAN-Status | nein |
| `/wifi` | WLAN suchen, auswählen und verbinden | ja¹ |
| `/config` | Einstellungen (siehe unten), Neustart, Firmware-Update | ja¹ |

¹ Aus dem Heimnetz mit Benutzer `admin` und dem Admin-Passwort, über den Access Point ohne Anmeldung.

### Einstellungen (`/config`)

| Einstellung | Wirkt | Werkseinstellung |
|---|---|---|
| Zählerkonstante k (U/kWh) | sofort | 150 |
| Schwellen HIGH / LOW **je Eingang** | sofort | 3450 / 3300 |
| min/max automatisch mitlaufen | sofort | aus (fest 0..4095) |
| Anzahl Eingänge | nach Neustart | 5 |
| GPIO je Eingang Q1..Q8 | nach Neustart | 36, 39, 34, 35, 32 (Q6..Q8: 33, 37, 38) |
| Zählpaar Sensor A / B | nach Neustart | Q1 / Q2 |
| Hostname | nach Neustart | `ferraris` |
| MQTT-Broker, Benutzer, Passwort | nach Neustart | leer = kein MQTT |
| MQTT-Topic-Präfix, Intervall | nach Neustart | `ferraris`, 10 s |
| AP-/Admin-Passwort | nach Neustart | `ferraris-setup` |

- Als Eingänge gehen nur die ADC1-Pins **GPIO32..39**, jeder Pin nur einmal. GPIO37/38 sind auf
  den meisten ESP32-Boards nicht herausgeführt. GPIO32/33 haben interne Pull-Widerstände, das
  nutzt der Selbsttest. Das Zählpaar muss unter den aktiven Eingängen liegen.
- **Schwellen richtig setzen:** Die Reihenfolge muss **Ruhepegel < LOW < HIGH < Marke** sein.
  Über HIGH gilt die Marke als erkannt, erst unter LOW wieder als vorbei. Liegt LOW unter dem
  Ruhepegel, bleibt die Lichtschranke im Zustand „Marke“ hängen und **es wird nicht mehr
  gezählt**. Jede Lichtschranke hat eigene Werte, weil Ruhepegel und Ausschlag je Sensor
  verschieden sind.
- Die Seite zeigt je Lichtschranke live den Rohwert (Strich) über den Schwellen aus dem Formular
  (gestrichelt). In Ruhe muss der Strich links von beiden Linien stehen, bei der Marke rechts.
- **Mit WLAN liegt der Ruhepegel höher** (gemessen: etwa +500 bis +600). Schwellen deshalb im
  normalen Betrieb mit WLAN einstellen.
- Wirksam sind die Schwellen von Sensor A und B. Die Werte der anderen Eingänge bleiben
  gespeichert und gelten, sobald der Eingang ins Zählpaar kommt. „Werte von Sensor A für alle
  übernehmen“ kopiert sie auf alle Eingänge.
- **Automatisch einmessen** (oben auf `/config`): Dauer wählen, „Einmessen starten“. Der
  Zähler zeichnet alle Lichtschranken auf (die Zählung läuft weiter) und schlägt danach vor,
  welche zwei Sensoren das Zählpaar bilden und welche Schwellen jeder Sensor bekommt. Die
  Tabelle zeigt je Sensor Ruhepegel, Markenspitze, Ausschlag und die Vorschläge.
  „Vorschlag ins Formular übernehmen“ trägt die Werte ein, gespeichert wird erst mit
  „Speichern“. Ein neues Zählpaar wirkt nach dem Neustart.
  - Die Scheibe muss sich während der Messung **mindestens zweimal** drehen. Bei wenig
    Verbrauch entsprechend lange messen, z. B. 5–10 min.
  - Während der Messung sollte Strom **bezogen** werden. Bei Einspeisung den Haken „Es wird
    gerade eingespeist“ setzen, sonst sind A und B vertauscht.
  - Findet die Automatik kein Paar, nennt sie den Grund, z. B. zu wenige Umdrehungen oder
    Sensoren überlappen sich nicht.
- „Standardwerte eintragen“ füllt das Formular mit den Werkseinstellungen. Gespeichert wird erst
  mit „Speichern“, Passwörter bleiben dabei unverändert.
- Leere Passwortfelder behalten das gespeicherte Passwort.
- Die Einstellungen liegen im Flash des ESP32 und überstehen Neustart, Stromausfall und neues
  Flashen der Firmware.

### Schnittstellen für andere Systeme

| Adresse | Inhalt |
|---|---|
| `GET /api/status` | Alle Werte als JSON (Zähler, Leistung, Sensoren, WLAN, ADC) |
| `GET /api/history` | Leistungsverlauf: 60 min in 10-s-Schritten |
| `GET /api/csv` | Aktuelle Messzeile im CSV-Format der Konsole |
| MQTT `<präfix>/state` | Status-JSON im eingestellten Intervall |
| MQTT `<präfix>/status` | `online` / `offline` (retained, auch bei Verbindungsabbruch) |

### WLAN-Verhalten

- Kommt nach 20 s keine Verbindung zustande, startet der Access Point zur Einrichtung.
- Im Access-Point-Betrieb versucht der Zähler alle 2 Minuten erneut, sich mit dem Heim-WLAN zu
  verbinden, aber nur, wenn gerade kein Gerät am Access Point hängt.
- Geht das WLAN im Betrieb verloren, verbindet er sich 60 s lang neu, danach kommt zusätzlich
  der Access Point.
- Die Zählung läuft in allen Fällen weiter, auch ganz ohne WLAN.

## Firmware-Update (OTA)

Neue Firmware lässt sich über die Webseite einspielen, ohne USB-Kabel:

1. Firmware bauen (`pio run`). Die Datei ist `.pio/build/esp32/firmware.bin`.
   **Nicht** `firmware.factory.bin` nehmen, die ist nur für das erste Flashen per USB.
2. **Einstellungen** (`/config`) öffnen, ganz unten „Firmware-Update“: Datei wählen, „Installieren“.
3. Nach der Übertragung prüft der Zähler die Datei, installiert sie und startet neu. Die Seite
   lädt danach von selbst neu und zeigt die neue Version.

- Einstellungen und WLAN-Daten bleiben erhalten, die **kWh-Zähler beginnen wieder bei 0**.
- Ist die Datei kaputt oder bricht die Übertragung ab, läuft die bisherige Firmware unverändert
  weiter. Der Zähler hat zwei Firmware-Bereiche und schaltet erst nach erfolgreicher Prüfung um.
- **Automatisches Zurückschalten:** Eine neue Firmware muss sich erst bewähren. Sie wird
  bestätigt, sobald die Messung läuft und das Netz erreichbar ist, und zwar ca. 30 s am Stück
  (frühestens 1 min nach dem Start). Stürzt sie vorher ab oder ist sie nach 5 min nicht gesund,
  startet automatisch wieder die vorige Version. `/config` zeigt dann „Das letzte Update wurde
  zurückgenommen“.
- Wichtig: Ist beim Update gerade das Heim-WLAN länger als 5 Minuten weg, wird auch eine
  fehlerfreie Firmware zurückgenommen. Dann das Update einfach wiederholen.
- Per Kommandozeile (Passwort wie bei der Anmeldung):

  ```sh
  curl -u admin:ferraris-setup -H 'X-Ferraris: 1' \
       -F firmware=@.pio/build/esp32/firmware.bin http://ferraris.local/api/ota
  ```

**Voraussetzung: 4 MB Flash.** Geräte, die noch mit der alten Firmware (2-MB-Aufteilung ohne OTA)
laufen, müssen **einmal per USB** geflasht werden (`pio run -t upload`). WLAN-Daten und
Einstellungen bleiben dabei erhalten. Danach gehen Updates über die Webseite.

## Robustheit und Diagnose

| Funktion | Was passiert |
|---|---|
| **System-Karte** (Dashboard) | Grund des letzten Starts, Starts/Abstürze, freier Speicher, WLAN-Abbrüche, MQTT-Status, ADC-Fehler, Firmware-Status, Hinweis auf Absturzbericht. Dieselben Werte stehen im JSON (`health`) und gehen per MQTT raus. |
| **Loop-Watchdog** | Hängen WLAN/Webseite/MQTT länger als 30 s, startet der Zähler neu. Die Messung hat ihre eigene Überwachung (Neustart der Abtastung nach 3 s ohne Messwerte). |
| **Abgesicherter Modus** | Nach 3 Abstürzen hintereinander startet der Zähler mit Werkseinstellungen, denn eine falsche Einstellung (z. B. Eingänge) könnte die Ursache sein. Dashboard und `/config` zeigen einen Hinweis. Die gespeicherten Werte lassen sich auf `/config` korrigieren, danach neu starten. Läuft das Gerät 1 min stabil, endet die Absturzserie. |
| **Absturzbericht** | Nach einem Absturz liegt ein Bericht im Flash. `/config` zeigt Task und Ursache, er lässt sich herunterladen und löschen. Auswerten mit `esp-coredump` und der passenden `firmware.elf`. |
| **Firmware-Rückfall** | Siehe [Firmware-Update](#firmware-update-ota). |

Die Konsole meldet jeden Start als `# Start Nr. 14: Absturz, Absturzbericht vorhanden`.

## Zurücksetzen

Passwort vergessen oder Einstellungen verkorkst: Den Flash-Speicher per USB komplett löschen
und die Firmware neu aufspielen. Danach gelten wieder alle Werkseinstellungen, auch das
WLAN muss neu eingerichtet werden.

```sh
pio run -t erase          # Flash komplett loeschen (WLAN-Daten + Einstellungen)
pio run -t upload         # Firmware neu aufspielen
```

Neu flashen **ohne** Löschen behält WLAN-Daten und Einstellungen.

## Für Entwickler

```sh
pio run                     # bauen
pio run -t upload           # flashen (USB, Auto-Reset)
pio device monitor          # Konsole 115200 Baud
pio test -e native          # Unit-Tests auf dem PC
python3 tools/web_preview.py   # Webseiten ohne Hardware: http://localhost:8080/
```

### Versionen und Releases

Die Firmware-Version kommt automatisch aus Git (`git describe`): auf einem Tag z. B. `2.2.0`,
danach `2.2.0-3-g1a2b3c4`, mit nicht committeten Änderungen zusätzlich `-dirty`. Sie steht im
Dashboard, auf `/config`, im JSON/MQTT und in der Konsole.

Neues Release (Meilenstein):

```sh
# Fallback-Version in src/main.cpp anpassen, committen, dann:
git tag -a V2.3.0 -m "Kurzbeschreibung"
git push origin main V2.3.0
pio run
```

Auf GitHub unter **Releases → Draft a new release** den Tag wählen und aus `.pio/build/esp32/`
anhängen: `firmware.bin` (OTA-Update über `/config`), `firmware.factory.bin` (erstes Flashen
per USB) und `firmware.elf` (nötig, um Absturzberichte dieser Version auszuwerten).
Zurück zu einer alten Version: deren `firmware.bin` aus dem Release per OTA einspielen, oder
`git checkout V2.2.0 && pio run`.

`pio` liegt unter `~/.platformio/penv/bin/pio`. Werkseinstellungen und Schalter stehen in
`include/ferraris_config.h`, Standard-WLAN und -MQTT optional in `include/secrets.h` (Vorlage
`secrets.example.h`, nicht im Git). Architektur, Datenfluss und Konventionen beschreibt
[AGENTS.md](AGENTS.md).
