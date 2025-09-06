#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <libwebsockets.h>
#include "nostr-storage.h"
#include "nostr-relay-core.h"
#include "nostr-relay-limits.h"
#include "nip11.h"
#include "relayd_config.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#ifdef HAVE_NIP86
#include "nip86.h"
#endif

static volatile int force_exit = 0;

static void sigint_handler(int sig){ (void)sig; force_exit = 1; }

/* Minimal HTTP handler to serve NIP-11 JSON at GET / (placeholder). */
typedef struct {
  NostrStorage *storage;
  RelaydConfig cfg;
} RelaydCtx;

/* Per-connection user data for the 'nostr' protocol */
typedef struct {
  void *it;              /* storage iterator */
  char subid[128];       /* active subscription id */
  int authed;            /* NIP-42 auth state */
  int need_auth_chal;    /* send AUTH challenge on next writeable */
  char auth_chal[64];    /* simple challenge string */
  char authed_pubkey[128]; /* hex pubkey of authenticated client */
} ConnState;

typedef struct {
  int collecting;
  char *body;
  size_t len;
  size_t cap;
  int is_nip86;
  char uri[256];
} HttpState;

/* Secure nonce generator */
static void gen_nonce(char *out_hex, size_t out_sz) {
  if (!out_hex || out_sz < 33) return; /* need room for 16-byte hex + NUL */
  unsigned char buf[16];
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t r = -1;
  if (fd >= 0) { r = read(fd, buf, sizeof(buf)); close(fd); }
  if (r != (ssize_t)sizeof(buf)) {
    /* Fallback to time-based if urandom not available */
    unsigned long t = (unsigned long)time(NULL);
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (t >> ((i % sizeof(unsigned long)) * 8)) & 0xFF;
  }
  static const char *hex = "0123456789abcdef";
  size_t j = 0;
  for (size_t i = 0; i < sizeof(buf) && j + 2 < out_sz; i++) {
    out_hex[j++] = hex[(buf[i] >> 4) & 0xF];
    out_hex[j++] = hex[buf[i] & 0xF];
  }
  out_hex[j] = '\0';
}

/* Kind semantics helpers */
static inline bool is_replaceable_kind(int kind) { return kind == 0 || kind == 3 || kind == 41; }
static inline bool is_param_replaceable_kind(int kind) { return kind >= 30000 && kind < 40000; }

static int http_cb(struct lws *wsi, enum lws_callback_reasons reason,
                   void *user, void *in, size_t len) {
  switch (reason) {
    case LWS_CALLBACK_HTTP: {
      const char *uri = (const char*)in;
      /* Check for NIP-86 JSON-RPC content-type */
      char ctype[128]; ctype[0] = '\0';
      if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE) > 0) {
        lws_hdr_copy(wsi, ctype, sizeof(ctype), WSI_TOKEN_HTTP_CONTENT_TYPE);
      }
      int is_nip86 = (ctype[0] && strcasecmp(ctype, "application/nostr+json+rpc") == 0);
      if (is_nip86) {
        HttpState *hs = (HttpState*)user;
        if (hs) {
          hs->collecting = 1; hs->is_nip86 = 1; hs->len = 0; hs->cap = 16384; /* 16KB cap */
          hs->body = (char*)malloc(hs->cap);
          hs->uri[0] = '\0'; if (uri) strncpy(hs->uri, uri, sizeof(hs->uri)-1);
        }
        return 0;
      }
      if (uri && strcmp(uri, "/") == 0) {
        const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
        char *body = NULL;
        int need_free = 0;
#ifdef HAVE_NIP11
        /* Build NIP-11 via module using config */
        RelayInformationDocument doc = {0};
        if (ctx) {
          if (ctx->cfg.name[0]) doc.name = ctx->cfg.name; else doc.name = "nostrc-relayd";
          if (ctx->cfg.software[0]) doc.software = ctx->cfg.software; else doc.software = "nostrc";
          if (ctx->cfg.version[0]) doc.version = ctx->cfg.version; else doc.version = "0.1";
          if (ctx->cfg.description[0]) doc.description = ctx->cfg.description;
          if (ctx->cfg.contact[0]) doc.contact = ctx->cfg.contact;
          doc.supported_nips = (int*)ctx->cfg.supported_nips;
          doc.supported_nips_count = ctx->cfg.supported_nips_count;
        } else {
          doc.name = "nostrc-relayd";
          doc.software = "nostrc";
          doc.version = "0.1";
        }
        RelayLimitationDocument *L = NULL;
        if (ctx) {
          L = (RelayLimitationDocument*)calloc(1, sizeof(*L));
          if (L) {
            L->max_filters = ctx->cfg.max_filters;
            L->max_limit = ctx->cfg.max_limit;
            doc.limitation = L;
          }
        }
        body = nostr_nip11_build_info_json(&doc);
        need_free = (body != NULL);
        if (L) free(L);
#else
        /* Minimal NIP-11-like JSON if module not built */
        const char *name = (ctx && ctx->cfg.name[0]) ? ctx->cfg.name : "nostrc-relayd";
        const char *software = (ctx && ctx->cfg.software[0]) ? ctx->cfg.software : "nostrc";
        const char *version = (ctx && ctx->cfg.version[0]) ? ctx->cfg.version : "0.1";
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\"}", name, software, version);
        body = strdup(tmp);
        need_free = 1;
#endif
        if (!body) body = "{\"name\":\"nostrc-relayd\"}";
        unsigned char buf[LWS_PRE + 1024];
        unsigned char *p = &buf[LWS_PRE];
        size_t blen = strlen(body);
        memcpy(p, body, blen);
        (void)lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "application/json", blen, &p, buf + sizeof(buf));
        (void)lws_finalize_http_header(wsi, &p, buf + sizeof(buf));
        if (lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_HTTP) < 0) return -1;
        if (need_free && body && body[0] == '{') free(body);
        return -1; /* close connection */
      }
      break;
    }
    case LWS_CALLBACK_HTTP_BODY: {
      HttpState *hs = (HttpState*)user;
      if (hs && hs->collecting && hs->is_nip86 && hs->body && hs->len < hs->cap) {
        size_t take = len;
        if (hs->len + take > hs->cap) take = hs->cap - hs->len;
        memcpy(hs->body + hs->len, in, take);
        hs->len += take;
      }
      return 0;
    }
    case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
      HttpState *hs = (HttpState*)user;
      if (hs && hs->collecting && hs->is_nip86) {
        /* Null-terminate */
        if (hs->body) {
          if (hs->len == hs->cap) hs->len--; /* ensure room */
          hs->body[hs->len] = '\0';
        }
        char auth[1024]; auth[0] = '\0';
        if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_AUTHORIZATION) > 0) {
          lws_hdr_copy(wsi, auth, sizeof(auth), WSI_TOKEN_HTTP_AUTHORIZATION);
        }
        int http_status = 200;
        /* Reconstruct absolute URL: http(s)://<host><uri> */
        char host[256]; host[0]='\0';
        if (lws_hdr_total_length(wsi, WSI_TOKEN_HOST) > 0) {
          lws_hdr_copy(wsi, host, sizeof(host), WSI_TOKEN_HOST);
        }
        const char *scheme = lws_is_ssl(wsi) ? "https://" : "http://";
        char url[768]; url[0]='\0';
        const char *uri2 = (hs && hs->uri[0]) ? hs->uri : "/";
        snprintf(url, sizeof(url), "%s%s%s", scheme, host, uri2);
        const char *method = "POST"; /* JSON-RPC over HTTP is POST */
        char *resp = NULL;
#ifdef HAVE_NIP86
        resp = nostr_nip86_process_request((void*)lws_context_user(lws_get_context(wsi)), auth, hs->body, method, url, &http_status);
#else
        http_status = 501; /* Not Implemented */
        resp = strdup("{\"error\":\"nip86 disabled\"}");
#endif
        const char *body = resp ? resp : "{\"error\":\"internal\"}";
        char hdr[256];
        int n = lws_snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", http_status, strlen(body));
        unsigned char *buf = (unsigned char*)malloc(LWS_PRE + n + strlen(body));
        if (buf) {
          memcpy(&buf[LWS_PRE], hdr, n);
          memcpy(&buf[LWS_PRE + n], body, strlen(body));
          lws_write(wsi, &buf[LWS_PRE], n + strlen(body), LWS_WRITE_HTTP);
          free(buf);
        }
        if (resp) free(resp);
        if (hs->body) free(hs->body);
        hs->body = NULL; hs->collecting = 0; hs->is_nip86 = 0; hs->len = hs->cap = 0;
        return -1; /* close */
      }
      return 0;
    }
    default: break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}

/* Minimal Nostr WS protocol callback (scaffold). */
static int nostr_cb(struct lws *wsi, enum lws_callback_reasons reason,
                   void *user, void *in, size_t len) {
  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
      /* connection established */
      if (user) {
        memset(user, 0, sizeof(ConnState));
        const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
        ConnState *cs = (ConnState*)user;
        /* If auth is required, start unauthenticated */
        cs->authed = (ctx && strcmp(ctx->cfg.auth, "required") == 0) ? 0 : 1;
        if (ctx && strcmp(ctx->cfg.auth, "off") != 0) {
          /* Build a simple challenge (timestamp-based); prod: random nonce */
          snprintf(cs->auth_chal, sizeof(cs->auth_chal), "%lu", (unsigned long)time(NULL));
          cs->need_auth_chal = 1;
          lws_callback_on_writable(wsi);
        }
        /* IP block enforcement (NIP-86) */
#ifdef HAVE_NIP86
        char ipbuf[128]; ipbuf[0]='\0';
        if (lws_get_peer_simple(wsi, ipbuf, sizeof(ipbuf))) {
          if (nostr_nip86_is_ip_blocked(ipbuf)) {
            fprintf(stderr, "relayd: blocked IP %s, closing\n", ipbuf);
            return -1; /* close connection */
          }
        }
#endif
      }
      fprintf(stderr, "relayd: client connected\n");
      break;
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      /* Stream events for an active REQ iterator, if any */
      ConnState *cs = (ConnState*)user;
      const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
      NostrStorage *st = (ctx ? ctx->storage : NULL);
      if (cs && cs->need_auth_chal) {
        /* Send ["AUTH","challenge"] */
        unsigned char *buf = (unsigned char*)malloc(LWS_PRE + 64 + strlen(cs->auth_chal));
        if (buf) {
          int m = snprintf((char*)&buf[LWS_PRE], 64 + (int)strlen(cs->auth_chal), "[\"AUTH\",\"%s\"]", cs->auth_chal);
          if (m > 0) lws_write(wsi, &buf[LWS_PRE], (size_t)m, LWS_WRITE_TEXT);
          free(buf);
        }
        cs->need_auth_chal = 0;
        break;
      }
      if (!cs || !cs->it || !st || !st->vt || !st->vt->query_next) break;
      int sent_any = 0;
      for (int i = 0; i < 8; i++) { /* small batch per writable */
        size_t n = 1;
        NostrEvent ev = {0};
        int rc = st->vt->query_next(st, cs->it, &ev, &n);
        if (!(rc == 0 && n > 0)) {
          break;
        }
        /* Serialize and send ["EVENT","subid", {event}] */
        char *ejson = nostr_event_serialize_compact(&ev);
        if (!ejson) ejson = nostr_event_serialize(&ev);
        if (ejson) {
          size_t need = strlen(ejson) + strlen(cs->subid) + 64;
          unsigned char *buf = (unsigned char*)malloc(LWS_PRE + need);
          if (buf) {
            int m = snprintf((char*)&buf[LWS_PRE], need, "[\"EVENT\",\"%s\",%s]", cs->subid, ejson);
            if (m > 0) {
              lws_write(wsi, &buf[LWS_PRE], (size_t)m, LWS_WRITE_TEXT);
              sent_any = 1;
            }
            free(buf);
          }
          free(ejson);
        }
      }
      if (sent_any) {
        /* Re-arm to continue if more remain */
        lws_callback_on_writable(wsi);
      } else {
        /* Done: send EOSE and cleanup */
        if (cs->subid[0]) {
          char *eose = nostr_eose_build_json(cs->subid);
          if (eose) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(eose); memcpy(&buf[LWS_PRE], eose, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(eose); }
        }
        if (st->vt->query_free && cs->it) st->vt->query_free(st, cs->it);
        cs->it = NULL; cs->subid[0] = '\0';
        fprintf(stderr, "relayd: iterator finished\n");
      }
      break;
    }
    case LWS_CALLBACK_RECEIVE: {
      /* TODO: parse JSON frame (libjson) and dispatch EVENT/REQ/CLOSE/COUNT */
      const char *msg = (const char*)in;
      ConnState *cs = (ConnState*)user;
      const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
      NostrStorage *st_ctx = ctx ? ctx->storage : NULL;
      if (len >= 7 && memcmp(msg, "[\"AUTH\"", 7) == 0) {
        /* NIP-42: verify AUTH event with matching challenge and valid signature */
        const char *payload = strchr(msg, ',');
        if (cs && payload) {
          payload++;
          /* Copy JSON object */
          size_t plen = (size_t)len - (payload - msg);
          while (plen > 0 && (payload[plen-1] == '\n' || payload[plen-1] == '\r' || payload[plen-1] == ' ')) plen--;
          if (plen > 0 && payload[plen-1] == ']') plen--;
          if (plen > 0 && payload[0] == '{') {
            char *pbuf = (char*)malloc(plen + 1);
            if (pbuf) {
              memcpy(pbuf, payload, plen); pbuf[plen] = '\0';
              NostrEvent *ev = nostr_event_new();
              bool ok_parse = false;
              if (nostr_event_deserialize_compact(ev, pbuf)) ok_parse = true;
              else ok_parse = (nostr_event_deserialize(ev, pbuf) == 0);
              if (ok_parse && nostr_event_check_signature(ev)) {
                /* Verify challenge tag */
                NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
                const char *chal = NULL;
                if (tags) {
                  /* Try to find a tag with key "challenge"; fallback to "c" (non-standard) not used */
                  size_t tcount = nostr_tags_size(tags);
                  for (size_t i = 0; i < tcount; i++) {
                    NostrTag *t = nostr_tags_get(tags, i);
                    const char *k = t ? nostr_tag_get_key(t) : NULL;
                    if (k && strcmp(k, "challenge") == 0) { chal = nostr_tag_get_value(t); break; }
                  }
                }
                if (chal && cs->auth_chal[0] && strcmp(chal, cs->auth_chal) == 0) {
                  const char *pk = nostr_event_get_pubkey(ev);
                  if (pk && *pk) {
                    snprintf(cs->authed_pubkey, sizeof(cs->authed_pubkey), "%s", pk);
                    cs->authed = 1;
                    fprintf(stderr, "relayd: AUTH verified pubkey=%s\n", cs->authed_pubkey);
                  }
                } else {
                  fprintf(stderr, "relayd: AUTH rejected (challenge mismatch)\n");
                }
              } else {
                fprintf(stderr, "relayd: AUTH rejected (bad signature or parse)\n");
              }
              if (ev) nostr_event_free(ev);
              free(pbuf);
            }
          }
        }
      } else
      if (len >= 14 && memcmp(msg, "[\"NEGENTROPY\"", 14) == 0) {
        /* NIP-77 stub: CLOSED unsupported */
        const char *p = strchr(msg, ',');
        char subbuf[64]; subbuf[0]='\0';
        if (p) {
          const char *q1 = strchr(p+1, '"'); if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2 && (size_t)(q2-(q1+1))<sizeof(subbuf)) { memcpy(subbuf, q1+1, (size_t)(q2-(q1+1))); subbuf[(size_t)(q2-(q1+1))]='\0'; } }
        }
        char *closed = nostr_closed_build_json(subbuf[0]?subbuf:"sub1", "unsupported: negentropy");
        if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}        
      } else
      if (len >= 7 && memcmp(msg, "[\"COUNT\"", 8) == 0) {
        /* Parse filters from COUNT frame: ["COUNT", <filters...>] */
        const char *filters_json = NULL;
        const char *comma = strchr(msg, ',');
        if (comma && (size_t)(comma - msg) < len) {
          filters_json = comma + 1;
          /* Attempt to strip trailing ']' if present */
          size_t flen = (size_t)len - (filters_json - msg);
          while (flen > 0 && (filters_json[flen-1] == '\n' || filters_json[flen-1] == '\r' || filters_json[flen-1] == ' ')) flen--;
          if (flen > 0 && filters_json[flen-1] == ']') flen--;
          /* Copy to NUL-terminated buffer */
          char *fbuf = (char*)malloc(flen + 1);
          if (fbuf) {
            memcpy(fbuf, filters_json, flen);
            fbuf[flen] = '\0';
            NostrFilter **arr = NULL; size_t n = 0;
            /* TODO: parse filters JSON into NostrFilter array if needed */
            arr = NULL; n = 0;
            /* ctx already defined above */
            if (ctx) {
              if ((int)n > ctx->cfg.max_filters) {
                char *closed = nostr_closed_build_json("count", nostr_limits_reason_invalid_filter());
                if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}                
                if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr);} free(fbuf);
                break;
              }
              /* Clamp per-filter limit */
              for (size_t i=0;i<n;i++) if (arr[i] && arr[i]->limit > ctx->cfg.max_limit) arr[i]->limit = ctx->cfg.max_limit;
            }
            uint64_t count = 0; int rc = -1;
            NostrStorage *st = ctx ? ctx->storage : NULL;
            if (st && st->vt && st->vt->count) rc = st->vt->count(st, (const NostrFilter*)(arr ? arr[0] : NULL), n, &count);
            if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); }
            free(fbuf);
            char resp[128];
            if (rc == 0) snprintf(resp, sizeof(resp), "[\"COUNT\",{}, {\"count\": %llu}]", (unsigned long long)count);
            else snprintf(resp, sizeof(resp), "[\"COUNT\",{}, {\"count\": 0}]");
            unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(resp);
            memcpy(&buf[LWS_PRE], resp, blen);
            lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
          }
        }
      } else if (len >= 7 && memcmp(msg, "[\"EVENT\"", 8) == 0) {
        if (ctx && strcmp(ctx->cfg.auth, "required") == 0 && cs && !cs->authed) {
          /* Gate publish */
          char *ok = nostr_ok_build_json("0000", 0, "auth-required");
          if (ok) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(ok); memcpy(&buf[LWS_PRE], ok, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(ok);}          
          fprintf(stderr, "relayd: EVENT rejected (auth-required)\n");
          break;
        }
        /* EVENT frame: ["EVENT", {event}] */
        const char *ev_json = strchr(msg, ',');
        if (ev_json) {
          ev_json++; /* point to '{' */
          /* Strip trailing ']' */
          size_t elen = (size_t)len - (ev_json - msg);
          while (elen > 0 && (ev_json[elen-1] == '\n' || ev_json[elen-1] == '\r' || ev_json[elen-1] == ' ')) elen--;
          if (elen > 0 && ev_json[elen-1] == ']') elen--;
          char *ebuf = (char*)malloc(elen + 1);
          if (ebuf) {
            memcpy(ebuf, ev_json, elen); ebuf[elen] = '\0';
            NostrEvent *ev = nostr_event_new();
            int ok_parse = 0;
            if (ev) {
              /* Try compact, fall back to general */
              if (nostr_event_deserialize_compact(ev, ebuf)) ok_parse = 1;
              else ok_parse = (nostr_event_deserialize(ev, ebuf) == 0);
            }
            int rc_store = -1;
            const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
            NostrStorage *st = ctx ? ctx->storage : NULL;
            const char *reason = NULL;
            char *id = NULL;
            if (ok_parse && ev && st && st->vt && st->vt->put_event) {
              /* If auth is required and a pubkey is authed, require event pubkey match */
              if (ctx && strcmp(ctx->cfg.auth, "required") == 0 && cs && cs->authed_pubkey[0]) {
                const char *epk = nostr_event_get_pubkey(ev);
                if (!epk || strcmp(epk, cs->authed_pubkey) != 0) {
                  const char *id_hex_tmp = "0000";
                  char *ok = nostr_ok_build_json(id_hex_tmp, 0, "auth-pubkey-mismatch");
                  if (ok) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(ok); memcpy(&buf[LWS_PRE], ok, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(ok);}                  
                  nostr_event_free(ev); free(ebuf);
                  break;
                }
              }
              /* NIP-86 policy enforcement: bans/allow list/kinds */
              const char *epk = nostr_event_get_pubkey(ev);
              int kind = nostr_event_get_kind(ev);
              /* NIP-86 enforcement (optional) */
#ifdef HAVE_NIP86
              if (epk) {
                if (nostr_nip86_is_pubkey_banned(epk)) {
                  char *ok = nostr_ok_build_json("0000", 0, "ban-pubkey");
                  if (ok) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(ok); memcpy(&buf[LWS_PRE], ok, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(ok);}                  
                  nostr_event_free(ev); free(ebuf);
                  break;
                }
                if (nostr_nip86_has_allowlist() && !nostr_nip86_is_pubkey_allowed(epk)) {
                  char *ok = nostr_ok_build_json("0000", 0, "not-allowed");
                  if (ok) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(ok); memcpy(&buf[LWS_PRE], ok, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(ok);}                  
                  nostr_event_free(ev); free(ebuf);
                  break;
                }
              }
              if (nostr_nip86_has_allowed_kinds() && !nostr_nip86_is_kind_allowed(kind)) {
                char *ok = nostr_ok_build_json("0000", 0, "disallowed-kind");
                if (ok) { unsigned char buf[LWS_PRE + 128]; size_t blen = strlen(ok); memcpy(&buf[LWS_PRE], ok, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(ok);}                
                nostr_event_free(ev); free(ebuf);
                break;
              }
#endif
              /* Basic policy: signature must verify */
              if (!nostr_event_check_signature(ev)) {
                reason = "invalid: bad signature";
              } else {
              /* Best-effort upsert for replaceable/parameterized kinds:
               * prior to storing, delete any previous event by same author(+kind).
               * For parameterized kinds, future work: also match on 'd' tag value.
               */
              int kind = nostr_event_get_kind(ev);
              const char *author = nostr_event_get_pubkey(ev);
              if (author && (is_replaceable_kind(kind) || is_param_replaceable_kind(kind))) {
                NostrFilter *ff = nostr_filter_new();
                if (ff) {
                  nostr_filter_add_author(ff, author);
                  nostr_filter_add_kind(ff, kind);
                  /* For parameterized replaceable kinds, include d tag if present */
                  if (is_param_replaceable_kind(kind)) {
                    NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
                    const char *dval = tags ? nostr_tags_get_d(tags) : NULL;
                    if (dval && *dval) {
                      nostr_filter_tags_append(ff, "d", dval, NULL);
                    }
                  }
                  NostrFilter *farr[1] = { ff };
                  int err = 0; void *it = st->vt->query(st, (const NostrFilter*)farr, 1, 1 /*limit*/, 0, 0, &err);
                  if (it) {
                    /* Fetch first match and delete it */
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
              id = nostr_event_get_id(ev);
              rc_store = st->vt->put_event(st, ev);
              reason = rc_store == 0 ? "" : "error: store failed";
              }
            } else {
              reason = "invalid: bad event";
            }
            const char *id_hex = id ? id : "0000";
            char *ok = nostr_ok_build_json(id_hex, rc_store == 0, reason);
            if (ok) {
              unsigned char buf[LWS_PRE + 512];
              size_t blen = strlen(ok);
              memcpy(&buf[LWS_PRE], ok, blen);
              lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
              free(ok);
            }
            if (id) free(id);
            if (ev) nostr_event_free(ev);
            free(ebuf);
          }
        }
      } else if (len >= 7 && memcmp(msg, "[\"REQ\"", 6) == 0) {
        if (ctx && strcmp(ctx->cfg.auth, "required") == 0 && cs && !cs->authed) {
          /* Gate subscription */
          /* Try to extract subid to echo */
          const char *p = strchr(msg, ','); const char *q1 = p?strchr(p+1,'"'):NULL; const char *q2 = q1?strchr(q1+1,'"'):NULL;
          char subtmp[128]; subtmp[0]='\0'; if (q1&&q2 && (size_t)(q2-(q1+1))<sizeof(subtmp)) { memcpy(subtmp, q1+1, (size_t)(q2-(q1+1))); subtmp[(size_t)(q2-(q1+1))]='\0'; }
          char *closed = nostr_closed_build_json(subtmp[0]?subtmp:"sub1", "auth-required");
          if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}          
          fprintf(stderr, "relayd: REQ rejected (auth-required) sub=%s\n", subtmp[0]?subtmp:"sub1");
          break;
        }
        /* Parse REQ frame: ["REQ", "subid", <filters...>] */
        const char *p = strchr(msg, ',');
        const char *subid = NULL; size_t sublen = 0;
        if (p) {
          /* Expect quotes after comma */
          const char *q1 = strchr(p+1, '"');
          if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2) { subid = q1+1; sublen = (size_t)(q2 - (q1+1)); p = strchr(q2, ','); } }
        }
        const char *filters_json = p ? p+1 : NULL;
        const char *sub = subid ? subid : "sub1";
        size_t sub_len = subid ? sublen : strlen("sub1");
        if (filters_json) {
          /* Enforce per-connection max_subs. Currently we support a single active iterator. */
          if (ctx && ctx->cfg.max_subs <= 1 && cs && cs->it) {
            char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
            char *closed = nostr_closed_build_json(subtmp, "too-many-subs");
            if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}            
            fprintf(stderr, "relayd: REQ rejected (too-many-subs) sub=%s\n", subtmp);
            break;
          }
          size_t flen = (size_t)len - (filters_json - msg);
          while (flen > 0 && (filters_json[flen-1] == '\n' || filters_json[flen-1] == '\r' || filters_json[flen-1] == ' ')) flen--;
          if (flen > 0 && filters_json[flen-1] == ']') flen--;
          char *fbuf = (char*)malloc(flen + 1);
          if (fbuf) {
            memcpy(fbuf, filters_json, flen); fbuf[flen] = '\0';
            NostrFilter **arr = NULL; size_t n = 0;
            /* TODO: parse filters JSON into NostrFilter array if needed */
            if (ctx) {
              if ((int)n > ctx->cfg.max_filters) {
                char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
                char *closed = nostr_closed_build_json(subtmp, nostr_limits_reason_invalid_filter());
                if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}                
                if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr);} free(fbuf);
                break;
              }
              /* TODO: clamp per-filter limit after parsing when implemented */
            }
            NostrStorage *st = ctx ? ctx->storage : NULL;
            /* NIP-50 via filter.search: if any filter has search and storage supports search, use it */
            const char *qsearch = NULL; const NostrFilter *scope = NULL;
            for (size_t i=0;i<n;i++) { const char *q = nostr_filter_get_search(arr[i]); if (q && *q) { qsearch = q; scope = arr[i]; break; } }
            if (qsearch && st && st->vt && st->vt->search) {
              void *it = NULL; int rc = st->vt->search(st, qsearch, scope, 0, &it);
              if (rc == -ENOTSUP) {
                char *closed = nostr_closed_build_json(sub, "unsupported: search");
                if (closed) { unsigned char buf[LWS_PRE + 256]; size_t blen = strlen(closed); memcpy(&buf[LWS_PRE], closed, blen); lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT); free(closed);}                
              } else if (rc == 0 && it) {
                ConnState *cs2 = (ConnState*)user;
                if (cs2) {
                  cs2->it = it; size_t cplen = sub_len < sizeof(cs2->subid)-1 ? sub_len : sizeof(cs2->subid)-1; memcpy(cs2->subid, sub, cplen); cs2->subid[cplen]='\0'; lws_callback_on_writable(wsi);
                  fprintf(stderr, "relayd: iterator started (search) sub=%s\n", cs2->subid);
                }
              }
            } else if (st && st->vt && st->vt->query) {
              int err = 0; void *it = st->vt->query(st, (const NostrFilter*)(arr ? arr[0] : NULL), n, 0, 0, 0, &err);
              if (it) {
                ConnState *cs = (ConnState*)user;
                if (cs) {
                  cs->it = it;
                  size_t cplen = sub_len < sizeof(cs->subid)-1 ? sub_len : sizeof(cs->subid)-1;
                  memcpy(cs->subid, sub, cplen); cs->subid[cplen] = '\0';
                  lws_callback_on_writable(wsi);
                  fprintf(stderr, "relayd: iterator started sub=%s\n", cs->subid);
                } else {
                  if (st->vt->query_free) st->vt->query_free(st, it);
                }
              }
            }
            if (arr) { for (size_t i=0;i<n;i++) if (arr[i]) nostr_filter_free(arr[i]); free(arr); }
            free(fbuf);
          }
        }
      } else if (len >= 8 && memcmp(msg, "[\"CLOSE\"", 8) == 0) {
        /* CLOSE frame: ["CLOSE","subid"] -> cancel iterator */
        const char *q1 = strchr(msg, '"');
        const char *q2 = q1 ? strchr(q1+1, '"') : NULL; /* end of CLOSE */
        const char *q3 = q2 ? strchr(q2+1, '"') : NULL; /* start subid */
        const char *q4 = q3 ? strchr(q3+1, '"') : NULL; /* end subid */
        if (q3 && q4 && q4 > q3+1) {
          size_t sl = (size_t)(q4 - (q3+1));
          char subbuf[128];
          size_t cplen = sl < sizeof(subbuf)-1 ? sl : sizeof(subbuf)-1;
          memcpy(subbuf, q3+1, cplen); subbuf[cplen] = '\0';
          ConnState *cs = (ConnState*)user;
          const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
          NostrStorage *st = ctx ? ctx->storage : NULL;
          if (cs && cs->it && cs->subid[0] && strncmp(cs->subid, subbuf, sizeof(cs->subid)) == 0) {
            if (st && st->vt && st->vt->query_free) st->vt->query_free(st, cs->it);
            cs->it = NULL; cs->subid[0] = '\0';
          }
        }
      } else {
        /* Unknown -> CLOSED */
        /* Try to extract subid if present as second element */
        const char *p = strchr(msg, ',');
        char subbuf[128]; subbuf[0]='\0';
        if (p) {
          const char *q1 = strchr(p+1, '"');
          if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2 && (size_t)(q2-(q1+1)) < sizeof(subbuf)) { memcpy(subbuf, q1+1, (size_t)(q2-(q1+1))); subbuf[(size_t)(q2-(q1+1))]='\0'; } }
        }
        const char *sid = subbuf[0]? subbuf : "sub1";
        char *closed = nostr_closed_build_json(sid, "invalid: unsupported frame");
        if (closed) {
          unsigned char buf[LWS_PRE + 256];
          size_t blen = strlen(closed);
          memcpy(&buf[LWS_PRE], closed, blen);
          lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
          free(closed);
        }
      }
      break;
    }
    default: break;
  }
  return 0;
}

static const struct lws_protocols protocols[] = {
  { "http", http_cb, 0, 0 },
  { "nostr", nostr_cb, sizeof(ConnState), 8*1024 },
  { NULL, NULL, 0, 0 }
};

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  /* Initialize JSON backend */
  nostr_json_init();
  /* Load config (simple TOML) */
  RelaydConfig cfg; relayd_config_load("relay.toml", &cfg);

  /* Instantiate storage (driver from config) */
  const char *driver = cfg.storage_driver[0] ? cfg.storage_driver : "nostrdb";
  NostrStorage *st = nostr_storage_create(driver);
  if (!st) {
    fprintf(stderr, "nostrc-relayd: storage '%s' not available; please enable components/nostrdb or choose another driver.\n", driver);
  }

  /* libwebsockets context */
  struct lws_context_creation_info info; memset(&info, 0, sizeof info);
  /* Extract port from cfg.listen */
  int port = 4848; const char *colon = strrchr(cfg.listen, ':'); if (colon) port = atoi(colon+1);
  info.port = port;
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  /* Attach shared context */
  RelaydCtx ctx = { .storage = st, .cfg = cfg };
  info.user = &ctx; /* Make config + storage accessible in protocol callbacks */

  struct lws_context *context = lws_create_context(&info);
  if (!context) {
    fprintf(stderr, "nostrc-relayd: failed to create lws context\n");
    return 1;
  }

  signal(SIGINT, sigint_handler);
  fprintf(stderr, "nostrc-relayd: listening on %s (port %d)\n", cfg.listen, info.port);

  /* Service loop */
  while (!force_exit) {
    lws_service(context, 200);
  }

  lws_context_destroy(context);
  if (st && st->vt && st->vt->close) st->vt->close(st);
  free(st);
  return 0;
}
