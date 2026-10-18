/**
 * @file ferraris_netutil.c
 * @brief Hilfen fuer die Web-Schnittstelle (pure C).
 */

#include "ferraris_netutil.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool ferraris_wifi_credentials_valid(const char *ssid, const char *password, const char **why)
{
    const char *msg = NULL;
    size_t sl = ssid ? strlen(ssid) : 0;
    size_t pl = password ? strlen(password) : 0;

    if (sl == 0)                          msg = "SSID fehlt";
    else if (sl > FERRARIS_SSID_MAX)      msg = "SSID laenger als 32 Zeichen";
    else if (pl > 0 && pl < 8)            msg = "Passwort muss mindestens 8 Zeichen haben";
    else if (pl > FERRARIS_PASSWORD_MAX)  msg = "Passwort laenger als 63 Zeichen";

    if (why) *why = msg;
    return msg == NULL;
}

int ferraris_json_string(char *buf, size_t len, const char *s)
{
    if (!buf && len) return -1;
    size_t pos = 0;
#define EMIT(c) do { if (pos + 1 < len) buf[pos] = (c); pos++; } while (0)
    EMIT('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            EMIT('\\');
            EMIT((char)*p);
        } else if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            for (const char *e = esc; *e; e++) EMIT(*e);
        } else {
            EMIT((char)*p);
        }
    }
    EMIT('"');
#undef EMIT
    if (len) buf[pos < len ? pos : len - 1] = '\0';
    return (int)pos;
}


bool ferraris_parse_decimal(const char *s, double *out)
{
    if (!s) return false;
    while (isspace((unsigned char)*s)) s++;
    double v = 0.0, scale = 0.0;   // scale > 0: Nachkommastellen
    int digits = 0;
    bool sep = false;
    for (; *s && !isspace((unsigned char)*s); s++) {
        if (*s >= '0' && *s <= '9') {
            if (sep) { scale /= 10.0; v += (*s - '0') * scale; }
            else     v = v * 10.0 + (*s - '0');
            if (++digits > 15) return false;     // jenseits double-Genauigkeit
        } else if ((*s == ',' || *s == '.') && !sep) {
            sep = true;
            scale = 1.0;
        } else {
            return false;
        }
    }
    while (isspace((unsigned char)*s)) s++;
    if (*s || digits == 0) return false;
    if (out) *out = v;
    return true;
}
