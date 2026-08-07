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
#include "relayd_ctx.h"
#include "protocol_nip50.h"
#include "rate_limit.h"
#include "retention.h"
#include "protocol_nip45.h"
#include "protocol_nip11.h"
#include "relayd_conn.h"
#include "protocol_nip01.h"
#include "metrics.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "security_limits.h"
#include "security_limits_runtime.h"
#include "relay_policy.h"
#ifdef HAVE_NIP86
#include "nip86.h"
#endif

static volatile int force_exit = 0;

static void sigint_handler(int sig){ (void)sig; force_exit = 1; }

/* Minimal HTTP handler to serve NIP-11 JSON at GET / (placeholder). */

/* Per-connection user data moved to relayd_conn.h */

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
        metrics_on_connect();
        return 0;
      }
      if (uri && strcmp(uri, "/") == 0) {
        /* Handle CORS preflight for NIP-11 endpoint */
        if (lws_hdr_total_length(wsi, WSI_TOKEN_OPTIONS_URI) > 0) {
          (void)relayd_handle_nip11_options(wsi);
          return -1;
        }
        const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
        (void)relayd_handle_nip11_root(wsi, ctx);
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
        /* Minimal admin methods before delegating to nip86 module */
        if (hs->body && strstr(hs->body, "\"method\":\"supportedmethods\"")) {
          const char *list = "{\"result\":[\"getstats\",\"supportedmethods\"]}";
          resp = strdup(list);
          http_status = 200;
        } else if (hs->body && strstr(hs->body, "\"method\":\"getstats\"")) {
          resp = metrics_build_json();
          http_status = resp ? 200 : 500;
        } else if (hs->body && strstr(hs->body, "\"method\":\"getlimits\"")) {
          const RelaydCtx *rctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
          if (rctx) {
            char buf[1024];
            int n2 = snprintf(buf, sizeof(buf),
              "{\"result\":{\"max_filters\":%d,\"max_limit\":%d,\"max_subs\":%d,\"max_event_bytes\":%d,\"rate_ops_per_sec\":%d,\"rate_burst\":%d,\"rate_event_cost\":%d,\"replay_ttl_seconds\":%d,\"future_skew_seconds\":%d,\"past_skew_seconds\":%d,\"verification_conn_per_sec\":%d,\"verification_ip_per_sec\":%d,\"verification_global_per_sec\":%d,\"verification_max_jobs\":%d,\"verification_max_bytes\":%d,\"negentropy_enabled\":%d,\"auth\":\"%s\",\"storage_driver\":\"%s\",\"listen\":\"%s\"}}",
              rctx->cfg.max_filters, rctx->cfg.max_limit, rctx->cfg.max_subs,
              rctx->cfg.max_event_bytes, rctx->cfg.rate_ops_per_sec,
              rctx->cfg.rate_burst, rctx->cfg.rate_event_cost,
              rctx->cfg.replay_ttl_seconds, rctx->cfg.future_skew_seconds,
              rctx->cfg.past_skew_seconds,
              rctx->cfg.verification_conn_per_sec,
              rctx->cfg.verification_ip_per_sec,
              rctx->cfg.verification_global_per_sec,
              rctx->cfg.verification_max_jobs,
              rctx->cfg.verification_max_bytes,
              rctx->cfg.negentropy_enabled, rctx->cfg.auth,
              rctx->cfg.storage_driver, rctx->cfg.listen);
            resp = (n2>0) ? strndup(buf, (size_t)n2) : NULL; http_status = resp?200:500;
          } else {
            resp = strdup("{\"error\":\"noctx\"}"); http_status = 500;
          }
        } else if (hs->body && strstr(hs->body, "\"method\":\"getconnections\"")) {
          /* Quick alias to connections subset of getstats */
          char *m = metrics_build_json();
          if (m) {
            /* naive extraction is fine: clients should prefer getstats */
            resp = m; http_status = 200;
          } else {
            resp = strdup("{\"error\":\"nometrics\"}"); http_status = 500;
          }
        }
#ifdef HAVE_NIP86
        if (!resp) resp = nostr_nip86_process_request((void*)lws_context_user(lws_get_context(wsi)), auth, hs->body, method, url, &http_status);
#else
        if (!resp) { http_status = 501; resp = strdup("{\"error\":\"nip86 disabled\"}"); }
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
        /* Monotonic weighted frame and verification buckets. */
        uint64_t now_ms = rate_limit_now_ms();
        rate_limit_bucket_init(&cs->frame_rate,
                               (uint32_t)ctx->cfg.rate_ops_per_sec,
                               (uint32_t)ctx->cfg.rate_burst, now_ms);
        rate_limit_bucket_init(
            &cs->byte_rate,
            (uint32_t)nostr_limit_max_bytes_per_sec(),
            (uint32_t)nostr_limit_max_bytes_per_sec(), now_ms);
        rate_limit_bucket_init(&cs->verification_rate,
                               (uint32_t)ctx->cfg.verification_conn_per_sec,
                               (uint32_t)ctx->cfg.verification_conn_burst,
                               now_ms);
        if (!lws_get_peer_simple(wsi, cs->peer_ip, sizeof(cs->peer_ip)))
          snprintf(cs->peer_ip, sizeof(cs->peer_ip), "%s", "unknown");
        if (ctx && strcmp(ctx->cfg.auth, "off") != 0) {
          gen_nonce(cs->auth_chal, sizeof(cs->auth_chal));
          cs->need_auth_chal = (cs->auth_chal[0] != '\0');
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
      metrics_on_connect();
      fprintf(stderr, "relayd: client connected\n");
      break;
    case LWS_CALLBACK_CLOSED:
      {
        ConnState *cs = (ConnState *)user;
        const RelaydCtx *ctx =
            (const RelaydCtx *)lws_context_user(lws_get_context(wsi));
        if (cs && ctx && cs->it && ctx->storage && ctx->storage->vt &&
            ctx->storage->vt->query_free)
          ctx->storage->vt->query_free(ctx->storage, cs->it);
        if (cs && ctx && cs->neg_state && ctx->storage && ctx->storage->vt &&
            ctx->storage->vt->set_free)
          ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
        if (cs) {
          free(cs->rx_buffer);
          cs->rx_buffer = NULL;
          cs->rx_length = cs->rx_capacity = 0;
        }
      }
      metrics_on_disconnect();
      break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
      relayd_nip01_on_writable(wsi, (ConnState*)user, (const RelaydCtx*)lws_context_user(lws_get_context(wsi)));
      break;
    case LWS_CALLBACK_RECEIVE: {
      const RelaydCtx *ctx = (const RelaydCtx*)lws_context_user(lws_get_context(wsi));
      ConnState *cs = (ConnState *)user;
      if (!ctx || !cs ||
          !rate_limit_bucket_allow(&cs->byte_rate, rate_limit_now_ms(),
                                   len > UINT32_MAX ? UINT32_MAX
                                                    : (uint32_t)len))
        return -1;
      if (len > NOSTR_MAX_FRAME_LEN_BYTES - cs->rx_length) {
        metrics_on_oversize_reject();
        free(cs->rx_buffer);
        cs->rx_buffer = NULL;
        cs->rx_length = cs->rx_capacity = 0;
        return -1;
      }
      size_t needed = cs->rx_length + len + 1;
      if (needed > cs->rx_capacity) {
        size_t capacity = cs->rx_capacity ? cs->rx_capacity : 4096;
        while (capacity < needed && capacity < NOSTR_MAX_FRAME_LEN_BYTES + 1)
          capacity *= 2;
        if (capacity > NOSTR_MAX_FRAME_LEN_BYTES + 1)
          capacity = NOSTR_MAX_FRAME_LEN_BYTES + 1;
        char *grown = realloc(cs->rx_buffer, capacity);
        if (!grown) return -1;
        cs->rx_buffer = grown;
        cs->rx_capacity = capacity;
      }
      memcpy(cs->rx_buffer + cs->rx_length, in, len);
      cs->rx_length += len;
      if (!lws_is_final_fragment(wsi) ||
          lws_remaining_packet_payload(wsi) != 0)
        break;
      cs->rx_buffer[cs->rx_length] = '\0';
      size_t message_len = cs->rx_length;
      cs->rx_length = 0;
      relayd_nip01_on_receive(wsi, cs, ctx, cs->rx_buffer, message_len);
      break;
    }
    default: break;
  }
  return 0;
}

static const struct lws_protocols protocols[] = {
  { "http", http_cb, 0, 0 },
  { "nostr", nostr_cb, sizeof(ConnState), NOSTR_MAX_FRAME_LEN_BYTES },
  { NULL, NULL, 0, 0 }
};

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  /* Initialize JSON backend */
  nostr_json_init();
  RelaydConfig cfg;
  if (relayd_config_load("relay.toml", &cfg) != 0) {
    fprintf(stderr, "nostrc-relayd: invalid relay.toml security limits\n");
    return 1;
  }

  RelayPolicy *policy = relay_policy_create(
      (size_t)cfg.replay_cache_capacity, cfg.replay_ttl_seconds,
      cfg.future_skew_seconds, cfg.past_skew_seconds);
  VerificationBudgetConfig verify_cfg = {
      .per_ip_rate = (uint32_t)cfg.verification_ip_per_sec,
      .per_ip_burst = (uint32_t)cfg.verification_ip_burst,
      .global_rate = (uint32_t)cfg.verification_global_per_sec,
      .global_burst = (uint32_t)cfg.verification_global_burst,
      .max_ip_entries = (size_t)cfg.verification_max_ips,
      .max_inflight_jobs = (size_t)cfg.verification_max_jobs,
      .max_inflight_bytes = (size_t)cfg.verification_max_bytes,
      .negative_cache_entries =
          (size_t)cfg.verification_negative_cache_entries,
      .negative_cache_ttl_ms =
          (uint64_t)cfg.verification_negative_ttl_seconds * 1000ull,
  };
  VerificationBudget *verification_budget =
      verification_budget_create(&verify_cfg, rate_limit_now_ms());
  if (!policy || !verification_budget) {
    fprintf(stderr, "nostrc-relayd: failed to allocate ingress security state\n");
    relay_policy_destroy(policy);
    verification_budget_destroy(verification_budget);
    return 1;
  }

  fprintf(stderr,
          "nostrc-relayd: security validator=canonical replayTTL=%ds "
          "replayCapacity=%d skew=+%d/-%d maxEvent=%dB "
          "verify(conn/ip/global)=%d/%d/%d tokens/s "
          "verifyInflight=%d jobs/%dB\n",
          cfg.replay_ttl_seconds, cfg.replay_cache_capacity,
          cfg.future_skew_seconds, cfg.past_skew_seconds,
          cfg.max_event_bytes, cfg.verification_conn_per_sec,
          cfg.verification_ip_per_sec, cfg.verification_global_per_sec,
          cfg.verification_max_jobs, cfg.verification_max_bytes);

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
  RelaydCtx ctx = {
      .storage = st,
      .cfg = cfg,
      .policy = policy,
      .verification_budget = verification_budget,
  };
  info.user = &ctx; /* Make config + storage accessible in protocol callbacks */

  struct lws_context *context = lws_create_context(&info);
  if (!context) {
    fprintf(stderr, "nostrc-relayd: failed to create lws context\n");
    relay_policy_destroy(policy);
    verification_budget_destroy(verification_budget);
    if (st && st->vt && st->vt->close) st->vt->close(st);
    free(st);
    return 1;
  }

  signal(SIGINT, sigint_handler);
  fprintf(stderr, "nostrc-relayd: listening on %s (port %d)\n", cfg.listen, info.port);

  /* Service loop */
  unsigned long long last_ret_ms = 0;
  while (!force_exit) {
    lws_service(context, 200);
    /* Periodic retention tick every 5s */
    unsigned long long now_ms = rate_limit_now_ms();
    if (last_ret_ms == 0 || now_ms - last_ret_ms >= 5000) {
      retention_tick(&ctx);
      last_ret_ms = now_ms;
    }
  }

  lws_context_destroy(context);
  if (st && st->vt && st->vt->close) st->vt->close(st);
  free(st);
  relay_policy_destroy(policy);
  verification_budget_destroy(verification_budget);
  return 0;
}
