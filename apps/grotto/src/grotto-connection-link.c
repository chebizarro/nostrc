/* SPDX-License-Identifier: MIT */
#include "grotto-connection-link.h"

typedef struct { GDBusConnection *bus; gboolean pending, dirty; guint64 generation; } ConnectionHost;
static void host_free(gpointer data) {
  ConnectionHost *host = data; g_object_unref(host->bus); g_free(host);
}
static guint64 ready_generation(GrottoRemotePanel *panel) {
  if (!gtk_widget_get_mapped(GTK_WIDGET(panel)) ||
      !grotto_panel_can_command(GROTTO_PANEL(panel), GROTTO_PANEL_CONFIGURE_REMOTE)) return 0;
  GrottoPanelsService *service = grotto_panel_get_service(GROTTO_PANEL(panel));
  GListModel *model = service ? grotto_panels_service_get_model(service, GROTTO_PANELS_STATUS) : NULL;
  g_autoptr(GrottoPanelsItem) item = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
  const GrottoPanelsMetadata *m = item ? grotto_panels_item_get_metadata(item) : NULL;
  return m && m->remote_enabled && !g_strcmp0(m->state, "LISTENING") ? m->owner_generation : 0;
}
static void requested(GrottoRemotePanel *panel, gpointer data);
static void offer_ready(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoRemotePanel *panel = data; ConnectionHost *host = g_object_get_data(G_OBJECT(panel), "connection-host");
  host->pending = FALSE;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  guint64 generation = ready_generation(panel);
  if (generation && host->dirty) { host->dirty = FALSE; requested(panel, host); }
  else if (generation && generation == host->generation) {
    g_autoptr(GVariant) offer = reply ? g_variant_get_child_value(reply, 0) : NULL; const gchar *uri = NULL;
    if (offer) g_variant_lookup(offer, "uri", "&s", &uri);
    grotto_remote_panel_set_connection_link(panel, uri);
  }
  g_object_unref(panel);
}
static void requested(GrottoRemotePanel *panel, gpointer data) {
  ConnectionHost *host = data;
  guint64 generation = ready_generation(panel);
  if (!generation) { grotto_remote_panel_set_connection_link(panel, NULL); return; }
  if (host->pending) { host->dirty = TRUE; return; }
  host->pending = TRUE; host->dirty = FALSE; host->generation = generation;
  g_dbus_connection_call(host->bus, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "GetConnectionLink",
    g_variant_new("(s)", ""), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 10000,
    grotto_panel_get_cancellable(GROTTO_PANEL(panel)), offer_ready, g_object_ref(panel));
}
void grotto_connection_link_bind(GrottoRemotePanel *panel, GDBusConnection *bus) {
  g_return_if_fail(GROTTO_IS_REMOTE_PANEL(panel)); g_return_if_fail(G_IS_DBUS_CONNECTION(bus));
  g_return_if_fail(!g_object_get_data(G_OBJECT(panel), "connection-host"));
  ConnectionHost *host = g_new0(ConnectionHost, 1); host->bus = g_object_ref(bus);
  g_object_set_data_full(G_OBJECT(panel), "connection-host", host, host_free);
  g_signal_connect(panel, "connection-link-requested", G_CALLBACK(requested), host);
  if (gtk_widget_get_mapped(GTK_WIDGET(panel))) requested(panel, host);
}
