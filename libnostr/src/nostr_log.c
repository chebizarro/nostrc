#include "nostr_log.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
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
/* nostrc-val0v: g_rl is shared by every relay message_loop thread; guard the
 * whole window-check/update sequence so concurrent callers cannot race the
 * count or the window reset. */
static pthread_mutex_t g_rl_mu = PTHREAD_MUTEX_INITIALIZER;

/* nostrc-p0ng: Minimum level threshold. Previously the level argument was
 * only used for the prefix string — every message printed regardless of
 * level, so NLOG_DEBUG chatter (websocket pongs, rate-limit drops) always
 * reached stderr. Default is NLOG_INFO; opt into debug with
 * NOSTR_LOG_LEVEL=debug or the existing NOSTR_DEBUG convention. */
static NostrLogLevel min_level(void){
  static _Atomic int cached = -1; /* atomic: worst case was a duplicate getenv */
  int c = atomic_load_explicit(&cached, memory_order_acquire);
  if (c >= 0) return (NostrLogLevel)c;
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
  atomic_store_explicit(&cached, (int)lvl, memory_order_release);
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
  pthread_mutex_lock(&g_rl_mu);
  if (g_rl.window_start == 0) g_rl.window_start = now;
  if (now - g_rl.window_start >= NOSTR_LOG_WINDOW_SECONDS){
    g_rl.window_start = now;
    g_rl.count = 0;
  }
  if (g_rl.count >= NOSTR_LOG_MAX_PER_WINDOW) { pthread_mutex_unlock(&g_rl_mu); return; }
  g_rl.count++;
  pthread_mutex_unlock(&g_rl_mu);

  fprintf(stderr, "[%s][%s] ", lvl_str(lvl), tag ? tag : "nostr");
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}
