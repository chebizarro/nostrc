/**
 * @file custom_emoji.c
 * @brief NIP-30 Custom Emoji implementation
 *
 * Implements parsing of emoji tags and caching of emoji images.
 * nostrc-3nj: Migrated from json-glib to NostrJsonInterface
 */

#include "custom_emoji.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* ========== Emoji Tag Parsing ========== */

GnostrCustomEmoji *gnostr_custom_emoji_new(const char *shortcode, const char *url) {
  if (!shortcode || !url) return NULL;
  GnostrCustomEmoji *emoji = g_new0(GnostrCustomEmoji, 1);
  emoji->shortcode = g_strdup(shortcode);
  emoji->url = g_strdup(url);
  return emoji;
}

void gnostr_custom_emoji_free(GnostrCustomEmoji *emoji) {
  if (!emoji) return;
  g_free(emoji->shortcode);
  g_free(emoji->url);
  g_free(emoji);
}

GnostrEmojiList *gnostr_emoji_list_new(void) {
  GnostrEmojiList *list = g_new0(GnostrEmojiList, 1);
  list->capacity = 8;
  list->items = g_new0(GnostrCustomEmoji *, list->capacity);
  return list;
}

void gnostr_emoji_list_free(GnostrEmojiList *list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    gnostr_custom_emoji_free(list->items[i]);
  }
  g_free(list->items);
  g_free(list);
}

void gnostr_emoji_list_append(GnostrEmojiList *list, GnostrCustomEmoji *emoji) {
  if (!list || !emoji) return;

  if (list->count >= list->capacity) {
    list->capacity *= 2;
    list->items = g_renew(GnostrCustomEmoji *, list->items, list->capacity);
  }
  list->items[list->count++] = emoji;
}

/* nostrc-3nj: Callback context for iterating over tags array */
typedef struct {
  GnostrEmojiList *list;
} EmojiParseContext;

/* nostrc-3nj: Callback for outer tags array iteration */
static bool parse_emoji_tag_cb(size_t index, const char *element_json, void *user_data) {
  (void)index;
  EmojiParseContext *ctx = (EmojiParseContext *)user_data;

  /* Each element should be an array (a tag) */
  if (!element_json || !nostr_json_is_array_str(element_json)) return true;

  /* Get tag length - NIP-30 emoji tag format: ["emoji", "shortcode", "url"] */
  size_t tag_len = 0;
  if (nostr_json_get_array_length(element_json, NULL, &tag_len) != 0 || tag_len < 3) {
    return true;  /* Skip invalid tags, continue iteration */
  }

  /* Check if first element is "emoji" */
  char *tag_name = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }

  if (strcmp(tag_name, "emoji") != 0) {
    free(tag_name);
    return true;  /* Not an emoji tag, continue */
  }
  free(tag_name);

  /* Get shortcode (index 1) */
  char *shortcode = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 1, &shortcode) != 0 || !shortcode || !*shortcode) {
    free(shortcode);
    return true;
  }

  /* Get URL (index 2) */
  char *url = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 2, &url) != 0 || !url || !*url) {
    free(shortcode);
    free(url);
    return true;
  }

  /* Validate URL starts with http:// or https:// */
  if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
    g_debug("emoji: Skipping invalid URL for shortcode '%s': %s", shortcode, url);
    free(shortcode);
    free(url);
    return true;
  }

  /* Create emoji entry */
  GnostrCustomEmoji *emoji = gnostr_custom_emoji_new(shortcode, url);
  if (emoji) {
    gnostr_emoji_list_append(ctx->list, emoji);
    g_debug("emoji: Parsed custom emoji :%s: -> %s", shortcode, url);
  }

  free(shortcode);
  free(url);

  return true;  /* Continue iteration */
}

GnostrEmojiList *gnostr_emoji_parse_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  /* Validate it's an array */
  if (!nostr_json_is_array_str(tags_json)) {
    g_debug("emoji: Tags JSON is not an array");
    return NULL;
  }

  GnostrEmojiList *list = gnostr_emoji_list_new();

  /* nostrc-3nj: Use NostrJsonInterface to iterate over tags array */
  EmojiParseContext ctx = { .list = list };
  nostr_json_array_foreach_root(tags_json, parse_emoji_tag_cb, &ctx);

  if (list->count == 0) {
    gnostr_emoji_list_free(list);
    return NULL;
  }

  g_debug("emoji: Parsed %zu custom emoji tags", list->count);
  return list;
}

GnostrCustomEmoji *gnostr_emoji_find_by_shortcode(GnostrEmojiList *list, const char *shortcode) {
  if (!list || !shortcode || !*shortcode) return NULL;

  for (size_t i = 0; i < list->count; i++) {
    GnostrCustomEmoji *emoji = list->items[i];
    if (emoji && emoji->shortcode && strcmp(emoji->shortcode, shortcode) == 0) {
      return emoji;
    }
  }

  return NULL;
}

/**
 * Find and extract a :shortcode: from text starting at position p.
 * Returns the shortcode (without colons) if found, or NULL.
 * Sets *end_pos to point after the closing colon.
 */
static gchar *extract_shortcode(const char *p, const char **end_pos) {
  if (!p || *p != ':') return NULL;

  const char *start = p + 1;  /* Skip opening colon */
  const char *end = start;

  /* Find closing colon - shortcode must be alphanumeric/underscore only */
  while (*end && *end != ':' && *end != ' ' && *end != '\n' && *end != '\t') {
    char c = *end;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return NULL;  /* Invalid character in shortcode */
    }
    end++;
  }

  if (*end != ':') return NULL;  /* No closing colon found */
  if (end == start) return NULL;  /* Empty shortcode */

  *end_pos = end + 1;  /* Point after closing colon */
  return g_strndup(start, end - start);
}

gchar *gnostr_emoji_replace_shortcodes(const char *content, GnostrEmojiList *emoji_list) {
  if (!content || !emoji_list || emoji_list->count == 0) return NULL;

  GString *result = g_string_new("");
  const char *p = content;
  gboolean had_replacement = FALSE;

  while (*p) {
    if (*p == ':') {
      const char *end_pos = NULL;
      gchar *shortcode = extract_shortcode(p, &end_pos);

      if (shortcode) {
        GnostrCustomEmoji *emoji = gnostr_emoji_find_by_shortcode(emoji_list, shortcode);
        if (emoji) {
          /* Found a matching custom emoji - insert a placeholder image reference
           * GTK4 GtkLabel doesn't support inline images directly, so we use
           * a special span class that can be detected during rendering.
           * We also store the URL in a title attribute for retrieval. */
          gchar *esc_url = g_markup_escape_text(emoji->url, -1);
          gchar *esc_shortcode = g_markup_escape_text(emoji->shortcode, -1);

          /* Use a span with custom attributes that our rendering code can detect.
           * The emoji-url attribute stores the URL for image loading. */
          g_string_append_printf(result,
            "<span font_features=\"emoji-shortcode\" title=\"%s\">:%s:</span>",
            esc_url, esc_shortcode);

          g_free(esc_url);
          g_free(esc_shortcode);
          had_replacement = TRUE;
          p = end_pos;
          g_free(shortcode);
          continue;
        }
        g_free(shortcode);
      }
    }

    /* Escape the character for Pango markup */
    if (*p == '<') {
      g_string_append(result, "&lt;");
    } else if (*p == '>') {
      g_string_append(result, "&gt;");
    } else if (*p == '&') {
      g_string_append(result, "&amp;");
    } else {
      g_string_append_c(result, *p);
    }
    p++;
  }

  if (!had_replacement) {
    g_string_free(result, TRUE);
    return NULL;
  }

  return g_string_free(result, FALSE);
}

/* ========== Emoji Image Cache ========== */

/* Similar structure to avatar cache but optimized for small emoji images */

typedef struct _EmojiCacheCtx {
  char *url;            /* owned */
  GtkWidget *target;    /* weak (optional widget to update) */
} EmojiCacheCtx;

/* Cache state */
static GHashTable *s_emoji_texture_cache = NULL;  /* key: char* url, value: GdkTexture* */
static char *s_emoji_cache_dir = NULL;            /* disk cache dir */
static GQueue *s_emoji_lru = NULL;                /* queue of char* url (head=oldest) */
static GHashTable *s_emoji_lru_nodes = NULL;      /* key=url -> GList* node */
static guint s_emoji_cap = 0;                     /* max resident textures */
static guint s_emoji_size = 0;                    /* target decode size in pixels */
static gboolean s_emoji_config_initialized = FALSE;
static gboolean s_emoji_log_started = FALSE;

/* Metrics */
static GnostrEmojiCacheMetrics s_emoji_metrics = {0};

/* Forward declarations */
static void emoji_ctx_free(EmojiCacheCtx *c);
static void emoji_lru_touch(const char *url);
static void emoji_lru_insert(const char *url);
static void emoji_lru_evict_if_needed(void);
static const char *ensure_emoji_cache_dir(void);
static char *emoji_path_for_url(const char *url);
static GdkTexture *try_load_emoji_from_disk(const char *url);
#ifdef HAVE_SOUP3
static void on_emoji_http_done(GObject *source, GAsyncResult *res, gpointer user_data);
#endif

/* str_has_prefix_http is now provided by utils.h */

/* Read configuration from environment variables */
static void emoji_init_config(void) {
  if (s_emoji_config_initialized) return;
  s_emoji_config_initialized = TRUE;

  /* GNOSTR_EMOJI_MEM_CAP: max in-memory textures (default 500 - more than avatars since emojis are smaller) */
  const char *cap_env = g_getenv("GNOSTR_EMOJI_MEM_CAP");
  if (cap_env && *cap_env) {
    long val = strtol(cap_env, NULL, 10);
    if (val > 0 && val < 100000) {
      s_emoji_cap = (guint)val;
      g_message("[EMOJI_CACHE] Using GNOSTR_EMOJI_MEM_CAP=%u", s_emoji_cap);
    }
  }
  if (s_emoji_cap == 0) s_emoji_cap = 500;

  /* GNOSTR_EMOJI_SIZE: target decode size in pixels (default 24 for inline emoji) */
  const char *size_env = g_getenv("GNOSTR_EMOJI_SIZE");
  if (size_env && *size_env) {
    long val = strtol(size_env, NULL, 10);
    if (val >= 16 && val <= 128) {
      s_emoji_size = (guint)val;
      g_message("[EMOJI_CACHE] Using GNOSTR_EMOJI_SIZE=%u", s_emoji_size);
    }
  }
  if (s_emoji_size == 0) s_emoji_size = 24;

  g_message("[EMOJI_CACHE] Config: cap=%u size=%upx", s_emoji_cap, s_emoji_size);
}

static gboolean emoji_cache_log_cb(gpointer data) {
  (void)data;
  guint msz = s_emoji_texture_cache ? g_hash_table_size(s_emoji_texture_cache) : 0;
  guint lsz = s_emoji_lru_nodes ? g_hash_table_size(s_emoji_lru_nodes) : 0;
  g_message("[EMOJI_CACHE] mem=%u lru=%u cap=%u size=%upx", msz, lsz, s_emoji_cap, s_emoji_size);
  gnostr_emoji_cache_metrics_log();
  return TRUE;
}

static void ensure_emoji_cache(void) {
  emoji_init_config();
  if (!s_emoji_texture_cache) {
    s_emoji_texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  }
  if (!s_emoji_lru) s_emoji_lru = g_queue_new();
  if (!s_emoji_lru_nodes) {
    s_emoji_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  if (!s_emoji_log_started) {
    s_emoji_log_started = TRUE;
    /* LEGITIMATE TIMEOUT - Periodic cache stats logging (60s intervals).
     * nostrc-b0h: Audited - diagnostic logging is appropriate. */
    g_timeout_add_seconds(60, emoji_cache_log_cb, NULL);
  }
}

static void emoji_lru_touch(const char *url) {
  if (!url || !s_emoji_lru || !s_emoji_lru_nodes) return;
  GList *node = g_hash_table_lookup(s_emoji_lru_nodes, url);
  if (!node) return;
  g_queue_unlink(s_emoji_lru, node);
  g_queue_push_tail_link(s_emoji_lru, node);
}

static void emoji_lru_insert(const char *url) {
  if (!url || !s_emoji_lru || !s_emoji_lru_nodes) return;
  if (g_hash_table_contains(s_emoji_lru_nodes, url)) {
    emoji_lru_touch(url);
    return;
  }
  char *k = g_strdup(url);
  GList *node = g_list_alloc();
  node->data = k;
  g_queue_push_tail_link(s_emoji_lru, node);
  g_hash_table_insert(s_emoji_lru_nodes, g_strdup(url), node);
}

static void emoji_lru_evict_if_needed(void) {
  if (!s_emoji_lru || !s_emoji_lru_nodes || !s_emoji_texture_cache) return;
  while (g_hash_table_size(s_emoji_lru_nodes) > s_emoji_cap) {
    GList *old = s_emoji_lru->head;
    if (!old) break;
    char *old_url = (char*)old->data;
    g_queue_unlink(s_emoji_lru, old);
    g_list_free_1(old);
    g_hash_table_remove(s_emoji_lru_nodes, old_url);
    g_hash_table_remove(s_emoji_texture_cache, old_url);
    g_free(old_url);
  }
}

static const char *ensure_emoji_cache_dir(void) {
  if (s_emoji_cache_dir) return s_emoji_cache_dir;
  const char *base = g_get_user_cache_dir();
  if (!base || !*base) base = ".";
  char *dir = g_build_filename(base, "gnostr", "emoji", NULL);
  if (g_mkdir_with_parents(dir, 0700) != 0 && errno != EEXIST) {
    g_warning("emoji cache: mkdir failed (%s): %s", dir, g_strerror(errno));
  }
  s_emoji_cache_dir = dir;
  g_message("emoji cache: using dir %s", s_emoji_cache_dir);
  return s_emoji_cache_dir;
}

static char *emoji_path_for_url(const char *url) {
  if (!url || !*url) return NULL;
  const char *dir = ensure_emoji_cache_dir();
  g_autofree char *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
  return g_build_filename(dir, hex, NULL);
}

/* Create GdkTexture from GdkPixbuf */
static GdkTexture *texture_new_from_pixbuf(GdkPixbuf *pixbuf) {
  g_return_val_if_fail(pixbuf != NULL, NULL);

  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  GBytes *bytes = gdk_pixbuf_read_pixel_bytes(pixbuf);

  GdkMemoryFormat format = has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
  GdkTexture *texture = gdk_memory_texture_new(width, height, format, bytes, rowstride);
  g_bytes_unref(bytes);

  return texture;
}

/* Decode emoji image at bounded size using GdkPixbuf */
static GdkTexture *emoji_texture_from_file_scaled(const char *path, GError **error) {
  g_return_val_if_fail(path != NULL, NULL);

  GdkPixbuf *loaded = gdk_pixbuf_new_from_file_at_scale(
    path,
    s_emoji_size,
    s_emoji_size,
    TRUE,  /* preserve aspect ratio */
    error
  );

  if (!loaded) return NULL;

  GdkTexture *texture = texture_new_from_pixbuf(loaded);
  g_object_unref(loaded);

  return texture;
}

/* Decode emoji image from bytes at bounded size */
static GdkTexture *emoji_texture_from_bytes_scaled(GBytes *bytes, GError **error) {
  g_return_val_if_fail(bytes != NULL, NULL);

  GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
  if (!stream) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create input stream");
    return NULL;
  }

  GdkPixbuf *loaded = gdk_pixbuf_new_from_stream_at_scale(
    stream,
    s_emoji_size,
    s_emoji_size,
    TRUE,  /* preserve aspect ratio */
    NULL,  /* cancellable */
    error
  );

  g_object_unref(stream);

  if (!loaded) return NULL;

  GdkTexture *texture = texture_new_from_pixbuf(loaded);
  g_object_unref(loaded);

  return texture;
}

static GdkTexture *try_load_emoji_from_disk(const char *url) {
  if (!url || !*url) return NULL;
  g_autofree char *path = emoji_path_for_url(url);
  if (!path) return NULL;
  if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    g_debug("emoji disk: miss for url=%s", url);
    return NULL;
  }
  GError *err = NULL;
  GdkTexture *tex = emoji_texture_from_file_scaled(path, &err);
  if (!tex) {
    g_warning("emoji disk: INVALID cached file %s: %s - deleting", path, err ? err->message : "unknown");
    g_clear_error(&err);
    unlink(path);
    return NULL;
  }
  g_debug("emoji disk: hit for url=%s", url);
  s_emoji_metrics.disk_cache_hits++;
  return tex;
}

static void emoji_ctx_free(EmojiCacheCtx *c) {
  if (!c) return;
  if (c->target) g_object_unref(c->target);
  g_free(c->url);
  g_free(c);
}

void gnostr_emoji_cache_prefetch(const char *url) {
  if (!url || !*url) return;
  if (!str_has_prefix_http(url)) return;

  s_emoji_metrics.requests_total++;
  ensure_emoji_cache();

  /* Already in memory? */
  if (g_hash_table_lookup(s_emoji_texture_cache, url)) {
    emoji_lru_touch(url);
    return;
  }

  /* Disk cached? Promote to memory */
  GdkTexture *disk_tex = try_load_emoji_from_disk(url);
  if (disk_tex) {
    g_hash_table_replace(s_emoji_texture_cache, g_strdup(url), g_object_ref(disk_tex));
    emoji_lru_insert(url);
    emoji_lru_evict_if_needed();
    g_object_unref(disk_tex);
    g_debug("emoji prefetch: promoted disk->mem url=%s", url);
    return;
  }

#ifdef HAVE_SOUP3
  /* Fetch asynchronously - use shared session to avoid per-request session overhead */
  SoupMessage *msg = soup_message_new("GET", url);
  EmojiCacheCtx *ctx = g_new0(EmojiCacheCtx, 1);
  ctx->url = g_strdup(url);
  ctx->target = NULL;

  g_debug("emoji prefetch: fetching url=%s", url);
  s_emoji_metrics.http_start++;
  soup_session_send_and_read_async(gnostr_get_shared_soup_session(), msg, G_PRIORITY_DEFAULT, NULL, on_emoji_http_done, ctx);
  g_object_unref(msg);
#endif
}

GdkTexture *gnostr_emoji_try_load_cached(const char *url) {
  if (!url || !*url) return NULL;
  if (!str_has_prefix_http(url)) return NULL;

  ensure_emoji_cache();

  /* Check memory cache */
  GdkTexture *mem_tex = g_hash_table_lookup(s_emoji_texture_cache, url);
  if (mem_tex) {
    s_emoji_metrics.mem_cache_hits++;
    emoji_lru_touch(url);
    return g_object_ref(mem_tex);
  }

  /* Check disk cache */
  GdkTexture *disk_tex = try_load_emoji_from_disk(url);
  if (disk_tex) {
    g_hash_table_replace(s_emoji_texture_cache, g_strdup(url), g_object_ref(disk_tex));
    emoji_lru_insert(url);
    emoji_lru_evict_if_needed();
    return disk_tex;
  }

  return NULL;
}

#ifdef HAVE_SOUP3
static void on_emoji_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  EmojiCacheCtx *ctx = (EmojiCacheCtx*)user_data;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (!bytes) {
    s_emoji_metrics.http_error++;
    g_debug("emoji http: error fetching url=%s: %s", ctx->url, error ? error->message : "unknown");
    g_clear_error(&error);
    emoji_ctx_free(ctx);
    return;
  }

  gsize blen = 0;
  (void)g_bytes_get_data(bytes, &blen);
  s_emoji_metrics.http_ok++;
  g_debug("emoji http: fetched url=%s bytes=%zu", ctx->url, (size_t)blen);

  /* Validate and decode the image */
  GdkTexture *tex = emoji_texture_from_bytes_scaled(bytes, &error);
  if (!tex) {
    g_warning("emoji http: INVALID IMAGE for url=%s: %s", ctx->url, error ? error->message : "unknown");
    g_clear_error(&error);
    g_bytes_unref(bytes);
    emoji_ctx_free(ctx);
    return;
  }

  /* Write to disk cache */
  g_autofree char *path = emoji_path_for_url(ctx->url);
  if (path) {
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(bytes, &len);
    GError *werr = NULL;
    if (g_file_set_contents(path, (const char*)data, (gssize)len, &werr)) {
      g_debug("emoji http: wrote cache file %s", path);
    } else {
      s_emoji_metrics.cache_write_error++;
      g_warning("emoji http: failed to write cache %s: %s", path, werr ? werr->message : "unknown");
      g_clear_error(&werr);
    }
  }

  g_bytes_unref(bytes);
  ensure_emoji_cache();
  g_hash_table_replace(s_emoji_texture_cache, g_strdup(ctx->url), g_object_ref(tex));
  emoji_lru_insert(ctx->url);
  emoji_lru_evict_if_needed();
  g_debug("emoji http: cached texture for url=%s", ctx->url);

  g_object_unref(tex);
  emoji_ctx_free(ctx);
}
#endif

void gnostr_emoji_cache_metrics_get(GnostrEmojiCacheMetrics *out) {
  if (!out) return;
  *out = s_emoji_metrics;
}

void gnostr_emoji_cache_metrics_log(void) {
  g_message("emoji_metrics: requests=%" G_GUINT64_FORMAT " mem_hits=%" G_GUINT64_FORMAT
            " disk_hits=%" G_GUINT64_FORMAT " http_start=%" G_GUINT64_FORMAT
            " http_ok=%" G_GUINT64_FORMAT " http_err=%" G_GUINT64_FORMAT
            " cache_write_err=%" G_GUINT64_FORMAT,
            s_emoji_metrics.requests_total,
            s_emoji_metrics.mem_cache_hits,
            s_emoji_metrics.disk_cache_hits,
            s_emoji_metrics.http_start,
            s_emoji_metrics.http_ok,
            s_emoji_metrics.http_error,
            s_emoji_metrics.cache_write_error);
}
