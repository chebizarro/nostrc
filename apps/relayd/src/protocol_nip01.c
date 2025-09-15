#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <libwebsockets.h>
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"
#include "nostr-relay-limits.h"
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "protocol_nip01.h"
#include "protocol_nip42.h"
#include "protocol_nip50.h"
#include "rate_limit.h"
#include "protocol_nip77.h"
#include "metrics.h"

static void ws_send_text(struct lws *wsi, const char *s) {
  if (!wsi || !s) return;
  size_t blen = strlen(s);
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen);
  if (!buf) return;
  memcpy(&buf[LWS_PRE], s, blen);
  lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
  free(buf);
}

static inline int is_replaceable_kind(int kind) { return kind == 0 || kind == 3 || kind == 41; }
static inline int is_param_replaceable_kind(int kind) { return kind >= 30000 && kind < 40000; }

/* === Replay cache (simple fixed-size ring with TTL) === */
#define SEEN_ID_CAPACITY 65536
static int g_seen_id_ttl_seconds = 0; /* default disabled; set via env NOSTR_RELAY_REPLAY_TTL */
typedef struct { char id[65]; time_t seen_at; } SeenIdEntry;
static SeenIdEntry g_seen_ids[SEEN_ID_CAPACITY];
static size_t g_seen_cursor = 0;

static inline int ids_equal64(const char *a, const char *b) {
  for (int i = 0; i < 64; i++) if (a[i] != b[i]) return 0; return 1;
}

static int seen_ids_check_and_add(const char *id_hex, time_t now) {
  size_t pos = 0;
  if (!id_hex || id_hex[0] == '\0') return 0;
  /* Fast scan a small window around the ring head for recent entries; full scan is expensive.
     Trade-off: scan 1024 recent entries which should capture burst duplicates. */
  size_t scan = SEEN_ID_CAPACITY < 1024 ? SEEN_ID_CAPACITY : 1024;
  if (g_seen_id_ttl_seconds <= 0) {
    /* Disabled */
    goto insert;
  }
  for (size_t i = 0; i < scan; i++) {
    size_t idx = (g_seen_cursor + SEEN_ID_CAPACITY - 1 - i) % SEEN_ID_CAPACITY;
    if (g_seen_ids[idx].id[0] && (now - g_seen_ids[idx].seen_at) <= g_seen_id_ttl_seconds) {
      if (ids_equal64(g_seen_ids[idx].id, id_hex)) return 1; /* duplicate */
    }
  }
insert:
  /* Insert at cursor position (evict old) */
  pos = g_seen_cursor % SEEN_ID_CAPACITY;
  for (int i = 0; i < 64; i++) g_seen_ids[pos].id[i] = id_hex[i] ? id_hex[i] : '0';
  g_seen_ids[pos].id[64] = '\0';
  g_seen_ids[pos].seen_at = now;
  g_seen_cursor = (g_seen_cursor + 1) % SEEN_ID_CAPACITY;
  return 0;
}

/* === Timestamp skew policy (disabled by default; set via env) === */
static int g_future_skew_seconds = 0;
static int g_past_skew_seconds = 0;

/* Expose runtime configuration setters */
void nostr_relay_set_replay_ttl(int seconds) {
  g_seen_id_ttl_seconds = seconds > 0 ? seconds : 0;
}
void nostr_relay_set_skew(int future_seconds, int past_seconds) {
  g_future_skew_seconds = future_seconds > 0 ? future_seconds : 0;
  g_past_skew_seconds = past_seconds > 0 ? past_seconds : 0;
}

/* Expose getters for tests */
int nostr_relay_get_replay_ttl(void) { return g_seen_id_ttl_seconds; }
void nostr_relay_get_skew(int *future_seconds, int *past_seconds) {
  if (future_seconds) *future_seconds = g_future_skew_seconds;
  if (past_seconds) *past_seconds = g_past_skew_seconds;
}

/* Local helper: parse array or single filter into array */
static int parse_filters_json_local(const char *json, NostrFilter ***out_arr, size_t *out_n, int max_filters) {
  if (!out_arr || !out_n || !json) return -1;
  *out_arr = NULL; *out_n = 0;
  const char *p = json;
  while (*p && *p != '{' && *p != '[') p++;
  if (*p == '{') {
    NostrFilter *f = (NostrFilter*)calloc(1, sizeof(NostrFilter));
    if (!f) return -1;
    if (nostr_filter_deserialize(f, p) != 0) { free(f); return -1; }
    *out_arr = (NostrFilter**)calloc(1, sizeof(NostrFilter*));
    if (!*out_arr) { nostr_filter_free(f); return -1; }
    (*out_arr)[0] = f; *out_n = 1; return 0;
  } else if (*p == '[') {
    size_t cap = (size_t)(max_filters > 0 ? max_filters : 16);
    NostrFilter **arr = (NostrFilter**)calloc(cap, sizeof(NostrFilter*));
    if (!arr) return -1;
    size_t n = 0; const char *q = p+1; int depth = 1;
    while (*q && depth > 0) {
      if (*q == '{' && depth == 1) {
        if (max_filters > 0 && (int)n >= max_filters) break;
        NostrFilter *f = (NostrFilter*)calloc(1, sizeof(NostrFilter));
        if (!f) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); return -1; }
        if (nostr_filter_deserialize(f, q) != 0) { nostr_filter_free(f); for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); return -1; }
        if (n == cap) {
          size_t ncap = cap * 2; NostrFilter **tmp = (NostrFilter**)realloc(arr, ncap * sizeof(NostrFilter*));
          if (!tmp) { nostr_filter_free(f); for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); return -1; }
          arr = tmp; cap = ncap;
        }
        arr[n++] = f;
        int obj = 1; q++;
        while (*q && obj > 0) { if (*q == '{') obj++; else if (*q == '}') obj--; q++; }
        continue;
      } else if (*q == '[') { depth++; }
      else if (*q == ']') { depth--; }
      q++;
    }
    *out_arr = arr; *out_n = n; return 0;
  }
  return -1;
}

void relayd_nip01_on_writable(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx) {
  if (!wsi || !cs) return;
  NostrStorage *st = ctx ? ctx->storage : NULL;
  if (relayd_nip42_maybe_send_challenge_on_writable(wsi, cs, ctx)) return;
  if (!st || !st->vt || !st->vt->query_next || !cs->it) return;
  int sent_any = 0;
  for (int i = 0; i < 8; i++) {
    size_t n = 1; NostrEvent ev = {0};
    int rc = st->vt->query_next(st, cs->it, &ev, &n);
    if (!(rc == 0 && n > 0)) break;
    char *ejson = nostr_event_serialize_compact(&ev);
    if (!ejson) ejson = nostr_event_serialize(&ev);
    if (ejson) {
      size_t need = strlen(ejson) + strlen(cs->subid) + 64;
      unsigned char *buf = (unsigned char*)malloc(LWS_PRE + need);
      if (buf) {
        int m = snprintf((char*)&buf[LWS_PRE], need, "[\"EVENT\",\"%s\",%s]", cs->subid, ejson);
        if (m > 0) { lws_write(wsi, &buf[LWS_PRE], (size_t)m, LWS_WRITE_TEXT); sent_any = 1; }
        free(buf);
      }
      free(ejson);
    }
  }
  if (sent_any) {
    lws_callback_on_writable(wsi);
  } else {
    if (cs->subid[0]) {
      char *eose = nostr_eose_build_json(cs->subid);
      if (eose) { ws_send_text(wsi, eose); free(eose); }
    }
    if (st && st->vt && st->vt->query_free) st->vt->query_free(st, cs->it);
    cs->it = NULL; cs->subid[0] = '\0';
  }
}

/* test-only ingress decision is provided in apps/relayd/src/policy_decider.c */

void relayd_nip01_on_receive(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const void *in, size_t len) {
  if (!wsi || !ctx || !in || len < 2) return;
  const char *msg = (const char*)in;
  /* Delegate AUTH frame to NIP-42 */
  if (relayd_nip42_handle_auth_frame(wsi, cs, ctx, msg, len)) return;
  /* Rate limit for non-CLOSE */
  if (len >= 8 && memcmp(msg, "[\"CLOSE\"", 8) != 0) {
    uint64_t now_ms = rate_limit_now_ms();
    if (!rate_limit_allow(cs, now_ms)) {
      metrics_on_rate_limit_drop();
      /* Optional: send CLOSED with reason 'rate-limited' for REQ */
      if (len >= 7 && memcmp(msg, "[\"REQ\"", 6) == 0) {
        const char *p = strchr(msg, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
        char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
        char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"sub1", "rate-limited");
        if (closed) { ws_send_text(wsi, closed); free(closed);}                
      }
      return;
    }
  }
  NostrStorage *st = ctx->storage;
  if (len >= 7 && memcmp(msg, "[\"EVENT\"", 7) == 0) {
    if (strcmp(ctx->cfg.auth, "required") == 0 && cs && !cs->authed) {
      char *ok = nostr_ok_build_json("0000", 0, "auth-required");
      if (ok) { ws_send_text(wsi, ok); free(ok);}          
      return;
    }
    const char *ev_json = strchr(msg, ',');
    if (!ev_json) return;
    ev_json++;
    size_t elen = (size_t)len - (ev_json - msg);
    while (elen > 0 && (ev_json[elen-1] == '\n' || ev_json[elen-1] == '\r' || ev_json[elen-1] == ' ')) elen--;
    if (elen > 0 && ev_json[elen-1] == ']') elen--;
    char *ebuf = (char*)malloc(elen + 1);
    if (!ebuf) return;
    memcpy(ebuf, ev_json, elen); ebuf[elen] = '\0';
    NostrEvent *ev = nostr_event_new(); int ok_parse = 0;
    if (ev) {
      if (nostr_event_deserialize_compact(ev, ebuf)) ok_parse = 1;
      else ok_parse = (nostr_event_deserialize(ev, ebuf) == 0);
    }
    int rc_store = -1; const char *reason = NULL; char *id = NULL;
    if (ok_parse && ev && st && st->vt && st->vt->put_event) {
      /* created_at skew check (optional) */
      time_t now = time(NULL);
      int64_t created_at = nostr_event_get_created_at(ev);
      if (g_future_skew_seconds > 0 || g_past_skew_seconds > 0) {
        if (created_at > 0 &&
            (((g_future_skew_seconds > 0) && ((created_at - (int64_t)now) > g_future_skew_seconds)) ||
             ((g_past_skew_seconds > 0) && (((int64_t)now - created_at) > g_past_skew_seconds)))) {
          reason = "invalid: created_at out of range";
          rc_store = -1;
          metrics_on_skew_reject();
          goto respond_ok;
        }
      }
      if (strcmp(ctx->cfg.auth, "required") == 0 && cs && cs->authed_pubkey[0]) {
        const char *epk = nostr_event_get_pubkey(ev);
        if (!epk || strcmp(epk, cs->authed_pubkey) != 0) {
          char *ok = nostr_ok_build_json("0000", 0, "auth-pubkey-mismatch");
          if (ok) { ws_send_text(wsi, ok); free(ok);}          
          nostr_event_free(ev); free(ebuf); return;
        }
      }
      const char *epk = nostr_event_get_pubkey(ev);
      int kind = nostr_event_get_kind(ev);
      if (!nostr_event_check_signature(ev)) {
        reason = "invalid: bad signature";
      } else {
        /* Recompute canonical id and check replay cache (optional) */
        id = nostr_event_get_id(ev);
        if (id && strlen(id) == 64 && g_seen_id_ttl_seconds > 0) {
          if (seen_ids_check_and_add(id, now)) {
            /* Duplicate within TTL: accept but do not store */
            rc_store = 0;
            reason = "duplicate";
            metrics_on_duplicate_drop();
            goto respond_ok;
          }
        }
        if (epk && (is_replaceable_kind(kind) || is_param_replaceable_kind(kind))) {
          NostrFilter *ff = nostr_filter_new();
          if (ff) {
            nostr_filter_add_author(ff, epk);
            nostr_filter_add_kind(ff, kind);
            if (is_param_replaceable_kind(kind)) {
              NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
              const char *dval = tags ? nostr_tags_get_d(tags) : NULL;
              if (dval && *dval) {
                nostr_filter_tags_append(ff, "d", dval, NULL);
              }
            }
            NostrFilter *farr[1] = { ff };
            int err = 0; void *it = st->vt->query(st, (const NostrFilter*)farr, 1, 1, 0, 0, &err);
            if (it) {
              NostrEvent prev = {0}; size_t n1 = 1;
              if (st->vt->query_next && st->vt->query_next(st, it, &prev, &n1) == 0 && n1 > 0) {
                char *old_id = nostr_event_get_id(&prev);
                if (old_id && st->vt->delete_event) (void)st->vt->delete_event(st, old_id);
                if (old_id) free(old_id);
              }
              if (st->vt->query_free) st->vt->query_free(st, it);
            }
            nostr_filter_free(ff);
          }
        }
        rc_store = st->vt->put_event(st, ev);
        reason = rc_store == 0 ? "" : "error: store failed";
      }
    } else {
      reason = "invalid: bad event";
    }
respond_ok:
    {
      const char *id_hex = id ? id : "0000";
      char *ok = nostr_ok_build_json(id_hex, rc_store == 0, reason);
    if (ok) { ws_send_text(wsi, ok); free(ok); }
    }
    if (id) free(id);
    if (ev) nostr_event_free(ev);
    free(ebuf);
    return;
  }
  if (len >= 7 && memcmp(msg, "[\"REQ\"", 6) == 0) {
    if (strcmp(ctx->cfg.auth, "required") == 0 && cs && !cs->authed) {
      const char *p = strchr(msg, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
      char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
      char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"sub1", "auth-required");
      if (closed) { ws_send_text(wsi, closed); free(closed);}          
      return;
    }
    const char *p = strchr(msg, ',');
    const char *subid = NULL; size_t sublen = 0;
    if (p) { const char *q1 = strchr(p+1, '"'); if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2) { subid = q1+1; sublen = (size_t)(q2 - (q1+1)); p = strchr(q2, ','); } } }
    const char *filters_json = p ? p+1 : NULL;
    const char *sub = subid ? subid : "sub1";
    size_t sub_len = subid ? sublen : strlen("sub1");
    if (!filters_json) return;
    if (ctx->cfg.max_subs <= 1 && cs && cs->it) {
      char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
      char *closed = nostr_closed_build_json(subtmp, "too-many-subs");
      if (closed) { ws_send_text(wsi, closed); free(closed);}          
      return;
    }
    size_t flen = (size_t)len - (filters_json - msg);
    while (flen > 0 && (filters_json[flen-1] == '\n' || filters_json[flen-1] == '\r' || filters_json[flen-1] == ' ')) flen--;
    if (flen > 0 && filters_json[flen-1] == ']') flen--;
    char *fbuf = (char*)malloc(flen + 1);
    if (!fbuf) return;
    memcpy(fbuf, filters_json, flen); fbuf[flen] = '\0';
    NostrFilter **arr = NULL; size_t n = 0;
    if (parse_filters_json_local(fbuf, &arr, &n, ctx->cfg.max_filters) != 0) {
      char *closed = nostr_closed_build_json(sub, nostr_limits_reason_invalid_filter());
      if (closed) { ws_send_text(wsi, closed); free(closed);}                
      free(fbuf);
      return;
    }
    if ((int)n > ctx->cfg.max_filters) {
      char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
      char *closed = nostr_closed_build_json(subtmp, nostr_limits_reason_invalid_filter());
      if (closed) { ws_send_text(wsi, closed); free(closed);}                
      if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr);} free(fbuf);
      return;
    }
    for (size_t i=0;i<n;i++) if (arr[i] && arr[i]->limit > ctx->cfg.max_limit) arr[i]->limit = ctx->cfg.max_limit;
    if (relayd_nip50_maybe_start_search(wsi, cs, ctx, sub, sub_len, arr, n)) {
      /* handled by NIP-50 module */
    } else if (st && st->vt && st->vt->query) {
      int err = 0; void *it = st->vt->query(st, (const NostrFilter*)(arr ? arr[0] : NULL), n, 0, 0, 0, &err);
      if (it) {
        cs->it = it; size_t cplen = sub_len < sizeof(cs->subid)-1 ? sub_len : sizeof(cs->subid)-1; memcpy(cs->subid, sub, cplen); cs->subid[cplen] = '\0'; lws_callback_on_writable(wsi);
        metrics_on_sub_start();
      }
    }
    if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); }
    free(fbuf);
    return;
  }
  if (len >= 8 && memcmp(msg, "[\"CLOSE\"", 8) == 0) {
    const char *q1 = strchr(msg, '"');
    const char *q2 = q1 ? strchr(q1+1, '"') : NULL;
    const char *q3 = q2 ? strchr(q2+1, '"') : NULL;
    const char *q4 = q3 ? strchr(q3+1, '"') : NULL;
    if (q3 && q4 && q4 > q3+1) {
      size_t sl = (size_t)(q4 - (q3+1));
      char subbuf[128]; size_t cplen = sl < sizeof(subbuf)-1 ? sl : sizeof(subbuf)-1; memcpy(subbuf, q3+1, cplen); subbuf[cplen] = '\0';
      NostrStorage *st2 = ctx->storage;
      if (cs && cs->it && cs->subid[0] && strncmp(cs->subid, subbuf, sizeof(cs->subid)) == 0) {
        if (st2 && st2->vt && st2->vt->query_free) st2->vt->query_free(st2, cs->it);
        cs->it = NULL; cs->subid[0] = '\0';
        metrics_on_sub_end();
      }
    }
    return;
  }
}
