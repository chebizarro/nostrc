#ifndef NOSTR_LOG_H
#define NOSTR_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NLOG_DEBUG = 0,
  NLOG_INFO  = 1,
  NLOG_WARN  = 2,
  NLOG_ERROR = 3
} NostrLogLevel;

/* Simple global rate-limited logger to avoid log-based DoS.
 * Implementation uses a coarse per-process window. */
void nostr_rl_log(NostrLogLevel lvl, const char *tag, const char *fmt, ...);

/* Secure logging helper: never prints secret values; callers should format
 * messages without embedding sensitive bytes. This alias goes through the
 * same rate limiter. */
#define SECURE_LOG(tag, fmt, ...) nostr_rl_log(NLOG_WARN, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_LOG_H */
