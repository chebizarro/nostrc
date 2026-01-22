/**
 * NIP-51 Bookmark List Service Implementation
 *
 * Manages bookmark lists (kind 10003) for saving notes.
 * Uses the nip51 library for parsing and creating list events.
 * Implements relay fetch and publish per NIP-51.
 */

#include "bookmarks.h"
#include "relays.h"
#include "../ipc/signer_ipc.h"
#include <glib.h>
#include <jansson.h>
#include <string.h>
#include <time.h>

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY
#include "nostr_simple_pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#endif

/* Kind 10003 = Bookmark List per NIP-51 */
#define BOOKMARK_LIST_KIND 10003

/* ---- Internal Data Structure ---- */

typedef struct {
    char *event_id;
    char *relay_hint;
    gboolean is_private;
} BookmarkEntry;

static void bookmark_entry_free(gpointer data) {
    BookmarkEntry *e = (BookmarkEntry *)data;
    if (e) {
        g_free(e->event_id);
        g_free(e->relay_hint);
        g_free(e);
    }
}

static BookmarkEntry *bookmark_entry_new(const char *event_id,
                                          const char *relay_hint,
                                          gboolean is_private) {
    BookmarkEntry *e = g_new0(BookmarkEntry, 1);
    e->event_id = g_strdup(event_id);
    e->relay_hint = g_strdup(relay_hint);
    e->is_private = is_private;
    return e;
}

struct _GnostrBookmarks {
    /* Hash table for O(1) lookup: key = event_id, value = BookmarkEntry */
    GHashTable *bookmarks;

    /* State */
    gboolean dirty;              /* Has unsaved changes */
    gint64 last_event_time;      /* created_at of last loaded event */
    char *user_pubkey;           /* Current user's pubkey (for fetching) */

    /* Thread safety */
    GMutex lock;
};

/* Singleton instance */
static GnostrBookmarks *s_default_instance = NULL;
static GMutex s_init_lock;

/* ---- Internal Helpers ---- */

static void bookmarks_clear(GnostrBookmarks *self) {
    g_hash_table_remove_all(self->bookmarks);
    self->dirty = FALSE;
    self->last_event_time = 0;
}

/* ---- Public API Implementation ---- */

GnostrBookmarks *gnostr_bookmarks_get_default(void) {
    g_mutex_lock(&s_init_lock);
    if (!s_default_instance) {
        s_default_instance = g_new0(GnostrBookmarks, 1);
        g_mutex_init(&s_default_instance->lock);
        s_default_instance->bookmarks = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, bookmark_entry_free);
        s_default_instance->dirty = FALSE;
        s_default_instance->last_event_time = 0;
        s_default_instance->user_pubkey = NULL;
    }
    g_mutex_unlock(&s_init_lock);
    return s_default_instance;
}

void gnostr_bookmarks_shutdown(void) {
    g_mutex_lock(&s_init_lock);
    if (s_default_instance) {
        g_mutex_lock(&s_default_instance->lock);
        g_hash_table_destroy(s_default_instance->bookmarks);
        g_free(s_default_instance->user_pubkey);
        g_mutex_unlock(&s_default_instance->lock);
        g_mutex_clear(&s_default_instance->lock);
        g_free(s_default_instance);
        s_default_instance = NULL;
    }
    g_mutex_unlock(&s_init_lock);
}

gboolean gnostr_bookmarks_load_from_json(GnostrBookmarks *self,
                                          const char *event_json) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    json_error_t error;
    json_t *root = json_loads(event_json, 0, &error);
    if (!root) {
        g_warning("bookmarks: failed to parse event JSON: %s", error.text);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    json_t *kind_val = json_object_get(root, "kind");
    if (!kind_val || json_integer_value(kind_val) != BOOKMARK_LIST_KIND) {
        g_warning("bookmarks: not a kind 10003 event");
        json_decref(root);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    json_t *created_at = json_object_get(root, "created_at");
    gint64 event_time = created_at ? json_integer_value(created_at) : 0;
    if (event_time <= self->last_event_time) {
        g_debug("bookmarks: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        json_decref(root);
        g_mutex_unlock(&self->lock);
        return TRUE; /* Not an error, just older data */
    }

    /* Clear existing data and load new */
    bookmarks_clear(self);
    self->last_event_time = event_time;

    /* Parse tags */
    json_t *tags = json_object_get(root, "tags");
    if (json_is_array(tags)) {
        size_t idx;
        json_t *tag;
        json_array_foreach(tags, idx, tag) {
            if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

            const char *tag_name = json_string_value(json_array_get(tag, 0));
            const char *value = json_string_value(json_array_get(tag, 1));
            if (!tag_name || !value) continue;

            /* NIP-51 bookmark list uses "e" tags for event bookmarks */
            if (strcmp(tag_name, "e") == 0) {
                /* Optional relay hint in third position */
                const char *relay_hint = NULL;
                if (json_array_size(tag) >= 3) {
                    relay_hint = json_string_value(json_array_get(tag, 2));
                }
                BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                g_hash_table_insert(self->bookmarks, entry->event_id, entry);
                g_debug("bookmarks: loaded event %s", value);
            }
            /* Also support "a" tags for addressable events (articles, etc.) */
            else if (strcmp(tag_name, "a") == 0) {
                const char *relay_hint = NULL;
                if (json_array_size(tag) >= 3) {
                    relay_hint = json_string_value(json_array_get(tag, 2));
                }
                BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                g_hash_table_insert(self->bookmarks, entry->event_id, entry);
                g_debug("bookmarks: loaded addressable %s", value);
            }
        }
    }

    /* TODO: Parse encrypted content for private entries (requires NIP-44) */
    /* For now, we only handle public entries in tags */

    json_decref(root);
    g_mutex_unlock(&self->lock);

    g_message("bookmarks: loaded %u bookmarks",
              g_hash_table_size(self->bookmarks));

    return TRUE;
}

/* ---- Async Fetch Implementation ---- */

typedef struct {
    GnostrBookmarks *bookmarks;
    GnostrBookmarksFetchCallback callback;
    gpointer user_data;
    char *pubkey_hex;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY

/* Internal: merge remote bookmarks into local, preferring most recent */
static void bookmarks_merge_from_json_unlocked(GnostrBookmarks *self,
                                                const char *event_json,
                                                gint64 *out_created_at) {
    if (!self || !event_json) return;

    json_error_t error;
    json_t *root = json_loads(event_json, 0, &error);
    if (!root) return;

    /* Verify kind */
    json_t *kind_val = json_object_get(root, "kind");
    if (!kind_val || json_integer_value(kind_val) != BOOKMARK_LIST_KIND) {
        json_decref(root);
        return;
    }

    /* Extract created_at */
    json_t *created_at = json_object_get(root, "created_at");
    gint64 event_time = created_at ? json_integer_value(created_at) : 0;
    if (out_created_at) *out_created_at = event_time;

    /* Parse tags and merge - remote event replaces local if newer */
    if (event_time > self->last_event_time) {
        /* Clear existing and load from remote (remote is newer) */
        g_hash_table_remove_all(self->bookmarks);
        self->last_event_time = event_time;

        json_t *tags = json_object_get(root, "tags");
        if (json_is_array(tags)) {
            size_t idx;
            json_t *tag;
            json_array_foreach(tags, idx, tag) {
                if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

                const char *tag_name = json_string_value(json_array_get(tag, 0));
                const char *value = json_string_value(json_array_get(tag, 1));
                if (!tag_name || !value) continue;

                if (strcmp(tag_name, "e") == 0 || strcmp(tag_name, "a") == 0) {
                    const char *relay_hint = NULL;
                    if (json_array_size(tag) >= 3) {
                        relay_hint = json_string_value(json_array_get(tag, 2));
                    }
                    BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                    g_hash_table_insert(self->bookmarks, entry->event_id, entry);
                }
            }
        }
        self->dirty = FALSE; /* Synced with remote */
        g_message("bookmarks: merged %u bookmarks from remote (remote newer)",
                  g_hash_table_size(self->bookmarks));
    } else {
        /* Local is newer or same, keep local but mark as needing publish */
        g_debug("bookmarks: local data is newer, keeping local");
    }

    json_decref(root);
}

/* Callback when relay query completes */
static void on_bookmarks_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchContext *ctx = (FetchContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("bookmarks: query failed: %s", err->message);
        }
        g_error_free(err);
        if (ctx->callback) ctx->callback(ctx->bookmarks, FALSE, ctx->user_data);
        fetch_context_free(ctx);
        return;
    }

    gboolean success = FALSE;
    gint64 newest_created_at = 0;

    /* Find and load the newest bookmark list event */
    if (results && results->len > 0) {
        g_mutex_lock(&ctx->bookmarks->lock);

        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);
            gint64 created_at = 0;
            bookmarks_merge_from_json_unlocked(ctx->bookmarks, json, &created_at);
            if (created_at > newest_created_at) {
                newest_created_at = created_at;
                success = TRUE;
            }
        }

        g_mutex_unlock(&ctx->bookmarks->lock);
    }

    if (results) g_ptr_array_unref(results);

    g_message("bookmarks: fetch completed, success=%d, count=%zu",
              success, gnostr_bookmarks_get_count(ctx->bookmarks));

    if (ctx->callback) {
        ctx->callback(ctx->bookmarks, TRUE, ctx->user_data);
    }

    fetch_context_free(ctx);
}

/* Singleton pool for bookmark queries */
static GnostrSimplePool *s_bookmarks_pool = NULL;

void gnostr_bookmarks_fetch_async(GnostrBookmarks *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrBookmarksFetchCallback callback,
                                   gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);

    /* Create fetch context */
    FetchContext *ctx = g_new0(FetchContext, 1);
    ctx->bookmarks = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pubkey_hex = g_strdup(pubkey_hex);

    /* Build filter for kind 10003 */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { BOOKMARK_LIST_KIND };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 5); /* Get a few in case of duplicates */

    /* Get relay URLs - use provided relays or fall back to configured */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
    if (relays && relays[0]) {
        for (size_t i = 0; relays[i] != NULL; i++) {
            g_ptr_array_add(relay_arr, g_strdup(relays[i]));
        }
    } else {
        gnostr_load_relays_into(relay_arr);
    }

    if (relay_arr->len == 0) {
        g_warning("bookmarks: no relays configured for fetch");
        if (callback) callback(self, FALSE, user_data);
        nostr_filter_free(filter);
        g_ptr_array_unref(relay_arr);
        fetch_context_free(ctx);
        return;
    }

    /* Build URL array */
    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    /* Use static pool */
    if (!s_bookmarks_pool) s_bookmarks_pool = gnostr_simple_pool_new();

    g_message("bookmarks: fetching kind %d from %u relays for pubkey %.8s...",
              BOOKMARK_LIST_KIND, relay_arr->len, pubkey_hex);

    gnostr_simple_pool_query_single_async(
        s_bookmarks_pool,
        urls,
        relay_arr->len,
        filter,
        NULL, /* cancellable */
        on_bookmarks_query_done,
        ctx
    );

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

#else /* GNOSTR_BOOKMARKS_TEST_ONLY */

void gnostr_bookmarks_fetch_async(GnostrBookmarks *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrBookmarksFetchCallback callback,
                                   gpointer user_data) {
    (void)relays;
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }
    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);
    g_message("bookmarks: fetch requested (test stub)");
    if (callback) callback(self, TRUE, user_data);
}

#endif /* GNOSTR_BOOKMARKS_TEST_ONLY */

/* ---- Query Functions ---- */

gboolean gnostr_bookmarks_is_bookmarked(GnostrBookmarks *self,
                                         const char *event_id_hex) {
    if (!self || !event_id_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = g_hash_table_contains(self->bookmarks, event_id_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

/* ---- Modification Functions ---- */

void gnostr_bookmarks_add(GnostrBookmarks *self,
                           const char *event_id_hex,
                           const char *relay_hint,
                           gboolean is_private) {
    if (!self || !event_id_hex || strlen(event_id_hex) != 64) return;

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->bookmarks, event_id_hex)) {
        BookmarkEntry *entry = bookmark_entry_new(event_id_hex, relay_hint, is_private);
        g_hash_table_insert(self->bookmarks, entry->event_id, entry);
        self->dirty = TRUE;
        g_message("bookmarks: added event %s (private=%d)", event_id_hex, is_private);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_bookmarks_remove(GnostrBookmarks *self,
                              const char *event_id_hex) {
    if (!self || !event_id_hex) return;

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->bookmarks, event_id_hex)) {
        self->dirty = TRUE;
        g_message("bookmarks: removed event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);
}

gboolean gnostr_bookmarks_toggle(GnostrBookmarks *self,
                                  const char *event_id_hex,
                                  const char *relay_hint) {
    if (!self || !event_id_hex) return FALSE;

    g_mutex_lock(&self->lock);
    gboolean now_bookmarked;
    if (g_hash_table_contains(self->bookmarks, event_id_hex)) {
        /* Remove bookmark */
        g_hash_table_remove(self->bookmarks, event_id_hex);
        self->dirty = TRUE;
        now_bookmarked = FALSE;
        g_message("bookmarks: toggled OFF event %s", event_id_hex);
    } else {
        /* Add bookmark (public by default) */
        BookmarkEntry *entry = bookmark_entry_new(event_id_hex, relay_hint, FALSE);
        g_hash_table_insert(self->bookmarks, entry->event_id, entry);
        self->dirty = TRUE;
        now_bookmarked = TRUE;
        g_message("bookmarks: toggled ON event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);

    return now_bookmarked;
}

/* ---- Save Implementation ---- */

typedef struct {
    GnostrBookmarks *bookmarks;
    GnostrBookmarksSaveCallback callback;
    gpointer user_data;
    char *event_json;
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    if (ctx) {
        g_free(ctx->event_json);
        g_free(ctx);
    }
}

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY

static void on_bookmarks_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

    if (!ok || !signed_event_json) {
        g_warning("bookmarks: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->bookmarks, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        save_context_free(ctx);
        return;
    }

    g_message("bookmarks: signed event successfully");

    /* Parse the signed event JSON into a NostrEvent */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json);
    if (parse_rc != 1) {
        g_warning("bookmarks: failed to parse signed event");
        if (ctx->callback) {
            ctx->callback(ctx->bookmarks, FALSE, "Failed to parse signed event", ctx->user_data);
        }
        nostr_event_free(event);
        g_free(signed_event_json);
        save_context_free(ctx);
        return;
    }

    /* Get relay URLs for publishing */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_get_write_relay_urls_into(relay_urls);

    /* Fall back to all relays if no write relays configured */
    if (relay_urls->len == 0) {
        gnostr_load_relays_into(relay_urls);
    }

    /* Publish to each relay */
    guint success_count = 0;
    guint fail_count = 0;

    for (guint i = 0; i < relay_urls->len; i++) {
        const char *url = (const char *)g_ptr_array_index(relay_urls, i);
        GNostrRelay *relay = gnostr_relay_new(url);
        if (!relay) {
            fail_count++;
            continue;
        }

        GError *conn_err = NULL;
        if (!gnostr_relay_connect(relay, &conn_err)) {
            g_debug("bookmarks: failed to connect to %s: %s",
                    url, conn_err ? conn_err->message : "unknown");
            g_clear_error(&conn_err);
            g_object_unref(relay);
            fail_count++;
            continue;
        }

        GError *pub_err = NULL;
        if (gnostr_relay_publish(relay, event, &pub_err)) {
            g_message("bookmarks: published to %s", url);
            success_count++;
        } else {
            g_debug("bookmarks: publish failed to %s: %s",
                    url, pub_err ? pub_err->message : "unknown");
            g_clear_error(&pub_err);
            fail_count++;
        }
        g_object_unref(relay);
    }

    /* Update state on success */
    if (success_count > 0) {
        g_mutex_lock(&ctx->bookmarks->lock);
        ctx->bookmarks->dirty = FALSE;
        ctx->bookmarks->last_event_time = (gint64)time(NULL);
        g_mutex_unlock(&ctx->bookmarks->lock);
    }

    /* Cleanup */
    nostr_event_free(event);
    g_free(signed_event_json);
    g_ptr_array_free(relay_urls, TRUE);

    /* Notify callback */
    if (ctx->callback) {
        if (success_count > 0) {
            ctx->callback(ctx->bookmarks, TRUE, NULL, ctx->user_data);
        } else {
            ctx->callback(ctx->bookmarks, FALSE, "Failed to publish to any relay", ctx->user_data);
        }
    }

    g_message("bookmarks: published to %u relays, failed %u", success_count, fail_count);
    save_context_free(ctx);
}

#else /* GNOSTR_BOOKMARKS_TEST_ONLY */

static void on_bookmarks_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

    if (!ok || !signed_event_json) {
        g_warning("bookmarks: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->bookmarks, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        save_context_free(ctx);
        return;
    }

    g_message("bookmarks: signed event (test stub - no relay publish)");
    g_mutex_lock(&ctx->bookmarks->lock);
    ctx->bookmarks->dirty = FALSE;
    ctx->bookmarks->last_event_time = (gint64)time(NULL);
    g_mutex_unlock(&ctx->bookmarks->lock);

    if (ctx->callback) {
        ctx->callback(ctx->bookmarks, TRUE, NULL, ctx->user_data);
    }

    g_free(signed_event_json);
    save_context_free(ctx);
}

#endif /* GNOSTR_BOOKMARKS_TEST_ONLY */

void gnostr_bookmarks_save_async(GnostrBookmarks *self,
                                  GnostrBookmarksSaveCallback callback,
                                  gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid bookmark list", user_data);
        return;
    }

    /* Get signer proxy */
    GError *proxy_err = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
    if (!proxy) {
        char *msg = g_strdup_printf("Signer not available: %s",
                                     proxy_err ? proxy_err->message : "not connected");
        if (callback) callback(self, FALSE, msg, user_data);
        g_free(msg);
        g_clear_error(&proxy_err);
        return;
    }

    g_mutex_lock(&self->lock);

    /* Build the tags array */
    json_t *tags = json_array();

    /* Add bookmarked events as "e" tags */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->bookmarks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        BookmarkEntry *entry = (BookmarkEntry *)value;
        if (!entry->is_private) {
            json_t *tag = json_array();
            json_array_append_new(tag, json_string("e"));
            json_array_append_new(tag, json_string(entry->event_id));
            /* Add relay hint if available */
            if (entry->relay_hint && *entry->relay_hint) {
                json_array_append_new(tag, json_string(entry->relay_hint));
            }
            json_array_append(tags, tag);
            json_decref(tag);
        }
    }

    g_mutex_unlock(&self->lock);

    /* Build unsigned event */
    json_t *event_obj = json_object();
    json_object_set_new(event_obj, "kind", json_integer(BOOKMARK_LIST_KIND));
    json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
    json_object_set_new(event_obj, "content", json_string("")); /* TODO: Add encrypted private entries */
    json_object_set_new(event_obj, "tags", tags);

    char *event_json = json_dumps(event_obj, JSON_COMPACT);
    json_decref(event_obj);

    if (!event_json) {
        if (callback) callback(self, FALSE, "Failed to build event JSON", user_data);
        return;
    }

    g_message("bookmarks: requesting signature for: %s", event_json);

    /* Create save context */
    SaveContext *ctx = g_new0(SaveContext, 1);
    ctx->bookmarks = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->event_json = event_json;

    /* Call signer asynchronously */
    nostr_org_nostr_signer_call_sign_event(
        proxy,
        event_json,
        "",        /* current_user: empty = use default */
        "gnostr",  /* app_id */
        NULL,      /* cancellable */
        on_bookmarks_sign_complete,
        ctx
    );
}

/* ---- Accessors ---- */

const char **gnostr_bookmarks_get_event_ids(GnostrBookmarks *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->bookmarks);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->bookmarks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

gboolean gnostr_bookmarks_is_dirty(GnostrBookmarks *self) {
    if (!self) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = self->dirty;
    g_mutex_unlock(&self->lock);
    return result;
}

size_t gnostr_bookmarks_get_count(GnostrBookmarks *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    size_t result = g_hash_table_size(self->bookmarks);
    g_mutex_unlock(&self->lock);
    return result;
}

gint64 gnostr_bookmarks_get_last_sync_time(GnostrBookmarks *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    gint64 result = self->last_event_time;
    g_mutex_unlock(&self->lock);
    return result;
}

void gnostr_bookmarks_sync_on_login(const char *pubkey_hex) {
    if (!pubkey_hex || !*pubkey_hex) return;

    GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
    if (!bookmarks) return;

    g_message("bookmarks: auto-syncing for user %.8s...", pubkey_hex);
    gnostr_bookmarks_fetch_async(bookmarks, pubkey_hex, NULL, NULL, NULL);
}
