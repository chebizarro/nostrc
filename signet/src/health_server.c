/* SPDX-License-Identifier: MIT
 *
 * health_server.c - Minimal /health endpoint.
 *
 * Production goals:
 * - Only serve GET /health (monitoring only; no management operations).
 * - Fast responses built from an in-memory snapshot (no blocking probes).
 * - Thread-safe snapshot updates.
 * - Graceful shutdown.
 *
 * This implementation uses a small socket-based server to avoid extra deps.
 */

#include "signet/health_server.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#define SIGNET_HEALTH_BACKLOG 64
#define SIGNET_HEALTH_MAX_REQ 4096

struct SignetHealthServer {
  char *listen;

  int listen_fd;
  GThread *thr;

  GMutex mu;
  SignetHealthSnapshot snap;

  int64_t started_at_unix;

  gint stop_flag; /* atomic int */
};

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

static bool signet_parse_listen(const char *listen, char **out_host, char **out_port) {
  *out_host = NULL;
  *out_port = NULL;

  if (!listen || listen[0] == '\0') return false;

  const char *colon = strrchr(listen, ':');
  if (!colon || colon == listen || colon[1] == '\0') return false;

  *out_host = g_strndup(listen, (gsize)(colon - listen));
  *out_port = g_strdup(colon + 1);
  if (!*out_host || !*out_port) {
    g_free(*out_host);
    g_free(*out_port);
    *out_host = NULL;
    *out_port = NULL;
    return false;
  }

  return true;
}

static int signet_bind_listen_tcp(const char *host, const char *port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo((host && host[0] != '\0') ? host : NULL, port, &hints, &res);
  if (rc != 0 || !res) return -1;

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      if (listen(fd, SIGNET_HEALTH_BACKLOG) == 0) {
        freeaddrinfo(res);
        return fd;
      }
    }

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  return -1;
}

static bool signet_read_request_line(int fd, char *out, size_t out_cap) {
  size_t used = 0;
  while (used + 1 < out_cap) {
    char c = 0;
    ssize_t n = recv(fd, &c, 1, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }

    out[used++] = c;
    out[used] = '\0';

    if (used >= 2 && out[used - 2] == '\r' && out[used - 1] == '\n') {
      out[used - 2] = '\0';
      return true;
    }

    /* hard stop if request line is too big */
    if (used >= SIGNET_HEALTH_MAX_REQ - 1) return false;
  }

  return false;
}

static int signet_write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, p + off, len - off, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static char *signet_build_health_json(const SignetHealthSnapshot *snap,
                                      int64_t now_unix,
                                      uint64_t uptime_seconds) {
  const char *relay_s = (snap && snap->relay_connected) ? "connected" : "disconnected";
  const char *vault_s = (snap && snap->vault_reachable) ? "reachable" : "unreachable";
  const char *policy_s = (snap && snap->policy_store_loaded) ? "loaded" : "error";
  const char *keystore_s = (snap && snap->key_store_available) ? "available" : "unavailable";

  const char *overall = "ok";
  if (!(snap && snap->key_store_available) || !(snap && snap->policy_store_loaded)) {
    overall = "error";
  } else if (!(snap && snap->relay_connected) || !(snap && snap->vault_reachable)) {
    overall = "degraded";
  }

  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "status");
  json_builder_add_string_value(b, overall);

  json_builder_set_member_name(b, "timestamp");
  json_builder_add_int_value(b, (gint64)now_unix);

  json_builder_set_member_name(b, "uptime_seconds");
  json_builder_add_int_value(b, (gint64)uptime_seconds);

  json_builder_set_member_name(b, "components");
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "relay_pool");
  json_builder_add_string_value(b, relay_s);

  json_builder_set_member_name(b, "vault_client");
  json_builder_add_string_value(b, vault_s);

  json_builder_set_member_name(b, "policy_store");
  json_builder_add_string_value(b, policy_s);

  json_builder_set_member_name(b, "key_store");
  json_builder_add_string_value(b, keystore_s);

  json_builder_end_object(b);
  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  return out; /* g_free */
}

static void signet_handle_conn(SignetHealthServer *hs, int fd) {
  char line[SIGNET_HEALTH_MAX_REQ];
  memset(line, 0, sizeof(line));

  if (!signet_read_request_line(fd, line, sizeof(line))) {
    return;
  }

  /* Very small parser: "<METHOD> <PATH> HTTP/1.x" */
  char method[16];
  char path[256];
  memset(method, 0, sizeof(method));
  memset(path, 0, sizeof(path));

  if (sscanf(line, "%15s %255s", method, path) != 2) {
    return;
  }

  if (strcmp(method, "GET") != 0 || strcmp(path, "/health") != 0) {
    const char *body = "{\"error\":\"not found\"}\n";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     strlen(body));
    if (n > 0) {
      (void)signet_write_all(fd, hdr, (size_t)n);
      (void)signet_write_all(fd, body, strlen(body));
    }
    return;
  }

  SignetHealthSnapshot snap_copy;
  memset(&snap_copy, 0, sizeof(snap_copy));

  g_mutex_lock(&hs->mu);
  snap_copy = hs->snap;
  int64_t started_at = hs->started_at_unix;
  g_mutex_unlock(&hs->mu);

  int64_t now = signet_now_unix();
  uint64_t uptime = 0;
  if (started_at > 0 && now >= started_at) uptime = (uint64_t)(now - started_at);
  else uptime = snap_copy.uptime_sec;

  char *json = signet_build_health_json(&snap_copy, now, uptime);
  if (!json) {
    const char *body = "{\"status\":\"error\"}\n";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     strlen(body));
    if (n > 0) {
      (void)signet_write_all(fd, hdr, (size_t)n);
      (void)signet_write_all(fd, body, strlen(body));
    }
    return;
  }

  size_t json_len = strlen(json);
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   json_len);

  if (n > 0) {
    (void)signet_write_all(fd, hdr, (size_t)n);
    (void)signet_write_all(fd, json, json_len);
  }

  g_free(json);
}

static gpointer signet_health_thread_main(gpointer data) {
  SignetHealthServer *hs = (SignetHealthServer *)data;

  while (g_atomic_int_get(&hs->stop_flag) == 0) {
    int fd = accept(hs->listen_fd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) continue;
      /* If we're stopping, accept() may fail after close/shutdown. */
      if (g_atomic_int_get(&hs->stop_flag) != 0) break;
      /* brief sleep to avoid a tight loop on repeated errors */
      g_usleep(50 * 1000);
      continue;
    }

    /* Handle one request per connection; close afterwards. */
    signet_handle_conn(hs, fd);
    close(fd);
  }

  return NULL;
}

SignetHealthServer *signet_health_server_new(const SignetHealthServerConfig *cfg) {
  if (!cfg || !cfg->listen) return NULL;

  SignetHealthServer *hs = (SignetHealthServer *)calloc(1, sizeof(*hs));
  if (!hs) return NULL;

  hs->listen = g_strdup(cfg->listen);
  if (!hs->listen) {
    free(hs);
    return NULL;
  }

  hs->listen_fd = -1;
  hs->thr = NULL;
  g_mutex_init(&hs->mu);

  memset(&hs->snap, 0, sizeof(hs->snap));
  hs->snap.relay_connected = false;
  hs->snap.vault_reachable = false;
  hs->snap.uptime_sec = 0;
  hs->snap.policy_store_loaded = false;
  hs->snap.key_store_available = false;

  hs->started_at_unix = 0;
  g_atomic_int_set(&hs->stop_flag, 0);

  return hs;
}

void signet_health_server_free(SignetHealthServer *hs) {
  if (!hs) return;

  signet_health_server_stop(hs);

  g_mutex_clear(&hs->mu);
  g_free(hs->listen);
  free(hs);
}

int signet_health_server_start(SignetHealthServer *hs) {
  if (!hs) return -1;

  /* already started */
  if (hs->thr != NULL) return 0;

  char *host = NULL;
  char *port = NULL;
  if (!signet_parse_listen(hs->listen, &host, &port)) {
    g_free(host);
    g_free(port);
    return -1;
  }

  int fd = signet_bind_listen_tcp(host, port);
  g_free(host);
  g_free(port);

  if (fd < 0) return -1;

  hs->listen_fd = fd;

  g_mutex_lock(&hs->mu);
  hs->started_at_unix = signet_now_unix();
  g_mutex_unlock(&hs->mu);

  g_atomic_int_set(&hs->stop_flag, 0);
  hs->thr = g_thread_new("signet-health", signet_health_thread_main, hs);
  if (!hs->thr) {
    close(hs->listen_fd);
    hs->listen_fd = -1;
    return -1;
  }

  return 0;
}

void signet_health_server_stop(SignetHealthServer *hs) {
  if (!hs) return;

  if (!hs->thr) return;

  g_atomic_int_set(&hs->stop_flag, 1);

  if (hs->listen_fd >= 0) {
    shutdown(hs->listen_fd, SHUT_RDWR);
    close(hs->listen_fd);
    hs->listen_fd = -1;
  }

  g_thread_join(hs->thr);
  hs->thr = NULL;
}

void signet_health_server_set_snapshot(SignetHealthServer *hs, const SignetHealthSnapshot *snap) {
  if (!hs || !snap) return;

  g_mutex_lock(&hs->mu);
  hs->snap = *snap;
  g_mutex_unlock(&hs->mu);
}