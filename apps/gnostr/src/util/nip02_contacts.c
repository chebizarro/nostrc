/**
 * NIP-02 Contact List Service Implementation
 *
 * Manages contact lists (kind 3) for following relationships.
 * Parses p tags with full metadata: pubkey, relay hint, petname.
 * Caches results in nostrdb for offline access.
 */

#include "nip02_contacts.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <glib.h>
#include <string.h>

#ifndef GNOSTR_NIP02_CONTACTS_TEST_ONLY
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include "../ipc/gnostr-signer-service.h"
#include "utils.h"
#endif

/* Kind 3 = Contact List per NIP-02 */
#define CONTACT_LIST_KIND 3

/* ---- Contact Entry ---- */

void gnostr_contact_entry_free(GnostrContactEntry *entry) {
    if (entry) {
        g_free(entry->pubkey_hex);
        g_free(entry->relay_hint);
        g_free(entry->petname);
        g_free(entry);
    }
}

static GnostrContactEntry *contact_entry_new(const char *pubkey,
                                              const char *relay_hint,
                                              const char *petname) {
    GnostrContactEntry *entry = g_new0(GnostrContactEntry, 1);
    entry->pubkey_hex = g_strdup(pubkey);
    entry->relay_hint = (relay_hint && *relay_hint) ? g_strdup(relay_hint) : NULL;
    entry->petname = (petname && *petname) ? g_strdup(petname) : NULL;
    return entry;
}

/* ---- Internal Data Structure ---- */

struct _GnostrContactList {
    /* Hash table for O(1) lookup: key = pubkey, value = GnostrContactEntry* */
    GHashTable *contacts;

    /* State */
    gint64 last_event_time;      /* created_at of last loaded event */
    char *user_pubkey;           /* Pubkey of user whose contacts these are */

    /* Thread safety */
    GMutex lock;
};

/* Singleton instance */
static GnostrContactList *s_default_instance = NULL;
static GMutex s_init_lock;

/* ---- Internal Helpers ---- */

static void contact_list_clear(GnostrContactList *self) {
    g_hash_table_remove_all(self->contacts);
    self->last_event_time = 0;
}

/* ---- Public API Implementation ---- */

GnostrContactList *gnostr_contact_list_get_default(void) {
    g_mutex_lock(&s_init_lock);
    if (!s_default_instance) {
        s_default_instance = g_new0(GnostrContactList, 1);
        g_mutex_init(&s_default_instance->lock);
        s_default_instance->contacts = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, (GDestroyNotify)gnostr_contact_entry_free);
        s_default_instance->last_event_time = 0;
        s_default_instance->user_pubkey = NULL;
    }
    g_mutex_unlock(&s_init_lock);
    return s_default_instance;
}

void gnostr_contact_list_shutdown(void) {
    g_mutex_lock(&s_init_lock);
    if (s_default_instance) {
        g_mutex_lock(&s_default_instance->lock);
        g_hash_table_destroy(s_default_instance->contacts);
        g_free(s_default_instance->user_pubkey);
        g_mutex_unlock(&s_default_instance->lock);
        g_mutex_clear(&s_default_instance->lock);
        g_free(s_default_instance);
        s_default_instance = NULL;
    }
    g_mutex_unlock(&s_init_lock);
}

#ifndef GNOSTR_NIP02_CONTACTS_TEST_ONLY

gboolean gnostr_contact_list_load_from_json(GnostrContactList *self,
                                             const char *event_json) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        g_warning("nip02_contacts: failed to parse event JSON");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    int kind = nostr_event_get_kind(event);
    if (kind != CONTACT_LIST_KIND) {
        g_warning("nip02_contacts: not a kind 3 event (got kind %d)", kind);
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    gint64 event_time = nostr_event_get_created_at(event);
    if (event_time <= self->last_event_time) {
        g_debug("nip02_contacts: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return TRUE; /* Not an error, just older data */
    }

    /* Store user pubkey from event author */
    const char *author = nostr_event_get_pubkey(event);
    if (author) {
        g_free(self->user_pubkey);
        self->user_pubkey = g_strdup(author);
    }

    /* Clear existing data and load new */
    contact_list_clear(self);
    self->last_event_time = event_time;

    /* Parse p tags using NostrTags API */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get(tag, 0);
            if (!tag_name || strcmp(tag_name, "p") != 0) continue;

            /* NIP-02 p-tag format: ["p", "<pubkey>", "<relay>", "<petname>"] */
            const char *pubkey = nostr_tag_get(tag, 1);
            if (!pubkey || strlen(pubkey) != 64) continue;

            /* Optional fields */
            const char *relay_hint = (nostr_tag_size(tag) > 2) ? nostr_tag_get(tag, 2) : NULL;
            const char *petname = (nostr_tag_size(tag) > 3) ? nostr_tag_get(tag, 3) : NULL;

            /* Skip duplicates */
            if (g_hash_table_contains(self->contacts, pubkey)) {
                g_debug("nip02_contacts: skipping duplicate pubkey %.8s", pubkey);
                continue;
            }

            GnostrContactEntry *entry = contact_entry_new(pubkey, relay_hint, petname);
            g_hash_table_insert(self->contacts, entry->pubkey_hex, entry);
            g_debug("nip02_contacts: loaded contact %.8s relay=%s petname=%s",
                    pubkey,
                    relay_hint ? relay_hint : "(none)",
                    petname ? petname : "(none)");
        }
    }

    nostr_event_free(event);
    g_mutex_unlock(&self->lock);

    g_message("nip02_contacts: loaded %u contacts from kind 3 event",
              g_hash_table_size(self->contacts));

    return TRUE;
}

#else /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */

gboolean gnostr_contact_list_load_from_json(GnostrContactList *self,
                                             const char *event_json) {
    (void)self;
    (void)event_json;
    return FALSE;
}

#endif /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */

/* ---- Async Fetch Implementation ---- */

typedef struct {
    GnostrContactList *contact_list;
    GnostrContactListFetchCallback callback;
    gpointer user_data;
    char *pubkey_hex;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

#ifndef GNOSTR_NIP02_CONTACTS_TEST_ONLY

/* Static pool for contact list operations */
static GNostrPool *s_contact_list_pool = NULL;

/* Callback when relay query completes */
static void on_contact_list_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchContext *ctx = (FetchContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(
        GNOSTR_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("nip02_contacts: query failed: %s", err->message);
        }
        if (ctx->callback) {
            ctx->callback(ctx->contact_list, FALSE, ctx->user_data);
        }
        g_error_free(err);
        fetch_context_free(ctx);
        return;
    }

    gboolean success = FALSE;
    gint64 newest_created_at = 0;
    const char *newest_event_json = NULL;

    /* Find the newest contact list event */
    if (results && results->len > 0) {
        for (guint i = 0; i < results->len; i++) {
            const char *json_str = g_ptr_array_index(results, i);

            NostrEvent *event = nostr_event_new();
            int parse_rc = nostr_event_deserialize_compact(event, json_str, NULL);
            if (parse_rc != 1) {
                nostr_event_free(event);
                continue;
            }

            /* Verify kind */
            int kind = nostr_event_get_kind(event);
            if (kind != CONTACT_LIST_KIND) {
                nostr_event_free(event);
                continue;
            }

            /* Check timestamp */
            gint64 event_time = nostr_event_get_created_at(event);

            if (event_time > newest_created_at) {
                newest_created_at = event_time;
                newest_event_json = json_str;
            }
            nostr_event_free(event);
        }
    }

    /* Load the newest event */
    if (newest_event_json) {
        /* Ingest into nostrdb for caching */
        storage_ndb_ingest_event_json(newest_event_json, NULL);

        if (gnostr_contact_list_load_from_json(ctx->contact_list, newest_event_json)) {
            success = TRUE;
        }
    } else {
        g_debug("nip02_contacts: no kind 3 events found for %.8s", ctx->pubkey_hex);
    }

    if (results) g_ptr_array_unref(results);

    if (ctx->callback) {
        ctx->callback(ctx->contact_list, success, ctx->user_data);
    }

    fetch_context_free(ctx);
}

void gnostr_contact_list_fetch_async(GnostrContactList *self,
                                      const char *pubkey_hex,
                                      const char * const *relays,
                                      GnostrContactListFetchCallback callback,
                                      gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);

    FetchContext *ctx = g_new0(FetchContext, 1);
    ctx->contact_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pubkey_hex = g_strdup(pubkey_hex);

    /* Build filter for kind 3 by author */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { CONTACT_LIST_KIND };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 5);  /* Get a few to find newest */

    /* Get relay URLs */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);

    if (relays && relays[0]) {
        /* Use provided relays */
        for (const char * const *r = relays; *r; r++) {
            g_ptr_array_add(relay_arr, g_strdup(*r));
        }
    } else {
        /* Fall back to configured relays */
        gnostr_load_relays_into(relay_arr);
    }

    const char **urls = g_new0(const char*, relay_arr->len + 1);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    /* Use static pool */
    if (!s_contact_list_pool) {
        s_contact_list_pool = gnostr_pool_new();
    }

    g_message("nip02_contacts: fetching kind %d for pubkey %.8s from %u relays",
              CONTACT_LIST_KIND, pubkey_hex, relay_arr->len);

        gnostr_pool_sync_relays(s_contact_list_pool, (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      /* nostrc-uaf3: task takes ownership of _qf — do NOT stash on pool */
      gnostr_pool_query_async(s_contact_list_pool, _qf, NULL, /* cancellable */
        on_contact_list_query_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

#else /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */

void gnostr_contact_list_fetch_async(GnostrContactList *self,
                                      const char *pubkey_hex,
                                      const char * const *relays,
                                      GnostrContactListFetchCallback callback,
                                      gpointer user_data) {
    (void)relays;
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }
    g_message("nip02_contacts: fetch requested for pubkey %s (test mode - stub)", pubkey_hex);
    if (callback) callback(self, TRUE, user_data);
}

#endif /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */

/* ---- Query Functions ---- */

gboolean gnostr_contact_list_is_following(GnostrContactList *self,
                                           const char *pubkey_hex) {
    if (!self || !pubkey_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = g_hash_table_contains(self->contacts, pubkey_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

const char *gnostr_contact_list_get_relay_hint(GnostrContactList *self,
                                                const char *pubkey_hex) {
    if (!self || !pubkey_hex) return NULL;
    g_mutex_lock(&self->lock);
    GnostrContactEntry *entry = g_hash_table_lookup(self->contacts, pubkey_hex);
    const char *result = entry ? entry->relay_hint : NULL;
    g_mutex_unlock(&self->lock);
    return result;
}

const char *gnostr_contact_list_get_petname(GnostrContactList *self,
                                             const char *pubkey_hex) {
    if (!self || !pubkey_hex) return NULL;
    g_mutex_lock(&self->lock);
    GnostrContactEntry *entry = g_hash_table_lookup(self->contacts, pubkey_hex);
    const char *result = entry ? entry->petname : NULL;
    g_mutex_unlock(&self->lock);
    return result;
}

const GnostrContactEntry *gnostr_contact_list_get_entry(GnostrContactList *self,
                                                         const char *pubkey_hex) {
    if (!self || !pubkey_hex) return NULL;
    g_mutex_lock(&self->lock);
    GnostrContactEntry *entry = g_hash_table_lookup(self->contacts, pubkey_hex);
    g_mutex_unlock(&self->lock);
    return entry;
}

/* ---- Accessors ---- */

const char **gnostr_contact_list_get_pubkeys(GnostrContactList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->contacts);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->contacts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

const GnostrContactEntry **gnostr_contact_list_get_entries(GnostrContactList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->contacts);
    const GnostrContactEntry **result = g_new0(const GnostrContactEntry *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->contacts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const GnostrContactEntry *)value;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

size_t gnostr_contact_list_get_count(GnostrContactList *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    size_t count = g_hash_table_size(self->contacts);
    g_mutex_unlock(&self->lock);
    return count;
}

const char *gnostr_contact_list_get_user_pubkey(GnostrContactList *self) {
    if (!self) return NULL;
    /* No lock needed for read of pointer - caller should not free */
    return self->user_pubkey;
}

gint64 gnostr_contact_list_get_last_update(GnostrContactList *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    gint64 result = self->last_event_time;
    g_mutex_unlock(&self->lock);
    return result;
}

/* ---- Convenience Functions ---- */

gboolean gnostr_contact_list_load_from_ndb(GnostrContactList *self,
                                            const char *pubkey_hex) {
    if (!self || !pubkey_hex || strlen(pubkey_hex) != 64) return FALSE;

    /* Query nostrdb for kind 3 from this author */
    g_autofree char *filter_json = g_strdup_printf(
        "[{\"kinds\":[3],\"authors\":[\"%s\"],\"limit\":1}]",
        pubkey_hex);

    void *txn = NULL;
    int rc = storage_ndb_begin_query(&txn);
    if (rc != 0 || !txn) {
        return FALSE;
    }

    char **results = NULL;
    int count = 0;
    rc = storage_ndb_query(txn, filter_json, &results, &count);

    if (rc != 0 || count == 0 || !results) {
        storage_ndb_end_query(txn);
        return FALSE;
    }

    /* Load the first (newest) result */
    gboolean success = gnostr_contact_list_load_from_json(self, results[0]);

    storage_ndb_free_results(results, count);
    storage_ndb_end_query(txn);

    if (success) {
        g_message("nip02_contacts: loaded from nostrdb cache");
    }

    return success;
}

GPtrArray *gnostr_contact_list_get_pubkeys_with_relay_hints(GnostrContactList *self,
                                                             GPtrArray **relay_hints) {
    if (!self) {
        if (relay_hints) *relay_hints = NULL;
        return NULL;
    }

    g_mutex_lock(&self->lock);

    guint n = g_hash_table_size(self->contacts);
    GPtrArray *pubkeys = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *hints = relay_hints ? g_ptr_array_new_with_free_func(g_free) : NULL;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->contacts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnostrContactEntry *entry = (GnostrContactEntry *)value;
        g_ptr_array_add(pubkeys, g_strdup(entry->pubkey_hex));
        if (hints) {
            g_ptr_array_add(hints, entry->relay_hint ? g_strdup(entry->relay_hint) : NULL);
        }
    }

    g_mutex_unlock(&self->lock);

    if (relay_hints) *relay_hints = hints;
    return pubkeys;
}

/* ---- Mutation Functions (nostrc-s0e0) ---- */

static gboolean is_valid_hex_pubkey(const char *s) {
    if (!s || strlen(s) != 64) return FALSE;
    for (int i = 0; i < 64; i++) {
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    }
    return TRUE;
}

gboolean gnostr_contact_list_add(GnostrContactList *self,
                                  const char *pubkey_hex,
                                  const char *relay_hint) {
    if (!self || !is_valid_hex_pubkey(pubkey_hex)) return FALSE;

    g_mutex_lock(&self->lock);
    if (g_hash_table_contains(self->contacts, pubkey_hex)) {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }
    GnostrContactEntry *entry = contact_entry_new(pubkey_hex, relay_hint, NULL);
    g_hash_table_insert(self->contacts, entry->pubkey_hex, entry);
    g_mutex_unlock(&self->lock);

    g_debug("nip02_contacts: added contact %.8s", pubkey_hex);
    return TRUE;
}

gboolean gnostr_contact_list_remove(GnostrContactList *self,
                                     const char *pubkey_hex) {
    if (!self || !pubkey_hex) return FALSE;

    g_mutex_lock(&self->lock);
    gboolean removed = g_hash_table_remove(self->contacts, pubkey_hex);
    g_mutex_unlock(&self->lock);

    if (removed) {
        g_debug("nip02_contacts: removed contact %.8s", pubkey_hex);
    }
    return removed;
}

#ifndef GNOSTR_NIP02_CONTACTS_TEST_ONLY

static void contacts_publish_done(guint success_count, guint fail_count, gpointer user_data);

/* ---- Save (sign + publish) Implementation (nostrc-s0e0) ---- */

typedef struct {
    GnostrContactList *contact_list;
    GnostrContactListSaveCallback callback;
    gpointer user_data;
} ContactListSaveContext;

static void contact_list_save_context_free(ContactListSaveContext *ctx) {
    g_free(ctx);
}

/* nostrc-s0e0: Publish signed contact list to relays */
static void publish_contact_list_to_relays(ContactListSaveContext *ctx,
                                            const char *signed_event_json) {
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
    if (parse_rc != 1) {
        g_warning("nip02_contacts: failed to parse signed event");
        if (ctx->callback)
            ctx->callback(ctx->contact_list, FALSE, "Failed to parse signed event", ctx->user_data);
        nostr_event_free(event);
        contact_list_save_context_free(ctx);
        return;
    }

    /* Ingest into local NDB cache */
    storage_ndb_ingest_event_json(signed_event_json, NULL);

    /* Publish to relays asynchronously (hq-gflmf) */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(relay_urls);

    gnostr_publish_to_relays_async(event, relay_urls,
        contacts_publish_done, ctx);
    /* event + relay_urls ownership transferred; ctx freed in callback */
}

static void
contacts_publish_done(guint success_count, guint fail_count, gpointer user_data)
{
    ContactListSaveContext *ctx = (ContactListSaveContext *)user_data;

    if (success_count > 0) {
        g_mutex_lock(&ctx->contact_list->lock);
        ctx->contact_list->last_event_time = (gint64)time(NULL);
        g_mutex_unlock(&ctx->contact_list->lock);
    }

    if (ctx->callback) {
        if (success_count > 0) {
            ctx->callback(ctx->contact_list, TRUE, NULL, ctx->user_data);
        } else {
            ctx->callback(ctx->contact_list, FALSE,
                         "Failed to publish to any relay", ctx->user_data);
        }
    }

    g_message("nip02_contacts: published to %u relays, failed %u",
              success_count, fail_count);
    contact_list_save_context_free(ctx);
}

/* nostrc-s0e0: Sign callback */
static void on_contact_list_sign_complete(GObject *source, GAsyncResult *res,
                                           gpointer user_data) {
    ContactListSaveContext *ctx = (ContactListSaveContext *)user_data;
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        g_warning("nip02_contacts: signing failed: %s",
                  error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->contact_list, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        contact_list_save_context_free(ctx);
        return;
    }

    g_message("nip02_contacts: signed contact list event");
    publish_contact_list_to_relays(ctx, signed_event_json);
    g_free(signed_event_json);
}

void gnostr_contact_list_save_async(GnostrContactList *self,
                                     GnostrContactListSaveCallback callback,
                                     gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid contact list", user_data);
        return;
    }

    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (callback) callback(self, FALSE, "Signer not available", user_data);
        return;
    }

    g_mutex_lock(&self->lock);

    /* Build tags from current contacts */
    NostrTags *tags = nostr_tags_new(0);
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, self->contacts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnostrContactEntry *entry = (GnostrContactEntry *)value;
        NostrTag *tag;
        if (entry->relay_hint && *entry->relay_hint) {
            tag = nostr_tag_new("p", entry->pubkey_hex, entry->relay_hint, NULL);
        } else {
            tag = nostr_tag_new("p", entry->pubkey_hex, NULL);
        }
        nostr_tags_append(tags, tag);
    }

    g_mutex_unlock(&self->lock);

    /* Build unsigned kind 3 event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, CONTACT_LIST_KIND);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "");
    nostr_event_set_tags(event, tags);

    char *event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!event_json) {
        if (callback) callback(self, FALSE, "Failed to build event JSON", user_data);
        return;
    }

    ContactListSaveContext *ctx = g_new0(ContactListSaveContext, 1);
    ctx->contact_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_message("nip02_contacts: requesting signature for kind 3 event (%u contacts)",
              g_hash_table_size(self->contacts));

    gnostr_sign_event_async(event_json, "", "gnostr", NULL,
                             on_contact_list_sign_complete, ctx);
    g_free(event_json);
}

#else /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */

void gnostr_contact_list_save_async(GnostrContactList *self,
                                     GnostrContactListSaveCallback callback,
                                     gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid contact list", user_data);
        return;
    }
    if (callback) callback(self, TRUE, NULL, user_data);
}

#endif /* GNOSTR_NIP02_CONTACTS_TEST_ONLY */
