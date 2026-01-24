/*
 * nip_c7_chats.c - NIP-C7 (0xC7 / Kind 199) Chat Rooms Implementation
 *
 * Implements chat room and message parsing for:
 *   - Kind 199 (0xC7): Chat message
 *   - Kind 39001: Chat room metadata
 */

#define G_LOG_DOMAIN "nip-c7-chats"

#include "nip_c7_chats.h"
#include <jansson.h>
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

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("chat_room: failed to parse JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIPC7_KIND_CHAT_ROOM) {
    json_decref(root);
    return NULL;
  }

  GnostrChatRoom *room = gnostr_chat_room_new();

  /* Extract event ID */
  json_t *id_val = json_object_get(root, "id");
  if (id_val && json_is_string(id_val)) {
    room->event_id = g_strdup(json_string_value(id_val));
  }

  /* Extract pubkey (creator) */
  json_t *pubkey_val = json_object_get(root, "pubkey");
  if (pubkey_val && json_is_string(pubkey_val)) {
    room->creator_pubkey = g_strdup(json_string_value(pubkey_val));
  }

  /* Extract created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    room->created_at = json_integer_value(created_val);
  }

  /* Parse tags for room metadata */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    /* First pass: count moderators */
    guint mod_count = 0;
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
      const char *tag_name = json_string_value(json_array_get(tag, 0));
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
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
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

  json_decref(root);

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

gboolean
gnostr_chat_room_parse_from_tags(const gchar *tags_json, GnostrChatRoom *room)
{
  if (!tags_json || !room) return FALSE;

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags || !json_is_array(tags)) {
    if (tags) json_decref(tags);
    return FALSE;
  }

  /* First pass: count moderators */
  guint mod_count = 0;
  size_t i;
  json_t *tag;
  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
    const char *tag_name = json_string_value(json_array_get(tag, 0));
    if (g_strcmp0(tag_name, "moderator") == 0) {
      mod_count++;
    }
  }

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
  if (mod_count > 0) {
    room->moderators = g_new0(gchar *, mod_count);
  }

  /* Clear topics */
  if (room->topics) {
    g_ptr_array_set_size(room->topics, 0);
  } else {
    room->topics = g_ptr_array_new_with_free_func(g_free);
  }

  /* Parse tags */
  guint mod_idx = 0;
  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

    const char *tag_name = json_string_value(json_array_get(tag, 0));
    const char *tag_value = json_string_value(json_array_get(tag, 1));
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

  json_decref(tags);
  return TRUE;
}

gchar *
gnostr_chat_room_create_tags(const GnostrChatRoom *room)
{
  if (!room || !room->room_id) return NULL;

  json_t *tags = json_array();

  /* d tag - room identifier (required) */
  json_t *d_tag = json_array();
  json_array_append_new(d_tag, json_string("d"));
  json_array_append_new(d_tag, json_string(room->room_id));
  json_array_append_new(tags, d_tag);

  /* name tag */
  if (room->name && *room->name) {
    json_t *name_tag = json_array();
    json_array_append_new(name_tag, json_string("name"));
    json_array_append_new(name_tag, json_string(room->name));
    json_array_append_new(tags, name_tag);
  }

  /* about tag */
  if (room->about && *room->about) {
    json_t *about_tag = json_array();
    json_array_append_new(about_tag, json_string("about"));
    json_array_append_new(about_tag, json_string(room->about));
    json_array_append_new(tags, about_tag);
  }

  /* picture tag */
  if (room->picture && *room->picture) {
    json_t *pic_tag = json_array();
    json_array_append_new(pic_tag, json_string("picture"));
    json_array_append_new(pic_tag, json_string(room->picture));
    json_array_append_new(tags, pic_tag);
  }

  /* moderator tags */
  if (room->moderators) {
    for (guint i = 0; i < room->mod_count; i++) {
      if (room->moderators[i] && *room->moderators[i]) {
        json_t *mod_tag = json_array();
        json_array_append_new(mod_tag, json_string("moderator"));
        json_array_append_new(mod_tag, json_string(room->moderators[i]));
        json_array_append_new(tags, mod_tag);
      }
    }
  }

  /* topic tags */
  if (room->topics) {
    for (guint i = 0; i < room->topics->len; i++) {
      const gchar *topic = g_ptr_array_index(room->topics, i);
      if (topic && *topic) {
        json_t *t_tag = json_array();
        json_array_append_new(t_tag, json_string("t"));
        json_array_append_new(t_tag, json_string(topic));
        json_array_append_new(tags, t_tag);
      }
    }
  }

  char *result = json_dumps(tags, JSON_COMPACT);
  json_decref(tags);

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

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("chat_message: failed to parse JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIPC7_KIND_CHAT_MESSAGE) {
    json_decref(root);
    return NULL;
  }

  GnostrChatMessage *msg = gnostr_c7_chat_message_new();

  /* Extract event ID */
  json_t *id_val = json_object_get(root, "id");
  if (id_val && json_is_string(id_val)) {
    msg->event_id = g_strdup(json_string_value(id_val));
  }

  /* Extract pubkey (author) */
  json_t *pubkey_val = json_object_get(root, "pubkey");
  if (pubkey_val && json_is_string(pubkey_val)) {
    msg->author_pubkey = g_strdup(json_string_value(pubkey_val));
  }

  /* Extract content */
  json_t *content_val = json_object_get(root, "content");
  if (content_val && json_is_string(content_val)) {
    msg->content = g_strdup(json_string_value(content_val));
  }

  /* Extract created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    msg->created_at = json_integer_value(created_val);
  }

  /* Parse tags for message metadata */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    /* First pass: count mentions (p tags) */
    guint mention_count = 0;
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
      const char *tag_name = json_string_value(json_array_get(tag, 0));
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
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        g_free(msg->room_id);
        msg->room_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* Reply reference */
        if (!msg->reply_to) {
          msg->reply_to = g_strdup(tag_value);
          /* Check for relay hint */
          if (json_array_size(tag) >= 3) {
            const char *relay = json_string_value(json_array_get(tag, 2));
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

  json_decref(root);

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

gboolean
gnostr_c7_chat_message_parse_from_tags(const gchar *tags_json, GnostrChatMessage *msg)
{
  if (!tags_json || !msg) return FALSE;

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags || !json_is_array(tags)) {
    if (tags) json_decref(tags);
    return FALSE;
  }

  /* First pass: count mentions */
  guint mention_count = 0;
  size_t i;
  json_t *tag;
  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
    const char *tag_name = json_string_value(json_array_get(tag, 0));
    if (g_strcmp0(tag_name, "p") == 0) {
      mention_count++;
    }
  }

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
  if (mention_count > 0) {
    msg->mentions = g_new0(gchar *, mention_count);
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
  guint mention_idx = 0;
  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

    const char *tag_name = json_string_value(json_array_get(tag, 0));
    const char *tag_value = json_string_value(json_array_get(tag, 1));
    if (!tag_name || !tag_value) continue;

    if (g_strcmp0(tag_name, "d") == 0) {
      g_free(msg->room_id);
      msg->room_id = g_strdup(tag_value);
    } else if (g_strcmp0(tag_name, "e") == 0) {
      if (!msg->reply_to) {
        msg->reply_to = g_strdup(tag_value);
        if (json_array_size(tag) >= 3) {
          const char *relay = json_string_value(json_array_get(tag, 2));
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

  json_decref(tags);
  return TRUE;
}

gchar *
gnostr_c7_chat_message_create_tags(const GnostrChatMessage *msg)
{
  if (!msg || !msg->room_id) return NULL;

  json_t *tags = json_array();

  /* d tag - room identifier (required) */
  json_t *d_tag = json_array();
  json_array_append_new(d_tag, json_string("d"));
  json_array_append_new(d_tag, json_string(msg->room_id));
  json_array_append_new(tags, d_tag);

  /* e tag - reply reference */
  if (msg->reply_to && *msg->reply_to) {
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(msg->reply_to));
    if (msg->reply_relay && *msg->reply_relay) {
      json_array_append_new(e_tag, json_string(msg->reply_relay));
    }
    json_array_append_new(tags, e_tag);
  }

  /* p tags - mentions */
  if (msg->mentions) {
    for (guint i = 0; i < msg->mention_count; i++) {
      if (msg->mentions[i] && *msg->mentions[i]) {
        json_t *p_tag = json_array();
        json_array_append_new(p_tag, json_string("p"));
        json_array_append_new(p_tag, json_string(msg->mentions[i]));
        json_array_append_new(tags, p_tag);
      }
    }
  }

  /* t tags - topics */
  if (msg->topics) {
    for (guint i = 0; i < msg->topics->len; i++) {
      const gchar *topic = g_ptr_array_index(msg->topics, i);
      if (topic && *topic) {
        json_t *t_tag = json_array();
        json_array_append_new(t_tag, json_string("t"));
        json_array_append_new(t_tag, json_string(topic));
        json_array_append_new(tags, t_tag);
      }
    }
  }

  char *result = json_dumps(tags, JSON_COMPACT);
  json_decref(tags);

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

gchar *
gnostr_c7_chat_message_extract_room_id(const gchar *tags_json)
{
  if (!tags_json) return NULL;

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags || !json_is_array(tags)) {
    if (tags) json_decref(tags);
    return NULL;
  }

  gchar *room_id = NULL;
  size_t i;
  json_t *tag;

  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

    const char *tag_name = json_string_value(json_array_get(tag, 0));
    if (g_strcmp0(tag_name, "d") == 0) {
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (tag_value) {
        room_id = g_strdup(tag_value);
        break;
      }
    }
  }

  json_decref(tags);
  return room_id;
}
