/**
 * NIP-51 Mute List Service Implementation
 *
 * Manages mute lists (kind 10000) for filtering content.
 * Uses the nip51 library for parsing and creating list events.
 * Supports private mute entries via NIP-44 encryption.
 */

#include "gnostr-mute-list.h"
#include "gnostr-relays.h"
#include "nostr-error.h"
#include <glib.h>
#include <string.h>
#include <time.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include "nostr-event.h"
#include "nostr-tag.h"

#ifndef GNOSTR_MUTE_LIST_TEST_ONLY
#include "utils.h"
#include "signer_ipc.h"
#include "gnostr-signer-service.h"
#include "json.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#endif

/* Kind 10000 = Mute List per NIP-51 */
#define MUTE_LIST_KIND 10000

/* ---- Internal Data Structure ---- */

typedef struct {
    char *value;
    gboolean is_private;
} MuteEntry;

static void mute_entry_free(gpointer data) {
    MuteEntry *e = (MuteEntry *)data;
    if (e) {
        g_free(e->value);
        g_free(e);
    }
}

static MuteEntry *mute_entry_new(const char *value, gboolean is_private) {
    MuteEntry *e = g_new0(MuteEntry, 1);
    e->value = g_strdup(value);
    e->is_private = is_private;
    return e;
}

struct _GNostrMuteList {
    /* Hash tables for O(1) lookup: key = value, value = MuteEntry */
    GHashTable *muted_pubkeys;   /* "p" tags */
    GHashTable *muted_events;    /* "e" tags */
    GHashTable *muted_hashtags;  /* "t" tags */
    GHashTable *muted_words;     /* "word" tags */

    /* State */
    gboolean dirty;              /* Has unsaved changes */
    gint64 last_event_time;      /* created_at of last loaded event */
    char *user_pubkey;           /* Current user's pubkey (for fetching) */

    /* Thread safety */
    GMutex lock;
};

/* Singleton instance */
static GNostrMuteList *s_default_instance = NULL;
static GMutex s_init_lock;

/* ---- Internal Helpers ---- */

static void mute_list_clear(GNostrMuteList *self) {
    g_hash_table_remove_all(self->muted_pubkeys);
    g_hash_table_remove_all(self->muted_events);
    g_hash_table_remove_all(self->muted_hashtags);
    g_hash_table_remove_all(self->muted_words);
    self->dirty = FALSE;
    self->last_event_time = 0;
}

static gboolean str_in_hashtable(GHashTable *ht, const char *key) {
    if (!key || !ht) return FALSE;
    return g_hash_table_contains(ht, key);
}

/* Case-insensitive word match check */
static gboolean content_contains_word(const char *content, const char *word) {
    if (!content || !word || !*word) return FALSE;

    gchar *lower_content = g_utf8_strdown(content, -1);
    gchar *lower_word = g_utf8_strdown(word, -1);

    /* Simple substring match - could be enhanced with word boundary checks */
    gboolean found = (strstr(lower_content, lower_word) != NULL);

    g_free(lower_content);
    g_free(lower_word);
    return found;
}

/* ---- Public API Implementation ---- */

GNostrMuteList *gnostr_mute_list_get_default(void) {
    g_mutex_lock(&s_init_lock);
    if (!s_default_instance) {
        s_default_instance = g_new0(GNostrMuteList, 1);
        g_mutex_init(&s_default_instance->lock);
        s_default_instance->muted_pubkeys = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, mute_entry_free);
        s_default_instance->muted_events = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, mute_entry_free);
        s_default_instance->muted_hashtags = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, mute_entry_free);
        s_default_instance->muted_words = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL, mute_entry_free);
        s_default_instance->dirty = FALSE;
        s_default_instance->last_event_time = 0;
        s_default_instance->user_pubkey = NULL;
    }
    g_mutex_unlock(&s_init_lock);
    return s_default_instance;
}

void gnostr_mute_list_shutdown(void) {
    g_mutex_lock(&s_init_lock);
    if (s_default_instance) {
        g_mutex_lock(&s_default_instance->lock);
        g_hash_table_destroy(s_default_instance->muted_pubkeys);
        g_hash_table_destroy(s_default_instance->muted_events);
        g_hash_table_destroy(s_default_instance->muted_hashtags);
        g_hash_table_destroy(s_default_instance->muted_words);
        g_free(s_default_instance->user_pubkey);
        g_mutex_unlock(&s_default_instance->lock);
        g_mutex_clear(&s_default_instance->lock);
        g_free(s_default_instance);
        s_default_instance = NULL;
    }
    g_mutex_unlock(&s_init_lock);
}

gboolean gnostr_mute_list_load_from_json(GNostrMuteList *self,
                                          const char *event_json) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        g_warning("mute_list: failed to parse event JSON");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    int kind = nostr_event_get_kind(event);
    if (kind != MUTE_LIST_KIND) {
        g_warning("mute_list: not a kind 10000 event");
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    gint64 event_time = nostr_event_get_created_at(event);
    if (event_time <= self->last_event_time) {
        g_debug("mute_list: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return TRUE; /* Not an error, just older data */
    }

    /* Clear existing data and load new */
    mute_list_clear(self);
    self->last_event_time = event_time;

    /* Parse tags using NostrTags API */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get(tag, 0);
            const char *value = nostr_tag_get(tag, 1);
            if (!tag_name || !value) continue;

            MuteEntry *entry = mute_entry_new(value, FALSE);

            if (strcmp(tag_name, "p") == 0) {
                g_hash_table_insert(self->muted_pubkeys, entry->value, entry);
                g_debug("mute_list: loaded pubkey %s", value);
            } else if (strcmp(tag_name, "e") == 0) {
                g_hash_table_insert(self->muted_events, entry->value, entry);
                g_debug("mute_list: loaded event %s", value);
            } else if (strcmp(tag_name, "t") == 0) {
                g_hash_table_insert(self->muted_hashtags, entry->value, entry);
                g_debug("mute_list: loaded hashtag %s", value);
            } else if (strcmp(tag_name, "word") == 0) {
                g_hash_table_insert(self->muted_words, entry->value, entry);
                g_debug("mute_list: loaded word %s", value);
            } else {
                mute_entry_free(entry);
            }
        }
    }

    /* Private entries are handled via decrypt_private_entries_async() in
     * on_mute_list_query_done() after loading. This function only parses
     * public entries from tags. See nostrc-nluo for NIP-44 implementation. */

    nostr_event_free(event);
    g_mutex_unlock(&self->lock);

    g_message("mute_list: loaded %u pubkeys, %u events, %u hashtags, %u words",
              g_hash_table_size(self->muted_pubkeys),
              g_hash_table_size(self->muted_events),
              g_hash_table_size(self->muted_hashtags),
              g_hash_table_size(self->muted_words));

    return TRUE;
}

/* Merge entries from JSON without clearing existing data (for UNION strategy) */
static gboolean mute_list_merge_from_json(GNostrMuteList *self,
                                           const char *event_json,
                                           gint64 *out_event_time) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    int kind = nostr_event_get_kind(event);
    if (kind != MUTE_LIST_KIND) {
        nostr_event_free(event);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    gint64 event_time = nostr_event_get_created_at(event);
    if (out_event_time) *out_event_time = event_time;

    /* Update timestamp if newer */
    if (event_time > self->last_event_time) {
        self->last_event_time = event_time;
    }

    /* Merge tags - add items that don't already exist */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        guint added = 0;
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get(tag, 0);
            const char *value = nostr_tag_get(tag, 1);
            if (!tag_name || !value) continue;

            MuteEntry *entry = mute_entry_new(value, FALSE);
            gboolean inserted = FALSE;

            if (strcmp(tag_name, "p") == 0 && !g_hash_table_contains(self->muted_pubkeys, value)) {
                g_hash_table_insert(self->muted_pubkeys, entry->value, entry);
                inserted = TRUE;
            } else if (strcmp(tag_name, "e") == 0 && !g_hash_table_contains(self->muted_events, value)) {
                g_hash_table_insert(self->muted_events, entry->value, entry);
                inserted = TRUE;
            } else if (strcmp(tag_name, "t") == 0 && !g_hash_table_contains(self->muted_hashtags, value)) {
                g_hash_table_insert(self->muted_hashtags, entry->value, entry);
                inserted = TRUE;
            } else if (strcmp(tag_name, "word") == 0 && !g_hash_table_contains(self->muted_words, value)) {
                g_hash_table_insert(self->muted_words, entry->value, entry);
                inserted = TRUE;
            }

            if (inserted) {
                added++;
            } else {
                mute_entry_free(entry);
            }
        }
        g_debug("mute_list: merged %u new entries", added);
    }

    nostr_event_free(event);
    g_mutex_unlock(&self->lock);
    return TRUE;
}

gint64 gnostr_mute_list_get_last_event_time(GNostrMuteList *self) {
    if (!self) return 0;
    g_mutex_lock(&self->lock);
    gint64 time = self->last_event_time;
    g_mutex_unlock(&self->lock);
    return time;
}

/* ---- Async Fetch Implementation ---- */

typedef struct {
    GNostrMuteList *mute_list;
    GNostrMuteListFetchCallback callback;
    gpointer user_data;
    char *pubkey_hex;
    GNostrMuteListMergeStrategy strategy;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

#ifndef GNOSTR_MUTE_LIST_TEST_ONLY

/* Context for private entry decryption */
typedef struct {
    GNostrMuteList *mute_list;
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
    GNostrMuteList *self;
} PrivateEntriesCtx;

/* Callback for iterating private entry tags */
static gboolean parse_private_entry_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    PrivateEntriesCtx *ctx = (PrivateEntriesCtx *)user_data;
    GNostrMuteList *self = ctx->self;

    /* Each element is a tag array like ["p", "pubkey"] */
    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    char *value = NULL;
    /* Get tag name (index 0) and value (index 1) from the root array */
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }
    value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (!value) {
        free(tag_name);
        return true;
    }

    MuteEntry *entry = mute_entry_new(value, TRUE);  /* is_private = TRUE */

    if (strcmp(tag_name, "p") == 0) {
        if (!g_hash_table_contains(self->muted_pubkeys, value)) {
            g_hash_table_insert(self->muted_pubkeys, entry->value, entry);
            g_debug("mute_list: loaded private pubkey %s", value);
        } else {
            mute_entry_free(entry);
        }
    } else if (strcmp(tag_name, "e") == 0) {
        if (!g_hash_table_contains(self->muted_events, value)) {
            g_hash_table_insert(self->muted_events, entry->value, entry);
            g_debug("mute_list: loaded private event %s", value);
        } else {
            mute_entry_free(entry);
        }
    } else if (strcmp(tag_name, "t") == 0) {
        if (!g_hash_table_contains(self->muted_hashtags, value)) {
            g_hash_table_insert(self->muted_hashtags, entry->value, entry);
            g_debug("mute_list: loaded private hashtag %s", value);
        } else {
            mute_entry_free(entry);
        }
    } else if (strcmp(tag_name, "word") == 0) {
        gchar *lower_word = g_utf8_strdown(value, -1);
        if (!g_hash_table_contains(self->muted_words, lower_word)) {
            mute_entry_free(entry);
            entry = mute_entry_new(lower_word, TRUE);
            g_hash_table_insert(self->muted_words, entry->value, entry);
            g_debug("mute_list: loaded private word '%s'", lower_word);
        } else {
            mute_entry_free(entry);
        }
        g_free(lower_word);
    } else {
        mute_entry_free(entry);
    }

    free(tag_name);
    free(value);
    return true;  /* Continue iteration */
}

/* Parse decrypted private entries (JSON array of tags) */
static void parse_private_entries(GNostrMuteList *self, const char *decrypted_json) {
    if (!self || !decrypted_json || !*decrypted_json) return;

    /* Validate that it's an array */
    if (!gnostr_json_is_array_str(decrypted_json)) {
        g_warning("mute_list: decrypted content is not an array");
        return;
    }

    g_mutex_lock(&self->lock);

    /* Iterate over root array using callback */
    PrivateEntriesCtx ctx = { .self = self };
    gnostr_json_array_foreach_root(decrypted_json, parse_private_entry_cb, &ctx);

    g_mutex_unlock(&self->lock);

    g_message("mute_list: parsed private entries");
}

/* Callback when private entries are decrypted */
static void on_private_entries_decrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    DecryptPrivateContext *ctx = (DecryptPrivateContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    g_autoptr(GError) error = NULL;
    char *decrypted_json = NULL;

    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &decrypted_json, res, &error);

    if (!ok || !decrypted_json || !*decrypted_json) {
        /* Not an error - just means no private entries or decryption failed */
        g_debug("mute_list: no private entries to decrypt or decryption failed: %s",
                error ? error->message : "empty result");
        decrypt_private_ctx_free(ctx);
        return;
    }

    g_debug("mute_list: decrypted private entries: %.100s...", decrypted_json);
    parse_private_entries(ctx->mute_list, decrypted_json);

    g_free(decrypted_json);
    decrypt_private_ctx_free(ctx);
}

/* Decrypt private mute entries from event content */
static void decrypt_private_entries_async(GNostrMuteList *self,
                                           const char *encrypted_content,
                                           const char *user_pubkey) {
    if (!self || !encrypted_content || !*encrypted_content || !user_pubkey) return;

    g_autoptr(GError) error = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_debug("mute_list: cannot decrypt private entries - signer not available: %s",
                error ? error->message : "unknown");
        return;
    }

    DecryptPrivateContext *ctx = g_new0(DecryptPrivateContext, 1);
    ctx->mute_list = self;
    ctx->encrypted_content = g_strdup(encrypted_content);
    ctx->user_pubkey = g_strdup(user_pubkey);

    /* NIP-44 decrypt: for mute list, we encrypt to ourselves */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        encrypted_content,
        user_pubkey,      /* peer pubkey = self (encrypted to self) */
        user_pubkey,      /* identity = current user */
        NULL,             /* GCancellable */
        on_private_entries_decrypted,
        ctx);
}

/* Callback when relay query completes */
static void on_mute_list_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchContext *ctx = (FetchContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(
        GNOSTR_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("mute_list: query failed: %s", err->message);
        }
        if (ctx->callback) {
            ctx->callback(ctx->mute_list, FALSE, ctx->user_data);
        }
        g_error_free(err);
        fetch_context_free(ctx);
        return;
    }

    gboolean success = FALSE;
    gint64 newest_created_at = 0;
    const char *newest_event_json = NULL;
    char *encrypted_content = NULL;  /* owned copy */

    /* Find the newest mute list event using NostrEvent API */
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
            if (kind != MUTE_LIST_KIND) {
                nostr_event_free(event);
                continue;
            }

            /* Check timestamp */
            gint64 event_time = nostr_event_get_created_at(event);

            if (event_time > newest_created_at) {
                newest_created_at = event_time;
                newest_event_json = json_str;

                /* Get encrypted content for private entries */
                const char *content_str = nostr_event_get_content(event);
                g_free(encrypted_content);
                encrypted_content = (content_str && *content_str) ? g_strdup(content_str) : NULL;
            }
            nostr_event_free(event);
        }
    }

    /* Apply merge strategy */
    if (newest_event_json) {
        gint64 local_time = gnostr_mute_list_get_last_event_time(ctx->mute_list);

        switch (ctx->strategy) {
        case GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS:
            /* Keep local if it exists and has data */
            if (local_time > 0) {
                g_debug("mute_list: LOCAL_WINS - keeping local data (time=%lld)", (long long)local_time);
                success = TRUE;
            } else {
                /* No local data, load remote */
                success = gnostr_mute_list_load_from_json(ctx->mute_list, newest_event_json);
            }
            break;

        case GNOSTR_MUTE_LIST_MERGE_LATEST:
            /* Compare timestamps */
            if (newest_created_at > local_time) {
                g_debug("mute_list: LATEST - using remote (remote=%lld > local=%lld)",
                        (long long)newest_created_at, (long long)local_time);
                success = gnostr_mute_list_load_from_json(ctx->mute_list, newest_event_json);
            } else {
                g_debug("mute_list: LATEST - keeping local (local=%lld >= remote=%lld)",
                        (long long)local_time, (long long)newest_created_at);
                success = TRUE;
            }
            break;

        case GNOSTR_MUTE_LIST_MERGE_UNION:
            /* Merge without clearing - add new items */
            g_debug("mute_list: UNION - merging remote into local");
            success = mute_list_merge_from_json(ctx->mute_list, newest_event_json, NULL);
            break;

        case GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS:
        default:
            /* Replace local with remote (original behavior) */
            g_debug("mute_list: REMOTE_WINS - replacing local with remote");
            success = gnostr_mute_list_load_from_json(ctx->mute_list, newest_event_json);
            break;
        }

        /* Decrypt private entries if we loaded/merged remote data */
        if (success && encrypted_content && *encrypted_content &&
            ctx->strategy != GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS) {
            decrypt_private_entries_async(ctx->mute_list, encrypted_content, ctx->pubkey_hex);
        }
    }

    g_free(encrypted_content);
    if (results) g_ptr_array_unref(results);

    if (ctx->callback) {
        ctx->callback(ctx->mute_list, success, ctx->user_data);
    }

    fetch_context_free(ctx);
}

/* Static pool for mute list operations */
static GNostrPool *s_mute_list_pool = NULL;

#endif /* GNOSTR_MUTE_LIST_TEST_ONLY */

void gnostr_mute_list_fetch_async(GNostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GNostrMuteListFetchCallback callback,
                                   gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);

#ifdef GNOSTR_MUTE_LIST_TEST_ONLY
    (void)relays;
    g_message("mute_list: fetch requested for pubkey %s (test mode - stub)", pubkey_hex);
    if (callback) callback(self, TRUE, user_data);
#else
    FetchContext *ctx = g_new0(FetchContext, 1);
    ctx->mute_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pubkey_hex = g_strdup(pubkey_hex);
    ctx->strategy = GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS;  /* Default: replace local */

    /* Build filter for kind 10000 by author */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { MUTE_LIST_KIND };
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
    if (!s_mute_list_pool) {
        s_mute_list_pool = gnostr_pool_new();
    }

    g_message("mute_list: fetching kind %d for pubkey %.8s from %u relays",
              MUTE_LIST_KIND, pubkey_hex, relay_arr->len);

        gnostr_pool_sync_relays(s_mute_list_pool, (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(s_mute_list_pool, _qf, NULL, /* cancellable */
        on_mute_list_query_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
#endif
}

void gnostr_mute_list_fetch_with_strategy_async(GNostrMuteList *self,
                                                 const char *pubkey_hex,
                                                 const char * const *relays,
                                                 GNostrMuteListMergeStrategy strategy,
                                                 GNostrMuteListFetchCallback callback,
                                                 gpointer user_data) {
    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);

#ifdef GNOSTR_MUTE_LIST_TEST_ONLY
    (void)relays;
    (void)strategy;
    g_message("mute_list: fetch with strategy requested for pubkey %s (test mode - stub)", pubkey_hex);
    if (callback) callback(self, TRUE, user_data);
#else
    FetchContext *ctx = g_new0(FetchContext, 1);
    ctx->mute_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pubkey_hex = g_strdup(pubkey_hex);
    ctx->strategy = strategy;

    /* Build filter for kind 10000 by author */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { MUTE_LIST_KIND };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 5);  /* Get a few to find newest */

    /* Get relay URLs */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);

    if (relays && relays[0]) {
        for (const char * const *r = relays; *r; r++) {
            g_ptr_array_add(relay_arr, g_strdup(*r));
        }
    } else {
        gnostr_load_relays_into(relay_arr);
    }

    const char **urls = g_new0(const char*, relay_arr->len + 1);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    if (!s_mute_list_pool) {
        s_mute_list_pool = gnostr_pool_new();
    }

    g_message("mute_list: fetching kind %d for pubkey %.8s from %u relays (strategy=%d)",
              MUTE_LIST_KIND, pubkey_hex, relay_arr->len, strategy);

        gnostr_pool_sync_relays(s_mute_list_pool, (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(s_mute_list_pool, _qf, NULL, on_mute_list_query_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
#endif
}

/* ---- Query Functions ---- */

gboolean gnostr_mute_list_is_pubkey_muted(GNostrMuteList *self,
                                           const char *pubkey_hex) {
    if (!self || !pubkey_hex) return FALSE;
    /* nostrc-akyz: defensively normalize npub/nprofile to hex */
    g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    if (!hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = str_in_hashtable(self->muted_pubkeys, hex);
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_mute_list_is_event_muted(GNostrMuteList *self,
                                          const char *event_id_hex) {
    if (!self || !event_id_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = str_in_hashtable(self->muted_events, event_id_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_mute_list_is_hashtag_muted(GNostrMuteList *self,
                                            const char *hashtag) {
    if (!self || !hashtag) return FALSE;
    g_mutex_lock(&self->lock);
    /* Normalize: strip leading # if present */
    const char *tag = hashtag;
    if (tag[0] == '#') tag++;
    gboolean result = str_in_hashtable(self->muted_hashtags, tag);
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_mute_list_contains_muted_word(GNostrMuteList *self,
                                               const char *content) {
    if (!self || !content) return FALSE;

    g_mutex_lock(&self->lock);
    gboolean found = FALSE;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->muted_words);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *word = (const char *)key;
        if (content_contains_word(content, word)) {
            found = TRUE;
            break;
        }
    }

    g_mutex_unlock(&self->lock);
    return found;
}

gboolean gnostr_mute_list_should_hide_event(GNostrMuteList *self,
                                             const char *event_json) {
    if (!self || !event_json) return FALSE;

    /* Parse event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, event_json, NULL);
    if (parse_rc != 1) {
        nostr_event_free(event);
        return FALSE;
    }

    gboolean hide = FALSE;

    /* Check author pubkey */
    const char *pubkey = nostr_event_get_pubkey(event);
    if (pubkey && gnostr_mute_list_is_pubkey_muted(self, pubkey)) {
        hide = TRUE;
        goto done;
    }

    /* Check event id */
    char *id = nostr_event_get_id(event);
    if (id && gnostr_mute_list_is_event_muted(self, id)) {
        free(id);
        hide = TRUE;
        goto done;
    }
    free(id);

    /* Check content for muted words */
    const char *content = nostr_event_get_content(event);
    if (content && gnostr_mute_list_contains_muted_word(self, content)) {
        hide = TRUE;
        goto done;
    }

    /* Check hashtags in tags using NostrTags API */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t idx = 0; idx < tag_count; idx++) {
            NostrTag *tag = nostr_tags_get(tags, idx);
            if (!tag || nostr_tag_size(tag) < 2) continue;
            const char *tag_name = nostr_tag_get(tag, 0);
            const char *value = nostr_tag_get(tag, 1);
            if (tag_name && strcmp(tag_name, "t") == 0 && value) {
                if (gnostr_mute_list_is_hashtag_muted(self, value)) {
                    hide = TRUE;
                    goto done;
                }
            }
        }
    }

done:
    nostr_event_free(event);
    return hide;
}

/* ---- Modification Functions ---- */

void gnostr_mute_list_add_pubkey(GNostrMuteList *self,
                                  const char *pubkey_hex,
                                  gboolean is_private) {
    /* nostrc-akyz: defensively normalize npub/nprofile to hex */
    g_autofree gchar *hex = NULL;
    if (pubkey_hex && strlen(pubkey_hex) != 64) {
      hex = gnostr_ensure_hex_pubkey(pubkey_hex);
      pubkey_hex = hex;
    }
    if (!self || !pubkey_hex || strlen(pubkey_hex) != 64) return;

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->muted_pubkeys, pubkey_hex)) {
        MuteEntry *entry = mute_entry_new(pubkey_hex, is_private);
        g_hash_table_insert(self->muted_pubkeys, entry->value, entry);
        self->dirty = TRUE;
        g_message("mute_list: added pubkey %s (private=%d)", pubkey_hex, is_private);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_mute_list_remove_pubkey(GNostrMuteList *self,
                                     const char *pubkey_hex) {
    if (!self || !pubkey_hex) return;
    /* nostrc-akyz: defensively normalize npub/nprofile to hex */
    g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
    if (!hex) return;
    pubkey_hex = hex;

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->muted_pubkeys, pubkey_hex)) {
        self->dirty = TRUE;
        g_message("mute_list: removed pubkey %s", pubkey_hex);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_mute_list_add_word(GNostrMuteList *self,
                                const char *word,
                                gboolean is_private) {
    if (!self || !word || !*word) return;

    gchar *lower_word = g_utf8_strdown(word, -1);

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->muted_words, lower_word)) {
        MuteEntry *entry = mute_entry_new(lower_word, is_private);
        g_hash_table_insert(self->muted_words, entry->value, entry);
        self->dirty = TRUE;
        g_message("mute_list: added word '%s' (private=%d)", lower_word, is_private);
    }
    g_mutex_unlock(&self->lock);

    g_free(lower_word);
}

void gnostr_mute_list_remove_word(GNostrMuteList *self,
                                   const char *word) {
    if (!self || !word) return;

    gchar *lower_word = g_utf8_strdown(word, -1);

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->muted_words, lower_word)) {
        self->dirty = TRUE;
        g_message("mute_list: removed word '%s'", lower_word);
    }
    g_mutex_unlock(&self->lock);

    g_free(lower_word);
}

void gnostr_mute_list_add_hashtag(GNostrMuteList *self,
                                   const char *hashtag,
                                   gboolean is_private) {
    if (!self || !hashtag || !*hashtag) return;

    /* Strip leading # if present */
    const char *tag = hashtag;
    if (tag[0] == '#') tag++;

    gchar *lower_tag = g_utf8_strdown(tag, -1);

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->muted_hashtags, lower_tag)) {
        MuteEntry *entry = mute_entry_new(lower_tag, is_private);
        g_hash_table_insert(self->muted_hashtags, entry->value, entry);
        self->dirty = TRUE;
        g_message("mute_list: added hashtag '%s' (private=%d)", lower_tag, is_private);
    }
    g_mutex_unlock(&self->lock);

    g_free(lower_tag);
}

void gnostr_mute_list_remove_hashtag(GNostrMuteList *self,
                                      const char *hashtag) {
    if (!self || !hashtag) return;

    const char *tag = hashtag;
    if (tag[0] == '#') tag++;

    gchar *lower_tag = g_utf8_strdown(tag, -1);

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->muted_hashtags, lower_tag)) {
        self->dirty = TRUE;
        g_message("mute_list: removed hashtag '%s'", lower_tag);
    }
    g_mutex_unlock(&self->lock);

    g_free(lower_tag);
}

void gnostr_mute_list_add_event(GNostrMuteList *self,
                                 const char *event_id_hex,
                                 gboolean is_private) {
    if (!self || !event_id_hex || strlen(event_id_hex) != 64) return;

    g_mutex_lock(&self->lock);
    if (!g_hash_table_contains(self->muted_events, event_id_hex)) {
        MuteEntry *entry = mute_entry_new(event_id_hex, is_private);
        g_hash_table_insert(self->muted_events, entry->value, entry);
        self->dirty = TRUE;
        g_message("mute_list: added event %s (private=%d)", event_id_hex, is_private);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_mute_list_remove_event(GNostrMuteList *self,
                                    const char *event_id_hex) {
    if (!self || !event_id_hex) return;

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->muted_events, event_id_hex)) {
        self->dirty = TRUE;
        g_message("mute_list: removed event %s", event_id_hex);
    }
    g_mutex_unlock(&self->lock);
}

/* ---- Save Implementation ---- */

typedef struct {
    GNostrMuteList *mute_list;
    GNostrMuteListSaveCallback callback;
    gpointer user_data;
    char *event_json;
    char *private_tags_json;  /* JSON array of private tags for encryption */
    char *user_pubkey;
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    if (ctx) {
        g_free(ctx->event_json);
        g_free(ctx->private_tags_json);
        g_free(ctx->user_pubkey);
        g_free(ctx);
    }
}

#ifndef GNOSTR_MUTE_LIST_TEST_ONLY

static void mute_list_publish_done(guint success_count, guint fail_count, gpointer user_data);

/* Publish signed event to relays */
static void publish_to_relays(SaveContext *ctx, const char *signed_event_json) {
    /* Parse the signed event */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
    if (parse_rc != 1) {
        g_warning("mute_list: failed to parse signed event");
        if (ctx->callback) {
            ctx->callback(ctx->mute_list, FALSE, "Failed to parse signed event", ctx->user_data);
        }
        nostr_event_free(event);
        save_context_free(ctx);
        return;
    }

    /* Publish to relays asynchronously (hq-gflmf) */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(relay_urls);

    gnostr_publish_to_relays_async(event, relay_urls,
        mute_list_publish_done, ctx);
    /* event + relay_urls ownership transferred; ctx freed in callback */
}

static void
mute_list_publish_done(guint success_count, guint fail_count, gpointer user_data)
{
    SaveContext *ctx = (SaveContext *)user_data;

    if (success_count > 0) {
        g_mutex_lock(&ctx->mute_list->lock);
        ctx->mute_list->dirty = FALSE;
        ctx->mute_list->last_event_time = (gint64)time(NULL);
        g_mutex_unlock(&ctx->mute_list->lock);
    }

    if (ctx->callback) {
        if (success_count > 0) {
            ctx->callback(ctx->mute_list, TRUE, NULL, ctx->user_data);
        } else {
            ctx->callback(ctx->mute_list, FALSE, "Failed to publish to any relay", ctx->user_data);
        }
    }

    g_message("mute_list: published to %u relays, failed %u", success_count, fail_count);
    save_context_free(ctx);
}

#endif /* GNOSTR_MUTE_LIST_TEST_ONLY */

static void on_mute_list_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    (void)source;
    if (!ctx) return;

    g_autoptr(GError) error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        g_warning("mute_list: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->mute_list, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        save_context_free(ctx);
        return;
    }

    g_message("mute_list: signed event successfully");

#ifdef GNOSTR_MUTE_LIST_TEST_ONLY
    /* In test mode, just mark as saved */
    g_mutex_lock(&ctx->mute_list->lock);
    ctx->mute_list->dirty = FALSE;
    ctx->mute_list->last_event_time = (gint64)time(NULL);
    g_mutex_unlock(&ctx->mute_list->lock);

    if (ctx->callback) {
        ctx->callback(ctx->mute_list, TRUE, NULL, ctx->user_data);
    }
    g_free(signed_event_json);
    save_context_free(ctx);
#else
    /* Publish to relays */
    publish_to_relays(ctx, signed_event_json);
    g_free(signed_event_json);
#endif
}

/* Helper to build JSON array of private tags using GNostrJsonBuilder */
static char *build_private_tags_json(GNostrMuteList *self) {
    GNostrJsonBuilder *builder = gnostr_json_builder_new();
    GHashTableIter iter;
    gpointer key, value;
    int tag_count = 0;

    gnostr_json_builder_begin_array(builder);

    /* Add private pubkeys */
    g_hash_table_iter_init(&iter, self->muted_pubkeys);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (entry->is_private) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "p");
            gnostr_json_builder_add_string(builder, entry->value);
            gnostr_json_builder_end_array(builder);
            tag_count++;
        }
    }

    /* Add private events */
    g_hash_table_iter_init(&iter, self->muted_events);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (entry->is_private) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "e");
            gnostr_json_builder_add_string(builder, entry->value);
            gnostr_json_builder_end_array(builder);
            tag_count++;
        }
    }

    /* Add private hashtags */
    g_hash_table_iter_init(&iter, self->muted_hashtags);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (entry->is_private) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "t");
            gnostr_json_builder_add_string(builder, entry->value);
            gnostr_json_builder_end_array(builder);
            tag_count++;
        }
    }

    /* Add private words */
    g_hash_table_iter_init(&iter, self->muted_words);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (entry->is_private) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "word");
            gnostr_json_builder_add_string(builder, entry->value);
            gnostr_json_builder_end_array(builder);
            tag_count++;
        }
    }

    gnostr_json_builder_end_array(builder);

    /* Return NULL if no private tags */
    if (tag_count == 0) {
        g_object_unref(builder);
        return NULL;
    }

    char *result = gnostr_json_builder_finish(builder);
    g_object_unref(builder);
    return result;
}

/* Forward declaration */
static void proceed_to_sign(SaveContext *ctx, const char *encrypted_content);

/* Callback when private tags are encrypted */
static void on_private_tags_encrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    g_autoptr(GError) error = NULL;
    char *encrypted_content = NULL;

    gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(
        proxy, &encrypted_content, res, &error);

    if (!ok || !encrypted_content) {
        g_warning("mute_list: failed to encrypt private entries: %s",
                  error ? error->message : "unknown");
        /* Proceed without private entries - still save public ones */
        proceed_to_sign(ctx, "");
        return;
    }

    g_debug("mute_list: encrypted private entries");
    proceed_to_sign(ctx, encrypted_content);
    g_free(encrypted_content);
}

/* Build and sign the event with given content */
static void proceed_to_sign(SaveContext *ctx, const char *encrypted_content) {
    /* Check if signer service is available */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (ctx->callback) ctx->callback(ctx->mute_list, FALSE, "Signer not available", ctx->user_data);
        save_context_free(ctx);
        return;
    }

    g_mutex_lock(&ctx->mute_list->lock);

    /* Build the public tags using NostrTags API */
    NostrTags *tags = nostr_tags_new(0);
    GHashTableIter iter;
    gpointer key, value;

    /* Add public pubkeys */
    g_hash_table_iter_init(&iter, ctx->mute_list->muted_pubkeys);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            NostrTag *tag = nostr_tag_new("p", entry->value, NULL);
            nostr_tags_append(tags, tag);
        }
    }

    /* Add public events */
    g_hash_table_iter_init(&iter, ctx->mute_list->muted_events);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            NostrTag *tag = nostr_tag_new("e", entry->value, NULL);
            nostr_tags_append(tags, tag);
        }
    }

    /* Add public hashtags */
    g_hash_table_iter_init(&iter, ctx->mute_list->muted_hashtags);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            NostrTag *tag = nostr_tag_new("t", entry->value, NULL);
            nostr_tags_append(tags, tag);
        }
    }

    /* Add public words */
    g_hash_table_iter_init(&iter, ctx->mute_list->muted_words);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            NostrTag *tag = nostr_tag_new("word", entry->value, NULL);
            nostr_tags_append(tags, tag);
        }
    }

    g_mutex_unlock(&ctx->mute_list->lock);

    /* Build unsigned event using NostrEvent API */
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, MUTE_LIST_KIND);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, encrypted_content ? encrypted_content : "");
    nostr_event_set_tags(event, tags);  /* Takes ownership of tags */

    char *event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!event_json) {
        if (ctx->callback) ctx->callback(ctx->mute_list, FALSE, "Failed to build event JSON", ctx->user_data);
        save_context_free(ctx);
        return;
    }

    g_message("mute_list: requesting signature for event");
    g_free(ctx->event_json);
    ctx->event_json = event_json;

    /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
    gnostr_sign_event_async(
        event_json,
        "",        /* current_user: ignored */
        "gnostr",  /* app_id: ignored */
        NULL,      /* cancellable */
        on_mute_list_sign_complete,
        ctx
    );
}

void gnostr_mute_list_save_async(GNostrMuteList *self,
                                  GNostrMuteListSaveCallback callback,
                                  gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid mute list", user_data);
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
    ctx->mute_list = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    /* Get user pubkey for encryption */
    g_mutex_lock(&self->lock);
    ctx->user_pubkey = g_strdup(self->user_pubkey);

    /* Build private tags JSON */
    ctx->private_tags_json = build_private_tags_json(self);
    g_mutex_unlock(&self->lock);

    /* Note: Private entry encryption still uses the D-Bus proxy directly
     * because the unified signer service doesn't yet support NIP-44 encrypt.
     * This is a separate issue to address in a future task. */
    GError *proxy_err = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);

    /* If there are private entries and we have user pubkey and proxy, encrypt them first */
    if (ctx->private_tags_json && ctx->user_pubkey && proxy) {
        g_message("mute_list: encrypting private entries");
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

const char **gnostr_mute_list_get_pubkeys(GNostrMuteList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->muted_pubkeys);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->muted_pubkeys);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

const char **gnostr_mute_list_get_words(GNostrMuteList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->muted_words);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->muted_words);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

const char **gnostr_mute_list_get_hashtags(GNostrMuteList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->muted_hashtags);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->muted_hashtags);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

const char **gnostr_mute_list_get_events(GNostrMuteList *self, size_t *count) {
    if (!self || !count) {
        if (count) *count = 0;
        return NULL;
    }

    g_mutex_lock(&self->lock);
    guint n = g_hash_table_size(self->muted_events);
    const char **result = g_new0(const char *, n + 1);

    GHashTableIter iter;
    gpointer key, value;
    size_t i = 0;
    g_hash_table_iter_init(&iter, self->muted_events);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        result[i++] = (const char *)key;
    }
    *count = n;
    g_mutex_unlock(&self->lock);

    return result;
}

gboolean gnostr_mute_list_is_dirty(GNostrMuteList *self) {
    if (!self) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = self->dirty;
    g_mutex_unlock(&self->lock);
    return result;
}

/* ============== GTask-based Async API (R3: GIR-friendly) ============== */

/* Bridge callback: adapts old fetch callback to GTask completion */
static void fetch_gtask_bridge_cb(GNostrMuteList *self,
                                   gboolean success,
                                   gpointer user_data) {
    GTask *task = G_TASK(user_data);
    if (success) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_new_error(task, NOSTR_ERROR, NOSTR_ERROR_RELAY_ERROR,
                                "Mute list fetch failed");
    }
    g_object_unref(task);
}

void gnostr_mute_list_fetch_gtask_async(GNostrMuteList *self,
                                         const char *pubkey_hex,
                                         const char * const *relays,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    gnostr_mute_list_fetch_async(self, pubkey_hex, relays,
                                  fetch_gtask_bridge_cb, task);
}

gboolean gnostr_mute_list_fetch_gtask_finish(GNostrMuteList *self,
                                              GAsyncResult *result,
                                              GError **error) {
    (void)self;
    return g_task_propagate_boolean(G_TASK(result), error);
}

/* Bridge callback: adapts old save callback to GTask completion */
static void save_gtask_bridge_cb(GNostrMuteList *self,
                                  gboolean success,
                                  const char *error_msg,
                                  gpointer user_data) {
    (void)self;
    GTask *task = G_TASK(user_data);
    if (success) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_new_error(task, NOSTR_ERROR, NOSTR_ERROR_RELAY_ERROR,
                                "%s", error_msg ? error_msg : "Mute list save failed");
    }
    g_object_unref(task);
}

void gnostr_mute_list_save_gtask_async(GNostrMuteList *self,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    gnostr_mute_list_save_async(self, save_gtask_bridge_cb, task);
}

gboolean gnostr_mute_list_save_gtask_finish(GNostrMuteList *self,
                                             GAsyncResult *result,
                                             GError **error) {
    (void)self;
    return g_task_propagate_boolean(G_TASK(result), error);
}
