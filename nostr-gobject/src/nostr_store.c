#include "nostr_store.h"
#include "nostr_event.h"
#include <glib.h>

/* ============ GNostrStore Interface ============ */

G_DEFINE_INTERFACE(GNostrStore, gnostr_store, G_TYPE_OBJECT)

static void
gnostr_store_default_init(GNostrStoreInterface *iface)
{
    (void)iface;
}

/* ---- Core CRUD (Phase 1) ---- */

gboolean
gnostr_store_save_event(GNostrStore *self, GNostrEvent *event, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->save_event != NULL, FALSE);

    return iface->save_event(self, event, error);
}

GPtrArray *
gnostr_store_query(GNostrStore *self, NostrFilter *filter, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(filter != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->query != NULL, NULL);

    return iface->query(self, filter, error);
}

gboolean
gnostr_store_delete_event(GNostrStore *self, const gchar *event_id, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(event_id != NULL, FALSE);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->delete_event != NULL, FALSE);

    return iface->delete_event(self, event_id, error);
}

gint
gnostr_store_count(GNostrStore *self, NostrFilter *filter, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), -1);
    g_return_val_if_fail(filter != NULL, -1);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->count != NULL, -1);

    return iface->count(self, filter, error);
}

/* ---- Note retrieval ---- */

gchar *
gnostr_store_get_note_by_id(GNostrStore *self, const gchar *id_hex, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(id_hex != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_note_by_id != NULL, NULL);

    return iface->get_note_by_id(self, id_hex, error);
}

gchar *
gnostr_store_get_note_by_key(GNostrStore *self, guint64 note_key, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_note_by_key != NULL, NULL);

    return iface->get_note_by_key(self, note_key, error);
}

/* ---- Profile operations ---- */

gchar *
gnostr_store_get_profile_by_pubkey(GNostrStore *self, const gchar *pubkey_hex, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(pubkey_hex != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_profile_by_pubkey != NULL, NULL);

    return iface->get_profile_by_pubkey(self, pubkey_hex, error);
}

/* ---- Search ---- */

GPtrArray *
gnostr_store_text_search(GNostrStore *self, const gchar *query, gint limit, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(query != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->text_search != NULL, NULL);

    return iface->text_search(self, query, limit, error);
}

GPtrArray *
gnostr_store_search_profile(GNostrStore *self, const gchar *query, gint limit, GError **error)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(query != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->search_profile != NULL, NULL);

    return iface->search_profile(self, query, limit, error);
}

/* ---- Reactive store ---- */

guint64
gnostr_store_subscribe(GNostrStore *self, const gchar *filter_json)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), 0);
    g_return_val_if_fail(filter_json != NULL, 0);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->subscribe != NULL, 0);

    return iface->subscribe(self, filter_json);
}

void
gnostr_store_unsubscribe(GNostrStore *self, guint64 subid)
{
    GNostrStoreInterface *iface;

    g_return_if_fail(GNOSTR_IS_STORE(self));

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_if_fail(iface->unsubscribe != NULL);

    iface->unsubscribe(self, subid);
}

gint
gnostr_store_poll_notes(GNostrStore *self, guint64 subid, guint64 *note_keys, gint capacity)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), 0);
    g_return_val_if_fail(note_keys != NULL, 0);
    g_return_val_if_fail(capacity > 0, 0);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->poll_notes != NULL, 0);

    return iface->poll_notes(self, subid, note_keys, capacity);
}

/* ---- Note metadata ---- */

gboolean
gnostr_store_get_note_counts(GNostrStore *self, const gchar *id_hex, GNostrNoteCounts *out)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(id_hex != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_note_counts != NULL, FALSE);

    return iface->get_note_counts(self, id_hex, out);
}

gboolean
gnostr_store_write_note_counts(GNostrStore *self, const gchar *id_hex, const GNostrNoteCounts *counts)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), FALSE);
    g_return_val_if_fail(id_hex != NULL, FALSE);
    g_return_val_if_fail(counts != NULL, FALSE);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->write_note_counts != NULL, FALSE);

    return iface->write_note_counts(self, id_hex, counts);
}

/* ---- Batch operations ---- */

GHashTable *
gnostr_store_count_reactions_batch(GNostrStore *self, const gchar * const *event_ids, guint n_ids)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(event_ids != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->count_reactions_batch != NULL, NULL);

    return iface->count_reactions_batch(self, event_ids, n_ids);
}

GHashTable *
gnostr_store_get_zap_stats_batch(GNostrStore *self, const gchar * const *event_ids, guint n_ids)
{
    GNostrStoreInterface *iface;

    g_return_val_if_fail(GNOSTR_IS_STORE(self), NULL);
    g_return_val_if_fail(event_ids != NULL, NULL);

    iface = GNOSTR_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_zap_stats_batch != NULL, NULL);

    return iface->get_zap_stats_batch(self, event_ids, n_ids);
}
