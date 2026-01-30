/**
 * NIP-28 Public Chat Implementation
 */

#include "nip28_chat.h"
#include "json.h"
#include <string.h>
#include <stdlib.h>

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

    if (!nostr_json_is_valid(content)) return FALSE;

    char *name = NULL;
    char *about = NULL;
    char *picture = NULL;

    if (nostr_json_get_string(content, "name", &name) == 0 && name) {
        g_free(channel->name);
        channel->name = g_strdup(name);
        free(name);
    }

    if (nostr_json_get_string(content, "about", &about) == 0 && about) {
        g_free(channel->about);
        channel->about = g_strdup(about);
        free(about);
    }

    if (nostr_json_get_string(content, "picture", &picture) == 0 && picture) {
        g_free(channel->picture);
        channel->picture = g_strdup(picture);
        free(picture);
    }

    return TRUE;
}

char *
gnostr_channel_create_metadata_json(const GnostrChannel *channel)
{
    if (!channel) return NULL;

    NostrJsonBuilder *builder = nostr_json_builder_new();
    if (!builder) return NULL;

    nostr_json_builder_begin_object(builder);

    if (channel->name) {
        nostr_json_builder_set_key(builder, "name");
        nostr_json_builder_add_string(builder, channel->name);
    }
    if (channel->about) {
        nostr_json_builder_set_key(builder, "about");
        nostr_json_builder_add_string(builder, channel->about);
    }
    if (channel->picture) {
        nostr_json_builder_set_key(builder, "picture");
        nostr_json_builder_add_string(builder, channel->picture);
    }

    nostr_json_builder_end_object(builder);

    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    return result;
}

/* Callback context for extracting channel ID */
typedef struct {
    char *channel_id;
    char *fallback_id;
} ExtractChannelCtx;

/* Callback to find channel ID in tags */
static bool extract_channel_id_cb(size_t index, const char *element_json, void *user_data) {
    (void)index;
    ExtractChannelCtx *ctx = user_data;

    /* Get tag array length */
    size_t arr_len = 0;
    if (nostr_json_get_array_length(element_json, NULL, &arr_len) != 0 || arr_len < 2) {
        return true; /* continue */
    }

    /* Get tag name (first element) */
    char *tag_name = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
        return true;
    }

    if (strcmp(tag_name, "e") != 0) {
        free(tag_name);
        return true;
    }
    free(tag_name);

    /* Get event ID (second element) */
    char *event_id = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 1, &event_id) != 0 || !event_id) {
        return true;
    }

    /* Per NIP-28: the "e" tag with "root" marker identifies the channel */
    if (arr_len >= 4) {
        char *marker = NULL;
        if (nostr_json_get_array_string(element_json, NULL, 3, &marker) == 0 && marker) {
            if (strcmp(marker, "root") == 0) {
                ctx->channel_id = g_strdup(event_id);
                free(marker);
                free(event_id);
                return false; /* stop - found root */
            }
            free(marker);
        }
    }

    /* Fallback: first "e" tag without a reply marker is the channel */
    if (!ctx->fallback_id) {
        ctx->fallback_id = g_strdup(event_id);
    }

    free(event_id);
    return true; /* continue iteration */
}

char *
gnostr_chat_message_extract_channel_id(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!nostr_json_is_valid(tags_json) || !nostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    ExtractChannelCtx ctx = { .channel_id = NULL, .fallback_id = NULL };
    nostr_json_array_foreach_root(tags_json, extract_channel_id_cb, &ctx);

    /* Return root-marked channel or fallback to first e-tag */
    if (ctx.channel_id) {
        g_free(ctx.fallback_id);
        return ctx.channel_id;
    }
    return ctx.fallback_id;
}

char *
gnostr_chat_message_create_tags(const char *channel_id,
                                 const char *reply_to,
                                 const char *recommended_relay)
{
    if (!channel_id) return NULL;

    NostrJsonBuilder *builder = nostr_json_builder_new();
    if (!builder) return NULL;

    nostr_json_builder_begin_array(builder);

    /* Channel reference - always the root: ["e", channel_id, relay, "root"] */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "e");
    nostr_json_builder_add_string(builder, channel_id);
    nostr_json_builder_add_string(builder, recommended_relay ? recommended_relay : "");
    nostr_json_builder_add_string(builder, "root");
    nostr_json_builder_end_array(builder);

    /* Reply reference if this is a reply: ["e", reply_to, relay, "reply"] */
    if (reply_to) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "e");
        nostr_json_builder_add_string(builder, reply_to);
        nostr_json_builder_add_string(builder, recommended_relay ? recommended_relay : "");
        nostr_json_builder_add_string(builder, "reply");
        nostr_json_builder_end_array(builder);
    }

    nostr_json_builder_end_array(builder);

    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

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

    NostrJsonBuilder *builder = nostr_json_builder_new();
    if (!builder) return NULL;

    nostr_json_builder_begin_array(builder);

    /* Reference to the channel being updated: ["e", channel_id, relay?] */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "e");
    nostr_json_builder_add_string(builder, channel_id);
    if (recommended_relay) {
        nostr_json_builder_add_string(builder, recommended_relay);
    }
    nostr_json_builder_end_array(builder);

    nostr_json_builder_end_array(builder);

    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    return result;
}
