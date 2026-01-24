/**
 * NIP-7D Forum Threads Implementation
 *
 * Provides parsing and creation utilities for NIP-7D threaded discussions.
 */

#include "nip7d_threads.h"
#include <jansson.h>
#include <string.h>
#include <time.h>
#include <glib/gi18n.h>

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

char *
gnostr_thread_parse_subject(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    char *subject = NULL;
    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name || strcmp(tag_name, "subject") != 0)
            continue;

        const char *value = json_string_value(json_array_get(tag, 1));
        if (value && *value) {
            subject = g_strdup(value);
            break;
        }
    }

    json_decref(tags);
    return subject;
}

GPtrArray *
gnostr_thread_parse_hashtags(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    GPtrArray *hashtags = g_ptr_array_new_with_free_func(g_free);
    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name || strcmp(tag_name, "t") != 0)
            continue;

        const char *value = json_string_value(json_array_get(tag, 1));
        if (value && *value) {
            g_ptr_array_add(hashtags, g_strdup(value));
        }
    }

    json_decref(tags);
    return hashtags;
}

char *
gnostr_thread_reply_extract_root_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    char *root_id = NULL;
    char *first_e_id = NULL;
    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name)
            continue;

        /* Check for "E" tag (NIP-22 uppercase for root event) */
        if (strcmp(tag_name, "E") == 0) {
            const char *event_id = json_string_value(json_array_get(tag, 1));
            if (event_id && strlen(event_id) == 64) {
                root_id = g_strdup(event_id);
                break;
            }
        }

        /* Check for "e" tag with "root" marker */
        if (strcmp(tag_name, "e") == 0) {
            const char *event_id = json_string_value(json_array_get(tag, 1));
            if (!event_id || strlen(event_id) != 64)
                continue;

            /* Check for NIP-10 marker */
            if (json_array_size(tag) >= 4) {
                const char *marker = json_string_value(json_array_get(tag, 3));
                if (marker && strcmp(marker, "root") == 0) {
                    root_id = g_strdup(event_id);
                    break;
                }
            }

            /* Track first "e" tag for fallback */
            if (!first_e_id) {
                first_e_id = g_strdup(event_id);
            }
        }
    }

    /* Fallback to first "e" tag if no explicit root marker */
    if (!root_id && first_e_id) {
        root_id = first_e_id;
        first_e_id = NULL;
    }

    g_free(first_e_id);
    json_decref(tags);
    return root_id;
}

char *
gnostr_thread_reply_extract_parent_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    char *parent_id = NULL;
    char *last_e_id = NULL;
    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name || strcmp(tag_name, "e") != 0)
            continue;

        const char *event_id = json_string_value(json_array_get(tag, 1));
        if (!event_id || strlen(event_id) != 64)
            continue;

        /* Check for NIP-10 marker */
        if (json_array_size(tag) >= 4) {
            const char *marker = json_string_value(json_array_get(tag, 3));
            if (marker && strcmp(marker, "reply") == 0) {
                parent_id = g_strdup(event_id);
                break;
            }
        }

        /* Track last "e" tag for positional fallback */
        g_free(last_e_id);
        last_e_id = g_strdup(event_id);
    }

    /* Fallback to last "e" tag (NIP-10 positional) */
    if (!parent_id && last_e_id) {
        parent_id = last_e_id;
        last_e_id = NULL;
    }

    g_free(last_e_id);
    json_decref(tags);
    return parent_id;
}

GnostrThread *
gnostr_thread_parse_from_json(const char *json_str)
{
    if (!json_str) return NULL;

    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) return NULL;

    /* Verify it's a kind-11 event */
    json_t *kind_json = json_object_get(root, "kind");
    if (!kind_json || json_integer_value(kind_json) != NIP7D_KIND_THREAD_ROOT) {
        json_decref(root);
        return NULL;
    }

    GnostrThread *thread = gnostr_thread_new();

    /* Extract basic fields */
    json_t *id = json_object_get(root, "id");
    json_t *pubkey = json_object_get(root, "pubkey");
    json_t *content = json_object_get(root, "content");
    json_t *created_at = json_object_get(root, "created_at");

    if (json_is_string(id))
        thread->event_id = g_strdup(json_string_value(id));
    if (json_is_string(pubkey))
        thread->pubkey = g_strdup(json_string_value(pubkey));
    if (json_is_string(content))
        thread->content = g_strdup(json_string_value(content));
    if (json_is_integer(created_at))
        thread->created_at = json_integer_value(created_at);

    /* Parse tags */
    json_t *tags = json_object_get(root, "tags");
    if (tags && json_is_array(tags)) {
        char *tags_str = json_dumps(tags, JSON_COMPACT);
        if (tags_str) {
            thread->subject = gnostr_thread_parse_subject(tags_str);

            GPtrArray *hashtags = gnostr_thread_parse_hashtags(tags_str);
            if (hashtags) {
                g_ptr_array_unref(thread->hashtags);
                thread->hashtags = hashtags;
            }

            /* Extract mentions ("p" tags) */
            size_t index;
            json_t *tag;
            json_array_foreach(tags, index, tag) {
                if (!json_is_array(tag) || json_array_size(tag) < 2)
                    continue;

                const char *tag_name = json_string_value(json_array_get(tag, 0));
                if (!tag_name || strcmp(tag_name, "p") != 0)
                    continue;

                const char *pk = json_string_value(json_array_get(tag, 1));
                if (pk && strlen(pk) == 64) {
                    g_ptr_array_add(thread->mentions, g_strdup(pk));
                }
            }

            free(tags_str);
        }
    }

    /* Set default values */
    thread->last_activity = thread->created_at;
    thread->replies_count = 0;

    json_decref(root);
    return thread;
}

GnostrThreadReply *
gnostr_thread_reply_parse_from_json(const char *json_str)
{
    if (!json_str) return NULL;

    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) return NULL;

    /* Verify it's a kind-1111 event */
    json_t *kind_json = json_object_get(root, "kind");
    if (!kind_json || json_integer_value(kind_json) != NIP7D_KIND_THREAD_REPLY) {
        json_decref(root);
        return NULL;
    }

    GnostrThreadReply *reply = gnostr_thread_reply_new();

    /* Extract basic fields */
    json_t *id = json_object_get(root, "id");
    json_t *pubkey = json_object_get(root, "pubkey");
    json_t *content = json_object_get(root, "content");
    json_t *created_at = json_object_get(root, "created_at");

    if (json_is_string(id))
        reply->event_id = g_strdup(json_string_value(id));
    if (json_is_string(pubkey))
        reply->pubkey = g_strdup(json_string_value(pubkey));
    if (json_is_string(content))
        reply->content = g_strdup(json_string_value(content));
    if (json_is_integer(created_at))
        reply->created_at = json_integer_value(created_at);

    /* Parse tags for root and parent references */
    json_t *tags = json_object_get(root, "tags");
    if (tags && json_is_array(tags)) {
        char *tags_str = json_dumps(tags, JSON_COMPACT);
        if (tags_str) {
            reply->thread_root_id = gnostr_thread_reply_extract_root_id(tags_str);
            reply->parent_id = gnostr_thread_reply_extract_parent_id(tags_str);
            free(tags_str);
        }
    }

    reply->depth = 0; /* Will be calculated later */

    json_decref(root);
    return reply;
}

/* ============================================================================
 * Event Creation Functions
 * ============================================================================ */

char *
gnostr_thread_create_tags(const char *subject, const char * const *hashtags)
{
    json_t *tags = json_array();

    /* Add subject tag */
    if (subject && *subject) {
        json_t *subject_tag = json_array();
        json_array_append_new(subject_tag, json_string("subject"));
        json_array_append_new(subject_tag, json_string(subject));
        json_array_append_new(tags, subject_tag);
    }

    /* Add hashtag tags */
    if (hashtags) {
        for (int i = 0; hashtags[i] != NULL; i++) {
            json_t *t_tag = json_array();
            json_array_append_new(t_tag, json_string("t"));
            json_array_append_new(t_tag, json_string(hashtags[i]));
            json_array_append_new(tags, t_tag);
        }
    }

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);
    return result;
}

char *
gnostr_thread_reply_create_tags(const char *thread_root_id,
                                 const char *parent_id,
                                 const char * const *author_pubkeys,
                                 const char *recommended_relay)
{
    if (!thread_root_id) return NULL;

    json_t *tags = json_array();

    /* Add "K" tag indicating the root event kind (NIP-22) */
    json_t *k_tag = json_array();
    json_array_append_new(k_tag, json_string("K"));
    json_array_append_new(k_tag, json_string("11"));
    json_array_append_new(tags, k_tag);

    /* Add "E" tag for root event reference (NIP-22 uppercase) */
    json_t *e_root_tag = json_array();
    json_array_append_new(e_root_tag, json_string("E"));
    json_array_append_new(e_root_tag, json_string(thread_root_id));
    if (recommended_relay)
        json_array_append_new(e_root_tag, json_string(recommended_relay));
    json_array_append_new(tags, e_root_tag);

    /* Also add lowercase "e" tag with root marker for NIP-10 compatibility */
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(thread_root_id));
    json_array_append_new(e_tag, json_string(recommended_relay ? recommended_relay : ""));
    json_array_append_new(e_tag, json_string("root"));
    json_array_append_new(tags, e_tag);

    /* Add parent reference if this is a nested reply */
    if (parent_id && strcmp(parent_id, thread_root_id) != 0) {
        json_t *reply_tag = json_array();
        json_array_append_new(reply_tag, json_string("e"));
        json_array_append_new(reply_tag, json_string(parent_id));
        json_array_append_new(reply_tag, json_string(recommended_relay ? recommended_relay : ""));
        json_array_append_new(reply_tag, json_string("reply"));
        json_array_append_new(tags, reply_tag);
    }

    /* Add "p" tags for author mentions */
    if (author_pubkeys) {
        for (int i = 0; author_pubkeys[i] != NULL; i++) {
            json_t *p_tag = json_array();
            json_array_append_new(p_tag, json_string("p"));
            json_array_append_new(p_tag, json_string(author_pubkeys[i]));
            json_array_append_new(tags, p_tag);
        }
    }

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);
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
