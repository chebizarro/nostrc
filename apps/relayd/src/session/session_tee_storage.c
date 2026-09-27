/*
 * session_tee_storage.c — see session_tee_storage.h.
 */
#include "session_tee_storage.h"

#include <stdlib.h>

typedef struct {
  NostrStorageVTable vt; /* per-instance: NULL where the inner store has NULL */
  NostrStorage *inner;
  NsrTeeOfferFn offer;
  void *ud;
} Tee;

#define INNER(s) (((Tee *)(s)->impl)->inner)
#define IVT(s) (INNER(s)->vt)

static int tee_open(NostrStorage *s, const char *uri, const char *opts) {
  (void)s; (void)uri; (void)opts;
  return 0;
}
static void tee_close(NostrStorage *s) { (void)s; }

/* Store first, queue second: see the header. If the queue write fails the
 * event stays in the local store and the client is told to retry (a retry
 * of the same event re-queues it; see README "Upstream federation"). */
static int tee_put(NostrStorage *s, const NostrEvent *ev) {
  Tee *t = s->impl;
  int rc = IVT(s)->put_event(INNER(s), ev);
  if (rc != 0) return rc;
  /* libnostr getters are not const-clean */
  return t->offer ? t->offer(t->ud, (NostrEvent *)ev) : 0;
}
static int tee_put_relay(NostrStorage *s, const NostrEvent *ev, const char *relay) {
  Tee *t = s->impl;
  int rc = IVT(s)->put_event_with_relay(INNER(s), ev, relay);
  if (rc != 0) return rc;
  return t->offer ? t->offer(t->ud, (NostrEvent *)ev) : 0;
}
/* Bulk ingest bypasses federation: it is an import path, not an app write. */
static int tee_ingest(NostrStorage *s, const char *l, size_t n) {
  return IVT(s)->ingest_ldjson(INNER(s), l, n);
}
static int tee_ingest_relay(NostrStorage *s, const char *l, size_t n, const char *r) {
  return IVT(s)->ingest_ldjson_with_relay(INNER(s), l, n, r);
}
static int tee_delete(NostrStorage *s, const char *id) {
  return IVT(s)->delete_event(INNER(s), id);
}
static void *tee_query(NostrStorage *s, const NostrFilter *f, size_t nf, size_t limit,
                       uint64_t since, uint64_t until, int *err) {
  return IVT(s)->query(INNER(s), f, nf, limit, since, until, err);
}
static int tee_query_next(NostrStorage *s, void *it, NostrEvent *out, size_t *n) {
  return IVT(s)->query_next(INNER(s), it, out, n);
}
static void tee_query_free(NostrStorage *s, void *it) { IVT(s)->query_free(INNER(s), it); }
static int tee_count(NostrStorage *s, const NostrFilter *f, size_t nf, uint64_t *out) {
  return IVT(s)->count(INNER(s), f, nf, out);
}
static int tee_search(NostrStorage *s, const char *q, const NostrFilter *scope, size_t limit,
                      void **it) {
  return IVT(s)->search(INNER(s), q, scope, limit, it);
}
static int tee_set_digest(NostrStorage *s, const NostrFilter *scope, void **state) {
  return IVT(s)->set_digest(INNER(s), scope, state);
}
static int tee_set_reconcile(NostrStorage *s, void *state, const void *msg, size_t len,
                             void **resp, size_t *resp_len) {
  return IVT(s)->set_reconcile(INNER(s), state, msg, len, resp, resp_len);
}
static void tee_set_free(NostrStorage *s, void *state) { IVT(s)->set_free(INNER(s), state); }

NostrStorage *nsr_tee_storage_new(NostrStorage *inner, NsrTeeOfferFn offer, void *user_data) {
  if (!inner || !inner->vt) return NULL;
  NostrStorage *s = calloc(1, sizeof *s);
  Tee *t = calloc(1, sizeof *t);
  if (!s || !t) {
    free(s);
    free(t);
    return NULL;
  }
  const NostrStorageVTable *i = inner->vt;
  t->inner = inner;
  t->offer = offer;
  t->ud = user_data;
  t->vt.open = tee_open;
  t->vt.close = tee_close;
#define FWD(member, fn) t->vt.member = i->member ? fn : NULL
  FWD(put_event, tee_put);
  FWD(put_event_with_relay, tee_put_relay);
  FWD(ingest_ldjson, tee_ingest);
  FWD(ingest_ldjson_with_relay, tee_ingest_relay);
  FWD(delete_event, tee_delete);
  FWD(query, tee_query);
  FWD(query_next, tee_query_next);
  FWD(query_free, tee_query_free);
  FWD(count, tee_count);
  FWD(search, tee_search);
  FWD(set_digest, tee_set_digest);
  FWD(set_reconcile, tee_set_reconcile);
  FWD(set_free, tee_set_free);
#undef FWD
  s->vt = &t->vt;
  s->impl = t;
  return s;
}

void nsr_tee_storage_free(NostrStorage *tee) {
  if (!tee) return;
  free(tee->impl);
  free(tee);
}
