#include "nostr_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef NOSTR_LOG_WINDOW_SECONDS
#define NOSTR_LOG_WINDOW_SECONDS 1
#endif
#ifndef NOSTR_LOG_MAX_PER_WINDOW
#define NOSTR_LOG_MAX_PER_WINDOW 50
#endif

static struct {
  time_t window_start;
  int count;
} g_rl = {0, 0};

/* nostrc-p0ng: Minimum level threshold. Previously the level argument was
 * only used for the prefix string — every message printed regardless of
 * level, so NLOG_DEBUG chatter (websocket pongs, rate-limit drops) always
 * reached stderr. Default is NLOG_INFO; opt into debug with
 * NOSTR_LOG_LEVEL=debug or the existing NOSTR_DEBUG convention. */
static NostrLogLevel min_level(void){
  static int cached = -1; /* benign race: worst case is a duplicate getenv */
  if (cached >= 0) return (NostrLogLevel)cached;
  NostrLogLevel lvl = NLOG_INFO;
  const char *env = getenv("NOSTR_LOG_LEVEL");
  if (env && *env){
    if (!strcasecmp(env, "debug")) lvl = NLOG_DEBUG;
    else if (!strcasecmp(env, "info")) lvl = NLOG_INFO;
    else if (!strcasecmp(env, "warn") || !strcasecmp(env, "warning")) lvl = NLOG_WARN;
    else if (!strcasecmp(env, "error")) lvl = NLOG_ERROR;
  } else if (getenv("NOSTR_DEBUG")){
    lvl = NLOG_DEBUG;
  }
  cached = (int)lvl;
  return lvl;
}

static const char *lvl_str(NostrLogLevel lvl){
  switch(lvl){
    case NLOG_DEBUG: return "DEBUG";
    case NLOG_INFO: return "INFO";
    case NLOG_WARN: return "WARN";
    case NLOG_ERROR: return "ERROR";
    default: return "LOG";
  }
}

void nostr_rl_log(NostrLogLevel lvl, const char *tag, const char *fmt, ...){
  if (lvl < min_level()) return;
  time_t now = time(NULL);
  if (g_rl.window_start == 0) g_rl.window_start = now;
  if (now - g_rl.window_start >= NOSTR_LOG_WINDOW_SECONDS){
    g_rl.window_start = now;
    g_rl.count = 0;
  }
  if (g_rl.count >= NOSTR_LOG_MAX_PER_WINDOW) return;
  g_rl.count++;

  fprintf(stderr, "[%s][%s] ", lvl_str(lvl), tag ? tag : "nostr");
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}
