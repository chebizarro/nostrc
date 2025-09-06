#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr-json.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"

/* Minimal GApplication with libsoup WebSocket server scaffold.
 * TODO: bridge to NostrStorage, implement NIP-01/11/42/45/50 handlers.
 */

typedef struct {
  GApplication parent_instance;
  SoupServer *server;
  guint port;
  /* config */
  char storage_driver[64];
  char name[128];
  char software[128];
  char version[64];
  int max_filters;
  int max_limit;
  int max_subs;
  /* supported NIPs as a raw JSON array string for simplicity */
  char supported_nips[256];
  char auth[32]; /* off|optional|required */
  char contact[256];
  char description[512];
  char icon[256];
  char posting_policy[256];
  /* rate limiting */
  int rate_ops_per_sec;
  int rate_burst;
  /* backpressure */
  int backpressure_max_ticks;
  /* storage */
  NostrStorage *storage;
} GRelayApp;

typedef GApplicationClass GRelayAppClass;

G_DEFINE_TYPE(GRelayApp, g_relay_app, G_TYPE_APPLICATION)

typedef struct {
  GRelayApp *app;
  SoupWebsocketConnection *conn;
  void *it;
  char subid[128];
  guint idle_id;
  int authed;
  char auth_chal[64];
  char authed_pubkey[128];
  /* token bucket */
  double rl_tokens;
  guint64 rl_last_ms;
  /* backpressure */
  unsigned int no_progress_ticks;
} GRelayConnState;

/* simple metrics */
static struct {
  guint64 connections_current;
  guint64 connections_total;
  guint64 connections_closed;
  guint64 subs_current;
  guint64 subs_started;
  guint64 subs_ended;
  guint64 events_streamed;
  guint64 rate_limit_drops;
  guint64 backpressure_drops;
  guint64 eose_sent;
} Gm;

static void grelay_on_ws_closed(SoupWebsocketConnection *c, gpointer u) {
  (void)c; (void)u;
  if (Gm.connections_current > 0) Gm.connections_current--;
  Gm.connections_closed++;
}

static void grelay_state_free(gpointer data) {
  GRelayConnState *st = (GRelayConnState*)data;
  if (!st) return;
  if (st->idle_id) g_source_remove(st->idle_id);
  if (st->it && st->app && st->app->storage && st->app->storage->vt && st->app->storage->vt->query_free)
    st->app->storage->vt->query_free(st->app->storage, st->it);
  g_free(st);
}

static inline guint64 grelay_now_ms(void) {
  return (guint64)(g_get_monotonic_time() / 1000);
}

static gboolean grelay_rate_allow(GRelayApp *app, GRelayConnState *st) {
  if (!app || !st) return TRUE;
  if (app->rate_ops_per_sec <= 0) return TRUE;
  guint64 now = grelay_now_ms();
  if (st->rl_last_ms == 0) st->rl_last_ms = now;
  double capacity = (double)(app->rate_burst > 0 ? app->rate_burst : app->rate_ops_per_sec * 2);
  double tokens = st->rl_tokens;
  double refill = ((double)(now - st->rl_last_ms)) * ((double)app->rate_ops_per_sec) / 1000.0;
  tokens += refill;
  if (tokens > capacity) tokens = capacity;
  if (tokens >= 1.0) { tokens -= 1.0; st->rl_tokens = tokens; st->rl_last_ms = now; return TRUE; }
  st->rl_tokens = tokens; st->rl_last_ms = now; return FALSE;
}

static GRelayConnState* grelay_get_state(GRelayApp *app, SoupWebsocketConnection *conn) {
  GRelayConnState *st = (GRelayConnState*)g_object_get_data(G_OBJECT(conn), "grelay-state");
  if (!st) {
    st = g_new0(GRelayConnState, 1);
    st->app = app;
    st->conn = conn;
    g_object_set_data_full(G_OBJECT(conn), "grelay-state", st, grelay_state_free);
  }
  return st;
}

static gboolean grelay_stream_tick(gpointer user_data) {
  GRelayConnState *st = (GRelayConnState*)user_data;
  if (!st || !st->app || !st->app->storage || !st->app->storage->vt || !st->app->storage->vt->query_next || !st->it) return FALSE;
  gboolean sent_any = FALSE;
  for (int i=0; i<8; i++) {
    NostrEvent ev={0}; size_t one=1; if (st->app->storage->vt->query_next(st->app->storage, st->it, &ev, &one) != 0 || one==0) break;
    char *ejson = nostr_event_serialize_compact(&ev); if (!ejson) ejson = nostr_event_serialize(&ev);
    if (ejson) {
      gsize need = strlen(ejson) + strlen(st->subid) + 32; char *frame = g_malloc(need);
      if (frame) { g_snprintf(frame, need, "[\"EVENT\",\"%s\",%s]", st->subid, ejson); soup_websocket_connection_send_text(st->conn, frame); g_free(frame);} 
      g_free(ejson);
      sent_any = TRUE;
      Gm.events_streamed++;
    }
  }
  if (!sent_any) {
    /* backpressure: if we repeatedly cannot make progress, drop */
    st->no_progress_ticks++;
    int max_ticks = st->app ? st->app->backpressure_max_ticks : 0;
    if (max_ticks > 0 && (int)st->no_progress_ticks >= max_ticks) {
      char *closed = nostr_closed_build_json(st->subid[0]?st->subid:"sub1", "backpressure");
      if (closed) { soup_websocket_connection_send_text(st->conn, closed); g_free(closed); }
      if (st->it && st->app->storage->vt->query_free) st->app->storage->vt->query_free(st->app->storage, st->it);
      st->it = NULL; st->idle_id = 0; st->subid[0] = '\0'; st->no_progress_ticks = 0;
      if (Gm.subs_current > 0) Gm.subs_current--; Gm.subs_ended++; Gm.backpressure_drops++;
      return FALSE;
    }
    /* send EOSE and cleanup */
    char *eose = nostr_eose_build_json(st->subid);
    if (eose) { soup_websocket_connection_send_text(st->conn, eose); g_free(eose); }
    if (st->it && st->app->storage->vt->query_free) st->app->storage->vt->query_free(st->app->storage, st->it);
    st->it = NULL; st->idle_id = 0; st->subid[0] = '\0'; st->no_progress_ticks = 0;
    if (Gm.subs_current > 0) Gm.subs_current--; Gm.subs_ended++;
    Gm.eose_sent++;
    return FALSE;
  }
  st->no_progress_ticks = 0;
  return TRUE; /* keep streaming */
}

static void on_ws_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer user_data) {
  GRelayApp *app = (GRelayApp*)user_data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
  gsize len = 0; const char *data = g_bytes_get_data(message, &len);
  if (!data || len < 2) return;
  GRelayConnState *st = grelay_get_state(app, conn);
  /* Rate limit all non-CLOSE/AUTH frames */
  if (!(len >= 7 && (g_str_has_prefix(data, "[\"AUTH\"") || g_str_has_prefix(data, "[\"CLOSE\"")))) {
    if (!grelay_rate_allow(app, st)) {
      Gm.rate_limit_drops++;
      /* Try to extract subid where applicable for REQ/COUNT to send CLOSED */
      if (len >= 7 && g_str_has_prefix(data, "[\"REQ\"")) {
        const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
        char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
        char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"sub1", "rate-limited"); if (closed) { soup_websocket_connection_send_text(conn, closed); g_free(closed);} 
      } else if (len >= 8 && g_str_has_prefix(data, "[\"COUNT\"")) {
        const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
        char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
        char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"count", "rate-limited"); if (closed) { soup_websocket_connection_send_text(conn, closed); g_free(closed);} 
      } else if (len >= 7 && g_str_has_prefix(data, "[\"EVENT\"")) {
        char *ok = nostr_ok_build_json("0000", 0, "rate-limited"); if (ok) { soup_websocket_connection_send_text(conn, ok); g_free(ok);} 
      }
      return;
    }
  }
  /* NIP-42 AUTH frame */
  if (len >= 7 && g_str_has_prefix(data, "[\"AUTH\"")) {
    const char *p = strchr(data, ','); if (!p) return; p++;
    while (len>0 && (data[len-1]=='\n'||data[len-1]=='\r'||data[len-1]==' ')) len--;
    if (len>0 && data[len-1] == ']') len--;
    if (p < data + len && *p == '{') {
      size_t elen = (size_t)((data + len) - p);
      char *json = g_strndup(p, (gssize)elen);
      if (json) {
        NostrEvent *ev = nostr_event_new();
        gboolean ok = FALSE;
        if (nostr_event_deserialize_compact(ev, json)) ok=TRUE; else ok = (nostr_event_deserialize(ev, json) == 0);
        if (ok && nostr_event_check_signature(ev)) {
          /* find challenge tag */
          NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev); const char *chal = NULL;
          if (tags) {
            size_t tcount = nostr_tags_size(tags);
            for (size_t i=0;i<tcount;i++) { NostrTag *t = nostr_tags_get(tags, i); const char *k = t?nostr_tag_get_key(t):NULL; if (k && strcmp(k, "challenge") == 0) { chal = nostr_tag_get_value(t); break; } }
          }
          if (chal && st && st->auth_chal[0] && strcmp(chal, st->auth_chal) == 0) {
            const char *pk = nostr_event_get_pubkey(ev);
            if (pk && *pk) { g_strlcpy(st->authed_pubkey, pk, sizeof(st->authed_pubkey)); st->authed = 1; }
          }
        }
        if (ev) nostr_event_free(ev);
        g_free(json);
      }
    }
    return;
  }
  /* Minimal NIP-01: handle EVENT and REQ */
  if (len >= 8 && g_str_has_prefix(data, "[\"COUNT\"")) {
    /* Optional subid then filters */
    const char *p = strchr(data, ',');
    const char *subid = NULL; size_t sublen = 0;
    if (p) { const char *q1 = strchr(p+1, '"'); if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2) { subid = q1+1; sublen = (size_t)(q2-(q1+1)); p = strchr(q2, ','); } } }
    const char *filters_json = p ? p+1 : NULL;
    char sidbuf[128]; sidbuf[0]='\0'; if (subid) { size_t cplen = sublen < sizeof(sidbuf)-1 ? sublen : sizeof(sidbuf)-1; memcpy(sidbuf, subid, cplen); sidbuf[cplen]='\0'; } else { g_strlcpy(sidbuf, "count", sizeof(sidbuf)); }
    NostrFilter *ff = NULL; 
    if (filters_json) {
      gsize flen = (gsize)(data + len - filters_json);
      while (flen>0 && (filters_json[flen-1]=='\n'||filters_json[flen-1]=='\r'||filters_json[flen-1]==' ')) flen--;
      if (flen>0 && filters_json[flen-1] == ']') flen--;
      const char *obj = strchr(filters_json, '{');
      if (obj && obj < (filters_json + flen)) {
        gsize olen = (gsize)((filters_json + flen) - obj);
        char *j = g_strndup(obj, (gssize)olen);
        if (j) { ff = nostr_filter_new(); if (nostr_filter_deserialize(ff, j) != 0) { nostr_filter_free(ff); ff=NULL; } g_free(j); }
      }
    }
    if (app && app->storage && app->storage->vt && app->storage->vt->count) {
      uint64_t cval = 0; NostrFilter *arr1[1]; size_t n=0; if (ff) { arr1[0]=ff; n=1; }
      int rc = app->storage->vt->count(app->storage, (const NostrFilter*)(n?arr1:NULL), n, &cval);
      if (ff) nostr_filter_free(ff);
      if (rc == 0) {
        char resp[128]; g_snprintf(resp, sizeof(resp), "[\"COUNT\",\"%s\",{\"count\":%llu}]", sidbuf, (unsigned long long)cval);
        soup_websocket_connection_send_text(conn, resp);
      } else {
        char *closed = nostr_closed_build_json(sidbuf, "count-failed"); if (closed) { soup_websocket_connection_send_text(conn, closed); g_free(closed);} 
      }
    }
    return;
  }
  if (len >= 7 && g_str_has_prefix(data, "[\"EVENT\"")) {
    if (app && strcmp(app->auth, "required") == 0 && st && !st->authed) {
      /* send OK error for publish */
      char *ok = nostr_ok_build_json("0000", 0, "auth-required"); if (ok) { soup_websocket_connection_send_text(conn, ok); g_free(ok);} 
      return;
    }
    const char *p = strchr(data, ','); if (!p) return; p++;
    /* strip trailing ] and whitespace */
    while (len>0 && (data[len-1]=='\n'||data[len-1]=='\r'||data[len-1]==' ')) len--;
    if (len>0 && data[len-1] == ']') len--;
    if (p < data + len && *p == '{') {
      size_t elen = (size_t)((data + len) - p);
      char *json = g_strndup(p, (gssize)elen);
      if (json) {
        NostrEvent *ev = nostr_event_new();
        int ok = 0;
        if (nostr_event_deserialize_compact(ev, json)) ok=1; else ok = (nostr_event_deserialize(ev, json) == 0);
        const char *reason = NULL; int rc_store = -1; char *id = NULL;
        if (ok && app && app->storage && app->storage->vt && app->storage->vt->put_event) {
          /* If authenticated pubkey is known, enforce match */
          if (app && st && st->authed_pubkey[0]) {
            const char *epk = nostr_event_get_pubkey(ev);
            if (!epk || g_strcmp0(epk, st->authed_pubkey) != 0) {
              char *okj = nostr_ok_build_json("0000", 0, "auth-pubkey-mismatch"); if (okj) { soup_websocket_connection_send_text(conn, okj); g_free(okj);} 
              nostr_event_free(ev); g_free(json); return;
            }
          }
          id = nostr_event_get_id(ev);
          rc_store = app->storage->vt->put_event(app->storage, ev);
          reason = rc_store == 0 ? "" : "error: store failed";
        } else {
          reason = "invalid: bad event";
        }
        const char *id_hex = id ? id : "0000";
        char *okjson = nostr_ok_build_json(id_hex, rc_store == 0, reason);
        if (okjson) { soup_websocket_connection_send_text(conn, okjson); g_free(okjson); }
        if (id) g_free(id);
        if (ev) nostr_event_free(ev);
        g_free(json);
      }
    }
    return;
  }
  if (len >= 7 && g_str_has_prefix(data, "[\"REQ\"")) {
    if (app && strcmp(app->auth, "required") == 0 && st && !st->authed) {
      const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
      char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
      char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"sub1", "auth-required"); if (closed) { soup_websocket_connection_send_text(conn, closed); g_free(closed);} 
      return;
    }
    /* Extract subid and reply EOSE immediately */
    const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
    char subid[128]; subid[0]='\0';
    if (q1 && q2) { size_t sl = (size_t)(q2-(q1+1)); if (sl >= sizeof(subid)) sl = sizeof(subid)-1; memcpy(subid, q1+1, sl); subid[sl]='\0'; p = strchr(q2, ','); }
    else { g_strlcpy(subid, "sub1", sizeof(subid)); }
    const char *filters_json = p ? p+1 : NULL;
    /* Parse first filter object if present */
    NostrFilter *ff = NULL; 
    if (filters_json) {
      /* Trim trailing whitespace and ']' */
      gsize flen = (gsize)(data + len - filters_json);
      while (flen>0 && (filters_json[flen-1]=='\n'||filters_json[flen-1]=='\r'||filters_json[flen-1]==' ')) flen--;
      if (flen>0 && filters_json[flen-1] == ']') flen--;
      const char *obj = strchr(filters_json, '{');
      if (obj && obj < (filters_json + flen)) {
        gsize olen = (gsize)((filters_json + flen) - obj);
        char *j = g_strndup(obj, (gssize)olen);
        if (j) { ff = nostr_filter_new(); if (nostr_filter_deserialize(ff, j) != 0) { nostr_filter_free(ff); ff=NULL; } g_free(j); }
      }
    }
    /* Check NIP-50 SEARCH first */
    gboolean did_search = FALSE;
    if (ff && app && app->storage && app->storage->vt && app->storage->vt->search) {
      const char *q = nostr_filter_get_search(ff);
      if (q && *q) {
        void *it = NULL;
        int rc = app->storage->vt->search(app->storage, q, ff, 0, &it);
        if (rc == 0 && it) {
          GRelayConnState *st = grelay_get_state(app, conn);
          if (st) {
            if (st->it && app->storage->vt->query_free) app->storage->vt->query_free(app->storage, st->it);
            st->it = it; g_strlcpy(st->subid, subid, sizeof(st->subid));
            if (!st->idle_id) st->idle_id = g_idle_add(grelay_stream_tick, st);
          } else {
            if (app->storage->vt->query_free) app->storage->vt->query_free(app->storage, it);
          }
          did_search = TRUE;
        } else if (rc == -ENOTSUP) {
          char *closed = nostr_closed_build_json(subid, "unsupported: search"); if (closed) { soup_websocket_connection_send_text(conn, closed); g_free(closed);} 
          did_search = TRUE;
        }
      }
    }
    if (did_search) { if (ff) nostr_filter_free(ff); return; }
    /* Query storage and stream results persistently */
    if (app && app->storage && app->storage->vt && app->storage->vt->query) {
      int err = 0; NostrFilter *arr1[1]; size_t n=0; if (ff) { arr1[0]=ff; n=1; }
      void *it = app->storage->vt->query(app->storage, (const NostrFilter*)(n?arr1:NULL), n, 0, 0, 0, &err);
      if (it) {
        GRelayConnState *st = grelay_get_state(app, conn);
        if (st) {
          if (st->it && app->storage->vt->query_free) app->storage->vt->query_free(app->storage, st->it);
          st->it = it; g_strlcpy(st->subid, subid, sizeof(st->subid)); st->no_progress_ticks = 0;
          if (!st->idle_id) st->idle_id = g_idle_add(grelay_stream_tick, st);
          Gm.subs_started++; Gm.subs_current++;
        } else {
          /* fallback: immediate drain */
          if (app->storage->vt->query_free) app->storage->vt->query_free(app->storage, it);
        }
      }
    }
    if (ff) nostr_filter_free(ff);
    return;
  }
  if (len >= 8 && g_str_has_prefix(data, "[\"CLOSE\"")) {
    /* Cancel active iterator for this sub */
    const char *q1 = strchr(data, '"');
    const char *q2 = q1 ? strchr(q1+1, '"') : NULL; /* end of CLOSE */
    const char *q3 = q2 ? strchr(q2+1, '"') : NULL; /* start subid */
    const char *q4 = q3 ? strchr(q3+1, '"') : NULL; /* end subid */
    if (q3 && q4 && q4 > q3+1) {
      size_t sl = (size_t)(q4 - (q3+1));
      char subbuf[128]; gsize cplen = sl < sizeof(subbuf)-1 ? sl : sizeof(subbuf)-1; memcpy(subbuf, q3+1, cplen); subbuf[cplen] = '\0';
      GRelayConnState *st = grelay_get_state(app, conn);
      if (st && st->it && st->subid[0] && g_strcmp0(st->subid, subbuf) == 0) {
        if (app->storage->vt->query_free) app->storage->vt->query_free(app->storage, st->it);
        st->it = NULL; st->subid[0] = '\0';
        if (st->idle_id) { g_source_remove(st->idle_id); st->idle_id = 0; }
      }
    }
    return;
  }
  /* Fallback */
  const char *resp = "{\"notice\":\"grelay stub\"}";
  soup_websocket_connection_send_text(conn, resp);
}

static void on_ws_open(SoupServer *server, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *conn, gpointer user_data) {
  (void)server; (void)msg; (void)path; (void)user_data;
  g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), user_data);
  /* NIP-42: send AUTH challenge if auth not off */
  GRelayApp *app = (GRelayApp*)user_data; GRelayConnState *st = grelay_get_state(app, conn);
  if (app && strcmp(app->auth, "off") != 0 && st) {
    /* random 16-byte hex challenge using g_random_int */
    static const char *hx = "0123456789abcdef"; char buf[33];
    for (int i=0;i<8;i++) {
      guint32 r = g_random_int();
      buf[i*4+0] = hx[(r >> 28) & 0xF];
      buf[i*4+1] = hx[(r >> 24) & 0xF];
      buf[i*4+2] = hx[(r >> 20) & 0xF];
      buf[i*4+3] = hx[(r >> 16) & 0xF];
    }
    buf[32] = '\0';
    g_strlcpy(st->auth_chal, buf, sizeof(st->auth_chal));
    char frame[128]; g_snprintf(frame, sizeof(frame), "[\"AUTH\",\"%s\"]", st->auth_chal);
    soup_websocket_connection_send_text(conn, frame);
  }
  Gm.connections_current++; Gm.connections_total++;
  /* Track closure */
  g_signal_connect(conn, "closed", G_CALLBACK(grelay_on_ws_closed), NULL);
}

static void on_http(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data) {
  GRelayApp *app = (GRelayApp*)user_data; (void)server; (void)query;
  /* NIP-86 style JSON-RPC over HTTP */
  if (soup_server_message_get_method(msg) == SOUP_METHOD_POST) {
    /* Require application/nostr+json+rpc */
    SoupMessageHeaders *reqh = soup_server_message_get_request_headers(msg);
    const char *ctype = reqh ? soup_message_headers_get_content_type(reqh, NULL) : NULL;
    if (ctype && g_ascii_strcasecmp(ctype, "application/nostr+json+rpc") == 0) {
      SoupMessageBody *bodyb = soup_server_message_get_request_body(msg);
      GBytes *bytes = bodyb ? soup_message_body_flatten(bodyb) : NULL;
      gsize blen = 0; const char *b = bytes ? g_bytes_get_data(bytes, &blen) : NULL;
      int http_status = SOUP_STATUS_OK;
      char *resp = NULL;
      if (b && blen) {
        if (g_strstr_len(b, blen, "\"method\":\"supportedmethods\"")) {
          resp = g_strdup("{\"result\":[\"getstats\",\"getlimits\",\"supportedmethods\"]}");
        } else if (g_strstr_len(b, blen, "\"method\":\"getstats\"")) {
          char tmp[512];
          g_snprintf(tmp, sizeof(tmp),
            "{\"connections\":{\"current\":%llu,\"total\":%llu,\"closed\":%llu},\"subs\":{\"current\":%llu,\"started\":%llu,\"ended\":%llu},\"stream\":{\"events\":%llu,\"eose\":%llu},\"drops\":{\"rate_limit\":%llu,\"backpressure\":%llu}}",
            (unsigned long long)Gm.connections_current, (unsigned long long)Gm.connections_total, (unsigned long long)Gm.connections_closed,
            (unsigned long long)Gm.subs_current, (unsigned long long)Gm.subs_started, (unsigned long long)Gm.subs_ended,
            (unsigned long long)Gm.events_streamed, (unsigned long long)Gm.eose_sent,
            (unsigned long long)Gm.rate_limit_drops,
            (unsigned long long)Gm.backpressure_drops);
          resp = g_strdup(tmp);
        } else if (g_strstr_len(b, blen, "\"method\":\"getlimits\"")) {
          char tmp[768];
          g_snprintf(tmp, sizeof(tmp),
            "{\"result\":{\"port\":%d,\"storage_driver\":\"%s\",\"max_filters\":%d,\"max_limit\":%d,\"max_subs\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d,\"auth\":\"%s\"}}",
            app?app->port:4849, app?app->storage_driver:"nostrdb",
            app?app->max_filters:10, app?app->max_limit:500, app?app->max_subs:1,
            app?app->rate_ops_per_sec:20, app?app->rate_burst:40, app?app->auth:"off");
          resp = g_strdup(tmp);
        }
      }
      if (!resp) { http_status = SOUP_STATUS_NOT_IMPLEMENTED; resp = g_strdup("{\"error\":\"unsupported\"}"); }
      soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_TAKE, resp, strlen(resp));
      soup_server_message_set_status(msg, http_status, NULL);
      if (bytes) g_bytes_unref(bytes);
      return;
    }
  }
  if (soup_server_message_get_method(msg) == SOUP_METHOD_GET && g_strcmp0(path, "/") == 0) {
    char body[1024];
    const char *name = app && app->name[0] ? app->name : "grelay";
    const char *software = app && app->software[0] ? app->software : "nostrc";
    const char *version = app && app->version[0] ? app->version : "0.1";
    const char *nips = app && app->supported_nips[0] ? app->supported_nips : "[1,11]";
    const char *contact = app && app->contact[0] ? app->contact : NULL;
    const char *desc = app && app->description[0] ? app->description : NULL;
    const char *icon = app && app->icon[0] ? app->icon : NULL;
    const char *policy = app && app->posting_policy[0] ? app->posting_policy : NULL;
    g_snprintf(body, sizeof(body),
      "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\",\"supported_nips\":%s,\"auth\":\"%s\",\"contact\":%s,\"description\":%s,\"icon\":%s,\"posting_policy\":%s,\"limitation\":{\"max_filters\":%d,\"max_limit\":%d,\"max_subscriptions\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d}}",
      name, software, version, nips,
      app?app->auth:"off",
      contact?contact:"null", desc?desc:"null", icon?icon:"null", policy?policy:"null",
      app?app->max_filters:10, app?app->max_limit:500, app?app->max_subs:1,
      app?app->rate_ops_per_sec:20, app?app->rate_burst:40);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, body, strlen(body));
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    return;
  }
  if (soup_server_message_get_method(msg) == SOUP_METHOD_GET && g_strcmp0(path, "/admin/limits") == 0) {
    char body[1024];
    g_snprintf(body, sizeof(body),
      "{\"port\":%d,\"storage_driver\":\"%s\",\"max_filters\":%d,\"max_limit\":%d,\"max_subscriptions\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d}",
      app?app->port:4849, app?app->storage_driver:"nostrdb",
      app?app->max_filters:10, app?app->max_limit:500, app?app->max_subs:1,
      app?app->rate_ops_per_sec:20, app?app->rate_burst:40);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, body, strlen(body));
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    return;
  }
  if (soup_server_message_get_method(msg) == SOUP_METHOD_GET && g_strcmp0(path, "/admin/stats") == 0) {
    char body[1024];
    g_snprintf(body, sizeof(body),
      "{\"connections_total\":%llu,\"connections_current\":%llu,\"connections_closed\":%llu,\"subs_started\":%llu,\"subs_current\":%llu}",
      (unsigned long long)Gm.connections_total, (unsigned long long)Gm.connections_current, (unsigned long long)Gm.connections_closed,
      (unsigned long long)Gm.subs_started, (unsigned long long)Gm.subs_current);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, body, strlen(body));
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    return;
  }
  soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
}

static int g_relay_app_command_line(GApplication *app, GApplicationCommandLine *cmdline) {
  (void)cmdline;
  GRelayApp *self = (GRelayApp*)app;
  GError *error = NULL;
  /* Load JSON backend (libnostr) */
  nostr_json_init();
  /* Create storage */
  if (self->storage_driver[0]) {
    self->storage = nostr_storage_create(self->storage_driver);
  } else {
    self->storage = nostr_storage_create("nostrdb");
  }
  if (self->storage && self->storage->vt && self->storage->vt->open) {
    (void)self->storage->vt->open(self->storage, NULL, NULL);
  }
  self->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(self->server, "/", on_http, self, NULL);
  /* WebSocket endpoint */
  soup_server_add_websocket_handler(self->server, "/nostr", NULL, NULL, on_ws_open, self, NULL);

  if (!soup_server_listen_all(self->server, self->port, 0, &error)) {
    g_printerr("grelay: failed to listen on %u: %s\n", self->port, error ? error->message : "?");
    if (error) g_error_free(error);
    return 1;
  }
  GSList *uris = soup_server_get_uris(self->server);
  for (GSList *l = uris; l; l = l->next) {
    char *s = g_uri_to_string((GUri*)l->data);
    g_print("grelay: listening on %s\n", s);
    g_free(s);
  }
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_application_hold(app);
  return 0;
}

static void g_relay_app_init(GRelayApp *self) {
  self->port = 4849; /* default grelay port */
  g_strlcpy(self->storage_driver, g_getenv("GRELAY_STORAGE_DRIVER") ? g_getenv("GRELAY_STORAGE_DRIVER") : "nostrdb", sizeof(self->storage_driver));
  const char *env_port = g_getenv("GRELAY_PORT"); if (env_port) { guint p = (guint)atoi(env_port); if (p>0) self->port = p; }
  /* defaults */
  g_strlcpy(self->name, g_getenv("GRELAY_NAME") ? g_getenv("GRELAY_NAME") : "grelay", sizeof(self->name));
  g_strlcpy(self->software, g_getenv("GRELAY_SOFTWARE") ? g_getenv("GRELAY_SOFTWARE") : "nostrc", sizeof(self->software));
  g_strlcpy(self->version, g_getenv("GRELAY_VERSION") ? g_getenv("GRELAY_VERSION") : "0.1", sizeof(self->version));
  self->max_filters = g_getenv("GRELAY_MAX_FILTERS") ? atoi(g_getenv("GRELAY_MAX_FILTERS")) : 10;
  self->max_limit = g_getenv("GRELAY_MAX_LIMIT") ? atoi(g_getenv("GRELAY_MAX_LIMIT")) : 500;
  self->max_subs = g_getenv("GRELAY_MAX_SUBS") ? atoi(g_getenv("GRELAY_MAX_SUBS")) : 1;
  self->rate_ops_per_sec = g_getenv("GRELAY_RATE_OPS_PER_SEC") ? atoi(g_getenv("GRELAY_RATE_OPS_PER_SEC")) : 20;
  self->rate_burst = g_getenv("GRELAY_RATE_BURST") ? atoi(g_getenv("GRELAY_RATE_BURST")) : 40;
  self->backpressure_max_ticks = g_getenv("GRELAY_BACKPRESSURE_MAX_TICKS") ? atoi(g_getenv("GRELAY_BACKPRESSURE_MAX_TICKS")) : 0;
  const char *sn = g_getenv("GRELAY_SUPPORTED_NIPS");
  if (sn && *sn) { g_strlcpy(self->supported_nips, sn, sizeof(self->supported_nips)); }
  else { g_strlcpy(self->supported_nips, "[1,11,42,45,50,86]", sizeof(self->supported_nips)); }
  g_strlcpy(self->auth, g_getenv("GRELAY_AUTH") ? g_getenv("GRELAY_AUTH") : "off", sizeof(self->auth));
  g_strlcpy(self->contact, g_getenv("GRELAY_CONTACT") ? g_getenv("GRELAY_CONTACT") : "", sizeof(self->contact));
  g_strlcpy(self->description, g_getenv("GRELAY_DESCRIPTION") ? g_getenv("GRELAY_DESCRIPTION") : "", sizeof(self->description));
  g_strlcpy(self->icon, g_getenv("GRELAY_ICON") ? g_getenv("GRELAY_ICON") : "", sizeof(self->icon));
  g_strlcpy(self->posting_policy, g_getenv("GRELAY_POSTING_POLICY") ? g_getenv("GRELAY_POSTING_POLICY") : "", sizeof(self->posting_policy));
  /* Try GSettings if schema is installed (ignore errors) */
  if (g_settings_schema_source_lookup(g_settings_schema_source_get_default(), "org.nostr.grelay", TRUE)) {
    GSettings *s = g_settings_new("org.nostr.grelay");
    if (s) {
      gint p = g_settings_get_int(s, "port"); if (p>0) self->port = (guint)p;
      gchar *drv = g_settings_get_string(s, "storage-driver"); if (drv && *drv) { g_strlcpy(self->storage_driver, drv, sizeof(self->storage_driver)); }
      gchar *nm = g_settings_get_string(s, "name"); if (nm && *nm) { g_strlcpy(self->name, nm, sizeof(self->name)); }
      gchar *sw = g_settings_get_string(s, "software"); if (sw && *sw) { g_strlcpy(self->software, sw, sizeof(self->software)); }
      gchar *ver = g_settings_get_string(s, "version"); if (ver && *ver) { g_strlcpy(self->version, ver, sizeof(self->version)); }
      gint mf = g_settings_get_int(s, "max-filters"); if (mf>0) self->max_filters = mf;
      gint ml = g_settings_get_int(s, "max-limit"); if (ml>0) self->max_limit = ml;
      gint ms = g_settings_get_int(s, "max-subs"); if (ms>0) self->max_subs = ms;
      gchar *nip = g_settings_get_string(s, "supported-nips"); if (nip && *nip) { g_strlcpy(self->supported_nips, nip, sizeof(self->supported_nips)); }
      gchar *auth = g_settings_get_string(s, "auth"); if (auth && *auth) { g_strlcpy(self->auth, auth, sizeof(self->auth)); }
      gchar *ct = g_settings_get_string(s, "contact"); if (ct && *ct) { g_strlcpy(self->contact, ct, sizeof(self->contact)); }
      gchar *ds = g_settings_get_string(s, "description"); if (ds && *ds) { g_strlcpy(self->description, ds, sizeof(self->description)); }
      gchar *ic = g_settings_get_string(s, "icon"); if (ic && *ic) { g_strlcpy(self->icon, ic, sizeof(self->icon)); }
      gchar *pp = g_settings_get_string(s, "posting-policy"); if (pp && *pp) { g_strlcpy(self->posting_policy, pp, sizeof(self->posting_policy)); }
      gint rops = g_settings_get_int(s, "rate-ops-per-sec"); if (rops>0) self->rate_ops_per_sec = rops;
      gint rburst = g_settings_get_int(s, "rate-burst"); if (rburst>0) self->rate_burst = rburst;
      gint bpt = g_settings_get_int(s, "backpressure-max-ticks"); if (bpt>0) self->backpressure_max_ticks = bpt;
      if (drv) g_free(drv);
      if (nm) g_free(nm);
      if (sw) g_free(sw);
      if (ver) g_free(ver);
      if (nip) g_free(nip);
      if (auth) g_free(auth);
      if (ct) g_free(ct);
      if (ds) g_free(ds);
      if (ic) g_free(ic);
      if (pp) g_free(pp);
      g_object_unref(s);
    }
  }
}

static void g_relay_app_startup(GApplication *app) { G_APPLICATION_CLASS(g_relay_app_parent_class)->startup(app); }
static void g_relay_app_shutdown(GApplication *app) {
  GRelayApp *self = (GRelayApp*)app;
  if (self->server) g_object_unref(self->server);
  if (self->storage) {
    if (self->storage->vt && self->storage->vt->close) self->storage->vt->close(self->storage);
    g_free(self->storage);
  }
  G_APPLICATION_CLASS(g_relay_app_parent_class)->shutdown(app);
}

static void g_relay_app_class_init(GRelayAppClass *klass) {
  GApplicationClass *ac = G_APPLICATION_CLASS(klass);
  ac->startup = g_relay_app_startup;
  ac->shutdown = g_relay_app_shutdown;
  ac->command_line = g_relay_app_command_line;
}

int main(int argc, char **argv) {
  /* Parse CLI for --port and --storage-driver */
  guint port = 0; const char *driver = NULL;
  for (int i=1;i<argc;i++) {
    if (g_strcmp0(argv[i], "--port") == 0 && i+1<argc) { port = (guint)atoi(argv[++i]); continue; }
    if (g_strcmp0(argv[i], "--storage-driver") == 0 && i+1<argc) { driver = argv[++i]; continue; }
  }
  GRelayApp *app = g_object_new(g_relay_app_get_type(),
                                "application-id", "org.nostr.grelay",
                                "flags", G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE,
                                NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  /* Apply CLI overrides after init but before running server: not trivial with GApplication; recommend using env or GSettings. */
  (void)port; (void)driver;
  g_object_unref(app);
  return status;
}
