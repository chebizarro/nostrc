#ifndef LIBGO_FIBER_SCHED_SCHED_H
#define LIBGO_FIBER_SCHED_SCHED_H
#include <stddef.h>
#include <stdint.h>
#include "../context/context.h"
#include "../stack/stack.h"
#include "../include/libgo/fiber.h" /* for gof_sched_stats */

typedef enum { GOF_RUNNABLE=0, GOF_BLOCKED=1, GOF_FINISHED=2 } gof_state;

typedef struct gof_fiber {
  uint64_t    id;
  const char *name;
  gof_state   state;
  gof_context ctx;
  gof_stack   stack;
  void      (*entry)(void*);
  void       *arg;
  int         w_affinity; /* preferred worker index for affinity (-1 if none) */
} gof_fiber;

void gof_sched_init(size_t default_stack_bytes);
void gof_sched_enqueue(gof_fiber *f);
void gof_sched_run(void);
void gof_sched_yield(void);

/* New helpers for cooperative blocking primitives */
gof_fiber* gof_sched_current(void);
void       gof_sched_block_current(void);
void       gof_sched_make_runnable(gof_fiber *f);
/* Poller-partition-aware make runnable: called from poller thread when fd ready. */
void       gof_sched_make_runnable_from_poller(gof_fiber *f, int poller_index);

/* Internal: scheduler-driven parking (used by timers/netpoll) */
void       gof_sched_park_until(uint64_t deadline_ns);
void       gof_sched_unpark_ready(void);

/* Return current poller index if called from poller thread, else -1 */
int        gof_sched_current_poller_index(void);

gof_fiber* gof_fiber_create(void (*fn)(void*), void *arg, size_t stack_bytes);

/* Internal: stats snapshot */
void gof_sched_get_stats(gof_sched_stats *out);

/* Internal: runtime configuration of work-stealing */
void gof_sched_set_steal_params(int enable, int min_live, int min_victim);
void gof_sched_get_steal_params(int *enable, int *min_live, int *min_victim);

/* Internal: runtime configuration of periodic rebalancing */
void gof_sched_set_rebalance_params(int enable, int threshold, int interval_ms);
void gof_sched_get_rebalance_params(int *enable, int *threshold, int *interval_ms);

/* Internal: configure number of netpollers. Set function is only effective before gof_sched_init. */
void gof_sched_set_npollers_preinit(int n);
int  gof_sched_get_npollers_value(void);

/* Internal: affinity toggle */
void gof_sched_set_affinity_enabled(int enable);
int  gof_sched_get_affinity_enabled(void);

/* Background mode: externally callable worker loop for pthread_create */
void *gof_worker_main_external(void *arg);

/* Wake all idle workers (used during shutdown / stop request) */
void gof_sched_wake_all(void);

/* Check if background stop has been requested (provided by api.c) */
int gof_bg_stop_requested(void);

#endif /* LIBGO_FIBER_SCHED_SCHED_H */
