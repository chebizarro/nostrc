/* SPDX-License-Identifier: MIT
 *
 * audit_logger.c - Atomic-append JSONL audit logger with SIGHUP reopen and
 *                  thread safety.
 */

#include "signet/audit_logger.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#if defined(__linux__)
#  include <sys/types.h>
#endif

struct SignetAuditLogger {
  char *path;
  bool to_stdout;
  bool flush_each_write;

  int fd;         /* -1 disabled, otherwise open O_APPEND FD */
  bool enabled;   /* true if fd is usable */

  GMutex mu;
};

static volatile sig_atomic_t g_reopen_requested = 0;

static void signet_sighup_handler(int signo) {
  (void)signo;
  g_reopen_requested = 1;
}

void signet_audit_logger_request_reopen(void) {
  g_reopen_requested = 1;
}

int signet_audit_logger_install_sighup_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signet_sighup_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  if (sigaction(SIGHUP, &sa, NULL) != 0) return -1;
  return 0;
}

const char *signet_audit_event_type_to_string(SignetAuditEventType type) {
  switch (type) {
    case SIGNET_AUDIT_EVENT_STARTUP: return "startup";
    case SIGNET_AUDIT_EVENT_SHUTDOWN: return "shutdown";
    case SIGNET_AUDIT_EVENT_POLICY_DECISION: return "policy_decision";
    case SIGNET_AUDIT_EVENT_KEY_ACCESS: return "key_access";
    case SIGNET_AUDIT_EVENT_SIGN_REQUEST: return "sign_request";
    case SIGNET_AUDIT_EVENT_SIGN_RESPONSE: return "sign_response";
    case SIGNET_AUDIT_EVENT_REPLAY_REJECTED: return "replay_rejected";
    case SIGNET_AUDIT_EVENT_MGMT_APPLIED: return "mgmt_applied";
    case SIGNET_AUDIT_EVENT_ERROR: return "error";
    case SIGNET_AUDIT_EVENT_UNSPECIFIED:
    default: return "unspecified";
  }
}

static int signet_open_append_fd(const char *path) {
  if (!path || path[0] == '\0') return -1;

  /* Restrictive perms: audit logs should not be world-readable by default. */
  int flags = O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC;
  int fd = open(path, flags, 0600);
  return fd;
}

static int signet_write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t left = len;

  while (left > 0) {
    ssize_t w = write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) return -1;
    p += (size_t)w;
    left -= (size_t)w;
  }
  return 0;
}

static int signet_sync_fd(int fd) {
#if defined(__linux__)
  /* fdatasync is lighter than fsync for typical files. */
  if (fdatasync(fd) == 0) return 0;
  return -1;
#else
  if (fsync(fd) == 0) return 0;
  return -1;
#endif
}

static bool signet_is_disabled(const SignetAuditLogger *l) {
  return (!l || !l->enabled || l->fd < 0);
}

static int signet_audit_logger_reopen_locked(SignetAuditLogger *l) {
  if (!l) return -1;
  if (!l->enabled) return 0;
  if (l->to_stdout) return 0;
  if (!l->path || l->path[0] == '\0') return 0;

  int new_fd = signet_open_append_fd(l->path);
  if (new_fd < 0) return -1;

  int old_fd = l->fd;
  l->fd = new_fd;

  if (old_fd >= 0) close(old_fd);
  return 0;
}

static int signet_audit_write_record(SignetAuditLogger *l, const char *line) {
  if (!l || !line) return -1;
  if (signet_is_disabled(l)) return 0;

  size_t n = strlen(line);
  if (n == 0) return -1;

  /* One write call per line + newline for best-effort atomicity with O_APPEND. */
  int rc = signet_write_all(l->fd, line, n);
  if (rc != 0) return -1;
  rc = signet_write_all(l->fd, "\n", 1);
  if (rc != 0) return -1;

  if (l->flush_each_write) {
    (void)signet_sync_fd(l->fd);
  }
  return 0;
}

static int signet_audit_log_payload_node(SignetAuditLogger *l,
                                         SignetAuditEventType type,
                                         JsonNode *payload_object_node) {
  if (!l) return -1;
  if (signet_is_disabled(l)) return 0;
  if (!payload_object_node || !JSON_NODE_HOLDS_OBJECT(payload_object_node)) return -1;

  /* Timestamp: real time (unix). */
  gint64 rt_us = g_get_real_time();
  gint64 ts_sec = rt_us / G_USEC_PER_SEC;
  gint64 ts_us = rt_us % G_USEC_PER_SEC;

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ts");
  json_builder_add_int_value(b, ts_sec);

  json_builder_set_member_name(b, "ts_us");
  json_builder_add_int_value(b, ts_us);

  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, signet_audit_event_type_to_string(type));

  json_builder_set_member_name(b, "type_id");
  json_builder_add_int_value(b, (gint64)type);

  json_builder_set_member_name(b, "data");
  json_builder_add_value(b, json_node_copy(payload_object_node));

  json_builder_end_object(b);

  JsonNode *root = json_builder_get_root(b);
  if (!root) return -1;

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);

  gchar *out = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  if (!out) return -1;

  int rc = 0;

  g_mutex_lock(&l->mu);

  if (g_reopen_requested) {
    g_reopen_requested = 0;
    (void)signet_audit_logger_reopen_locked(l);
  }

  /* Write; on failure try a single reopen+retry for rotated/unlinked files. */
  rc = signet_audit_write_record(l, out);
  if (rc != 0 && l->enabled && !l->to_stdout && l->path && l->path[0] != '\0') {
    if (signet_audit_logger_reopen_locked(l) == 0) {
      rc = signet_audit_write_record(l, out);
    }
  }

  g_mutex_unlock(&l->mu);

  g_free(out);
  return rc;
}

SignetAuditLogger *signet_audit_logger_new(const SignetAuditLoggerConfig *cfg) {
  if (!cfg) return NULL;

  SignetAuditLogger *l = (SignetAuditLogger *)calloc(1, sizeof(*l));
  if (!l) return NULL;

  g_mutex_init(&l->mu);

  l->to_stdout = cfg->to_stdout;
  l->flush_each_write = cfg->flush_each_write;
  l->fd = -1;
  l->enabled = false;

  if (cfg->path && cfg->path[0] != '\0') {
    l->path = strdup(cfg->path);
    if (!l->path) {
      g_mutex_clear(&l->mu);
      free(l);
      return NULL;
    }
  }

  if (l->to_stdout) {
    l->fd = STDOUT_FILENO;
    l->enabled = true;
    return l;
  }

  /* Disabled mode (no-op) if no output was configured. */
  if (!l->path || l->path[0] == '\0') {
    l->fd = -1;
    l->enabled = false;
    return l;
  }

  l->fd = signet_open_append_fd(l->path);
  if (l->fd < 0) {
    free(l->path);
    g_mutex_clear(&l->mu);
    free(l);
    return NULL;
  }

  l->enabled = true;
  return l;
}

void signet_audit_logger_free(SignetAuditLogger *l) {
  if (!l) return;

  g_mutex_lock(&l->mu);

  if (l->enabled && l->fd >= 0 && !l->to_stdout) {
    close(l->fd);
    l->fd = -1;
  }
  l->enabled = false;

  g_mutex_unlock(&l->mu);

  free(l->path);
  g_mutex_clear(&l->mu);
  free(l);
}

int signet_audit_logger_reopen(SignetAuditLogger *l) {
  if (!l) return -1;
  if (!l->enabled) return 0;
  if (l->to_stdout) return 0;

  g_mutex_lock(&l->mu);
  int rc = signet_audit_logger_reopen_locked(l);
  g_mutex_unlock(&l->mu);

  return rc;
}

static JsonNode *signet_parse_json_object_node(const char *json_object) {
  if (!json_object) return NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json_object, -1, NULL)) return NULL;

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;

  return json_node_copy(root);
}

int signet_audit_log_json(SignetAuditLogger *l,
                          SignetAuditEventType type,
                          const char *json_object) {
  if (!l) return -1;
  if (signet_is_disabled(l)) return 0;
  if (!json_object) return -1;

  JsonNode *payload = signet_parse_json_object_node(json_object);
  if (!payload) return -1;

  int rc = signet_audit_log_payload_node(l, type, payload);
  json_node_unref(payload);
  return rc;
}

char *signet_audit_build_common_payload_json(const SignetAuditCommonFields *fields,
                                             const char *details_json_object) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  if (fields) {
    if (fields->client_pubkey_hex && fields->client_pubkey_hex[0] != '\0') {
      json_builder_set_member_name(b, "client_pubkey");
      json_builder_add_string_value(b, fields->client_pubkey_hex);
    }
    if (fields->identity && fields->identity[0] != '\0') {
      json_builder_set_member_name(b, "identity");
      json_builder_add_string_value(b, fields->identity);
    }
    if (fields->method && fields->method[0] != '\0') {
      json_builder_set_member_name(b, "method");
      json_builder_add_string_value(b, fields->method);
    }
    if (fields->event_kind >= 0) {
      json_builder_set_member_name(b, "event_kind");
      json_builder_add_int_value(b, (gint64)fields->event_kind);
    }
    if (fields->decision && fields->decision[0] != '\0') {
      json_builder_set_member_name(b, "decision");
      json_builder_add_string_value(b, fields->decision);
    }
    if (fields->reason_code && fields->reason_code[0] != '\0') {
      json_builder_set_member_name(b, "reason");
      json_builder_add_string_value(b, fields->reason_code);
    }
  }

  if (details_json_object && details_json_object[0] != '\0') {
    JsonNode *details = signet_parse_json_object_node(details_json_object);
    if (!details) return NULL;

    json_builder_set_member_name(b, "details");
    json_builder_add_value(b, details); /* ownership transferred */
  }

  json_builder_end_object(b);

  JsonNode *root = json_builder_get_root(b);
  if (!root) return NULL;

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);

  gchar *out = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  return out; /* g_free() */
}

int signet_audit_log_common(SignetAuditLogger *l,
                            SignetAuditEventType type,
                            const SignetAuditCommonFields *fields,
                            const char *details_json_object) {
  if (!l) return -1;
  if (signet_is_disabled(l)) return 0;

  char *payload = signet_audit_build_common_payload_json(fields, details_json_object);
  if (!payload) return -1;

  int rc = signet_audit_log_json(l, type, payload);
  g_free(payload);
  return rc;
}