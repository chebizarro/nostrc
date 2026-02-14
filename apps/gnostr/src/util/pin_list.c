/**
 * NIP-51 Pin List Service Implementation
 *
 * Manages pin lists (kind 10001) for pinned notes on profile.
 * Uses the nip51 library for parsing and creating list events.
 * Modeled on the bookmark list service (kind 10003).
 */

/* nostrc-ch2v: NIP-51 pin list service */

#include "pin_list.h"
#include "relays.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include <glib.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <string.h>
#include <time.h>

#ifndef GNOSTR_PIN_LIST_TEST_ONLY
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include "utils.h"
#endif

/* Kind 10001 = Pin List per NIP-51 */
#define PIN_LIST_KIND 10001

/* ---- Internal Data Structure ---- */

typedef struct {
    char *event_id;
    char *relay_hint;
} PinEntry;

static void pin_entry_free(gpointer data) {
    PinEntry *e = (PinEntry *)data;
    if (e) {
        g_free(e->event_id);
        g_free(e->relay_hint);
        g_free(e);
    }
}

static PinEntry *pin_entry_new(const char *event_id,
                                const char *relay_hint) {
    PinEntry *e = g_new0(PinEntry, 1);
    e->event_id = g_strdup(event_id);
    e->relay_hint = g_strdup(relay_hint);
    return e;
}

struct _GnostrPinList {
    /* Hash table for O(1) lookup: key = event_id, value = PinEntry */
    GHashTable *pins;

    /* State */
    gboolean dirty;              /* Has unsaved changes */
    gint64 last_event_time;      /* created_at of last loaded event */
    char *user_pubkey;           /* Current user's pubkey (for fetching) */

    /* Async fetch */
    GCancellable *fetch_cancellable; /* cancels previous in-flight fetch (nostrc-m8l8) */

    /* Thread safety */
    GMutex lock;
};

/* Singleton instance */
static GnostrPinList *s_default_instance = NULL;
static GMutex s_init_lock;

/* ---- Internal Helpers ---- */

static void pin_list_clear(GnostrPinList *self) {
    g_hash_table_remove_all(self->pins);
    self->dirty = FALSE;
    self->last_event_time = 0;
}

/* ---- Public API Implementation ---- */

GnostrPinList *gnostr_pin_list_get_default(void) {
    g_mutex_lock(&s_init_lock);
    if (!s_default_instance) {
        s_default_instance = g_new0(GnostrPinList, 1);
        g_mutex_init(&s_default_instance->lock);
        s_default_instance->pins = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, pin_entry_free);
        s_default_instance->dirty = FALSE;
        s_default_instance->last_event_time = 0;
        s_default_instance->user_pubkey = NULL;
    }
    g_mutex_unlock(&s_init_lock);
    return s_default_instance;
}

void gnostr_pin_list_shutdown(void) {
    g_mutex_lock(&s_init_lock);
    if (s_default_instance) {
        g_mutex_lock(&s_default_instance->lock);
        g_hash_table_destroy(s_default_instance->pins);
        g_free(s_default_instance->user_pubkey);
        g_mutex_unlock(&s_default_instance->lock);
        g_mutex_clear(&s_default_instance->lock);
        g_free(s_default_instance);
        s_default_instance = NULL;
    }
    g_mutex_unlock(&s_init_lock);
}

gboolean gnostr_pin_list_load_from_json(GnostrPinList *self,
                                         const char *event_json) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        g_warning("pin_list: failed to parse event JSON");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    if (nostr_event_get_kind(event) != PIN_LIST_KIND) {
        g_warning("pin_list: not a kind 10001 event");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    gint64 event_time = nostr_event_get_created_at(event);
    if (event_time <= self->last_event_time) {
        g_debug("pin_list: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return TRUE;
    }

    /* Clear existing data and load new */
    pin_list_clear(self);
    self->last_event_time = event_time;

    /* Parse tags - pin list uses "e" tags */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get(tag, 0);
            const char *value = nostr_tag_get(tag, 1);
            if (!tag_name || !value) continue;

            if (strcmp(tag_name, "e") == 0) {
                const char *relay_hint = NULL;
                if (nostr_tag_size(tag) >= 3) {
                    relay_hint = nostr_tag_get(tag, 2);
                }
                PinEntry *entry = pin_entry_new(value, relay_hint);
                g_hash_table_insert(self->pins, entry->event_id, entry);
                g_debug("pin_list: loaded event %s", value);
            }
        }
    }

    nostr_event_free(event);
    g_mutex_unlock(&self->lock);

    g_message("pin_list: loaded %u pinned notes",
              g_hash_table_size(self->pins));

    return TRUE;
}

/* ---- Async Fetch Implementation ---- */

typedef struct {
    GnostrPinList *pin_list;
    GnostrPinListFetchCallback callback;
    gpointer user_data;
    char *pubkey_hex;
    GnostrPinListMergeStrategy strategy;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

#ifndef GNOSTR_PIN_LIST_TEST_ONLY

/* Internal helper: load pins from event tags into hash table */
static void load_pins_from_event_unlocked(GnostrPinList *self, NostrEvent *event) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (!tags) return;

    size_t tag_count = nostr_tags_size(tags);
    for (size_t idx = 0; idx < tag_count; idx++) {
        NostrTag *tag = nostr_tags_get(tags, idx);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *tag_name = nostr_tag_get(tag, 0);
        const char *value = nostr_tag_get(tag, 1);
        if (!tag_name || !value) continue;

        if (strcmp(tag_name, "e") == 0) {
            const char *relay_hint = NULL;
            if (nostr_tag_size(tag) >= 3) {
                relay_hint = nostr_tag_get(tag, 2);
            }
            if (!g_hash_table_contains(self->pins, value)) {
                PinEntry *entry = pin_entry_new(value, relay_hint);
                g_hash_table_insert(self->pins, entry->event_id, entry);
            }
        }
    }
}

/* Internal: merge remote pins with strategy */
static void pin_list_merge_from_json_with_strategy_unlocked(GnostrPinList *self,
                                                             const char *event_json,
                                                             GnostrPinListMergeStrategy strategy,
                                                             gint64 *out_created_at) {
    if (!self || !event_json) return;

    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        nostr_event_free(event);
        return;
    }

    if (nostr_event_get_kind(event) != PIN_LIST_KIND) {
        nostr_event_free(event);
        return;
    }

    gint64 event_time = nostr_event_get_created_at(event);
    if (out_created_at) *out_created_at = event_time;

    switch (strategy) {
    case GNOSTR_PIN_LIST_MERGE_LOCAL_WINS:
        if (event_time > self->last_event_time) {
            self->last_event_time = event_time;
        }
        g_debug("pin_list: LOCAL_WINS - keeping local data");
        break;

    case GNOSTR_PIN_LIST_MERGE_REMOTE_WINS:
        g_hash_table_remove_all(self->pins);
        self->last_event_time = event_time;
        load_pins_from_event_unlocked(self, event);
        self->dirty = FALSE;
        g_message("pin_list: REMOTE_WINS - replaced with %u remote pins",
                  g_hash_table_size(self->pins));
        break;

    case GNOSTR_PIN_LIST_MERGE_UNION:
        load_pins_from_event_unlocked(self, event);
        if (event_time > self->last_event_time) {
            self->last_event_time = event_time;
        }
        self->dirty = TRUE;
        g_message("pin_list: UNION - now have %u pins",
                  g_hash_table_size(self->pins));
        break;

    case GNOSTR_PIN_LIST_MERGE_LATEST:
    default:
        if (event_time > self->last_event_time) {
            g_hash_table_remove_all(self->pins);
            self->last_event_time = event_time;
            load_pins_from_event_unlocked(self, event);
            self->dirty = FALSE;
            g_message("pin_list: LATEST - loaded %u pins (remote newer)",
                      g_hash_table_size(self->pins));
        } else {
            g_debug("pin_list: LATEST - keeping local (local newer or same)");
        }
        break;
    }

    nostr_event_free(event);
}

/* Callback when relay query completes */
static void on_pin_list_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchContext *ctx = (FetchContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("pin_list: query failed: %s", err->message);
        }
        g_error_free(err);
        if (ctx->callback) ctx->callback(ctx->pin_list, FALSE, ctx->user_data);
        fetch_context_free(ctx);
        return;
    }

    gboolean success = FALSE;
    gint64 newest_created_at = 0;
    const char *newest_event_json = NULL;

    /* Find the newest pin list event */
    if (results && results->len > 0) {
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);

            NostrEvent *event = nostr_event_new();
            int parse_rc = nostr_event_deserialize_compact(event, json, NULL);
            if (parse_rc != 1) {
                nostr_event_free(event);
                continue;
            }

            if (nostr_event_get_kind(event) != PIN_LIST_KIND) {
                nostr_event_free(event);
                continue;
            }

            gint64 event_time = nostr_event_get_created_at(event);
            if (event_time > newest_created_at) {
                newest_created_at = event_time;
                newest_event_json = json;
            }
            nostr_event_free(event);
        }

        if (newest_event_json) {
            g_mutex_lock(&ctx->pin_list->lock);
            pin_list_merge_from_json_with_strategy_unlocked(ctx->pin_list,
                newest_event_json, ctx->strategy, NULL);
            g_mutex_unlock(&ctx->pin_list->lock);
            success = TRUE;
        }
    }

    if (results) g_ptr_array_unref(results);

    g_message("pin_list: fetch completed, success=%d, count=%zu",
              success, gnostr_pin_list_get_count(ctx->pin_list));

    if (ctx->callback) {
        ctx->callback(ctx->pin_list, TRUE, ctx->user_data);
    }

    fetch_context_free(ctx);
}

/* Singleton pool for pin list queries */
static GNostrPool *s_pin_list_pool = NULL;

void gnostr_pin_list_fetch_with_strategy_async(GnostrPinList *self,
                                                const char *pubkey_hex,
                                                const char * const *relays,
                                                GnostrPinListMergeStrategy strategy,
                                                GnostrPinListFetchCallback callback,
                                                gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    if (strategy == GNOSTR_PIN_LIST_MERGE_LOCAL_WINS) {
        g_message("pin_list: LOCAL_WINS strategy - skipping remote fetch");
        if (callback) callback(self, TRUE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);

    /* Cancel any in-flight fetch to prevent concurrent overlap (nostrc-m8l8) */
    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }
    self->fetch_cancellable = g_cancellable_new();
    g_mutex_unlock(&self->lock);

    FetchContext *ctx = g_new0(FetchContext, 1);
    ctx->pin_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pubkey_hex = g_strdup(pubkey_hex);
    ctx->strategy = strategy;

    /* Build filter for kind 10001 */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { PIN_LIST_KIND };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 5);

    /* Get relay URLs */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
    if (relays && relays[0]) {
        for (size_t i = 0; relays[i] != NULL; i++) {
            g_ptr_array_add(relay_arr, g_strdup(relays[i]));
        }
    } else {
        gnostr_load_relays_into(relay_arr);
    }

    if (relay_arr->len == 0) {
        g_warning("pin_list: no relays configured for fetch");
        if (callback) callback(self, FALSE, user_data);
        nostr_filter_free(filter);
        g_ptr_array_unref(relay_arr);
        fetch_context_free(ctx);
        return;
    }

    const char **urls = g_new0(const char*, relay_arr->len + 1);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    if (!s_pin_list_pool) s_pin_list_pool = gnostr_pool_new();

    g_message("pin_list: fetching kind %d from %u relays for pubkey %.8s...",
              PIN_LIST_KIND, relay_arr->len, pubkey_hex);

    gnostr_pool_sync_relays(s_pin_list_pool, (const gchar **)urls, relay_arr->len);
    {
        /* nostrc-m8l8: Use unique key per query to avoid freeing filters still
         * in use by a concurrent query (use-after-free on overlapping fetches).
         * Same pattern as follow_list.c and profile_pane.c. */
        static gint _qf_counter_pl = 0;
        int _qfid = g_atomic_int_add(&_qf_counter_pl, 1);
        char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-pl-%d", _qfid);
        NostrFilters *_qf = nostr_filters_new();
        nostr_filters_add(_qf, filter);
        g_object_set_data_full(G_OBJECT(s_pin_list_pool), _qfk, _qf,
                               (GDestroyNotify)nostr_filters_free);
        gnostr_pool_query_async(s_pin_list_pool, _qf, self->fetch_cancellable,
                                on_pin_list_query_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

void gnostr_pin_list_fetch_async(GnostrPinList *self,
                                  const char *pubkey_hex,
                                  const char * const *relays,
                                  GnostrPinListFetchCallback callback,
                                  gpointer user_data) {
    gnostr_pin_list_fetch_with_strategy_async(self, pubkey_hex, relays,
                                               GNOSTR_PIN_LIST_MERGE_LATEST,
                                               callback, user_data);
}

/* ---- Save Implementation ---- */

typedef struct {
    GnostrPinList *pin_list;
    GnostrPinListSaveCallback callback;
    gpointer user_data;
    char *event_json;
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    if (ctx) {
        g_free(ctx->event_json);
        g_free(ctx);
    }
}

static void pin_list_publish_done(guint success_count, guint fail_count, gpointer user_data);

static void on_pin_list_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        g_warning("pin_list: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->pin_list, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        save_context_free(ctx);
        return;
    }

    g_message("pin_list: signed event successfully");

    /* Parse the signed event */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
    if (parse_rc != 1) {
        g_warning("pin_list: failed to parse signed event");
        if (ctx->callback) {
            ctx->callback(ctx->pin_list, FALSE, "Failed to parse signed event", ctx->user_data);
        }
        nostr_event_free(event);
        g_free(signed_event_json);
        save_context_free(ctx);
        return;
    }

    /* Publish to relays asynchronously (hq-gflmf) */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(relay_urls);

    g_free(signed_event_json);
    gnostr_publish_to_relays_async(event, relay_urls,
        pin_list_publish_done, ctx);
    /* event + relay_urls ownership transferred; ctx freed in callback */
}

static void
pin_list_publish_done(guint success_count, guint fail_count, gpointer user_data)
{
    SaveContext *ctx = (SaveContext *)user_data;

    if (success_count > 0) {
        g_mutex_lock(&ctx->pin_list->lock);
        ctx->pin_list->dirty = FALSE;
        ctx->pin_list->last_event_time = (gint64)time(NULL);
        g_mutex_unlock(&ctx->pin_list->lock);
    }

    if (ctx->callback) {
        if (success_count > 0) {
            ctx->callback(ctx->pin_list, TRUE, NULL, ctx->user_data);
        } else {
            ctx->callback(ctx->pin_list, FALSE, "Failed to publish to any relay", ctx->user_data);
        }
    }

    g_message("pin_list: published to %u relays, failed %u", success_count, fail_count);
    save_context_free(ctx);
}

#else /* GNOSTR_PIN_LIST_TEST_ONLY */

void gnostr_pin_list_fetch_with_strategy_async(GnostrPinList *self,
                                                const char *pubkey_hex,
                                                const char * const *relays,
                                                GnostrPinListMergeStrategy strategy,
                                                GnostrPinListFetchCallback callback,
                                                gpointer user_data) {
    (void)relays;
    (void)strategy;
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }
    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);
    g_message("pin_list: fetch with strategy requested (test stub)");
    if (callback) callback(self, TRUE, user_data);
}

void gnostr_pin_list_fetch_async(GnostrPinList *self,
                                  const char *pubkey_hex,
                                  const char * const *relays,
                                  GnostrPinListFetchCallback callback,
                                  gpointer user_data) {
    gnostr_pin_list_fetch_with_strategy_async(self, pubkey_hex, relays,
                                               GNOSTR_PIN_LIST_MERGE_LATEST,
                                               callback, user_data);
}

static void on_pin_list_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        g_warning("pin_list: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->pin_list, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        save_context_free(ctx);
        return;
    }

    g_message("pin_list: signed event (test stub - no relay publish)");
    g_mutex_lock(&ctx->pin_list->lock);
    ctx->pin_list->dirty = FALSE;
    ctx->pin_list->last_event_time = (gint64)time(NULL);
    g_mutex_unlock(&ctx->pin_list->lock);

    if (ctx->callback) {
        ctx->callback(ctx->pin_list, TRUE, NULL, ctx->user_data);
    }

    g_free(signed_event_json);
    save_context_free(ctx);
}

#endif /* GNOSTR_PIN_LIST_TEST_ONLY */

void gnostr_pin_list_save_async(GnostrPinList *self,
                                 GnostrPinListSaveCallback callback,
                                 gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid pin list", user_data);
        return;
    }

    /* Check if signer service is available */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (callback) callback(self, FALSE, "Signer not available", user_data);
        return;
    }

    SaveContext *ctx = g_new0(SaveContext, 1);
    ctx->pin_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_mutex_lock(&self->lock);

    /* Build tags array - pin list is all public "e" tags */
    NostrTags *tags = nostr_tags_new(0);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->pins);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PinEntry *entry = (PinEntry *)value;
        NostrTag *tag;
        if (entry->relay_hint && *entry->relay_hint) {
            tag = nostr_tag_new("e", entry->event_id, entry->relay_hint, NULL);
        } else {
            tag = nostr_tag_new("e", entry->event_id, NULL);
        }
        nostr_tags_append(tags, tag);
    }

    g_mutex_unlock(&self->lock);

    /* Build unsigned event */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, PIN_LIST_KIND);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "");
    nostr_event_set_tags(event, tags);

    char *event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!event_json) {
        if (callback) callback(self, FALSE, "Failed to build event JSON", user_data);
        save_context_free(ctx);
        return;
    }

    g_message("pin_list: requesting signature for event");
    ctx->event_json = event_json;

    gnostr_sign_event_async(
        event_json,
        "",
        "gnostr",
        NULL,
        on_pin_list_sign_complete,
        ctx
    );
}

/* ---- Query Functions ---- */

gboolean gnostr_pin_list_is_pinned(GnostrPinList *self,
                                    const char *event_id_hex) {
    if (!self || !event_id_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = g_hash_table_contains(self->pins, event_id_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

/* ---- Modification Functions ---- */

void gnostr_pin_list_add(GnostrPinList *self,
                          const char *event_id_hex,
                          const char *relay_hint) {
    if (!self || !event_id_hex || strlen(event_id_hex) != 64) return;

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->pins, event_id_hex)) {
        PinEntry *entry = pin_entry_new(event_id_hex, relay_hint);
        g_hash_table_insert(self->pins, entry->event_id, entry);
        self->dirty = TRUE;
        g_message("pin_list: added event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_pin_list_remove(GnostrPinList *self,
                             const char *event_id_hex) {
    if (!self || !event_id_hex) return;

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->pins, event_id_hex)) {
        self->dirty = TRUE;
        g_message("pin_list: removed event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);
}

gboolean gnostr_pin_list_toggle(GnostrPinList *self,
                                 const char *event_id_hex,
                                 const char *relay_hint) {
    if (!self || !event_id_hex) return FALSE;

    g_mutex_lock(&self->lock);
    gboolean now_pinned;
    if (g_hash_table_contains(self->pins, event_id_hex)) {
        g_hash_table_remove(self->pins, event_id_hex);
        self->dirty = TRUE;
        now_pinned = FALSE;
        g_message("pin_list: toggled OFF event %s", event_id_hex);
    } else {
        PinEntry *entry = pin_entry_new(event_id_hex, relay_hint);
        g_hash_table_insert(self->pins, entry->event_id, entry);
        self->dirty = TRUE;
        now_pinned = TRUE;
        g_message("pin_list: toggled ON event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);

    return now_pinned;
}

/* ---- Accessors ---- */

const char **gnostr_pin_list_get_event_ids(GnostrPinList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->pins);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->pins);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

gboolean gnostr_pin_list_is_dirty(GnostrPinList *self) {
    if (!self) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = self->dirty;
    g_mutex_unlock(&self->lock);
    return result;
}

size_t gnostr_pin_list_get_count(GnostrPinList *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    size_t result = g_hash_table_size(self->pins);
    g_mutex_unlock(&self->lock);
    return result;
}

gint64 gnostr_pin_list_get_last_sync_time(GnostrPinList *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    gint64 result = self->last_event_time;
    g_mutex_unlock(&self->lock);
    return result;
}

void gnostr_pin_list_sync_on_login(const char *pubkey_hex) {
    if (!pubkey_hex || !*pubkey_hex) return;

    GnostrPinList *pin_list = gnostr_pin_list_get_default();
    if (!pin_list) return;

    g_message("pin_list: auto-syncing for user %.8s...", pubkey_hex);
    gnostr_pin_list_fetch_async(pin_list, pubkey_hex, NULL, NULL, NULL);
}
