/**
 * NIP-22 Comment Implementation
 */

#include "nip22_comments.h"
#include <json-glib/json-glib.h>
#include <string.h>

GnostrComment *
gnostr_comment_new(void)
{
    GnostrComment *comment = g_new0(GnostrComment, 1);
    comment->root_kind = -1; /* Indicate unset */
    comment->mentions = NULL;
    comment->mention_count = 0;
    return comment;
}

void
gnostr_comment_free(GnostrComment *comment)
{
    if (!comment) return;

    g_free(comment->content);
    g_free(comment->root_id);
    g_free(comment->root_relay);
    g_free(comment->reply_id);
    g_free(comment->reply_relay);
    g_free(comment->root_addr);
    g_free(comment->root_addr_relay);
    g_free(comment->event_id);
    g_free(comment->author_pubkey);

    if (comment->mentions) {
        for (gsize i = 0; i < comment->mention_count; i++) {
            g_free(comment->mentions[i]);
        }
        g_free(comment->mentions);
    }

    g_free(comment);
}

GnostrComment *
gnostr_comment_copy(const GnostrComment *comment)
{
    if (!comment) return NULL;

    GnostrComment *copy = gnostr_comment_new();

    copy->content = g_strdup(comment->content);
    copy->root_id = g_strdup(comment->root_id);
    copy->root_relay = g_strdup(comment->root_relay);
    copy->root_kind = comment->root_kind;
    copy->reply_id = g_strdup(comment->reply_id);
    copy->reply_relay = g_strdup(comment->reply_relay);
    copy->root_addr = g_strdup(comment->root_addr);
    copy->root_addr_relay = g_strdup(comment->root_addr_relay);
    copy->created_at = comment->created_at;
    copy->event_id = g_strdup(comment->event_id);
    copy->author_pubkey = g_strdup(comment->author_pubkey);

    if (comment->mentions && comment->mention_count > 0) {
        copy->mentions = g_new0(gchar*, comment->mention_count + 1);
        for (gsize i = 0; i < comment->mention_count; i++) {
            copy->mentions[i] = g_strdup(comment->mentions[i]);
        }
        copy->mention_count = comment->mention_count;
    }

    return copy;
}

GnostrComment *
gnostr_comment_parse(const gchar *tags_json, const gchar *content)
{
    if (!tags_json || !*tags_json) return NULL;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
        g_warning("NIP-22: Failed to parse tags JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_warning("NIP-22: Tags is not an array");
        g_object_unref(parser);
        return NULL;
    }

    JsonArray *tags = json_node_get_array(root);
    guint n_tags = json_array_get_length(tags);

    GnostrComment *comment = gnostr_comment_new();
    comment->content = g_strdup(content);

    GPtrArray *mentions_arr = g_ptr_array_new();

    for (guint i = 0; i < n_tags; i++) {
        JsonNode *tag_node = json_array_get_element(tags, i);
        if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

        JsonArray *tag = json_node_get_array(tag_node);
        guint tag_len = json_array_get_length(tag);
        if (tag_len < 2) continue;

        const gchar *tag_name = json_array_get_string_element(tag, 0);
        const gchar *tag_value = json_array_get_string_element(tag, 1);

        if (!tag_name || !tag_value) continue;

        if (g_strcmp0(tag_name, "e") == 0) {
            /* Handle e-tag with optional relay and marker */
            const gchar *relay = NULL;
            const gchar *marker = NULL;

            if (tag_len >= 3) {
                relay = json_array_get_string_element(tag, 2);
            }
            if (tag_len >= 4) {
                marker = json_array_get_string_element(tag, 3);
            }

            if (marker && g_strcmp0(marker, "root") == 0) {
                /* Root event reference */
                g_free(comment->root_id);
                comment->root_id = g_strdup(tag_value);
                g_free(comment->root_relay);
                comment->root_relay = (relay && *relay) ? g_strdup(relay) : NULL;
            } else if (marker && g_strcmp0(marker, "reply") == 0) {
                /* Direct parent comment reference */
                g_free(comment->reply_id);
                comment->reply_id = g_strdup(tag_value);
                g_free(comment->reply_relay);
                comment->reply_relay = (relay && *relay) ? g_strdup(relay) : NULL;
            } else if (!comment->root_id) {
                /* Fallback: first e-tag without marker is root */
                comment->root_id = g_strdup(tag_value);
                comment->root_relay = (relay && *relay) ? g_strdup(relay) : NULL;
            }
        } else if (g_strcmp0(tag_name, "p") == 0) {
            /* Pubkey mention */
            if (mentions_arr->len < NIP22_MAX_MENTIONS) {
                /* Check for duplicates */
                gboolean duplicate = FALSE;
                for (guint j = 0; j < mentions_arr->len; j++) {
                    if (g_strcmp0(g_ptr_array_index(mentions_arr, j), tag_value) == 0) {
                        duplicate = TRUE;
                        break;
                    }
                }
                if (!duplicate) {
                    g_ptr_array_add(mentions_arr, g_strdup(tag_value));
                }
            }
        } else if (g_strcmp0(tag_name, "k") == 0) {
            /* Root event kind */
            gchar *endptr;
            gint64 kind = g_ascii_strtoll(tag_value, &endptr, 10);
            if (endptr != tag_value && *endptr == '\0' && kind >= 0 && kind <= 65535) {
                comment->root_kind = (gint)kind;
            }
        } else if (g_strcmp0(tag_name, "a") == 0) {
            /* Addressable event reference */
            g_free(comment->root_addr);
            comment->root_addr = g_strdup(tag_value);

            if (tag_len >= 3) {
                const gchar *relay = json_array_get_string_element(tag, 2);
                g_free(comment->root_addr_relay);
                comment->root_addr_relay = (relay && *relay) ? g_strdup(relay) : NULL;
            }
        }
    }

    /* Convert mentions array */
    comment->mention_count = mentions_arr->len;
    if (mentions_arr->len > 0) {
        comment->mentions = g_new0(gchar*, mentions_arr->len + 1);
        for (guint i = 0; i < mentions_arr->len; i++) {
            comment->mentions[i] = g_ptr_array_index(mentions_arr, i);
        }
        comment->mentions[mentions_arr->len] = NULL;
    }
    g_ptr_array_free(mentions_arr, FALSE);

    g_object_unref(parser);
    return comment;
}

gchar *
gnostr_comment_build_tags(const GnostrComment *comment)
{
    if (!comment) return NULL;

    /* NIP-22 requires at least a root reference */
    if (!comment->root_id && !comment->root_addr) {
        g_warning("NIP-22: Comment must have root_id or root_addr");
        return NULL;
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);

    /* Root event e-tag: ["e", "<event-id>", "<relay>", "root"] */
    if (comment->root_id) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "e");
        json_builder_add_string_value(builder, comment->root_id);
        json_builder_add_string_value(builder, comment->root_relay ? comment->root_relay : "");
        json_builder_add_string_value(builder, "root");
        json_builder_end_array(builder);
    }

    /* Reply e-tag: ["e", "<event-id>", "<relay>", "reply"] */
    if (comment->reply_id) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "e");
        json_builder_add_string_value(builder, comment->reply_id);
        json_builder_add_string_value(builder, comment->reply_relay ? comment->reply_relay : "");
        json_builder_add_string_value(builder, "reply");
        json_builder_end_array(builder);
    }

    /* Kind tag: ["k", "<kind>"] */
    if (comment->root_kind >= 0) {
        gchar *kind_str = g_strdup_printf("%d", comment->root_kind);
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "k");
        json_builder_add_string_value(builder, kind_str);
        json_builder_end_array(builder);
        g_free(kind_str);
    }

    /* Addressable event a-tag: ["a", "<kind:pubkey:d-tag>", "<relay>"] */
    if (comment->root_addr) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "a");
        json_builder_add_string_value(builder, comment->root_addr);
        if (comment->root_addr_relay) {
            json_builder_add_string_value(builder, comment->root_addr_relay);
        }
        json_builder_end_array(builder);
    }

    /* Pubkey mentions: ["p", "<pubkey>"] */
    if (comment->mentions) {
        for (gsize i = 0; i < comment->mention_count; i++) {
            if (comment->mentions[i]) {
                json_builder_begin_array(builder);
                json_builder_add_string_value(builder, "p");
                json_builder_add_string_value(builder, comment->mentions[i]);
                json_builder_end_array(builder);
            }
        }
    }

    json_builder_end_array(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *result = json_generator_to_data(gen, NULL);

    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(builder);

    return result;
}

gboolean
gnostr_comment_is_comment(gint kind)
{
    return kind == NIP22_KIND_COMMENT;
}

gboolean
gnostr_comment_is_nested_reply(const GnostrComment *comment)
{
    return comment && comment->reply_id != NULL;
}

gboolean
gnostr_comment_is_addressable(const GnostrComment *comment)
{
    return comment && comment->root_addr != NULL;
}

gboolean
gnostr_comment_add_mention(GnostrComment *comment, const gchar *pubkey)
{
    if (!comment || !pubkey || !*pubkey) return FALSE;

    /* Check limit */
    if (comment->mention_count >= NIP22_MAX_MENTIONS) return FALSE;

    /* Check for duplicates */
    for (gsize i = 0; i < comment->mention_count; i++) {
        if (g_strcmp0(comment->mentions[i], pubkey) == 0) {
            return FALSE;
        }
    }

    /* Expand array */
    gsize new_count = comment->mention_count + 1;
    comment->mentions = g_realloc(comment->mentions, sizeof(gchar*) * (new_count + 1));
    comment->mentions[comment->mention_count] = g_strdup(pubkey);
    comment->mentions[new_count] = NULL;
    comment->mention_count = new_count;

    return TRUE;
}

void
gnostr_comment_set_root_event(GnostrComment *comment,
                               const gchar *event_id,
                               gint kind,
                               const gchar *relay)
{
    if (!comment) return;

    g_free(comment->root_id);
    comment->root_id = g_strdup(event_id);

    comment->root_kind = kind;

    g_free(comment->root_relay);
    comment->root_relay = g_strdup(relay);
}

void
gnostr_comment_set_reply_target(GnostrComment *comment,
                                 const gchar *event_id,
                                 const gchar *relay)
{
    if (!comment) return;

    g_free(comment->reply_id);
    comment->reply_id = g_strdup(event_id);

    g_free(comment->reply_relay);
    comment->reply_relay = g_strdup(relay);
}

void
gnostr_comment_set_addressable_root(GnostrComment *comment,
                                     gint kind,
                                     const gchar *pubkey,
                                     const gchar *d_tag,
                                     const gchar *relay)
{
    if (!comment || !pubkey || !d_tag) return;

    g_free(comment->root_addr);
    comment->root_addr = g_strdup_printf("%d:%s:%s", kind, pubkey, d_tag);

    comment->root_kind = kind;

    g_free(comment->root_addr_relay);
    comment->root_addr_relay = g_strdup(relay);
}

gboolean
gnostr_comment_parse_addr(const gchar *addr,
                           gint *out_kind,
                           gchar **out_pubkey,
                           gchar **out_d_tag)
{
    if (!addr || !*addr) return FALSE;

    /* Format: "kind:pubkey:d-tag" */
    gchar **parts = g_strsplit(addr, ":", 3);
    if (!parts || !parts[0] || !parts[1] || !parts[2]) {
        g_strfreev(parts);
        return FALSE;
    }

    /* Parse kind */
    gchar *endptr;
    glong kind = strtol(parts[0], &endptr, 10);
    if (*endptr != '\0' || kind < 0 || kind > 65535) {
        g_strfreev(parts);
        return FALSE;
    }

    /* Validate pubkey (64 hex chars) */
    gsize pubkey_len = strlen(parts[1]);
    if (pubkey_len != 64) {
        g_strfreev(parts);
        return FALSE;
    }

    for (gsize i = 0; i < 64; i++) {
        if (!g_ascii_isxdigit(parts[1][i])) {
            g_strfreev(parts);
            return FALSE;
        }
    }

    /* Output results */
    if (out_kind) *out_kind = (gint)kind;
    if (out_pubkey) *out_pubkey = g_strdup(parts[1]);
    if (out_d_tag) *out_d_tag = g_strdup(parts[2]);

    g_strfreev(parts);
    return TRUE;
}
