/*
 * nip_b0_bookmarks.c - NIP-B0 Web Bookmarks Utilities Implementation
 *
 * Implements parsing and building of kind 176 web bookmark events.
 */

#include "nip_b0_bookmarks.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

GnostrWebBookmark *gnostr_web_bookmark_new(void) {
  GnostrWebBookmark *b = g_new0(GnostrWebBookmark, 1);
  return b;
}

void gnostr_web_bookmark_free(GnostrWebBookmark *bookmark) {
  if (!bookmark) return;

  g_free(bookmark->url);
  g_free(bookmark->title);
  g_free(bookmark->description);
  g_free(bookmark->image);
  g_free(bookmark->notes);
  g_free(bookmark->event_id);
  g_free(bookmark->pubkey);

  /* Free tags array */
  if (bookmark->tags) {
    for (gsize i = 0; i < bookmark->tag_count; i++) {
      g_free(bookmark->tags[i]);
    }
    g_free(bookmark->tags);
  }

  g_free(bookmark);
}

gboolean gnostr_web_bookmark_validate_url(const char *url) {
  if (!url || !*url) return FALSE;

  /* Must start with http:// or https:// */
  if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
    return FALSE;
  }

  /* Use GLib's URI parser for validation */
  GError *error = NULL;
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
  if (!uri) {
    g_clear_error(&error);
    return FALSE;
  }

  /* Must have a host */
  const char *host = g_uri_get_host(uri);
  gboolean valid = (host && *host);

  g_uri_unref(uri);
  return valid;
}

GnostrWebBookmark *gnostr_web_bookmark_parse_json(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("NIP-B0: Failed to parse event JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIPB0_KIND_BOOKMARK) {
    g_debug("NIP-B0: Not a web bookmark event (kind=%lld)",
            kind_val ? (long long)json_integer_value(kind_val) : -1);
    json_decref(root);
    return NULL;
  }

  GnostrWebBookmark *b = gnostr_web_bookmark_new();

  /* Extract event metadata */
  json_t *id_val = json_object_get(root, "id");
  if (json_is_string(id_val)) {
    b->event_id = g_strdup(json_string_value(id_val));
  }

  json_t *pubkey_val = json_object_get(root, "pubkey");
  if (json_is_string(pubkey_val)) {
    b->pubkey = g_strdup(json_string_value(pubkey_val));
  }

  json_t *created_at = json_object_get(root, "created_at");
  if (json_is_integer(created_at)) {
    b->created_at = json_integer_value(created_at);
  }

  /* Extract content (notes) */
  json_t *content_val = json_object_get(root, "content");
  if (json_is_string(content_val)) {
    const char *content = json_string_value(content_val);
    if (content && *content) {
      b->notes = g_strdup(content);
    }
  }

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (json_is_array(tags)) {
    /* First pass: count "t" tags */
    gsize t_count = 0;
    size_t idx;
    json_t *tag;
    json_array_foreach(tags, idx, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
      const char *tag_name = json_string_value(json_array_get(tag, 0));
      if (tag_name && strcmp(tag_name, "t") == 0) {
        t_count++;
      }
    }

    /* Allocate tags array if needed */
    if (t_count > 0) {
      b->tags = g_new0(gchar *, t_count + 1);
      b->tag_count = 0;
    }

    /* Second pass: parse all tags */
    json_array_foreach(tags, idx, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "r") == 0) {
        /* URL - required */
        g_free(b->url);
        b->url = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "title") == 0) {
        g_free(b->title);
        b->title = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "description") == 0) {
        g_free(b->description);
        b->description = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "image") == 0) {
        g_free(b->image);
        b->image = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "t") == 0) {
        /* Tag/category - repeatable */
        if (b->tags && b->tag_count < t_count) {
          b->tags[b->tag_count++] = g_strdup(tag_value);
        }
      }
      else if (strcmp(tag_name, "published_at") == 0) {
        /* Parse timestamp */
        gint64 ts = g_ascii_strtoll(tag_value, NULL, 10);
        if (ts > 0) {
          b->published_at = ts;
        }
      }
    }
  }

  json_decref(root);

  /* URL is required */
  if (!b->url || !*b->url) {
    g_warning("NIP-B0: Web bookmark missing required URL");
    gnostr_web_bookmark_free(b);
    return NULL;
  }

  return b;
}

GnostrWebBookmark *gnostr_web_bookmark_parse_tags(const char *tags_json,
                                                   const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  GnostrWebBookmark *b = gnostr_web_bookmark_new();
  if (content && *content) {
    b->notes = g_strdup(content);
  }

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags || !json_is_array(tags)) {
    if (tags) json_decref(tags);
    gnostr_web_bookmark_free(b);
    return NULL;
  }

  /* First pass: count "t" tags */
  gsize t_count = 0;
  size_t idx;
  json_t *tag;
  json_array_foreach(tags, idx, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;
    const char *tag_name = json_string_value(json_array_get(tag, 0));
    if (tag_name && strcmp(tag_name, "t") == 0) {
      t_count++;
    }
  }

  /* Allocate tags array if needed */
  if (t_count > 0) {
    b->tags = g_new0(gchar *, t_count + 1);
    b->tag_count = 0;
  }

  /* Second pass: parse all tags */
  json_array_foreach(tags, idx, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

    const char *tag_name = json_string_value(json_array_get(tag, 0));
    const char *tag_value = json_string_value(json_array_get(tag, 1));
    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "r") == 0) {
      g_free(b->url);
      b->url = g_strdup(tag_value);
    }
    else if (strcmp(tag_name, "title") == 0) {
      g_free(b->title);
      b->title = g_strdup(tag_value);
    }
    else if (strcmp(tag_name, "description") == 0) {
      g_free(b->description);
      b->description = g_strdup(tag_value);
    }
    else if (strcmp(tag_name, "image") == 0) {
      g_free(b->image);
      b->image = g_strdup(tag_value);
    }
    else if (strcmp(tag_name, "t") == 0) {
      if (b->tags && b->tag_count < t_count) {
        b->tags[b->tag_count++] = g_strdup(tag_value);
      }
    }
    else if (strcmp(tag_name, "published_at") == 0) {
      gint64 ts = g_ascii_strtoll(tag_value, NULL, 10);
      if (ts > 0) {
        b->published_at = ts;
      }
    }
  }

  json_decref(tags);

  /* URL is required */
  if (!b->url || !*b->url) {
    gnostr_web_bookmark_free(b);
    return NULL;
  }

  return b;
}

gchar *gnostr_web_bookmark_build_tags(const GnostrWebBookmark *bookmark) {
  if (!bookmark || !bookmark->url || !*bookmark->url) {
    g_warning("NIP-B0: Cannot build tags without URL");
    return NULL;
  }

  /* Validate URL */
  if (!gnostr_web_bookmark_validate_url(bookmark->url)) {
    g_warning("NIP-B0: Invalid URL: %s", bookmark->url);
    return NULL;
  }

  json_t *tags = json_array();

  /* Add URL tag (required) */
  json_t *r_tag = json_array();
  json_array_append_new(r_tag, json_string("r"));
  json_array_append_new(r_tag, json_string(bookmark->url));
  json_array_append(tags, r_tag);
  json_decref(r_tag);

  /* Add title tag if provided */
  if (bookmark->title && *bookmark->title) {
    json_t *title_tag = json_array();
    json_array_append_new(title_tag, json_string("title"));
    json_array_append_new(title_tag, json_string(bookmark->title));
    json_array_append(tags, title_tag);
    json_decref(title_tag);
  }

  /* Add description tag if provided */
  if (bookmark->description && *bookmark->description) {
    json_t *desc_tag = json_array();
    json_array_append_new(desc_tag, json_string("description"));
    json_array_append_new(desc_tag, json_string(bookmark->description));
    json_array_append(tags, desc_tag);
    json_decref(desc_tag);
  }

  /* Add image tag if provided */
  if (bookmark->image && *bookmark->image) {
    json_t *img_tag = json_array();
    json_array_append_new(img_tag, json_string("image"));
    json_array_append_new(img_tag, json_string(bookmark->image));
    json_array_append(tags, img_tag);
    json_decref(img_tag);
  }

  /* Add t tags for categories */
  if (bookmark->tags && bookmark->tag_count > 0) {
    for (gsize i = 0; i < bookmark->tag_count; i++) {
      if (bookmark->tags[i] && *bookmark->tags[i]) {
        json_t *t_tag = json_array();
        json_array_append_new(t_tag, json_string("t"));
        json_array_append_new(t_tag, json_string(bookmark->tags[i]));
        json_array_append(tags, t_tag);
        json_decref(t_tag);
      }
    }
  }

  /* Add published_at tag if provided */
  if (bookmark->published_at > 0) {
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, bookmark->published_at);
    json_t *pub_tag = json_array();
    json_array_append_new(pub_tag, json_string("published_at"));
    json_array_append_new(pub_tag, json_string(ts_str));
    json_array_append(tags, pub_tag);
    json_decref(pub_tag);
    g_free(ts_str);
  }

  char *tags_json = json_dumps(tags, JSON_COMPACT);
  json_decref(tags);

  return tags_json;
}

gchar *gnostr_web_bookmark_build_event_json(const GnostrWebBookmark *bookmark) {
  if (!bookmark || !bookmark->url || !*bookmark->url) {
    g_warning("NIP-B0: Cannot create bookmark event without URL");
    return NULL;
  }

  /* Validate URL */
  if (!gnostr_web_bookmark_validate_url(bookmark->url)) {
    g_warning("NIP-B0: Invalid URL: %s", bookmark->url);
    return NULL;
  }

  json_t *tags = json_array();

  /* Add URL tag (required) */
  json_t *r_tag = json_array();
  json_array_append_new(r_tag, json_string("r"));
  json_array_append_new(r_tag, json_string(bookmark->url));
  json_array_append(tags, r_tag);
  json_decref(r_tag);

  /* Add title tag if provided */
  if (bookmark->title && *bookmark->title) {
    json_t *title_tag = json_array();
    json_array_append_new(title_tag, json_string("title"));
    json_array_append_new(title_tag, json_string(bookmark->title));
    json_array_append(tags, title_tag);
    json_decref(title_tag);
  }

  /* Add description tag if provided */
  if (bookmark->description && *bookmark->description) {
    json_t *desc_tag = json_array();
    json_array_append_new(desc_tag, json_string("description"));
    json_array_append_new(desc_tag, json_string(bookmark->description));
    json_array_append(tags, desc_tag);
    json_decref(desc_tag);
  }

  /* Add image tag if provided */
  if (bookmark->image && *bookmark->image) {
    json_t *img_tag = json_array();
    json_array_append_new(img_tag, json_string("image"));
    json_array_append_new(img_tag, json_string(bookmark->image));
    json_array_append(tags, img_tag);
    json_decref(img_tag);
  }

  /* Add t tags for categories */
  if (bookmark->tags && bookmark->tag_count > 0) {
    for (gsize i = 0; i < bookmark->tag_count; i++) {
      if (bookmark->tags[i] && *bookmark->tags[i]) {
        json_t *t_tag = json_array();
        json_array_append_new(t_tag, json_string("t"));
        json_array_append_new(t_tag, json_string(bookmark->tags[i]));
        json_array_append(tags, t_tag);
        json_decref(t_tag);
      }
    }
  }

  /* Add published_at tag */
  gint64 pub_time = bookmark->published_at > 0 ? bookmark->published_at : (gint64)time(NULL);
  gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, pub_time);
  json_t *pub_tag = json_array();
  json_array_append_new(pub_tag, json_string("published_at"));
  json_array_append_new(pub_tag, json_string(ts_str));
  json_array_append(tags, pub_tag);
  json_decref(pub_tag);
  g_free(ts_str);

  /* Build the unsigned event */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(NIPB0_KIND_BOOKMARK));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(bookmark->notes ? bookmark->notes : ""));
  json_object_set_new(event_obj, "tags", tags);

  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  return event_json;
}

void gnostr_web_bookmark_add_tag(GnostrWebBookmark *bookmark, const char *tag) {
  if (!bookmark || !tag || !*tag) return;

  /* Check for duplicate */
  if (gnostr_web_bookmark_has_tag(bookmark, tag)) {
    return;
  }

  /* Grow the array */
  bookmark->tag_count++;
  bookmark->tags = g_renew(gchar *, bookmark->tags, bookmark->tag_count + 1);
  bookmark->tags[bookmark->tag_count - 1] = g_strdup(tag);
  bookmark->tags[bookmark->tag_count] = NULL;
}

gboolean gnostr_web_bookmark_remove_tag(GnostrWebBookmark *bookmark, const char *tag) {
  if (!bookmark || !tag || !*tag || !bookmark->tags) return FALSE;

  for (gsize i = 0; i < bookmark->tag_count; i++) {
    if (bookmark->tags[i] && strcmp(bookmark->tags[i], tag) == 0) {
      g_free(bookmark->tags[i]);

      /* Shift remaining elements */
      for (gsize j = i; j < bookmark->tag_count - 1; j++) {
        bookmark->tags[j] = bookmark->tags[j + 1];
      }
      bookmark->tag_count--;
      bookmark->tags[bookmark->tag_count] = NULL;
      return TRUE;
    }
  }

  return FALSE;
}

gboolean gnostr_web_bookmark_has_tag(const GnostrWebBookmark *bookmark, const char *tag) {
  if (!bookmark || !tag || !*tag || !bookmark->tags) return FALSE;

  for (gsize i = 0; i < bookmark->tag_count; i++) {
    if (bookmark->tags[i] && strcmp(bookmark->tags[i], tag) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

GnostrWebBookmark *gnostr_web_bookmark_copy(const GnostrWebBookmark *bookmark) {
  if (!bookmark) return NULL;

  GnostrWebBookmark *copy = gnostr_web_bookmark_new();

  copy->url = g_strdup(bookmark->url);
  copy->title = g_strdup(bookmark->title);
  copy->description = g_strdup(bookmark->description);
  copy->image = g_strdup(bookmark->image);
  copy->notes = g_strdup(bookmark->notes);
  copy->event_id = g_strdup(bookmark->event_id);
  copy->pubkey = g_strdup(bookmark->pubkey);
  copy->published_at = bookmark->published_at;
  copy->created_at = bookmark->created_at;

  /* Copy tags */
  if (bookmark->tags && bookmark->tag_count > 0) {
    copy->tags = g_new0(gchar *, bookmark->tag_count + 1);
    for (gsize i = 0; i < bookmark->tag_count; i++) {
      copy->tags[i] = g_strdup(bookmark->tags[i]);
    }
    copy->tag_count = bookmark->tag_count;
  }

  return copy;
}
