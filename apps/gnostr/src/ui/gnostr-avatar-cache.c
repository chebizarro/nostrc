#include "gnostr-avatar-cache.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
#include "utils.h"

/* Avatar context for async HTTP downloads */
typedef struct _AvatarCtx {
  GtkWidget *image;     /* weak (we ref for async) */
  GtkWidget *initials;  /* weak (we ref for async) */
  char *url;            /* owned */
} AvatarCtx;

/* Simple shared cache for downloaded avatar textures by URL */
static GHashTable *avatar_texture_cache = NULL; /* key: char* url, value: GdkTexture* */
static char *avatar_cache_dir = NULL; /* disk cache dir */
static GQueue *s_avatar_lru = NULL;               /* queue of char* url (head=oldest) */
static GHashTable *s_avatar_lru_nodes = NULL;     /* key=url -> GList* node */
static guint s_avatar_cap = 0;                    /* max resident textures (0 = not initialized) */
static guint s_avatar_size = 0;                   /* target decode size in pixels (0 = not initialized) */
static gboolean s_avatar_log_started = FALSE;     /* periodic logging started */
static gboolean s_config_initialized = FALSE;     /* env vars read */

/* Metrics */
static GnostrAvatarMetrics s_avatar_metrics = {0};

/* Read configuration from environment variables */
static void avatar_init_config(void) {
  if (s_config_initialized) return;
  s_config_initialized = TRUE;
  
  /* GNOSTR_AVATAR_MEM_CAP: max in-memory textures (default 200) */
  const char *cap_env = g_getenv("GNOSTR_AVATAR_MEM_CAP");
  if (cap_env && *cap_env) {
    long val = strtol(cap_env, NULL, 10);
    if (val > 0 && val < 100000) {
      s_avatar_cap = (guint)val;
      g_message("[AVATAR_CACHE] Using GNOSTR_AVATAR_MEM_CAP=%u", s_avatar_cap);
    } else {
      g_warning("[AVATAR_CACHE] Invalid GNOSTR_AVATAR_MEM_CAP=%s, using default", cap_env);
    }
  }
  if (s_avatar_cap == 0) s_avatar_cap = 200; /* default */
  
  /* GNOSTR_AVATAR_SIZE: target decode size in pixels (default 96) */
  const char *size_env = g_getenv("GNOSTR_AVATAR_SIZE");
  if (size_env && *size_env) {
    long val = strtol(size_env, NULL, 10);
    if (val >= 32 && val <= 512) {
      s_avatar_size = (guint)val;
      g_message("[AVATAR_CACHE] Using GNOSTR_AVATAR_SIZE=%u", s_avatar_size);
    } else {
      g_warning("[AVATAR_CACHE] Invalid GNOSTR_AVATAR_SIZE=%s (must be 32-512), using default", size_env);
    }
  }
  if (s_avatar_size == 0) s_avatar_size = 96; /* default */
  
  g_message("[AVATAR_CACHE] Config: cap=%u size=%upx", s_avatar_cap, s_avatar_size);
}

/* Forward declarations */
static void avatar_ctx_free(AvatarCtx *c);
static void avatar_lru_touch(const char *url);
static void avatar_lru_insert(const char *url);
static void avatar_lru_evict_if_needed(void);
static const char *ensure_avatar_cache_dir(void);
static char *avatar_path_for_url(const char *url);
static GdkTexture *try_load_avatar_from_disk(const char *url);
static gboolean avatar_cache_log_cb(gpointer data);
#ifdef HAVE_SOUP3
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data);
#endif

/* Periodic logger for avatar cache */
static gboolean avatar_cache_log_cb(gpointer data) {
  (void)data;
  guint msz = avatar_texture_cache ? g_hash_table_size(avatar_texture_cache) : 0;
  guint lsz = s_avatar_lru_nodes ? g_hash_table_size(s_avatar_lru_nodes) : 0;
  g_message("[AVATAR_CACHE] mem=%u lru=%u cap=%u size=%upx", msz, lsz, s_avatar_cap, s_avatar_size);
  gnostr_avatar_metrics_log();
  return TRUE;
}

static void ensure_avatar_cache(void) {
  avatar_init_config(); /* Read env vars on first call */
  if (!avatar_texture_cache) avatar_texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  if (!s_avatar_lru) s_avatar_lru = g_queue_new();
  if (!s_avatar_lru_nodes) s_avatar_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!s_avatar_log_started) {
    s_avatar_log_started = TRUE;
    /* Log every 60s */
    g_timeout_add_seconds(60, avatar_cache_log_cb, NULL);
  }
}

/* LRU helpers for avatar memory cache */
static void avatar_lru_touch(const char *url) {
  if (!url || !s_avatar_lru || !s_avatar_lru_nodes) return;
  GList *node = g_hash_table_lookup(s_avatar_lru_nodes, url);
  if (!node) return;
  g_queue_unlink(s_avatar_lru, node);
  g_queue_push_tail_link(s_avatar_lru, node);
}

static void avatar_lru_insert(const char *url) {
  if (!url || !s_avatar_lru || !s_avatar_lru_nodes) return;
  if (g_hash_table_contains(s_avatar_lru_nodes, url)) { avatar_lru_touch(url); return; }
  char *k = g_strdup(url);
  GList *node = g_list_alloc(); node->data = k;
  g_queue_push_tail_link(s_avatar_lru, node);
  g_hash_table_insert(s_avatar_lru_nodes, g_strdup(url), node);
}

static void avatar_lru_evict_if_needed(void) {
  if (!s_avatar_lru || !s_avatar_lru_nodes || !avatar_texture_cache) return;
  while (g_hash_table_size(s_avatar_lru_nodes) > s_avatar_cap) {
    GList *old = s_avatar_lru->head; if (!old) break;
    char *old_url = (char*)old->data;
    g_queue_unlink(s_avatar_lru, old);
    g_list_free_1(old);
    g_hash_table_remove(s_avatar_lru_nodes, old_url);
    /* Remove from texture cache (unrefs texture) */
    g_hash_table_remove(avatar_texture_cache, old_url);
    g_free(old_url);
  }
}

/* Ensure disk cache directory exists: $XDG_CACHE_HOME/gnostr/avatars */
static const char *ensure_avatar_cache_dir(void) {
  if (avatar_cache_dir) return avatar_cache_dir;
  const char *base = g_get_user_cache_dir();
  if (!base || !*base) base = ".";
  char *dir = g_build_filename(base, "gnostr", "avatars", NULL);
  if (g_mkdir_with_parents(dir, 0700) != 0 && errno != EEXIST) {
    g_warning("avatar cache: mkdir failed (%s): %s", dir, g_strerror(errno));
  }
  avatar_cache_dir = dir; /* keep */
  g_message("avatar cache: using dir %s", avatar_cache_dir);
  return avatar_cache_dir;
}

/* Build a safe cache path from URL using SHA256 */
static char *avatar_path_for_url(const char *url) {
  if (!url || !*url) return NULL;
  const char *dir = ensure_avatar_cache_dir();
  g_autofree char *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
  return g_build_filename(dir, hex, NULL);
}

/* Create GdkTexture from GdkPixbuf using gdk_memory_texture_new().
 * This replaces the deprecated gdk_texture_new_for_pixbuf() in GTK 4.20+. */
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

/* Decode image at bounded size using GdkPixbuf, then create GdkTexture.
 * This drastically reduces memory usage vs. loading full-size images.
 * Returns new ref or NULL on error. */
static GdkTexture *avatar_texture_from_file_scaled(const char *path, GError **error) {
  g_return_val_if_fail(path != NULL, NULL);

  /* Load and scale using GdkPixbuf */
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
    path,
    s_avatar_size,  /* width */
    s_avatar_size,  /* height */
    TRUE,           /* preserve aspect ratio */
    error
  );

  if (!pixbuf) {
    return NULL;
  }

  /* Create texture from scaled pixbuf */
  GdkTexture *texture = texture_new_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);

  return texture;
}

/* Decode image from bytes at bounded size. Returns new ref or NULL on error. */
static GdkTexture *avatar_texture_from_bytes_scaled(GBytes *bytes, GError **error) {
  g_return_val_if_fail(bytes != NULL, NULL);
  
  /* Create input stream from bytes */
  GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
  if (!stream) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create input stream");
    return NULL;
  }
  
  /* Load and scale using GdkPixbuf */
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(
    stream,
    s_avatar_size,  /* width */
    s_avatar_size,  /* height */
    TRUE,           /* preserve aspect ratio */
    NULL,           /* cancellable */
    error
  );
  
  g_object_unref(stream);
  
  if (!pixbuf) {
    return NULL;
  }

  /* Create texture from scaled pixbuf */
  GdkTexture *texture = texture_new_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);

  return texture;
}

/* Try to load texture from disk cache; returns new ref or NULL */
static GdkTexture *try_load_avatar_from_disk(const char *url) {
  if (!url || !*url) return NULL;
  g_autofree char *path = avatar_path_for_url(url);
  if (!path) return NULL;
  if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    g_debug("avatar disk: miss for url=%s path=%s", url, path);
    return NULL;
  }
  GError *err = NULL;
  /* Use scaled decode to reduce memory usage */
  GdkTexture *tex = avatar_texture_from_file_scaled(path, &err);
  if (!tex) {
    g_warning("avatar disk: INVALID cached file %s (url=%s): %s - deleting corrupt cache", 
              path, url, err ? err->message : "unknown");
    g_clear_error(&err);
    
    /* Delete the corrupt cache file so it can be re-downloaded */
    if (unlink(path) != 0) {
      g_warning("avatar disk: failed to delete corrupt cache file %s: %s", path, g_strerror(errno));
    } else {
      g_message("avatar disk: deleted corrupt cache file %s", path);
    }
    
    return NULL;
  }
  g_debug("avatar disk: hit for url=%s path=%s (scaled to %upx)", url, path, s_avatar_size);
  s_avatar_metrics.disk_cache_hits++;
  return tex; /* transfer */
}

static void avatar_ctx_free(AvatarCtx *c) {
  if (!c) return;
  if (c->image) g_object_unref(c->image);
  if (c->initials) g_object_unref(c->initials);
  g_free(c->url);
  g_free(c);
}  

/* Public: prefetch and cache avatar by URL without any UI. No-op if cached or invalid. */
void gnostr_avatar_prefetch(const char *url) {
    if (!url || !*url) return;
    if (!str_has_prefix_http(url)) { g_debug("avatar prefetch: invalid url=%s", url ? url : "(null)"); return; }
    g_message("avatar prefetch: entry url=%s", url);
    s_avatar_metrics.requests_total++;
    ensure_avatar_cache();
    /* Already in-memory cached? */
    if (g_hash_table_lookup(avatar_texture_cache, url)) { 
      g_debug("avatar prefetch: memory cache hit url=%s", url);
      avatar_lru_touch(url);
      return; 
    }
    /* Disk cached? If so, promote into memory cache and return */
    GdkTexture *disk_tex = try_load_avatar_from_disk(url);
    if (disk_tex) {
      g_hash_table_replace(avatar_texture_cache, g_strdup(url), g_object_ref(disk_tex));
      avatar_lru_insert(url);
      avatar_lru_evict_if_needed();
      g_object_unref(disk_tex);
      g_debug("avatar prefetch: promoted disk->mem url=%s", url);
      return;
    }
  #ifdef HAVE_SOUP3
    /* Fetch asynchronously and store in cache */
    SoupSession *sess = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", url);
    AvatarCtx *ctx = g_new0(AvatarCtx, 1);
    ctx->image = NULL;    /* no UI to update */
    ctx->initials = NULL; /* no UI to update */
    ctx->url = g_strdup(url);
    g_message("avatar prefetch: fetching via HTTP url=%s", url);
    s_avatar_metrics.http_start++;
    soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT, NULL, on_avatar_http_done, ctx);
    g_object_unref(msg);
    g_object_unref(sess);
  #else
    (void)url; /* libsoup not available; skip */
  #endif
  }

  /* Public: Try to load avatar from cache (memory or disk). Returns new ref or NULL. */
GdkTexture *gnostr_avatar_try_load_cached(const char *url) {
    if (!url || !*url) return NULL;
    if (!str_has_prefix_http(url)) return NULL;
    
    ensure_avatar_cache();
    
    /* Check memory cache first */
    GdkTexture *mem_tex = g_hash_table_lookup(avatar_texture_cache, url);
    if (mem_tex) {
      s_avatar_metrics.mem_cache_hits++;
      avatar_lru_touch(url);
      return g_object_ref(mem_tex); /* return new ref */
    }
    
    /* Check disk cache */
    GdkTexture *disk_tex = try_load_avatar_from_disk(url);
    if (disk_tex) {
      /* Promote to memory cache */
      g_hash_table_replace(avatar_texture_cache, g_strdup(url), g_object_ref(disk_tex));
      avatar_lru_insert(url);
      avatar_lru_evict_if_needed();
      return disk_tex; /* transfer ownership */
    }
    
    return NULL;
}

/* Public: Download avatar asynchronously and update widgets when done. */
void gnostr_avatar_download_async(const char *url, GtkWidget *image, GtkWidget *initials) {
    #ifdef HAVE_SOUP3
      if (!url || !*url || !str_has_prefix_http(url)) return;
      
      s_avatar_metrics.requests_total++;
      s_avatar_metrics.http_start++;
      
      SoupSession *sess = soup_session_new();
      SoupMessage *msg = soup_message_new("GET", url);
      AvatarCtx *ctx = g_new0(AvatarCtx, 1);
      ctx->image = image ? g_object_ref(image) : NULL;
      ctx->initials = initials ? g_object_ref(initials) : NULL;
      ctx->url = g_strdup(url);
      
      soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT, NULL, on_avatar_http_done, ctx);
      g_object_unref(msg);
      g_object_unref(sess);
    #else
      (void)url; (void)image; (void)initials;
    #endif
}

#ifdef HAVE_SOUP3
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  AvatarCtx *ctx = (AvatarCtx*)user_data;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (!bytes) {
    s_avatar_metrics.http_error++;
    g_debug("avatar http: error fetching url=%s: %s", ctx && ctx->url ? ctx->url : "(null)", error ? error->message : "unknown");
    g_message("avatar http: fetch failed; profile metadata/UI remain applied (url=%s)", ctx && ctx->url ? ctx->url : "(null)");
    g_clear_error(&error);
    avatar_ctx_free(ctx);
    return;
  }
  gsize blen = 0; (void)g_bytes_get_data(bytes, &blen);
  s_avatar_metrics.http_ok++;
  g_message("avatar http: fetched url=%s bytes=%zu", ctx && ctx->url ? ctx->url : "(null)", (size_t)blen);
  
  /* CRITICAL: Validate it's actually an image BEFORE caching, and decode at bounded size */
  GdkTexture *tex = avatar_texture_from_bytes_scaled(bytes, &error);
  if (!tex) {
    g_warning("avatar http: INVALID IMAGE DATA for url=%s: %s (likely HTML error page)", 
              ctx && ctx->url ? ctx->url : "(null)", error ? error->message : "unknown");
    g_clear_error(&error);
    g_bytes_unref(bytes);
    avatar_ctx_free(ctx);
    return;
  }
  g_debug("avatar http: decoded and scaled to %upx for url=%s", s_avatar_size, ctx->url);
  
  /* Only persist to disk cache if it's a valid image */
  g_autofree char *path = avatar_path_for_url(ctx->url);
  if (path) {
    gsize len = 0; const guint8 *data = g_bytes_get_data(bytes, &len);
    GError *werr = NULL; 
    if (g_file_set_contents(path, (const char*)data, (gssize)len, &werr)) {
      g_debug("avatar http: wrote cache file %s len=%zu", path, (size_t)len);
    } else {
      s_avatar_metrics.cache_write_error++;
      g_warning("avatar http: failed to write cache file %s: %s", path, werr ? werr->message : "unknown");
      g_clear_error(&werr);
    }
  }
  
  g_bytes_unref(bytes);
  ensure_avatar_cache();
  g_hash_table_replace(avatar_texture_cache, g_strdup(ctx->url), g_object_ref(tex));
  /* LRU update */
  avatar_lru_insert(ctx->url);
  avatar_lru_evict_if_needed();
  g_debug("avatar http: cached texture for url=%s", ctx && ctx->url ? ctx->url : "(null)");
  if (GTK_IS_PICTURE(ctx->image)) { gtk_picture_set_paintable(GTK_PICTURE(ctx->image), GDK_PAINTABLE(tex)); gtk_widget_set_visible(ctx->image, TRUE); }
  if (GTK_IS_WIDGET(ctx->initials)) gtk_widget_set_visible(ctx->initials, FALSE);
  g_object_unref(tex);
  avatar_ctx_free(ctx);
}
#endif

/* Public: Get current avatar metrics */
void gnostr_avatar_metrics_get(GnostrAvatarMetrics *out) {
  if (!out) return;
  *out = s_avatar_metrics;
}

/* Public: Log current avatar metrics */
void gnostr_avatar_metrics_log(void) {
  g_message("avatar_metrics: requests=%" G_GUINT64_FORMAT " mem_hits=%" G_GUINT64_FORMAT
            " disk_hits=%" G_GUINT64_FORMAT " http_start=%" G_GUINT64_FORMAT
            " http_ok=%" G_GUINT64_FORMAT " http_err=%" G_GUINT64_FORMAT
            " initials=%" G_GUINT64_FORMAT " cache_write_err=%" G_GUINT64_FORMAT,
            s_avatar_metrics.requests_total,
            s_avatar_metrics.mem_cache_hits,
            s_avatar_metrics.disk_cache_hits,
            s_avatar_metrics.http_start,
            s_avatar_metrics.http_ok,
            s_avatar_metrics.http_error,
            s_avatar_metrics.initials_shown,
            s_avatar_metrics.cache_write_error);
}
