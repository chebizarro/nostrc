#include "nostr_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
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
