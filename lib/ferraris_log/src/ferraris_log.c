/**
 * @file ferraris_log.c
 * @brief Minimales Log mit Laufzeit-Level (pure C).
 */

#include "ferraris_log.h"
#include <stdarg.h>
#include <stdio.h>

static volatile flog_level_t s_level = FLOG_INFO;

void ferraris_log_set_level(flog_level_t level)
{
    s_level = level;
}

flog_level_t ferraris_log_get_level(void)
{
    return s_level;
}

void ferraris_log_write(flog_level_t level, const char *tag, const char *fmt, ...)
{
    if (level == FLOG_NONE || level > s_level) return;

    static const char letters[] = { '-', 'E', 'W', 'I' };
    char line[256];
    int len = snprintf(line, sizeof(line), "%c %s: ", letters[level], tag ? tag : "?");
    if (len < 0) return;
    if (len < (int)sizeof(line)) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + len, sizeof(line) - (size_t)len, fmt, ap);
        va_end(ap);
    }
    // Ganze Zeile in einem printf -> kein Zerreissen durch andere Tasks.
    printf("%s\n", line);
}
