/**
 * NIP-51 Mute List Service Implementation
 *
 * Manages mute lists (kind 10000) for filtering content.
 * Uses the nip51 library for parsing and creating list events.
 */

#include "mute_list.h"
#include "../ipc/signer_ipc.h"
#include <glib.h>
#include <jansson.h>
#include <string.h>
#include <time.h>

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

struct _GnostrMuteList {
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
static GnostrMuteList *s_default_instance = NULL;
static GMutex s_init_lock;

/* ---- Internal Helpers ---- */

static void mute_list_clear(GnostrMuteList *self) {
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

GnostrMuteList *gnostr_mute_list_get_default(void) {
    g_mutex_lock(&s_init_lock);
    if (!s_default_instance) {
        s_default_instance = g_new0(GnostrMuteList, 1);
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

gboolean gnostr_mute_list_load_from_json(GnostrMuteList *self,
                                          const char *event_json) {
    if (!self || !event_json) return FALSE;

    g_mutex_lock(&self->lock);

    json_error_t error;
    json_t *root = json_loads(event_json, 0, &error);
    if (!root) {
        g_warning("mute_list: failed to parse event JSON: %s", error.text);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Verify kind */
    json_t *kind_val = json_object_get(root, "kind");
    if (!kind_val || json_integer_value(kind_val) != MUTE_LIST_KIND) {
        g_warning("mute_list: not a kind 10000 event");
        json_decref(root);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    /* Check if this is newer than what we have */
    json_t *created_at = json_object_get(root, "created_at");
    gint64 event_time = created_at ? json_integer_value(created_at) : 0;
    if (event_time <= self->last_event_time) {
        g_debug("mute_list: ignoring older event (have=%lld, got=%lld)",
                (long long)self->last_event_time, (long long)event_time);
        json_decref(root);
        g_mutex_unlock(&self->lock);
        return TRUE; /* Not an error, just older data */
    }

    /* Clear existing data and load new */
    mute_list_clear(self);
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

    /* TODO: Parse encrypted content for private entries (requires NIP-44) */
    /* For now, we only handle public entries in tags */

    json_decref(root);
    g_mutex_unlock(&self->lock);

    g_message("mute_list: loaded %u pubkeys, %u events, %u hashtags, %u words",
              g_hash_table_size(self->muted_pubkeys),
              g_hash_table_size(self->muted_events),
              g_hash_table_size(self->muted_hashtags),
              g_hash_table_size(self->muted_words));

    return TRUE;
}

/* ---- Async Fetch Implementation ---- */

typedef struct {
    GnostrMuteList *mute_list;
    GnostrMuteListFetchCallback callback;
    gpointer user_data;
    char *pubkey_hex;
} FetchContext;

static void fetch_context_free(FetchContext *ctx) {
    if (ctx) {
        g_free(ctx->pubkey_hex);
        g_free(ctx);
    }
}

/* TODO: Implement relay fetch using SimplePool */
/* For now, this is a stub that can be connected to the main window's pool */
void gnostr_mute_list_fetch_async(GnostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrMuteListFetchCallback callback,
                                   gpointer user_data) {
    (void)relays; /* Will be used when implementing relay fetch */

    if (!self || !pubkey_hex) {
        if (callback) callback(self, FALSE, user_data);
        return;
    }

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);

    /* TODO: Use SimplePool to fetch kind 10000 from relays */
    /* For now, just call the callback indicating we need external loading */
    g_message("mute_list: fetch requested for pubkey %s (stub - load from storage)", pubkey_hex);
    if (callback) callback(self, TRUE, user_data);
}

/* ---- Query Functions ---- */

gboolean gnostr_mute_list_is_pubkey_muted(GnostrMuteList *self,
                                           const char *pubkey_hex) {
    if (!self || !pubkey_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = str_in_hashtable(self->muted_pubkeys, pubkey_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_mute_list_is_event_muted(GnostrMuteList *self,
                                          const char *event_id_hex) {
    if (!self || !event_id_hex) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = str_in_hashtable(self->muted_events, event_id_hex);
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_mute_list_is_hashtag_muted(GnostrMuteList *self,
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

gboolean gnostr_mute_list_contains_muted_word(GnostrMuteList *self,
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

gboolean gnostr_mute_list_should_hide_event(GnostrMuteList *self,
                                             const char *event_json) {
    if (!self || !event_json) return FALSE;

    json_error_t error;
    json_t *root = json_loads(event_json, 0, &error);
    if (!root) return FALSE;

    gboolean hide = FALSE;

    /* Check author pubkey */
    json_t *pubkey = json_object_get(root, "pubkey");
    if (pubkey && json_is_string(pubkey)) {
        if (gnostr_mute_list_is_pubkey_muted(self, json_string_value(pubkey))) {
            hide = TRUE;
            goto done;
        }
    }

    /* Check event id */
    json_t *id = json_object_get(root, "id");
    if (id && json_is_string(id)) {
        if (gnostr_mute_list_is_event_muted(self, json_string_value(id))) {
            hide = TRUE;
            goto done;
        }
    }

    /* Check content for muted words */
    json_t *content = json_object_get(root, "content");
    if (content && json_is_string(content)) {
        if (gnostr_mute_list_contains_muted_word(self, json_string_value(content))) {
            hide = TRUE;
            goto done;
        }
    }

    /* Check hashtags in tags */
    json_t *tags = json_object_get(root, "tags");
    if (json_is_array(tags)) {
        size_t idx;
        json_t *tag;
        json_array_foreach(tags, idx, tag) {
            if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
            const char *tag_name = json_string_value(json_array_get(tag, 0));
            const char *value = json_string_value(json_array_get(tag, 1));
            if (tag_name && strcmp(tag_name, "t") == 0 && value) {
                if (gnostr_mute_list_is_hashtag_muted(self, value)) {
                    hide = TRUE;
                    goto done;
                }
            }
        }
    }

done:
    json_decref(root);
    return hide;
}

/* ---- Modification Functions ---- */

void gnostr_mute_list_add_pubkey(GnostrMuteList *self,
                                  const char *pubkey_hex,
                                  gboolean is_private) {
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

void gnostr_mute_list_remove_pubkey(GnostrMuteList *self,
                                     const char *pubkey_hex) {
    if (!self || !pubkey_hex) return;

    g_mutex_lock(&self->lock);
    if (g_hash_table_remove(self->muted_pubkeys, pubkey_hex)) {
        self->dirty = TRUE;
        g_message("mute_list: removed pubkey %s", pubkey_hex);
    }
    g_mutex_unlock(&self->lock);
}

void gnostr_mute_list_add_word(GnostrMuteList *self,
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

void gnostr_mute_list_remove_word(GnostrMuteList *self,
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

void gnostr_mute_list_add_hashtag(GnostrMuteList *self,
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

void gnostr_mute_list_remove_hashtag(GnostrMuteList *self,
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

void gnostr_mute_list_add_event(GnostrMuteList *self,
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

void gnostr_mute_list_remove_event(GnostrMuteList *self,
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
    GnostrMuteList *mute_list;
    GnostrMuteListSaveCallback callback;
    gpointer user_data;
    char *event_json;
} SaveContext;

static void save_context_free(SaveContext *ctx) {
    if (ctx) {
        g_free(ctx->event_json);
        g_free(ctx);
    }
}

static void on_mute_list_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    SaveContext *ctx = (SaveContext *)user_data;
    if (!ctx) return;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);
    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

    if (!ok || !signed_event_json) {
        g_warning("mute_list: signing failed: %s", error ? error->message : "unknown error");
        if (ctx->callback) {
            ctx->callback(ctx->mute_list, FALSE,
                         error ? error->message : "Signing failed",
                         ctx->user_data);
        }
        g_clear_error(&error);
        save_context_free(ctx);
        return;
    }

    g_message("mute_list: signed event successfully");

    /* TODO: Publish to relays via SimplePool */
    /* For now, mark as saved and notify success */
    g_mutex_lock(&ctx->mute_list->lock);
    ctx->mute_list->dirty = FALSE;
    ctx->mute_list->last_event_time = (gint64)time(NULL);
    g_mutex_unlock(&ctx->mute_list->lock);

    if (ctx->callback) {
        ctx->callback(ctx->mute_list, TRUE, NULL, ctx->user_data);
    }

    g_free(signed_event_json);
    save_context_free(ctx);
}

void gnostr_mute_list_save_async(GnostrMuteList *self,
                                  GnostrMuteListSaveCallback callback,
                                  gpointer user_data) {
    if (!self) {
        if (callback) callback(self, FALSE, "Invalid mute list", user_data);
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

    /* Add pubkeys */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->muted_pubkeys);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            json_t *tag = json_array();
            json_array_append_new(tag, json_string("p"));
            json_array_append_new(tag, json_string(entry->value));
            json_array_append(tags, tag);
            json_decref(tag);
        }
    }

    /* Add events */
    g_hash_table_iter_init(&iter, self->muted_events);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            json_t *tag = json_array();
            json_array_append_new(tag, json_string("e"));
            json_array_append_new(tag, json_string(entry->value));
            json_array_append(tags, tag);
            json_decref(tag);
        }
    }

    /* Add hashtags */
    g_hash_table_iter_init(&iter, self->muted_hashtags);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            json_t *tag = json_array();
            json_array_append_new(tag, json_string("t"));
            json_array_append_new(tag, json_string(entry->value));
            json_array_append(tags, tag);
            json_decref(tag);
        }
    }

    /* Add words */
    g_hash_table_iter_init(&iter, self->muted_words);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MuteEntry *entry = (MuteEntry *)value;
        if (!entry->is_private) {
            json_t *tag = json_array();
            json_array_append_new(tag, json_string("word"));
            json_array_append_new(tag, json_string(entry->value));
            json_array_append(tags, tag);
            json_decref(tag);
        }
    }

    g_mutex_unlock(&self->lock);

    /* Build unsigned event */
    json_t *event_obj = json_object();
    json_object_set_new(event_obj, "kind", json_integer(MUTE_LIST_KIND));
    json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
    json_object_set_new(event_obj, "content", json_string("")); /* TODO: Add encrypted private entries */
    json_object_set_new(event_obj, "tags", tags);

    char *event_json = json_dumps(event_obj, JSON_COMPACT);
    json_decref(event_obj);

    if (!event_json) {
        if (callback) callback(self, FALSE, "Failed to build event JSON", user_data);
        return;
    }

    g_message("mute_list: requesting signature for: %s", event_json);

    /* Create save context */
    SaveContext *ctx = g_new0(SaveContext, 1);
    ctx->mute_list = self;
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
        on_mute_list_sign_complete,
        ctx
    );
}

/* ---- Accessors ---- */

const char **gnostr_mute_list_get_pubkeys(GnostrMuteList *self, size_t *count) {
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

const char **gnostr_mute_list_get_words(GnostrMuteList *self, size_t *count) {
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

const char **gnostr_mute_list_get_hashtags(GnostrMuteList *self, size_t *count) {
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

const char **gnostr_mute_list_get_events(GnostrMuteList *self, size_t *count) {
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

gboolean gnostr_mute_list_is_dirty(GnostrMuteList *self) {
    if (!self) return FALSE;
    g_mutex_lock(&self->lock);
    gboolean result = self->dirty;
    g_mutex_unlock(&self->lock);
    return result;
}
