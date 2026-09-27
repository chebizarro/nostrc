/*
 * session_dbus.c — org.nostr.SessionRelay1 (see session_dbus.h and
 * gnome/dbus/org.nostr.SessionRelay1.xml). Bead nostrc-janr.
 */
#define _GNU_SOURCE
#include "session_dbus.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "metrics.h"
#include "nostr-filter.h"
#ifdef NSR_HAVE_FEDERATION
#include "session_federation.h"
#endif

#ifdef NSR_HAVE_GDBUS
#include <gio/gio.h>

/* Must match gnome/dbus/org.nostr.SessionRelay1.xml (checked by
 * tests/test_session_relay_dbus.c against the installed-interface file). */
static const char k_introspection_xml[] =
    "<node>"
    "  <interface name='org.nostr.SessionRelay1'>"
    "    <annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='false'/>"
    "    <property name='EventCount' type='x' access='read'/>"
    "    <property name='StorageBytes' type='t' access='read'/>"
    "    <property name='ConnectedClients' type='u' access='read'/>"
    "    <property name='Uptime' type='t' access='read'/>"
    "    <property name='StorageBackend' type='s' access='read'/>"
    "    <property name='RequestedStorageBackend' type='s' access='read'/>"
    "    <property name='StoragePath' type='s' access='read'/>"
    "    <property name='SupportedNips' type='au' access='read'/>"
    "    <property name='RetentionSupported' type='b' access='read'/>"
    "    <property name='FederationState' type='s' access='read'/>"
    "    <property name='PendingUpstream' type='u' access='read'/>"
    "    <property name='ForwardedCount' type='t' access='read'/>"
    "    <property name='FailedUpstream' type='t' access='read'/>"
    "    <property name='LastUpstreamError' type='s' access='read'/>"
    "    <method name='GetStats'>"
    "      <arg name='stats' type='a{sv}' direction='out'/>"
    "    </method>"
    "    <method name='GetEventUpstream'>"
    "      <arg name='event_id' type='s' direction='in'/>"
    "      <arg name='state' type='s' direction='out'/>"
    "      <arg name='detail' type='s' direction='out'/>"
    "      <arg name='relays' type='a(sssuxx)' direction='out'/>"
    "    </method>"
    "    <method name='GetUpstreamRelays'>"
    "      <arg name='relays' type='a(sa{sv})' direction='out'/>"
    "    </method>"
    "    <signal name='UpstreamStatusChanged'>"
    "      <arg name='event_id' type='s'/>"
    "      <arg name='relay_url' type='s'/>"
    "      <arg name='relay_state' type='s'/>"
    "      <arg name='reason' type='s'/>"
    "      <arg name='event_state' type='s'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

#define EVENT_COUNT_TTL_US (30 * G_USEC_PER_SEC)
#define STORAGE_BYTES_TTL_US (5 * G_USEC_PER_SEC)

typedef struct {
  const RelaydConfig *cfg;
  NostrStorage *storage;
  char *backend;           /* "nostrdb" | "none" */
  char *requested_backend;
  char *storage_dir;
  char *version;
  gint64 started_us;       /* monotonic */
  void *federation;        /* NsrFederation*, borrowed, nullable */
  char *fed_state;         /* reported while federation == NULL */
  char *fed_detail;

  /* D-Bus-thread-only caches. */
  gint64 event_count;
  gint64 event_count_at;
  guint64 storage_bytes;
  gint64 storage_bytes_at;

  GThread *thread;
  GMainContext *ctx;
  GMainLoop *loop;
  GDBusNodeInfo *node;
  guint owner_id;
  guint reg_id;
  GDBusConnection *conn;
} NsrDbus;

static NsrDbus *s_dbus;

/* ── Statistic sources ────────────────────────────────────────────────── */

static gint64 stat_event_count(NsrDbus *d) {
  if (!d->storage) return 0; /* cache-less: stores nothing */
  gint64 now = g_get_monotonic_time();
  if (d->event_count_at && now - d->event_count_at < EVENT_COUNT_TTL_US)
    return d->event_count;
  gint64 v = -1;
  if (d->storage->vt && d->storage->vt->count) {
    NostrFilter all; /* zeroed filter = no constraints */
    memset(&all, 0, sizeof all);
    uint64_t n = 0;
    if (d->storage->vt->count(d->storage, &all, 1, &n) == 0)
      v = n > (uint64_t)G_MAXINT64 ? G_MAXINT64 : (gint64)n;
  }
  d->event_count = v;
  d->event_count_at = now;
  return v;
}

static unsigned long long s_walk_total;

static int walk_cb(const char *path, const struct stat *st, int flag,
                   struct FTW *ftw) {
  (void)path; (void)ftw;
  if (flag == FTW_F && S_ISREG(st->st_mode))
    s_walk_total += (unsigned long long)st->st_blocks * 512ULL;
  return 0;
}

#endif /* NSR_HAVE_GDBUS */

unsigned long long nsr_dir_allocated_bytes(const char *dir) {
#ifdef NSR_HAVE_GDBUS
  /* nftw has no user-data pointer; only the D-Bus thread and tests call
   * this, never concurrently. */
  s_walk_total = 0;
  if (!dir || !*dir) return 0;
  if (nftw(dir, walk_cb, 8, FTW_PHYS) != 0) return s_walk_total;
  return s_walk_total;
#else
  (void)dir;
  return 0;
#endif
}

#ifdef NSR_HAVE_GDBUS

static guint64 stat_storage_bytes(NsrDbus *d) {
  gint64 now = g_get_monotonic_time();
  if (d->storage_bytes_at && now - d->storage_bytes_at < STORAGE_BYTES_TTL_US)
    return d->storage_bytes;
  d->storage_bytes = nsr_dir_allocated_bytes(d->storage_dir);
  d->storage_bytes_at = now;
  return d->storage_bytes;
}

static guint64 stat_uptime(NsrDbus *d) {
  return (guint64)((g_get_monotonic_time() - d->started_us) / G_USEC_PER_SEC);
}

static GVariant *stat_nips(NsrDbus *d) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
  for (int i = 0; d->cfg && i < d->cfg->supported_nips_count; i++)
    g_variant_builder_add(&b, "u", (guint32)d->cfg->supported_nips[i]);
  return g_variant_builder_end(&b);
}

/* Federation figures; zeros + the static state when there is no engine. */
typedef struct {
  const char *state;
  char *detail;
  guint32 pending, unroutable, pending_targets, relays_connected;
  guint64 forwarded, partial, failed, skipped;
  char *last_error;
} FedFigures;

static void fed_figures(NsrDbus *d, FedFigures *out) {
  memset(out, 0, sizeof *out);
#ifdef NSR_HAVE_FEDERATION
  if (d->federation) {
    NsrFedStatus st;
    nsr_federation_status(d->federation, &st);
    out->state = st.state;
    out->detail = g_strdup(st.detail);
    out->pending = (guint32)MIN(st.outbox.queued, G_MAXUINT32);
    out->unroutable = (guint32)MIN(st.outbox.unroutable, G_MAXUINT32);
    out->pending_targets = (guint32)MIN(st.outbox.pending_targets, G_MAXUINT32);
    out->relays_connected = st.relays_connected;
    out->forwarded = st.outbox.forwarded;
    out->partial = st.outbox.partial;
    out->failed = st.outbox.failed;
    out->skipped = st.outbox.skipped;
    out->last_error = g_strdup(st.outbox.last_error);
    nsr_federation_status_clear(&st);
    return;
  }
#endif
  out->state = d->fed_state;
  out->detail = g_strdup(d->fed_detail);
  out->last_error = g_strdup("");
}

static void fed_figures_clear(FedFigures *f) {
  g_free(f->detail);
  g_free(f->last_error);
}

static GVariant *get_property(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *name, GError **error,
                              gpointer user_data) {
  (void)c; (void)sender; (void)path; (void)iface;
  NsrDbus *d = user_data;
  RelaydMetricsSnapshot m;
  metrics_snapshot(&m);
  if (g_str_equal(name, "EventCount"))
    return g_variant_new_int64(stat_event_count(d));
  if (g_str_equal(name, "StorageBytes"))
    return g_variant_new_uint64(stat_storage_bytes(d));
  if (g_str_equal(name, "ConnectedClients"))
    return g_variant_new_uint32((guint32)MIN(m.connections_current, G_MAXUINT32));
  if (g_str_equal(name, "Uptime")) return g_variant_new_uint64(stat_uptime(d));
  if (g_str_equal(name, "StorageBackend")) return g_variant_new_string(d->backend);
  if (g_str_equal(name, "RequestedStorageBackend"))
    return g_variant_new_string(d->requested_backend);
  if (g_str_equal(name, "StoragePath")) return g_variant_new_string(d->storage_dir);
  if (g_str_equal(name, "SupportedNips")) return stat_nips(d);
  if (g_str_equal(name, "RetentionSupported")) return g_variant_new_boolean(FALSE);
  if (g_str_has_prefix(name, "Federation") || g_str_has_suffix(name, "Upstream") ||
      g_str_equal(name, "ForwardedCount") || g_str_equal(name, "LastUpstreamError")) {
    FedFigures ff;
    fed_figures(d, &ff);
    GVariant *v = NULL;
    if (g_str_equal(name, "FederationState")) v = g_variant_new_string(ff.state);
    else if (g_str_equal(name, "PendingUpstream")) v = g_variant_new_uint32(ff.pending);
    else if (g_str_equal(name, "ForwardedCount"))
      v = g_variant_new_uint64(ff.forwarded + ff.partial);
    else if (g_str_equal(name, "FailedUpstream")) v = g_variant_new_uint64(ff.failed);
    else if (g_str_equal(name, "LastUpstreamError")) v = g_variant_new_string(ff.last_error);
    fed_figures_clear(&ff);
    if (v) return v;
  }
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
              "No such property: %s", name);
  return NULL;
}

static void handle_get_event_upstream(NsrDbus *d, GVariant *params,
                                      GDBusMethodInvocation *inv) {
  const gchar *id = NULL;
  g_variant_get(params, "(&s)", &id);
  GVariantBuilder rb;
  g_variant_builder_init(&rb, G_VARIANT_TYPE("a(sssuxx)"));
  char *state = NULL, *detail = NULL;
#ifdef NSR_HAVE_FEDERATION
  GPtrArray *targets = NULL;
  if (d->federation &&
      nsr_outbox_event_status(nsr_federation_get_outbox(d->federation), id, &state, &detail,
                              &targets) == 0) {
    for (guint i = 0; i < targets->len; i++) {
      NsrOutboxTargetInfo *t = g_ptr_array_index(targets, i);
      g_variant_builder_add(&rb, "(sssuxx)", t->relay, t->state, t->reason, t->attempts,
                            (gint64)t->updated_at, (gint64)t->acked_at);
    }
    g_ptr_array_unref(targets);
  }
#endif
  if (!state) {
    /* Never queued: unknown id, a local-only event (never forwarded), or
     * no federation engine. */
    state = g_strdup("unknown");
    detail = g_strdup(d->federation ? "not in the upstream outbox (never queued: local-only, "
                                      "never stored here, or already pruned)"
                                    : (d->fed_detail ? d->fed_detail : ""));
  }
  g_dbus_method_invocation_return_value(inv, g_variant_new("(ssa(sssuxx))", state, detail, &rb));
  g_free(state);
  g_free(detail);
}

static void handle_get_upstream_relays(NsrDbus *d, GDBusMethodInvocation *inv) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(sa{sv})"));
#ifdef NSR_HAVE_FEDERATION
  if (d->federation) {
    GPtrArray *relays = nsr_federation_relays(d->federation);
    for (guint i = 0; i < relays->len; i++) {
      NsrFedRelayInfo *r = g_ptr_array_index(relays, i);
      GVariantBuilder p;
      g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&p, "{sv}", "connected", g_variant_new_boolean(r->connected));
      g_variant_builder_add(&p, "{sv}", "authenticated", g_variant_new_boolean(r->authed));
      g_variant_builder_add(&p, "{sv}", "pending", g_variant_new_uint32(r->pending));
      g_variant_builder_add(&p, "{sv}", "acked", g_variant_new_uint64(r->acked));
      g_variant_builder_add(&p, "{sv}", "failed", g_variant_new_uint64(r->failed));
      g_variant_builder_add(&p, "{sv}", "last_error", g_variant_new_string(r->last_error));
      g_variant_builder_add(&p, "{sv}", "last_ok_at", g_variant_new_int64(r->last_ok_at));
      g_variant_builder_add(&b, "(sa{sv})", r->url, &p);
    }
    g_ptr_array_unref(relays);
  }
#endif
  g_dbus_method_invocation_return_value(inv, g_variant_new("(a(sa{sv}))", &b));
}

static void method_call(GDBusConnection *c, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer user_data) {
  (void)c; (void)sender; (void)path; (void)iface; (void)params;
  NsrDbus *d = user_data;
  if (g_str_equal(method, "GetEventUpstream")) {
    handle_get_event_upstream(d, params, inv);
    return;
  }
  if (g_str_equal(method, "GetUpstreamRelays")) {
    handle_get_upstream_relays(d, inv);
    return;
  }
  if (!g_str_equal(method, "GetStats")) {
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No such method: %s", method);
    return;
  }
  RelaydMetricsSnapshot m;
  metrics_snapshot(&m);
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&b, "{sv}", "event_count",
                        g_variant_new_int64(stat_event_count(d)));
  g_variant_builder_add(&b, "{sv}", "storage_bytes",
                        g_variant_new_uint64(stat_storage_bytes(d)));
  g_variant_builder_add(&b, "{sv}", "connected_clients",
                        g_variant_new_uint32((guint32)MIN(m.connections_current, G_MAXUINT32)));
  g_variant_builder_add(&b, "{sv}", "uptime", g_variant_new_uint64(stat_uptime(d)));
  g_variant_builder_add(&b, "{sv}", "storage_backend", g_variant_new_string(d->backend));
  g_variant_builder_add(&b, "{sv}", "requested_storage_backend",
                        g_variant_new_string(d->requested_backend));
  g_variant_builder_add(&b, "{sv}", "storage_path", g_variant_new_string(d->storage_dir));
  g_variant_builder_add(&b, "{sv}", "supported_nips", stat_nips(d));
  g_variant_builder_add(&b, "{sv}", "retention_supported", g_variant_new_boolean(FALSE));
  g_variant_builder_add(&b, "{sv}", "connections_total",
                        g_variant_new_uint64((guint64)m.connections_total));
  g_variant_builder_add(&b, "{sv}", "subscriptions",
                        g_variant_new_uint32((guint32)MIN(m.subs_current, G_MAXUINT32)));
  g_variant_builder_add(&b, "{sv}", "events_streamed",
                        g_variant_new_uint64((guint64)m.events_streamed));
  g_variant_builder_add(&b, "{sv}", "version", g_variant_new_string(d->version));
  FedFigures ff;
  fed_figures(d, &ff);
  g_variant_builder_add(&b, "{sv}", "federation_state", g_variant_new_string(ff.state));
  g_variant_builder_add(&b, "{sv}", "federation_detail", g_variant_new_string(ff.detail));
  g_variant_builder_add(&b, "{sv}", "pending_upstream", g_variant_new_uint32(ff.pending));
  g_variant_builder_add(&b, "{sv}", "unroutable_upstream", g_variant_new_uint32(ff.unroutable));
  g_variant_builder_add(&b, "{sv}", "pending_upstream_deliveries",
                        g_variant_new_uint32(ff.pending_targets));
  g_variant_builder_add(&b, "{sv}", "forwarded_count",
                        g_variant_new_uint64(ff.forwarded + ff.partial));
  g_variant_builder_add(&b, "{sv}", "partially_forwarded_count",
                        g_variant_new_uint64(ff.partial));
  g_variant_builder_add(&b, "{sv}", "failed_upstream", g_variant_new_uint64(ff.failed));
  g_variant_builder_add(&b, "{sv}", "skipped_upstream", g_variant_new_uint64(ff.skipped));
  g_variant_builder_add(&b, "{sv}", "last_upstream_error", g_variant_new_string(ff.last_error));
  g_variant_builder_add(&b, "{sv}", "upstream_relays_connected",
                        g_variant_new_uint32(ff.relays_connected));
  fed_figures_clear(&ff);
  g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv})", &b));
}

static const GDBusInterfaceVTable k_vtable = {method_call, get_property, NULL, {0}};

static void on_bus_acquired(GDBusConnection *conn, const gchar *name,
                            gpointer user_data) {
  (void)name;
  NsrDbus *d = user_data;
  GError *err = NULL;
  d->conn = g_object_ref(conn);
  d->reg_id = g_dbus_connection_register_object(
      conn, NSR_DBUS_PATH, d->node->interfaces[0], &k_vtable, d, NULL, &err);
  if (!d->reg_id) {
    fprintf(stderr, "nostr-session-relayd: D-Bus register_object: %s\n",
            err ? err->message : "failed");
    g_clear_error(&err);
  }
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name,
                             gpointer user_data) {
  (void)conn; (void)user_data;
  fprintf(stderr, "nostr-session-relayd: exporting %s on the session bus\n", name);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name,
                         gpointer user_data) {
  (void)user_data;
  if (!conn)
    fprintf(stderr,
            "nostr-session-relayd: no session bus; %s stats interface "
            "disabled (relay continues)\n", name);
  else
    fprintf(stderr,
            "nostr-session-relayd: %s is owned by another process; stats "
            "interface disabled (relay continues)\n", name);
}

static gpointer dbus_thread(gpointer data) {
  NsrDbus *d = data;
  g_main_context_push_thread_default(d->ctx);
  d->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, NSR_DBUS_NAME,
                               G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                               on_bus_acquired, on_name_acquired, on_name_lost,
                               d, NULL);
  g_main_loop_run(d->loop);
  if (d->reg_id && d->conn) g_dbus_connection_unregister_object(d->conn, d->reg_id);
  d->reg_id = 0;
  g_bus_unown_name(d->owner_id);
  d->owner_id = 0;
  if (d->conn) {
    /* Flush the name release before the process exits. */
    g_dbus_connection_flush_sync(d->conn, NULL, NULL);
    g_clear_object(&d->conn);
  }
  /* Drain callbacks queued by unown so nothing references d afterwards. */
  while (g_main_context_iteration(d->ctx, FALSE)) {
  }
  g_main_context_pop_thread_default(d->ctx);
  return NULL;
}

void nsr_dbus_start(const NsrDbusInfo *info) {
  if (s_dbus || !info) return;
  NsrDbus *d = g_new0(NsrDbus, 1);
  GError *err = NULL;
  d->node = g_dbus_node_info_new_for_xml(k_introspection_xml, &err);
  if (!d->node) {
    fprintf(stderr, "nostr-session-relayd: bad introspection XML: %s\n",
            err ? err->message : "?");
    g_clear_error(&err);
    g_free(d);
    return;
  }
  d->cfg = info->cfg;
  d->storage = info->storage;
  d->backend = g_strdup(info->storage_backend ? info->storage_backend : "none");
  d->requested_backend = g_strdup(info->requested_backend ? info->requested_backend : "");
  d->storage_dir = g_strdup(info->storage_dir ? info->storage_dir : "");
  d->version = g_strdup(info->version ? info->version : "");
  d->federation = info->federation;
  d->fed_state = g_strdup(info->federation_state ? info->federation_state : "unavailable");
  d->fed_detail = g_strdup(info->federation_detail ? info->federation_detail : "");
  d->started_us = g_get_monotonic_time();
  d->ctx = g_main_context_new();
  d->loop = g_main_loop_new(d->ctx, FALSE);
  d->thread = g_thread_try_new("nsr-dbus", dbus_thread, d, &err);
  if (!d->thread) {
    fprintf(stderr, "nostr-session-relayd: D-Bus thread: %s\n",
            err ? err->message : "?");
    g_clear_error(&err);
    g_main_loop_unref(d->loop);
    g_main_context_unref(d->ctx);
    g_dbus_node_info_unref(d->node);
    g_free(d->backend); g_free(d->requested_backend);
    g_free(d->storage_dir); g_free(d->version);
    g_free(d->fed_state); g_free(d->fed_detail);
    g_free(d);
    return;
  }
  s_dbus = d;
}

void nsr_dbus_stop(void) {
  NsrDbus *d = s_dbus;
  if (!d) return;
  s_dbus = NULL;
  g_main_loop_quit(d->loop); /* thread-safe; wakes d->ctx */
  g_thread_join(d->thread);
  g_main_loop_unref(d->loop);
  g_main_context_unref(d->ctx);
  g_dbus_node_info_unref(d->node);
  g_free(d->backend);
  g_free(d->requested_backend);
  g_free(d->storage_dir);
  g_free(d->version);
  g_free(d->fed_state);
  g_free(d->fed_detail);
  g_free(d);
}

/* ── UpstreamStatusChanged ────────────────────────────────────────────── */

typedef struct {
  NsrDbus *d;
  GVariant *args;
} EmitJob;

static gboolean emit_on_dbus_thread(gpointer p) {
  EmitJob *j = p;
  /* j->args is not floating: emit_signal takes its own reference and
   * emit_job_free() drops ours. */
  if (j->d->conn && j->d->reg_id)
    g_dbus_connection_emit_signal(j->d->conn, NULL, NSR_DBUS_PATH, NSR_DBUS_IFACE,
                                  "UpstreamStatusChanged", j->args, NULL);
  return G_SOURCE_REMOVE;
}

static void emit_job_free(gpointer p) {
  EmitJob *j = p;
  g_variant_unref(j->args);
  g_free(j);
}

void nsr_dbus_emit_upstream(const char *event_id, const char *relay_url,
                            const char *relay_state, const char *reason,
                            const char *event_state, void *user_data) {
  (void)user_data;
  NsrDbus *d = s_dbus; /* set before the federation thread starts, cleared after it stops */
  if (!d || !event_id) return;
  EmitJob *j = g_new0(EmitJob, 1);
  j->d = d;
  j->args = g_variant_ref_sink(g_variant_new(
      "(sssss)", event_id, relay_url ? relay_url : "", relay_state ? relay_state : "",
      reason ? reason : "", event_state ? event_state : ""));
  g_main_context_invoke_full(d->ctx, G_PRIORITY_DEFAULT, emit_on_dbus_thread, j, emit_job_free);
}

/* ── --stats client ───────────────────────────────────────────────────── */

static void print_value(const char *key, GVariant *v) {
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
    printf("%s: %s\n", key, g_variant_get_string(v, NULL));
  } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) {
    printf("%s: %" G_GINT64_FORMAT "\n", key, g_variant_get_int64(v));
  } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)) {
    printf("%s: %" G_GUINT64_FORMAT "\n", key, g_variant_get_uint64(v));
  } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) {
    printf("%s: %" G_GUINT32_FORMAT "\n", key, g_variant_get_uint32(v));
  } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
    printf("%s: %s\n", key, g_variant_get_boolean(v) ? "true" : "false");
  } else if (g_variant_is_of_type(v, G_VARIANT_TYPE("au"))) {
    gsize n = 0;
    const guint32 *a = g_variant_get_fixed_array(v, &n, sizeof(guint32));
    printf("%s:", key);
    for (gsize i = 0; i < n; i++) printf("%s%" G_GUINT32_FORMAT, i ? "," : " ", a[i]);
    printf("\n");
  } else {
    gchar *s = g_variant_print(v, FALSE);
    printf("%s: %s\n", key, s);
    g_free(s);
  }
}

int nsr_dbus_print_stats(void) {
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    fprintf(stderr, "nostr-session-relayd: no session bus: %s\n", err->message);
    g_error_free(err);
    return 1;
  }
  GVariant *r = g_dbus_connection_call_sync(
      bus, NSR_DBUS_NAME, NSR_DBUS_PATH, NSR_DBUS_IFACE, "GetStats", NULL,
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL,
      &err);
  g_object_unref(bus);
  if (!r) {
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
      fprintf(stderr, "nostr-session-relayd: not running (no owner for %s)\n",
              NSR_DBUS_NAME);
    else
      fprintf(stderr, "nostr-session-relayd: GetStats: %s\n", err->message);
    g_error_free(err);
    return 1;
  }
  GVariant *dict = g_variant_get_child_value(r, 0);
  /* Stable order for scripts: sort keys. */
  GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
  GVariantIter it;
  const gchar *k;
  GVariant *v;
  g_variant_iter_init(&it, dict);
  while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
    g_ptr_array_add(keys, g_strdup(k));
    g_variant_unref(v);
  }
  g_ptr_array_sort_values(keys, (GCompareFunc)g_strcmp0);
  for (guint i = 0; i < keys->len; i++) {
    const char *key = g_ptr_array_index(keys, i);
    GVariant *val = g_variant_lookup_value(dict, key, NULL);
    if (val) {
      print_value(key, val);
      g_variant_unref(val);
    }
  }
  g_ptr_array_unref(keys);
  g_variant_unref(dict);
  g_variant_unref(r);
  return 0;
}

static GVariant *call_daemon(const char *method, GVariant *args, const char *reply_type) {
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    fprintf(stderr, "nostr-session-relayd: no session bus: %s\n", err->message);
    g_error_free(err);
    return NULL;
  }
  GVariant *r = g_dbus_connection_call_sync(bus, NSR_DBUS_NAME, NSR_DBUS_PATH, NSR_DBUS_IFACE,
                                            method, args, G_VARIANT_TYPE(reply_type),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &err);
  g_object_unref(bus);
  if (!r) {
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
      fprintf(stderr, "nostr-session-relayd: not running (no owner for %s)\n", NSR_DBUS_NAME);
    else
      fprintf(stderr, "nostr-session-relayd: %s: %s\n", method, err->message);
    g_error_free(err);
  }
  return r;
}

int nsr_dbus_print_upstream(const char *event_id) {
  if (event_id) {
    GVariant *r = call_daemon("GetEventUpstream", g_variant_new("(s)", event_id),
                              "(ssa(sssuxx))");
    if (!r) return 1;
    const gchar *state = NULL, *detail = NULL;
    GVariantIter *it = NULL;
    g_variant_get(r, "(&s&sa(sssuxx))", &state, &detail, &it);
    printf("event: %s\nstate: %s\n", event_id, state);
    if (detail && *detail) printf("detail: %s\n", detail);
    const gchar *relay, *rstate, *reason;
    guint32 attempts;
    gint64 updated, acked;
    while (g_variant_iter_next(it, "(&s&s&suxx)", &relay, &rstate, &reason, &attempts,
                               &updated, &acked))
      printf("relay: %s state=%s attempts=%" G_GUINT32_FORMAT " updated_at=%" G_GINT64_FORMAT
             " acked_at=%" G_GINT64_FORMAT "%s%s\n",
             relay, rstate, attempts, updated, acked, *reason ? " reason=" : "", reason);
    g_variant_iter_free(it);
    g_variant_unref(r);
    return 0;
  }
  GVariant *r = call_daemon("GetUpstreamRelays", NULL, "(a(sa{sv}))");
  if (!r) return 1;
  GVariantIter *it = NULL;
  g_variant_get(r, "(a(sa{sv}))", &it);
  const gchar *url;
  GVariant *props;
  while (g_variant_iter_next(it, "(&s@a{sv})", &url, &props)) {
    gboolean connected = FALSE, authed = FALSE;
    guint32 pending = 0;
    guint64 acked = 0, failed = 0;
    gint64 last_ok = 0;
    const gchar *last_error = "";
    g_variant_lookup(props, "connected", "b", &connected);
    g_variant_lookup(props, "authenticated", "b", &authed);
    g_variant_lookup(props, "pending", "u", &pending);
    g_variant_lookup(props, "acked", "t", &acked);
    g_variant_lookup(props, "failed", "t", &failed);
    g_variant_lookup(props, "last_ok_at", "x", &last_ok);
    g_variant_lookup(props, "last_error", "&s", &last_error);
    printf("%s connected=%s authenticated=%s pending=%" G_GUINT32_FORMAT " acked=%"
           G_GUINT64_FORMAT " failed=%" G_GUINT64_FORMAT " last_ok_at=%" G_GINT64_FORMAT
           "%s%s\n",
           url, connected ? "yes" : "no", authed ? "yes" : "no", pending, acked, failed,
           last_ok, *last_error ? " last_error=" : "", last_error);
    g_variant_unref(props);
  }
  g_variant_iter_free(it);
  g_variant_unref(r);
  return 0;
}

#else /* !NSR_HAVE_GDBUS */

void nsr_dbus_start(const NsrDbusInfo *info) { (void)info; }
void nsr_dbus_stop(void) {}
void nsr_dbus_emit_upstream(const char *event_id, const char *relay_url,
                            const char *relay_state, const char *reason,
                            const char *event_state, void *user_data) {
  (void)event_id; (void)relay_url; (void)relay_state; (void)reason; (void)event_state;
  (void)user_data;
}
int nsr_dbus_print_upstream(const char *event_id) {
  (void)event_id;
  fprintf(stderr, "nostr-session-relayd: built without GIO; --upstream unavailable\n");
  return 1;
}
int nsr_dbus_print_stats(void) {
  fprintf(stderr,
          "nostr-session-relayd: built without GIO; --stats unavailable\n");
  return 1;
}

#endif /* NSR_HAVE_GDBUS */
