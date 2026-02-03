#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "sched.h"
#include "../debug/debug.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../io/netpoll.h"

/* Forward declaration for poller thread entry */
static void* poller_main(void* arg);

/* Provided by io.c */
int gof_io_have_waiters(void);

/* Debug logging */
#if defined(GOF_DEBUG)
#define LOGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOGF(...) do{}while(0)
#endif

/* Private scheduler state (supports multiple scheduler threads + 1 poller thread) */
typedef struct gof_node { struct gof_node *next; gof_fiber *f; } gof_node;
typedef struct gof_sleep {
  struct gof_sleep *next;
  uint64_t deadline_ns;
  gof_fiber *f;
} gof_sleep;
typedef struct gof_worker {
  pthread_t   tid;
  gof_context sched_ctx;  /* scheduler context for this OS thread */
  gof_fiber  *current;    /* currently running fiber on this worker */
  gof_node   *rq_head;     /* per-worker run queue */
  gof_node   *rq_tail;
  pthread_mutex_t rq_mu;   /* protects run queue for stealing */
  int        running;      /* set to 1 while executing a fiber; protected by rq_mu */
  int        index;        /* worker index in S.workers */
  int        last_victim;  /* rotating start for victim selection */
} gof_worker;
static struct {
  size_t      default_stack;
  /* sleepers list is global and must be accessed under mu */
  gof_sleep  *sleep_head; /* sorted by deadline */
  uint64_t    next_id;
  int         initialized;
  int         nworkers;
  gof_worker *workers;
  pthread_key_t worker_key; /* TLS for current worker */
  pthread_key_t poller_key; /* TLS for current poller (index+1, 0 means not poller) */
  int         enable_steal; /* gated by GOF_WORKSTEAL=1 */
  int         affinity_enable; /* GOF_AFFINITY=1 to route to preferred worker */
  /* Cross-thread inject queue (MPSC) guarded by mutex */
  gof_node   *inj_head;
  gof_node   *inj_tail;
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  /* One or more poller threads */
  pthread_t *poller_threads;
  int        npollers;
  int        poll_partition_enable; /* GOF_POLL_PARTITION=1 to route wakeups to partitioned workers */
  int        bootstrapped; /* only worker0 drains inject until set */
  atomic_int live_fibers;  /* number of fibers not yet finished */
  int        steal_min_live;   /* min live fibers before enabling stealing */
  int        steal_min_victim; /* min victim queue length to allow stealing */
  /* Stats (atomics for lock-free increments) */
  atomic_ullong steals_attempted;
  atomic_ullong steals_success;
  atomic_ullong inject_enqueues;
  atomic_ullong inject_drains;
  /* Rebalancer tunables and stats */
  int        rebalance_enable;         /* 0/1 */
  int        rebalance_threshold;      /* migrate when max_len - min_len >= threshold */
  int        rebalance_interval_ms;    /* how often worker0 attempts rebalancing */
  uint64_t   rebalance_last_ns;        /* last wall time we rebalanced */
  atomic_ullong rebalances_attempted;
  atomic_ullong rebalances_migrated;
} S;

static inline gof_worker* cur_worker(void) {
  return (gof_worker*)pthread_getspecific(S.worker_key);
}

int gof_sched_current_poller_index(void) {
  void *val = pthread_getspecific(S.poller_key);
  if (!val) return -1;
  int idx = (int)((intptr_t)val) - 1;
  return idx;
}

void gof_sched_set_affinity_enabled(int enable) {
  if (!S.initialized) gof_sched_init(0);
  pthread_mutex_lock(&S.mu);
  S.affinity_enable = enable ? 1 : 0;
  pthread_mutex_unlock(&S.mu);
}

int gof_sched_get_affinity_enabled(void) {
  if (!S.initialized) gof_sched_init(0);
  pthread_mutex_lock(&S.mu);
  int v = S.affinity_enable;
  pthread_mutex_unlock(&S.mu);
  return v;
}

void gof_sched_set_rebalance_params(int enable, int threshold, int interval_ms) {
  if (!S.initialized) gof_sched_init(0);
  if (threshold < 1) threshold = 1;
  if (interval_ms < 1) interval_ms = 1;
  pthread_mutex_lock(&S.mu);
  S.rebalance_enable = enable ? 1 : 0;
  S.rebalance_threshold = threshold;
  S.rebalance_interval_ms = interval_ms;
  pthread_mutex_unlock(&S.mu);
}

void gof_sched_get_rebalance_params(int *enable, int *threshold, int *interval_ms) {
  if (!S.initialized) gof_sched_init(0);
  pthread_mutex_lock(&S.mu);
  if (enable) *enable = S.rebalance_enable;
  if (threshold) *threshold = S.rebalance_threshold;
  if (interval_ms) *interval_ms = S.rebalance_interval_ms;
  pthread_mutex_unlock(&S.mu);
}

static int rq_len_trylocked(gof_worker *w) {
  int len = 0;
  for (gof_node *t = w->rq_head; t; t = t->next) ++len;
  return len;
}

static int try_get_rq_len(gof_worker *w, int *out_len) {
  if (pthread_mutex_trylock(&w->rq_mu) != 0) return 0;
  *out_len = rq_len_trylocked(w);
  pthread_mutex_unlock(&w->rq_mu);
  return 1;
}

/* Forward declarations to satisfy maybe_rebalance() */
static uint64_t gof_now_ns(void);
static int rq_steal_one(struct gof_worker *from, struct gof_worker *to);

static void maybe_rebalance(void) {
  if (!S.rebalance_enable || S.nworkers <= 1) return;
  uint64_t now = gof_now_ns();
  uint64_t interval_ns = (uint64_t)S.rebalance_interval_ms * 1000000ull;
  if (interval_ns == 0) interval_ns = 1000000ull; /* 1ms minimum */
  if (now - S.rebalance_last_ns < interval_ns) return;
  S.rebalance_last_ns = now;
  /* Identify busiest and idlest workers using trylock snapshots to avoid contention */
  int max_len = -1, min_len = 1<<30;
  int max_i = -1, min_i = -1;
  for (int i = 0; i < S.nworkers; ++i) {
    int l;
    if (!try_get_rq_len(&S.workers[i], &l)) continue;
    if (l > max_len) { max_len = l; max_i = i; }
    if (l < min_len) { min_len = l; min_i = i; }
  }
  if (max_i < 0 || min_i < 0) return;
  if (max_len - min_len < S.rebalance_threshold) return;
  atomic_fetch_add(&S.rebalances_attempted, 1);
  /* Migrate a single task using the same safe stealing primitive */
  if (rq_steal_one(&S.workers[max_i], &S.workers[min_i])) {
    atomic_fetch_add(&S.rebalances_migrated, 1);
  }
}

void gof_sched_set_steal_params(int enable, int min_live, int min_victim) {
  if (!S.initialized) gof_sched_init(0);
  if (min_live < 0) min_live = 0;
  if (min_victim < 2) min_victim = 2;
  pthread_mutex_lock(&S.mu);
  S.enable_steal = enable ? 1 : 0;
  S.steal_min_live = min_live;
  S.steal_min_victim = min_victim;
  pthread_mutex_unlock(&S.mu);
}

void gof_sched_get_steal_params(int *enable, int *min_live, int *min_victim) {
  if (enable) *enable = 0;
  if (min_live) *min_live = 0;
  if (min_victim) *min_victim = 0;
  if (!S.initialized) gof_sched_init(0);
  pthread_mutex_lock(&S.mu);
  if (enable) *enable = S.enable_steal;
  if (min_live) *min_live = S.steal_min_live;
  if (min_victim) *min_victim = S.steal_min_victim;
  pthread_mutex_unlock(&S.mu);
}

/* Internal: stats snapshot */
void gof_sched_get_stats(gof_sched_stats *out) {
  if (!out) return;
  pthread_mutex_lock(&S.mu);
  out->nworkers = S.nworkers;
  out->enable_steal = S.enable_steal;
  out->affinity_enable = S.affinity_enable;
  out->steal_min_live = S.steal_min_live;
  out->steal_min_victim = S.steal_min_victim;
  out->live_fibers = (uint64_t)atomic_load(&S.live_fibers);
  out->steals_attempted = (uint64_t)atomic_load(&S.steals_attempted);
  out->steals_success = (uint64_t)atomic_load(&S.steals_success);
  out->inject_enqueues = (uint64_t)atomic_load(&S.inject_enqueues);
  out->inject_drains = (uint64_t)atomic_load(&S.inject_drains);
  /* Rebalancer snapshot */
  out->rebalance_enable = S.rebalance_enable;
  out->rebalance_threshold = S.rebalance_threshold;
  out->rebalance_interval_ms = S.rebalance_interval_ms;
  out->rebalances_attempted = (uint64_t)atomic_load(&S.rebalances_attempted);
  out->rebalances_migrated = (uint64_t)atomic_load(&S.rebalances_migrated);
  pthread_mutex_unlock(&S.mu);
}

static void rq_push_to(gof_worker *W, gof_fiber *f) {
  gof_node *n = (gof_node*)malloc(sizeof(*n));
  n->next = NULL; n->f = f;
  pthread_mutex_lock(&W->rq_mu);
  if (!W->rq_tail) { W->rq_head = W->rq_tail = n; }
  else { W->rq_tail->next = n; W->rq_tail = n; }
  pthread_mutex_unlock(&W->rq_mu);
}
static void rq_push(gof_fiber *f) {
  gof_worker *W = cur_worker();
  rq_push_to(W, f);
}
static __attribute__((unused)) gof_fiber* rq_pop(void) {
  gof_worker *W = cur_worker();
  pthread_mutex_lock(&W->rq_mu);
  gof_node *n = W->rq_head; if (!n) { pthread_mutex_unlock(&W->rq_mu); return NULL; }
  W->rq_head = n->next; if (!W->rq_head) W->rq_tail = NULL; pthread_mutex_unlock(&W->rq_mu);
  gof_fiber *f = n->f; free(n); return f;
}

/* Atomically pop a fiber and mark the worker as running under rq_mu.
 * This prevents thieves from stealing between pop and running flag set. */
static gof_fiber* rq_pop_mark_running(gof_worker *W) {
  pthread_mutex_lock(&W->rq_mu);
  gof_node *n = W->rq_head;
  if (!n) { pthread_mutex_unlock(&W->rq_mu); return NULL; }
  W->rq_head = n->next; if (!W->rq_head) W->rq_tail = NULL;
  W->running = 1;
  pthread_mutex_unlock(&W->rq_mu);
  gof_fiber *f = n->f; free(n); return f;
}
static int rq_steal_one(gof_worker *from, gof_worker *to) {
  /* Preserve victim's head (next-to-run) to keep FIFO order deterministic. */
  gof_node *stolen_node = NULL;
  /* Do not attempt stealing when the system has too few live fibers; keeps tiny tests deterministic */
  if (atomic_load(&S.live_fibers) < S.steal_min_live) {
    return 0;
  }
  atomic_fetch_add(&S.steals_attempted, 1);
  if (pthread_mutex_trylock(&from->rq_mu) != 0) {
    /* Contended victim, skip to reduce lock contention */
    return 0;
  }
  /* Do not steal from a worker that is actively running a fiber. */
  if (from->running) {
    pthread_mutex_unlock(&from->rq_mu);
    return 0;
  }
  gof_node *head = from->rq_head;
  /* Only steal if there are at least steal_min_victim items queued to minimize reordering on small queues */
  int need = S.steal_min_victim;
  int have = 0;
  for (gof_node *t = head; t && have < need; t = t->next) ++have;
  if (have >= need) {
    /* Steal the second node */
    stolen_node = head->next;
    head->next = stolen_node->next;
    if (from->rq_tail == stolen_node) {
      from->rq_tail = head;
    }
  }
  pthread_mutex_unlock(&from->rq_mu);
  if (stolen_node) {
    gof_fiber *f = stolen_node->f; free(stolen_node);
    rq_push_to(to, f);
    atomic_fetch_add(&S.steals_success, 1);
    return 1;
  }
  return 0;
}

static void fiber_entry_tramp(void *arg) {
  gof_fiber *self = (gof_fiber*)arg;
  LOGF("[gof] fiber %llu enter entry=%p arg=%p\n", (unsigned long long)self->id, (void*)self->entry, self->arg);
  self->entry(self->arg);
  self->state = GOF_FINISHED;
  /* Switch back to scheduler */
  LOGF("[gof] fiber %llu finished, switching to scheduler\n", (unsigned long long)self->id);
  gof_worker *W = cur_worker();
  gof_ctx_swap(&self->ctx, &W->sched_ctx);
}

static uint64_t gof_now_ns(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleepers_add(gof_fiber *f, uint64_t deadline_ns) {
  gof_sleep *s = (gof_sleep*)malloc(sizeof(*s));
  s->f = f; s->deadline_ns = deadline_ns; s->next = NULL;
  pthread_mutex_lock(&S.mu);
  if (!S.sleep_head || deadline_ns < S.sleep_head->deadline_ns) {
    s->next = S.sleep_head; S.sleep_head = s; pthread_mutex_unlock(&S.mu); return;
  }
  gof_sleep *cur = S.sleep_head;
  while (cur->next && cur->next->deadline_ns <= deadline_ns) cur = cur->next;
  s->next = cur->next; cur->next = s;
  pthread_mutex_unlock(&S.mu);
}

static void sleepers_wake_ready(uint64_t now_ns) {
  pthread_mutex_lock(&S.mu);
  while (S.sleep_head && S.sleep_head->deadline_ns <= now_ns) {
    gof_sleep *s = S.sleep_head; S.sleep_head = s->next;
    pthread_mutex_unlock(&S.mu);
    gof_fiber *f = s->f; free(s);
    if (f && f->state == GOF_BLOCKED) {
      f->state = GOF_RUNNABLE;
      LOGF("[gof] wake fiber %llu\n", (unsigned long long)f->id);
      rq_push(f);
    }
    pthread_mutex_lock(&S.mu);
  }
  pthread_mutex_unlock(&S.mu);
}

/* Cancel any pending sleeper entry for the given fiber. This prevents a later
 * timer wakeup from touching a fiber that has since been made runnable or finished. */
static void sleepers_cancel(gof_fiber *f) {
  if (!f) return;
  pthread_mutex_lock(&S.mu);
  for (gof_sleep *cur = S.sleep_head; cur; cur = cur->next) {
    if (cur->f == f) {
      /* Null out the pointer; the wake path will ignore and free the node. */
      cur->f = NULL;
    }
  }
  pthread_mutex_unlock(&S.mu);
}

void gof_sched_init(size_t default_stack_bytes) {
  if (S.initialized) return;
  memset(&S, 0, sizeof(S));
  S.default_stack = default_stack_bytes ? default_stack_bytes : (256 * 1024);
  S.next_id = 1;
  S.initialized = 1;
  LOGF("[gof] sched_init default_stack=%zu\n", S.default_stack);
  /* Initialize netpoll backend early */
  (void)gof_netpoll_init();
  /* Init synchronization primitives */
  pthread_mutex_init(&S.mu, NULL);
  pthread_cond_init(&S.cv, NULL);
  pthread_key_create(&S.worker_key, NULL);
  pthread_key_create(&S.poller_key, NULL);
  atomic_store(&S.steals_attempted, 0);
  atomic_store(&S.steals_success, 0);
  atomic_store(&S.inject_enqueues, 0);
  atomic_store(&S.inject_drains, 0);
  /* Determine number of workers */
  const char *env = getenv("GOF_NWORKERS");
  int n = env ? atoi(env) : 1;
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  S.nworkers = n;
  const char *steal = getenv("GOF_WORKSTEAL");
  S.enable_steal = (steal && atoi(steal) != 0) ? 1 : 0;
  /* Affinity routing default (on) */
  const char *aff = getenv("GOF_AFFINITY");
  S.affinity_enable = (aff == NULL) ? 1 : (atoi(aff) != 0);
  /* Poller partitioning default (on) */
  const char *pp = getenv("GOF_POLL_PARTITION");
  S.poll_partition_enable = (pp == NULL) ? 1 : (atoi(pp) != 0);
  /* Tunable stealing thresholds (defaults: live>=4, victim_len>=3) */
  const char *min_live = getenv("GOF_STEAL_MIN_LIVE");
  const char *min_victim = getenv("GOF_STEAL_MIN_VICTIM");
  S.steal_min_live = (min_live && atoi(min_live) >= 0) ? atoi(min_live) : 4;
  S.steal_min_victim = (min_victim && atoi(min_victim) >= 0) ? atoi(min_victim) : 3;
  if (S.steal_min_victim < 2) S.steal_min_victim = 2; /* need at least 2 to steal the second */
  /* Rebalancer defaults from env */
  const char *reb_en = getenv("GOF_REBALANCE");
  const char *reb_th = getenv("GOF_REBALANCE_THRESHOLD");
  const char *reb_iv = getenv("GOF_REBALANCE_INTERVAL_MS");
  S.rebalance_enable = reb_en ? atoi(reb_en) : 0;
  S.rebalance_threshold = reb_th ? atoi(reb_th) : 4;
  if (S.rebalance_threshold < 1) S.rebalance_threshold = 1;
  S.rebalance_interval_ms = reb_iv ? atoi(reb_iv) : 10;
  if (S.rebalance_interval_ms < 1) S.rebalance_interval_ms = 1;
  S.rebalance_last_ns = 0;
  atomic_store(&S.rebalances_attempted, 0);
  atomic_store(&S.rebalances_migrated, 0);
  S.workers = (gof_worker*)calloc((size_t)S.nworkers, sizeof(gof_worker));
  for (int i = 0; i < S.nworkers; ++i) {
    pthread_mutex_init(&S.workers[i].rq_mu, NULL);
    S.workers[i].running = 0;
    S.workers[i].index = i;
    S.workers[i].last_victim = i;
  }
  /* Determine number of pollers (default 1, <= nworkers). If pre-set via API, honor it. */
  int np;
  if (S.npollers > 0) {
    np = S.npollers;
  } else {
    const char *env_np = getenv("GOF_NPOLLERS");
    np = env_np ? atoi(env_np) : 1;
  }
  if (np < 1) np = 1;
  if (np > S.nworkers) np = S.nworkers;
  S.npollers = np;
  S.poller_threads = (pthread_t*)calloc((size_t)S.npollers, sizeof(pthread_t));
  for (int i = 0; i < S.npollers; ++i) {
    pthread_create(&S.poller_threads[i], NULL, poller_main, (void*)(intptr_t)i);
    pthread_detach(S.poller_threads[i]);
  }
}

void gof_sched_set_npollers_preinit(int n) {
  if (S.initialized) return; /* no-op after init */
  if (n < 1) n = 1;
  S.npollers = n;
}

int gof_sched_get_npollers_value(void) {
  if (S.npollers > 0) return S.npollers;
  return 1; /* default before init */
}

gof_fiber* gof_fiber_create(void (*fn)(void*), void *arg, size_t stack_bytes) {
  gof_fiber *f = (gof_fiber*)calloc(1, sizeof(*f));
  if (!f) return NULL;
  f->id = S.next_id++;
  f->state = GOF_RUNNABLE;
  f->entry = fn;
  f->arg = arg;
  f->w_affinity = -1;
  size_t sz = stack_bytes ? stack_bytes : S.default_stack;
  if (gof_stack_alloc(&f->stack, sz) != 0) { free(f); return NULL; }
  void *base = f->stack.base;
  size_t size = f->stack.size;
  if (gof_ctx_init_bootstrap(&f->ctx, base, size, fiber_entry_tramp, f) != 0) {
    gof_stack_free(&f->stack); free(f); return NULL;
  }
  LOGF("[gof] fiber_create id=%llu stack=[%p..+%zu]\n", (unsigned long long)f->id, base, size);
  atomic_fetch_add(&S.live_fibers, 1);
  gof_introspect_register(f);

  return f;
}

void gof_sched_enqueue(gof_fiber *f) {
  /* Prefer local fast-path if called from a scheduler worker thread. */
  gof_worker *W = cur_worker();
  /* Cancel any pending timed park for this fiber to avoid double-wake. */
  sleepers_cancel(f);
  if (W) { rq_push_to(W, f); return; }
  /* External thread: prefer affinity if set, else route through inject queue and signal. */
  if (S.affinity_enable && f && f->w_affinity >= 0 && f->w_affinity < S.nworkers) {
    gof_worker *WA = &S.workers[f->w_affinity];
    rq_push_to(WA, f);
    pthread_mutex_lock(&S.mu);
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
    return;
  }
  pthread_mutex_lock(&S.mu);
  gof_node *n = (gof_node*)malloc(sizeof(*n)); n->next = NULL; n->f = f;
  if (!S.inj_tail) { S.inj_head = S.inj_tail = n; }
  else { S.inj_tail->next = n; S.inj_tail = n; }
  pthread_cond_signal(&S.cv);
  pthread_mutex_unlock(&S.mu);
  atomic_fetch_add(&S.inject_enqueues, 1);
}

static void drain_inject_queue(void) {
  /* Move all injected runnables to the calling worker's local run queue */
  pthread_mutex_lock(&S.mu);
  /* During bootstrap, only worker 0 should drain to keep initial order deterministic */
  if (!S.bootstrapped) {
    gof_worker *Wcur = cur_worker();
    if (Wcur != &S.workers[0]) {
      pthread_mutex_unlock(&S.mu);
      return;
    }
  }
  gof_node *n = S.inj_head; S.inj_head = S.inj_tail = NULL;
  gof_worker *W = cur_worker();
  int drained = 0;
  while (n) {
    gof_node *next = n->next;
    /* While holding S.mu, enqueue to W's run queue to avoid exit race */
    rq_push_to(W, n->f);
    free(n);
    n = next;
    drained++;
  }
  if (!S.bootstrapped) {
    /* Mark bootstrap as done after worker0 performed the first drain */
    S.bootstrapped = 1;
  }
  pthread_mutex_unlock(&S.mu);
  if (drained > 0) atomic_fetch_add(&S.inject_drains, (unsigned long long)drained);
}

static void* worker_main(void *arg) {
  /* Set TLS for this worker */
  gof_worker *W = (gof_worker*)arg;
  pthread_setspecific(S.worker_key, W);
  /* Simple loop until no runnables remain */
  for(;;) {
    /* Drain any cross-thread injections first */
    drain_inject_queue();
    /* Wake any sleepers that are ready now */
    sleepers_wake_ready(gof_now_ns());
    /* Periodically rebalance from worker 0 */
    if (W->index == 0) { maybe_rebalance(); }
    gof_fiber *f = rq_pop_mark_running(W);
    if (!f && S.nworkers > 1 && S.enable_steal) {
      /* Attempt to steal one task from other workers using rotating start */
      int start = (W->last_victim + 1) % S.nworkers;
      for (int off = 0; off < S.nworkers && !f; ++off) {
        int vi = (start + off) % S.nworkers;
        gof_worker *V = &S.workers[vi];
        if (V == W) continue;
        if (rq_steal_one(V, W)) {
          f = rq_pop_mark_running(W);
          W->last_victim = vi;
        }
      }
    }
    if (!f) {
      /* Idle: wait on condition variable until either inject queue gets items or next sleeper is due. */
      pthread_mutex_lock(&S.mu);
      int exit_sched = 0;
      for (;;) {
        int have_inject = (S.inj_head != NULL);
        int have_sleepers = (S.sleep_head != NULL);
        int have_live = (atomic_load(&S.live_fibers) > 0);
        /* Check if any worker currently has runnable items queued */
        int have_runnables = 0;
        for (int i = 0; i < S.nworkers && !have_runnables; ++i) {
          pthread_mutex_lock(&S.workers[i].rq_mu);
          have_runnables = (S.workers[i].rq_head != NULL);
          pthread_mutex_unlock(&S.workers[i].rq_mu);
        }
        if (have_inject || have_runnables) {
          break; /* will drain after unlocking */
        }
        if (!have_sleepers) {
          /* No sleepers and no inject; if no IO waiters either, exit loop */
          if (!gof_io_have_waiters() && !have_live) { exit_sched = 1; break; }
          /* Otherwise, wait indefinitely for injected work */
          pthread_cond_wait(&S.cv, &S.mu);
          continue;
        }
        uint64_t now = gof_now_ns();
        if (S.sleep_head->deadline_ns <= now) {
          /* sleeper due; break to outer to handle wake */
          break;
        }
        /* Timed wait until next deadline or until signaled */
        uint64_t ns = S.sleep_head->deadline_ns - now;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t cur_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        uint64_t abs_ns = cur_ns + ns;
        struct timespec abstime; abstime.tv_sec = abs_ns / 1000000000ull; abstime.tv_nsec = abs_ns % 1000000000ull;
        pthread_cond_timedwait(&S.cv, &S.mu, &abstime);
        /* loop and re-check predicate */
      }
      pthread_mutex_unlock(&S.mu);
      if (exit_sched) {
        break; /* Exit scheduler loop cleanly */
      }
      continue;
    }
    W->current = f;
    /* Update fiber affinity to this worker for locality */
    if (f) f->w_affinity = W->index;
    LOGF("[gof] switch to fiber id=%llu\n", (unsigned long long)f->id);
    /* Switch to fiber */
    gof_ctx_swap(&W->sched_ctx, &f->ctx);
    /* Returned from fiber (yield/finish/block) */
    pthread_mutex_lock(&W->rq_mu);
    W->running = 0;
    pthread_mutex_unlock(&W->rq_mu);
    if (f->state == GOF_RUNNABLE) {
      LOGF("[gof] fiber %llu yielded; requeue\n", (unsigned long long)f->id);
      rq_push(f);
    } else if (f->state == GOF_FINISHED) {
      LOGF("[gof] fiber %llu cleanup\n", (unsigned long long)f->id);
      gof_introspect_unregister(f);
      gof_stack_free(&f->stack);
      free(f);
      atomic_fetch_sub(&S.live_fibers, 1);
    } else {
      /* BLOCKED: parked elsewhere */
      LOGF("[gof] fiber %llu blocked\n", (unsigned long long)f->id);
    }
    W->current = NULL;
  }
  return NULL;
}

void gof_sched_run(void) {
  /* Create additional workers if configured (>1). The current thread becomes worker 0. */
  if (S.nworkers <= 0) {
    S.nworkers = 1;
    S.workers = (gof_worker*)calloc(1, sizeof(gof_worker));
  }
  /* Launch worker threads 1..n-1 */
  for (int i = 1; i < S.nworkers; ++i) {
    pthread_create(&S.workers[i].tid, NULL, worker_main, &S.workers[i]);
  }
  /* Run worker 0 on this thread */
  (void)pthread_setspecific(S.worker_key, &S.workers[0]);
  (void)worker_main(&S.workers[0]);
}

void gof_sched_yield(void) {
  gof_worker *W = cur_worker();
  gof_fiber *self = W ? W->current : NULL;
  if (!self) return; /* not in fiber */
  /* Mark runnable and switch to scheduler; scheduler will requeue once */
  self->state = GOF_RUNNABLE;
  LOGF("[gof] fiber %llu yield\n", (unsigned long long)self->id);
  gof_ctx_swap(&self->ctx, &W->sched_ctx);
}

gof_fiber* gof_sched_current(void) {
  gof_worker *W = cur_worker();
  return W ? W->current : NULL;
}

void gof_sched_block_current(void) {
  gof_worker *W = cur_worker();
  gof_fiber *self = W ? W->current : NULL;
  if (!self) return;
  self->state = GOF_BLOCKED;
  /* Switch to scheduler without enqueuing */
  LOGF("[gof] fiber %llu block\n", (unsigned long long)self->id);
  gof_ctx_swap(&self->ctx, &W->sched_ctx);
}

void gof_sched_make_runnable(gof_fiber *f) {
  /* If called from a scheduler worker, push to its local queue to preserve ordering. */
  gof_worker *W = cur_worker();
  sleepers_cancel(f);
  if (W) { rq_push_to(W, f); return; }
  /* External thread (e.g., poller): prefer to enqueue to affinity worker if any */
  if (S.affinity_enable && f && f->w_affinity >= 0 && f->w_affinity < S.nworkers) {
    gof_worker *WA = &S.workers[f->w_affinity];
    rq_push_to(WA, f);
    pthread_mutex_lock(&S.mu);
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
    return;
  }
  /* Otherwise, route through global inject queue and signal */
  pthread_mutex_lock(&S.mu);
  gof_node *n = (gof_node*)malloc(sizeof(*n)); n->next = NULL; n->f = f;
  if (!S.inj_tail) { S.inj_head = S.inj_tail = n; }
  else { S.inj_tail->next = n; S.inj_tail = n; }
  pthread_cond_signal(&S.cv);
  pthread_mutex_unlock(&S.mu);
}

/* Partition-aware runnable enqueue: try to keep ready fiber within same poller partition. */
void gof_sched_make_runnable_from_poller(gof_fiber *f, int poller_index) {
  if (!f) return;
  /* If called accidentally from a worker, use fast path */
  gof_worker *W = cur_worker();
  sleepers_cancel(f);
  if (W) { rq_push_to(W, f); return; }
  int use_partition = (S.poll_partition_enable && S.npollers > 0 && poller_index >= 0);
  if (!use_partition) {
    gof_sched_make_runnable(f);
    return;
  }
  int target = -1;
  /* If affinity is enabled and falls within this partition, prefer that */
  if (S.affinity_enable && f->w_affinity >= 0 && f->w_affinity < S.nworkers) {
    if ((f->w_affinity % S.npollers) == poller_index) {
      target = f->w_affinity;
    }
  }
  if (target < 0) {
    /* Choose first worker that belongs to this poller partition */
    for (int w = poller_index; w < S.nworkers; w += S.npollers) { target = w; break; }
  }
  if (target >= 0) {
    gof_worker *WT = &S.workers[target];
    rq_push_to(WT, f);
    pthread_mutex_lock(&S.mu);
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
    return;
  }
  /* Fallback */
  gof_sched_make_runnable(f);
}

void gof_sched_park_until(uint64_t deadline_ns) {
  gof_worker *W = cur_worker();
  gof_fiber *self = W ? W->current : NULL;
  if (!self) return;
  self->state = GOF_BLOCKED;
  sleepers_add(self, deadline_ns);
  LOGF("[gof] fiber %llu park until %llu\n", (unsigned long long)self->id, (unsigned long long)deadline_ns);
  gof_ctx_swap(&self->ctx, &W->sched_ctx);
}

void gof_sched_unpark_ready(void) {
  sleepers_wake_ready(gof_now_ns());
}

/* Dedicated poller thread: waits for IO readiness and wakes fibers via callback path */
static void* poller_main(void* arg) {
  int idx = (int)(intptr_t)arg;
  pthread_setspecific(S.poller_key, (void*)((intptr_t)idx + 1));
  for(;;) {
    (void)gof_netpoll_wait(-1);
    /* Callback path enqueues runnables and signals condvar */
  }
  return NULL;
}
