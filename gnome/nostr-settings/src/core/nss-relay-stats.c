/* nss-relay-stats.c — see nss-relay-stats.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-relay-stats.h"

#include <string.h>

void
nss_relay_stats_clear(NssRelayStats *s)
{
  g_clear_pointer(&s->storage_backend, g_free);
  g_clear_pointer(&s->requested_backend, g_free);
  g_clear_pointer(&s->storage_path, g_free);
  g_clear_pointer(&s->supported_nips, g_array_unref);
  g_clear_pointer(&s->version, g_free);
  memset(s, 0, sizeof *s);
}

void
nss_relay_stats_from_variant(NssRelayStats *out, GVariant *dict)
{
  nss_relay_stats_clear(out);
  out->event_count = -1;
  out->supported_nips = g_array_new(FALSE, FALSE, sizeof(guint32));
  if (dict == NULL || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT))
    return;
  (void)g_variant_lookup(dict, "event_count", "x", &out->event_count);
  (void)g_variant_lookup(dict, "storage_bytes", "t", &out->storage_bytes);
  (void)g_variant_lookup(dict, "connected_clients", "u", &out->connected_clients);
  (void)g_variant_lookup(dict, "uptime", "t", &out->uptime);
  (void)g_variant_lookup(dict, "storage_backend", "s", &out->storage_backend);
  (void)g_variant_lookup(dict, "requested_storage_backend", "s", &out->requested_backend);
  (void)g_variant_lookup(dict, "storage_path", "s", &out->storage_path);
  (void)g_variant_lookup(dict, "retention_supported", "b", &out->retention_supported);
  (void)g_variant_lookup(dict, "connections_total", "t", &out->connections_total);
  (void)g_variant_lookup(dict, "subscriptions", "u", &out->subscriptions);
  (void)g_variant_lookup(dict, "version", "s", &out->version);
  g_autoptr(GVariant) nips = g_variant_lookup_value(dict, "supported_nips", G_VARIANT_TYPE("au"));
  if (nips) {
    gsize n = 0;
    const guint32 *a = g_variant_get_fixed_array(nips, &n, sizeof(guint32));
    g_array_append_vals(out->supported_nips, a, (guint)n);
  }
  if (out->storage_backend == NULL)
    out->storage_backend = g_strdup("none");
}

gboolean
nss_relay_stats_has_storage(const NssRelayStats *s)
{
  return s->storage_backend && *s->storage_backend &&
         !g_str_equal(s->storage_backend, "none");
}

static GVariant *
call(GDBusConnection *bus, GError **error)
{
  return g_dbus_connection_call_sync(bus, NSS_RELAY_BUS_NAME, NSS_RELAY_OBJ_PATH,
                                     NSS_RELAY_IFACE, "GetStats", NULL,
                                     G_VARIANT_TYPE("(a{sv})"),
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, error);
}

gboolean
nss_relay_stats_fetch(GDBusConnection *bus, NssRelayStats *out, GError **error)
{
  g_autoptr(GVariant) r = call(bus, error);
  if (r == NULL)
    return FALSE;
  g_autoptr(GVariant) d = g_variant_get_child_value(r, 0);
  nss_relay_stats_from_variant(out, d);
  return TRUE;
}

static void
on_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (r == NULL)
    g_task_return_error(task, err);
  else
    g_task_return_pointer(task, r, (GDestroyNotify)g_variant_unref);
  g_object_unref(task);
}

void
nss_relay_stats_fetch_async(GDBusConnection *bus, GCancellable *c, GAsyncReadyCallback cb,
                            gpointer user_data)
{
  GTask *task = g_task_new(NULL, c, cb, user_data);
  g_dbus_connection_call(bus, NSS_RELAY_BUS_NAME, NSS_RELAY_OBJ_PATH, NSS_RELAY_IFACE,
                         "GetStats", NULL, G_VARIANT_TYPE("(a{sv})"),
                         G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, c, on_reply, task);
}

gboolean
nss_relay_stats_fetch_finish(GAsyncResult *res, NssRelayStats *out, GError **error)
{
  g_autoptr(GVariant) r = g_task_propagate_pointer(G_TASK(res), error);
  if (r == NULL)
    return FALSE;
  g_autoptr(GVariant) d = g_variant_get_child_value(r, 0);
  nss_relay_stats_from_variant(out, d);
  return TRUE;
}

gchar *
nss_format_storage_title(const NssRelayStats *s)
{
  if (nss_relay_stats_has_storage(s))
    return g_strdup(s->storage_backend);
  if (s->requested_backend && *s->requested_backend &&
      !g_str_equal(s->requested_backend, "none"))
    return g_strdup_printf("No storage — this relay keeps nothing (%s is not available)",
                           s->requested_backend);
  return g_strdup("No storage — this relay keeps nothing");
}

static gchar *
group_digits(gint64 v)
{
  g_autofree gchar *raw = g_strdup_printf("%" G_GINT64_FORMAT, v < 0 ? -v : v);
  GString *s = g_string_new(v < 0 ? "-" : "");
  gsize n = strlen(raw);
  for (gsize i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0)
      g_string_append_c(s, ',');
    g_string_append_c(s, raw[i]);
  }
  return g_string_free(s, FALSE);
}

gchar *
nss_format_storage_detail(const NssRelayStats *s)
{
  g_autofree gchar *size = g_format_size(s->storage_bytes);
  if (!nss_relay_stats_has_storage(s))
    return g_strdup_printf("%s on disk", size);
  if (s->event_count < 0)
    return g_strdup_printf("%s on disk · event count unavailable", size);
  g_autofree gchar *n = group_digits(s->event_count);
  return g_strdup_printf("%s on disk · %s event%s", size, n,
                         s->event_count == 1 ? "" : "s");
}

gchar *
nss_format_uptime(guint64 t)
{
  guint64 d = t / 86400, h = (t % 86400) / 3600, m = (t % 3600) / 60, sec = t % 60;
  if (d > 0)
    return g_strdup_printf("%" G_GUINT64_FORMAT " d %" G_GUINT64_FORMAT " h", d, h);
  if (h > 0)
    return g_strdup_printf("%" G_GUINT64_FORMAT " h %" G_GUINT64_FORMAT " min", h, m);
  if (m > 0)
    return g_strdup_printf("%" G_GUINT64_FORMAT " min", m);
  return g_strdup_printf("%" G_GUINT64_FORMAT " s", sec);
}

gchar *
nss_format_nips(const GArray *nips)
{
  GString *s = g_string_new(NULL);
  for (guint i = 0; nips && i < nips->len; i++)
    g_string_append_printf(s, "%s%" G_GUINT32_FORMAT, i ? ", " : "",
                           g_array_index(nips, guint32, i));
  return g_string_free(s, FALSE);
}

guint64
nss_disk_usage(const gchar *path)
{
  g_autoptr(GFile) f = g_file_new_for_path(path);
  guint64 bytes = 0;
  if (!g_file_measure_disk_usage(f, G_FILE_MEASURE_NO_XDEV, NULL, NULL, NULL,
                                 &bytes, NULL, NULL, NULL))
    return 0;
  return bytes;
}

gchar *
nss_session_relay_storage_dir(void)
{
  return g_build_filename(g_get_user_data_dir(), "nostr", "session-relay", NULL);
}
