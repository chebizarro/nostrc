#include <gio/gio.h>
#include <libwebsockets.h>
#include <string.h>
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr-json.h"
#include "nostr_jansson.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"

/* Minimal GApplication with libsoup WebSocket server scaffold.
 * TODO: bridge to NostrStorage, implement NIP-01/11/42/45/50 handlers.
 */

typedef struct {
  GApplication parent_instance;
  struct lws_context *lws;
  GThread *lws_thread;
  int lws_stop;
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
  struct lws *wsi;
  void *it;
  char subid[128];
  int authed;
  char auth_chal[64];
  char authed_pubkey[128];
  /* token bucket */
  double rl_tokens;
  guint64 rl_last_ms;
  /* backpressure */
  unsigned int no_progress_ticks;
  /* retry window to mitigate publish->subscribe races */
  int pending_ticks;
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

static void grelay_on_ws_closed_lws(void) {
  if (Gm.connections_current > 0) Gm.connections_current--;
  Gm.connections_closed++;
}

static void grelay_state_free(gpointer data) {
  GRelayConnState *st = (GRelayConnState*)data;
  if (!st) return;
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

/* LWS service thread */
static gpointer grelay_lws_service_thread(gpointer arg) {
  GRelayApp *self = (GRelayApp*)arg;
  if (!self) return NULL;
  while (!self->lws_stop) {
    if (!self->lws) break;
    int n = lws_service(self->lws, 50);
    if (n < 0) break;
  }
  return NULL;
}

static gboolean grelay_stream_tick(gpointer user_data) {
  GRelayConnState *st = (GRelayConnState*)user_data;
  if (!st || !st->app || !st->app->storage || !st->app->storage->vt || !st->app->storage->vt->query_next || !st->it) return FALSE;
  gboolean sent_any = FALSE; int sent_count = 0;
  for (int i=0; i<8; i++) {
    NostrEvent ev={0}; size_t one=1; if (st->app->storage->vt->query_next(st->app->storage, st->it, &ev, &one) != 0 || one==0) break;
    char *ejson = nostr_event_serialize_compact(&ev); if (!ejson) ejson = nostr_event_serialize(&ev);
    if (ejson) {
      gsize need = strlen(ejson) + strlen(st->subid) + 32; char *frame = g_malloc(need);
      if (frame) {
        g_snprintf(frame, need, "[\"EVENT\",\"%s\",%s]", st->subid, ejson);
        size_t mlen = strlen(frame);
        size_t buflen = LWS_PRE + mlen;
        unsigned char *buf = g_malloc(buflen);
        if (buf) {
          memcpy(buf + LWS_PRE, frame, mlen);
          lws_write(st->wsi, buf + LWS_PRE, mlen, LWS_WRITE_TEXT);
          g_free(buf);
        }
        g_free(frame);
      }
      g_free(ejson);
      sent_any = TRUE;
      Gm.events_streamed++;
      sent_count++;
    }
  }
  if (sent_count > 0) {
    g_message("grelay: stream tick sent=%d sub=%.32s", sent_count, st->subid);
  }
  if (!sent_any) {
    /* backpressure: if we repeatedly cannot make progress, drop */
    st->no_progress_ticks++;
    int max_ticks = st->app ? st->app->backpressure_max_ticks : 0;
    if (max_ticks > 0 && (int)st->no_progress_ticks >= max_ticks) {
      char *closed = nostr_closed_build_json(st->subid[0]?st->subid:"sub1", "backpressure");
      if (closed) {
        size_t mlen = strlen(closed);
        unsigned char *buf = g_malloc(LWS_PRE + mlen);
        if (buf) { memcpy(buf + LWS_PRE, closed, mlen); lws_write(st->wsi, buf + LWS_PRE, mlen, LWS_WRITE_TEXT); g_free(buf);} 
        g_free(closed);
      }
      if (st->it && st->app->storage->vt->query_free) st->app->storage->vt->query_free(st->app->storage, st->it);
      st->it = NULL; st->subid[0] = '\0'; st->no_progress_ticks = 0;
      if (Gm.subs_current > 0) Gm.subs_current--; Gm.subs_ended++; Gm.backpressure_drops++;
      g_message("grelay: backpressure drop sub closed");
      return FALSE;
    }
    /* send EOSE and cleanup */
    char *eose = nostr_eose_build_json(st->subid);
    if (eose) {
      size_t mlen = strlen(eose);
      size_t buflen = LWS_PRE + mlen;
      unsigned char *buf = g_malloc(buflen);
      if (buf) { memcpy(buf + LWS_PRE, eose, mlen); lws_write(st->wsi, buf + LWS_PRE, mlen, LWS_WRITE_TEXT); g_free(buf);} 
      g_free(eose);
    }
    if (st->it && st->app->storage->vt->query_free) st->app->storage->vt->query_free(st->app->storage, st->it);
    st->it = NULL; st->subid[0] = '\0'; st->no_progress_ticks = 0;
    if (Gm.subs_current > 0) Gm.subs_current--; Gm.subs_ended++;
    Gm.eose_sent++;
    g_message("grelay: EOSE sent");
    return FALSE;
  }
  st->no_progress_ticks = 0;
  return TRUE; /* keep streaming */
}

/* LWS protocol callback: HTTP + WS on root */
static int grelay_lws_cb(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {
  GRelayConnState *st = (GRelayConnState*)user;
  GRelayApp *app = (GRelayApp*)lws_context_user(lws_get_context(wsi));
  switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
      /* Handshake diagnostics: log current protocol context (no header APIs for max portability) */
      const struct lws_protocols *proto = lws_get_protocol(wsi);
      const char *prname = proto && proto->name ? proto->name : "(none)";
      g_message("grelay: FILTER_PROTOCOL_CONNECTION: proto=%s", prname);
      fprintf(stderr, "[grelay] filter_proto: proto=%s\n", prname);
      return 0;
    }
    case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED: {
      g_message("grelay: new client instantiated");
      fprintf(stderr, "[grelay] new client instantiated\n");
      return 0;
    }
    case LWS_CALLBACK_HTTP: {
      const char *uri = (const char*)in;
      if (!uri) uri = "/";
      if (strcmp(uri, "/") == 0) {
        char body[1024];
        const char *name = app && app->name[0] ? app->name : "grelay";
        const char *software = app && app->software[0] ? app->software : "nostrc";
        const char *version = app && app->version[0] ? app->version : "0.1";
        const char *nips = app && app->supported_nips[0] ? app->supported_nips : "[1,11,42,45,50,86]";
        g_snprintf(body, sizeof(body),
          "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\",\"supported_nips\":%s,\"auth\":\"%s\",\"contact\":null,\"description\":null,\"icon\":null,\"posting_policy\":null,\"limitation\":{\"max_filters\":%d,\"max_limit\":%d,\"max_subscriptions\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d}}",
          name, software, version, nips, app?app->auth:"off", app?app->max_filters:10, app?app->max_limit:500, app?app->max_subs:1, app?app->rate_ops_per_sec:20, app?app->rate_burst:40);
        (void)lws_return_http_status(wsi, HTTP_STATUS_OK, body);
        return 0;
      } else if (strcmp(uri, "/admin/stats") == 0) {
        char body[512];
        g_snprintf(body, sizeof(body),
          "{\"connections\":{\"current\":%llu,\"total\":%llu,\"closed\":%llu},\"subs\":{\"current\":%llu,\"started\":%llu,\"ended\":%llu},\"stream\":{\"events\":%llu,\"eose\":%llu},\"drops\":{\"rate_limit\":%llu,\"backpressure\":%llu}}",
          (unsigned long long)Gm.connections_current, (unsigned long long)Gm.connections_total, (unsigned long long)Gm.connections_closed,
          (unsigned long long)Gm.subs_current, (unsigned long long)Gm.subs_started, (unsigned long long)Gm.subs_ended,
          (unsigned long long)Gm.events_streamed, (unsigned long long)Gm.eose_sent,
          (unsigned long long)Gm.rate_limit_drops, (unsigned long long)Gm.backpressure_drops);
        (void)lws_return_http_status(wsi, HTTP_STATUS_OK, body);
        return 0;
      } else if (strcmp(uri, "/admin/limits") == 0) {
        char body[512];
        g_snprintf(body, sizeof(body),
          "{\"port\":%d,\"storage_driver\":\"%s\",\"max_filters\":%d,\"max_limit\":%d,\"max_subscriptions\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d}",
          app?app->port:4849, app?app->storage_driver:"nostrdb", app?app->max_filters:10, app?app->max_limit:500, app?app->max_subs:1, app?app->rate_ops_per_sec:20, app?app->rate_burst:40);
        (void)lws_return_http_status(wsi, HTTP_STATUS_OK, body);
        return 0;
      }
      /* 404 */
      (void)lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
      return 0;
    }
    case LWS_CALLBACK_ESTABLISHED: {
      if (!st) break; st->wsi = wsi; st->app = app;
      st->pending_ticks = 0;
      Gm.connections_current++; Gm.connections_total++;
      const struct lws_protocols *proto = lws_get_protocol(wsi);
      const char *pname = proto && proto->name ? proto->name : "(unknown)";
      g_message("grelay: WS established (protocol=%s)", pname);
      fprintf(stderr, "[grelay] WS established (protocol=%s)\n", pname);
      /* Ask to be called back when writeable so we can sanity-check send path */
      lws_callback_on_writable(wsi);
      return 0;
    }
    case LWS_CALLBACK_CLOSED: {
      grelay_on_ws_closed_lws();
      g_message("grelay: WS closed");
      fprintf(stderr, "[grelay] WS closed\n");
      return 0;
    }
    case LWS_CALLBACK_RECEIVE: {
      if (!st) break; const char *data = (const char*)in; size_t n = len; if (!data || n<2) return 0;
      {
        char dbg[257]; size_t cp = n < 256 ? n : 256; memcpy(dbg, data, cp); dbg[cp] = '\0';
        g_message("grelay: ws frame: %.256s", dbg); fprintf(stderr, "[grelay] ws frame: %.256s\n", dbg);
      }
      /* Reuse existing parsing logic by adapting minimal pieces inline */
      /* Only handle EVENT/REQ/COUNT/CLOSE here; AUTH challenge can be added similarly */
      if (n >= 7 && g_str_has_prefix(data, "[\"EVENT\"")) {
        if (app && strcmp(app->auth, "required") == 0 && st && !st->authed) {
          char *ok = nostr_ok_build_json("0000", 0, "auth-required");
          if (ok) { size_t m=strlen(ok); unsigned char *buf=g_malloc(LWS_PRE+m); if(buf){ memcpy(buf+LWS_PRE, ok, m); lws_write(wsi, buf+LWS_PRE, m, LWS_WRITE_TEXT); g_free(buf);} g_free(ok);} 
          return 0;
        }
        const char *p = strchr(data, ','); if (!p) return 0; p++;
        /* Trim trailing whitespace */
        while (n>0 && (data[n-1]=='\n'||data[n-1]=='\r'||data[n-1]==' ')) n--;
        /* Find the last '}' before any trailing array ']' */
        size_t end_idx = n;
        if (end_idx>0) end_idx--; /* last valid index */
        while (end_idx > (size_t)(p - data) && data[end_idx] != '}') {
          end_idx--;
        }
        if (p < data + n && *p == '{' && data[end_idx] == '}') {
          size_t elen = (size_t)((data + end_idx + 1) - p);
          char *json = g_strndup(p, (gssize)elen);
          if (json) {
            NostrEvent *ev = nostr_event_new();
            /* Disable compact parser; use canonical public API only */
            int rc = nostr_event_deserialize(ev, json);
            int ok = (rc == 0 || rc == 1); /* accept 0 or 1 as success across backends */
            const char *reason = NULL; int rc_store = -1; char *id = NULL;
            if (ok && app && app->storage && app->storage->vt && app->storage->vt->put_event) {
              if (app && st && st->authed_pubkey[0]) {
                const char *epk = nostr_event_get_pubkey(ev);
                if (!epk || g_strcmp0(epk, st->authed_pubkey) != 0) {
                  char *okj = nostr_ok_build_json("0000", 0, "auth-pubkey-mismatch");
                  if (okj) { size_t m=strlen(okj); unsigned char *buf=g_malloc(LWS_PRE+m); if(buf){ memcpy(buf+LWS_PRE, okj, m); lws_write(wsi, buf+LWS_PRE, m, LWS_WRITE_TEXT); g_free(buf);} g_free(okj);} 
                  nostr_event_free(ev); g_free(json); return 0;
                }
              }
              id = nostr_event_get_id(ev);
              rc_store = app->storage->vt->put_event(app->storage, ev);
              reason = rc_store == 0 ? "" : "error: store failed";
              if (rc_store == 0) g_message("grelay: store ok id=%.16s kind=%d", id?id:"0000", ev->kind);
              else g_message("grelay: store failed id=%.16s kind=%d", id?id:"0000", ev->kind);
            } else {
              if (ok) {
                /* Parsing succeeded but storage path is unavailable */
                reason = "error: store-unavailable";
                g_message("grelay: EVENT ok (rc=%d) but storage unavailable: storage=%p vt=%p put_event=%p",
                          rc,
                          (void*) (app ? app->storage : NULL),
                          (app && app->storage) ? (void*)app->storage->vt : NULL,
                          (app && app->storage && app->storage->vt) ? (void*)app->storage->vt->put_event : NULL);
              } else {
                reason = "invalid: bad event";
                g_message("grelay: EVENT parse failed rc=%d (len=%zu): %.256s ...", rc, elen, json);
              }
            }
            const char *id_hex = id ? id : "0000";
            char *okjson = nostr_ok_build_json(id_hex, rc_store == 0, reason);
            if (okjson) { size_t m=strlen(okjson); unsigned char *buf=g_malloc(LWS_PRE+m); if(buf){ memcpy(buf+LWS_PRE, okjson, m); lws_write(wsi, buf+LWS_PRE, m, LWS_WRITE_TEXT); g_free(buf);} g_free(okjson);} 
            if (id) g_free(id); if (ev) nostr_event_free(ev); g_free(json);
          }
        }
        return 0;
      }
      if (n >= 7 && g_str_has_prefix(data, "[\"REQ\"")) {
        const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
        char subid[128]; subid[0]='\0'; if (q1 && q2) { size_t sl=(size_t)(q2-(q1+1)); if (sl>=sizeof(subid)) sl=sizeof(subid)-1; memcpy(subid, q1+1, sl); subid[sl]='\0'; p = strchr(q2, ','); } else { g_strlcpy(subid, "sub1", sizeof(subid)); }
        const char *filters_json = p ? p+1 : NULL;
        NostrFilter *ff = NULL; if (filters_json) {
          size_t flen = (size_t)(data + n - filters_json); while (flen>0 && (filters_json[flen-1]=='\n'||filters_json[flen-1]=='\r'||filters_json[flen-1]==' ')) flen--; if (flen>0 && filters_json[flen-1] == ']') flen--;
          const char *obj = strchr(filters_json, '{'); if (obj && obj < (filters_json + flen)) { size_t olen = (size_t)((filters_json + flen) - obj); char *j = g_strndup(obj, (gssize)olen); if (j) { ff = nostr_filter_new(); if (nostr_filter_deserialize(ff, j) != 0) { nostr_filter_free(ff); ff=NULL; } g_free(j);} }
        }
        if (app && app->storage && app->storage->vt && app->storage->vt->query) {
          int err = 0; NostrFilter *arr1[1]; size_t m=0; if (ff) { arr1[0]=ff; m=1; }
          void *it = app->storage->vt->query(app->storage, (const NostrFilter*)(m?arr1:NULL), m, 0, 0, 0, &err);
          if (it) {
            if (!st) st = (GRelayConnState*)user; st->it = it; g_strlcpy(st->subid, subid, sizeof(st->subid)); st->no_progress_ticks=0; Gm.subs_started++; Gm.subs_current++; g_message("grelay: query iterator started sub=%.32s", st->subid);
            /* Kick a first tick inline */
            grelay_stream_tick(st);
            /* Schedule a few follow-up ticks to catch near-simultaneous publishes */
            if (st->it) { st->pending_ticks = 10; lws_set_timer_usecs(wsi, 50 * 1000); }
          } else {
            g_message("grelay: query returned no iterator err=%d", err);
            /* immediate EOSE */
            char *eose = nostr_eose_build_json(subid);
            if (eose){ size_t m=strlen(eose); unsigned char *buf=g_malloc(LWS_PRE+m); if(buf){ memcpy(buf+LWS_PRE, eose, m); lws_write(wsi, buf+LWS_PRE, m, LWS_WRITE_TEXT); g_free(buf);} g_free(eose);} 
          }
        }
        if (ff) nostr_filter_free(ff);
        return 0;
      }
      if (n >= 8 && g_str_has_prefix(data, "[\"COUNT\"")) {
        const char *p = strchr(data, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL; char sidbuf[128]; sidbuf[0]='\0'; if (q1&&q2){ size_t sl=(size_t)(q2-(q1+1)); if (sl>=sizeof(sidbuf)) sl=sizeof(sidbuf)-1; memcpy(sidbuf, q1+1, sl); sidbuf[sl]='\0'; p=strchr(q2, ','); } else { g_strlcpy(sidbuf, "count", sizeof(sidbuf)); }
        const char *filters_json = p ? p+1 : NULL; NostrFilter *ff=NULL; if (filters_json){ size_t flen=(size_t)(data+n-filters_json); while(flen>0 && (filters_json[flen-1]=='\n'||filters_json[flen-1]=='\r'||filters_json[flen-1]==' ')) flen--; if(flen>0 && filters_json[flen-1]==']') flen--; const char *obj=strchr(filters_json,'{'); if(obj && obj<(filters_json+flen)){ size_t olen=(size_t)((filters_json+flen)-obj); char *j=g_strndup(obj,(gssize)olen); if(j){ ff=nostr_filter_new(); if(nostr_filter_deserialize(ff,j)!=0){ nostr_filter_free(ff); ff=NULL;} g_free(j);} } }
        if (app && app->storage && app->storage->vt && app->storage->vt->count) {
          uint64_t cval=0; NostrFilter *arr1[1]; size_t m=0; if(ff){ arr1[0]=ff; m=1; }
          int rc = app->storage->vt->count(app->storage, (const NostrFilter*)(m?arr1:NULL), m, &cval);
          if (rc==0){ char resp[128]; g_snprintf(resp,sizeof(resp), "[\"COUNT\",\"%s\",{\"count\":%llu}]", sidbuf, (unsigned long long)cval); size_t mm=strlen(resp); unsigned char *buf=g_malloc(LWS_PRE+mm); if(buf){ memcpy(buf+LWS_PRE, resp, mm); lws_write(wsi, buf+LWS_PRE, mm, LWS_WRITE_TEXT); g_free(buf);} }
          else { char *closed = nostr_closed_build_json(sidbuf, "count-failed"); if (closed){ size_t mm=strlen(closed); unsigned char *buf=g_malloc(LWS_PRE+mm); if(buf){ memcpy(buf+LWS_PRE, closed, mm); lws_write(wsi, buf+LWS_PRE, mm, LWS_WRITE_TEXT); g_free(buf);} g_free(closed);} }
        }
        if (ff) nostr_filter_free(ff);
        return 0;
      }
      if (n >= 8 && g_str_has_prefix(data, "[\"CLOSE\"")) {
        /* Stop iterator and send nothing */
        if (st && st->it && st->subid[0]) { if (st->app->storage->vt->query_free) st->app->storage->vt->query_free(st->app->storage, st->it); st->it=NULL; st->subid[0]='\0'; if (Gm.subs_current>0) Gm.subs_current--; Gm.subs_ended++; }
        return 0;
      }
      return 0;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      /* Send a small NOTICE to prove outbound works and nudge any buffering */
      static const char *notice = "[\"NOTICE\",\"hello\"]";
      size_t m = strlen(notice);
      unsigned char *buf = g_malloc(LWS_PRE + m);
      if (buf) {
        memcpy(buf + LWS_PRE, notice, m);
        lws_write(wsi, buf + LWS_PRE, m, LWS_WRITE_TEXT);
        g_free(buf);
        g_message("grelay: writeable: sent NOTICE"); fprintf(stderr, "[grelay] writeable: sent NOTICE\n");
      }
      return 0;
    }
    default:
      /* Verbose callback trace for debugging */
      g_message("grelay: LWS reason=%d", (int)reason);
      break;
    case LWS_CALLBACK_TIMER: {
      if (!st) return 0;
      if (st->it) {
        grelay_stream_tick(st);
        if (st->it && st->pending_ticks > 0) {
          st->pending_ticks--;
          lws_set_timer_usecs(wsi, 50 * 1000);
        }
      }
      return 0;
    }
  }
  /* Do not delegate to lws_callback_http_dummy for WS reasons; just return 0 */
  return 0;
}

/* LWS protocol list: only 'nostr' so both HTTP and WS are handled in grelay_lws_cb */
static const struct lws_protocols grelay_protocols[] = {
  { "nostr", grelay_lws_cb, sizeof(GRelayConnState), 4096 },
  { NULL, NULL, 0, 0 }
};

static int g_relay_app_command_line(GApplication *app, GApplicationCommandLine *cmdline) {
  (void)cmdline;
  GRelayApp *self = (GRelayApp*)app;
  GError *error = NULL;
  /* Load JSON backend (libnostr): force canonical backend and disable compact */
  nostr_set_json_interface(jansson_impl);
  nostr_json_force_fallback(true);
  nostr_json_init();
  /* Create storage */
  if (self->storage_driver[0]) {
    self->storage = nostr_storage_create(self->storage_driver);
  } else {
    self->storage = nostr_storage_create("nostrdb");
  }
  /* Pick a durable default path and allow override */
  const char *uri = g_getenv("GrelayStoragePath"); /* legacy */
  if (!uri || !*uri) uri = g_getenv("GRELAY_STORAGE_URI");
  if (!uri || !*uri) uri = "./data/ndb";
  /* Ensure directory exists */
  if (g_mkdir_with_parents(uri, 0700) != 0) {
    g_message("grelay: failed to create storage dir %s, continuing without storage", uri);
    if (self->storage) { if (self->storage->vt && self->storage->vt->close) self->storage->vt->close(self->storage); self->storage = NULL; }
  } else {
    if (self->storage && self->storage->vt && self->storage->vt->open) {
      int rc_open = self->storage->vt->open(self->storage, uri, NULL);
      if (rc_open != 0) {
        g_message("grelay: storage open failed rc=%d for uri=%s; disabling storage", rc_open, uri);
        if (self->storage->vt->close) self->storage->vt->close(self->storage);
        self->storage = NULL;
      }
    }
  }
  /* Log storage backend availability */
  g_message("grelay: storage driver=%s vt=%p put_event=%p query=%p",
            self->storage_driver[0]?self->storage_driver:"nostrdb",
            self->storage ? (void*)self->storage->vt : NULL,
            (self->storage && self->storage->vt) ? (void*)self->storage->vt->put_event : NULL,
            (self->storage && self->storage->vt) ? (void*)self->storage->vt->query : NULL);
  /* Initialize LWS context */
  struct lws_context_creation_info info; memset(&info, 0, sizeof(info));
  info.port = (int)self->port;
  info.protocols = grelay_protocols;
  info.user = self;
  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  /* Harden context to avoid extension/negotiation quirks */
  info.extensions = NULL; /* disable permessage-deflate etc. */
  info.pt_serv_buf_size = 32768;
  info.timeout_secs = 20;
  self->lws = lws_create_context(&info);
  if (!self->lws) { g_printerr("grelay: failed to create LWS context on %u\n", self->port); return 1; }
  self->lws_stop = 0;
  self->lws_thread = g_thread_new("grelay-lws", grelay_lws_service_thread, self);
  g_print("grelay: listening on http://0.0.0.0:%u/\n", self->port);
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
  /* Stop service thread first to avoid races */
  if (self->lws) {
    self->lws_stop = 1;
    lws_cancel_service(self->lws);
  }
  if (self->lws_thread) {
    g_thread_join(self->lws_thread);
    self->lws_thread = NULL;
  }
  if (self->lws) { lws_context_destroy(self->lws); self->lws = NULL; }
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
