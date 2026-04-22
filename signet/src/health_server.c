/* SPDX-License-Identifier: MIT
 *
 * health_server.c - /health endpoint using libmicrohttpd.
 *
 * Serves GET /health with a JSON snapshot. No management ops exposed.
 * Uses MHD_start_daemon() for a robust, well-tested HTTP server.
 */

#include "signet/health_server.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <microhttpd.h>

/* Process-global atomic counters — incremented by subsystem handlers. */
SignetMetricsCounters g_signet_metrics;

struct SignetHealthServer {
  char *listen;
  struct MHD_Daemon *mhd;

  GMutex mu;
  SignetHealthSnapshot snap;
  int64_t started_at_unix;
};

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

/* ----------------------------- JSON builder ------------------------------ */

static char *signet_build_health_json(const SignetHealthSnapshot *snap,
                                      int64_t now_unix,
                                      uint64_t uptime_seconds) {
  const char *db_s = (snap && snap->db_open) ? "open" : "closed";

  const char *overall = "ok";
  if (!(snap && snap->db_open) || !(snap && snap->key_store_available)) {
    overall = "error";
  } else if (!(snap && snap->relay_connected)) {
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

  json_builder_set_member_name(b, "db");
  json_builder_add_string_value(b, db_s);

  json_builder_set_member_name(b, "relays");
  json_builder_add_int_value(b, (gint64)(snap ? snap->relay_count : 0));

  json_builder_set_member_name(b, "agents_active");
  json_builder_add_int_value(b, (gint64)(snap ? snap->agents_active : 0));

  json_builder_set_member_name(b, "cache_entries");
  json_builder_add_int_value(b, (gint64)(snap ? snap->cache_entries : 0));

  json_builder_set_member_name(b, "key_store");
  json_builder_add_string_value(b, (snap && snap->key_store_available) ? "available" : "unavailable");

  json_builder_end_object(b);
  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) { g_object_unref(b); return NULL; }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out;
}

/* ----------------------------- /ready handler ----------------------------- */

static enum MHD_Result
signet_handle_ready(SignetHealthServer *hs, struct MHD_Connection *connection) {
  SignetHealthSnapshot snap_copy;
  g_mutex_lock(&hs->mu);
  snap_copy = hs->snap;
  g_mutex_unlock(&hs->mu);

  /* Ready = DB open AND fleet synced. */
  bool ready = snap_copy.db_open && snap_copy.fleet_synced;
  unsigned int status = ready ? MHD_HTTP_OK : MHD_HTTP_SERVICE_UNAVAILABLE;
  const char *body = ready ? "{\"ready\":true}" : "{\"ready\":false}";

  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
  MHD_add_response_header(resp, "Content-Type", "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  enum MHD_Result ret = MHD_queue_response(connection, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* ----------------------------- /metrics handler --------------------------- */

static char *signet_build_metrics(const SignetHealthSnapshot *s, uint64_t uptime_sec) {
  GString *m = g_string_sized_new(1024);

  g_string_append_printf(m, "# HELP signet_up Whether signet is up.\n");
  g_string_append_printf(m, "# TYPE signet_up gauge\n");
  g_string_append_printf(m, "signet_up 1\n");

  g_string_append_printf(m, "# HELP signet_uptime_seconds Daemon uptime.\n");
  g_string_append_printf(m, "# TYPE signet_uptime_seconds gauge\n");
  g_string_append_printf(m, "signet_uptime_seconds %" G_GUINT64_FORMAT "\n", uptime_sec);

  g_string_append_printf(m, "# HELP signet_bootstrap_total Bootstrap attempts.\n");
  g_string_append_printf(m, "# TYPE signet_bootstrap_total counter\n");
  g_string_append_printf(m, "signet_bootstrap_total %" G_GUINT64_FORMAT "\n",
                         s->bootstrap_total);

  g_string_append_printf(m, "# HELP signet_auth_total Auth attempts by result.\n");
  g_string_append_printf(m, "# TYPE signet_auth_total counter\n");
  g_string_append_printf(m, "signet_auth_total{result=\"ok\"} %" G_GUINT64_FORMAT "\n",
                         s->auth_total_ok);
  g_string_append_printf(m, "signet_auth_total{result=\"denied\"} %" G_GUINT64_FORMAT "\n",
                         s->auth_total_denied);
  g_string_append_printf(m, "signet_auth_total{result=\"error\"} %" G_GUINT64_FORMAT "\n",
                         s->auth_total_error);

  g_string_append_printf(m, "# HELP signet_sign_total Signing operations.\n");
  g_string_append_printf(m, "# TYPE signet_sign_total counter\n");
  g_string_append_printf(m, "signet_sign_total %" G_GUINT64_FORMAT "\n", s->sign_total);

  g_string_append_printf(m, "# HELP signet_revoke_total Revocations.\n");
  g_string_append_printf(m, "# TYPE signet_revoke_total counter\n");
  g_string_append_printf(m, "signet_revoke_total %" G_GUINT64_FORMAT "\n", s->revoke_total);

  g_string_append_printf(m, "# HELP signet_fleet_sync_last_timestamp Fleet sync time.\n");
  g_string_append_printf(m, "# TYPE signet_fleet_sync_last_timestamp gauge\n");
  g_string_append_printf(m, "signet_fleet_sync_last_timestamp %" G_GINT64_FORMAT "\n",
                         s->fleet_sync_last_ts);

  g_string_append_printf(m, "# HELP signet_active_sessions Current sessions.\n");
  g_string_append_printf(m, "# TYPE signet_active_sessions gauge\n");
  g_string_append_printf(m, "signet_active_sessions %u\n", s->active_sessions);

  g_string_append_printf(m, "# HELP signet_active_leases Current credential leases.\n");
  g_string_append_printf(m, "# TYPE signet_active_leases gauge\n");
  g_string_append_printf(m, "signet_active_leases %u\n", s->active_leases);

  g_string_append_printf(m, "# HELP signet_agents_active Active agents.\n");
  g_string_append_printf(m, "# TYPE signet_agents_active gauge\n");
  g_string_append_printf(m, "signet_agents_active %u\n", s->agents_active);

  g_string_append_printf(m, "# HELP signet_relays_connected Connected relays.\n");
  g_string_append_printf(m, "# TYPE signet_relays_connected gauge\n");
  g_string_append_printf(m, "signet_relays_connected %u\n", s->relay_count);

  return g_string_free(m, FALSE);
}

static enum MHD_Result
signet_handle_metrics(SignetHealthServer *hs, struct MHD_Connection *connection) {
  SignetHealthSnapshot snap_copy;
  int64_t started_at;

  g_mutex_lock(&hs->mu);
  snap_copy = hs->snap;
  started_at = hs->started_at_unix;
  g_mutex_unlock(&hs->mu);

  int64_t now = signet_now_unix();
  uint64_t uptime = (started_at > 0 && now >= started_at)
                      ? (uint64_t)(now - started_at)
                      : snap_copy.uptime_sec;

  char *body = signet_build_metrics(&snap_copy, uptime);
  if (!body) {
    const char *err = "# error building metrics\n";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), body, MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(resp, "Content-Type",
                          "text/plain; version=0.0.4; charset=utf-8");
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* ----------------------------- MHD handler ------------------------------- */

static enum MHD_Result
signet_health_handler(void *cls,
                      struct MHD_Connection *connection,
                      const char *url,
                      const char *method,
                      const char *version,
                      const char *upload_data,
                      size_t *upload_data_size,
                      void **con_cls) {
  (void)version;
  (void)upload_data;
  (void)upload_data_size;
  (void)con_cls;

  SignetHealthServer *hs = (SignetHealthServer *)cls;

  if (strcmp(method, "GET") != 0) {
    const char *body = "{\"error\":\"method not allowed\"}";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  /* GET /ready */
  if (strcmp(url, "/ready") == 0) {
    return signet_handle_ready(hs, connection);
  }

  /* GET /metrics */
  if (strcmp(url, "/metrics") == 0) {
    return signet_handle_metrics(hs, connection);
  }

  /* GET /health (existing) */
  if (strcmp(url, "/health") != 0) {
    const char *body = "{\"error\":\"not found\"}";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  /* Build health JSON from snapshot. */
  SignetHealthSnapshot snap_copy;
  int64_t started_at;

  g_mutex_lock(&hs->mu);
  snap_copy = hs->snap;
  started_at = hs->started_at_unix;
  g_mutex_unlock(&hs->mu);

  int64_t now = signet_now_unix();
  uint64_t uptime = (started_at > 0 && now >= started_at)
                      ? (uint64_t)(now - started_at)
                      : snap_copy.uptime_sec;

  char *json = signet_build_health_json(&snap_copy, now, uptime);
  if (!json) {
    const char *err = "{\"status\":\"error\"}";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(json), json, MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(resp, "Content-Type", "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-store");

  unsigned int status = MHD_HTTP_OK;
  /* Return 503 if overall status is "error". */
  if (!(snap_copy.db_open) || !(snap_copy.key_store_available)) {
    status = MHD_HTTP_SERVICE_UNAVAILABLE;
  }

  enum MHD_Result ret = MHD_queue_response(connection, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* ------------------------------ public API -------------------------------- */

SignetHealthServer *signet_health_server_new(const SignetHealthServerConfig *cfg) {
  if (!cfg || !cfg->listen) return NULL;

  SignetHealthServer *hs = (SignetHealthServer *)calloc(1, sizeof(*hs));
  if (!hs) return NULL;

  hs->listen = g_strdup(cfg->listen);
  g_mutex_init(&hs->mu);
  memset(&hs->snap, 0, sizeof(hs->snap));
  hs->started_at_unix = 0;

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
  if (!hs || !hs->listen) return -1;
  if (hs->mhd) return 0; /* already started */

  /* Parse host:port from listen string. */
  const char *colon = strrchr(hs->listen, ':');
  if (!colon || colon[1] == '\0') return -1;

  unsigned int port = (unsigned int)atoi(colon + 1);
  if (port == 0 && strcmp(colon + 1, "0") != 0) return -1;

  hs->mhd = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
      (uint16_t)port,
      NULL, NULL,                        /* accept policy: accept all */
      signet_health_handler, hs,         /* handler + cls */
      MHD_OPTION_END);

  if (!hs->mhd) return -1;

  g_mutex_lock(&hs->mu);
  hs->started_at_unix = signet_now_unix();
  g_mutex_unlock(&hs->mu);

  return 0;
}

void signet_health_server_stop(SignetHealthServer *hs) {
  if (!hs || !hs->mhd) return;
  MHD_stop_daemon(hs->mhd);
  hs->mhd = NULL;
}

void signet_health_server_set_snapshot(SignetHealthServer *hs, const SignetHealthSnapshot *snap) {
  if (!hs || !snap) return;
  g_mutex_lock(&hs->mu);
  hs->snap = *snap;
  g_mutex_unlock(&hs->mu);
}