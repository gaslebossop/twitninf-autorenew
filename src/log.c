#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static arw_log_level g_level = ARW_LOG_INFO;

void arw_log_set_level(arw_log_level level)
{
    g_level = level;
}

static const char *level_name(arw_log_level level)
{
    switch (level) {
    case ARW_LOG_DEBUG: return "DEBUG";
    case ARW_LOG_INFO:  return "INFO ";
    case ARW_LOG_WARN:  return "WARN ";
    case ARW_LOG_ERROR: return "ERROR";
    }
    return "?????";
}

void arw_log(arw_log_level level, const char *fmt, ...)
{
    if (level < g_level) {
        return;
    }

    /* Horodatage UTC : le démon tourne sous systemd, dont le journal est
     * lui-même en UTC. Mélanger les deux fuseaux rend une enquête sur un
     * prélèvement pénible pour rien. */
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    FILE *out = (level >= ARW_LOG_WARN) ? stderr : stdout;
    fprintf(out, "%s [%s] ", stamp, level_name(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
    fflush(out);
}
