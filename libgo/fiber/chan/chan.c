#include "../include/libgo/fiber_chan.h"
#include "../sched/sched.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct waiter {
  struct waiter *next;
  gof_fiber     *f;
  void         **slot;   /* for receiver: where to store; for sender: points to value to send */
  void          *value;  /* for sender: cached value; for receiver unused */
  int            is_sender;
  int           *done;   /* points to caller's stack-local flag; set to 1 on successful handoff */
} waiter;

typedef struct gof_chan {
  size_t   cap;
  size_t   head;
  size_t   size;
  void   **buf;      /* ring of void* */
  int      closed;
  waiter  *sendq;
  waiter  *recvq;
  pthread_mutex_t mu; /* protects buf/sendq/recvq/closed */
} gof_chan;

static void qpush(waiter **q, waiter *w){ w->next=NULL; if(!*q){*q=w;return;} waiter *t=*q; while(t->next) t=t->next; t->next=w; }
static waiter* qpop(waiter **q){ waiter *w=*q; if(!w) return NULL; *q=w->next; return w; }

static void buf_put(gof_chan *c, void *v){ size_t idx=(c->head + c->size) % c->cap; c->buf[idx]=v; c->size++; }
static void* buf_get(gof_chan *c){ void *v=c->buf[c->head]; c->head=(c->head+1)%c->cap; c->size--; return v; }

static int handoff_to_waiter(gof_chan *c, void *v) {
  waiter *r = qpop(&c->recvq);
  if (!r) return 0;
  /* deliver directly */
  if (r->slot) *r->slot = v; else { /* ignore if no slot */ }
  if (r->done) *r->done = 1; /* signal successful handoff before waking */
  gof_sched_make_runnable(r->f);
  free(r);
  (void)c; return 1;
}

static int handoff_from_waiter(gof_chan *c, void **out) {
  waiter *s = qpop(&c->sendq);
  if (!s) return 0;
  void *v = s->value;
  if (out) *out = v;
  if (s->done) *s->done = 1; /* signal successful handoff before waking */
  gof_sched_make_runnable(s->f);
  free(s);
  (void)c; return 1;
}

static int is_full(gof_chan *c){ return c->cap>0 && c->size==c->cap; }
static int is_empty(gof_chan *c){ return c->cap==0 ? 1 : (c->size==0); }

/* API */
gof_chan_t* gof_chan_make(size_t capacity) {
  gof_chan *c = (gof_chan*)calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->cap = capacity;
  if (capacity > 0) {
    c->buf = (void**)calloc(capacity, sizeof(void*));
    if (!c->buf) { free(c); return NULL; }
  }
  pthread_mutex_init(&c->mu, NULL);
  return (gof_chan_t*)c;
}

void gof_chan_close(gof_chan_t* cc) {
  gof_chan *c = (gof_chan*)cc;
  if (!c) return;
  pthread_mutex_lock(&c->mu);
  c->closed = 1;
  /* Pop all waiters under lock, then wake after unlock to avoid holding mutex during wake */
  waiter *rq = c->recvq; c->recvq = NULL;
  waiter *sq = c->sendq; c->sendq = NULL;
  pthread_mutex_unlock(&c->mu);
  waiter *w;
  while ((w = qpop(&rq))) { gof_sched_make_runnable(w->f); free(w); }
  while ((w = qpop(&sq))) { gof_sched_make_runnable(w->f); free(w); }
}

int gof_chan_try_send(gof_chan_t* cc, void* value) {
  gof_chan *c = (gof_chan*)cc;
  if (!c || c->closed) return -1;
  pthread_mutex_lock(&c->mu);
  if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
  /* If a receiver is waiting, handoff */
  if (handoff_to_waiter(c, value)) { pthread_mutex_unlock(&c->mu); return 1; }
  if (c->cap == 0) { pthread_mutex_unlock(&c->mu); return 0; } /* would block */
  if (!is_full(c)) { buf_put(c, value); pthread_mutex_unlock(&c->mu); return 1; }
  pthread_mutex_unlock(&c->mu);
  return 0;
}

int gof_chan_try_recv(gof_chan_t* cc, void** out_value) {
  gof_chan *c = (gof_chan*)cc;
  if (!c) return -1;
  pthread_mutex_lock(&c->mu);
  if (!is_empty(c)) { if (out_value) *out_value = buf_get(c); pthread_mutex_unlock(&c->mu); return 1; }
  /* if sender waiting, take from sender */
  if (handoff_from_waiter(c, out_value)) { pthread_mutex_unlock(&c->mu); return 1; }
  if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
  pthread_mutex_unlock(&c->mu);
  return 0;
}

int gof_chan_send(gof_chan_t* cc, void* value) {
  gof_chan *c = (gof_chan*)cc;
  if (!c) return -1;
  /* fast path */
  int tr = gof_chan_try_send(cc, value);
  if (tr == 1) return 0; /* success */
  if (tr < 0) return -1; /* closed */
  /* must block */
  pthread_mutex_lock(&c->mu);
  if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
  /* If receiver available, handoff now */
  if (handoff_to_waiter(c, value)) { pthread_mutex_unlock(&c->mu); return 0; }
  /* If buffer has space, use it */
  if (c->cap > 0 && !is_full(c)) { buf_put(c, value); pthread_mutex_unlock(&c->mu); return 0; }
  /* enqueue self as sender */
  int done = 0;
  waiter *w = (waiter*)calloc(1, sizeof(*w));
  if (!w) { fprintf(stderr, "[gof] FATAL: malloc failed in gof_chan_send\n"); abort(); }
  w->f = gof_sched_current();
  w->is_sender = 1;
  w->value = value;
  w->done = &done;
  qpush(&c->sendq, w);
  pthread_mutex_unlock(&c->mu);
  /* Park until the handoff completes or the channel closes. The loop (not
   * an if) is required: gof_sched_block_current may return without our
   * waiter being touched when it consumes a stale pending wake
   * (nostrc-q9lp0); our waiter stays queued and we park again. */
  for (;;) {
    gof_sched_block_current();
    if (done) return 0; /* handoff_to_waiter set done before waking us */
    pthread_mutex_lock(&c->mu);
    if (done) { pthread_mutex_unlock(&c->mu); return 0; }
    if (c->closed) {
      /* close() detached and freed every queued waiter, including ours:
       * do not touch w. */
      pthread_mutex_unlock(&c->mu);
      return -1;
    }
    pthread_mutex_unlock(&c->mu);
    /* Spurious wake: our waiter is still queued; park again. */
  }
}

int gof_chan_recv(gof_chan_t* cc, void** out_value) {
  gof_chan *c = (gof_chan*)cc;
  if (!c) return -1;
  /* fast path */
  int tr = gof_chan_try_recv(cc, out_value);
  if (tr == 1) return 0;
  if (tr < 0) return -1;
  /* must block */
  pthread_mutex_lock(&c->mu);
  /* If buffer has data, take it */
  if (!is_empty(c)) { if (out_value) *out_value = buf_get(c); pthread_mutex_unlock(&c->mu); return 0; }
  /* If a sender is waiting, handoff */
  if (handoff_from_waiter(c, out_value)) { pthread_mutex_unlock(&c->mu); return 0; }
  if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
  /* enqueue self as receiver */
  int done = 0;
  waiter *w = (waiter*)calloc(1, sizeof(*w));
  if (!w) { fprintf(stderr, "[gof] FATAL: malloc failed in gof_chan_recv\n"); abort(); }
  w->f = gof_sched_current();
  w->is_sender = 0;
  w->slot = out_value;
  w->done = &done;
  qpush(&c->recvq, w);
  pthread_mutex_unlock(&c->mu);
  /* Park until a handoff or close; re-park on spurious wakes (see
   * gof_chan_send, nostrc-q9lp0). */
  for (;;) {
    gof_sched_block_current();
    if (done) return 0;
    pthread_mutex_lock(&c->mu);
    if (done) { pthread_mutex_unlock(&c->mu); return 0; }
    if (c->closed) {
      /* close() detached and freed our waiter; do not touch w. */
      pthread_mutex_unlock(&c->mu);
      return -1;
    }
    pthread_mutex_unlock(&c->mu);
  }
}
