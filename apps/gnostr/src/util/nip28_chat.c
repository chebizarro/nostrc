/**
 * NIP-28 Public Chat Implementation
 */

#include "nip28_chat.h"
#include <jansson.h>
#include <string.h>

GnostrChannel *
gnostr_channel_new(void)
{
    return g_new0(GnostrChannel, 1);
}

void
gnostr_channel_free(GnostrChannel *channel)
{
    if (!channel) return;

    g_free(channel->channel_id);
    g_free(channel->creator_pubkey);
    g_free(channel->name);
    g_free(channel->about);
    g_free(channel->picture);
    g_free(channel);
}

GnostrChannel *
gnostr_channel_copy(const GnostrChannel *channel)
{
    if (!channel) return NULL;

    GnostrChannel *copy = gnostr_channel_new();
    copy->channel_id = g_strdup(channel->channel_id);
    copy->creator_pubkey = g_strdup(channel->creator_pubkey);
    copy->name = g_strdup(channel->name);
    copy->about = g_strdup(channel->about);
    copy->picture = g_strdup(channel->picture);
    copy->created_at = channel->created_at;
    copy->metadata_at = channel->metadata_at;
    copy->message_count = channel->message_count;
    copy->member_count = channel->member_count;

    return copy;
}

GnostrChatMessage *
gnostr_chat_message_new(void)
{
    return g_new0(GnostrChatMessage, 1);
}

void
gnostr_chat_message_free(GnostrChatMessage *msg)
{
    if (!msg) return;

    g_free(msg->event_id);
    g_free(msg->channel_id);
    g_free(msg->author_pubkey);
    g_free(msg->content);
    g_free(msg->reply_to);
    g_free(msg->root_id);
    g_free(msg);
}

GnostrChatMessage *
gnostr_chat_message_copy(const GnostrChatMessage *msg)
{
    if (!msg) return NULL;

    GnostrChatMessage *copy = gnostr_chat_message_new();
    copy->event_id = g_strdup(msg->event_id);
    copy->channel_id = g_strdup(msg->channel_id);
    copy->author_pubkey = g_strdup(msg->author_pubkey);
    copy->content = g_strdup(msg->content);
    copy->created_at = msg->created_at;
    copy->reply_to = g_strdup(msg->reply_to);
    copy->root_id = g_strdup(msg->root_id);
    copy->is_hidden = msg->is_hidden;

    return copy;
}

gboolean
gnostr_channel_parse_metadata(const char *content, GnostrChannel *channel)
{
    if (!content || !channel) return FALSE;

    json_error_t error;
    json_t *root = json_loads(content, 0, &error);
    if (!root) return FALSE;

    json_t *name = json_object_get(root, "name");
    json_t *about = json_object_get(root, "about");
    json_t *picture = json_object_get(root, "picture");

    if (json_is_string(name)) {
        g_free(channel->name);
        channel->name = g_strdup(json_string_value(name));
    }

    if (json_is_string(about)) {
        g_free(channel->about);
        channel->about = g_strdup(json_string_value(about));
    }

    if (json_is_string(picture)) {
        g_free(channel->picture);
        channel->picture = g_strdup(json_string_value(picture));
    }

    json_decref(root);
    return TRUE;
}

char *
gnostr_channel_create_metadata_json(const GnostrChannel *channel)
{
    if (!channel) return NULL;

    json_t *obj = json_object();

    if (channel->name)
        json_object_set_new(obj, "name", json_string(channel->name));
    if (channel->about)
        json_object_set_new(obj, "about", json_string(channel->about));
    if (channel->picture)
        json_object_set_new(obj, "picture", json_string(channel->picture));

    char *result = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);

    return result;
}

char *
gnostr_chat_message_extract_channel_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    char *channel_id = NULL;
    size_t index;
    json_t *tag;

    /* Look for ["e", "<channel_id>", "<relay>", "root"] pattern */
    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name || strcmp(tag_name, "e") != 0)
            continue;

        const char *event_id = json_string_value(json_array_get(tag, 1));
        if (!event_id)
            continue;

        /* Per NIP-28: the "e" tag with "root" marker identifies the channel */
        if (json_array_size(tag) >= 4) {
            const char *marker = json_string_value(json_array_get(tag, 3));
            if (marker && strcmp(marker, "root") == 0) {
                channel_id = g_strdup(event_id);
                break;
            }
        }

        /* Fallback: first "e" tag without a reply marker is the channel */
        if (!channel_id) {
            channel_id = g_strdup(event_id);
        }
    }

    json_decref(tags);
    return channel_id;
}

char *
gnostr_chat_message_create_tags(const char *channel_id,
                                 const char *reply_to,
                                 const char *recommended_relay)
{
    if (!channel_id) return NULL;

    json_t *tags = json_array();

    /* Channel reference - always the root */
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(channel_id));
    json_array_append_new(e_tag, json_string(recommended_relay ? recommended_relay : ""));
    json_array_append_new(e_tag, json_string("root"));
    json_array_append_new(tags, e_tag);

    /* Reply reference if this is a reply */
    if (reply_to) {
        json_t *reply_tag = json_array();
        json_array_append_new(reply_tag, json_string("e"));
        json_array_append_new(reply_tag, json_string(reply_to));
        json_array_append_new(reply_tag, json_string(recommended_relay ? recommended_relay : ""));
        json_array_append_new(reply_tag, json_string("reply"));
        json_array_append_new(tags, reply_tag);
    }

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);

    return result;
}

char *
gnostr_channel_create_tags(void)
{
    /* Kind-40 channel creation has no required tags */
    return g_strdup("[]");
}

char *
gnostr_channel_metadata_create_tags(const char *channel_id,
                                     const char *recommended_relay)
{
    if (!channel_id) return NULL;

    json_t *tags = json_array();

    /* Reference to the channel being updated */
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(channel_id));
    if (recommended_relay)
        json_array_append_new(e_tag, json_string(recommended_relay));
    json_array_append_new(tags, e_tag);

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);

    return result;
}
