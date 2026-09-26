/*
 * libnostr-relay-server core.
 *
 * This file used to be the body of `apps/relayd/src/relayd_main.c`. It was
 * factored out so that both `nostrc-relayd` (system daemon, TCP listener from
 * relay.toml) and the per-user session relay (Unix-fd listener via
 * sd_listen_fds, Wave 3 / #13) can share every code path except how the
 * listening socket is created. See `include/nostr-relay-server.h` for the
 * caller-facing contract.
 */
#include "nostr-relay-server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libwebsockets.h>

#include "metrics.h"
#include "nip11.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"
#include "nostr-relay-core.h"
#include "nostr-relay-limits.h"
#include "protocol_nip01.h"
#include "protocol_nip11.h"
#include "protocol_nip45.h"
#include "protocol_nip50.h"
#include "rate_limit.h"
#include "relay_policy.h"
#include "relayd_conn.h"
#include "relayd_ctx.h"
#include "retention.h"
#include "security_limits.h"
#include "security_limits_runtime.h"
#ifdef HAVE_NIP86
#include "nip86.h"
#endif

/*
 * SIGINT/SIGTERM fallback stop flag. The public API also accepts a caller-
 * owned `stop_flag` in NostrRelayServerConfig; the run loop OR-combines them
 * so tests can trigger shutdown deterministically without raising a signal.
 */
static volatile sig_atomic_t s_signal_stop = 0;

static void handle_stop_signal(int sig) {
  (void)sig;
  s_signal_stop = 1;
}

/* Per-connection HTTP body accumulator for NIP-86 JSON-RPC POST bodies. */
typedef struct {
  int collecting;
  char *body;
  size_t len;
  size_t cap;
  int is_nip86;
  char uri[256];
} HttpState;

/* Secure nonce generator (unchanged from the pre-refactor code path). */
static void gen_nonce(char *out_hex, size_t out_sz) {
  if (!out_hex || out_sz < 33) return; /* need room for 16-byte hex + NUL */
  unsigned char buf[16];
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t r = -1;
  if (fd >= 0) { r = read(fd, buf, sizeof(buf)); close(fd); }
  if (r != (ssize_t)sizeof(buf)) {
    /* Fallback to time-based if urandom not available */
    unsigned long t = (unsigned long)time(NULL);
    for (size_t i = 0; i < sizeof(buf); i++)
      buf[i] = (t >> ((i % sizeof(unsigned long)) * 8)) & 0xFF;
  }
  static const char *hex = "0123456789abcdef";
  size_t j = 0;
  for (size_t i = 0; i < sizeof(buf) && j + 2 < out_sz; i++) {
    out_hex[j++] = hex[(buf[i] >> 4) & 0xF];
    out_hex[j++] = hex[buf[i] & 0xF];
  }
  out_hex[j] = '\0';
}

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

/*
 * Try to accept one waiting connection on `listen_fd` and hand it to lws.
 * Returns 1 if a connection was accepted (successfully or not), 0 if the
 * listener had nothing pending (EAGAIN/EWOULDBLOCK), -1 on a fatal listener
 * error that should terminate the loop.
 *
 * The listener fd is expected to already be in nonblocking mode. We loop
 * across transient EINTR only; the caller drives repeat calls via poll().
 */
static int accept_and_adopt_once(struct lws_context *context, int listen_fd) {
  for (;;) {
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    int cfd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    if (cfd >= 0) {
      /* Match lws's usual client-fd disposition: nonblocking + CLOEXEC. */
      int flags = fcntl(cfd, F_GETFL, 0);
      if (flags >= 0) (void)fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
      int fdflags = fcntl(cfd, F_GETFD, 0);
      if (fdflags >= 0) (void)fcntl(cfd, F_SETFD, fdflags | FD_CLOEXEC);
      struct lws *w = lws_adopt_socket(context, cfd);
      if (!w) {
        /* Adoption failed — close so we don't leak. lws would have closed
         * on success; on failure the fd is still ours. */
        close(cfd);
      }
      return 1;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == ECONNABORTED || errno == EMFILE || errno == ENFILE ||
        errno == ENOBUFS || errno == ENOMEM) {
      /* Transient — log and back off; caller will retry on next tick. */
      fprintf(stderr, "nostr-relay-server: accept(): %s\n", strerror(errno));
      return 0;
    }
    fprintf(stderr, "nostr-relay-server: fatal accept(): %s\n",
            strerror(errno));
    return -1;
  }
}

int nostr_relay_server_run(const NostrRelayServerConfig *server_cfg) {
  if (!server_cfg || !server_cfg->cfg) {
    fprintf(stderr, "nostr-relay-server: null config\n");
    return 1;
  }
  const RelaydConfig *cfg = server_cfg->cfg;

  const int listener_kind = server_cfg->listener.kind;

  if (listener_kind == NOSTR_RELAY_LISTENER_TCP) {
    if (server_cfg->listener.u.tcp.host[0] == '\0' ||
        server_cfg->listener.u.tcp.port <= 0 ||
        server_cfg->listener.u.tcp.port > 65535) {
      fprintf(stderr, "nostr-relay-server: invalid TCP listener host/port\n");
      return 1;
    }
  } else if (listener_kind == NOSTR_RELAY_LISTENER_UNIX_FD) {
    if (server_cfg->listener.u.unix_fd.fd < 0) {
      fprintf(stderr, "nostr-relay-server: invalid Unix listener fd\n");
      return 1;
    }
  } else {
    fprintf(stderr, "nostr-relay-server: unknown listener kind %d\n",
            listener_kind);
    return 1;
  }

  RelayPolicy *policy = relay_policy_create(
      (size_t)cfg->replay_cache_capacity, cfg->replay_ttl_seconds,
      cfg->future_skew_seconds, cfg->past_skew_seconds);
  VerificationBudgetConfig verify_cfg = {
      .per_ip_rate = (uint32_t)cfg->verification_ip_per_sec,
      .per_ip_burst = (uint32_t)cfg->verification_ip_burst,
      .global_rate = (uint32_t)cfg->verification_global_per_sec,
      .global_burst = (uint32_t)cfg->verification_global_burst,
      .max_ip_entries = (size_t)cfg->verification_max_ips,
      .max_inflight_jobs = (size_t)cfg->verification_max_jobs,
      .max_inflight_bytes = (size_t)cfg->verification_max_bytes,
      .negative_cache_entries =
          (size_t)cfg->verification_negative_cache_entries,
      .negative_cache_ttl_ms =
          (uint64_t)cfg->verification_negative_ttl_seconds * 1000ull,
  };
  VerificationBudget *verification_budget =
      verification_budget_create(&verify_cfg, rate_limit_now_ms());
  if (!policy || !verification_budget) {
    fprintf(stderr,
            "nostr-relay-server: failed to allocate ingress security state\n");
    relay_policy_destroy(policy);
    verification_budget_destroy(verification_budget);
    return 1;
  }

  fprintf(stderr,
          "nostr-relay-server: security validator=canonical replayTTL=%ds "
          "replayCapacity=%d skew=+%d/-%d maxEvent=%dB "
          "verify(conn/ip/global)=%d/%d/%d tokens/s "
          "verifyInflight=%d jobs/%dB\n",
          cfg->replay_ttl_seconds, cfg->replay_cache_capacity,
          cfg->future_skew_seconds, cfg->past_skew_seconds,
          cfg->max_event_bytes, cfg->verification_conn_per_sec,
          cfg->verification_ip_per_sec, cfg->verification_global_per_sec,
          cfg->verification_max_jobs, cfg->verification_max_bytes);

  if (!server_cfg->storage) {
    fprintf(stderr,
            "nostr-relay-server: no storage backend supplied; queries will "
            "return empty results.\n");
  }

  /* libwebsockets context — differs only in whether lws owns the listen fd.
   *
   * TCP path: hand lws the host:port and let it bind.
   * Unix-fd path: pre-bound listen fd owned by the caller (systemd or the
   *   session daemon's fallback path). We disable lws's own listener
   *   (`CONTEXT_PORT_NO_LISTEN`) and drive an accept loop below, handing
   *   each accepted client fd to lws_adopt_socket(). This keeps the peer-
   *   credential check at the daemon boundary — no byte crosses the lws
   *   protocol layer until the UID is verified. */
  struct lws_context_creation_info info; memset(&info, 0, sizeof info);
  if (listener_kind == NOSTR_RELAY_LISTENER_TCP) {
    info.iface = server_cfg->listener.u.tcp.host;
    info.port = server_cfg->listener.u.tcp.port;
  } else {
    info.iface = NULL;
    info.port = CONTEXT_PORT_NO_LISTEN;
  }
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

  RelaydCtx ctx = {
      .storage = server_cfg->storage,
      .cfg = *cfg,
      .policy = policy,
      .verification_budget = verification_budget,
  };
  info.user = &ctx;

  struct lws_context *context = lws_create_context(&info);
  if (!context) {
    fprintf(stderr, "nostr-relay-server: failed to create lws context\n");
    relay_policy_destroy(policy);
    verification_budget_destroy(verification_budget);
    return 1;
  }

  /* Install stop-signal handlers, preserving previous dispositions so we do
   * not stomp on the caller's handlers when the loop returns. */
  s_signal_stop = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = handle_stop_signal;
  sigemptyset(&sa.sa_mask);
  struct sigaction prev_int, prev_term;
  sigaction(SIGINT, &sa, &prev_int);
  sigaction(SIGTERM, &sa, &prev_term);

  int listen_fd = -1;
  if (listener_kind == NOSTR_RELAY_LISTENER_UNIX_FD) {
    listen_fd = server_cfg->listener.u.unix_fd.fd;
    /* Put listener into nonblocking mode so accept() cannot stall the loop.
     * systemd's activation fds are typically already nonblocking, but be
     * defensive. */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0 && !(flags & O_NONBLOCK))
      (void)fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    fprintf(stderr,
            "nostr-relay-server: listening on Unix fd %d (per-user session)\n",
            listen_fd);
  } else {
    fprintf(stderr, "nostr-relay-server: listening on %s:%d\n",
            info.iface, info.port);
  }

  unsigned long long last_ret_ms = 0;
  const volatile int *external_stop = server_cfg->stop_flag;
  while (!s_signal_stop && !(external_stop && *external_stop)) {
    /* TCP path: lws owns the listen socket and drives its own poll loop.
     * Unix-fd path: we drive an accept-and-adopt loop for the pre-bound
     * listen fd, and let lws service already-adopted client fds. Both
     * paths share the same retention-tick and stop-signal handling. */
    if (listener_kind == NOSTR_RELAY_LISTENER_UNIX_FD) {
      struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
      int prc = poll(&pfd, 1, 50);
      if (prc > 0 && (pfd.revents & POLLIN)) {
        /* Drain everything currently pending before returning to lws so a
         * burst of connects does not starve accepted-side servicing. */
        for (;;) {
          int arc = accept_and_adopt_once(context, listen_fd);
          if (arc <= 0) {
            if (arc < 0) s_signal_stop = 1;
            break;
          }
        }
      } else if (prc < 0 && errno != EINTR) {
        fprintf(stderr, "nostr-relay-server: poll(listen): %s\n",
                strerror(errno));
        s_signal_stop = 1;
      }
      /* Service already-adopted clients. Short timeout because the accept
       * poll above already gave the loop its wait; we do not want to block
       * a second time when a client fd has activity. */
      lws_service(context, 0);
    } else {
      lws_service(context, 200);
    }

    unsigned long long now_ms = rate_limit_now_ms();
    if (last_ret_ms == 0 || now_ms - last_ret_ms >= 5000) {
      retention_tick(&ctx);
      last_ret_ms = now_ms;
    }
  }

  lws_context_destroy(context);
  sigaction(SIGINT, &prev_int, NULL);
  sigaction(SIGTERM, &prev_term, NULL);
  relay_policy_destroy(policy);
  verification_budget_destroy(verification_budget);
  /* Note: on the Unix-fd path we deliberately do NOT close listen_fd — the
   * caller retains ownership (typically systemd, so the same fd can be
   * reused on the next activation cycle without a race). */
  return 0;
}
