#include "../include/libgo/fiber.h"
#include "sched.h"
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

/* go.h provides go_register_fiber_spawn; sched.h provides gof_worker_main_external;
   fiber.h provides gof_spawn — no need for bare extern forward declarations. */
#include "../../include/go.h"

static pthread_once_t gof_once_ctrl = PTHREAD_ONCE_INIT;
static size_t gof_init_stack_bytes = 0;

static void gof_do_init(void) {
  gof_sched_init(gof_init_stack_bytes);
  /* Register our spawn function with libgo so go_fiber_compat works */
  go_register_fiber_spawn(gof_spawn);
}

/* ── Background scheduler state ──────────────────────────────────────── */
static pthread_t *bg_worker_threads = NULL;
static int bg_nworkers = 0;
static atomic_int bg_stop_requested = 0;
static atomic_int bg_running = 0;
static pthread_mutex_t bg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Exposed to sched.c worker_main so it can observe the stop flag */
int gof_bg_stop_requested(void) {
    return atomic_load_explicit(&bg_stop_requested, memory_order_acquire);
}

void gof_init(size_t default_stack_bytes) {
  gof_init_stack_bytes = default_stack_bytes;
  pthread_once(&gof_once_ctrl, gof_do_init);
}

gof_fiber_t* gof_spawn(gof_fn fn, void *arg, size_t stack_bytes) {
  gof_init(0);
  gof_fiber *f = gof_fiber_create(fn, arg, stack_bytes);
  if (f) {
    gof_sched_enqueue(f);
  }
  return (gof_fiber_t*)f;
}

void gof_yield(void) {
  gof_sched_yield();
}

void gof_run(void) {
  gof_sched_run();
}

/* __gof_sleep_ns is declared in timer_bridge.h;
   IO shims + introspection are declared in fiber.h (already included). */
#include "../timers/timer_bridge.h"

void gof_sleep_ms(uint64_t ms) {
  __gof_sleep_ns(ms * 1000000ull);
}

void gof_get_stats(gof_sched_stats *out) {
  if (!out) return;
  gof_init(0);
  gof_sched_get_stats(out);
}

void gof_set_steal_params(const gof_steal_params *p) {
  if (!p) return;
  gof_init(0);
  /* clamp and forward to scheduler */
  int enable = p->enable_steal ? 1 : 0;
  int min_live = p->steal_min_live >= 0 ? p->steal_min_live : 0;
  int min_victim = p->steal_min_victim >= 2 ? p->steal_min_victim : 2;
  gof_sched_set_steal_params(enable, min_live, min_victim);
#if defined(GOF_DEBUG)
  fprintf(stderr, "[gof] steal params: enable=%d min_live=%d min_victim=%d\n", enable, min_live, min_victim);
#endif
}

void gof_get_steal_params(gof_steal_params *out) {
  if (!out) return;
  gof_init(0);
  int enable, min_live, min_victim;
  gof_sched_get_steal_params(&enable, &min_live, &min_victim);
  out->enable_steal = enable;
  out->steal_min_live = min_live;
  out->steal_min_victim = min_victim;
}

void gof_set_rebalance_params(const gof_rebalance_params *p) {
  if (!p) return;
  gof_init(0);
  int enable = p->enable ? 1 : 0;
  int threshold = p->threshold >= 1 ? p->threshold : 1;
  int interval_ms = p->interval_ms >= 1 ? p->interval_ms : 1;
  gof_sched_set_rebalance_params(enable, threshold, interval_ms);
#if defined(GOF_DEBUG)
  fprintf(stderr, "[gof] rebalance params: enable=%d threshold=%d interval_ms=%d\n", enable, threshold, interval_ms);
#endif
}

void gof_get_rebalance_params(gof_rebalance_params *out) {
  if (!out) return;
  gof_init(0);
  int enable, threshold, interval_ms;
  gof_sched_get_rebalance_params(&enable, &threshold, &interval_ms);
  out->enable = enable;
  out->threshold = threshold;
  out->interval_ms = interval_ms;
}

void gof_set_affinity_enabled(int enable) {
  gof_init(0);
  gof_sched_set_affinity_enabled(enable ? 1 : 0);
#if defined(GOF_DEBUG)
  fprintf(stderr, "[gof] affinity: enable=%d\n", enable ? 1 : 0);
#endif
}

int gof_get_affinity_enabled(void) {
  gof_init(0);
  return gof_sched_get_affinity_enabled();
}

int gof_set_npollers(int n) {
  if (n < 1) return -1;
  if (gof_sched_is_initialized()) {
    /* must be set before scheduler initialization */
    return -1;
  }
  gof_sched_set_npollers_preinit(n);
  return 0;
}

int gof_get_npollers(void) {
  /* If not initialized yet, returns preset or default value. */
  return gof_sched_get_npollers_value();
}

/* ── Background scheduler API ────────────────────────────────────────── */

int gof_start_background(size_t default_stack_bytes) {
  pthread_mutex_lock(&bg_mutex);
  if (atomic_load_explicit(&bg_running, memory_order_acquire)) {
    pthread_mutex_unlock(&bg_mutex);
    return -1; /* already running */
  }
  gof_init(default_stack_bytes);
  atomic_store_explicit(&bg_stop_requested, 0, memory_order_release);

  /* Get nworkers from the scheduler init */
  gof_sched_stats stats;
  gof_get_stats(&stats);
  bg_nworkers = stats.nworkers;
  if (bg_nworkers < 1) bg_nworkers = 1;

  bg_worker_threads = (pthread_t *)calloc((size_t)bg_nworkers, sizeof(pthread_t));
  if (!bg_worker_threads) {
    pthread_mutex_unlock(&bg_mutex);
    return -1;
  }

  /* Launch ALL workers as background threads (unlike gof_run which uses
   * the calling thread as worker 0). */
  for (int i = 0; i < bg_nworkers; ++i) {
    /* gof_worker_main_external takes a worker index as argument */
    int rc = pthread_create(&bg_worker_threads[i], NULL,
                            gof_worker_main_external,
                            (void *)(intptr_t)i);
    if (rc != 0) {
      fprintf(stderr, "[gof] failed to create background worker %d: %d\n", i, rc);
      /* Clean up any already-created threads */
      atomic_store_explicit(&bg_stop_requested, 1, memory_order_release);
      for (int j = 0; j < i; ++j) {
        pthread_join(bg_worker_threads[j], NULL);
      }
      free(bg_worker_threads);
      bg_worker_threads = NULL;
      bg_nworkers = 0;
      pthread_mutex_unlock(&bg_mutex);
      return -1;
    }
  }

  atomic_store_explicit(&bg_running, 1, memory_order_release);
  pthread_mutex_unlock(&bg_mutex);
#if defined(GOF_DEBUG)
  fprintf(stderr, "[gof] background scheduler started with %d workers\n", bg_nworkers);
#endif
  return 0;
}

void gof_request_stop(void) {
  atomic_store_explicit(&bg_stop_requested, 1, memory_order_release);
  /* Signal the scheduler condition variable to wake any idle workers */
  /* This is done via the scheduler's internal API — we need to expose it.
   * For now, inject a NULL sentinel that workers check. */
  gof_sched_wake_all();
}

int gof_join_background(void) {
  pthread_mutex_lock(&bg_mutex);
  if (!atomic_load_explicit(&bg_running, memory_order_acquire) || !bg_worker_threads) {
    pthread_mutex_unlock(&bg_mutex);
    return -1;
  }
  /* Copy state locally so we can release the mutex before joining */
  pthread_t *threads = bg_worker_threads;
  int nworkers = bg_nworkers;
  bg_worker_threads = NULL;
  bg_nworkers = 0;
  atomic_store_explicit(&bg_running, 0, memory_order_release);
  pthread_mutex_unlock(&bg_mutex);

  /* Join threads outside the mutex to avoid blocking other callers */
  for (int i = 0; i < nworkers; ++i) {
    pthread_join(threads[i], NULL);
  }
  free(threads);
#if defined(GOF_DEBUG)
  fprintf(stderr, "[gof] background scheduler stopped\n");
#endif
  return 0;
}

int gof_in_fiber(void) {
  gof_fiber *f = gof_sched_current();
  return f != NULL ? 1 : 0;
}

gof_fiber_t *gof_current(void) {
  return (gof_fiber_t *)gof_sched_current();
}
