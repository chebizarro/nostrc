#include "gh-net-tls.h"

static void
on_network_event(SoupMessage *message, GSocketClientEvent event, GIOStream *connection,
                 gpointer data)
{
  (void)message;
  (void)data;
  if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING || !G_IS_TLS_CLIENT_CONNECTION(connection))
    return;
  /* A glib-networking property (not GIO API); another TLS backend may lack
   * it, and then has no such cache to seed. */
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(connection), "session-resumption-enabled"))
    g_object_set(connection, "session-resumption-enabled", FALSE, NULL);
}

void
gh_net_tls_no_resumption(SoupMessage *message)
{
  g_return_if_fail(SOUP_IS_MESSAGE(message));
  g_signal_connect(message, "network-event", G_CALLBACK(on_network_event), NULL);
}
