/* nd-file-entry.c - WebDAV file metadata backed by NIP-94 (kind 1063)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-file-entry.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#define ND_FILE_ERROR (nd_file_error_quark())
G_DEFINE_QUARK(nd-file-error-quark, nd_file_error)

enum {
  ND_FILE_ERROR_PARSE = 1,
  ND_FILE_ERROR_MISSING_FIELD
};

/* ---- Free ---- */

void
nd_file_entry_free(NdFileEntry *entry)
{
  if (entry == NULL) return;
  g_free(entry->path);
  g_free(entry->sha256);
  g_free(entry->mime_type);
  g_clear_pointer(&entry->content, g_bytes_unref);
  g_free(entry->blossom_url);
  g_free(entry->pubkey);
  g_free(entry->display_name);
  g_free(entry);
}

/* ---- SHA-256 ---- */

/**
 * Compute hex-encoded SHA-256 of raw bytes.
 */
static gchar *
sha256_hex(const guint8 *data, gsize len)
{
  g_autoptr(GChecksum) cs = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(cs, data, (gssize)len);
  return g_strdup(g_checksum_get_string(cs));
}

/* ---- MIME guessing ---- */

static const gchar *
guess_mime_type(const gchar *path)
{
  if (path == NULL)
    return "application/octet-stream";

  if (g_str_has_suffix(path, ".txt"))
    return "text/plain";
  if (g_str_has_suffix(path, ".md"))
    return "text/markdown";
  if (g_str_has_suffix(path, ".html") || g_str_has_suffix(path, ".htm"))
    return "text/html";
  if (g_str_has_suffix(path, ".css"))
    return "text/css";
  if (g_str_has_suffix(path, ".js"))
    return "application/javascript";
  if (g_str_has_suffix(path, ".json"))
    return "application/json";
  if (g_str_has_suffix(path, ".xml"))
    return "application/xml";
  if (g_str_has_suffix(path, ".png"))
    return "image/png";
  if (g_str_has_suffix(path, ".jpg") || g_str_has_suffix(path, ".jpeg"))
    return "image/jpeg";
  if (g_str_has_suffix(path, ".gif"))
    return "image/gif";
  if (g_str_has_suffix(path, ".svg"))
    return "image/svg+xml";
  if (g_str_has_suffix(path, ".webp"))
    return "image/webp";
  if (g_str_has_suffix(path, ".pdf"))
    return "application/pdf";
  if (g_str_has_suffix(path, ".zip"))
    return "application/zip";
  if (g_str_has_suffix(path, ".tar.gz") || g_str_has_suffix(path, ".tgz"))
    return "application/gzip";

  return "application/octet-stream";
}

/**
 * Extract display name (filename) from a path.
 */
static gchar *
extract_display_name(const gchar *path)
{
  if (path == NULL)
    return g_strdup("unnamed");

  const gchar *slash = strrchr(path, '/');
  if (slash != NULL && slash[1] != '\0')
    return g_strdup(slash + 1);

  return g_strdup(path);
}

/* ---- Constructor ---- */

NdFileEntry *
nd_file_entry_new(const gchar *path,
                  GBytes      *content,
                  const gchar *mime_type)
{
  g_return_val_if_fail(path != NULL, NULL);
  g_return_val_if_fail(content != NULL, NULL);

  NdFileEntry *entry = g_new0(NdFileEntry, 1);
  entry->path = g_strdup(path);
  entry->content = g_bytes_ref(content);

  gsize data_len = 0;
  const guint8 *data = g_bytes_get_data(content, &data_len);
  entry->sha256 = sha256_hex(data, data_len);
  entry->size = data_len;

  entry->mime_type = g_strdup(mime_type ? mime_type : guess_mime_type(path));
  entry->display_name = extract_display_name(path);
  entry->modified_at = (gint64)time(NULL);
  entry->created_at = entry->modified_at;

  return entry;
}

/* ---- ETag ---- */

gchar *
nd_file_entry_compute_etag(const NdFileEntry *entry)
{
  g_return_val_if_fail(entry != NULL, NULL);
  g_return_val_if_fail(entry->sha256 != NULL, NULL);

  /* Use first 16 hex chars of SHA-256 as ETag */
  gchar hash_prefix[17];
  memcpy(hash_prefix, entry->sha256, 16);
  hash_prefix[16] = '\0';

  return g_strdup_printf("\"%s\"", hash_prefix);
}

/* ---- NIP-94 JSON ---- */

gchar *
nd_file_entry_to_nip94_json(const NdFileEntry *entry)
{
  g_return_val_if_fail(entry != NULL, NULL);

  g_autoptr(JsonBuilder) b = json_builder_new();

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, ND_NIP94_KIND);

  /* content: file description (empty for raw uploads) */
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, "");

  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, entry->created_at);

  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);

  /* ["x", <sha256>] */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "x");
  json_builder_add_string_value(b, entry->sha256);
  json_builder_end_array(b);

  /* ["m", <mime>] */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "m");
  json_builder_add_string_value(b, entry->mime_type);
  json_builder_end_array(b);

  /* ["size", <bytes>] */
  {
    g_autofree gchar *size_str = g_strdup_printf("%" G_GSIZE_FORMAT, entry->size);
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "size");
    json_builder_add_string_value(b, size_str);
    json_builder_end_array(b);
  }

  /* ["path", <relative_path>] */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "path");
  json_builder_add_string_value(b, entry->path);
  json_builder_end_array(b);

  /* ["url", <blossom_url>] — only if uploaded */
  if (entry->blossom_url != NULL) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "url");
    json_builder_add_string_value(b, entry->blossom_url);
    json_builder_end_array(b);
  }

  json_builder_end_array(b); /* tags */
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);

  return json_generator_to_data(gen, NULL);
}

NdFileEntry *
nd_file_entry_from_nip94_json(const gchar *json_str,
                              GError     **error)
{
  g_return_val_if_fail(json_str != NULL, NULL);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_str, -1, error))
    return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, ND_FILE_ERROR, ND_FILE_ERROR_PARSE,
                "NIP-94 JSON root is not an object");
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  gint64 kind = json_object_get_int_member_with_default(obj, "kind", 0);
  if (kind != ND_NIP94_KIND) {
    g_set_error(error, ND_FILE_ERROR, ND_FILE_ERROR_PARSE,
                "Expected kind %d, got %" G_GINT64_FORMAT,
                ND_NIP94_KIND, kind);
    return NULL;
  }

  NdFileEntry *entry = g_new0(NdFileEntry, 1);
  entry->created_at = json_object_get_int_member_with_default(obj,
                        "created_at", 0);

  if (json_object_has_member(obj, "pubkey"))
    entry->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));

  /* Parse tags */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (tags != NULL) {
    guint n_tags = json_array_get_length(tags);
    for (guint i = 0; i < n_tags; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (tag == NULL || json_array_get_length(tag) < 2)
        continue;

      const gchar *key = json_array_get_string_element(tag, 0);
      const gchar *val = json_array_get_string_element(tag, 1);
      if (key == NULL || val == NULL)
        continue;

      if (g_str_equal(key, "x"))
        entry->sha256 = g_strdup(val);
      else if (g_str_equal(key, "m"))
        entry->mime_type = g_strdup(val);
      else if (g_str_equal(key, "size"))
        entry->size = (gsize)g_ascii_strtoull(val, NULL, 10);
      else if (g_str_equal(key, "path"))
        entry->path = g_strdup(val);
      else if (g_str_equal(key, "url"))
        entry->blossom_url = g_strdup(val);
    }
  }

  /* Validate required fields */
  if (entry->sha256 == NULL || entry->path == NULL) {
    g_set_error(error, ND_FILE_ERROR, ND_FILE_ERROR_MISSING_FIELD,
                "NIP-94 event missing required x (sha256) or path tag");
    nd_file_entry_free(entry);
    return NULL;
  }

  entry->display_name = extract_display_name(entry->path);
  entry->modified_at = entry->created_at;

  return entry;
}
