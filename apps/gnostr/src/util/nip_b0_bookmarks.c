/*
 * nip_b0_bookmarks.c - NIP-B0 Web Bookmarks Utilities Implementation
 *
 * Implements parsing and building of kind 176 web bookmark events.
 */

#include "nip_b0_bookmarks.h"
#include "nostr_json.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"
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

  /* Parse with NostrEvent API */
  NostrEvent event = {0};
  if (!nostr_event_deserialize_compact(&event, event_json)) {
    g_warning("NIP-B0: Failed to parse event JSON");
    return NULL;
  }

  /* Verify kind */
  if (nostr_event_get_kind(&event) != NIPB0_KIND_BOOKMARK) {
    g_debug("NIP-B0: Not a web bookmark event (kind=%d)",
            nostr_event_get_kind(&event));
    return NULL;
  }

  GnostrWebBookmark *b = gnostr_web_bookmark_new();

  /* Extract event metadata */
  char *id_str = nostr_event_get_id(&event);
  if (id_str) {
    b->event_id = id_str;  /* takes ownership */
  }

  const char *pubkey_str = nostr_event_get_pubkey(&event);
  if (pubkey_str) {
    b->pubkey = g_strdup(pubkey_str);
  }

  b->created_at = nostr_event_get_created_at(&event);

  /* Extract content (notes) */
  const char *content = nostr_event_get_content(&event);
  if (content && *content) {
    b->notes = g_strdup(content);
  }

  /* Parse tags using NostrTags API */
  NostrTags *tags = nostr_event_get_tags(&event);
  if (tags) {
    /* First pass: count "t" tags */
    gsize t_count = 0;
    size_t tag_count = nostr_tags_size(tags);
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;
      const char *tag_name = nostr_tag_get(tag, 0);
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
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag || nostr_tag_size(tag) < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
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

  /* URL is required */
  if (!b->url || !*b->url) {
    g_warning("NIP-B0: Web bookmark missing required URL");
    gnostr_web_bookmark_free(b);
    return NULL;
  }

  return b;
}

/* Context for counting t tags in first pass */
typedef struct {
  gsize t_count;
} BookmarkTagCountCtx;

static gboolean count_t_tags_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  BookmarkTagCountCtx *ctx = (BookmarkTagCountCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (tag_name && strcmp(tag_name, "t") == 0) {
      ctx->t_count++;
    }
    g_free(tag_name);
  }
  return true;
}

/* Context for parsing tags in second pass */
typedef struct {
  GnostrWebBookmark *b;
  gsize t_count;
} BookmarkTagParseCtx;

static gboolean parse_bookmark_tag_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  BookmarkTagParseCtx *ctx = (BookmarkTagParseCtx *)user_data;

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

  if (strcmp(tag_name, "r") == 0) {
    g_free(ctx->b->url);
    ctx->b->url = tag_value;
    tag_value = NULL;
  }
  else if (strcmp(tag_name, "title") == 0) {
    g_free(ctx->b->title);
    ctx->b->title = tag_value;
    tag_value = NULL;
  }
  else if (strcmp(tag_name, "description") == 0) {
    g_free(ctx->b->description);
    ctx->b->description = tag_value;
    tag_value = NULL;
  }
  else if (strcmp(tag_name, "image") == 0) {
    g_free(ctx->b->image);
    ctx->b->image = tag_value;
    tag_value = NULL;
  }
  else if (strcmp(tag_name, "t") == 0) {
    if (ctx->b->tags && ctx->b->tag_count < ctx->t_count) {
      ctx->b->tags[ctx->b->tag_count++] = tag_value;
      tag_value = NULL;
    }
  }
  else if (strcmp(tag_name, "published_at") == 0) {
    gint64 ts = g_ascii_strtoll(tag_value, NULL, 10);
    if (ts > 0) {
      ctx->b->published_at = ts;
    }
  }

  g_free(tag_name);
  g_free(tag_value);
  return true;
}

GnostrWebBookmark *gnostr_web_bookmark_parse_tags(const char *tags_json,
                                                   const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  if (!gnostr_json_is_array_str(tags_json)) {
    return NULL;
  }

  GnostrWebBookmark *b = gnostr_web_bookmark_new();
  if (content && *content) {
    b->notes = g_strdup(content);
  }

  /* First pass: count "t" tags */
  BookmarkTagCountCtx count_ctx = {0};
  gnostr_json_array_foreach_root(tags_json, count_t_tags_cb, &count_ctx);

  /* Allocate tags array if needed */
  if (count_ctx.t_count > 0) {
    b->tags = g_new0(gchar *, count_ctx.t_count + 1);
    b->tag_count = 0;
  }

  /* Second pass: parse all tags */
  BookmarkTagParseCtx parse_ctx = { .b = b, .t_count = count_ctx.t_count };
  gnostr_json_array_foreach_root(tags_json, parse_bookmark_tag_cb, &parse_ctx);

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

  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_array(builder);

  /* Add URL tag (required) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "r");
  gnostr_json_builder_add_string(builder, bookmark->url);
  gnostr_json_builder_end_array(builder);

  /* Add title tag if provided */
  if (bookmark->title && *bookmark->title) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "title");
    gnostr_json_builder_add_string(builder, bookmark->title);
    gnostr_json_builder_end_array(builder);
  }

  /* Add description tag if provided */
  if (bookmark->description && *bookmark->description) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "description");
    gnostr_json_builder_add_string(builder, bookmark->description);
    gnostr_json_builder_end_array(builder);
  }

  /* Add image tag if provided */
  if (bookmark->image && *bookmark->image) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "image");
    gnostr_json_builder_add_string(builder, bookmark->image);
    gnostr_json_builder_end_array(builder);
  }

  /* Add t tags for categories */
  if (bookmark->tags && bookmark->tag_count > 0) {
    for (gsize i = 0; i < bookmark->tag_count; i++) {
      if (bookmark->tags[i] && *bookmark->tags[i]) {
        gnostr_json_builder_begin_array(builder);
        gnostr_json_builder_add_string(builder, "t");
        gnostr_json_builder_add_string(builder, bookmark->tags[i]);
        gnostr_json_builder_end_array(builder);
      }
    }
  }

  /* Add published_at tag if provided */
  if (bookmark->published_at > 0) {
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, bookmark->published_at);
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "published_at");
    gnostr_json_builder_add_string(builder, ts_str);
    gnostr_json_builder_end_array(builder);
    g_free(ts_str);
  }

  gnostr_json_builder_end_array(builder);
  char *tags_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

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

  /* Build tags first using the helper */
  gchar *tags_json = gnostr_web_bookmark_build_tags(bookmark);
  if (!tags_json) {
    return NULL;
  }

  /* Build the unsigned event using GNostrJsonBuilder */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* kind */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NIPB0_KIND_BOOKMARK);

  /* created_at */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  /* content */
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, bookmark->notes ? bookmark->notes : "");

  /* tags (inject as raw JSON) */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_add_raw(builder, tags_json);
  g_free(tags_json);

  gnostr_json_builder_end_object(builder);
  char *event_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

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
