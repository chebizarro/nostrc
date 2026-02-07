#include "nostr_store.h"
#include "nostr_event.h"
#include "nostr-error.h"
#include "json.h"
#include "storage_ndb.h"
#include <glib.h>
#include <string.h>

/* ============ GNostrNdbStore Implementation ============ */

struct _GNostrNdbStore {
    GObject parent_instance;
};

static void g_nostr_ndb_store_iface_init(GNostrStoreInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GNostrNdbStore, g_nostr_ndb_store, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(G_NOSTR_TYPE_STORE, g_nostr_ndb_store_iface_init))

/* save_event: serialize to JSON and ingest via storage_ndb */
static gboolean
ndb_store_save_event(GNostrStore *store, GNostrEvent *event, GError **error)
{
    (void)store;

    gchar *json = gnostr_event_to_json(event);
    if (!json) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                            "Failed to serialize event to JSON");
        return FALSE;
    }

    int rc = storage_ndb_ingest_event_json(json, NULL);
    g_free(json);

    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_EVENT,
                    "NDB ingest failed (rc=%d)", rc);
        return FALSE;
    }

    return TRUE;
}

/* query: serialize filter to JSON, query NDB, parse results to GNostrEvent array */
static GPtrArray *
ndb_store_query(GNostrStore *store, NostrFilter *filter, GError **error)
{
    (void)store;

    /* Serialize core NostrFilter to JSON */
    char *filter_json = nostr_filter_serialize(filter);
    if (!filter_json) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                            "Failed to serialize filter to JSON");
        return NULL;
    }

    /* Wrap in array brackets for storage_ndb_query */
    gchar *query_json = g_strdup_printf("[%s]", filter_json);
    free(filter_json);

    /* Begin read transaction */
    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                    "Failed to begin NDB query transaction (rc=%d)", rc);
        g_free(query_json);
        return NULL;
    }

    /* Execute query */
    char **results = NULL;
    int count = 0;
    rc = storage_ndb_query(txn, query_json, &results, &count);
    g_free(query_json);
    storage_ndb_end_query(txn);

    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                    "NDB query failed (rc=%d)", rc);
        if (results)
            storage_ndb_free_results(results, count);
        return NULL;
    }

    /* Parse results into GNostrEvent array */
    GPtrArray *events = g_ptr_array_new_with_free_func(g_object_unref);

    for (int i = 0; i < count; i++) {
        if (!results[i])
            continue;

        GError *parse_err = NULL;
        GNostrEvent *ev = gnostr_event_new_from_json(results[i], &parse_err);
        if (ev) {
            g_ptr_array_add(events, ev);
        } else {
            g_debug("nostr_ndb_store: failed to parse event %d: %s",
                    i, parse_err ? parse_err->message : "unknown");
            g_clear_error(&parse_err);
        }
    }

    if (results)
        storage_ndb_free_results(results, count);

    return events;
}

/* delete_event: NDB is append-only, deletion not supported */
static gboolean
ndb_store_delete_event(GNostrStore *store, const gchar *event_id, GError **error)
{
    (void)store;
    (void)event_id;

    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                        "NDB store does not support event deletion");
    return FALSE;
}

/* count: query and return result count */
static gint
ndb_store_count(GNostrStore *store, NostrFilter *filter, GError **error)
{
    (void)store;

    char *filter_json = nostr_filter_serialize(filter);
    if (!filter_json) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                            "Failed to serialize filter to JSON");
        return -1;
    }

    gchar *query_json = g_strdup_printf("[%s]", filter_json);
    free(filter_json);

    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                    "Failed to begin NDB query transaction (rc=%d)", rc);
        g_free(query_json);
        return -1;
    }

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_query(txn, query_json, &results, &count);
    g_free(query_json);
    storage_ndb_end_query(txn);

    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                    "NDB count query failed (rc=%d)", rc);
        if (results)
            storage_ndb_free_results(results, count);
        return -1;
    }

    if (results)
        storage_ndb_free_results(results, count);

    return count;
}

/* Wire up the interface */
static void
g_nostr_ndb_store_iface_init(GNostrStoreInterface *iface)
{
    iface->save_event = ndb_store_save_event;
    iface->query = ndb_store_query;
    iface->delete_event = ndb_store_delete_event;
    iface->count = ndb_store_count;
}

static void
g_nostr_ndb_store_class_init(GNostrNdbStoreClass *klass)
{
    (void)klass;
}

static void
g_nostr_ndb_store_init(GNostrNdbStore *self)
{
    (void)self;
}

GNostrNdbStore *
g_nostr_ndb_store_new(void)
{
    return g_object_new(G_NOSTR_TYPE_NDB_STORE, NULL);
}
