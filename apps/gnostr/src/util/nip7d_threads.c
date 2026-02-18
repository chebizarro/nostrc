/**
 * NIP-7D Forum Threads Implementation
 *
 * Provides parsing and creation utilities for NIP-7D threaded discussions.
 */

#include "nip7d_threads.h"
#include <string.h>
#include <time.h>
#include <glib/gi18n.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include <nostr-gobject-1.0/nostr_event.h>
#include "nostr-tag.h"

/* ============================================================================
 * Thread Structure Management
 * ============================================================================ */

GnostrThread *
gnostr_thread_new(void)
{
    GnostrThread *thread = g_new0(GnostrThread, 1);
    thread->hashtags = g_ptr_array_new_with_free_func(g_free);
    thread->mentions = g_ptr_array_new_with_free_func(g_free);
    return thread;
}

void
gnostr_thread_free(GnostrThread *thread)
{
    if (!thread) return;

    g_free(thread->event_id);
    g_free(thread->pubkey);
    g_free(thread->subject);
    g_free(thread->content);

    if (thread->hashtags)
        g_ptr_array_unref(thread->hashtags);
    if (thread->mentions)
        g_ptr_array_unref(thread->mentions);

    g_free(thread);
}

GnostrThread *
gnostr_thread_copy(const GnostrThread *thread)
{
    if (!thread) return NULL;

    GnostrThread *copy = gnostr_thread_new();
    copy->event_id = g_strdup(thread->event_id);
    copy->pubkey = g_strdup(thread->pubkey);
    copy->subject = g_strdup(thread->subject);
    copy->content = g_strdup(thread->content);
    copy->created_at = thread->created_at;
    copy->replies_count = thread->replies_count;
    copy->last_activity = thread->last_activity;

    /* Copy hashtags */
    if (thread->hashtags) {
        for (guint i = 0; i < thread->hashtags->len; i++) {
            const char *tag = g_ptr_array_index(thread->hashtags, i);
            g_ptr_array_add(copy->hashtags, g_strdup(tag));
        }
    }

    /* Copy mentions */
    if (thread->mentions) {
        for (guint i = 0; i < thread->mentions->len; i++) {
            const char *pk = g_ptr_array_index(thread->mentions, i);
            g_ptr_array_add(copy->mentions, g_strdup(pk));
        }
    }

    return copy;
}

GnostrThreadReply *
gnostr_thread_reply_new(void)
{
    return g_new0(GnostrThreadReply, 1);
}

void
gnostr_thread_reply_free(GnostrThreadReply *reply)
{
    if (!reply) return;

    g_free(reply->event_id);
    g_free(reply->pubkey);
    g_free(reply->content);
    g_free(reply->thread_root_id);
    g_free(reply->parent_id);
    g_free(reply);
}

GnostrThreadReply *
gnostr_thread_reply_copy(const GnostrThreadReply *reply)
{
    if (!reply) return NULL;

    GnostrThreadReply *copy = gnostr_thread_reply_new();
    copy->event_id = g_strdup(reply->event_id);
    copy->pubkey = g_strdup(reply->pubkey);
    copy->content = g_strdup(reply->content);
    copy->created_at = reply->created_at;
    copy->thread_root_id = g_strdup(reply->thread_root_id);
    copy->parent_id = g_strdup(reply->parent_id);
    copy->depth = reply->depth;

    return copy;
}

GnostrThreadTreeNode *
gnostr_thread_tree_node_new(GnostrThreadReply *reply)
{
    GnostrThreadTreeNode *node = g_new0(GnostrThreadTreeNode, 1);
    node->reply = reply;
    node->children = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_thread_tree_node_free);
    node->parent = NULL;
    return node;
}

void
gnostr_thread_tree_node_free(GnostrThreadTreeNode *node)
{
    if (!node) return;

    /* Don't free reply here - it's owned externally */
    if (node->children)
        g_ptr_array_unref(node->children);

    g_free(node);
}

/* ============================================================================
 * Parsing Functions
 * ============================================================================ */

/* Callback context for parsing subject */
typedef struct {
    char *subject;
} ParseSubjectCtx;

static gboolean parse_subject_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    ParseSubjectCtx *ctx = (ParseSubjectCtx *)user_data;

    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }

    if (strcmp(tag_name, "subject") != 0) {
        free(tag_name);
        return true;
    }
    free(tag_name);

    char *value = NULL;
    if ((value = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL && value && *value) {
        ctx->subject = g_strdup(value);
        free(value);
        return false;  /* Stop iteration */
    }
    free(value);
    return true;
}

char *
gnostr_thread_parse_subject(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!gnostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    ParseSubjectCtx ctx = { .subject = NULL };
    gnostr_json_array_foreach_root(tags_json, parse_subject_cb, &ctx);
    return ctx.subject;
}

/* Callback context for parsing hashtags */
typedef struct {
    GPtrArray *hashtags;
} ParseHashtagsCtx;

static gboolean parse_hashtags_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    ParseHashtagsCtx *ctx = (ParseHashtagsCtx *)user_data;

    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }

    if (strcmp(tag_name, "t") != 0) {
        free(tag_name);
        return true;
    }
    free(tag_name);

    char *value = NULL;
    if ((value = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL && value && *value) {
        g_ptr_array_add(ctx->hashtags, g_strdup(value));
    }
    free(value);
    return true;  /* Continue to find all hashtags */
}

GPtrArray *
gnostr_thread_parse_hashtags(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!gnostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    GPtrArray *hashtags = g_ptr_array_new_with_free_func(g_free);
    ParseHashtagsCtx ctx = { .hashtags = hashtags };
    gnostr_json_array_foreach_root(tags_json, parse_hashtags_cb, &ctx);
    return hashtags;
}

/* Callback context for extracting root ID */
typedef struct {
    char *root_id;
    char *first_e_id;
    gboolean found_explicit;
} ExtractRootIdCtx;

static gboolean extract_root_id_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    ExtractRootIdCtx *ctx = (ExtractRootIdCtx *)user_data;

    if (ctx->found_explicit) return false;  /* Stop if we found explicit root */

    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }

    /* Check for "E" tag (NIP-22 uppercase for root event) */
    if (strcmp(tag_name, "E") == 0) {
        char *event_id = NULL;
        if ((event_id = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL &&
            event_id && strlen(event_id) == 64) {
            ctx->root_id = g_strdup(event_id);
            ctx->found_explicit = TRUE;
        }
        free(event_id);
        free(tag_name);
        return !ctx->found_explicit;
    }

    /* Check for "e" tag with "root" marker */
    if (strcmp(tag_name, "e") == 0) {
        char *event_id = NULL;
        if ((event_id = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) == NULL ||
            !event_id || strlen(event_id) != 64) {
            free(event_id);
            free(tag_name);
            return true;
        }

        /* Check for NIP-10 marker at index 3 */
        if (tag_len >= 4) {
            char *marker = NULL;
            if ((marker = gnostr_json_get_array_string(element_json, NULL, 3, NULL)) != NULL &&
                marker && strcmp(marker, "root") == 0) {
                ctx->root_id = g_strdup(event_id);
                ctx->found_explicit = TRUE;
            }
            free(marker);
        }

        /* Track first "e" tag for fallback */
        if (!ctx->first_e_id) {
            ctx->first_e_id = g_strdup(event_id);
        }

        free(event_id);
    }

    free(tag_name);
    return !ctx->found_explicit;
}

char *
gnostr_thread_reply_extract_root_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!gnostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    ExtractRootIdCtx ctx = { .root_id = NULL, .first_e_id = NULL, .found_explicit = FALSE };
    gnostr_json_array_foreach_root(tags_json, extract_root_id_cb, &ctx);

    /* Fallback to first "e" tag if no explicit root marker */
    if (!ctx.root_id && ctx.first_e_id) {
        ctx.root_id = ctx.first_e_id;
        ctx.first_e_id = NULL;
    }

    g_free(ctx.first_e_id);
    return ctx.root_id;
}

/* Callback context for extracting parent ID */
typedef struct {
    char *parent_id;
    char *last_e_id;
    gboolean found_explicit;
} ExtractParentIdCtx;

static gboolean extract_parent_id_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    ExtractParentIdCtx *ctx = (ExtractParentIdCtx *)user_data;

    if (ctx->found_explicit) return false;  /* Stop if found explicit reply marker */

    if (!gnostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
    if (tag_len < 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return true;
    }

    if (strcmp(tag_name, "e") != 0) {
        free(tag_name);
        return true;
    }
    free(tag_name);

    char *event_id = NULL;
    if ((event_id = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) == NULL ||
        !event_id || strlen(event_id) != 64) {
        free(event_id);
        return true;
    }

    /* Check for NIP-10 marker at index 3 */
    if (tag_len >= 4) {
        char *marker = NULL;
        if ((marker = gnostr_json_get_array_string(element_json, NULL, 3, NULL)) != NULL &&
            marker && strcmp(marker, "reply") == 0) {
            ctx->parent_id = g_strdup(event_id);
            ctx->found_explicit = TRUE;
        }
        free(marker);
    }

    /* Track last "e" tag for positional fallback */
    g_free(ctx->last_e_id);
    ctx->last_e_id = g_strdup(event_id);

    free(event_id);
    return !ctx->found_explicit;
}

char *
gnostr_thread_reply_extract_parent_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!gnostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    ExtractParentIdCtx ctx = { .parent_id = NULL, .last_e_id = NULL, .found_explicit = FALSE };
    gnostr_json_array_foreach_root(tags_json, extract_parent_id_cb, &ctx);

    /* Fallback to last "e" tag (NIP-10 positional) */
    if (!ctx.parent_id && ctx.last_e_id) {
        ctx.parent_id = ctx.last_e_id;
        ctx.last_e_id = NULL;
    }

    g_free(ctx.last_e_id);
    return ctx.parent_id;
}

GnostrThread *
gnostr_thread_parse_from_json(const char *json_str)
{
    if (!json_str) return NULL;

    /* Parse event using GNostrEvent API */
    g_autoptr(GNostrEvent) event = gnostr_event_new_from_json(json_str, NULL);
    if (!event) return NULL;

    /* Verify it's a kind-11 event */
    guint kind = gnostr_event_get_kind(event);
    if (kind != NIP7D_KIND_THREAD_ROOT) {
        return NULL;
    }

    GnostrThread *thread = gnostr_thread_new();

    /* Extract basic fields using GNostrEvent API */
    const gchar *id = gnostr_event_get_id(event);
    if (id)
        thread->event_id = g_strdup(id);

    const gchar *pubkey = gnostr_event_get_pubkey(event);
    if (pubkey)
        thread->pubkey = g_strdup(pubkey);

    const gchar *content = gnostr_event_get_content(event);
    if (content)
        thread->content = g_strdup(content);

    thread->created_at = gnostr_event_get_created_at(event);

    /* Parse tags using NostrTags API (gnostr_event_get_tags returns NostrTags*) */
    NostrTags *tags = (NostrTags *)gnostr_event_get_tags(event);
    if (tags) {
        /* Serialize tags for the helper functions that expect JSON */
        char *tags_str = nostr_tags_to_json(tags);
        if (tags_str) {
            thread->subject = gnostr_thread_parse_subject(tags_str);

            GPtrArray *hashtags = gnostr_thread_parse_hashtags(tags_str);
            if (hashtags) {
                g_ptr_array_unref(thread->hashtags);
                thread->hashtags = hashtags;
            }
            free(tags_str);
        }

        /* Extract mentions ("p" tags) using NostrTags API */
        size_t tag_count = nostr_tags_size(tags);
        for (size_t i = 0; i < tag_count; i++) {
            NostrTag *tag = nostr_tags_get(tags, i);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *tag_name = nostr_tag_get(tag, 0);
            if (!tag_name || strcmp(tag_name, "p") != 0) continue;

            const char *pk = nostr_tag_get(tag, 1);
            if (pk && strlen(pk) == 64) {
                g_ptr_array_add(thread->mentions, g_strdup(pk));
            }
        }
    }

    /* Set default values */
    thread->last_activity = thread->created_at;
    thread->replies_count = 0;

    return thread;
}

GnostrThreadReply *
gnostr_thread_reply_parse_from_json(const char *json_str)
{
    if (!json_str) return NULL;

    /* Parse event using GNostrEvent API */
    g_autoptr(GNostrEvent) event = gnostr_event_new_from_json(json_str, NULL);
    if (!event) return NULL;

    /* Verify it's a kind-1111 event */
    guint kind = gnostr_event_get_kind(event);
    if (kind != NIP7D_KIND_THREAD_REPLY) {
        return NULL;
    }

    GnostrThreadReply *reply = gnostr_thread_reply_new();

    /* Extract basic fields using GNostrEvent API */
    const gchar *id = gnostr_event_get_id(event);
    if (id)
        reply->event_id = g_strdup(id);

    const gchar *pubkey = gnostr_event_get_pubkey(event);
    if (pubkey)
        reply->pubkey = g_strdup(pubkey);

    const gchar *content = gnostr_event_get_content(event);
    if (content)
        reply->content = g_strdup(content);

    reply->created_at = gnostr_event_get_created_at(event);

    /* Parse tags for root and parent references */
    NostrTags *tags = (NostrTags *)gnostr_event_get_tags(event);
    if (tags) {
        char *tags_str = nostr_tags_to_json(tags);
        if (tags_str) {
            reply->thread_root_id = gnostr_thread_reply_extract_root_id(tags_str);
            reply->parent_id = gnostr_thread_reply_extract_parent_id(tags_str);
            free(tags_str);
        }
    }

    reply->depth = 0; /* Will be calculated later */

    return reply;
}

/* ============================================================================
 * Event Creation Functions
 * ============================================================================ */

char *
gnostr_thread_create_tags(const char *subject, const char * const *hashtags)
{
    g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
    gnostr_json_builder_begin_array(builder);

    /* Add subject tag */
    if (subject && *subject) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "subject");
        gnostr_json_builder_add_string(builder, subject);
        gnostr_json_builder_end_array(builder);
    }

    /* Add hashtag tags */
    if (hashtags) {
        for (int i = 0; hashtags[i] != NULL; i++) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "t");
            gnostr_json_builder_add_string(builder, hashtags[i]);
            gnostr_json_builder_end_array(builder);
        }
    }

    gnostr_json_builder_end_array(builder);
    char *result = gnostr_json_builder_finish(builder);
    return result;
}

char *
gnostr_thread_reply_create_tags(const char *thread_root_id,
                                 const char *parent_id,
                                 const char * const *author_pubkeys,
                                 const char *recommended_relay)
{
    if (!thread_root_id) return NULL;

    g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
    gnostr_json_builder_begin_array(builder);

    /* Add "K" tag indicating the root event kind (NIP-22) */
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "K");
    gnostr_json_builder_add_string(builder, "11");
    gnostr_json_builder_end_array(builder);

    /* Add "E" tag for root event reference (NIP-22 uppercase) */
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "E");
    gnostr_json_builder_add_string(builder, thread_root_id);
    if (recommended_relay)
        gnostr_json_builder_add_string(builder, recommended_relay);
    gnostr_json_builder_end_array(builder);

    /* Also add lowercase "e" tag with root marker for NIP-10 compatibility */
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, thread_root_id);
    gnostr_json_builder_add_string(builder, recommended_relay ? recommended_relay : "");
    gnostr_json_builder_add_string(builder, "root");
    gnostr_json_builder_end_array(builder);

    /* Add parent reference if this is a nested reply */
    if (parent_id && strcmp(parent_id, thread_root_id) != 0) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "e");
        gnostr_json_builder_add_string(builder, parent_id);
        gnostr_json_builder_add_string(builder, recommended_relay ? recommended_relay : "");
        gnostr_json_builder_add_string(builder, "reply");
        gnostr_json_builder_end_array(builder);
    }

    /* Add "p" tags for author mentions */
    if (author_pubkeys) {
        for (int i = 0; author_pubkeys[i] != NULL; i++) {
            gnostr_json_builder_begin_array(builder);
            gnostr_json_builder_add_string(builder, "p");
            gnostr_json_builder_add_string(builder, author_pubkeys[i]);
            gnostr_json_builder_end_array(builder);
        }
    }

    gnostr_json_builder_end_array(builder);
    char *result = gnostr_json_builder_finish(builder);
    return result;
}

/* ============================================================================
 * Reply Tree Building
 * ============================================================================ */

void
gnostr_thread_calculate_depths(GPtrArray *replies, const char *thread_root_id)
{
    if (!replies || !thread_root_id) return;

    /* Build a map of event_id -> reply for quick lookup */
    GHashTable *replies_by_id = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (reply && reply->event_id) {
            g_hash_table_insert(replies_by_id, reply->event_id, reply);
        }
    }

    /* Calculate depth for each reply */
    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (!reply) continue;

        guint depth = 0;
        const char *parent = reply->parent_id;

        /* Walk up the parent chain */
        while (parent && depth < 100) { /* Prevent infinite loops */
            /* Check if parent is the thread root */
            if (strcmp(parent, thread_root_id) == 0) {
                break;
            }

            /* Find parent reply */
            GnostrThreadReply *parent_reply = g_hash_table_lookup(replies_by_id, parent);
            if (!parent_reply) {
                /* Parent not in our reply set - must be direct reply to root */
                break;
            }

            depth++;
            parent = parent_reply->parent_id;
        }

        reply->depth = depth;
    }

    g_hash_table_unref(replies_by_id);
}

static gint
compare_replies_by_time(gconstpointer a, gconstpointer b)
{
    const GnostrThreadReply *reply_a = *(const GnostrThreadReply **)a;
    const GnostrThreadReply *reply_b = *(const GnostrThreadReply **)b;

    if (reply_a->created_at < reply_b->created_at) return -1;
    if (reply_a->created_at > reply_b->created_at) return 1;
    return 0;
}

void
gnostr_thread_sort_replies_chronological(GPtrArray *replies)
{
    if (!replies || replies->len < 2) return;
    g_ptr_array_sort(replies, compare_replies_by_time);
}

/* Helper for threaded sorting */
typedef struct {
    GnostrThreadReply *reply;
    gint64 sort_key;  /* Combined timestamp for sorting */
} ThreadedSortItem;

static void
build_threaded_order_recursive(GHashTable *children_map,
                                const char *parent_id,
                                GPtrArray *result,
                                gint64 parent_time)
{
    GPtrArray *children = g_hash_table_lookup(children_map, parent_id);
    if (!children) return;

    /* Sort children by timestamp */
    g_ptr_array_sort(children, compare_replies_by_time);

    for (guint i = 0; i < children->len; i++) {
        GnostrThreadReply *child = g_ptr_array_index(children, i);
        g_ptr_array_add(result, child);

        /* Recursively add children of this child */
        if (child->event_id) {
            build_threaded_order_recursive(children_map, child->event_id, result, child->created_at);
        }
    }
}

void
gnostr_thread_sort_replies_threaded(GPtrArray *replies, const char *thread_root_id)
{
    if (!replies || replies->len < 2 || !thread_root_id) return;

    /* Build parent -> children map */
    GHashTable *children_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                                      (GDestroyNotify)g_ptr_array_unref);

    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (!reply) continue;

        /* Determine effective parent */
        const char *parent = reply->parent_id;
        if (!parent || !*parent) {
            parent = thread_root_id;
        }

        /* Get or create children array for this parent */
        GPtrArray *children = g_hash_table_lookup(children_map, parent);
        if (!children) {
            children = g_ptr_array_new();
            g_hash_table_insert(children_map, (gpointer)parent, children);
        }
        g_ptr_array_add(children, reply);
    }

    /* Build sorted result */
    GPtrArray *sorted = g_ptr_array_new();
    build_threaded_order_recursive(children_map, thread_root_id, sorted, 0);

    /* Copy sorted order back to original array */
    g_ptr_array_set_size(replies, 0);
    for (guint i = 0; i < sorted->len; i++) {
        g_ptr_array_add(replies, g_ptr_array_index(sorted, i));
    }

    g_ptr_array_unref(sorted);
    g_hash_table_unref(children_map);
}

GnostrThreadTreeNode *
gnostr_thread_build_reply_tree(GPtrArray *replies)
{
    if (!replies) return NULL;

    /* Create virtual root node */
    GnostrThreadTreeNode *root = gnostr_thread_tree_node_new(NULL);

    /* Build map of event_id -> node */
    GHashTable *nodes_by_id = g_hash_table_new(g_str_hash, g_str_equal);

    /* First pass: create nodes for all replies */
    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (!reply || !reply->event_id) continue;

        GnostrThreadTreeNode *node = gnostr_thread_tree_node_new(reply);
        g_hash_table_insert(nodes_by_id, reply->event_id, node);
    }

    /* Second pass: link parents and children */
    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (!reply || !reply->event_id) continue;

        GnostrThreadTreeNode *node = g_hash_table_lookup(nodes_by_id, reply->event_id);
        if (!node) continue;

        /* Find parent node */
        GnostrThreadTreeNode *parent_node = NULL;
        if (reply->parent_id) {
            parent_node = g_hash_table_lookup(nodes_by_id, reply->parent_id);
        }

        /* If no parent found, attach to root */
        if (!parent_node) {
            parent_node = root;
        }

        node->parent = parent_node;
        g_ptr_array_add(parent_node->children, node);
    }

    /* Note: nodes are now owned by the tree - don't free them separately */
    g_hash_table_unref(nodes_by_id);

    return root;
}

guint
gnostr_thread_count_replies(GnostrThreadTreeNode *root)
{
    if (!root) return 0;

    guint count = 0;

    /* Count this node if it has a reply */
    if (root->reply) {
        count = 1;
    }

    /* Count children recursively */
    if (root->children) {
        for (guint i = 0; i < root->children->len; i++) {
            GnostrThreadTreeNode *child = g_ptr_array_index(root->children, i);
            count += gnostr_thread_count_replies(child);
        }
    }

    return count;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

gboolean
gnostr_thread_is_thread_event(gint kind)
{
    return kind == NIP7D_KIND_THREAD_ROOT || kind == NIP7D_KIND_THREAD_REPLY;
}

char *
gnostr_thread_format_timestamp(gint64 created_at)
{
    if (created_at <= 0) return g_strdup(_("Unknown"));

    gint64 now = (gint64)time(NULL);
    gint64 diff = now - created_at;

    if (diff < 0) {
        return g_strdup(_("Just now"));
    } else if (diff < 60) {
        return g_strdup(_("Just now"));
    } else if (diff < 3600) {
        gint minutes = (gint)(diff / 60);
        return g_strdup_printf(g_dngettext(NULL, "%d minute ago", "%d minutes ago", minutes), minutes);
    } else if (diff < 86400) {
        gint hours = (gint)(diff / 3600);
        return g_strdup_printf(g_dngettext(NULL, "%d hour ago", "%d hours ago", hours), hours);
    } else if (diff < 604800) {
        gint days = (gint)(diff / 86400);
        return g_strdup_printf(g_dngettext(NULL, "%d day ago", "%d days ago", days), days);
    } else if (diff < 2592000) {
        gint weeks = (gint)(diff / 604800);
        return g_strdup_printf(g_dngettext(NULL, "%d week ago", "%d weeks ago", weeks), weeks);
    } else if (diff < 31536000) {
        gint months = (gint)(diff / 2592000);
        return g_strdup_printf(g_dngettext(NULL, "%d month ago", "%d months ago", months), months);
    } else {
        gint years = (gint)(diff / 31536000);
        return g_strdup_printf(g_dngettext(NULL, "%d year ago", "%d years ago", years), years);
    }
}

char *
gnostr_thread_format_reply_count(guint count)
{
    if (count == 0) {
        return g_strdup(_("No replies"));
    } else {
        return g_strdup_printf(g_dngettext(NULL, "%u reply", "%u replies", count), count);
    }
}
