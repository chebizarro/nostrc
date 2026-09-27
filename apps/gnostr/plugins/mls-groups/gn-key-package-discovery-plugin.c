/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-discovery-plugin.c - GnKpBackend on the plugin host
 * (local store + raw relay queries). nostrc-prqu.11.
 */
#include "gn-key-package-discovery.h"

#include <gnostr-plugin-api.h>

static GPtrArray *
plugin_query_local(gpointer data, const char *filter_json)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *events = gnostr_plugin_context_query_events(data, filter_json, &error);
  if (error)
    g_debug("KeyPackageDiscovery: local query failed: %s", error->message);
  return events;
}

static void
plugin_query_relays_async(gpointer data, const char *const *relays, const char *filter_json,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  GnostrPluginRelayQuery query = {
    .relay_urls = relays,
    .n_relay_urls = relays ? g_strv_length((char **)relays) : 0,
    .filter_json = filter_json,
  };
  gnostr_plugin_context_query_relays_async(data, &query, cancellable, callback, user_data);
}

static GPtrArray *
plugin_query_relays_finish(gpointer data, GAsyncResult *result, GError **error)
{
  g_autoptr(GPtrArray) raw = gnostr_plugin_context_query_relays_finish(data, result, error);
  if (!raw)
    return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < raw->len; i++) {
    GnostrPluginRelayEvent *ev = g_ptr_array_index(raw, i);
    if (ev && ev->event_json)
      g_ptr_array_add(out, g_strdup(ev->event_json));
  }
  return out;
}

static char **
plugin_own_relays(gpointer data)
{
  return gnostr_plugin_context_get_relay_urls(data, NULL);
}

void
gn_kp_backend_init_for_plugin(GnKpBackend *backend, GnostrPluginContext *context)
{
  backend->query_local = plugin_query_local;
  backend->query_relays_async = plugin_query_relays_async;
  backend->query_relays_finish = plugin_query_relays_finish;
  backend->own_relays = plugin_own_relays;
  backend->data = context;
}
