#include <stdatomic.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include "nostr-init.h"
#include "nostr/metrics.h"

static atomic_int g_init_refcnt = 0;
#if NOSTR_ENABLE_METRICS
static atomic_int g_metrics_running = 0;
static pthread_t g_metrics_thread;
static int g_metrics_thread_started = 0;
static int g_metrics_interval_ms = 5000;
static int g_metrics_dump_on_exit = 0;

static void *nostr_metrics_thread_main(void *arg)
{
  (void)arg;
  while (atomic_load(&g_metrics_running)) {
    struct timespec ts;
    int ms = g_metrics_interval_ms > 0 ? g_metrics_interval_ms : 5000;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
    if (!atomic_load(&g_metrics_running)) break;
    nostr_metrics_dump();
  }
  return NULL;
}
#endif

int nostr_global_init(void)
{
  int expected;
  // Fast path: if already initialized, just increment
  expected = atomic_load(&g_init_refcnt);
  while (expected > 0) {
    if (atomic_compare_exchange_weak(&g_init_refcnt, &expected, expected + 1))
      return 0;
  }

  // We need to transition 0 -> 1 and perform one-time inits
  expected = 0;
  if (!atomic_compare_exchange_strong(&g_init_refcnt, &expected, 1)) {
    // Lost race; someone else initialized. Increment again for us.
    atomic_fetch_add(&g_init_refcnt, 1);
    return 0;
  }

  /* External crypto integration hooks previously used here have been removed. */

#if NOSTR_ENABLE_METRICS
  const char *dump_env = getenv("NOSTR_METRICS_DUMP");
  if (dump_env && *dump_env && strcmp(dump_env, "0") != 0) {
    const char *ival = getenv("NOSTR_METRICS_INTERVAL_MS");
    if (ival && *ival) {
      int ms = atoi(ival);
      if (ms > 0) g_metrics_interval_ms = ms;
    }
    atomic_store(&g_metrics_running, 1);
    if (pthread_create(&g_metrics_thread, NULL, nostr_metrics_thread_main, NULL) == 0) {
      g_metrics_thread_started = 1;
      fprintf(stderr, "[metrics] periodic dump enabled every %d ms\n", g_metrics_interval_ms);
    } else {
      atomic_store(&g_metrics_running, 0);
    }
  }
  const char *dump_on_exit = getenv("NOSTR_METRICS_DUMP_ON_EXIT");
  if (dump_on_exit && *dump_on_exit && strcmp(dump_on_exit, "0") != 0) {
    g_metrics_dump_on_exit = 1;
    fprintf(stderr, "[metrics] dump on exit enabled\n");
  }
#endif

  return 0;
}

void nostr_global_cleanup(void)
{
  int prev = atomic_load(&g_init_refcnt);
  if (prev <= 0)
    return; // not initialized or already cleaned
  prev = atomic_fetch_sub(&g_init_refcnt, 1);
  if (prev == 1) {
    /* External crypto integration hooks previously used here have been removed. */
#if NOSTR_ENABLE_METRICS
    if (g_metrics_thread_started) {
      atomic_store(&g_metrics_running, 0);
      pthread_join(g_metrics_thread, NULL);
      g_metrics_thread_started = 0;
    }
    if (g_metrics_dump_on_exit) {
      nostr_metrics_dump();
    }
#endif
  }
}

#ifndef NOSTR_DISABLE_AUTO_INIT
__attribute__((constructor)) static void __nostr_ctor(void)
{
  (void)nostr_global_init();
}

__attribute__((destructor)) static void __nostr_dtor(void)
{
  nostr_global_cleanup();
}
#endif
