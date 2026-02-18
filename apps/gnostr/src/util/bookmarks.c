/**
 * NIP-51 Bookmark List Service Implementation
 *
 * Manages bookmark lists (kind 10003) for saving notes.
 * Uses the nip51 library for parsing and creating list events.
 * Implements relay fetch and publish per NIP-51.
 */

#include "bookmarks.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include <glib.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <string.h>
#include <time.h>

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include "utils.h"
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

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize(event, event_json) != 0) {
        g_warning("bookmarks: failed to parse event JSON");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    if (nostr_event_get_kind(event) != BOOKMARK_LIST_KIND) {
        g_warning("bookmarks: not a kind 10003 event");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    gint64 event_time = nostr_event_get_created_at(event);
    if (event_time <= self->last_event_time) {
        g_debug("bookmarks: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return TRUE; /* Not an error, just older data */
    }

    /* Clear existing data and load new */
    bookmarks_clear(self);
    self->last_event_time = event_time;

    /* Parse tags using NostrTags API */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get_key(tag);
            const char *value = nostr_tag_get_value(tag);
            if (!tag_name || !value) continue;

            /* NIP-51 bookmark list uses "e" tags for event bookmarks */
            if (strcmp(tag_name, "e") == 0) {
                /* Optional relay hint in third position */
                const char *relay_hint = NULL;
                if (nostr_tag_size(tag) >= 3) {
                    relay_hint = nostr_tag_get(tag, 2);
                }
                BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                g_hash_table_insert(self->bookmarks, entry->event_id, entry);
                g_debug("bookmarks: loaded event %s", value);
            }
            /* Also support "a" tags for addressable events (articles, etc.) */
            else if (strcmp(tag_name, "a") == 0) {
                const char *relay_hint = NULL;
                if (nostr_tag_size(tag) >= 3) {
                    relay_hint = nostr_tag_get(tag, 2);
                }
                BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                g_hash_table_insert(self->bookmarks, entry->event_id, entry);
                g_debug("bookmarks: loaded addressable %s", value);
            }
        }
    }

    /* Private entries are handled via decrypt_private_entries_async() in
     * on_bookmarks_query_done() after loading. This function only parses
     * public entries from tags. See nostrc-nluo for NIP-44 implementation. */

    nostr_event_free(event);
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
    GnostrBookmarksMergeStrategy strategy;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY

/* ---- NIP-44 Private Entry Decryption (nostrc-nluo) ---- */

/* Context for private entry decryption */
typedef struct {
    GnostrBookmarks *bookmarks;
    char *encrypted_content;
    char *user_pubkey;
} DecryptPrivateContext;

static void decrypt_private_ctx_free(DecryptPrivateContext *ctx) {
    if (ctx) {
        g_free(ctx->encrypted_content);
        g_free(ctx->user_pubkey);
        g_free(ctx);
    }
}

/* Callback context for parsing private entries */
typedef struct {
    GnostrBookmarks *self;
} PrivateEntriesCtx;

/* Callback for iterating private entry tags */
static gboolean parse_private_entry_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    PrivateEntriesCtx *ctx = (PrivateEntriesCtx *)user_data;
    GnostrBookmarks *self = ctx->self;

    /* Each element is a tag array like ["e", "event_id", "relay_hint"] */
    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    char *value = NULL;
    char *relay_hint = NULL;

    /* Get tag name (index 0) and value (index 1) */
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }
    value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (!value) {
        free(tag_name);
        return true;
    }
    /* Optional relay hint at index 2 */
    if (tag_len >= 3) {
        relay_hint = gnostr_json_get_array_string(element_json, NULL, 2, NULL);
    }

    /* Only handle "e" and "a" tags for bookmarks */
    if (strcmp(tag_name, "e") == 0 || strcmp(tag_name, "a") == 0) {
        if (!g_hash_table_contains(self->bookmarks, value)) {
            BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, TRUE);  /* is_private = TRUE */
            g_hash_table_insert(self->bookmarks, entry->event_id, entry);
            g_debug("bookmarks: loaded private %s %s", tag_name, value);
        }
    }

    free(tag_name);
    free(value);
    free(relay_hint);
    return true;  /* Continue iteration */
}

/* Parse decrypted private entries (JSON array of tags) */
static void parse_private_entries(GnostrBookmarks *self, const char *decrypted_json) {
    if (!self || !decrypted_json || !*decrypted_json) return;

    /* Validate that it's an array */
    if (!gnostr_json_is_array_str(decrypted_json)) {
        g_warning("bookmarks: decrypted content is not an array");
        return;
    }

    g_mutex_lock(&self->lock);

    /* Iterate over root array using callback */
    PrivateEntriesCtx ctx = { .self = self };
    gnostr_json_array_foreach_root(decrypted_json, parse_private_entry_cb, &ctx);

    g_mutex_unlock(&self->lock);

    g_message("bookmarks: parsed private entries");
}

/* Callback when private entries are decrypted */
static void on_private_entries_decrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    DecryptPrivateContext *ctx = (DecryptPrivateContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    GError *error = NULL;
    char *decrypted_json = NULL;

    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &decrypted_json, res, &error);

    if (!ok || !decrypted_json || !*decrypted_json) {
        /* Not an error - just means no private entries or decryption failed */
        g_debug("bookmarks: no private entries to decrypt or decryption failed: %s",
                error ? error->message : "empty result");
        g_clear_error(&error);
        decrypt_private_ctx_free(ctx);
        return;
    }

    g_debug("bookmarks: decrypted private entries: %.100s...", decrypted_json);
    parse_private_entries(ctx->bookmarks, decrypted_json);

    g_free(decrypted_json);
    decrypt_private_ctx_free(ctx);
}

/* Decrypt private bookmark entries from event content */
static void decrypt_private_entries_async(GnostrBookmarks *self,
                                           const char *encrypted_content,
                                           const char *user_pubkey) {
    if (!self || !encrypted_content || !*encrypted_content || !user_pubkey) return;

    GError *error = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_debug("bookmarks: cannot decrypt private entries - signer not available: %s",
                error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    DecryptPrivateContext *ctx = g_new0(DecryptPrivateContext, 1);
    ctx->bookmarks = self;
    ctx->encrypted_content = g_strdup(encrypted_content);
    ctx->user_pubkey = g_strdup(user_pubkey);

    /* NIP-44 decrypt: for bookmark list, we encrypt to ourselves */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        encrypted_content,
        user_pubkey,      /* peer pubkey = self (encrypted to self) */
        user_pubkey,      /* identity = current user */
        NULL,             /* GCancellable */
        on_private_entries_decrypted,
        ctx);
}

/* Internal helper: load bookmarks from event tags into hash table (does not clear first) */
static void load_bookmarks_from_event_unlocked(GnostrBookmarks *self, NostrEvent *event) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (!tags) return;

    size_t tag_count = nostr_tags_size(tags);
    for (size_t idx = 0; idx < tag_count; idx++) {
        NostrTag *tag = nostr_tags_get(tags, idx);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *tag_name = nostr_tag_get_key(tag);
        const char *value = nostr_tag_get_value(tag);
        if (!tag_name || !value) continue;

        if (strcmp(tag_name, "e") == 0 || strcmp(tag_name, "a") == 0) {
            const char *relay_hint = NULL;
            if (nostr_tag_size(tag) >= 3) {
                relay_hint = nostr_tag_get(tag, 2);
            }
            /* Only insert if not already present (for UNION strategy) */
            if (!g_hash_table_contains(self->bookmarks, value)) {
                BookmarkEntry *entry = bookmark_entry_new(value, relay_hint, FALSE);
                g_hash_table_insert(self->bookmarks, entry->event_id, entry);
            }
        }
    }
}

/* Internal: merge remote bookmarks into local with strategy */
static void bookmarks_merge_from_json_with_strategy_unlocked(GnostrBookmarks *self,
                                                               const char *event_json,
                                                               GnostrBookmarksMergeStrategy strategy,
                                                               gint64 *out_created_at) {
    if (!self || !event_json) return;

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize(event, event_json) != 0) {
        nostr_event_free(event);
        return;
    }

    /* Verify kind */
    if (nostr_event_get_kind(event) != BOOKMARK_LIST_KIND) {
        nostr_event_free(event);
        return;
    }

    /* Extract created_at */
    gint64 event_time = nostr_event_get_created_at(event);
    if (out_created_at) *out_created_at = event_time;

    switch (strategy) {
    case GNOSTR_BOOKMARKS_MERGE_LOCAL_WINS:
        /* Keep local data, only update timestamp if remote is newer */
        if (event_time > self->last_event_time) {
            self->last_event_time = event_time;
        }
        g_debug("bookmarks: LOCAL_WINS - keeping local data");
        break;

    case GNOSTR_BOOKMARKS_MERGE_REMOTE_WINS:
        /* Clear local and load from remote unconditionally */
        g_hash_table_remove_all(self->bookmarks);
        self->last_event_time = event_time;
        load_bookmarks_from_event_unlocked(self, event);
        self->dirty = FALSE;
        g_message("bookmarks: REMOTE_WINS - replaced with %u remote bookmarks",
                  g_hash_table_size(self->bookmarks));
        break;

    case GNOSTR_BOOKMARKS_MERGE_UNION:
        /* Add remote bookmarks to local without removing existing */
        load_bookmarks_from_event_unlocked(self, event);
        if (event_time > self->last_event_time) {
            self->last_event_time = event_time;
        }
        self->dirty = TRUE; /* Need to publish merged list */
        g_message("bookmarks: UNION - now have %u bookmarks",
                  g_hash_table_size(self->bookmarks));
        break;

    case GNOSTR_BOOKMARKS_MERGE_LATEST:
    default:
        /* Original behavior: prefer most recent by timestamp */
        if (event_time > self->last_event_time) {
            g_hash_table_remove_all(self->bookmarks);
            self->last_event_time = event_time;
            load_bookmarks_from_event_unlocked(self, event);
            self->dirty = FALSE;
            g_message("bookmarks: LATEST - loaded %u bookmarks (remote newer)",
                      g_hash_table_size(self->bookmarks));
        } else {
            g_debug("bookmarks: LATEST - keeping local (local newer or same)");
        }
        break;
    }

    nostr_event_free(event);
}

/* Internal: merge remote bookmarks into local, preferring most recent (legacy) */
static void bookmarks_merge_from_json_unlocked(GnostrBookmarks *self,
                                                const char *event_json,
                                                gint64 *out_created_at) {
    bookmarks_merge_from_json_with_strategy_unlocked(self, event_json,
                                                      GNOSTR_BOOKMARKS_MERGE_LATEST,
                                                      out_created_at);
}

/* Callback when relay query completes */
static void on_bookmarks_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchContext *ctx = (FetchContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

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
    const char *newest_event_json = NULL;
    char *encrypted_content = NULL;  /* owned copy */

    /* Find and load the newest bookmark list event */
    if (results && results->len > 0) {
        /* First pass: find newest event and extract encrypted content */
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);

            NostrEvent *event = nostr_event_new();
            if (nostr_event_deserialize(event, json) != 0) {
                nostr_event_free(event);
                continue;
            }

            /* Verify kind */
            if (nostr_event_get_kind(event) != BOOKMARK_LIST_KIND) {
                nostr_event_free(event);
                continue;
            }

            /* Check timestamp */
            gint64 event_time = nostr_event_get_created_at(event);
            if (event_time > newest_created_at) {
                newest_created_at = event_time;
                newest_event_json = json;

                /* Get encrypted content for private entries (nostrc-nluo) */
                const char *content_str = nostr_event_get_content(event);
                g_free(encrypted_content);
                encrypted_content = (content_str && *content_str) ? g_strdup(content_str) : NULL;
            }
            nostr_event_free(event);
        }

        /* Load the newest event with strategy */
        if (newest_event_json) {
            g_mutex_lock(&ctx->bookmarks->lock);
            gint64 created_at = 0;
            bookmarks_merge_from_json_with_strategy_unlocked(ctx->bookmarks, newest_event_json,
                                                              ctx->strategy, &created_at);
            g_mutex_unlock(&ctx->bookmarks->lock);
            success = TRUE;

            /* Decrypt private entries if present (nostrc-nluo) */
            if (encrypted_content && *encrypted_content) {
                decrypt_private_entries_async(ctx->bookmarks, encrypted_content, ctx->pubkey_hex);
            }
        }
    }

    g_free(encrypted_content);
    if (results) g_ptr_array_unref(results);

    g_message("bookmarks: fetch completed, success=%d, count=%zu",
              success, gnostr_bookmarks_get_count(ctx->bookmarks));

    if (ctx->callback) {
        ctx->callback(ctx->bookmarks, TRUE, ctx->user_data);
    }

    fetch_context_free(ctx);
}

/* Singleton pool for bookmark queries */
static GNostrPool *s_bookmarks_pool = NULL;

void gnostr_bookmarks_fetch_with_strategy_async(GnostrBookmarks *self,
                                                 const char *pubkey_hex,
                                                 const char * const *relays,
                                                 GnostrBookmarksMergeStrategy strategy,
                                                 GnostrBookmarksFetchCallback callback,
                                                 gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    /* LOCAL_WINS: Skip fetch entirely, just return success */
    if (strategy == GNOSTR_BOOKMARKS_MERGE_LOCAL_WINS) {
        g_message("bookmarks: LOCAL_WINS strategy - skipping remote fetch");
        if (callback) callback(self, TRUE, user_data);
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
    ctx->strategy = strategy;

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
    if (!s_bookmarks_pool) s_bookmarks_pool = gnostr_pool_new();

    g_message("bookmarks: fetching kind %d from %u relays for pubkey %.8s...",
              BOOKMARK_LIST_KIND, relay_arr->len, pubkey_hex);

        gnostr_pool_sync_relays(s_bookmarks_pool, (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(s_bookmarks_pool, _qf, NULL, /* cancellable */
        on_bookmarks_query_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

void gnostr_bookmarks_fetch_async(GnostrBookmarks *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrBookmarksFetchCallback callback,
                                   gpointer user_data) {
    gnostr_bookmarks_fetch_with_strategy_async(self, pubkey_hex, relays,
                                                GNOSTR_BOOKMARKS_MERGE_LATEST,
                                                callback, user_data);
}

#else /* GNOSTR_BOOKMARKS_TEST_ONLY */

void gnostr_bookmarks_fetch_with_strategy_async(GnostrBookmarks *self,
                                                 const char *pubkey_hex,
                                                 const char * const *relays,
                                                 GnostrBookmarksMergeStrategy strategy,
                                                 GnostrBookmarksFetchCallback callback,
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
    g_message("bookmarks: fetch with strategy requested (test stub)");
    if (callback) callback(self, TRUE, user_data);
}

void gnostr_bookmarks_fetch_async(GnostrBookmarks *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrBookmarksFetchCallback callback,
                                   gpointer user_data) {
    gnostr_bookmarks_fetch_with_strategy_async(self, pubkey_hex, relays,
                                                GNOSTR_BOOKMARKS_MERGE_LATEST,
                                                callback, user_data);
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
    char *user_pubkey;         /* For NIP-44 encryption */
    char *private_tags_json;   /* JSON array of private tags to encrypt */
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    if (ctx) {
        g_free(ctx->event_json);
        g_free(ctx->user_pubkey);
        g_free(ctx->private_tags_json);
        g_free(ctx);
    }
}

#ifndef GNOSTR_BOOKMARKS_TEST_ONLY

/* ---- NIP-44 Private Entry Encryption (nostrc-nluo) ---- */

/* Helper to build JSON array of private bookmark tags using GNostrJsonBuilder */
static char *build_private_tags_json(GnostrBookmarks *self) {
    g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
    GHashTableIter iter;
    gpointer key, value;
    int tag_count = 0;

    gnostr_json_builder_begin_array(builder);

    /* Add private bookmarked events */
    g_hash_table_iter_init(&iter, self->bookmarks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        BookmarkEntry *entry = (BookmarkEntry *)value;
        if (entry->is_private) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "e");
            gnostr_json_builder_add_string(builder, entry->event_id);
            if (entry->relay_hint && *entry->relay_hint) {
                gnostr_json_builder_add_string(builder, entry->relay_hint);
            }
            gnostr_json_builder_end_array(builder);
            tag_count++;
        }
    }

    gnostr_json_builder_end_array(builder);

    /* Return NULL if no private tags */
    if (tag_count == 0) {
        return NULL;
    }

    char *result = gnostr_json_builder_finish(builder);
    return result;
}

/* Forward declarations */
static void proceed_to_sign(SaveContext *ctx, const char *encrypted_content);
static void on_bookmarks_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data);
static void bookmarks_publish_done(guint success_count, guint fail_count, gpointer user_data);

/* Callback when private tags are encrypted */
static void on_private_tags_encrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    GError *error = NULL;
    char *encrypted_content = NULL;

    gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(
        proxy, &encrypted_content, res, &error);

    if (!ok || !encrypted_content) {
        g_warning("bookmarks: failed to encrypt private entries: %s",
                  error ? error->message : "unknown");
        g_clear_error(&error);
        /* Proceed without private entries - still save public ones */
        proceed_to_sign(ctx, "");
        return;
    }

    g_debug("bookmarks: encrypted private entries");
    proceed_to_sign(ctx, encrypted_content);
    g_free(encrypted_content);
}

/* Build and sign the event with given content */
static void proceed_to_sign(SaveContext *ctx, const char *encrypted_content) {
    /* Check if signer service is available */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (ctx->callback) ctx->callback(ctx->bookmarks, FALSE, "Signer not available", ctx->user_data);
        save_context_free(ctx);
        return;
    }

    g_mutex_lock(&ctx->bookmarks->lock);

    /* Count public bookmarks for tag array */
    guint public_count = 0;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ctx->bookmarks->bookmarks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        BookmarkEntry *entry = (BookmarkEntry *)value;
        if (!entry->is_private) public_count++;
    }

    /* Build the tags array using NostrTags API */
    NostrTags *tags = nostr_tags_new(public_count);
    size_t tag_idx = 0;

    /* Add public bookmarked events as "e" tags */
    g_hash_table_iter_init(&iter, ctx->bookmarks->bookmarks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        BookmarkEntry *entry = (BookmarkEntry *)value;
        if (!entry->is_private) {
            NostrTag *tag;
            /* Add relay hint if available */
            if (entry->relay_hint && *entry->relay_hint) {
                tag = nostr_tag_new("e", entry->event_id, entry->relay_hint, NULL);
            } else {
                tag = nostr_tag_new("e", entry->event_id, NULL);
            }
            nostr_tags_set(tags, tag_idx++, tag);
        }
    }

    g_mutex_unlock(&ctx->bookmarks->lock);

    /* Build unsigned event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, BOOKMARK_LIST_KIND);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, encrypted_content ? encrypted_content : "");
    nostr_event_set_tags(event, tags);  /* Takes ownership of tags */

    char *event_json = nostr_event_serialize(event);
    nostr_event_free(event);

    if (!event_json) {
        if (ctx->callback) ctx->callback(ctx->bookmarks, FALSE, "Failed to build event JSON", ctx->user_data);
        save_context_free(ctx);
        return;
    }

    g_message("bookmarks: requesting signature for event");
    g_free(ctx->event_json);
    ctx->event_json = event_json;

    /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
    gnostr_sign_event_async(
        event_json,
        "",        /* current_user: ignored */
        "gnostr",  /* app_id: ignored */
        NULL,      /* cancellable */
        on_bookmarks_sign_complete,
        ctx
    );
}

static void on_bookmarks_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

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
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
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

    /* Publish to relays asynchronously (hq-gflmf) */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_get_write_relay_urls_into(relay_urls);

    /* Fall back to all relays if no write relays configured */
    if (relay_urls->len == 0) {
        gnostr_load_relays_into(relay_urls);
    }

    g_free(signed_event_json);
    gnostr_publish_to_relays_async(event, relay_urls,
        bookmarks_publish_done, ctx);
    /* event + relay_urls ownership transferred; ctx freed in callback */
}

static void
bookmarks_publish_done(guint success_count, guint fail_count, gpointer user_data)
{
    SaveContext *ctx = (SaveContext *)user_data;

    if (success_count > 0) {
        g_mutex_lock(&ctx->bookmarks->lock);
        ctx->bookmarks->dirty = FALSE;
        ctx->bookmarks->last_event_time = (gint64)time(NULL);
        g_mutex_unlock(&ctx->bookmarks->lock);
    }

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
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

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

    /* Check if signer service is available */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (callback) callback(self, FALSE, "Signer not available", user_data);
        return;
    }

    /* Create save context */
    SaveContext *ctx = g_new0(SaveContext, 1);
    ctx->bookmarks = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    /* Get user pubkey for encryption */
    g_mutex_lock(&self->lock);
    ctx->user_pubkey = g_strdup(self->user_pubkey);

    /* Build private tags JSON (nostrc-nluo) */
    ctx->private_tags_json = build_private_tags_json(self);
    g_mutex_unlock(&self->lock);

    /* Note: Private entry encryption still uses the D-Bus proxy directly
     * because the unified signer service doesn't yet support NIP-44 encrypt.
     * This is a separate issue to address in a future task. */
    GError *proxy_err = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);

    /* If there are private entries and we have user pubkey and proxy, encrypt them first */
    if (ctx->private_tags_json && ctx->user_pubkey && proxy) {
        g_message("bookmarks: encrypting private entries");
        nostr_org_nostr_signer_call_nip44_encrypt(
            proxy,
            ctx->private_tags_json,
            ctx->user_pubkey,  /* encrypt to self */
            ctx->user_pubkey,  /* identity = current user */
            NULL,              /* GCancellable */
            on_private_tags_encrypted,
            ctx
        );
    } else {
        g_clear_error(&proxy_err);
        /* No private entries or no pubkey/proxy - proceed directly to sign */
        proceed_to_sign(ctx, "");
    }
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
