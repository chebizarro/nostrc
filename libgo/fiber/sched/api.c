#include "../include/libgo/fiber.h"
#include "sched.h"
#include <errno.h>
#include <time.h>
#include <stdio.h>

static int gof_once_inited = 0;

void gof_init(size_t default_stack_bytes) {
  if (!gof_once_inited) {
    gof_sched_init(default_stack_bytes);
    gof_once_inited = 1;
  }
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

/* timers provided via timer_bridge */
void __gof_sleep_ns(uint64_t ns);

void gof_sleep_ms(uint64_t ms) {
  __gof_sleep_ns(ms * 1000000ull);
}

/* IO shims declared in io.c */
ssize_t gof_read(int fd, void *buf, size_t n);
ssize_t gof_write(int fd, const void *buf, size_t n);
int     gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);
int     gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);

/* Debug/introspection hooks provided in debug/ */
void gof_set_name(const char *name);
size_t gof_list(gof_info *out, size_t max);
void gof_dump_stacks(int fd);

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
  if (gof_once_inited) {
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
