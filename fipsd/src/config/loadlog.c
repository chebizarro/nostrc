#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <stdlib.h>
#include <string.h>

bool cfg_error(fipsd_config_log *log, const char *fmt, ...)
{
    if (log && !log->error[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(log->error, sizeof(log->error), fmt, ap);
        va_end(ap);
    }
    return false;
}
void cfg_log(fipsd_config_log *log, fipsd_log_level level, const char *fmt, ...)
{
    if (!log) return;
    char message[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    struct fipsd_config_log_entry *p = realloc(log->entries, (log->count + 1) * sizeof(*p));
    if (!p) { cfg_error(log, "out of memory holding config load log"); return; }
    log->entries = p;
    char *copy = strdup(message);
    if (!copy) { cfg_error(log, "out of memory holding config load log"); return; }
    p[log->count++] = (struct fipsd_config_log_entry){level, copy};
}
void fipsd_config_log_clear(fipsd_config_log *log)
{
    if (!log) return;
    for (size_t i = 0; i < log->count; ++i) free(log->entries[i].message);
    free(log->entries);
    memset(log, 0, sizeof(*log));
}
void fipsd_config_log_replay(const fipsd_config_log *held, fipsd_log *logger)
{
    if (!held) return;
    for (size_t i = 0; i < held->count; ++i)
        fipsd_log_write(logger, held->entries[i].level, "config", "%s", held->entries[i].message);
}
const char *fipsd_config_log_message(const fipsd_config_log *log, size_t i)
{ return log && i < log->count ? log->entries[i].message : NULL; }
fipsd_log_level fipsd_config_log_level(const fipsd_config_log *log, size_t i)
{ return log && i < log->count ? log->entries[i].level : FIPSD_LOG_ERROR; }
