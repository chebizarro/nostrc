/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-nip10-thread-manager.c - Unified NIP-10 thread parsing with cache
 *
 * nostrc-pp64 (Epic 4.3): Single canonical implementation of NIP-10
 * e-tag parsing. Replaces 7+ scattered implementations with one
 * cacheable, testable parser.
 */

#include "gnostr-nip10-thread-manager.h"
#include "nostr_json.h"
#include <string.h>

/* Maximum cache entries before eviction */
#define NIP10_CACHE_MAX_SIZE 2048

/* ========== Cache entry ========== */

typedef struct {
    char *event_id;
    char *root_id;
    char *reply_id;
    char *root_relay_hint;
    char *reply_relay_hint;
    char *root_addr;
    char *root_addr_relay;
    gint root_kind;
    gboolean has_explicit_markers;
} CacheEntry;

static void cache_entry_free(CacheEntry *entry) {
    if (!entry) return;
    g_free(entry->event_id);
    g_free(entry->root_id);
    g_free(entry->reply_id);
    g_free(entry->root_relay_hint);
    g_free(entry->reply_relay_hint);
    g_free(entry->root_addr);
    g_free(entry->root_addr_relay);
    g_free(entry);
}

/* ========== Singleton cache ========== */

static GHashTable *cache = NULL; /* event_id -> CacheEntry* */
static GMutex cache_mutex;

static void ensure_cache(void) {
    if (G_UNLIKELY(!cache)) {
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                       (GDestroyNotify)cache_entry_free);
    }
}

/* Evict oldest entries when cache is full.
 * Simple strategy: clear half the cache. This is O(n) but runs rarely. */
static void maybe_evict(void) {
    if (g_hash_table_size(cache) >= NIP10_CACHE_MAX_SIZE) {
        /* Simple eviction: clear the whole cache.
         * A proper LRU would use a linked list, but the cache rebuilds
         * quickly from live events and this keeps the code simple. */
        g_hash_table_remove_all(cache);
    }
}

/* ========== Tag scanning callback ========== */

typedef struct {
    char *root_id;
    char *reply_id;
    char *root_relay_hint;
    char *reply_relay_hint;
    char *root_addr;
    char *root_addr_relay;
    gint root_kind;
    gboolean has_explicit_markers;
    guint etag_count;
    char *first_etag;
    char *last_etag;
    char *first_relay;
    char *last_relay;
} ParseCtx;

static gboolean tag_scan_cb(gsize index, const gchar *tag_json, gpointer user_data) {
    (void)index;
    ParseCtx *ctx = user_data;

    if (!gnostr_json_is_array_str(tag_json)) return TRUE;

    /* Get tag type (index 0) */
    char *tag_type = NULL;
    if ((tag_type = gnostr_json_get_array_string(tag_json, NULL, 0, NULL)) == NULL || !tag_type)
        return TRUE;

    /* Handle "k" tag (NIP-22: root event kind) */
    if (strcmp(tag_type, "k") == 0) {
        char *kind_str = NULL;
        kind_str = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
        if (kind_str) {
            char *endptr;
            long val = strtol(kind_str, &endptr, 10);
            if (*endptr == '\0' && val >= 0 && val <= G_MAXINT)
                ctx->root_kind = (gint)val;
            free(kind_str);
        }
        free(tag_type);
        return TRUE;
    }

    /* Handle "A"/"a" tag (NIP-22: addressable event reference) */
    if (strcmp(tag_type, "a") == 0 || strcmp(tag_type, "A") == 0) {
        char *addr = NULL;
        addr = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
        if (addr) {
            g_free(ctx->root_addr);
            ctx->root_addr = g_strdup(addr);
            free(addr);
            char *relay = NULL;
            relay = gnostr_json_get_array_string(tag_json, NULL, 2, NULL);
            if (relay) {
                g_free(ctx->root_addr_relay);
                ctx->root_addr_relay = (relay[0] != '\0') ? g_strdup(relay) : NULL;
                free(relay);
            }
        }
        free(tag_type);
        return TRUE;
    }

    /* Accept "e" (NIP-10) and "E" (NIP-22) */
    gboolean is_etag = (strcmp(tag_type, "e") == 0 || strcmp(tag_type, "E") == 0);
    free(tag_type);
    if (!is_etag) return TRUE;

    /* Get event ID (index 1) */
    char *event_id = NULL;
    if ((event_id = gnostr_json_get_array_string(tag_json, NULL, 1, NULL)) == NULL || !event_id)
        return TRUE;

    if (strlen(event_id) != 64) {
        free(event_id);
        return TRUE;
    }

    /* Get relay hint (index 2) */
    char *relay = NULL;
    relay = gnostr_json_get_array_string(tag_json, NULL, 2, NULL);

    /* Track positional info */
    ctx->etag_count++;
    if (!ctx->first_etag) {
        ctx->first_etag = g_strdup(event_id);
        g_free(ctx->first_relay);
        ctx->first_relay = relay ? g_strdup(relay) : NULL;
    }
    g_free(ctx->last_etag);
    ctx->last_etag = g_strdup(event_id);
    g_free(ctx->last_relay);
    ctx->last_relay = relay ? g_strdup(relay) : NULL;

    /* Check for explicit marker (index 3) */
    char *marker = NULL;
    marker = gnostr_json_get_array_string(tag_json, NULL, 3, NULL);
    if (marker) {
        if (strcmp(marker, "root") == 0) {
            ctx->has_explicit_markers = TRUE;
            g_free(ctx->root_id);
            ctx->root_id = g_strdup(event_id);
            g_free(ctx->root_relay_hint);
            ctx->root_relay_hint = relay ? g_strdup(relay) : NULL;
        } else if (strcmp(marker, "reply") == 0) {
            ctx->has_explicit_markers = TRUE;
            g_free(ctx->reply_id);
            ctx->reply_id = g_strdup(event_id);
            g_free(ctx->reply_relay_hint);
            ctx->reply_relay_hint = relay ? g_strdup(relay) : NULL;
        }
        /* "mention" marker is intentionally ignored for thread context */
        free(marker);
    }

    free(event_id);
    free(relay);
    return TRUE;
}

/* ========== Core parsing ========== */

static CacheEntry *parse_event_json(const char *event_json) {
    /* Extract event ID */
    char *id = NULL;
    if ((id = gnostr_json_get_string(event_json, "id", NULL)) == NULL || !id || strlen(id) != 64) {
        free(id);
        return NULL;
    }

    /* Scan tags */
    ParseCtx ctx = {0};
    ctx.root_kind = -1;
    gnostr_json_array_foreach(event_json, "tags", tag_scan_cb, &ctx);

    CacheEntry *entry = g_new0(CacheEntry, 1);
    entry->event_id = g_strdup(id);
    entry->has_explicit_markers = ctx.has_explicit_markers;
    entry->root_kind = ctx.root_kind;

    /* Apply explicit markers */
    if (ctx.root_id) {
        entry->root_id = ctx.root_id;
        ctx.root_id = NULL;
    }
    if (ctx.reply_id) {
        entry->reply_id = ctx.reply_id;
        ctx.reply_id = NULL;
    }
    if (ctx.root_relay_hint) {
        entry->root_relay_hint = ctx.root_relay_hint;
        ctx.root_relay_hint = NULL;
    }
    if (ctx.reply_relay_hint) {
        entry->reply_relay_hint = ctx.reply_relay_hint;
        ctx.reply_relay_hint = NULL;
    }
    if (ctx.root_addr) {
        entry->root_addr = ctx.root_addr;
        ctx.root_addr = NULL;
    }
    if (ctx.root_addr_relay) {
        entry->root_addr_relay = ctx.root_addr_relay;
        ctx.root_addr_relay = NULL;
    }

    /* Positional fallback if no explicit markers */
    if (!entry->root_id && ctx.first_etag) {
        entry->root_id = g_strdup(ctx.first_etag);
        if (ctx.first_relay)
            entry->root_relay_hint = g_strdup(ctx.first_relay);
    }
    if (!entry->reply_id && ctx.etag_count >= 2 && ctx.last_etag) {
        entry->reply_id = g_strdup(ctx.last_etag);
        if (ctx.last_relay)
            entry->reply_relay_hint = g_strdup(ctx.last_relay);
    }

    /* Cleanup scan context */
    g_free(ctx.root_id);
    g_free(ctx.reply_id);
    g_free(ctx.root_relay_hint);
    g_free(ctx.reply_relay_hint);
    g_free(ctx.root_addr);
    g_free(ctx.root_addr_relay);
    g_free(ctx.first_etag);
    g_free(ctx.last_etag);
    g_free(ctx.first_relay);
    g_free(ctx.last_relay);

    free(id);
    return entry;
}

static void fill_info_from_entry(const CacheEntry *entry, GnostrNip10ThreadInfo *info) {
    info->root_id = entry->root_id;
    info->reply_id = entry->reply_id;
    info->root_relay_hint = entry->root_relay_hint;
    info->reply_relay_hint = entry->reply_relay_hint;
    info->root_addr = entry->root_addr;
    info->root_addr_relay = entry->root_addr_relay;
    info->root_kind = entry->root_kind;
    info->has_explicit_markers = entry->has_explicit_markers;
}

/* ========== Public API ========== */

gboolean gnostr_nip10_parse_thread(const char *event_json,
                                    GnostrNip10ThreadInfo *info) {
    g_return_val_if_fail(event_json != NULL, FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    memset(info, 0, sizeof(*info));

    g_mutex_lock(&cache_mutex);
    ensure_cache();

    /* Check cache first */
    char *id = NULL;
    if ((id = gnostr_json_get_string(event_json, "id", NULL)) != NULL && id && strlen(id) == 64) {
        CacheEntry *cached = g_hash_table_lookup(cache, id);
        if (cached) {
            fill_info_from_entry(cached, info);
            g_mutex_unlock(&cache_mutex);
            free(id);
            return TRUE;
        }
    }
    free(id);

    /* Parse and cache */
    CacheEntry *entry = parse_event_json(event_json);
    if (!entry) {
        g_mutex_unlock(&cache_mutex);
        return FALSE;
    }

    maybe_evict();
    g_hash_table_insert(cache, entry->event_id, entry);
    fill_info_from_entry(entry, info);

    g_mutex_unlock(&cache_mutex);
    return TRUE;
}

gboolean gnostr_nip10_lookup_cached(const char *event_id,
                                     GnostrNip10ThreadInfo *info) {
    g_return_val_if_fail(event_id != NULL, FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    memset(info, 0, sizeof(*info));

    g_mutex_lock(&cache_mutex);
    ensure_cache();

    CacheEntry *cached = g_hash_table_lookup(cache, event_id);
    if (cached) {
        fill_info_from_entry(cached, info);
        g_mutex_unlock(&cache_mutex);
        return TRUE;
    }

    g_mutex_unlock(&cache_mutex);
    return FALSE;
}

void gnostr_nip10_cache_clear(void) {
    g_mutex_lock(&cache_mutex);
    if (cache) {
        g_hash_table_remove_all(cache);
    }
    g_mutex_unlock(&cache_mutex);
}

guint gnostr_nip10_cache_size(void) {
    g_mutex_lock(&cache_mutex);
    ensure_cache();
    guint size = g_hash_table_size(cache);
    g_mutex_unlock(&cache_mutex);
    return size;
}

gboolean gnostr_nip10_is_thread_reply(const char *event_json) {
    GnostrNip10ThreadInfo info;
    if (!gnostr_nip10_parse_thread(event_json, &info))
        return FALSE;
    return (info.root_id != NULL || info.reply_id != NULL);
}

const char *gnostr_nip10_get_thread_root(const char *event_json) {
    GnostrNip10ThreadInfo info;
    if (!gnostr_nip10_parse_thread(event_json, &info))
        return NULL;
    return info.root_id;
}
