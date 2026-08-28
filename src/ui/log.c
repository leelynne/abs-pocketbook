#include "ui/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "core/paths.h"

static int enabled = 0;

unsigned long abs_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

void abs_log_init(void)
{
    enabled = (access(ABS_LOG_TRIGGER, F_OK) == 0);
    if (!enabled) return;

    FILE *f = fopen(ABS_LOG_PATH, "a");
    if (f == NULL) {
        enabled = 0;
        return;
    }
    fprintf(f, "\n=== session start ===\n");
    fclose(f);
}

int abs_log_enabled(void)
{
    return enabled;
}

void abs_log(const char *fmt, ...)
{
    if (!enabled) return;

    FILE *f = fopen(ABS_LOG_PATH, "a");
    if (f == NULL) return;

    time_t now = time(NULL);
    struct tm tm_buf;
    if (localtime_r(&now, &tm_buf) != NULL) {
        char stamp[32];
        strftime(stamp, sizeof stamp, "%H:%M:%S", &tm_buf);
        fprintf(f, "[%s] ", stamp);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}
