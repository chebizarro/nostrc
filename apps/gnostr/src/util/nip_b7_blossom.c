/*
 * nip_b7_blossom.c - NIP-B7 Blossom Protocol Support Implementation
 *
 * Parsing utilities for Blossom blob metadata and server lists.
 */

#define G_LOG_DOMAIN "nip-b7-blossom"

#include "nip_b7_blossom.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <ctype.h>

/* ============== GnostrBlob Implementation ============== */

GnostrBlob *
gnostr_blob_new(void)
{
  GnostrBlob *blob = g_new0(GnostrBlob, 1);
  return blob;
}

void
gnostr_blob_free(GnostrBlob *blob)
{
  if (!blob)
    return;

  g_free(blob->sha256);
  g_free(blob->mime_type);
  g_free(blob->url);
  g_free(blob);
}

GnostrBlob *
gnostr_blob_copy(const GnostrBlob *blob)
{
  if (!blob)
    return NULL;

  GnostrBlob *copy = gnostr_blob_new();
  copy->sha256 = g_strdup(blob->sha256);
  copy->size = blob->size;
  copy->mime_type = g_strdup(blob->mime_type);
  copy->url = g_strdup(blob->url);
  copy->created_at = blob->created_at;

  return copy;
}

GnostrBlob *
gnostr_blob_parse_response(const gchar *json_data)
{
  if (!json_data || !*json_data)
    return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_data, -1, &error)) {
    g_debug("Failed to parse blob JSON: %s", error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_debug("Blob JSON root is not an object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GnostrBlob *blob = gnostr_blob_new();

  /* Parse sha256 (required) */
  if (json_object_has_member(obj, "sha256")) {
    const gchar *hash = json_object_get_string_member(obj, "sha256");
    if (gnostr_blob_validate_sha256(hash)) {
      blob->sha256 = g_strdup(hash);
    }
  }

  /* Parse size */
  if (json_object_has_member(obj, "size")) {
    blob->size = (gsize)json_object_get_int_member(obj, "size");
  }

  /* Parse type/mime_type (servers may use either) */
  if (json_object_has_member(obj, "type")) {
    blob->mime_type = g_strdup(json_object_get_string_member(obj, "type"));
  } else if (json_object_has_member(obj, "mime_type")) {
    blob->mime_type = g_strdup(json_object_get_string_member(obj, "mime_type"));
  }

  /* Parse url */
  if (json_object_has_member(obj, "url")) {
    blob->url = g_strdup(json_object_get_string_member(obj, "url"));
  }

  /* Parse created_at (optional) */
  if (json_object_has_member(obj, "created_at")) {
    blob->created_at = json_object_get_int_member(obj, "created_at");
  } else if (json_object_has_member(obj, "created")) {
    blob->created_at = json_object_get_int_member(obj, "created");
  }

  g_object_unref(parser);

  /* Validate we at least got a sha256 */
  if (!blob->sha256) {
    g_debug("Blob response missing sha256");
    gnostr_blob_free(blob);
    return NULL;
  }

  return blob;
}

GPtrArray *
gnostr_blob_parse_list_response(const gchar *json_data)
{
  if (!json_data || !*json_data)
    return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_data, -1, &error)) {
    g_debug("Failed to parse blob list JSON: %s", error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    g_debug("Blob list JSON root is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);

  GPtrArray *blobs = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_blob_free);

  for (guint i = 0; i < len; i++) {
    JsonNode *elem = json_array_get_element(arr, i);
    if (!JSON_NODE_HOLDS_OBJECT(elem))
      continue;

    JsonObject *obj = json_node_get_object(elem);
    GnostrBlob *blob = gnostr_blob_new();

    if (json_object_has_member(obj, "sha256")) {
      const gchar *hash = json_object_get_string_member(obj, "sha256");
      if (gnostr_blob_validate_sha256(hash)) {
        blob->sha256 = g_strdup(hash);
      }
    }

    if (json_object_has_member(obj, "size")) {
      blob->size = (gsize)json_object_get_int_member(obj, "size");
    }

    if (json_object_has_member(obj, "type")) {
      blob->mime_type = g_strdup(json_object_get_string_member(obj, "type"));
    } else if (json_object_has_member(obj, "mime_type")) {
      blob->mime_type = g_strdup(json_object_get_string_member(obj, "mime_type"));
    }

    if (json_object_has_member(obj, "url")) {
      blob->url = g_strdup(json_object_get_string_member(obj, "url"));
    }

    if (json_object_has_member(obj, "created_at")) {
      blob->created_at = json_object_get_int_member(obj, "created_at");
    } else if (json_object_has_member(obj, "created")) {
      blob->created_at = json_object_get_int_member(obj, "created");
    }

    /* Only add if we got a valid sha256 */
    if (blob->sha256) {
      g_ptr_array_add(blobs, blob);
    } else {
      gnostr_blob_free(blob);
    }
  }

  g_object_unref(parser);
  return blobs;
}

gboolean
gnostr_blob_validate_sha256(const gchar *hash)
{
  if (!hash)
    return FALSE;

  gsize len = strlen(hash);
  if (len != 64)
    return FALSE;

  for (gsize i = 0; i < len; i++) {
    char c = hash[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return FALSE;
    }
  }

  return TRUE;
}

/* ============== GnostrBlobServerList Implementation ============== */

GnostrBlobServerList *
gnostr_blob_server_list_new(void)
{
  GnostrBlobServerList *list = g_new0(GnostrBlobServerList, 1);
  list->servers = g_new0(gchar *, 1);  /* Start with NULL terminator */
  list->server_count = 0;
  return list;
}

void
gnostr_blob_server_list_free(GnostrBlobServerList *list)
{
  if (!list)
    return;

  if (list->servers) {
    g_strfreev(list->servers);
  }
  g_free(list);
}

GnostrBlobServerList *
gnostr_blob_server_list_parse(const gchar *tags_json)
{
  if (!tags_json || !*tags_json)
    return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_debug("Failed to parse tags JSON: %s", error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    g_debug("Tags JSON root is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint tags_len = json_array_get_length(tags);

  /* Collect server URLs */
  GPtrArray *servers = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < tags_len; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node))
      continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);

    /* Need at least 2 elements: ["server", "<url>"] */
    if (tag_len < 2)
      continue;

    const gchar *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name || g_strcmp0(tag_name, "server") != 0)
      continue;

    const gchar *url = json_array_get_string_element(tag, 1);
    if (url && *url) {
      gchar *normalized = gnostr_blossom_normalize_url(url);
      if (normalized) {
        /* Check for duplicates */
        gboolean duplicate = FALSE;
        for (guint j = 0; j < servers->len; j++) {
          if (g_strcmp0(g_ptr_array_index(servers, j), normalized) == 0) {
            duplicate = TRUE;
            break;
          }
        }
        if (!duplicate) {
          g_ptr_array_add(servers, normalized);
        } else {
          g_free(normalized);
        }
      }
    }
  }

  g_object_unref(parser);

  /* Convert to server list struct */
  GnostrBlobServerList *list = g_new0(GnostrBlobServerList, 1);
  list->server_count = servers->len;

  /* Create NULL-terminated array */
  list->servers = g_new0(gchar *, servers->len + 1);
  for (guint i = 0; i < servers->len; i++) {
    list->servers[i] = g_strdup(g_ptr_array_index(servers, i));
  }
  list->servers[servers->len] = NULL;

  g_ptr_array_unref(servers);
  return list;
}

GnostrBlobServerList *
gnostr_blob_server_list_parse_event(const gchar *event_json)
{
  if (!event_json || !*event_json)
    return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_debug("Failed to parse event JSON: %s", error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_debug("Event JSON root is not an object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *event = json_node_get_object(root);

  /* Verify kind is 10063 */
  if (json_object_has_member(event, "kind")) {
    gint64 kind = json_object_get_int_member(event, "kind");
    if (kind != NIPB7_KIND_BLOB_SERVERS) {
      g_debug("Event kind %" G_GINT64_FORMAT " is not blob server list (10063)", kind);
      g_object_unref(parser);
      return NULL;
    }
  }

  /* Get tags array */
  if (!json_object_has_member(event, "tags")) {
    g_object_unref(parser);
    return gnostr_blob_server_list_new();  /* Empty list */
  }

  /* Serialize tags back to JSON for parsing */
  JsonNode *tags_node = json_object_get_member(event, "tags");
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, tags_node);
  gchar *tags_json = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  g_object_unref(parser);

  GnostrBlobServerList *list = gnostr_blob_server_list_parse(tags_json);
  g_free(tags_json);

  return list;
}

gboolean
gnostr_blob_server_list_add(GnostrBlobServerList *list,
                             const gchar *server_url)
{
  if (!list || !server_url || !*server_url)
    return FALSE;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return FALSE;

  /* Check if already present */
  if (gnostr_blob_server_list_contains(list, normalized)) {
    g_free(normalized);
    return FALSE;
  }

  /* Expand array */
  gsize new_count = list->server_count + 1;
  list->servers = g_renew(gchar *, list->servers, new_count + 1);
  list->servers[list->server_count] = normalized;
  list->servers[new_count] = NULL;
  list->server_count = new_count;

  return TRUE;
}

gboolean
gnostr_blob_server_list_remove(GnostrBlobServerList *list,
                                const gchar *server_url)
{
  if (!list || !server_url || !*server_url || list->server_count == 0)
    return FALSE;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return FALSE;

  /* Find the server */
  gsize found_index = G_MAXSIZE;
  for (gsize i = 0; i < list->server_count; i++) {
    if (g_strcmp0(list->servers[i], normalized) == 0) {
      found_index = i;
      break;
    }
  }

  g_free(normalized);

  if (found_index == G_MAXSIZE)
    return FALSE;

  /* Free the element and shift remaining */
  g_free(list->servers[found_index]);

  for (gsize i = found_index; i < list->server_count - 1; i++) {
    list->servers[i] = list->servers[i + 1];
  }

  list->server_count--;
  list->servers[list->server_count] = NULL;

  return TRUE;
}

gboolean
gnostr_blob_server_list_contains(const GnostrBlobServerList *list,
                                  const gchar *server_url)
{
  if (!list || !server_url || !*server_url)
    return FALSE;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return FALSE;

  gboolean found = FALSE;
  for (gsize i = 0; i < list->server_count; i++) {
    if (g_strcmp0(list->servers[i], normalized) == 0) {
      found = TRUE;
      break;
    }
  }

  g_free(normalized);
  return found;
}

gchar *
gnostr_blob_server_list_to_tags_json(const GnostrBlobServerList *list)
{
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  if (list && list->servers) {
    for (gsize i = 0; i < list->server_count; i++) {
      if (list->servers[i]) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "server");
        json_builder_add_string_value(builder, list->servers[i]);
        json_builder_end_array(builder);
      }
    }
  }

  json_builder_end_array(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  gchar *json = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);

  return json;
}

/* ============== URL Building Utilities ============== */

gchar *
gnostr_blossom_normalize_url(const gchar *url)
{
  if (!url || !*url)
    return NULL;

  gchar *result = NULL;

  /* Add https:// if no scheme present */
  if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
    result = g_strdup_printf("https://%s", url);
  } else {
    result = g_strdup(url);
  }

  /* Remove trailing slashes */
  gsize len = strlen(result);
  while (len > 0 && result[len - 1] == '/') {
    result[len - 1] = '\0';
    len--;
  }

  return result;
}

gchar *
gnostr_blossom_build_blob_path(const gchar *server_url,
                                const gchar *sha256)
{
  if (!server_url || !sha256)
    return NULL;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return NULL;

  gchar *path = g_strdup_printf("%s/%s", normalized, sha256);
  g_free(normalized);

  return path;
}

gchar *
gnostr_blossom_build_upload_path(const gchar *server_url)
{
  if (!server_url)
    return NULL;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return NULL;

  gchar *path = g_strdup_printf("%s/upload", normalized);
  g_free(normalized);

  return path;
}

gchar *
gnostr_blossom_build_delete_path(const gchar *server_url,
                                  const gchar *sha256)
{
  /* Delete path is same as blob path */
  return gnostr_blossom_build_blob_path(server_url, sha256);
}

gchar *
gnostr_blossom_build_list_path(const gchar *server_url,
                                const gchar *pubkey_hex)
{
  if (!server_url || !pubkey_hex)
    return NULL;

  gchar *normalized = gnostr_blossom_normalize_url(server_url);
  if (!normalized)
    return NULL;

  gchar *path = g_strdup_printf("%s/list/%s", normalized, pubkey_hex);
  g_free(normalized);

  return path;
}
