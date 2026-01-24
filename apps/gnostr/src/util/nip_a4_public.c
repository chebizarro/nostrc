/**
 * NIP-A4 (Kind 164) Public Messages Implementation
 *
 * Parsing, building, and utility functions for public message events.
 */

#include "nip_a4_public.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

GnostrPublicMessage *
gnostr_public_message_new(void)
{
    return g_new0(GnostrPublicMessage, 1);
}

void
gnostr_public_message_free(GnostrPublicMessage *msg)
{
    if (!msg) return;

    g_free(msg->event_id);
    g_free(msg->pubkey);
    g_free(msg->subject);
    g_free(msg->content);
    g_strfreev(msg->tags);
    g_strfreev(msg->recipients);
    g_free(msg->location);
    g_free(msg->geohash);
    g_free(msg);
}

GnostrPublicMessage *
gnostr_public_message_copy(const GnostrPublicMessage *msg)
{
    if (!msg) return NULL;

    GnostrPublicMessage *copy = gnostr_public_message_new();

    copy->event_id = g_strdup(msg->event_id);
    copy->pubkey = g_strdup(msg->pubkey);
    copy->subject = g_strdup(msg->subject);
    copy->content = g_strdup(msg->content);
    copy->tags = g_strdupv(msg->tags);
    copy->tag_count = msg->tag_count;
    copy->expiration = msg->expiration;
    copy->recipients = g_strdupv(msg->recipients);
    copy->recipient_count = msg->recipient_count;
    copy->location = g_strdup(msg->location);
    copy->geohash = g_strdup(msg->geohash);
    copy->created_at = msg->created_at;

    return copy;
}

gboolean
gnostr_public_message_is_kind(gint kind)
{
    return kind == NIPA4_KIND_PUBLIC_MESSAGE;
}

GnostrPublicMessage *
gnostr_public_message_parse(const gchar *json_str)
{
    if (!json_str || !*json_str) return NULL;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
        g_debug("NIP-A4: Failed to parse public message JSON: %s",
                error ? error->message : "unknown");
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);

    /* Check kind */
    if (!json_object_has_member(obj, "kind")) {
        g_object_unref(parser);
        return NULL;
    }
    gint64 kind = json_object_get_int_member(obj, "kind");
    if (kind != NIPA4_KIND_PUBLIC_MESSAGE) {
        g_object_unref(parser);
        return NULL;
    }

    GnostrPublicMessage *msg = gnostr_public_message_new();

    /* Get event ID */
    if (json_object_has_member(obj, "id")) {
        msg->event_id = g_strdup(json_object_get_string_member(obj, "id"));
    }

    /* Get pubkey */
    if (json_object_has_member(obj, "pubkey")) {
        msg->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
    }

    /* Get content */
    if (json_object_has_member(obj, "content")) {
        msg->content = g_strdup(json_object_get_string_member(obj, "content"));
    }

    /* Get created_at */
    if (json_object_has_member(obj, "created_at")) {
        msg->created_at = json_object_get_int_member(obj, "created_at");
    }

    /* Parse tags */
    GPtrArray *topics_arr = g_ptr_array_new();
    GPtrArray *recipients_arr = g_ptr_array_new();

    if (json_object_has_member(obj, "tags")) {
        JsonArray *tags = json_object_get_array_member(obj, "tags");
        guint n_tags = json_array_get_length(tags);

        for (guint i = 0; i < n_tags; i++) {
            JsonNode *tag_node = json_array_get_element(tags, i);
            if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

            JsonArray *tag = json_node_get_array(tag_node);
            guint tag_len = json_array_get_length(tag);
            if (tag_len < 2) continue;

            const gchar *tag_name = json_array_get_string_element(tag, 0);
            if (!tag_name) continue;

            if (g_strcmp0(tag_name, "subject") == 0) {
                /* Subject: ["subject", "<subject-line>"] */
                const gchar *subject = json_array_get_string_element(tag, 1);
                if (subject && !msg->subject) {
                    msg->subject = g_strdup(subject);
                }
            } else if (g_strcmp0(tag_name, "t") == 0) {
                /* Topic tag: ["t", "<tag>"] */
                const gchar *topic = json_array_get_string_element(tag, 1);
                if (topic && *topic) {
                    g_ptr_array_add(topics_arr, g_strdup(topic));
                }
            } else if (g_strcmp0(tag_name, "expiration") == 0) {
                /* Expiration: ["expiration", "<timestamp>"] */
                const gchar *exp_str = json_array_get_string_element(tag, 1);
                if (exp_str) {
                    msg->expiration = g_ascii_strtoll(exp_str, NULL, 10);
                }
            } else if (g_strcmp0(tag_name, "p") == 0) {
                /* Recipient: ["p", "<pubkey>"] */
                const gchar *pubkey = json_array_get_string_element(tag, 1);
                if (pubkey && *pubkey) {
                    g_ptr_array_add(recipients_arr, g_strdup(pubkey));
                }
            } else if (g_strcmp0(tag_name, "location") == 0) {
                /* Location: ["location", "<geo>"] */
                const gchar *location = json_array_get_string_element(tag, 1);
                if (location && !msg->location) {
                    msg->location = g_strdup(location);
                }
            } else if (g_strcmp0(tag_name, "g") == 0) {
                /* Geohash: ["g", "<geohash>"] */
                const gchar *geohash = json_array_get_string_element(tag, 1);
                if (geohash && !msg->geohash) {
                    msg->geohash = g_strdup(geohash);
                }
            }
        }
    }

    /* Convert topics array to NULL-terminated string array */
    if (topics_arr->len > 0) {
        msg->tag_count = topics_arr->len;
        g_ptr_array_add(topics_arr, NULL);
        msg->tags = (gchar **)g_ptr_array_free(topics_arr, FALSE);
    } else {
        g_ptr_array_free(topics_arr, TRUE);
    }

    /* Convert recipients array to NULL-terminated string array */
    if (recipients_arr->len > 0) {
        msg->recipient_count = recipients_arr->len;
        g_ptr_array_add(recipients_arr, NULL);
        msg->recipients = (gchar **)g_ptr_array_free(recipients_arr, FALSE);
    } else {
        g_ptr_array_free(recipients_arr, TRUE);
    }

    g_object_unref(parser);

    return msg;
}

gboolean
gnostr_public_message_is_expired(const GnostrPublicMessage *msg)
{
    if (!msg || msg->expiration <= 0) return FALSE;
    gint64 now = (gint64)time(NULL);
    return now >= msg->expiration;
}

gboolean
gnostr_public_message_has_expiration(const GnostrPublicMessage *msg)
{
    return msg && msg->expiration > 0;
}

void
gnostr_public_message_add_topic(GnostrPublicMessage *msg, const gchar *topic)
{
    if (!msg || !topic || !*topic) return;

    GPtrArray *arr = g_ptr_array_new();

    /* Copy existing tags */
    if (msg->tags) {
        for (gsize i = 0; msg->tags[i]; i++) {
            g_ptr_array_add(arr, g_strdup(msg->tags[i]));
        }
        g_strfreev(msg->tags);
    }

    /* Add new topic */
    g_ptr_array_add(arr, g_strdup(topic));
    msg->tag_count = arr->len;

    g_ptr_array_add(arr, NULL);
    msg->tags = (gchar **)g_ptr_array_free(arr, FALSE);
}

void
gnostr_public_message_add_recipient(GnostrPublicMessage *msg, const gchar *pubkey)
{
    if (!msg || !pubkey || !*pubkey) return;

    GPtrArray *arr = g_ptr_array_new();

    /* Copy existing recipients */
    if (msg->recipients) {
        for (gsize i = 0; msg->recipients[i]; i++) {
            g_ptr_array_add(arr, g_strdup(msg->recipients[i]));
        }
        g_strfreev(msg->recipients);
    }

    /* Add new recipient */
    g_ptr_array_add(arr, g_strdup(pubkey));
    msg->recipient_count = arr->len;

    g_ptr_array_add(arr, NULL);
    msg->recipients = (gchar **)g_ptr_array_free(arr, FALSE);
}

gchar *
gnostr_public_message_build_tags(const GnostrPublicMessage *msg)
{
    if (!msg) return NULL;

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);

    /* Subject tag */
    if (msg->subject && *msg->subject) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "subject");
        json_builder_add_string_value(builder, msg->subject);
        json_builder_end_array(builder);
    }

    /* Topic tags */
    if (msg->tags) {
        for (gsize i = 0; msg->tags[i]; i++) {
            json_builder_begin_array(builder);
            json_builder_add_string_value(builder, "t");
            json_builder_add_string_value(builder, msg->tags[i]);
            json_builder_end_array(builder);
        }
    }

    /* Expiration tag */
    if (msg->expiration > 0) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "expiration");
        gchar *exp_str = g_strdup_printf("%" G_GINT64_FORMAT, msg->expiration);
        json_builder_add_string_value(builder, exp_str);
        g_free(exp_str);
        json_builder_end_array(builder);
    }

    /* Recipient tags */
    if (msg->recipients) {
        for (gsize i = 0; msg->recipients[i]; i++) {
            json_builder_begin_array(builder);
            json_builder_add_string_value(builder, "p");
            json_builder_add_string_value(builder, msg->recipients[i]);
            json_builder_end_array(builder);
        }
    }

    /* Location tag */
    if (msg->location && *msg->location) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "location");
        json_builder_add_string_value(builder, msg->location);
        json_builder_end_array(builder);
    }

    /* Geohash tag */
    if (msg->geohash && *msg->geohash) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "g");
        json_builder_add_string_value(builder, msg->geohash);
        json_builder_end_array(builder);
    }

    json_builder_end_array(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    gchar *result = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    g_object_unref(builder);

    return result;
}

gchar *
gnostr_public_message_build_event(const GnostrPublicMessage *msg)
{
    if (!msg) return NULL;

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    /* Kind 164 - public message */
    json_builder_set_member_name(builder, "kind");
    json_builder_add_int_value(builder, NIPA4_KIND_PUBLIC_MESSAGE);

    /* Content - message body */
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, msg->content ? msg->content : "");

    /* Created at */
    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder, (gint64)time(NULL));

    /* Tags */
    json_builder_set_member_name(builder, "tags");
    json_builder_begin_array(builder);

    /* Subject tag */
    if (msg->subject && *msg->subject) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "subject");
        json_builder_add_string_value(builder, msg->subject);
        json_builder_end_array(builder);
    }

    /* Topic tags */
    if (msg->tags) {
        for (gsize i = 0; msg->tags[i]; i++) {
            json_builder_begin_array(builder);
            json_builder_add_string_value(builder, "t");
            json_builder_add_string_value(builder, msg->tags[i]);
            json_builder_end_array(builder);
        }
    }

    /* Expiration tag */
    if (msg->expiration > 0) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "expiration");
        gchar *exp_str = g_strdup_printf("%" G_GINT64_FORMAT, msg->expiration);
        json_builder_add_string_value(builder, exp_str);
        g_free(exp_str);
        json_builder_end_array(builder);
    }

    /* Recipient tags */
    if (msg->recipients) {
        for (gsize i = 0; msg->recipients[i]; i++) {
            json_builder_begin_array(builder);
            json_builder_add_string_value(builder, "p");
            json_builder_add_string_value(builder, msg->recipients[i]);
            json_builder_end_array(builder);
        }
    }

    /* Location tag */
    if (msg->location && *msg->location) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "location");
        json_builder_add_string_value(builder, msg->location);
        json_builder_end_array(builder);
    }

    /* Geohash tag */
    if (msg->geohash && *msg->geohash) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "g");
        json_builder_add_string_value(builder, msg->geohash);
        json_builder_end_array(builder);
    }

    json_builder_end_array(builder);  /* tags */
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    gchar *result = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    g_object_unref(builder);

    return result;
}

gchar *
gnostr_public_message_format_expiration(gint64 expiration)
{
    if (expiration <= 0) return NULL;

    gint64 now = (gint64)time(NULL);
    gint64 remaining = expiration - now;

    if (remaining <= 0) {
        return g_strdup("Expired");
    }

    /* Convert to human-readable format */
    if (remaining < 60) {
        return g_strdup_printf("%d second%s",
                               (int)remaining,
                               remaining == 1 ? "" : "s");
    } else if (remaining < 3600) {
        int minutes = (int)(remaining / 60);
        return g_strdup_printf("%d minute%s",
                               minutes,
                               minutes == 1 ? "" : "s");
    } else if (remaining < 86400) {
        int hours = (int)(remaining / 3600);
        return g_strdup_printf("%d hour%s",
                               hours,
                               hours == 1 ? "" : "s");
    } else if (remaining < 604800) {
        int days = (int)(remaining / 86400);
        return g_strdup_printf("%d day%s",
                               days,
                               days == 1 ? "" : "s");
    } else if (remaining < 2592000) {
        int weeks = (int)(remaining / 604800);
        return g_strdup_printf("%d week%s",
                               weeks,
                               weeks == 1 ? "" : "s");
    } else {
        int months = (int)(remaining / 2592000);
        return g_strdup_printf("%d month%s",
                               months,
                               months == 1 ? "" : "s");
    }
}
