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
    "    <method name='GetStats'>"
    "      <arg name='stats' type='a{sv}' direction='out'/>"
    "    </method>"
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
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
              "No such property: %s", name);
  return NULL;
}

static void method_call(GDBusConnection *c, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer user_data) {
  (void)c; (void)sender; (void)path; (void)iface; (void)params;
  NsrDbus *d = user_data;
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
  g_free(d);
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

#else /* !NSR_HAVE_GDBUS */

void nsr_dbus_start(const NsrDbusInfo *info) { (void)info; }
void nsr_dbus_stop(void) {}
int nsr_dbus_print_stats(void) {
  fprintf(stderr,
          "nostr-session-relayd: built without GIO; --stats unavailable\n");
  return 1;
}

#endif /* NSR_HAVE_GDBUS */
