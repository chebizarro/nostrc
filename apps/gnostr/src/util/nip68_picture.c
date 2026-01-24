/**
 * @file nip68_picture.c
 * @brief NIP-68 Picture-first Feeds Parser Implementation
 */

#include "nip68_picture.h"
#include <json-glib/json-glib.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

GnostrPictureImage *gnostr_picture_image_new(void) {
  GnostrPictureImage *image = g_new0(GnostrPictureImage, 1);
  return image;
}

void gnostr_picture_image_free(GnostrPictureImage *image) {
  if (!image) return;

  g_free(image->url);
  g_free(image->mime_type);
  g_free(image->alt);
  g_free(image->sha256);
  g_free(image->blurhash);

  if (image->fallback_urls) {
    for (size_t i = 0; i < image->fallback_count; i++) {
      g_free(image->fallback_urls[i]);
    }
    g_free(image->fallback_urls);
  }

  g_free(image);
}

static GnostrPictureImage *gnostr_picture_image_copy(const GnostrPictureImage *src) {
  if (!src) return NULL;

  GnostrPictureImage *copy = gnostr_picture_image_new();
  copy->url = g_strdup(src->url);
  copy->mime_type = g_strdup(src->mime_type);
  copy->width = src->width;
  copy->height = src->height;
  copy->alt = g_strdup(src->alt);
  copy->sha256 = g_strdup(src->sha256);
  copy->blurhash = g_strdup(src->blurhash);

  if (src->fallback_urls && src->fallback_count > 0) {
    copy->fallback_count = src->fallback_count;
    copy->fallback_urls = g_new0(char *, src->fallback_count + 1);
    for (size_t i = 0; i < src->fallback_count; i++) {
      copy->fallback_urls[i] = g_strdup(src->fallback_urls[i]);
    }
  }

  return copy;
}

GnostrPictureMeta *gnostr_picture_meta_new(void) {
  GnostrPictureMeta *meta = g_new0(GnostrPictureMeta, 1);
  return meta;
}

void gnostr_picture_meta_free(GnostrPictureMeta *meta) {
  if (!meta) return;

  g_free(meta->event_id);
  g_free(meta->pubkey);
  g_free(meta->caption);
  g_free(meta->content_warning);

  if (meta->images) {
    for (size_t i = 0; i < meta->image_count; i++) {
      gnostr_picture_image_free(meta->images[i]);
    }
    g_free(meta->images);
  }

  if (meta->hashtags) {
    for (size_t i = 0; i < meta->hashtag_count; i++) {
      g_free(meta->hashtags[i]);
    }
    g_free(meta->hashtags);
  }

  if (meta->mentions) {
    for (size_t i = 0; i < meta->mention_count; i++) {
      g_free(meta->mentions[i]);
    }
    g_free(meta->mentions);
  }

  g_free(meta);
}

GnostrPictureMeta *gnostr_picture_meta_copy(const GnostrPictureMeta *meta) {
  if (!meta) return NULL;

  GnostrPictureMeta *copy = gnostr_picture_meta_new();
  copy->event_id = g_strdup(meta->event_id);
  copy->pubkey = g_strdup(meta->pubkey);
  copy->caption = g_strdup(meta->caption);
  copy->created_at = meta->created_at;
  copy->content_warning = g_strdup(meta->content_warning);
  copy->expiration = meta->expiration;
  copy->like_count = meta->like_count;
  copy->zap_count = meta->zap_count;
  copy->zap_amount = meta->zap_amount;
  copy->reply_count = meta->reply_count;
  copy->repost_count = meta->repost_count;

  /* Copy images */
  if (meta->images && meta->image_count > 0) {
    copy->image_count = meta->image_count;
    copy->images = g_new0(GnostrPictureImage *, meta->image_count);
    for (size_t i = 0; i < meta->image_count; i++) {
      copy->images[i] = gnostr_picture_image_copy(meta->images[i]);
    }
  }

  /* Copy hashtags */
  if (meta->hashtags && meta->hashtag_count > 0) {
    copy->hashtag_count = meta->hashtag_count;
    copy->hashtags = g_new0(char *, meta->hashtag_count + 1);
    for (size_t i = 0; i < meta->hashtag_count; i++) {
      copy->hashtags[i] = g_strdup(meta->hashtags[i]);
    }
  }

  /* Copy mentions */
  if (meta->mentions && meta->mention_count > 0) {
    copy->mention_count = meta->mention_count;
    copy->mentions = g_new0(char *, meta->mention_count + 1);
    for (size_t i = 0; i < meta->mention_count; i++) {
      copy->mentions[i] = g_strdup(meta->mentions[i]);
    }
  }

  return copy;
}

/**
 * Convert imeta structure to picture image
 */
static GnostrPictureImage *imeta_to_picture_image(const GnostrImeta *imeta) {
  if (!imeta || !imeta->url) return NULL;

  GnostrPictureImage *image = gnostr_picture_image_new();
  image->url = g_strdup(imeta->url);
  image->mime_type = g_strdup(imeta->mime_type);
  image->width = imeta->width;
  image->height = imeta->height;
  image->alt = g_strdup(imeta->alt);
  image->sha256 = g_strdup(imeta->sha256);
  image->blurhash = g_strdup(imeta->blurhash);

  if (imeta->fallback_urls && imeta->fallback_count > 0) {
    image->fallback_count = imeta->fallback_count;
    image->fallback_urls = g_new0(char *, imeta->fallback_count + 1);
    for (size_t i = 0; i < imeta->fallback_count; i++) {
      image->fallback_urls[i] = g_strdup(imeta->fallback_urls[i]);
    }
  }

  return image;
}

GnostrPictureMeta *gnostr_picture_parse_event(const char *event_id,
                                               const char *pubkey,
                                               const char *content,
                                               const char *tags_json,
                                               gint64 created_at) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-68: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-68: Tags is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrPictureMeta *meta = gnostr_picture_meta_new();
  meta->event_id = g_strdup(event_id);
  meta->pubkey = g_strdup(pubkey);
  meta->caption = g_strdup(content);
  meta->created_at = created_at;

  /* Temporary arrays for collecting data */
  GPtrArray *images_arr = g_ptr_array_new();
  GPtrArray *hashtags_arr = g_ptr_array_new();
  GPtrArray *mentions_arr = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 1) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name) continue;

    if (strcmp(tag_name, "imeta") == 0 && tag_len >= 2) {
      /* Parse imeta tag */
      const char **values = g_new0(const char *, tag_len + 1);
      for (guint j = 0; j < tag_len; j++) {
        JsonNode *elem = json_array_get_element(tag, j);
        if (JSON_NODE_HOLDS_VALUE(elem)) {
          values[j] = json_node_get_string(elem);
        }
      }

      GnostrImeta *imeta = gnostr_imeta_parse_tag(values, tag_len);
      if (imeta) {
        /* Only include image types */
        if (imeta->media_type == GNOSTR_MEDIA_TYPE_IMAGE ||
            imeta->media_type == GNOSTR_MEDIA_TYPE_UNKNOWN) {
          GnostrPictureImage *img = imeta_to_picture_image(imeta);
          if (img) {
            g_ptr_array_add(images_arr, img);
          }
        }
        gnostr_imeta_free(imeta);
      }
      g_free(values);

    } else if (strcmp(tag_name, "t") == 0 && tag_len >= 2) {
      /* Hashtag tag */
      const char *hashtag = json_array_get_string_element(tag, 1);
      if (hashtag && *hashtag) {
        /* Skip leading # if present */
        if (*hashtag == '#') hashtag++;
        if (*hashtag) {
          g_ptr_array_add(hashtags_arr, g_strdup(hashtag));
        }
      }

    } else if (strcmp(tag_name, "p") == 0 && tag_len >= 2) {
      /* Pubkey mention tag */
      const char *mention = json_array_get_string_element(tag, 1);
      if (mention && strlen(mention) == 64) {
        g_ptr_array_add(mentions_arr, g_strdup(mention));
      }

    } else if (strcmp(tag_name, "content-warning") == 0) {
      /* Content warning (NIP-36) */
      if (tag_len >= 2) {
        g_free(meta->content_warning);
        meta->content_warning = g_strdup(json_array_get_string_element(tag, 1));
      } else {
        /* Empty content-warning means "sensitive" */
        g_free(meta->content_warning);
        meta->content_warning = g_strdup("Sensitive content");
      }

    } else if (strcmp(tag_name, "expiration") == 0 && tag_len >= 2) {
      /* Expiration timestamp (NIP-40) */
      const char *exp_str = json_array_get_string_element(tag, 1);
      if (exp_str) {
        char *endptr;
        gint64 exp = g_ascii_strtoll(exp_str, &endptr, 10);
        if (endptr != exp_str && exp > 0) {
          meta->expiration = exp;
        }
      }
    }
  }

  /* Convert images array */
  meta->image_count = images_arr->len;
  if (images_arr->len > 0) {
    meta->images = g_new0(GnostrPictureImage *, images_arr->len);
    for (guint i = 0; i < images_arr->len; i++) {
      meta->images[i] = g_ptr_array_index(images_arr, i);
    }
  }
  g_ptr_array_free(images_arr, FALSE);

  /* Convert hashtags array */
  meta->hashtag_count = hashtags_arr->len;
  if (hashtags_arr->len > 0) {
    meta->hashtags = g_new0(char *, hashtags_arr->len + 1);
    for (guint i = 0; i < hashtags_arr->len; i++) {
      meta->hashtags[i] = g_ptr_array_index(hashtags_arr, i);
    }
  }
  g_ptr_array_free(hashtags_arr, FALSE);

  /* Convert mentions array */
  meta->mention_count = mentions_arr->len;
  if (mentions_arr->len > 0) {
    meta->mentions = g_new0(char *, mentions_arr->len + 1);
    for (guint i = 0; i < mentions_arr->len; i++) {
      meta->mentions[i] = g_ptr_array_index(mentions_arr, i);
    }
  }
  g_ptr_array_free(mentions_arr, FALSE);

  g_object_unref(parser);

  /* Picture events should have at least one image */
  if (meta->image_count == 0) {
    g_debug("NIP-68: Picture event has no images");
    gnostr_picture_meta_free(meta);
    return NULL;
  }

  return meta;
}

gboolean gnostr_picture_is_picture(int kind) {
  return kind == NOSTR_KIND_PICTURE;
}

const GnostrPictureImage *gnostr_picture_get_primary_image(const GnostrPictureMeta *meta) {
  if (!meta || !meta->images || meta->image_count == 0) {
    return NULL;
  }
  return meta->images[0];
}

const char *gnostr_picture_get_thumbnail_url(const GnostrPictureMeta *meta) {
  const GnostrPictureImage *img = gnostr_picture_get_primary_image(meta);
  return img ? img->url : NULL;
}

double gnostr_picture_get_aspect_ratio(const GnostrPictureMeta *meta) {
  const GnostrPictureImage *img = gnostr_picture_get_primary_image(meta);
  if (!img || img->width <= 0 || img->height <= 0) {
    return 1.0; /* Default to square */
  }
  return (double)img->width / (double)img->height;
}

gboolean gnostr_picture_has_content_warning(const GnostrPictureMeta *meta) {
  return meta && meta->content_warning && *meta->content_warning;
}

gboolean gnostr_picture_is_expired(const GnostrPictureMeta *meta) {
  if (!meta || meta->expiration <= 0) {
    return FALSE;
  }
  return meta->expiration < (gint64)time(NULL);
}

char *gnostr_picture_build_nevent(const GnostrPictureMeta *meta,
                                   const char **relays) {
  if (!meta || !meta->event_id) return NULL;

  NostrNeventConfig cfg = {
    .event_id = (char *)meta->event_id,
    .relays = (char **)relays,
    .relays_count = 0,
    .author = (char *)meta->pubkey,
    .kind = NOSTR_KIND_PICTURE
  };

  /* Count relays if provided */
  if (relays) {
    while (relays[cfg.relays_count]) cfg.relays_count++;
  }

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_nevent_config(&cfg, &ptr) != 0 || !ptr) {
    return NULL;
  }

  char *encoded = NULL;
  int result = nostr_pointer_to_bech32(ptr, &encoded);
  nostr_pointer_free(ptr);

  if (result != 0) {
    return NULL;
  }

  return encoded;
}

char *gnostr_picture_format_caption(const char *caption, size_t max_length) {
  if (!caption || !*caption) {
    return g_strdup("");
  }

  /* Normalize whitespace and newlines */
  GString *result = g_string_new(NULL);
  gboolean prev_space = TRUE; /* Start true to skip leading whitespace */

  for (const char *p = caption; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);

    if (g_unichar_isspace(c)) {
      if (!prev_space) {
        g_string_append_c(result, ' ');
        prev_space = TRUE;
      }
    } else {
      g_string_append_unichar(result, c);
      prev_space = FALSE;
    }

    /* Check length limit */
    if (max_length > 0 && result->len >= max_length) {
      /* Truncate and add ellipsis */
      while (result->len > max_length - 3) {
        /* Remove last UTF-8 character */
        char *last = g_utf8_prev_char(result->str + result->len);
        g_string_truncate(result, last - result->str);
      }
      g_string_append(result, "...");
      break;
    }
  }

  /* Remove trailing space */
  if (result->len > 0 && result->str[result->len - 1] == ' ') {
    g_string_truncate(result, result->len - 1);
  }

  return g_string_free(result, FALSE);
}

char **gnostr_picture_get_all_image_urls(const GnostrPictureMeta *meta,
                                          size_t *count) {
  if (count) *count = 0;

  if (!meta || !meta->images || meta->image_count == 0) {
    return NULL;
  }

  char **urls = g_new0(char *, meta->image_count + 1);

  for (size_t i = 0; i < meta->image_count; i++) {
    if (meta->images[i] && meta->images[i]->url) {
      urls[i] = g_strdup(meta->images[i]->url);
    }
  }

  if (count) *count = meta->image_count;

  return urls;
}
