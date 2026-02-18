/**
 * @file nip_a0_voice.c
 * @brief NIP-A0 (160) Voice Messages Utilities Implementation
 */

#include "nip_a0_voice.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

GnostrVoiceMessage *gnostr_voice_message_new(void) {
  GnostrVoiceMessage *msg = g_new0(GnostrVoiceMessage, 1);
  msg->duration_secs = 0;
  msg->size_bytes = 0;
  return msg;
}

void gnostr_voice_message_free(GnostrVoiceMessage *msg) {
  if (!msg) return;

  g_free(msg->audio_url);
  g_free(msg->mime_type);
  g_free(msg->blurhash);
  g_free(msg->content_hash);
  g_free(msg->transcript);
  g_free(msg->reply_to_id);
  g_free(msg->reply_to_relay);
  g_free(msg->recipient);

  g_free(msg);
}

GnostrVoiceMessage *gnostr_voice_message_copy(const GnostrVoiceMessage *msg) {
  if (!msg) return NULL;

  GnostrVoiceMessage *copy = gnostr_voice_message_new();
  copy->audio_url = g_strdup(msg->audio_url);
  copy->mime_type = g_strdup(msg->mime_type);
  copy->duration_secs = msg->duration_secs;
  copy->size_bytes = msg->size_bytes;
  copy->blurhash = g_strdup(msg->blurhash);
  copy->content_hash = g_strdup(msg->content_hash);
  copy->transcript = g_strdup(msg->transcript);
  copy->reply_to_id = g_strdup(msg->reply_to_id);
  copy->reply_to_relay = g_strdup(msg->reply_to_relay);
  copy->recipient = g_strdup(msg->recipient);

  return copy;
}

GnostrVoiceMessage *gnostr_voice_message_parse_tags(const char *tags_json,
                                                     const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-A0: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-A0: Tags is not an array");
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrVoiceMessage *msg = gnostr_voice_message_new();

  /* Set transcript from content if provided */
  if (content && *content) {
    msg->transcript = g_strdup(content);
  }

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    const char *tag_value = json_array_get_string_element(tag, 1);

    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "url") == 0) {
      /* Audio URL (required) */
      g_free(msg->audio_url);
      msg->audio_url = g_strdup(tag_value);

    } else if (strcmp(tag_name, "m") == 0) {
      /* MIME type */
      g_free(msg->mime_type);
      msg->mime_type = g_strdup(tag_value);

    } else if (strcmp(tag_name, "duration") == 0) {
      /* Duration in seconds */
      char *endptr;
      gint64 dur = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && dur >= 0) {
        msg->duration_secs = dur;
      }

    } else if (strcmp(tag_name, "size") == 0) {
      /* File size in bytes */
      char *endptr;
      gint64 sz = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && sz >= 0) {
        msg->size_bytes = sz;
      }

    } else if (strcmp(tag_name, "blurhash") == 0) {
      /* Waveform visualization hash */
      g_free(msg->blurhash);
      msg->blurhash = g_strdup(tag_value);

    } else if (strcmp(tag_name, "x") == 0) {
      /* SHA-256 content hash */
      g_free(msg->content_hash);
      msg->content_hash = g_strdup(tag_value);

    } else if (strcmp(tag_name, "e") == 0) {
      /* Reply to event - only set if not already set (first e tag) */
      if (!msg->reply_to_id) {
        msg->reply_to_id = g_strdup(tag_value);
        /* Get optional relay URL (third element) */
        if (tag_len >= 3) {
          const char *relay = json_array_get_string_element(tag, 2);
          if (relay && *relay) {
            msg->reply_to_relay = g_strdup(relay);
          }
        }
      }

    } else if (strcmp(tag_name, "p") == 0) {
      /* Recipient/mention pubkey - only set if not already set */
      if (!msg->recipient && strlen(tag_value) == 64) {
        msg->recipient = g_strdup(tag_value);
      }
    }
  }


  /* Validate required fields - URL is required */
  if (!msg->audio_url || !*msg->audio_url) {
    g_warning("NIP-A0: Voice message missing required 'url' tag");
    gnostr_voice_message_free(msg);
    return NULL;
  }

  /* Validate URL format */
  if (!gnostr_voice_validate_url(msg->audio_url)) {
    g_warning("NIP-A0: Voice message has invalid URL: %s", msg->audio_url);
    gnostr_voice_message_free(msg);
    return NULL;
  }

  return msg;
}

char *gnostr_voice_message_build_tags(const GnostrVoiceMessage *msg) {
  if (!msg || !msg->audio_url) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* URL tag (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "url");
  json_builder_add_string_value(builder, msg->audio_url);
  json_builder_end_array(builder);

  /* MIME type tag */
  if (msg->mime_type && *msg->mime_type) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "m");
    json_builder_add_string_value(builder, msg->mime_type);
    json_builder_end_array(builder);
  }

  /* Duration tag */
  if (msg->duration_secs > 0) {
    char duration_str[32];
    g_snprintf(duration_str, sizeof(duration_str), "%" G_GINT64_FORMAT,
               msg->duration_secs);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "duration");
    json_builder_add_string_value(builder, duration_str);
    json_builder_end_array(builder);
  }

  /* Size tag */
  if (msg->size_bytes > 0) {
    char size_str[32];
    g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT,
               msg->size_bytes);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "size");
    json_builder_add_string_value(builder, size_str);
    json_builder_end_array(builder);
  }

  /* Blurhash tag (waveform visualization) */
  if (msg->blurhash && *msg->blurhash) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "blurhash");
    json_builder_add_string_value(builder, msg->blurhash);
    json_builder_end_array(builder);
  }

  /* Content hash tag */
  if (msg->content_hash && *msg->content_hash) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "x");
    json_builder_add_string_value(builder, msg->content_hash);
    json_builder_end_array(builder);
  }

  /* Reply event tag */
  if (msg->reply_to_id && *msg->reply_to_id) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "e");
    json_builder_add_string_value(builder, msg->reply_to_id);
    if (msg->reply_to_relay && *msg->reply_to_relay) {
      json_builder_add_string_value(builder, msg->reply_to_relay);
    }
    json_builder_end_array(builder);
  }

  /* Recipient pubkey tag */
  if (msg->recipient && *msg->recipient) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, msg->recipient);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root_node = json_builder_get_root(builder);
  json_generator_set_root(generator, root_node);

  char *tags_json = json_generator_to_data(generator, NULL);

  json_node_free(root_node);

  return tags_json;
}

gboolean gnostr_voice_is_voice(int kind) {
  return kind == NIPA0_KIND_VOICE;
}

gboolean gnostr_voice_validate_url(const char *url) {
  if (!url || !*url) return FALSE;

  /* Check for valid scheme */
  if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
    return FALSE;
  }

  /* Check minimum length (scheme + at least one char for host) */
  size_t len = strlen(url);
  if (len < 10) return FALSE; /* "http://a.b" minimum */

  /* Check for a host after the scheme */
  const char *host_start = url + (g_str_has_prefix(url, "https://") ? 8 : 7);
  if (!*host_start || *host_start == '/') {
    return FALSE;
  }

  return TRUE;
}

gboolean gnostr_voice_validate_mime_type(const char *mime_type) {
  if (!mime_type || !*mime_type) return FALSE;

  /* Must start with "audio/" */
  if (!g_str_has_prefix(mime_type, "audio/")) {
    return FALSE;
  }

  /* Check for known audio subtypes */
  const char *subtype = mime_type + 6; /* Skip "audio/" */
  if (!*subtype) return FALSE;

  /* Common audio types */
  static const char *valid_subtypes[] = {
    "webm", "ogg", "opus", "mp3", "mpeg", "mp4", "m4a", "aac",
    "wav", "x-wav", "wave", "flac", "x-flac", "3gpp", "3gpp2",
    "amr", "x-aiff", "aiff", "basic", NULL
  };

  for (int i = 0; valid_subtypes[i]; i++) {
    if (g_ascii_strcasecmp(subtype, valid_subtypes[i]) == 0) {
      return TRUE;
    }
  }

  /* Accept any audio MIME type even if not in our list */
  return TRUE;
}

gboolean gnostr_voice_is_audio_mime(const char *mime_type) {
  if (!mime_type || !*mime_type) return FALSE;
  return g_str_has_prefix(mime_type, "audio/");
}

const char *gnostr_voice_detect_mime_type(const char *file_path) {
  if (!file_path || !*file_path) return NULL;

  /* Convert to lowercase for comparison */
  gchar *lower = g_ascii_strdown(file_path, -1);
  const char *result = NULL;

  if (g_str_has_suffix(lower, ".webm")) {
    result = "audio/webm";
  } else if (g_str_has_suffix(lower, ".ogg") || g_str_has_suffix(lower, ".oga")) {
    result = "audio/ogg";
  } else if (g_str_has_suffix(lower, ".opus")) {
    result = "audio/opus";
  } else if (g_str_has_suffix(lower, ".mp3")) {
    result = "audio/mpeg";
  } else if (g_str_has_suffix(lower, ".m4a") || g_str_has_suffix(lower, ".mp4")) {
    result = "audio/mp4";
  } else if (g_str_has_suffix(lower, ".aac")) {
    result = "audio/aac";
  } else if (g_str_has_suffix(lower, ".wav") || g_str_has_suffix(lower, ".wave")) {
    result = "audio/wav";
  } else if (g_str_has_suffix(lower, ".flac")) {
    result = "audio/flac";
  } else if (g_str_has_suffix(lower, ".aiff") || g_str_has_suffix(lower, ".aif")) {
    result = "audio/aiff";
  } else if (g_str_has_suffix(lower, ".3gp")) {
    result = "audio/3gpp";
  } else if (g_str_has_suffix(lower, ".amr")) {
    result = "audio/amr";
  } else if (g_str_has_suffix(lower, ".wma")) {
    result = "audio/x-ms-wma";
  }

  g_free(lower);
  return result;
}

char *gnostr_voice_format_duration(gint64 duration_seconds) {
  if (duration_seconds < 0) duration_seconds = 0;

  gint64 hours = duration_seconds / 3600;
  gint64 minutes = (duration_seconds % 3600) / 60;
  gint64 seconds = duration_seconds % 60;

  if (hours > 0) {
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
                           ":%02" G_GINT64_FORMAT,
                           hours, minutes, seconds);
  } else {
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
                           minutes, seconds);
  }
}

char *gnostr_voice_format_duration_short(gint64 duration_seconds) {
  if (duration_seconds < 0) duration_seconds = 0;

  gint64 hours = duration_seconds / 3600;
  gint64 minutes = (duration_seconds % 3600) / 60;
  gint64 seconds = duration_seconds % 60;

  if (hours > 0) {
    if (minutes > 0) {
      return g_strdup_printf("%" G_GINT64_FORMAT "h%" G_GINT64_FORMAT "m",
                             hours, minutes);
    } else {
      return g_strdup_printf("%" G_GINT64_FORMAT "h", hours);
    }
  } else if (minutes > 0) {
    if (seconds > 0) {
      return g_strdup_printf("%" G_GINT64_FORMAT "m%" G_GINT64_FORMAT "s",
                             minutes, seconds);
    } else {
      return g_strdup_printf("%" G_GINT64_FORMAT "m", minutes);
    }
  } else {
    return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
  }
}

char *gnostr_voice_format_size(gint64 size_bytes) {
  if (size_bytes < 0) size_bytes = 0;

  if (size_bytes < 1024) {
    return g_strdup_printf("%" G_GINT64_FORMAT " B", size_bytes);
  } else if (size_bytes < 1024 * 1024) {
    double kb = (double)size_bytes / 1024.0;
    return g_strdup_printf("%.1f KB", kb);
  } else if (size_bytes < 1024 * 1024 * 1024) {
    double mb = (double)size_bytes / (1024.0 * 1024.0);
    return g_strdup_printf("%.1f MB", mb);
  } else {
    double gb = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
    return g_strdup_printf("%.2f GB", gb);
  }
}

int gnostr_voice_get_kind(void) {
  return NIPA0_KIND_VOICE;
}
