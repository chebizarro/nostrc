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
#include "../util/utils.h"

/* Avatar context for async HTTP downloads.
 * CRITICAL: Use GWeakRef instead of g_object_ref to prevent use-after-free
 * when GtkListView recycles rows. When a widget is disposed, the weak ref
 * becomes NULL, preventing updates to zombie widgets that would corrupt
 * GTK's internal GtkImageDefinition and cause crashes. */
typedef struct _AvatarCtx {
  GWeakRef image_ref;     /* weak ref to image widget */
  GWeakRef initials_ref;  /* weak ref to initials widget */
  char *url;              /* owned */
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
static GHashTable *s_avatar_bad_urls = NULL;      /* negative cache: URLs that returned invalid data */

/* Metrics */
static GnostrAvatarMetrics s_avatar_metrics = {0};

/* --- Concurrent Request Limiter --- */
/* nostrc-img1: Reduced from 12 to 6 — avatar fetches were consuming half the
 * SoupSession's 24-connection pool, starving timeline media image loads. */
#define AVATAR_MAX_CONCURRENT_FETCHES 6   /* Max simultaneous HTTP requests */

typedef struct _PendingFetch {
  char *url;
  GWeakRef image_ref;
  GWeakRef initials_ref;
} PendingFetch;

static guint s_active_fetches = 0;           /* Currently in-flight HTTP requests */
static GQueue *s_pending_queue = NULL;       /* Queue of PendingFetch* waiting */
static GMutex s_fetch_mutex;                 /* Protects active count and queue */
static gboolean s_fetch_mutex_initialized = FALSE;

static void pending_fetch_free(PendingFetch *pf) {
  if (!pf) return;
  g_free(pf->url);
  g_weak_ref_clear(&pf->image_ref);
  g_weak_ref_clear(&pf->initials_ref);
  g_free(pf);
}

static void ensure_fetch_limiter(void) {
  if (!s_fetch_mutex_initialized) {
    g_mutex_init(&s_fetch_mutex);
    s_fetch_mutex_initialized = TRUE;
  }
  if (!s_pending_queue) {
    s_pending_queue = g_queue_new();
  }
}

/* Forward declaration for queue processing */
static void process_pending_fetch_queue(void);

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
static void start_avatar_fetch_internal(const char *url, GtkWidget *image, GtkWidget *initials);
#endif

/* Periodic logger for avatar cache */
static gboolean avatar_cache_log_cb(gpointer data) {
  (void)data;
  guint msz = avatar_texture_cache ? g_hash_table_size(avatar_texture_cache) : 0;
  guint lsz = s_avatar_lru_nodes ? g_hash_table_size(s_avatar_lru_nodes) : 0;
  guint pending = s_pending_queue ? g_queue_get_length(s_pending_queue) : 0;
  g_message("[AVATAR_CACHE] mem=%u lru=%u cap=%u size=%upx active_fetches=%u pending=%u max=%u",
            msz, lsz, s_avatar_cap, s_avatar_size, s_active_fetches, pending, AVATAR_MAX_CONCURRENT_FETCHES);
  gnostr_avatar_metrics_log();
  return TRUE;
}

/* Public: Get current cache size for memory stats */
guint gnostr_avatar_cache_size(void) {
  return avatar_texture_cache ? g_hash_table_size(avatar_texture_cache) : 0;
}

static void ensure_avatar_cache(void) {
  avatar_init_config(); /* Read env vars on first call */
  if (!avatar_texture_cache) avatar_texture_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  if (!s_avatar_lru) s_avatar_lru = g_queue_new();
  if (!s_avatar_lru_nodes) s_avatar_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!s_avatar_log_started) {
    s_avatar_log_started = TRUE;
    /* LEGITIMATE TIMEOUT - Periodic cache stats logging (60s intervals).
     * nostrc-b0h: Audited - diagnostic logging is appropriate. */
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

/* Create a centered, cropped square pixbuf from source pixbuf.
 * This implements "cover" style scaling: the image fills the target size
 * while maintaining aspect ratio, with overflow cropped from center. */
static GdkPixbuf *pixbuf_crop_to_square(GdkPixbuf *src, int target_size) {
  g_return_val_if_fail(src != NULL, NULL);

  int src_w = gdk_pixbuf_get_width(src);
  int src_h = gdk_pixbuf_get_height(src);

  /* Calculate scale factor so shorter side equals target_size */
  double scale;
  if (src_w < src_h) {
    scale = (double)target_size / src_w;
  } else {
    scale = (double)target_size / src_h;
  }

  int scaled_w = (int)(src_w * scale + 0.5);
  int scaled_h = (int)(src_h * scale + 0.5);

  /* Scale the image preserving aspect ratio */
  GdkPixbuf *scaled = gdk_pixbuf_scale_simple(src, scaled_w, scaled_h, GDK_INTERP_BILINEAR);
  if (!scaled) return NULL;

  /* Calculate crop offset to center the image */
  int crop_x = (scaled_w - target_size) / 2;
  int crop_y = (scaled_h - target_size) / 2;

  /* Clamp to valid range */
  if (crop_x < 0) crop_x = 0;
  if (crop_y < 0) crop_y = 0;
  if (crop_x + target_size > scaled_w) crop_x = scaled_w - target_size;
  if (crop_y + target_size > scaled_h) crop_y = scaled_h - target_size;

  /* Create the cropped square pixbuf */
  GdkPixbuf *cropped = gdk_pixbuf_new_subpixbuf(scaled, crop_x, crop_y, target_size, target_size);
  if (!cropped) {
    g_object_unref(scaled);
    return NULL;
  }

  /* gdk_pixbuf_new_subpixbuf shares data with parent, so we need to copy */
  GdkPixbuf *result = gdk_pixbuf_copy(cropped);
  g_object_unref(cropped);
  g_object_unref(scaled);

  return result;
}

/* Decode image at bounded size using GdkPixbuf, then create GdkTexture.
 * This drastically reduces memory usage vs. loading full-size images.
 * Images are scaled with "cover" style: maintaining aspect ratio,
 * scaling so the shorter side fills the target, and cropping to center.
 * Returns new ref or NULL on error. */
static GdkTexture *avatar_texture_from_file_scaled(const char *path, GError **error) {
  g_return_val_if_fail(path != NULL, NULL);

  /* First load the image at a reasonable size for memory efficiency.
   * We use a larger size here to allow for quality cropping, then crop to final size. */
  int load_size = s_avatar_size * 2; /* Load at 2x for better quality after crop */
  if (load_size > 512) load_size = 512; /* Cap at reasonable max */

  GdkPixbuf *loaded = gdk_pixbuf_new_from_file_at_scale(
    path,
    load_size,  /* width */
    load_size,  /* height */
    TRUE,       /* preserve aspect ratio - TRUE to avoid distortion */
    error
  );

  if (!loaded) {
    return NULL;
  }

  /* Crop to centered square */
  GdkPixbuf *cropped = pixbuf_crop_to_square(loaded, s_avatar_size);
  g_object_unref(loaded);

  if (!cropped) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to crop avatar to square");
    return NULL;
  }

  /* Create texture from cropped pixbuf */
  GdkTexture *texture = texture_new_from_pixbuf(cropped);
  g_object_unref(cropped);

  return texture;
}

/* Decode image from bytes at bounded size. Returns new ref or NULL on error.
 * Images are scaled with "cover" style: maintaining aspect ratio,
 * scaling so the shorter side fills the target, and cropping to center. */
static GdkTexture *avatar_texture_from_bytes_scaled(GBytes *bytes, GError **error) {
  g_return_val_if_fail(bytes != NULL, NULL);

  /* Create input stream from bytes */
  GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
  if (!stream) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create input stream");
    return NULL;
  }

  /* Load at a larger size for quality cropping */
  int load_size = s_avatar_size * 2;
  if (load_size > 512) load_size = 512;

  /* Load and scale using GdkPixbuf, preserving aspect ratio */
  GdkPixbuf *loaded = gdk_pixbuf_new_from_stream_at_scale(
    stream,
    load_size,  /* width */
    load_size,  /* height */
    TRUE,       /* preserve aspect ratio - TRUE to avoid distortion */
    NULL,       /* cancellable */
    error
  );

  g_object_unref(stream);

  if (!loaded) {
    return NULL;
  }

  /* Crop to centered square */
  GdkPixbuf *cropped = pixbuf_crop_to_square(loaded, s_avatar_size);
  g_object_unref(loaded);

  if (!cropped) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to crop avatar to square");
    return NULL;
  }

  /* Create texture from cropped pixbuf */
  GdkTexture *texture = texture_new_from_pixbuf(cropped);
  g_object_unref(cropped);

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
  g_weak_ref_clear(&c->image_ref);
  g_weak_ref_clear(&c->initials_ref);
  g_free(c->url);
  g_free(c);
}

#ifdef HAVE_SOUP3
/* Internal: Actually start an HTTP fetch (assumes slot is available).
 * IMPORTANT: Caller must have already incremented s_active_fetches.
 * On early return (error), we decrement it and process queue. */
static void start_avatar_fetch_internal(const char *url, GtkWidget *image, GtkWidget *initials) {
  if (!url || !*url) {
    g_warning("avatar fetch: NULL or empty URL");
    goto error_decrement;
  }

  SoupSession *sess = gnostr_get_shared_soup_session();
  if (!sess) {
    g_debug("avatar fetch: shared session unavailable (shutdown?)");
    goto error_decrement;
  }

  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_warning("avatar fetch: failed to create message for url=%s", url);
    goto error_decrement;
  }

  AvatarCtx *ctx = g_new0(AvatarCtx, 1);
  g_weak_ref_init(&ctx->image_ref, image);
  g_weak_ref_init(&ctx->initials_ref, initials);
  ctx->url = g_strdup(url);

  g_debug("avatar fetch: starting HTTP for url=%s (active=%u)", url, s_active_fetches);
  s_avatar_metrics.http_start++;

  soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT, NULL, on_avatar_http_done, ctx);
  g_object_unref(msg);
  return;

error_decrement:
  /* Decrement counter since we didn't actually start the fetch */
  g_mutex_lock(&s_fetch_mutex);
  if (s_active_fetches > 0) s_active_fetches--;
  g_mutex_unlock(&s_fetch_mutex);
  process_pending_fetch_queue();
}

/* Process pending fetch queue - called after a fetch completes */
static void process_pending_fetch_queue(void) {
  ensure_fetch_limiter();

  g_mutex_lock(&s_fetch_mutex);

  while (s_active_fetches < AVATAR_MAX_CONCURRENT_FETCHES && !g_queue_is_empty(s_pending_queue)) {
    PendingFetch *pf = g_queue_pop_head(s_pending_queue);
    if (!pf) break;

    /* Check if widgets are still alive before starting fetch */
    GtkWidget *image = g_weak_ref_get(&pf->image_ref);
    GtkWidget *initials = g_weak_ref_get(&pf->initials_ref);

    /* Even if widgets are gone, we might still want to cache the avatar */
    gboolean has_ui = (image != NULL || initials != NULL);

    if (has_ui || pf->url) {
      s_active_fetches++;
      g_mutex_unlock(&s_fetch_mutex);

      start_avatar_fetch_internal(pf->url, image, initials);

      /* Release refs obtained from g_weak_ref_get */
      if (image) g_object_unref(image);
      if (initials) g_object_unref(initials);

      g_mutex_lock(&s_fetch_mutex);
    }

    pending_fetch_free(pf);
  }

  guint pending = g_queue_get_length(s_pending_queue);
  g_mutex_unlock(&s_fetch_mutex);

  if (pending > 0) {
    g_debug("avatar fetch: queue has %u pending requests", pending);
  }
}
#endif

/* Public: prefetch and cache avatar by URL without any UI. No-op if cached or invalid. */
void gnostr_avatar_prefetch(const char *url) {
    if (!url || !*url) return;
    if (!str_has_prefix_http(url)) { g_debug("avatar prefetch: invalid url=%s", url ? url : "(null)"); return; }
    g_debug("avatar prefetch: entry url=%s", url);
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
    /* Fetch asynchronously and store in cache, using the limiter */
    ensure_fetch_limiter();

    g_mutex_lock(&s_fetch_mutex);
    if (s_active_fetches < AVATAR_MAX_CONCURRENT_FETCHES) {
      s_active_fetches++;
      g_mutex_unlock(&s_fetch_mutex);
      g_debug("avatar prefetch: fetching via HTTP url=%s", url);
      start_avatar_fetch_internal(url, NULL, NULL);
    } else {
      /* Queue the prefetch request */
      PendingFetch *pf = g_new0(PendingFetch, 1);
      pf->url = g_strdup(url);
      g_weak_ref_init(&pf->image_ref, NULL);
      g_weak_ref_init(&pf->initials_ref, NULL);
      g_queue_push_tail(s_pending_queue, pf);
      g_debug("avatar prefetch: queued url=%s (active=%u, pending=%u)",
              url, s_active_fetches, g_queue_get_length(s_pending_queue));
      g_mutex_unlock(&s_fetch_mutex);
    }
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

/* Public: Download avatar asynchronously and update widgets when done.
 * CRITICAL: Uses GWeakRef for widgets to prevent use-after-free crashes
 * when GtkListView recycles rows during scrolling.
 * Uses concurrent request limiter to prevent FD exhaustion. */
void gnostr_avatar_download_async(const char *url, GtkWidget *image, GtkWidget *initials) {
    #ifdef HAVE_SOUP3
      if (!url || !*url || !str_has_prefix_http(url)) return;

      /* Skip URLs known to return invalid data */
      if (s_avatar_bad_urls && g_hash_table_contains(s_avatar_bad_urls, url))
        return;

      s_avatar_metrics.requests_total++;
      ensure_avatar_cache();

      /* Check memory/disk cache before starting HTTP fetch.
       * This avoids wasting concurrent fetch slots on already-cached avatars
       * and ensures immediate display when cached. (nostrc-qm77) */
      GdkTexture *cached = g_hash_table_lookup(avatar_texture_cache, url);
      if (cached) {
        s_avatar_metrics.mem_cache_hits++;
        avatar_lru_touch(url);
        if (image && GTK_IS_PICTURE(image) && gtk_widget_get_native(image) != NULL) {
          gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(cached));
          gtk_widget_set_visible(image, TRUE);
        }
        if (initials && GTK_IS_WIDGET(initials) && gtk_widget_get_native(initials) != NULL) {
          gtk_widget_set_visible(initials, FALSE);
        }
        return;
      }

      GdkTexture *disk_tex = try_load_avatar_from_disk(url);
      if (disk_tex) {
        /* Promote to memory cache */
        g_hash_table_replace(avatar_texture_cache, g_strdup(url), g_object_ref(disk_tex));
        avatar_lru_insert(url);
        avatar_lru_evict_if_needed();
        if (image && GTK_IS_PICTURE(image) && gtk_widget_get_native(image) != NULL) {
          gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(disk_tex));
          gtk_widget_set_visible(image, TRUE);
        }
        if (initials && GTK_IS_WIDGET(initials) && gtk_widget_get_native(initials) != NULL) {
          gtk_widget_set_visible(initials, FALSE);
        }
        g_object_unref(disk_tex);
        return;
      }

      ensure_fetch_limiter();

      g_mutex_lock(&s_fetch_mutex);
      if (s_active_fetches < AVATAR_MAX_CONCURRENT_FETCHES) {
        /* Slot available - start immediately */
        s_active_fetches++;
        g_mutex_unlock(&s_fetch_mutex);
        start_avatar_fetch_internal(url, image, initials);
      } else {
        /* Queue the request */
        PendingFetch *pf = g_new0(PendingFetch, 1);
        pf->url = g_strdup(url);
        g_weak_ref_init(&pf->image_ref, image);
        g_weak_ref_init(&pf->initials_ref, initials);
        g_queue_push_tail(s_pending_queue, pf);
        g_debug("avatar fetch: queued url=%s (active=%u, pending=%u)",
                url, s_active_fetches, g_queue_get_length(s_pending_queue));
        g_mutex_unlock(&s_fetch_mutex);
      }
    #else
      (void)url; (void)image; (void)initials;
    #endif
}

#ifdef HAVE_SOUP3

/* Context passed from HTTP callback → worker thread → main-thread finish */
typedef struct {
  AvatarCtx *avatar_ctx;  /* original context with widget weak refs + url */
  GBytes *raw_bytes;      /* raw HTTP response bytes for disk cache write */
} AvatarDecodeCtx;

/* Worker thread: decode image + write disk cache off main thread.
 * Both gdk_pixbuf_new_from_stream_at_scale() and g_file_set_contents()
 * can block — the former for CPU-bound decompression, the latter for I/O.
 * GdkTexture is immutable and thread-safe to create from any thread. */
static void avatar_decode_thread(GTask *task, gpointer source_object,
                                  gpointer task_data, GCancellable *cancellable) {
  (void)source_object; (void)cancellable;
  AvatarDecodeCtx *dctx = (AvatarDecodeCtx*)task_data;
  GError *error = NULL;

  /* Decode and scale */
  GdkTexture *tex = avatar_texture_from_bytes_scaled(dctx->raw_bytes, &error);
  if (!tex) {
    g_task_return_error(task, error);
    return;
  }

  /* Write disk cache while still on worker thread */
  g_autofree char *path = avatar_path_for_url(dctx->avatar_ctx->url);
  if (path) {
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(dctx->raw_bytes, &len);
    GError *werr = NULL;
    if (g_file_set_contents(path, (const char*)data, (gssize)len, &werr)) {
      g_debug("avatar worker: wrote cache file %s len=%zu", path, (size_t)len);
    } else {
      g_warning("avatar worker: failed to write cache %s: %s", path,
                werr ? werr->message : "unknown");
      g_clear_error(&werr);
    }
  }

  g_task_return_pointer(task, tex, g_object_unref);
}

static void avatar_decode_ctx_free(AvatarDecodeCtx *dctx) {
  if (!dctx) return;
  g_clear_pointer(&dctx->raw_bytes, g_bytes_unref);
  /* avatar_ctx is freed separately in the completion callback */
  g_free(dctx);
}

/* Main-thread callback: apply decoded texture to widget + update caches */
static void on_avatar_decode_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  AvatarDecodeCtx *dctx = (AvatarDecodeCtx*)user_data;
  AvatarCtx *ctx = dctx->avatar_ctx;
  GError *error = NULL;

  GdkTexture *tex = g_task_propagate_pointer(G_TASK(res), &error);
  if (!tex) {
    g_debug("avatar decode: failed for url=%s: %s",
            ctx && ctx->url ? ctx->url : "(null)", error ? error->message : "unknown");
    if (ctx && ctx->url) {
      if (!s_avatar_bad_urls)
        s_avatar_bad_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_add(s_avatar_bad_urls, g_strdup(ctx->url));
    }
    g_clear_error(&error);
    avatar_ctx_free(ctx);
    avatar_decode_ctx_free(dctx);
    g_mutex_lock(&s_fetch_mutex);
    if (s_active_fetches > 0) s_active_fetches--;
    g_mutex_unlock(&s_fetch_mutex);
    process_pending_fetch_queue();
    return;
  }

  g_debug("avatar decode: done for url=%s (%upx)", ctx->url, s_avatar_size);

  ensure_avatar_cache();
  g_hash_table_replace(avatar_texture_cache, g_strdup(ctx->url), g_object_ref(tex));
  avatar_lru_insert(ctx->url);
  avatar_lru_evict_if_needed();

  /* CRITICAL: Use g_weak_ref_get to safely check if widgets still exist.
   * If widget was recycled/disposed during decode, weak ref returns NULL.
   *
   * ALSO check gtk_widget_get_native() — g_weak_ref_get returns non-NULL
   * for recycled (not finalized) GtkListView row widgets. A widget without
   * a native surface is in a transitional state; calling set_paintable on
   * it corrupts GtkPicture's internal GtkImageDefinition, causing:
   *   Gtk:ERROR:gtkimagedefinition.c:156:gtk_image_definition_unref:
   *     code should not be reached */
  GtkWidget *image = g_weak_ref_get(&ctx->image_ref);
  GtkWidget *initials = g_weak_ref_get(&ctx->initials_ref);

  if (image) {
    /* CRITICAL: Triple validation to prevent GTK image definition crash:
     * 1. Check widget is still a GtkPicture
     * 2. Check widget has a native surface (not recycled)
     * 3. Check widget is mapped (actually visible in UI hierarchy)
     * Setting paintable on unmapped widgets can corrupt GtkImageDefinition */
    if (GTK_IS_PICTURE(image) && 
        gtk_widget_get_native(image) != NULL &&
        gtk_widget_get_mapped(image)) {
      gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(tex));
      gtk_widget_set_visible(image, TRUE);
    } else {
      g_debug("avatar decode: image widget not ready (url=%s mapped=%d)", 
              ctx->url, image ? gtk_widget_get_mapped(image) : 0);
    }
    g_object_unref(image);
  } else {
    g_debug("avatar decode: image widget was recycled (url=%s)", ctx->url);
  }

  if (initials) {
    if (GTK_IS_WIDGET(initials) && 
        gtk_widget_get_native(initials) != NULL &&
        gtk_widget_get_mapped(initials)) {
      gtk_widget_set_visible(initials, FALSE);
    }
    g_object_unref(initials);
  }

  g_object_unref(tex);
  avatar_ctx_free(ctx);
  avatar_decode_ctx_free(dctx);

  g_mutex_lock(&s_fetch_mutex);
  if (s_active_fetches > 0) s_active_fetches--;
  g_mutex_unlock(&s_fetch_mutex);
  process_pending_fetch_queue();
}

/* HTTP completion callback — validates response then dispatches decode to worker */
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  AvatarCtx *ctx = (AvatarCtx*)user_data;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (!bytes) {
    s_avatar_metrics.http_error++;
    g_debug("avatar http: fetch failed url=%s: %s", ctx && ctx->url ? ctx->url : "(null)", error ? error->message : "unknown");
    g_clear_error(&error);
    avatar_ctx_free(ctx);
    g_mutex_lock(&s_fetch_mutex);
    if (s_active_fetches > 0) s_active_fetches--;
    g_mutex_unlock(&s_fetch_mutex);
    process_pending_fetch_queue();
    return;
  }
  gsize blen = 0; const guint8 *bdata = g_bytes_get_data(bytes, &blen);
  s_avatar_metrics.http_ok++;
  g_debug("avatar http: fetched url=%s bytes=%zu", ctx && ctx->url ? ctx->url : "(null)", (size_t)blen);

  /* nostrc-tqaj: Quick-reject non-image responses before expensive decode.
   * Servers often return HTML error pages with 200 OK for broken CDN URLs. */
  if (blen > 0 && (bdata[0] == '<' || bdata[0] == '{')) {
    g_debug("avatar http: response is HTML/JSON, not an image for url=%s", ctx->url);
    if (ctx && ctx->url) {
      if (!s_avatar_bad_urls)
        s_avatar_bad_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_add(s_avatar_bad_urls, g_strdup(ctx->url));
    }
    g_bytes_unref(bytes);
    avatar_ctx_free(ctx);
    g_mutex_lock(&s_fetch_mutex);
    if (s_active_fetches > 0) s_active_fetches--;
    g_mutex_unlock(&s_fetch_mutex);
    process_pending_fetch_queue();
    return;
  }

  /* Dispatch decode + disk write to worker thread to avoid blocking
   * the main loop with image decompression + synchronous file I/O.
   * Up to 12 avatar fetches can complete near-simultaneously. */
  AvatarDecodeCtx *dctx = g_new0(AvatarDecodeCtx, 1);
  dctx->avatar_ctx = ctx;
  dctx->raw_bytes = bytes; /* transfer ownership */

  GTask *task = g_task_new(NULL, NULL, on_avatar_decode_done, dctx);
  g_task_set_task_data(task, dctx, NULL); /* freed in completion callback */
  g_task_run_in_thread(task, avatar_decode_thread);
  g_object_unref(task);
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
