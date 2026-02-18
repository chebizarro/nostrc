/*
 * nip_c7_chats.c - NIP-C7 (0xC7 / Kind 199) Chat Rooms Implementation
 *
 * Implements chat room and message parsing for:
 *   - Kind 199 (0xC7): Chat message
 *   - Kind 39001: Chat room metadata
 */

#define G_LOG_DOMAIN "nip-c7-chats"

#include "nip_c7_chats.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <string.h>

/* ============== Chat Room Implementation ============== */

GnostrChatRoom *
gnostr_chat_room_new(void)
{
  GnostrChatRoom *room = g_new0(GnostrChatRoom, 1);
  room->topics = g_ptr_array_new_with_free_func(g_free);
  return room;
}

void
gnostr_chat_room_free(GnostrChatRoom *room)
{
  if (!room) return;

  g_free(room->room_id);
  g_free(room->name);
  g_free(room->about);
  g_free(room->picture);
  g_free(room->creator_pubkey);
  g_free(room->event_id);

  if (room->moderators) {
    for (guint i = 0; i < room->mod_count; i++) {
      g_free(room->moderators[i]);
    }
    g_free(room->moderators);
  }

  if (room->topics) {
    g_ptr_array_unref(room->topics);
  }

  g_free(room);
}

GnostrChatRoom *
gnostr_chat_room_copy(const GnostrChatRoom *room)
{
  if (!room) return NULL;

  GnostrChatRoom *copy = gnostr_chat_room_new();
  copy->room_id = g_strdup(room->room_id);
  copy->name = g_strdup(room->name);
  copy->about = g_strdup(room->about);
  copy->picture = g_strdup(room->picture);
  copy->creator_pubkey = g_strdup(room->creator_pubkey);
  copy->event_id = g_strdup(room->event_id);
  copy->created_at = room->created_at;

  /* Copy moderators */
  if (room->moderators && room->mod_count > 0) {
    copy->moderators = g_new0(gchar *, room->mod_count);
    copy->mod_count = room->mod_count;
    for (guint i = 0; i < room->mod_count; i++) {
      copy->moderators[i] = g_strdup(room->moderators[i]);
    }
  }

  /* Copy topics */
  if (room->topics) {
    for (guint i = 0; i < room->topics->len; i++) {
      g_ptr_array_add(copy->topics, g_strdup(g_ptr_array_index(room->topics, i)));
    }
  }

  return copy;
}

GnostrChatRoom *
gnostr_chat_room_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  /* Parse with NostrEvent API */
  NostrEvent event = {0};
  if (!nostr_event_deserialize_compact(&event, event_json, NULL)) {
    g_warning("chat_room: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  if (nostr_event_get_kind(&event) != NIPC7_KIND_CHAT_ROOM) {
    return NULL;
  }

  GnostrChatRoom *room = gnostr_chat_room_new();

  /* Extract event ID */
  char *id_str = nostr_event_get_id(&event);
  if (id_str) {
    room->event_id = id_str;  /* takes ownership */
  }

  /* Extract pubkey (creator) */
  const char *pubkey_str = nostr_event_get_pubkey(&event);
  if (pubkey_str) {
    room->creator_pubkey = g_strdup(pubkey_str);
  }

  /* Extract created_at */
  room->created_at = nostr_event_get_created_at(&event);

  /* Parse tags for room metadata using NostrTags API */
  NostrTags *tags = nostr_event_get_tags(&event);
  if (tags) {
    /* First pass: count moderators */
    guint mod_count = 0;
    size_t tag_count = nostr_tags_size(tags);
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;
      const char *tag_name = nostr_tag_get(tag, 0);
      if (g_strcmp0(tag_name, "moderator") == 0) {
        mod_count++;
      }
    }

    /* Allocate moderators array if needed */
    if (mod_count > 0) {
      room->moderators = g_new0(gchar *, mod_count);
    }

    /* Second pass: extract values */
    guint mod_idx = 0;
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        g_free(room->room_id);
        room->room_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "name") == 0) {
        g_free(room->name);
        room->name = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "about") == 0) {
        g_free(room->about);
        room->about = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "picture") == 0) {
        g_free(room->picture);
        room->picture = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "moderator") == 0 && mod_idx < mod_count) {
        room->moderators[mod_idx++] = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "t") == 0) {
        g_ptr_array_add(room->topics, g_strdup(tag_value));
      }
    }
    room->mod_count = mod_idx;
  }

  /* Validate: must have room_id (d tag) */
  if (!room->room_id) {
    g_debug("chat_room: missing 'd' tag room identifier");
    gnostr_chat_room_free(room);
    return NULL;
  }

  g_debug("chat_room: parsed room '%s' (id=%s) with %u moderators",
          room->name ? room->name : "(unnamed)",
          room->room_id,
          room->mod_count);

  return room;
}

/* Context for counting moderators in first pass */
typedef struct {
  guint mod_count;
} ChatRoomCountCtx;

static gboolean count_moderators_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  ChatRoomCountCtx *ctx = (ChatRoomCountCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (g_strcmp0(tag_name, "moderator") == 0) {
      ctx->mod_count++;
    }
    g_free(tag_name);
  }
  return true;
}

/* Context for parsing chat room tags */
typedef struct {
  GnostrChatRoom *room;
  guint mod_count;
  guint mod_idx;
} ChatRoomParseCtx;

static gboolean parse_chat_room_tag_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  ChatRoomParseCtx *ctx = (ChatRoomParseCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if ((tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL)) == NULL ||
      (tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) == NULL) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (!tag_name || !tag_value) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (g_strcmp0(tag_name, "d") == 0) {
    g_free(ctx->room->room_id);
    ctx->room->room_id = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "name") == 0) {
    g_free(ctx->room->name);
    ctx->room->name = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "about") == 0) {
    g_free(ctx->room->about);
    ctx->room->about = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "picture") == 0) {
    g_free(ctx->room->picture);
    ctx->room->picture = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "moderator") == 0 && ctx->mod_idx < ctx->mod_count) {
    ctx->room->moderators[ctx->mod_idx++] = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "t") == 0) {
    g_ptr_array_add(ctx->room->topics, tag_value);
    tag_value = NULL;
  }

  g_free(tag_name);
  g_free(tag_value);
  return true;
}

gboolean
gnostr_chat_room_parse_from_tags(const gchar *tags_json, GnostrChatRoom *room)
{
  if (!tags_json || !room) return FALSE;

  if (!gnostr_json_is_array_str(tags_json)) {
    return FALSE;
  }

  /* First pass: count moderators */
  ChatRoomCountCtx count_ctx = {0};
  gnostr_json_array_foreach_root(tags_json, count_moderators_cb, &count_ctx);

  /* Free existing moderators if any */
  if (room->moderators) {
    for (guint j = 0; j < room->mod_count; j++) {
      g_free(room->moderators[j]);
    }
    g_free(room->moderators);
    room->moderators = NULL;
    room->mod_count = 0;
  }

  /* Allocate moderators array if needed */
  if (count_ctx.mod_count > 0) {
    room->moderators = g_new0(gchar *, count_ctx.mod_count);
  }

  /* Clear topics */
  if (room->topics) {
    g_ptr_array_set_size(room->topics, 0);
  } else {
    room->topics = g_ptr_array_new_with_free_func(g_free);
  }

  /* Parse tags */
  ChatRoomParseCtx parse_ctx = { .room = room, .mod_count = count_ctx.mod_count, .mod_idx = 0 };
  gnostr_json_array_foreach_root(tags_json, parse_chat_room_tag_cb, &parse_ctx);
  room->mod_count = parse_ctx.mod_idx;

  return TRUE;
}

gchar *
gnostr_chat_room_create_tags(const GnostrChatRoom *room)
{
  if (!room || !room->room_id) return NULL;

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_array(builder);

  /* d tag - room identifier (required) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "d");
  gnostr_json_builder_add_string(builder, room->room_id);
  gnostr_json_builder_end_array(builder);

  /* name tag */
  if (room->name && *room->name) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "name");
    gnostr_json_builder_add_string(builder, room->name);
    gnostr_json_builder_end_array(builder);
  }

  /* about tag */
  if (room->about && *room->about) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "about");
    gnostr_json_builder_add_string(builder, room->about);
    gnostr_json_builder_end_array(builder);
  }

  /* picture tag */
  if (room->picture && *room->picture) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "picture");
    gnostr_json_builder_add_string(builder, room->picture);
    gnostr_json_builder_end_array(builder);
  }

  /* moderator tags */
  if (room->moderators) {
    for (guint i = 0; i < room->mod_count; i++) {
      if (room->moderators[i] && *room->moderators[i]) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "moderator");
        gnostr_json_builder_add_string(builder, room->moderators[i]);
        gnostr_json_builder_end_array(builder);
      }
    }
  }

  /* topic tags */
  if (room->topics) {
    for (guint i = 0; i < room->topics->len; i++) {
      const gchar *topic = g_ptr_array_index(room->topics, i);
      if (topic && *topic) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "t");
        gnostr_json_builder_add_string(builder, topic);
        gnostr_json_builder_end_array(builder);
      }
    }
  }

  gnostr_json_builder_end_array(builder);
  char *result = gnostr_json_builder_finish(builder);

  return result;
}

void
gnostr_chat_room_add_moderator(GnostrChatRoom *room, const gchar *pubkey_hex)
{
  if (!room || !pubkey_hex || !*pubkey_hex) return;

  /* Check if already a moderator */
  if (gnostr_chat_room_is_moderator(room, pubkey_hex)) return;

  /* Expand moderators array */
  room->moderators = g_realloc(room->moderators, sizeof(gchar *) * (room->mod_count + 1));
  room->moderators[room->mod_count] = g_strdup(pubkey_hex);
  room->mod_count++;
}

gboolean
gnostr_chat_room_is_moderator(const GnostrChatRoom *room, const gchar *pubkey_hex)
{
  if (!room || !pubkey_hex || !room->moderators) return FALSE;

  for (guint i = 0; i < room->mod_count; i++) {
    if (g_strcmp0(room->moderators[i], pubkey_hex) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

/* ============== Chat Message Implementation ============== */

GnostrChatMessage *
gnostr_c7_chat_message_new(void)
{
  GnostrChatMessage *msg = g_new0(GnostrChatMessage, 1);
  msg->topics = g_ptr_array_new_with_free_func(g_free);
  return msg;
}

void
gnostr_c7_chat_message_free(GnostrChatMessage *msg)
{
  if (!msg) return;

  g_free(msg->event_id);
  g_free(msg->room_id);
  g_free(msg->author_pubkey);
  g_free(msg->content);
  g_free(msg->reply_to);
  g_free(msg->reply_relay);

  if (msg->mentions) {
    for (guint i = 0; i < msg->mention_count; i++) {
      g_free(msg->mentions[i]);
    }
    g_free(msg->mentions);
  }

  if (msg->topics) {
    g_ptr_array_unref(msg->topics);
  }

  g_free(msg);
}

GnostrChatMessage *
gnostr_c7_chat_message_copy(const GnostrChatMessage *msg)
{
  if (!msg) return NULL;

  GnostrChatMessage *copy = gnostr_c7_chat_message_new();
  copy->event_id = g_strdup(msg->event_id);
  copy->room_id = g_strdup(msg->room_id);
  copy->author_pubkey = g_strdup(msg->author_pubkey);
  copy->content = g_strdup(msg->content);
  copy->created_at = msg->created_at;
  copy->reply_to = g_strdup(msg->reply_to);
  copy->reply_relay = g_strdup(msg->reply_relay);

  /* Copy mentions */
  if (msg->mentions && msg->mention_count > 0) {
    copy->mentions = g_new0(gchar *, msg->mention_count);
    copy->mention_count = msg->mention_count;
    for (guint i = 0; i < msg->mention_count; i++) {
      copy->mentions[i] = g_strdup(msg->mentions[i]);
    }
  }

  /* Copy topics */
  if (msg->topics) {
    for (guint i = 0; i < msg->topics->len; i++) {
      g_ptr_array_add(copy->topics, g_strdup(g_ptr_array_index(msg->topics, i)));
    }
  }

  return copy;
}

GnostrChatMessage *
gnostr_c7_chat_message_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  /* Parse with NostrEvent API */
  NostrEvent event = {0};
  if (!nostr_event_deserialize_compact(&event, event_json, NULL)) {
    g_warning("chat_message: failed to parse JSON");
    return NULL;
  }

  /* Verify kind */
  if (nostr_event_get_kind(&event) != NIPC7_KIND_CHAT_MESSAGE) {
    return NULL;
  }

  GnostrChatMessage *msg = gnostr_c7_chat_message_new();

  /* Extract event ID */
  char *id_str = nostr_event_get_id(&event);
  if (id_str) {
    msg->event_id = id_str;  /* takes ownership */
  }

  /* Extract pubkey (author) */
  const char *pubkey_str = nostr_event_get_pubkey(&event);
  if (pubkey_str) {
    msg->author_pubkey = g_strdup(pubkey_str);
  }

  /* Extract content */
  const char *content_str = nostr_event_get_content(&event);
  if (content_str) {
    msg->content = g_strdup(content_str);
  }

  /* Extract created_at */
  msg->created_at = nostr_event_get_created_at(&event);

  /* Parse tags for message metadata using NostrTags API */
  NostrTags *tags = nostr_event_get_tags(&event);
  if (tags) {
    /* First pass: count mentions (p tags) */
    guint mention_count = 0;
    size_t tag_count = nostr_tags_size(tags);
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;
      const char *tag_name = nostr_tag_get(tag, 0);
      if (g_strcmp0(tag_name, "p") == 0) {
        mention_count++;
      }
    }

    /* Allocate mentions array if needed */
    if (mention_count > 0) {
      msg->mentions = g_new0(gchar *, mention_count);
    }

    /* Second pass: extract values */
    guint mention_idx = 0;
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        g_free(msg->room_id);
        msg->room_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* Reply reference */
        if (!msg->reply_to) {
          msg->reply_to = g_strdup(tag_value);
          /* Check for relay hint */
          if (nostr_tag_size(tag) >= 3) {
            const char *relay = nostr_tag_get(tag, 2);
            if (relay && *relay) {
              msg->reply_relay = g_strdup(relay);
            }
          }
        }
      } else if (g_strcmp0(tag_name, "p") == 0 && mention_idx < mention_count) {
        msg->mentions[mention_idx++] = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "t") == 0) {
        g_ptr_array_add(msg->topics, g_strdup(tag_value));
      }
    }
    msg->mention_count = mention_idx;
  }

  /* Validate: must have room_id (d tag) */
  if (!msg->room_id) {
    g_debug("chat_message: missing 'd' tag room identifier");
    gnostr_c7_chat_message_free(msg);
    return NULL;
  }

  g_debug("chat_message: parsed message in room '%s' with %u mentions",
          msg->room_id, msg->mention_count);

  return msg;
}

/* Context for counting mentions in first pass */
typedef struct {
  guint mention_count;
} ChatMsgCountCtx;

static gboolean count_mentions_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  ChatMsgCountCtx *ctx = (ChatMsgCountCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (g_strcmp0(tag_name, "p") == 0) {
      ctx->mention_count++;
    }
    g_free(tag_name);
  }
  return true;
}

/* Context for parsing chat message tags */
typedef struct {
  GnostrChatMessage *msg;
  guint mention_count;
  guint mention_idx;
} ChatMsgParseCtx;

static gboolean parse_chat_msg_tag_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  ChatMsgParseCtx *ctx = (ChatMsgParseCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if ((tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL)) == NULL ||
      (tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) == NULL) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (!tag_name || !tag_value) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (g_strcmp0(tag_name, "d") == 0) {
    g_free(ctx->msg->room_id);
    ctx->msg->room_id = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "e") == 0) {
    if (!ctx->msg->reply_to) {
      ctx->msg->reply_to = tag_value;
      tag_value = NULL;
      /* Check for relay hint */
      char *relay = NULL;
      if ((relay = gnostr_json_get_array_string(element_json, NULL, 2, NULL)) != NULL && relay && *relay) {
        ctx->msg->reply_relay = relay;
      } else {
        g_free(relay);
      }
    }
  } else if (g_strcmp0(tag_name, "p") == 0 && ctx->mention_idx < ctx->mention_count) {
    ctx->msg->mentions[ctx->mention_idx++] = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "t") == 0) {
    g_ptr_array_add(ctx->msg->topics, tag_value);
    tag_value = NULL;
  }

  g_free(tag_name);
  g_free(tag_value);
  return true;
}

gboolean
gnostr_c7_chat_message_parse_from_tags(const gchar *tags_json, GnostrChatMessage *msg)
{
  if (!tags_json || !msg) return FALSE;

  if (!gnostr_json_is_array_str(tags_json)) {
    return FALSE;
  }

  /* First pass: count mentions */
  ChatMsgCountCtx count_ctx = {0};
  gnostr_json_array_foreach_root(tags_json, count_mentions_cb, &count_ctx);

  /* Free existing mentions if any */
  if (msg->mentions) {
    for (guint j = 0; j < msg->mention_count; j++) {
      g_free(msg->mentions[j]);
    }
    g_free(msg->mentions);
    msg->mentions = NULL;
    msg->mention_count = 0;
  }

  /* Allocate mentions array if needed */
  if (count_ctx.mention_count > 0) {
    msg->mentions = g_new0(gchar *, count_ctx.mention_count);
  }

  /* Clear topics */
  if (msg->topics) {
    g_ptr_array_set_size(msg->topics, 0);
  } else {
    msg->topics = g_ptr_array_new_with_free_func(g_free);
  }

  /* Clear reply info */
  g_clear_pointer(&msg->reply_to, g_free);
  g_clear_pointer(&msg->reply_relay, g_free);

  /* Parse tags */
  ChatMsgParseCtx parse_ctx = { .msg = msg, .mention_count = count_ctx.mention_count, .mention_idx = 0 };
  gnostr_json_array_foreach_root(tags_json, parse_chat_msg_tag_cb, &parse_ctx);
  msg->mention_count = parse_ctx.mention_idx;

  return TRUE;
}

gchar *
gnostr_c7_chat_message_create_tags(const GnostrChatMessage *msg)
{
  if (!msg || !msg->room_id) return NULL;

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_array(builder);

  /* d tag - room identifier (required) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "d");
  gnostr_json_builder_add_string(builder, msg->room_id);
  gnostr_json_builder_end_array(builder);

  /* e tag - reply reference */
  if (msg->reply_to && *msg->reply_to) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, msg->reply_to);
    if (msg->reply_relay && *msg->reply_relay) {
      gnostr_json_builder_add_string(builder, msg->reply_relay);
    }
    gnostr_json_builder_end_array(builder);
  }

  /* p tags - mentions */
  if (msg->mentions) {
    for (guint i = 0; i < msg->mention_count; i++) {
      if (msg->mentions[i] && *msg->mentions[i]) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "p");
        gnostr_json_builder_add_string(builder, msg->mentions[i]);
        gnostr_json_builder_end_array(builder);
      }
    }
  }

  /* t tags - topics */
  if (msg->topics) {
    for (guint i = 0; i < msg->topics->len; i++) {
      const gchar *topic = g_ptr_array_index(msg->topics, i);
      if (topic && *topic) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "t");
        gnostr_json_builder_add_string(builder, topic);
        gnostr_json_builder_end_array(builder);
      }
    }
  }

  gnostr_json_builder_end_array(builder);
  char *result = gnostr_json_builder_finish(builder);

  return result;
}

void
gnostr_c7_chat_message_add_mention(GnostrChatMessage *msg, const gchar *pubkey_hex)
{
  if (!msg || !pubkey_hex || !*pubkey_hex) return;

  /* Check if already mentioned */
  if (msg->mentions) {
    for (guint i = 0; i < msg->mention_count; i++) {
      if (g_strcmp0(msg->mentions[i], pubkey_hex) == 0) {
        return;
      }
    }
  }

  /* Expand mentions array */
  msg->mentions = g_realloc(msg->mentions, sizeof(gchar *) * (msg->mention_count + 1));
  msg->mentions[msg->mention_count] = g_strdup(pubkey_hex);
  msg->mention_count++;
}

void
gnostr_c7_chat_message_add_topic(GnostrChatMessage *msg, const gchar *topic)
{
  if (!msg || !topic || !*topic) return;

  if (!msg->topics) {
    msg->topics = g_ptr_array_new_with_free_func(g_free);
  }

  /* Check for duplicate */
  for (guint i = 0; i < msg->topics->len; i++) {
    if (g_strcmp0(g_ptr_array_index(msg->topics, i), topic) == 0) {
      return;
    }
  }

  g_ptr_array_add(msg->topics, g_strdup(topic));
}

/* Context for extracting room ID */
typedef struct {
  gchar *room_id;
} ExtractRoomIdCtx;

static gboolean extract_room_id_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  ExtractRoomIdCtx *ctx = (ExtractRoomIdCtx *)user_data;

  /* Stop if we already found it */
  if (ctx->room_id) return false;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (g_strcmp0(tag_name, "d") == 0) {
      char *tag_value = NULL;
      tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
      if (tag_value) {
        ctx->room_id = tag_value;
        g_free(tag_name);
        return false;  /* Stop iteration */
      }
      g_free(tag_value);
    }
    g_free(tag_name);
  }
  return true;
}

gchar *
gnostr_c7_chat_message_extract_room_id(const gchar *tags_json)
{
  if (!tags_json) return NULL;

  if (!gnostr_json_is_array_str(tags_json)) {
    return NULL;
  }

  ExtractRoomIdCtx ctx = {0};
  gnostr_json_array_foreach_root(tags_json, extract_room_id_cb, &ctx);

  return ctx.room_id;
}
