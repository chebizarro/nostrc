#include "nostr_store.h"
#include "nostr_event.h"
#include <glib.h>

/* ============ GNostrStore Interface ============ */

G_DEFINE_INTERFACE(GNostrStore, g_nostr_store, G_TYPE_OBJECT)

static void
g_nostr_store_default_init(GNostrStoreInterface *iface)
{
    (void)iface;
}

gboolean
g_nostr_store_save_event(GNostrStore *self, GNostrEvent *event, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(G_NOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    iface = G_NOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->save_event != NULL, FALSE);

    return iface->save_event(self, event, error);
}

GPtrArray *
g_nostr_store_query(GNostrStore *self, NostrFilter *filter, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(G_NOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(filter != NULL, NULL);

    iface = G_NOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->query != NULL, NULL);

    return iface->query(self, filter, error);
}

gboolean
g_nostr_store_delete_event(GNostrStore *self, const gchar *event_id, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(G_NOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(event_id != NULL, FALSE);

    iface = G_NOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->delete_event != NULL, FALSE);

    return iface->delete_event(self, event_id, error);
}

gint
g_nostr_store_count(GNostrStore *self, NostrFilter *filter, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(G_NOSTR_IS_STORE(self), -1);
    g_return_val_if_fail(filter != NULL, -1);

    iface = G_NOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->count != NULL, -1);

    return iface->count(self, filter, error);
}
