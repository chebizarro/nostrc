#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <libwebsockets.h>
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"
#include "nostr-relay-limits.h"
#include "nostr-utils.h"
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "protocol_nip01.h"
#include "protocol_nip42.h"
#include "protocol_nip45.h"
#include "protocol_nip50.h"
#include "rate_limit.h"
#include "protocol_nip77.h"
#include "metrics.h"
#include "relay_ingress.h"
#include "relay_policy.h"

static int conn_sub_find(const ConnState *cs, const char *sub, size_t sub_len);
static const char *vault_denial(const RelaydCtx *ctx, const ConnState *cs);
static int vault_kind(int kind);
static int filters_include_vault(NostrFilter **filters, size_t n);

static void frame_free(RelaydFrame *frame) {
  free(frame->text);
  free(frame);
}

static void queue_text(ConnState *cs, const char *text, const char *subid,
                       const char *event_id) {
  if (!cs || !cs->wsi || !text || cs->closing) return;
  size_t len = strlen(text);
  if (cs->send_count >= RELAYD_MAX_QUEUED_FRAMES ||
      len > RELAYD_MAX_QUEUED_BYTES - cs->send_bytes) goto overflow;
  RelaydFrame *frame = calloc(1, sizeof *frame);
  if (!frame) goto overflow;
  frame->text = malloc(LWS_PRE + len);
  if (!frame->text) { free(frame); goto overflow; }
  memcpy(frame->text + LWS_PRE, text, len);
  frame->length = len;
  if (subid) snprintf(frame->subid, sizeof frame->subid, "%s", subid);
  if (event_id) snprintf(frame->event_id, sizeof frame->event_id, "%s", event_id);
  if (cs->send_tail) cs->send_tail->next = frame;
  else cs->send_head = frame;
  cs->send_tail = frame;
  cs->send_count++;
  cs->send_bytes += len;
  lws_callback_on_writable(cs->wsi);
  return;
overflow:
  cs->closing = 1;
  lws_callback_on_writable(cs->wsi);
}

void relayd_conn_send_text(struct lws *wsi, const char *text) {
  if (wsi) queue_text((ConnState*)lws_wsi_user(wsi), text, NULL, NULL);
}

#define ws_send_text relayd_conn_send_text

void relayd_conn_send_free_all(ConnState *cs) {
  while (cs->send_head) {
    RelaydFrame *next = cs->send_head->next;
    frame_free(cs->send_head);
    cs->send_head = next;
  }
  cs->send_tail = NULL;
  cs->send_count = cs->send_bytes = 0;
}

static void send_event(ConnState *cs, const char *subid, const char *json,
                        const char *id) {
  size_t need = strlen(json) + strlen(subid) + 16;
  char *frame = malloc(need);
  if (!frame) { cs->closing = 1; lws_callback_on_writable(cs->wsi); return; }
  snprintf(frame, need, "[\"EVENT\",\"%s\",%s]", subid, json);
  queue_text(cs, frame, subid, id);
  free(frame);
}

static int live_filter_matches(const RelaydCtx *ctx, NostrFilter *filter,
                                NostrEvent *ev, const char *id) {
  if (!nostr_filter_matches(filter, ev)) return 0;
  const char *search = nostr_filter_get_search(filter);
  if (!search || !*search) return 1;
  /* NIP-50 semantics belong to the backend, not a substring approximation.
   * Scope this event-triggered search to the one newly stored id. */
  NostrStorage *st = ctx->storage;
  if (!relayd_storage_can_query(st) || !st->vt->search) return -1;
  NostrFilter *scope = nostr_filter_new();
  NostrEvent *found = nostr_event_new();
  if (!scope || !found) { nostr_filter_free(scope); nostr_event_free(found); return -1; }
  nostr_filter_add_id(scope, id);
  void *it = NULL;
  int matches = 0;
  int rc = st->vt->search(st, search, scope, 1, &it);
  if (rc) matches = -1;
  else if (it) {
    size_t n = 1;
    rc = st->vt->query_next(st, it, found, &n);
    if (rc) matches = -1;
    else if (n) {
      char *found_id = nostr_event_get_id(found);
      matches = found_id ? strcmp(found_id, id) == 0 : -1;
      free(found_id);
    }
  }
  if (it) st->vt->query_free(st, it);
  nostr_event_free(found);
  nostr_filter_free(scope);
  return matches;
}

/* Called only from ingress/commit reconciliation on the lws service thread.
 * A matching OR-list produces one delivery, not one per matching filter. */
static void fanout(const RelaydCtx *ctx, NostrEvent *ev) {
  char *json = nostr_event_serialize(ev);
  char *id = nostr_event_get_id(ev);
  if (!json || !id) {
    for (ConnState *cs = ctx->clients; cs; cs = cs->next_client)
      if (cs->nsubs) { cs->closing = 1; lws_callback_on_writable(cs->wsi); }
    free(json); free(id);
    return;
  }
  for (ConnState *cs = ctx->clients; cs; cs = cs->next_client) {
    if (cs->closing ||
        (strcmp(ctx->cfg.auth, "required") == 0 && !cs->authed)) continue;
    for (size_t i = 0; i < cs->nsubs; i++) {
      RelaydPendingSub *ps = &cs->subs[i];
      if (vault_denial(ctx, cs) &&
          (vault_kind(nostr_event_get_kind(ev)) ||
           filters_include_vault(ps->filters, ps->nfilters))) continue;
      for (size_t f = 0; f < ps->nfilters; f++) {
        int matches = live_filter_matches(ctx, ps->filters[f], ev, id);
        if (matches < 0) {
          cs->closing = 1;
          lws_callback_on_writable(cs->wsi);
          break;
        }
        if (!matches) continue;
        send_event(cs, ps->subid, json, id);
        break;
      }
    }
  }
  free(id);
  free(json);
}

static inline int is_replaceable_kind(int kind) { return kind == 0 || kind == 3 || (kind >= 10000 && kind < 20000); }
static inline int is_param_replaceable_kind(int kind) { return kind >= 30000 && kind < 40000; }

static uint32_t frame_operation_cost(const RelaydCtx *ctx, const char *msg,
                                     size_t len) {
  if (!ctx || !msg) return 1;
  if (len >= 8 && memcmp(msg, "[\"CLOSE\"", 8) == 0) return 0;
  if (len >= 7 && memcmp(msg, "[\"EVENT\"", 7) == 0)
    return (uint32_t)ctx->cfg.rate_event_cost;
  if ((len >= 6 && memcmp(msg, "[\"REQ\"", 6) == 0) ||
      (len >= 8 && memcmp(msg, "[\"COUNT\"", 8) == 0)) {
    uint32_t filters = 0;
    for (size_t i = 0; i < len; ++i)
      if (msg[i] == '{' && filters < (uint32_t)ctx->cfg.max_filters) filters++;
    return 1u + filters;
  }
  return 1;
}

/* Upper bound on filters collected from one REQ, independent of
 * cfg.max_filters, so a hostile frame cannot make us allocate unboundedly
 * before the max_filters check rejects it. */
#define REQ_FILTERS_HARD_CAP 256

/*
 * Parse the filter list of a REQ: the text after the subscription id, i.e.
 * `{f1},{f2},...` as NIP-01 sends it (also accepted wrapped as `[{f1},...]`).
 * Each top-level object is located with a string-aware scan -- braces
 * inside values (search terms, tag values) do not end a filter -- and
 * deserialized from its own NUL-terminated copy. Anything other than
 * objects, commas, brackets and whitespace is invalid. Returns 0 and the
 * filters (possibly more than cfg.max_filters; the caller checks), or -1.
 */
static int parse_filters_json_local(const char *json, NostrFilter ***out_arr, size_t *out_n) {
  if (!out_arr || !out_n || !json) return -1;
  *out_arr = NULL; *out_n = 0;
  NostrFilter **arr = NULL;
  size_t n = 0, cap = 0;
  const char *q = json;
  while (*q) {
    if (*q == ',' || *q == '[' || *q == ']' || *q == ' ' || *q == '\t' ||
        *q == '\r' || *q == '\n') {
      q++;
      continue;
    }
    if (*q != '{') goto fail;
    const char *start = q;
    int depth = 0, in_str = 0;
    for (; *q; q++) {
      if (in_str) {
        if (*q == '\\' && q[1]) q++;
        else if (*q == '"') in_str = 0;
        continue;
      }
      if (*q == '"') in_str = 1;
      else if (*q == '{') depth++;
      else if (*q == '}' && --depth == 0) { q++; break; }
    }
    if (depth != 0 || n >= REQ_FILTERS_HARD_CAP) goto fail;
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 4;
      NostrFilter **tmp = (NostrFilter**)realloc(arr, ncap * sizeof(NostrFilter*));
      if (!tmp) goto fail;
      arr = tmp; cap = ncap;
    }
    size_t olen = (size_t)(q - start);
    char *obj = (char*)malloc(olen + 1);
    /* nostr_filter_new(), not calloc: the deserializer appends to the
     * filter's arrays, and a zeroed IntArray has capacity 0 (heap overflow
     * in int_array_add, caught by ASan). */
    NostrFilter *f = nostr_filter_new();
    if (!obj || !f) { free(obj); nostr_filter_free(f); goto fail; }
    memcpy(obj, start, olen); obj[olen] = '\0';
    int rc = nostr_filter_deserialize(f, obj);
    free(obj);
    if (rc != 0) { nostr_filter_free(f); goto fail; }
    arr[n++] = f;
  }
  *out_arr = arr; *out_n = n;
  return 0;
fail:
  for (size_t i = 0; i < n; i++) nostr_filter_free(arr[i]);
  free(arr);
  return -1;
}

int relayd_storage_can_query(const NostrStorage *st) {
  return st && st->vt && st->vt->query && st->vt->query_next &&
         st->vt->query_free;
}

static int conn_sub_find(const ConnState *cs, const char *sub, size_t sub_len) {
  for (size_t i = 0; i < cs->nsubs; i++)
    if (strlen(cs->subs[i].subid) == sub_len &&
        memcmp(cs->subs[i].subid, sub, sub_len) == 0)
      return (int)i;
  return -1;
}

/* Drop a subscription and any queued deliveries on CLOSE/replacement. */
static void conn_sub_remove(ConnState *cs, const RelaydCtx *ctx, size_t idx) {
  NostrStorage *st = ctx ? ctx->storage : NULL;
  if (cs->subs[idx].it && relayd_storage_can_query(st))
    st->vt->query_free(st, cs->subs[idx].it);
  for (size_t f = 0; f < cs->subs[idx].nfilters; f++)
    nostr_filter_free(cs->subs[idx].filters[f]);
  free(cs->subs[idx].filters);
  RelaydFrame **link = &cs->send_head;
  cs->send_tail = NULL;
  while (*link) {
    RelaydFrame *frame = *link;
    if (strcmp(frame->subid, cs->subs[idx].subid) == 0) {
      *link = frame->next;
      cs->send_count--;
      cs->send_bytes -= frame->length;
      frame_free(frame);
    } else {
      cs->send_tail = frame;
      link = &frame->next;
    }
  }
  memmove(&cs->subs[idx], &cs->subs[idx + 1],
          (cs->nsubs - idx - 1) * sizeof(cs->subs[0]));
  cs->nsubs--;
  memset(&cs->subs[cs->nsubs], 0, sizeof(cs->subs[0]));
  metrics_on_sub_end();
}

void relayd_conn_sub_push(struct lws *wsi, ConnState *cs, const char *sub,
                          size_t sub_len, void *it) {
  RelaydPendingSub *ps = &cs->subs[cs->nsubs++];
  memcpy(ps->subid, sub, sub_len);
  ps->subid[sub_len] = '\0';
  ps->it = it;
  metrics_on_sub_start();
  lws_callback_on_writable(wsi);
}

void relayd_conn_subs_free_all(ConnState *cs, const RelaydCtx *ctx) {
  while (cs->nsubs > 0) conn_sub_remove(cs, ctx, cs->nsubs - 1);
}

static void ack_finish(RelaydPendingAck *ack, const RelaydCtx *ctx, int accepted) {
  if (ack->ready) return;
  ack->ready = 1;
  ack->accepted = accepted;
  (void)relay_ingress_finish(ctx->policy, &ack->ingress, accepted);
  ((RelaydCtx*)ctx)->pending_stores--;
  if (accepted && ack->old_replaceable_id && ctx->storage->vt->delete_event)
    (void)ctx->storage->vt->delete_event(ctx->storage,
                                         ack->old_replaceable_id);
  if (accepted) fanout(ctx, ack->ingress.event);
}

static void ack_schedule_timer(struct lws *wsi, const ConnState *cs) {
  uint64_t first = UINT64_MAX;
  for (const RelaydPendingAck *ack = cs->ack_head; ack; ack = ack->next)
    if (!ack->ready && ack->deadline_ms < first) first = ack->deadline_ms;
  if (first == UINT64_MAX) {
    lws_set_timer_usecs(wsi, LWS_SET_TIMER_USEC_CANCEL);
    return;
  }
  uint64_t now = rate_limit_now_ms();
  lws_set_timer_usecs(wsi, first > now ? (first - now) * 1000u : 1u);
}

static void ack_reconcile_connection(struct lws *wsi, ConnState *cs,
                                     const RelaydCtx *ctx) {
  if (!ctx->async_storage || !cs->ack_head) return;
  uint64_t now = rate_limit_now_ms();
  for (RelaydPendingAck *ack = cs->ack_head; ack; ack = ack->next) {
    if (ack->ready) continue;
    int visible = ctx->async_storage->visible(ctx->storage,
                                              ack->ingress.binary_id);
    if (visible > 0) ack_finish(ack, ctx, 1);
    else if (visible < 0 || now >= ack->deadline_ms)
      ack_finish(ack, ctx, 0);
  }
  ack_schedule_timer(wsi, cs);
  if (cs->ack_head->ready) lws_callback_on_writable(wsi);
}

void relayd_nip01_reconcile_all(const RelaydCtx *ctx) {
  if (!ctx || !ctx->async_storage) return;
  if (ctx->async_storage->drain) ctx->async_storage->drain(ctx->storage);
  for (ConnState *cs = ctx->clients; cs; cs = cs->next_client)
    ack_reconcile_connection(cs->wsi, cs, ctx);
  RelaydPendingAck **link = &((RelaydCtx*)ctx)->orphan_acks;
  while (*link) {
    RelaydPendingAck *ack = *link;
    int visible = ctx->async_storage->visible(ctx->storage, ack->ingress.binary_id);
    if (visible == 0 && rate_limit_now_ms() < ack->deadline_ms) {
      link = &ack->next;
      continue;
    }
    ack_finish(ack, ctx, visible > 0);
    *link = ack->next;
    nostr_event_free(ack->ingress.event);
    free(ack->old_replaceable_id);
    free(ack);
  }
}

void relayd_nip01_free_orphans(const RelaydCtx *ctx) {
  while (ctx->orphan_acks) {
    RelaydPendingAck *ack = ctx->orphan_acks;
    ((RelaydCtx*)ctx)->orphan_acks = ack->next;
    ack_finish(ack, ctx, 0);
    nostr_event_free(ack->ingress.event);
    free(ack->old_replaceable_id);
    free(ack);
  }
}

void relayd_nip01_on_timer(struct lws *wsi, ConnState *cs,
                           const RelaydCtx *ctx) {
  if (cs && ctx) ack_reconcile_connection(wsi, cs, ctx);
}

void relayd_conn_acks_free_all(ConnState *cs, const RelaydCtx *ctx) {
  if (!cs) return;
  RelaydPendingAck *ack = cs->ack_head;
  while (ack) {
    RelaydPendingAck *next = ack->next;
    if (!ack->ready && ctx && ctx->async_storage) {
      /* Commit ownership outlives the publisher. Never retain ConnState. */
      ack->next = ctx->orphan_acks;
      ((RelaydCtx*)ctx)->orphan_acks = ack;
    } else {
      nostr_event_free(ack->ingress.event);
      free(ack->old_replaceable_id);
      free(ack);
    }
    ack = next;
  }
  cs->ack_head = cs->ack_tail = NULL;
  cs->nacks = 0;
}

static int ack_enqueue(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                       RelayIngressResult *ingress, char **old_id) {
  if (cs->nacks >= RELAYD_MAX_PENDING_ACKS ||
      ctx->pending_stores >= RELAYD_MAX_PENDING_ACKS) return -EAGAIN;
  RelaydPendingAck *ack = calloc(1, sizeof *ack);
  if (!ack) return -ENOMEM;
  int rc = ctx->async_storage->enqueue(ctx->storage, ingress->event);
  if (rc != 0) { free(ack); return rc; }
  ack->ingress = *ingress;
  ingress->event = NULL; /* the ack owns it through commit and fanout */
  ((RelaydCtx*)ctx)->pending_stores++;
  ingress->replay_reserved = 0; /* reservation now belongs to the ack */
  ack->old_replaceable_id = *old_id;
  *old_id = NULL;
  unsigned int timeout = ctx->ack_timeout_ms ? ctx->ack_timeout_ms : 10000u;
  ack->deadline_ms = rate_limit_now_ms() + timeout;
  if (cs->ack_tail) cs->ack_tail->next = ack;
  else cs->ack_head = ack;
  cs->ack_tail = ack;
  cs->nacks++;
  ack_reconcile_connection(wsi, cs, ctx);
  return 0;
}

static int write_text(struct lws *wsi, const char *text) {
  if (!text) return -1;
  size_t len = strlen(text);
  unsigned char *buf = malloc(LWS_PRE + len);
  if (!buf) return -1;
  memcpy(buf + LWS_PRE, text, len);
  int written = lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
  free(buf);
  return written < 0 || (size_t)written != len ? -1 : 0;
}

static int has_backfill(const ConnState *cs) {
  for (size_t i = 0; i < cs->nsubs; i++)
    if (!cs->subs[i].eose_sent) return 1;
  return 0;
}

int relayd_nip01_on_writable(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx) {
  if (!wsi || !cs || !ctx) return -1;
  if (cs->closing) {
    lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION,
                     (unsigned char*)"delivery unavailable", 20);
    return -1;
  }
  if (relayd_nip42_maybe_send_challenge_on_writable(wsi, cs, ctx)) goto more;
  if (cs->ack_head && cs->ack_head->ready) {
    RelaydPendingAck *ack = cs->ack_head;
    char *ok = nostr_ok_build_json(ack->ingress.canonical_id, ack->accepted,
                                   ack->accepted ? "" : "error: storage commit unconfirmed");
    if (!ok) return -1;
    ws_send_text(wsi, ok);
    free(ok);
    cs->ack_head = ack->next;
    if (!cs->ack_head) cs->ack_tail = NULL;
    cs->nacks--;
    nostr_event_free(ack->ingress.event);
    free(ack->old_replaceable_id);
    free(ack);
    ack_schedule_timer(wsi, cs);
  }
  /* Live frames wait behind that subscription's catch-up/EOSE. Control
   * frames can pass them. Exactly one lws_write per writable callback. */
  if (cs->prefer_backfill && has_backfill(cs)) goto backfill;
  RelaydFrame **link = &cs->send_head;
  RelaydFrame *prev = NULL;
  while (*link) {
    RelaydFrame *frame = *link;
    int idx = frame->subid[0] ? conn_sub_find(cs, frame->subid, strlen(frame->subid)) : -1;
    if (idx >= 0 && !cs->subs[idx].eose_sent) {
      prev = frame;
      link = &frame->next;
      continue;
    }
    *link = frame->next;
    if (cs->send_tail == frame) cs->send_tail = prev;
    cs->send_count--;
    cs->send_bytes -= frame->length;
    int n = lws_write(wsi, (unsigned char*)frame->text + LWS_PRE,
                       frame->length, LWS_WRITE_TEXT);
    int failed = n < 0 || (size_t)n != frame->length;
    frame_free(frame);
    if (failed) return -1;
    cs->prefer_backfill = 1;
    goto more;
  }
backfill:
  for (size_t step = 0; step < cs->nsubs; step++) {
    size_t i = (cs->backfill_cursor + step) % cs->nsubs;
    RelaydPendingSub *ps = &cs->subs[i];
    if (ps->eose_sent) continue;
    cs->backfill_cursor = (i + 1) % cs->nsubs;
    cs->prefer_backfill = 0;
    const char *denial = vault_denial(ctx, cs);
    if ((strcmp(ctx->cfg.auth, "required") == 0 && !cs->authed) ||
        (denial && filters_include_vault(ps->filters, ps->nfilters))) {
      char *closed = nostr_closed_build_json(ps->subid, denial ? denial : "auth-required: NIP-42 required");
      int rc = write_text(wsi, closed);
      free(closed);
      conn_sub_remove(cs, ctx, i);
      if (rc) return -1;
      goto more;
    }
    NostrStorage *st = ctx->storage;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return -1;
    size_t n = 0;
    int rc = 0;
    if (ps->it) {
      n = 1;
      rc = st->vt->query_next(st, ps->it, ev, &n);
    }
    if (rc == 0 && n > 0) {
      /* The writer can commit between the previous notification and this
       * iterator read. Reconcile before sending so that a pending commit
       * visible in history has exactly one queued live copy. */
      relayd_nip01_reconcile_all(ctx);
      char *id = nostr_event_get_id(ev);
      int queued = 0;
      for (RelaydFrame *f = cs->send_head; f; f = f->next)
        if (id && strcmp(f->subid, ps->subid) == 0 &&
            strcmp(f->event_id, id) == 0) { queued = 1; break; }
      /* A commit during catch-up may also appear in the iterator. Keep
       * the queued live copy and do not deliver that id twice. */
      if (!queued && !(vault_denial(ctx, cs) && vault_kind(nostr_event_get_kind(ev)))) {
        char *json = nostr_event_serialize(ev);
        if (!json) { free(id); nostr_event_free(ev); return -1; }
        size_t need = strlen(json) + strlen(ps->subid) + 16;
        char *frame = malloc(need);
        if (!frame) { free(json); free(id); nostr_event_free(ev); return -1; }
        snprintf(frame, need, "[\"EVENT\",\"%s\",%s]", ps->subid, json);
        rc = write_text(wsi, frame);
        free(frame);
        free(json);
      }
      free(id);
      nostr_event_free(ev);
      if (rc) return -1;
      goto more;
    }
    nostr_event_free(ev);
    if (ps->it) st->vt->query_free(st, ps->it);
    ps->it = NULL;
    char *frame = rc ? nostr_closed_build_json(ps->subid, "error: query failed")
                     : nostr_eose_build_json(ps->subid);
    int failed = write_text(wsi, frame);
    free(frame);
    if (rc) conn_sub_remove(cs, ctx, i);
    else ps->eose_sent = 1;
    if (failed) return -1;
    goto more;
  }
more:
  if (cs->closing || cs->send_head || has_backfill(cs) ||
      (cs->ack_head && cs->ack_head->ready)) lws_callback_on_writable(wsi);
  return 0;
}

/* test-only ingress decision is provided in apps/relayd/src/policy_decider.c */

/* Reply for subscription `sub` (echoed in full, JSON-escaped):
 * EOSE when `closed_reason` is NULL, otherwise CLOSED with that reason. */
static void send_sub_reply(struct lws *wsi, const char *sub, size_t sub_len,
                           const char *closed_reason) {
  char *subtmp = (char*)malloc(sub_len + 1);
  if (!subtmp) return;
  memcpy(subtmp, sub, sub_len);
  subtmp[sub_len] = '\0';
  char *frame = closed_reason ? nostr_closed_build_json(subtmp, closed_reason)
                              : nostr_eose_build_json(subtmp);
  free(subtmp);
  if (frame) { ws_send_text(wsi, frame); free(frame); }
}

static void send_notice(struct lws *wsi, const char *text) {
  char *esc = nostr_escape_string(text);
  if (!esc) return;
  size_t need = strlen(esc) + 16;
  char *frame = (char*)malloc(need);
  if (frame) {
    snprintf(frame, need, "[\"NOTICE\",\"%s\"]", esc);
    ws_send_text(wsi, frame);
    free(frame);
  }
  free(esc);
}

/* NIP-01: non-empty, at most 64 chars. Quotes cannot occur (the id ends at
 * the first quote); backslashes and control characters are refused so the
 * id can be echoed verbatim in EVENT frames. */
static int subid_is_valid(const char *sub, size_t sub_len) {
  if (sub_len == 0 || sub_len > RELAYD_SUBID_MAX) return 0;
  for (size_t i = 0; i < sub_len; i++)
    if (sub[i] == '\\' || (unsigned char)sub[i] < 0x20) return 0;
  return 1;
}

static void free_filter_array(NostrFilter **arr, size_t n) {
  if (!arr) return;
  for (size_t i = 0; i < n; i++)
    if (arr[i]) nostr_filter_free(arr[i]);
  free(arr);
}

/*
 * NIP-01 REQ: every REQ gets a catch-up boundary or refusal promptly,
 * whatever storage the relay has. EOSE retains the live subscription --
 *
 *   EOSE             every stored match (possibly none) has been sent. A
 *                    relay without storage has an empty store, so it
 *                    answers EOSE at once; so does a query with no hits.
 *   CLOSED <reason>  the REQ was refused, with a NIP-01 machine-readable
 *                    prefix: "auth-required:", "invalid:", "unsupported:"
 *                    (e.g. search without a full-text index, see
 *                    protocol_nip50.c), "rate-limited:", or "error:" when
 *                    the store failed the query.
 *
 * Before this, a storage-less relay -- and a query that returned no
 * iterator -- sent nothing at all, and every client waited out its own
 * deadline on every REQ.
 */
static int vault_kind(int kind) {
  return kind == 30078 || kind == 445 || kind == 1059;
}

static const char *vault_denial(const RelaydCtx *ctx, const ConnState *cs) {
  if (!ctx->cfg.vault_enabled || cs->vault_authorized) return NULL;
  return cs->authed ? "restricted: pubkey not on vault allowlist"
                    : "auth-required: vault requires NIP-42";
}

static int filters_include_vault(NostrFilter **filters, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t kinds = nostr_filter_kinds_len(filters[i]);
    /* An ids/authors/tags-only filter can still expose protected kinds. */
    if (!kinds) return 1;
    for (size_t k = 0; k < kinds; k++)
      if (vault_kind(nostr_filter_kinds_get(filters[i], k))) return 1;
  }
  return 0;
}

static void handle_req(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                       const char *msg, size_t len, int count) {
  NostrStorage *st = ctx->storage;
  /* ["REQ", "<subid>", <filter>, ...]: the id must be the element right
   * after "REQ" -- not the first quoted string found anywhere. */
  const char *p = strchr(msg, ',');
  const char *subid = NULL; size_t sublen = 0;
  if (p) {
    const char *q1 = p + 1;
    while (*q1 == ' ' || *q1 == '\t' || *q1 == '\r' || *q1 == '\n') q1++;
    const char *q2 = *q1 == '"' ? strchr(q1 + 1, '"') : NULL;
    if (q2) { subid = q1 + 1; sublen = (size_t)(q2 - (q1 + 1)); p = strchr(q2, ','); }
    else p = NULL;
  }
  const char *filters_json = p ? p+1 : NULL;
  if (!subid) {
    /* No id to address a CLOSED to; NOTICE is NIP-01's channel for that. */
    send_notice(wsi, "invalid: REQ needs a subscription id");
    return;
  }
  const char *sub = subid;
  size_t sub_len = sublen;
  if (!subid_is_valid(sub, sub_len)) {
    send_sub_reply(wsi, sub, sub_len,
                   "invalid: subscription id must be 1-64 characters, "
                   "no backslash or control characters");
    return;
  }
  /* Replacement also cancels buffered frames from the old generation. */
  int existing = conn_sub_find(cs, sub, sub_len);
  if (!count && existing >= 0) conn_sub_remove(cs, ctx, (size_t)existing);
  if (strcmp(ctx->cfg.auth, "required") == 0 && !cs->authed) {
    send_sub_reply(wsi, sub, sub_len, "auth-required: NIP-42 required");
    return;
  }
  if (!filters_json) {
    send_sub_reply(wsi, sub, sub_len, "invalid: REQ needs at least one filter");
    return;
  }
  size_t max_pending = ctx->cfg.max_subs > 0 &&
                               (size_t)ctx->cfg.max_subs < RELAYD_MAX_PENDING_SUBS
                           ? (size_t)ctx->cfg.max_subs
                           : RELAYD_MAX_PENDING_SUBS;
  if (!count && cs->nsubs >= max_pending) {
    char reason[96];
    snprintf(reason, sizeof reason,
             "rate-limited: at most %zu active subscriptions per connection",
             max_pending);
    send_sub_reply(wsi, sub, sub_len, reason);
    return;
  }

  size_t flen = (size_t)len - (filters_json - msg);
  while (flen > 0 && (filters_json[flen-1] == '\n' || filters_json[flen-1] == '\r' || filters_json[flen-1] == ' ')) flen--;
  if (flen > 0 && filters_json[flen-1] == ']') flen--;
  char *fbuf = (char*)malloc(flen + 1);
  if (!fbuf) {
    send_sub_reply(wsi, sub, sub_len, "error: out of memory");
    return;
  }
  memcpy(fbuf, filters_json, flen); fbuf[flen] = '\0';
  NostrFilter **arr = NULL; size_t n = 0;
  int parsed = parse_filters_json_local(fbuf, &arr, &n);
  free(fbuf);
  if (parsed != 0 || n == 0 || (int)n > ctx->cfg.max_filters) {
    send_sub_reply(wsi, sub, sub_len, nostr_limits_reason_invalid_filter());
    free_filter_array(arr, n);
    return;
  }
  const char *denial = vault_denial(ctx, cs);
  if (denial && filters_include_vault(arr, n)) {
    send_sub_reply(wsi, sub, sub_len, denial);
    free_filter_array(arr, n);
    return;
  }
  if (count) {
    free_filter_array(arr, n);
    (void)relayd_handle_count(wsi, ctx, msg, len);
    return;
  }
  for (size_t i = 0; i < n; i++)
    if (arr[i]->limit > ctx->cfg.max_limit) arr[i]->limit = ctx->cfg.max_limit;

  size_t nquery = 0;
  for (size_t i = 0; i < n; i++) if (!arr[i]->limit_zero) nquery++;
  if (relayd_nip50_maybe_start_search(wsi, cs, ctx, sub, sub_len, arr, n)) {
    /* NIP-50 answered: CLOSED "unsupported: search", or a result stream
     * that on_writable finishes with EOSE. */
  } else if (!relayd_storage_can_query(st) || nquery == 0) {
    /* No storage means no stored events: the backfill is trivially done. */
    relayd_conn_sub_push(wsi, cs, sub, sub_len, NULL);
  } else {
    /* The storage vtable takes a contiguous NostrFilter array; `arr` holds
     * separately allocated filters. Shallow copies: `arr` keeps ownership,
     * so query() must not retain `filters` past its return. The top-level
     * limit argument is 0 ("derive from the filters"): each filter carries
     * its own limit, already clamped to max_limit above. */
    NostrFilter *flat = (NostrFilter*)calloc(nquery, sizeof(NostrFilter));
    if (!flat) {
      send_sub_reply(wsi, sub, sub_len, "error: out of memory");
    } else {
      size_t j = 0;
      for (size_t i = 0; i < n; i++)
        if (!arr[i]->limit_zero) flat[j++] = *arr[i];
      int err = 0;
      void *it = st->vt->query(st, flat, nquery, 0, 0, 0, &err);
      free(flat);
      if (err) {
        if (it) st->vt->query_free(st, it);
        send_sub_reply(wsi, sub, sub_len, "error: query failed");
      } else {
        relayd_conn_sub_push(wsi, cs, sub, sub_len, it);
      }
    }
  }
  int idx = conn_sub_find(cs, sub, sub_len);
  if (idx >= 0) {
    cs->subs[idx].filters = arr;
    cs->subs[idx].nfilters = n;
  } else {
    free_filter_array(arr, n);
  }
}

/* Replay reservations cover concurrent ingress; storage covers duplicates
 * that predate the process/replay-cache lifetime. */
static int stored_event_exists(const RelaydCtx *ctx, const RelayIngressResult *ingress) {
  if (ctx->async_storage)
    return ctx->async_storage->visible(ctx->storage, ingress->binary_id);
  NostrStorage *st = ctx->storage;
  if (!relayd_storage_can_query(st)) return 0;
  NostrFilter *filter = nostr_filter_new();
  NostrEvent *ev = nostr_event_new();
  if (!filter || !ev) { nostr_filter_free(filter); nostr_event_free(ev); return -1; }
  nostr_filter_add_id(filter, ingress->canonical_id);
  int err = 0;
  void *it = st->vt->query(st, filter, 1, 1, 0, 0, &err);
  int exists = err ? -1 : 0;
  if (it) {
    size_t n = 1;
    int rc = st->vt->query_next(st, it, ev, &n);
    char *id = rc == 0 && n ? nostr_event_get_id(ev) : NULL;
    exists = rc ? -1 : (id && strcmp(id, ingress->canonical_id) == 0);
    free(id);
    st->vt->query_free(st, it);
  }
  nostr_filter_free(filter);
  nostr_event_free(ev);
  return exists;
}

void relayd_nip01_on_receive(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const void *in, size_t len) {
  if (!wsi || !cs || !ctx || !in || len < 2) return;
  const char *msg = (const char*)in;
  uint32_t operation_cost = frame_operation_cost(ctx, msg, len);
  if (operation_cost > 0 &&
      !rate_limit_bucket_allow(&cs->frame_rate, rate_limit_now_ms(),
                               operation_cost)) {
    metrics_on_rate_limit_drop();
    if ((len >= 6 && memcmp(msg, "[\"REQ\"", 6) == 0) ||
        (len >= 8 && memcmp(msg, "[\"COUNT\"", 8) == 0)) {
      const char *p = strchr(msg, ',');
      const char *q1 = p ? strchr(p + 1, '"') : NULL;
      const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
      char subtmp[128] = "sub1";
      if (q1 && q2 && (size_t)(q2 - (q1 + 1)) < sizeof(subtmp)) {
        memcpy(subtmp, q1 + 1, (size_t)(q2 - (q1 + 1)));
        subtmp[(size_t)(q2 - (q1 + 1))] = '\0';
      }
      if (len >= 6 && memcmp(msg, "[\"REQ\"", 6) == 0) {
        int idx = conn_sub_find(cs, subtmp, strlen(subtmp));
        if (idx >= 0) conn_sub_remove(cs, ctx, (size_t)idx);
      }
      char *closed = nostr_closed_build_json(subtmp, "rate-limited");
      if (closed) { ws_send_text(wsi, closed); free(closed); }
    } else if (len >= 7 && memcmp(msg, "[\"EVENT\"", 7) == 0) {
      char *ok = nostr_ok_build_json("0000", 0, "rate-limited");
      if (ok) { ws_send_text(wsi, ok); free(ok); }
    }
    return;
  }
  if (relayd_nip42_handle_auth_frame(wsi, cs, ctx, msg, len)) return;
  NostrStorage *st = ctx->storage;
  if (len >= 8 && memcmp(msg, "[\"COUNT\"", 8) == 0) {
    handle_req(wsi, cs, ctx, msg, len, 1);
    return;
  }
  if (len >= 7 && memcmp(msg, "[\"EVENT\"", 7) == 0) {
    if (strcmp(ctx->cfg.auth, "required") == 0 && !cs->authed) {
      char *ok = nostr_ok_build_json("0000", 0, "auth-required: NIP-42 required");
      if (ok) { ws_send_text(wsi, ok); free(ok); }
      return;
    }
    const char *ev_json = strchr(msg, ',');
    if (!ev_json) return;
    ev_json++;
    size_t elen = (size_t)len - (ev_json - msg);
    while (elen > 0 && (ev_json[elen-1] == '\n' || ev_json[elen-1] == '\r' || ev_json[elen-1] == ' ')) elen--;
    if (elen > 0 && ev_json[elen-1] == ']') elen--;
    if (elen == 0 || elen > (size_t)ctx->cfg.max_event_bytes) {
      metrics_on_oversize_reject();
      char *ok = nostr_ok_build_json("0000", 0, "invalid: event too large");
      if (ok) { ws_send_text(wsi, ok); free(ok); }
      return;
    }
    char *ebuf = (char*)malloc(elen + 1);
    if (!ebuf) return;
    memcpy(ebuf, ev_json, elen); ebuf[elen] = '\0';
    RelayIngressConfig ingress_cfg = {
      .max_event_bytes = (size_t)ctx->cfg.max_event_bytes,
      .verification_cost = (uint32_t)ctx->cfg.verification_cost
    };
    RelayIngressResult ingress;
    RelayIngressDecision decision = relay_ingress_validate_and_reserve(
        &ingress_cfg, ctx->policy, ctx->verification_budget,
        &cs->verification_rate, cs->peer_ip, ebuf, elen, time(NULL),
        rate_limit_now_ms(), &ingress);

    int rc_store = -1;
    int deferred = 0;
    const char *reason = ingress.reason;
    const char *denial = ingress.event && vault_kind(nostr_event_get_kind(ingress.event))
                             ? vault_denial(ctx, cs) : NULL;
    if (denial) {
      reason = denial;
      (void)relay_ingress_finish(ctx->policy, &ingress, 0);
    } else if (ingress.event && nostr_event_get_kind(ingress.event) == 22242) {
      reason = "invalid: use AUTH for kind 22242";
      (void)relay_ingress_finish(ctx->policy, &ingress, 0);
    } else if (decision == RELAY_INGRESS_DUPLICATE) {
      rc_store = 0;
      metrics_on_duplicate_drop();
    } else if (decision != RELAY_INGRESS_ACCEPT) {
      if (decision == RELAY_INGRESS_REJECT_SKEW) metrics_on_skew_reject();
      else if (decision == RELAY_INGRESS_REJECT_VERIFY_BUDGET)
        metrics_on_verification_budget_drop();
      else if (decision == RELAY_INGRESS_REJECT_OVERSIZE)
        metrics_on_oversize_reject();
      else
        metrics_on_validation_reject();
    } else if (!st || !st->vt || !st->vt->put_event) {
      reason = "error: store unavailable";
      (void)relay_ingress_finish(ctx->policy, &ingress, 0);
    } else {
      int exists = stored_event_exists(ctx, &ingress);
      if (exists < 0 || (exists > 0 && ctx->async_storage)) {
        rc_store = exists > 0 ? 0 : -1;
        reason = exists > 0 ? "duplicate: already stored" : "error: duplicate lookup failed";
        (void)relay_ingress_finish(ctx->policy, &ingress, exists > 0);
      } else {
      NostrEvent *ev = ingress.event;
      const char *epk = nostr_event_get_pubkey(ev);
      int kind = nostr_event_get_kind(ev);
      if (strcmp(ctx->cfg.auth, "required") == 0 &&
          cs->authed_pubkey[0] &&
          (!epk || strcmp(epk, cs->authed_pubkey) != 0)) {
        reason = "auth-pubkey-mismatch";
        (void)relay_ingress_finish(ctx->policy, &ingress, 0);
      } else {
        char *old_replaceable_id = NULL;
        int replacement_is_stale = 0;
        if (epk && (is_replaceable_kind(kind) || is_param_replaceable_kind(kind))) {
          NostrFilter *ff = nostr_filter_new();
          if (ff) {
            nostr_filter_add_author(ff, epk);
            nostr_filter_add_kind(ff, kind);
            if (is_param_replaceable_kind(kind)) {
              NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
              const char *dval = tags ? nostr_tags_get_d(tags) : NULL;
              nostr_filter_tags_append(ff, "d", dval ? dval : "", NULL);
            }
            /* vt->query takes an array of NostrFilter structs: pass the
             * one filter itself (not an array of pointers to it). */
            int err = 0; void *it = st->vt->query(st, ff, 1, 1, 0, 0, &err);
            if (it) {
              NostrEvent *prev = nostr_event_new(); size_t n1 = 1;
              if (prev && st->vt->query_next &&
                  st->vt->query_next(st, it, prev, &n1) == 0 && n1 > 0) {
                if (nostr_event_get_created_at(prev) >=
                    nostr_event_get_created_at(ev))
                  replacement_is_stale = 1;
                else
                  old_replaceable_id = nostr_event_get_id(prev);
              }
              nostr_event_free(prev); /* query_next deserializes into it */
              if (st->vt->query_free) st->vt->query_free(st, it);
            }
            nostr_filter_free(ff);
          }
        }
        if (replacement_is_stale) {
          reason = "invalid: newer replaceable event exists";
          (void)relay_ingress_finish(ctx->policy, &ingress, 0);
        } else {
          if (ctx->async_storage) {
            rc_store = ack_enqueue(wsi, cs, ctx, &ingress,
                                   &old_replaceable_id);
            deferred = rc_store == 0;
            reason = rc_store == -EAGAIN ? "rate-limited: too many pending events"
                                         : "error: store enqueue failed";
            if (!deferred)
              (void)relay_ingress_finish(ctx->policy, &ingress, 0);
          } else {
            rc_store = st->vt->put_event(st, ev);
            reason = rc_store == 0 ? "" : "error: store failed";
            (void)relay_ingress_finish(ctx->policy, &ingress, rc_store == 0);
            /* Synchronous wrappers may have additional durable effects
             * (session outbox). Still call put_event for existing ids, but
             * never redeliver them as newly stored events. */
            if (rc_store == 0 && !exists) fanout(ctx, ev);
            if (rc_store == 0 && old_replaceable_id &&
                st->vt->delete_event)
              (void)st->vt->delete_event(st, old_replaceable_id);
          }
        }
        free(old_replaceable_id);
      }
      }
    }

    if (!deferred) {
      const char *id_hex = ingress.canonical_id[0] ? ingress.canonical_id : "0000";
      char *ok = nostr_ok_build_json(id_hex, rc_store == 0, reason ? reason : "");
      if (ok) { ws_send_text(wsi, ok); free(ok); }
    }
    relay_ingress_result_clear(&ingress);
    free(ebuf);
    return;
  }
  if (len >= 7 && memcmp(msg, "[\"REQ\"", 6) == 0) {
    handle_req(wsi, cs, ctx, msg, len, 0);
    return;
  }
  if (len >= 8 && memcmp(msg, "[\"CLOSE\"", 8) == 0) {
    const char *q1 = strchr(msg, '"');
    const char *q2 = q1 ? strchr(q1+1, '"') : NULL;
    const char *q3 = q2 ? strchr(q2+1, '"') : NULL;
    const char *q4 = q3 ? strchr(q3+1, '"') : NULL;
    if (q3 && q4 && q4 > q3+1) {
      size_t sl = (size_t)(q4 - (q3+1));
      int idx = conn_sub_find(cs, q3 + 1, sl);
      if (idx >= 0) conn_sub_remove(cs, ctx, (size_t)idx);
    }
    return;
  }
}
