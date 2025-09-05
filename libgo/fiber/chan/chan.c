#include "../include/libgo/fiber_chan.h"
#include "../sched/sched.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct waiter {
  struct waiter *next;
  gof_fiber     *f;
  void         **slot;   /* for receiver: where to store; for sender: points to value to send */
  void          *value;  /* for sender: cached value; for receiver unused */
  int            is_sender;
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
  gof_sched_make_runnable(r->f);
  free(r);
  (void)c; return 1;
}

static int handoff_from_waiter(gof_chan *c, void **out) {
  waiter *s = qpop(&c->sendq);
  if (!s) return 0;
  void *v = s->value;
  if (out) *out = v;
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
  for(;;) {
    pthread_mutex_lock(&c->mu);
    if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
    /* If receiver available, handoff now */
    if (handoff_to_waiter(c, value)) { pthread_mutex_unlock(&c->mu); return 0; }
    /* If buffer has space, use it */
    if (c->cap > 0 && !is_full(c)) { buf_put(c, value); pthread_mutex_unlock(&c->mu); return 0; }
    /* enqueue self as sender */
    waiter *w = (waiter*)calloc(1, sizeof(*w));
    w->f = gof_sched_current();
    w->is_sender = 1;
    w->value = value;
    qpush(&c->sendq, w);
    pthread_mutex_unlock(&c->mu);
    gof_sched_block_current();
    /* Unblocked by a receiver; send completed */
    return 0;
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
  for(;;) {
    pthread_mutex_lock(&c->mu);
    /* If buffer has data, take it */
    if (!is_empty(c)) { if (out_value) *out_value = buf_get(c); pthread_mutex_unlock(&c->mu); return 0; }
    /* If a sender is waiting, handoff */
    if (handoff_from_waiter(c, out_value)) { pthread_mutex_unlock(&c->mu); return 0; }
    if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }
    /* enqueue self as receiver */
    waiter *w = (waiter*)calloc(1, sizeof(*w));
    w->f = gof_sched_current();
    w->is_sender = 0;
    w->slot = out_value;
    qpush(&c->recvq, w);
    pthread_mutex_unlock(&c->mu);
    gof_sched_block_current();
    /* Sender delivered directly and unblocked us; consider it success */
    return 0;
  }
}
