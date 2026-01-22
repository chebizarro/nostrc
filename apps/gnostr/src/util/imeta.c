/**
 * @file imeta.c
 * @brief NIP-92 imeta tag parser implementation
 */

#include "imeta.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <json-glib/json-glib.h>

/* Unused: Helper for strdup with NULL check
 * static char *safe_strdup(const char *s) { return s ? g_strdup(s) : NULL; }
 */

GnostrImeta *gnostr_imeta_new(void) {
  GnostrImeta *imeta = g_new0(GnostrImeta, 1);
  imeta->media_type = GNOSTR_MEDIA_TYPE_UNKNOWN;
  return imeta;
}

void gnostr_imeta_free(GnostrImeta *imeta) {
  if (!imeta) return;
  g_free(imeta->url);
  g_free(imeta->mime_type);
  g_free(imeta->alt);
  g_free(imeta->sha256);
  g_free(imeta->blurhash);
  if (imeta->fallback_urls) {
    for (size_t i = 0; i < imeta->fallback_count; i++) {
      g_free(imeta->fallback_urls[i]);
    }
    g_free(imeta->fallback_urls);
  }
  g_free(imeta);
}

GnostrImetaList *gnostr_imeta_list_new(void) {
  GnostrImetaList *list = g_new0(GnostrImetaList, 1);
  list->capacity = 4;
  list->items = g_new0(GnostrImeta *, list->capacity);
  return list;
}

void gnostr_imeta_list_free(GnostrImetaList *list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    gnostr_imeta_free(list->items[i]);
  }
  g_free(list->items);
  g_free(list);
}

void gnostr_imeta_list_append(GnostrImetaList *list, GnostrImeta *imeta) {
  if (!list || !imeta) return;

  if (list->count >= list->capacity) {
    list->capacity *= 2;
    list->items = g_renew(GnostrImeta *, list->items, list->capacity);
  }
  list->items[list->count++] = imeta;
}

GnostrMediaType gnostr_imeta_get_media_type(const char *mime_type) {
  if (!mime_type || !*mime_type) return GNOSTR_MEDIA_TYPE_UNKNOWN;

  if (g_str_has_prefix(mime_type, "image/")) {
    return GNOSTR_MEDIA_TYPE_IMAGE;
  }
  if (g_str_has_prefix(mime_type, "video/")) {
    return GNOSTR_MEDIA_TYPE_VIDEO;
  }
  if (g_str_has_prefix(mime_type, "audio/")) {
    return GNOSTR_MEDIA_TYPE_AUDIO;
  }

  return GNOSTR_MEDIA_TYPE_UNKNOWN;
}

GnostrMediaType gnostr_imeta_get_media_type_from_url(const char *url) {
  if (!url || !*url) return GNOSTR_MEDIA_TYPE_UNKNOWN;

  /* Extract extension from URL (handle query strings) */
  const char *ext_start = NULL;
  const char *query = strchr(url, '?');
  const char *hash = strchr(url, '#');
  const char *end = url + strlen(url);

  if (query && query < end) end = query;
  if (hash && hash < end) end = hash;

  /* Find last dot before end */
  for (const char *p = end - 1; p > url; p--) {
    if (*p == '.') {
      ext_start = p;
      break;
    }
    if (*p == '/') break;  /* Stop at path separator */
  }

  if (!ext_start) return GNOSTR_MEDIA_TYPE_UNKNOWN;

  /* Copy extension and convert to lowercase */
  size_t ext_len = end - ext_start;
  if (ext_len > 10) return GNOSTR_MEDIA_TYPE_UNKNOWN;  /* Sanity check */

  char ext[12];
  for (size_t i = 0; i < ext_len && i < 11; i++) {
    ext[i] = g_ascii_tolower(ext_start[i]);
  }
  ext[ext_len < 11 ? ext_len : 11] = '\0';

  /* Check image extensions */
  static const char *image_exts[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".svg", ".avif", ".heic", ".heif"
  };
  for (size_t i = 0; i < G_N_ELEMENTS(image_exts); i++) {
    if (strcmp(ext, image_exts[i]) == 0) {
      return GNOSTR_MEDIA_TYPE_IMAGE;
    }
  }

  /* Check video extensions */
  static const char *video_exts[] = {
    ".mp4", ".webm", ".mov", ".avi", ".mkv", ".m4v", ".ogv", ".3gp"
  };
  for (size_t i = 0; i < G_N_ELEMENTS(video_exts); i++) {
    if (strcmp(ext, video_exts[i]) == 0) {
      return GNOSTR_MEDIA_TYPE_VIDEO;
    }
  }

  /* Check audio extensions */
  static const char *audio_exts[] = {
    ".mp3", ".wav", ".ogg", ".flac", ".m4a", ".aac", ".opus"
  };
  for (size_t i = 0; i < G_N_ELEMENTS(audio_exts); i++) {
    if (strcmp(ext, audio_exts[i]) == 0) {
      return GNOSTR_MEDIA_TYPE_AUDIO;
    }
  }

  return GNOSTR_MEDIA_TYPE_UNKNOWN;
}

/**
 * Parse a single imeta field in format "key value" or "key"
 */
static void parse_imeta_field(GnostrImeta *imeta, const char *field) {
  if (!imeta || !field || !*field) return;

  /* Find first space separator */
  const char *space = strchr(field, ' ');

  if (!space) {
    /* No value, just key - ignore */
    return;
  }

  size_t key_len = space - field;
  const char *value = space + 1;

  /* Skip empty values */
  if (!*value) return;

  /* Match known keys */
  if (key_len == 3 && strncmp(field, "url", 3) == 0) {
    g_free(imeta->url);
    imeta->url = g_strdup(value);
  }
  else if (key_len == 1 && *field == 'm') {
    g_free(imeta->mime_type);
    imeta->mime_type = g_strdup(value);
    imeta->media_type = gnostr_imeta_get_media_type(value);
  }
  else if (key_len == 3 && strncmp(field, "dim", 3) == 0) {
    /* Parse "WIDTHxHEIGHT" */
    int w = 0, h = 0;
    if (sscanf(value, "%dx%d", &w, &h) == 2) {
      imeta->width = w;
      imeta->height = h;
    }
  }
  else if (key_len == 3 && strncmp(field, "alt", 3) == 0) {
    g_free(imeta->alt);
    imeta->alt = g_strdup(value);
  }
  else if (key_len == 1 && *field == 'x') {
    g_free(imeta->sha256);
    imeta->sha256 = g_strdup(value);
  }
  else if (key_len == 8 && strncmp(field, "blurhash", 8) == 0) {
    g_free(imeta->blurhash);
    imeta->blurhash = g_strdup(value);
  }
  else if (key_len == 8 && strncmp(field, "fallback", 8) == 0) {
    /* Append to fallback list */
    imeta->fallback_count++;
    imeta->fallback_urls = g_renew(char *, imeta->fallback_urls, imeta->fallback_count + 1);
    imeta->fallback_urls[imeta->fallback_count - 1] = g_strdup(value);
    imeta->fallback_urls[imeta->fallback_count] = NULL;
  }
}

GnostrImeta *gnostr_imeta_parse_tag(const char **tag_values, size_t n_values) {
  if (!tag_values || n_values < 2) return NULL;

  /* First element should be "imeta" */
  if (!tag_values[0] || strcmp(tag_values[0], "imeta") != 0) return NULL;

  GnostrImeta *imeta = gnostr_imeta_new();

  /* Parse remaining elements as "key value" pairs */
  for (size_t i = 1; i < n_values; i++) {
    if (tag_values[i]) {
      parse_imeta_field(imeta, tag_values[i]);
    }
  }

  /* URL is required */
  if (!imeta->url || !*imeta->url) {
    gnostr_imeta_free(imeta);
    return NULL;
  }

  /* If media type not determined from MIME, try URL */
  if (imeta->media_type == GNOSTR_MEDIA_TYPE_UNKNOWN) {
    imeta->media_type = gnostr_imeta_get_media_type_from_url(imeta->url);
  }

  return imeta;
}

GnostrImetaList *gnostr_imeta_parse_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  GError *error = NULL;
  JsonParser *parser = json_parser_new();

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_debug("imeta: Failed to parse tags JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags_array = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags_array);

  GnostrImetaList *list = gnostr_imeta_list_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags_array, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    /* Check if this is an imeta tag */
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name || strcmp(tag_name, "imeta") != 0) continue;

    /* Extract tag values */
    const char **values = g_new0(const char *, tag_len + 1);
    for (guint j = 0; j < tag_len; j++) {
      JsonNode *elem = json_array_get_element(tag, j);
      if (JSON_NODE_HOLDS_VALUE(elem)) {
        values[j] = json_node_get_string(elem);
      }
    }

    GnostrImeta *imeta = gnostr_imeta_parse_tag(values, tag_len);
    if (imeta) {
      gnostr_imeta_list_append(list, imeta);
    }

    g_free(values);
  }

  g_object_unref(parser);

  if (list->count == 0) {
    gnostr_imeta_list_free(list);
    return NULL;
  }

  return list;
}

GnostrImeta *gnostr_imeta_find_by_url(GnostrImetaList *list, const char *url) {
  if (!list || !url || !*url) return NULL;

  for (size_t i = 0; i < list->count; i++) {
    GnostrImeta *imeta = list->items[i];
    if (imeta && imeta->url && strcmp(imeta->url, url) == 0) {
      return imeta;
    }
  }

  return NULL;
}
