/**
 * @file nip62_vanish.c
 * @brief NIP-62 Request to Vanish implementation
 *
 * Implements parsing and building of NIP-62 vanish request events.
 * See NIP-62: https://github.com/nostr-protocol/nips/blob/master/62.md
 */

#define G_LOG_DOMAIN "gnostr-nip62"

#include "nip62_vanish.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* Tag name for relay specification in NIP-62 */
#define NIP62_TAG_RELAY "relay"

GnostrVanishRequest *gnostr_vanish_request_new(void) {
  GnostrVanishRequest *request = g_new0(GnostrVanishRequest, 1);
  return request;
}

void gnostr_vanish_request_free(GnostrVanishRequest *request) {
  if (!request) return;

  g_free(request->reason);
  g_free(request->pubkey_hex);
  g_free(request->event_id_hex);

  if (request->relays) {
    g_strfreev(request->relays);
  }

  g_free(request);
}

gboolean gnostr_vanish_is_valid_relay_url(const gchar *url) {
  if (!url || !*url) return FALSE;

  /* Must start with ws:// or wss:// */
  if (!g_str_has_prefix(url, "ws://") && !g_str_has_prefix(url, "wss://")) {
    return FALSE;
  }

  /* Parse as URI to validate structure */
  GError *err = NULL;
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &err);
  if (!uri) {
    g_clear_error(&err);
    return FALSE;
  }

  /* Must have a host */
  const gchar *host = g_uri_get_host(uri);
  gboolean valid = (host && *host);

  g_uri_unref(uri);
  return valid;
}

GnostrVanishRequest *gnostr_vanish_request_parse(const gchar *event_json) {
  if (!event_json || !*event_json) {
    g_debug("nip62: empty event JSON");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_debug("nip62: failed to parse JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_debug("nip62: root is not an object");
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify this is kind 62 */
  if (!json_object_has_member(root, "kind")) {
    g_debug("nip62: missing kind field");
    return NULL;
  }

  gint64 kind = json_object_get_int_member(root, "kind");
  if (kind != NIP62_KIND_VANISH) {
    g_debug("nip62: wrong kind %" G_GINT64_FORMAT ", expected %d", kind, NIP62_KIND_VANISH);
    return NULL;
  }

  GnostrVanishRequest *request = gnostr_vanish_request_new();

  /* Extract content (reason) */
  if (json_object_has_member(root, "content")) {
    const gchar *content = json_object_get_string_member(root, "content");
    if (content && *content) {
      request->reason = g_strdup(content);
    }
  }

  /* Extract created_at */
  if (json_object_has_member(root, "created_at")) {
    request->created_at = json_object_get_int_member(root, "created_at");
  }

  /* Extract pubkey */
  if (json_object_has_member(root, "pubkey")) {
    const gchar *pubkey = json_object_get_string_member(root, "pubkey");
    if (pubkey && *pubkey) {
      request->pubkey_hex = g_strdup(pubkey);
    }
  }

  /* Extract event id */
  if (json_object_has_member(root, "id")) {
    const gchar *id = json_object_get_string_member(root, "id");
    if (id && *id) {
      request->event_id_hex = g_strdup(id);
    }
  }

  /* Parse tags for relay URLs */
  GPtrArray *relay_array = g_ptr_array_new_with_free_func(g_free);

  if (json_object_has_member(root, "tags")) {
    JsonNode *tags_node = json_object_get_member(root, "tags");
    if (JSON_NODE_HOLDS_ARRAY(tags_node)) {
      JsonArray *tags = json_node_get_array(tags_node);
      guint tag_count = json_array_get_length(tags);

      for (guint i = 0; i < tag_count; i++) {
        JsonNode *tag_node = json_array_get_element(tags, i);
        if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

        JsonArray *tag = json_node_get_array(tag_node);
        guint tag_len = json_array_get_length(tag);
        if (tag_len < 2) continue;

        /* Check for "relay" tag */
        const gchar *tag_name = json_array_get_string_element(tag, 0);
        if (g_strcmp0(tag_name, NIP62_TAG_RELAY) != 0) continue;

        const gchar *relay_url = json_array_get_string_element(tag, 1);
        if (!relay_url || !*relay_url) continue;

        /* Validate relay URL */
        if (!gnostr_vanish_is_valid_relay_url(relay_url)) {
          g_debug("nip62: skipping invalid relay URL: %s", relay_url);
          continue;
        }

        g_ptr_array_add(relay_array, g_strdup(relay_url));
      }
    }
  }

  /* Convert GPtrArray to NULL-terminated string array */
  if (relay_array->len > 0) {
    request->relay_count = relay_array->len;
    request->relays = g_new0(gchar *, relay_array->len + 1);
    for (guint i = 0; i < relay_array->len; i++) {
      request->relays[i] = g_strdup(g_ptr_array_index(relay_array, i));
    }
  }

  g_ptr_array_unref(relay_array);

  g_debug("nip62: parsed vanish request with %zu relays, reason: %s",
          request->relay_count,
          request->reason ? request->reason : "(none)");

  return request;
}

gchar *gnostr_vanish_build_request_tags(const gchar **relays, gsize relay_count) {
  g_autoptr(JsonBuilder) builder = json_builder_new();

  json_builder_begin_array(builder);

  if (relays && relay_count > 0) {
    for (gsize i = 0; i < relay_count; i++) {
      const gchar *url = relays[i];
      if (!url || !*url) continue;

      /* Validate relay URL before adding */
      if (!gnostr_vanish_is_valid_relay_url(url)) {
        g_debug("nip62: skipping invalid relay URL in tags: %s", url);
        continue;
      }

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, NIP62_TAG_RELAY);
      json_builder_add_string_value(builder, url);
      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  return json_str;
}

gchar *gnostr_vanish_build_unsigned_event(const gchar *reason,
                                           const gchar **relays,
                                           gsize relay_count) {
  g_autoptr(JsonBuilder) builder = json_builder_new();

  json_builder_begin_object(builder);

  /* kind 62 = NIP-62 Request to Vanish */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NIP62_KIND_VANISH);

  /* created_at - current Unix timestamp */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)g_get_real_time() / G_USEC_PER_SEC);

  /* content - human-readable reason (may be empty) */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, reason ? reason : "");

  /* tags - array of ["relay", "<url>"] tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  if (relays && relay_count > 0) {
    for (gsize i = 0; i < relay_count; i++) {
      const gchar *url = relays[i];
      if (!url || !*url) continue;

      /* Validate relay URL before adding */
      if (!gnostr_vanish_is_valid_relay_url(url)) {
        g_debug("nip62: skipping invalid relay URL in event: %s", url);
        continue;
      }

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, NIP62_TAG_RELAY);
      json_builder_add_string_value(builder, url);
      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  g_debug("nip62: built unsigned vanish event with %zu relays", relay_count);

  return json_str;
}

GPtrArray *gnostr_vanish_request_get_relays(const GnostrVanishRequest *request) {
  if (!request || !request->relays || request->relay_count == 0) {
    return NULL;
  }

  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);

  for (gsize i = 0; i < request->relay_count; i++) {
    if (request->relays[i]) {
      g_ptr_array_add(result, g_strdup(request->relays[i]));
    }
  }

  return result;
}

gboolean gnostr_vanish_request_has_relay(const GnostrVanishRequest *request,
                                          const gchar *relay_url) {
  if (!request || !relay_url || !*relay_url) {
    return FALSE;
  }

  if (!request->relays || request->relay_count == 0) {
    return FALSE;
  }

  for (gsize i = 0; i < request->relay_count; i++) {
    if (request->relays[i] && g_strcmp0(request->relays[i], relay_url) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

gboolean gnostr_vanish_request_is_global(const GnostrVanishRequest *request) {
  if (!request) {
    return TRUE;  /* NULL request is effectively global */
  }

  return (request->relays == NULL || request->relay_count == 0);
}
