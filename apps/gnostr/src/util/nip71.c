/*
 * nip71.c - NIP-71 Video Events Utilities
 */

#include "nip71.h"
#include <json-glib/json-glib.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <string.h>
#include <stdio.h>

GnostrVideoMeta *gnostr_video_meta_new(void) {
  GnostrVideoMeta *meta = g_new0(GnostrVideoMeta, 1);
  meta->duration = 0;
  meta->width = 0;
  meta->height = 0;
  meta->size = 0;
  meta->published_at = 0;
  meta->hashtags = NULL;
  meta->hashtags_count = 0;
  meta->orientation = GNOSTR_VIDEO_HORIZONTAL;
  return meta;
}

void gnostr_video_meta_free(GnostrVideoMeta *meta) {
  if (!meta) return;

  g_free(meta->d_tag);
  g_free(meta->url);
  g_free(meta->mime_type);
  g_free(meta->file_hash);
  g_free(meta->thumb_url);
  g_free(meta->title);
  g_free(meta->summary);
  g_free(meta->blurhash);

  if (meta->hashtags) {
    for (gsize i = 0; i < meta->hashtags_count; i++) {
      g_free(meta->hashtags[i]);
    }
    g_free(meta->hashtags);
  }

  g_free(meta);
}

GnostrVideoMeta *gnostr_video_parse_tags(const char *tags_json, int kind) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-71: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-71: Tags is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrVideoMeta *meta = gnostr_video_meta_new();
  GPtrArray *hashtags_arr = g_ptr_array_new();

  /* Set orientation from kind */
  meta->orientation = (kind == NOSTR_KIND_VIDEO_VERTICAL)
                        ? GNOSTR_VIDEO_VERTICAL
                        : GNOSTR_VIDEO_HORIZONTAL;

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    const char *tag_value = json_array_get_string_element(tag, 1);

    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "d") == 0) {
      g_free(meta->d_tag);
      meta->d_tag = g_strdup(tag_value);
    } else if (strcmp(tag_name, "url") == 0) {
      g_free(meta->url);
      meta->url = g_strdup(tag_value);
    } else if (strcmp(tag_name, "m") == 0) {
      g_free(meta->mime_type);
      meta->mime_type = g_strdup(tag_value);
    } else if (strcmp(tag_name, "x") == 0) {
      g_free(meta->file_hash);
      meta->file_hash = g_strdup(tag_value);
    } else if (strcmp(tag_name, "thumb") == 0) {
      g_free(meta->thumb_url);
      meta->thumb_url = g_strdup(tag_value);
    } else if (strcmp(tag_name, "title") == 0) {
      g_free(meta->title);
      meta->title = g_strdup(tag_value);
    } else if (strcmp(tag_name, "summary") == 0) {
      g_free(meta->summary);
      meta->summary = g_strdup(tag_value);
    } else if (strcmp(tag_name, "duration") == 0) {
      char *endptr;
      gint64 dur = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && dur >= 0) {
        meta->duration = dur;
      }
    } else if (strcmp(tag_name, "dim") == 0) {
      /* Parse dimensions as "WxH" */
      int w = 0, h = 0;
      if (sscanf(tag_value, "%dx%d", &w, &h) == 2) {
        meta->width = w;
        meta->height = h;
      }
    } else if (strcmp(tag_name, "size") == 0) {
      char *endptr;
      gint64 sz = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && sz >= 0) {
        meta->size = sz;
      }
    } else if (strcmp(tag_name, "blurhash") == 0) {
      g_free(meta->blurhash);
      meta->blurhash = g_strdup(tag_value);
    } else if (strcmp(tag_name, "published_at") == 0) {
      char *endptr;
      gint64 ts = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && *endptr == '\0' && ts > 0) {
        meta->published_at = ts;
      }
    } else if (strcmp(tag_name, "t") == 0) {
      /* Skip leading # if present */
      const char *hashtag = tag_value;
      if (*hashtag == '#') hashtag++;
      if (*hashtag) {
        g_ptr_array_add(hashtags_arr, g_strdup(hashtag));
      }
    }
  }

  /* Convert hashtags array */
  meta->hashtags_count = hashtags_arr->len;
  if (hashtags_arr->len > 0) {
    meta->hashtags = g_new0(gchar*, hashtags_arr->len + 1);
    for (guint i = 0; i < hashtags_arr->len; i++) {
      meta->hashtags[i] = g_ptr_array_index(hashtags_arr, i);
    }
    meta->hashtags[hashtags_arr->len] = NULL;
  }
  g_ptr_array_free(hashtags_arr, FALSE);

  g_object_unref(parser);

  /* Validate required fields - URL is required */
  if (!meta->url || !*meta->url) {
    g_warning("NIP-71: Video event missing required 'url' tag");
    gnostr_video_meta_free(meta);
    return NULL;
  }

  return meta;
}

gboolean gnostr_video_is_video(int kind) {
  return kind == NOSTR_KIND_VIDEO_HORIZONTAL || kind == NOSTR_KIND_VIDEO_VERTICAL;
}

gboolean gnostr_video_is_horizontal(int kind) {
  return kind == NOSTR_KIND_VIDEO_HORIZONTAL;
}

gboolean gnostr_video_is_vertical(int kind) {
  return kind == NOSTR_KIND_VIDEO_VERTICAL;
}

char *gnostr_video_format_duration(gint64 duration_seconds) {
  if (duration_seconds <= 0) return g_strdup("0:00");

  gint64 hours = duration_seconds / 3600;
  gint64 minutes = (duration_seconds % 3600) / 60;
  gint64 seconds = duration_seconds % 60;

  if (hours > 0) {
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
                           hours, minutes, seconds);
  } else {
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, minutes, seconds);
  }
}

char *gnostr_video_build_naddr(int kind, const char *pubkey_hex,
                                const char *d_tag, const char **relays) {
  if (!pubkey_hex || !d_tag) return NULL;

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_naddr(
    d_tag, pubkey_hex, kind,
    (const gchar *const *)relays, NULL);

  if (!n19) return NULL;

  return g_strdup(gnostr_nip19_get_bech32(n19));
}

char *gnostr_video_build_a_tag(int kind, const char *pubkey_hex,
                                const char *d_tag) {
  if (!pubkey_hex || !d_tag) return NULL;

  return g_strdup_printf("%d:%s:%s", kind, pubkey_hex, d_tag);
}

char *gnostr_video_event_create_tags(const GnostrVideoMeta *meta) {
  if (!meta || !meta->url) return NULL;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  /* URL tag (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "url");
  json_builder_add_string_value(builder, meta->url);
  json_builder_end_array(builder);

  /* MIME type tag */
  if (meta->mime_type && *meta->mime_type) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "m");
    json_builder_add_string_value(builder, meta->mime_type);
    json_builder_end_array(builder);
  }

  /* File hash tag */
  if (meta->file_hash && *meta->file_hash) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "x");
    json_builder_add_string_value(builder, meta->file_hash);
    json_builder_end_array(builder);
  }

  /* Thumbnail tag */
  if (meta->thumb_url && *meta->thumb_url) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "thumb");
    json_builder_add_string_value(builder, meta->thumb_url);
    json_builder_end_array(builder);
  }

  /* Title tag */
  if (meta->title && *meta->title) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "title");
    json_builder_add_string_value(builder, meta->title);
    json_builder_end_array(builder);
  }

  /* Summary tag */
  if (meta->summary && *meta->summary) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "summary");
    json_builder_add_string_value(builder, meta->summary);
    json_builder_end_array(builder);
  }

  /* Duration tag */
  if (meta->duration > 0) {
    char duration_str[32];
    g_snprintf(duration_str, sizeof(duration_str), "%" G_GINT64_FORMAT, meta->duration);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "duration");
    json_builder_add_string_value(builder, duration_str);
    json_builder_end_array(builder);
  }

  /* Dimensions tag */
  if (meta->width > 0 && meta->height > 0) {
    char dim_str[32];
    g_snprintf(dim_str, sizeof(dim_str), "%dx%d", meta->width, meta->height);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "dim");
    json_builder_add_string_value(builder, dim_str);
    json_builder_end_array(builder);
  }

  /* Size tag */
  if (meta->size > 0) {
    char size_str[32];
    g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT, meta->size);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "size");
    json_builder_add_string_value(builder, size_str);
    json_builder_end_array(builder);
  }

  /* Blurhash tag */
  if (meta->blurhash && *meta->blurhash) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "blurhash");
    json_builder_add_string_value(builder, meta->blurhash);
    json_builder_end_array(builder);
  }

  /* D tag (for addressable events) */
  if (meta->d_tag && *meta->d_tag) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "d");
    json_builder_add_string_value(builder, meta->d_tag);
    json_builder_end_array(builder);
  }

  /* Hashtag tags */
  if (meta->hashtags) {
    for (gsize i = 0; i < meta->hashtags_count && meta->hashtags[i]; i++) {
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "t");
      json_builder_add_string_value(builder, meta->hashtags[i]);
      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);

  JsonGenerator *generator = json_generator_new();
  JsonNode *root_node = json_builder_get_root(builder);
  json_generator_set_root(generator, root_node);

  char *tags_json = json_generator_to_data(generator, NULL);

  json_node_free(root_node);
  g_object_unref(generator);
  g_object_unref(builder);

  return tags_json;
}

const char *gnostr_video_detect_mime_type(const char *file_path) {
  if (!file_path || !*file_path) return NULL;

  /* Convert to lowercase for comparison */
  gchar *lower = g_ascii_strdown(file_path, -1);
  const char *result = NULL;

  if (g_str_has_suffix(lower, ".mp4") || g_str_has_suffix(lower, ".m4v")) {
    result = "video/mp4";
  } else if (g_str_has_suffix(lower, ".webm")) {
    result = "video/webm";
  } else if (g_str_has_suffix(lower, ".mov") || g_str_has_suffix(lower, ".qt")) {
    result = "video/quicktime";
  } else if (g_str_has_suffix(lower, ".avi")) {
    result = "video/x-msvideo";
  } else if (g_str_has_suffix(lower, ".mkv")) {
    result = "video/x-matroska";
  } else if (g_str_has_suffix(lower, ".wmv")) {
    result = "video/x-ms-wmv";
  } else if (g_str_has_suffix(lower, ".flv")) {
    result = "video/x-flv";
  } else if (g_str_has_suffix(lower, ".ogv") || g_str_has_suffix(lower, ".ogg")) {
    result = "video/ogg";
  } else if (g_str_has_suffix(lower, ".3gp")) {
    result = "video/3gpp";
  } else if (g_str_has_suffix(lower, ".ts") || g_str_has_suffix(lower, ".m2ts")) {
    result = "video/mp2t";
  }

  g_free(lower);
  return result;
}

gboolean gnostr_video_is_video_mime(const char *mime_type) {
  if (!mime_type || !*mime_type) return FALSE;

  /* Check if MIME type starts with "video/" */
  return g_str_has_prefix(mime_type, "video/");
}

GnostrVideoOrientation gnostr_video_detect_orientation(int width, int height) {
  /* If height is greater than width, it's vertical (portrait) */
  if (height > width) {
    return GNOSTR_VIDEO_VERTICAL;
  }
  return GNOSTR_VIDEO_HORIZONTAL;
}

int gnostr_video_get_event_kind(GnostrVideoOrientation orientation) {
  return (orientation == GNOSTR_VIDEO_VERTICAL)
           ? NOSTR_KIND_VIDEO_VERTICAL
           : NOSTR_KIND_VIDEO_HORIZONTAL;
}
