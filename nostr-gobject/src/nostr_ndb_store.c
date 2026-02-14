#include "nostr_store.h"
#include "nostr_event.h"
#include "nostr-error.h"
#include "json.h"
#include "storage_ndb.h"
#include <glib.h>
#include <string.h>

/* ============ Helpers ============ */

/* Decode a hex string to binary. Returns FALSE on invalid input. */
static gboolean
hex_decode(const char *hex, unsigned char *bin, size_t bin_len)
{
    for (size_t i = 0; i < bin_len; i++) {
        int hi = g_ascii_xdigit_value(hex[2 * i]);
        int lo = g_ascii_xdigit_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return FALSE;
        bin[i] = (unsigned char)((hi << 4) | lo);
    }
    return TRUE;
}

/* ============ GNostrNdbStore Implementation ============ */

struct _GNostrNdbStore {
    GObject parent_instance;
};

static void gnostr_ndb_store_iface_init(GNostrStoreInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GNostrNdbStore, gnostr_ndb_store, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_STORE, gnostr_ndb_store_iface_init))

/* ---- Core CRUD ---- */

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

/* ---- Note retrieval ---- */

static gchar *
ndb_store_get_note_by_id(GNostrStore *store, const gchar *id_hex, GError **error)
{
    (void)store;

    char *json = NULL;
    int json_len = 0;
    int rc = storage_ndb_get_note_by_id_nontxn(id_hex, &json, &json_len);
    if (rc != 0 || !json) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_NOT_FOUND,
                    "Note not found: %s", id_hex);
        return NULL;
    }

    /* json points to store-internal memory (do not free), copy for caller */
    return g_strndup(json, json_len);
}

static gchar *
ndb_store_get_note_by_key(GNostrStore *store, guint64 note_key, GError **error)
{
    (void)store;

    char *json = NULL;
    int json_len = 0;
    int rc = storage_ndb_get_note_json_by_key(note_key, &json, &json_len);
    if (rc != 0 || !json) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_NOT_FOUND,
                    "Note not found for key: %" G_GUINT64_FORMAT, note_key);
        return NULL;
    }

    /* json is caller-owned per storage_ndb.h contract */
    gchar *result = g_strndup(json, json_len);
    free(json);
    return result;
}

/* ---- Profile operations ---- */

static gchar *
ndb_store_get_profile_by_pubkey(GNostrStore *store, const gchar *pubkey_hex, GError **error)
{
    (void)store;

    unsigned char pk32[32];
    if (strlen(pubkey_hex) != 64 || !hex_decode(pubkey_hex, pk32, 32)) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                    "Invalid hex pubkey: %.8s...", pubkey_hex);
        return NULL;
    }

    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                            "Failed to begin NDB query transaction");
        return NULL;
    }

    char *json = NULL;
    int json_len = 0;
    rc = storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len);

    /* Copy before ending txn — json may reference txn-scoped memory */
    gchar *result = NULL;
    if (rc == 0 && json && json_len > 0)
        result = g_strndup(json, json_len);

    storage_ndb_end_query(txn);

    if (!result) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_NOT_FOUND,
                    "Profile not found for pubkey: %.8s...", pubkey_hex);
    }

    return result;
}

/* ---- Search ---- */

static GPtrArray *
ndb_store_text_search(GNostrStore *store, const gchar *query, gint limit, GError **error)
{
    (void)store;

    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                            "Failed to begin NDB query transaction");
        return NULL;
    }

    gchar *config = NULL;
    if (limit > 0)
        config = g_strdup_printf("{\"limit\":%d}", limit);

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_text_search(txn, query, config, &results, &count);
    g_free(config);
    storage_ndb_end_query(txn);

    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                    "Text search failed (rc=%d)", rc);
        if (results)
            storage_ndb_free_results(results, count);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < count; i++) {
        if (results[i])
            g_ptr_array_add(arr, g_strdup(results[i]));
    }

    if (results)
        storage_ndb_free_results(results, count);

    return arr;
}

static GPtrArray *
ndb_store_search_profile(GNostrStore *store, const gchar *query, gint limit, GError **error)
{
    (void)store;

    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                            "Failed to begin NDB query transaction");
        return NULL;
    }

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_search_profile(txn, query, limit > 0 ? limit : 20, &results, &count);
    storage_ndb_end_query(txn);

    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_FILTER,
                    "Profile search failed (rc=%d)", rc);
        if (results)
            storage_ndb_free_results(results, count);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < count; i++) {
        if (results[i])
            g_ptr_array_add(arr, g_strdup(results[i]));
    }

    if (results)
        storage_ndb_free_results(results, count);

    return arr;
}

/* ---- Reactive store ---- */

static guint64
ndb_store_subscribe(GNostrStore *store, const gchar *filter_json)
{
    (void)store;
    return storage_ndb_subscribe(filter_json);
}

static void
ndb_store_unsubscribe(GNostrStore *store, guint64 subid)
{
    (void)store;
    storage_ndb_unsubscribe(subid);
}

static gint
ndb_store_poll_notes(GNostrStore *store, guint64 subid, guint64 *note_keys, gint capacity)
{
    (void)store;
    return storage_ndb_poll_notes(subid, note_keys, capacity);
}

/* ---- Note metadata ---- */

static gboolean
ndb_store_get_note_counts(GNostrStore *store, const gchar *id_hex, GNostrNoteCounts *out)
{
    (void)store;

    unsigned char id32[32];
    if (strlen(id_hex) != 64 || !hex_decode(id_hex, id32, 32))
        return FALSE;

    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
    if (rc != 0 || !txn)
        return FALSE;

    StorageNdbNoteCounts ndb_counts;
    gboolean found = storage_ndb_read_note_counts(txn, id32, &ndb_counts);
    storage_ndb_end_query(txn);

    if (found && out) {
        out->total_reactions = ndb_counts.total_reactions;
        out->direct_replies = ndb_counts.direct_replies;
        out->thread_replies = ndb_counts.thread_replies;
        out->reposts = ndb_counts.reposts;
        out->quotes = ndb_counts.quotes;
    }

    return found;
}

static gboolean
ndb_store_write_note_counts(GNostrStore *store, const gchar *id_hex, const GNostrNoteCounts *counts)
{
    (void)store;

    unsigned char id32[32];
    if (strlen(id_hex) != 64 || !hex_decode(id_hex, id32, 32))
        return FALSE;

    StorageNdbNoteCounts ndb_counts = {
        .total_reactions = counts->total_reactions,
        .direct_replies = counts->direct_replies,
        .thread_replies = counts->thread_replies,
        .reposts = counts->reposts,
        .quotes = counts->quotes,
    };

    int rc = storage_ndb_write_note_counts(id32, &ndb_counts);
    return rc == 0;
}

/* ---- Batch operations ---- */

static GHashTable *
ndb_store_count_reactions_batch(GNostrStore *store, const gchar * const *event_ids, guint n_ids)
{
    (void)store;
    return storage_ndb_count_reactions_batch(event_ids, n_ids);
}

static GHashTable *
ndb_store_get_zap_stats_batch(GNostrStore *store, const gchar * const *event_ids, guint n_ids)
{
    (void)store;

    GHashTable *ndb_table = storage_ndb_get_zap_stats_batch(event_ids, n_ids);
    if (!ndb_table)
        return NULL;

    /* Convert StorageNdbZapStats* values to GNostrZapStats* */
    GHashTable *result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ndb_table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        StorageNdbZapStats *ndb_stats = value;
        GNostrZapStats *stats = g_new(GNostrZapStats, 1);
        stats->zap_count = ndb_stats->zap_count;
        stats->total_msat = ndb_stats->total_msat;
        g_hash_table_insert(result, g_strdup(key), stats);
    }

    g_hash_table_unref(ndb_table);
    return result;
}

/* ============ Interface wiring ============ */

static void
gnostr_ndb_store_iface_init(GNostrStoreInterface *iface)
{
    iface->save_event            = ndb_store_save_event;
    iface->query                 = ndb_store_query;
    iface->delete_event          = ndb_store_delete_event;
    iface->count                 = ndb_store_count;
    iface->get_note_by_id        = ndb_store_get_note_by_id;
    iface->get_note_by_key       = ndb_store_get_note_by_key;
    iface->get_profile_by_pubkey = ndb_store_get_profile_by_pubkey;
    iface->text_search           = ndb_store_text_search;
    iface->search_profile        = ndb_store_search_profile;
    iface->subscribe             = ndb_store_subscribe;
    iface->unsubscribe           = ndb_store_unsubscribe;
    iface->poll_notes            = ndb_store_poll_notes;
    iface->get_note_counts       = ndb_store_get_note_counts;
    iface->write_note_counts     = ndb_store_write_note_counts;
    iface->count_reactions_batch = ndb_store_count_reactions_batch;
    iface->get_zap_stats_batch   = ndb_store_get_zap_stats_batch;
}

static void
gnostr_ndb_store_class_init(GNostrNdbStoreClass *klass)
{
    (void)klass;
}

static void
gnostr_ndb_store_init(GNostrNdbStore *self)
{
    (void)self;
}

GNostrNdbStore *
gnostr_ndb_store_new(void)
{
    return g_object_new(GNOSTR_TYPE_NDB_STORE, NULL);
}
