#!/usr/bin/env python3
"""Vorschau der eingebetteten Webseiten ohne ESP32.

Liest die Seiten aus lib/ferraris_net/src/ferraris_web_pages.h und beantwortet
die API mit simulierten Werten (Leistung schwankt um ~450 W).

    python3 tools/web_preview.py          # -> http://localhost:8080/
    python3 tools/web_preview.py 9000     # anderer Port
"""

import json
import math
import random
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

HEADER = Path(__file__).resolve().parent.parent / "lib/ferraris_net/src/ferraris_web_pages.h"
START = time.time()
START0 = START   # Zaehlerstand ueberlebt simulierte Neustarts
WIFI = {"ssid": "Heimnetz", "connected": True}
COREDUMP = {"present": True}
# Einmessung: laeuft in der Vorschau 8 s, Ergebnis = Auswertung einer echten Aufzeichnung (3 Sensoren)
CALIB = {"state": "idle", "start": 0.0, "duration_s": 300, "reverse": False}
CALIB_RESULT = json.loads('{"ok": true, "message": "Zaehlpaar und Schwellen gefunden.", "rows": 2880, "bucket_ms": 166.7, "count_a": 3, "count_b": 1, "steps": 16, "revolutions": 4.0, "period_s": 78.7, "pairs_checked": 3, "pairs_valid": 1, "sensors": [{"usable": true, "base": 2672, "rest_hi": 2919, "peak": 3286, "gap": 367, "high": 3139, "low": 3029, "passes": 4}, {"usable": true, "base": 3120, "rest_hi": 3287, "peak": 3530, "gap": 243, "high": 3432, "low": 3359, "passes": 4}, {"usable": true, "base": 400, "rest_hi": 1103, "peak": 2201, "gap": 1098, "high": 1761, "low": 1432, "passes": 4}]}')


def calib_status():
    c = dict(CALIB)
    if c["state"] == "running" and time.time() - c["start"] >= 8:
        CALIB["state"] = c["state"] = "done"
    el = min(c["duration_s"], int((time.time() - c["start"]) / 8 * c["duration_s"]))
    out = {"state": c["state"], "elapsed_s": el, "duration_s": c["duration_s"], "reverse": c["reverse"],
           "bucket_ms": 5.0 * (1 + el // 30), "result": None}
    if c["state"] == "done":
        r = dict(CALIB_RESULT)
        if c["reverse"]:
            r["count_a"], r["count_b"] = r["count_b"], r["count_a"]
        out["result"] = r
    return out
DEFAULTS = {"sensor_count": 5, "gpio": [36, 39, 34, 35, 32, 33, 37, 38], "high": [3450] * 8, "low": [3300] * 8, "k": 150, "count_a": 1, "count_b": 2,
            "envelope_auto": False, "hostname": "ferraris", "mqtt_uri": "", "mqtt_user": "",
            "mqtt_password_set": False, "mqtt_prefix": "ferraris", "mqtt_interval_s": 10,
            "ap_password_set": True}
SETTINGS = dict(DEFAULTS)
BOOT = dict(DEFAULTS)
RESTART_FIELDS = ("sensor_count", "gpio", "count_a", "count_b", "hostname", "mqtt_uri", "mqtt_user", "mqtt_prefix", "mqtt_interval_s")


def load_pages():
    src = HEADER.read_text(encoding="utf-8")
    css = re.search(r'#define FERRARIS_WEB_CSS R"CSS\((.*?)\)CSS"', src, re.S).group(1)
    pages = {}
    for name, path in (("FERRARIS_PAGE_INDEX", "/"), ("FERRARIS_PAGE_WIFI", "/wifi"),
                       ("FERRARIS_PAGE_CONFIG", "/config")):
        body = re.search(name + r'\[\] = (.*?\)HTML");', src, re.S).group(1)
        html = ""
        for m in re.finditer(r'R"HTML\((.*?)\)HTML"|FERRARIS_WEB_CSS', body, re.S):
            html += m.group(1) if m.group(1) is not None else css
        pages[path] = html
    return pages


def power(t):
    return 450 + 250 * math.sin(t / 300) + random.uniform(-20, 20)


COUNTER = {"base": 0}   # Zuruecksetzen: Schritte bis dahin abziehen


def status():
    up = int(time.time() - START) + 7200
    steps = 640 + int(time.time() - START0) // 30 - COUNTER["base"]
    exp = 0 if COUNTER["base"] else 4
    return {
        "uptime_s": up, "counting": True,
        "meter": {"k": SETTINGS["k"], "steps_import": steps, "steps_export": exp,
                  "import_kwh": steps / 600, "export_kwh": exp / 600,
                  "net_kwh": (steps - exp) / 600, "power_w": round(power(time.time()), 1),
                  "saved_s": int(time.time() - START0) % 300},
        "schmitt": {"high": SETTINGS["high"][BOOT["count_a"] - 1], "low": SETTINGS["low"][BOOT["count_a"] - 1]},
        "count_pair": [f"Q{BOOT['count_a']}", f"Q{BOOT['count_b']}"],
        "sensors": [
            {"name": f"Q{i + 1}", "gpio": g,
             "raw": random.choice([random.randint(2650, 2800)] * 9 + [random.randint(3600, 3700)]),
             "min": 2600 + 20 * i, "max": 3690 - 10 * i, "hw": ["OK", "OK", "OK", "UNSICHER", "OFFEN"][i % 5],
             "high": SETTINGS["high"][i], "low": SETTINGS["low"][i]}
            for i, g in enumerate(BOOT["gpio"][:BOOT["sensor_count"]])
        ],
        "wifi": {"connected": WIFI["connected"], "ssid": WIFI["ssid"], "rssi": -61, "ip": "192.168.1.23",
                 "ap": {"active": True, "ssid": "Ferraris-3A7F", "ip": "192.168.4.1", "clients": 1}},
        "adc": {"sets_per_s": 10000, "restarts": 0, "errors": 0, "timeouts": 3},
        "health": {"reset": "Absturz" if COREDUMP["present"] else "Einschalten", "boots": 14,
                   "crashes": 1 if COREDUMP["present"] else 0, "safe_mode": False,
                   "heap_free": 151234, "heap_min": 118000, "heap_block": 98304, "wifi_reconnects": 2,
                   "mqtt": "aus", "coredump": COREDUMP["present"], "ota": "ok"},
        "fw": "2.0.0", "host": BOOT["hostname"],
    }


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        pages = load_pages()   # bei jedem Aufruf neu -> Aenderungen sofort sichtbar
        if self.path in pages:
            return self._send(200, pages[self.path], "text/html; charset=utf-8")
        if self.path == "/api/status":
            return self._send(200, json.dumps(status()))
        if self.path == "/api/history":
            now = time.time()
            return self._send(200, json.dumps({"interval_s": 10,
                                               "w": [round(power(now - 10 * (299 - i)), 1) for i in range(300)]}))
        if self.path == "/api/scan":
            nets = [("Heimnetz", -52, True), ("Nachbar-5G", -71, True), ('Cafe "Eck"', -80, False),
                    ("FRITZ!Box 7590 XY", -66, True)]
            return self._send(200, json.dumps({"scanning": False, "networks": [
                {"ssid": s, "rssi": r, "secure": sec} for s, r, sec in nets]}))
        if self.path == "/api/config":
            pending = any(SETTINGS[f] != BOOT[f] for f in RESTART_FIELDS) or SETTINGS.get("_pw_changed", False)
            return self._send(200, json.dumps({"settings": {k: v for k, v in SETTINGS.items() if k[0] != "_"},
                                               "defaults": DEFAULTS, "restart_pending": pending,
                                               "autotune": False, "counter_persist": True, "max_sensors": 8, "min_sensors": 2,
                                               "firmware": {"version": "2.1.0", "partition": "app0",
                                                            "ota_max": 0x1E0000, "ota_state": "pruefung",
                                                            "ota_left_s": 200, "safe_mode": False,
                                                            "coredump": {"present": COREDUMP["present"],
                                                                         "size": 21504, "task": "loopTask",
                                                                         "pc": "0x400d2f1c",
                                                                         "reason": "LoadProhibited"}}}))
        if self.path == "/api/coredump":
            if not COREDUMP["present"]:
                return self._send(404, "kein Absturzbericht", "text/plain")
            return self._send(200, b"\x00" * 21504, "application/octet-stream")
        if self.path == "/api/calib":
            return self._send(200, json.dumps(calib_status()))
        if self.path == "/api/csv":
            return self._send(200, "123456,2790,2788,2601,3690,3450,3300,1,12,2700,2702,2580,3650,3450,3300,1,12,455\n",
                              "text/plain")
        self._send(404, "nicht gefunden", "text/plain")

    def post_config(self, form):
        if self.headers.get("X-Ferraris") != "1":
            return self._send(403, json.dumps({"ok": False, "error": "Header X-Ferraris fehlt"}))
        f = {k: v[0] for k, v in form.items()}
        new = dict(SETTINGS)
        try:
            for key in ("k", "sensor_count", "count_a", "count_b",
                        "mqtt_interval_s"):
                if key in f:
                    new[key] = int(f[key])
            gpio = list(new["gpio"])
            for i in range(8):
                key = f"gpio{i + 1}"
                if key in f:
                    gpio[i] = int(f[key])
            new["gpio"] = gpio
            for arr in ("high", "low"):
                vals = list(new[arr])
                for i in range(8):
                    key = f"{arr}{i + 1}"
                    if key in f:
                        vals[i] = int(f[key])
                new[arr] = vals
        except ValueError:
            return self._send(400, json.dumps({"ok": False, "error": "keine ganze Zahl", "field": key}))
        for key in ("hostname", "mqtt_uri", "mqtt_user", "mqtt_prefix"):
            if key in f:
                new[key] = f[key]
        new["envelope_auto"] = f.get("envelope_auto") == "1"
        n = new["sensor_count"]
        active = new["gpio"][:n]
        dup = next((f"gpio{i + 1}" for i in range(n) if active[i] in active[:i]), None)
        bad = next((f"gpio{i + 1}" for i in range(n) if not 32 <= active[i] <= 39), None)
        bad_thr = next((f"high{i + 1}" for i in range(n) if new["high"][i] <= new["low"][i]), None)
        errors = [
            (not 2 <= n <= 8, "sensor_count", "Anzahl Eingaenge muss 2..8 sein"),
            (bad, bad, "Eingang muss ein ADC1-Pin sein (GPIO32..39)"),
            (dup, dup, "GPIO wird schon von einem anderen Eingang benutzt"),
            (not (1 <= new["count_a"] <= n and 1 <= new["count_b"] <= n), "count_a",
             "Sensor A und B muessen aktive Eingaenge sein"),
            (not 1 <= new["k"] <= 10000, "k", "k muss 1..10000 U/kWh sein"),
            (bad_thr, bad_thr, "HIGH-Schwelle muss groesser als LOW sein"),
            (new["count_a"] == new["count_b"], "count_b", "Sensor A und B muessen verschieden sein"),
            (not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,30}[A-Za-z0-9])?", new["hostname"]), "hostname",
             "Hostname: 1..32 Zeichen a-z, 0-9 und '-' (nicht am Anfang/Ende)"),
            (new["mqtt_uri"] and not re.match(r"(mqtts?|wss?)://\S+$", new["mqtt_uri"]), "mqtt_uri",
             "MQTT-URI muss mit mqtt://, mqtts://, ws:// oder wss:// beginnen (ohne Leerzeichen)"),
            (f.get("ap_password") and not 8 <= len(f["ap_password"]) <= 63, "ap_password",
             "Passwort muss 8..63 Zeichen haben"),
        ]
        for bad, field, msg in errors:
            if bad:
                return self._send(400, json.dumps({"ok": False, "error": msg, "field": field}))
        if f.get("mqtt_password"):
            new["mqtt_password_set"] = True
        if f.get("ap_password"):
            new["_pw_changed"] = True
        SETTINGS.update(new)
        pending = any(SETTINGS[k] != BOOT[k] for k in RESTART_FIELDS) or SETTINGS.get("_pw_changed", False)
        self._send(200, json.dumps({"ok": True, "restart_pending": pending}))

    def post_ota(self, body):
        if self.headers.get("X-Ferraris") != "1":
            return self._send(403, json.dumps({"ok": False, "error": "Header X-Ferraris fehlt"}))
        # multipart: Inhalt beginnt nach der ersten Leerzeile des Teils
        start = body.find(b"\r\n\r\n")
        data = body[start + 4:] if start >= 0 else b""
        if not data or data[0] != 0xE9:
            return self._send(400, json.dumps({"ok": False,
                                               "error": "Keine ESP32-App (firmware.bin verwenden, nicht factory.bin)"}))
        time.sleep(1.5)   # Pruefen/Schreiben simulieren
        global START
        START = time.time() + 7200 - 5
        self._send(200, json.dumps({"ok": True, "bytes": len(data)}))

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/api/ota":
            return self.post_ota(raw)
        if self.path == "/api/calib/start":
            f = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            CALIB.update(state="running", start=time.time(), duration_s=int(f.get("duration_s", 300)),
                         reverse=f.get("reverse") == "1")
            return self._send(200, json.dumps({"ok": True}))
        if self.path == "/api/calib/stop":
            f = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            CALIB["state"] = "idle" if f.get("mode") == "cancel" else "done"
            return self._send(200, json.dumps({"ok": True}))
        if self.path == "/api/counter/reset":
            COUNTER["base"] = 640 + int(time.time() - START0) // 30
            return self._send(200, json.dumps({"ok": True}))
        if self.path == "/api/coredump/delete":
            COREDUMP["present"] = False
            return self._send(200, json.dumps({"ok": True}))
        form = parse_qs(raw.decode(), keep_blank_values=True)
        if self.path == "/api/config":
            return self.post_config(form)
        if self.path == "/api/restart":
            global START
            BOOT.update({k: (list(v) if isinstance(v, list) else v) for k, v in SETTINGS.items() if k[0] != "_"})
            SETTINGS.pop("_pw_changed", None)
            START = time.time() + 7200 - 5   # Uptime nach "Neustart" = 5 s
            return self._send(200, json.dumps({"ok": True}))
        if self.path != "/api/wifi":
            return self._send(404, "nicht gefunden", "text/plain")
        ssid = form.get("ssid", [""])[0].strip()
        pw = form.get("password", [""])[0]
        if not ssid:
            return self._send(400, json.dumps({"ok": False, "error": "SSID fehlt"}))
        if 0 < len(pw) < 8:
            return self._send(400, json.dumps({"ok": False, "error": "Passwort muss mindestens 8 Zeichen haben"}))
        WIFI["ssid"] = ssid
        self._send(200, json.dumps({"ok": True}))

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Vorschau: http://localhost:{port}/  /wifi  /config  (Strg+C beendet)")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
