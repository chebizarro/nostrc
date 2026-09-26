/* nsp-avatar.c — see nsp-avatar.h. */
#include "nsp-avatar.h"

#include <glib/gstdio.h>
#include <string.h>

#include "nsp-http.h"

#ifdef NSP_HAVE_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#define MAX_ACTIVE 2
#define MAX_QUEUED 32
#define NEGATIVE_TTL_S (10 * 60)
#define PRUNE_EVERY 16
#define MAX_SOURCE_DIM 8192

struct _NspAvatars {
  char *dir;
  gboolean fetch;
  GApplication *app;
  SoupSession *session;
  GQueue queue;          /* char* urls */
  GHashTable *inflight;  /* url set (queued or running) */
  GHashTable *failed;    /* url -> gint64 monotonic failure time */
  guint active;
  guint saves;
  gboolean disposed;
  gint refs;             /* 1 + running downloads */
};

static void avatars_unref(NspAvatars *a) {
  if (--a->refs > 0) return;
  g_free(a->dir);
  g_clear_object(&a->session);
  g_queue_clear_full(&a->queue, g_free);
  g_hash_table_destroy(a->inflight);
  g_hash_table_destroy(a->failed);
  g_free(a);
}

NspAvatars *nsp_avatars_new(const char *cache_dir, gboolean fetch_enabled, GApplication *app) {
  NspAvatars *a = g_new0(NspAvatars, 1);
  a->refs = 1;
  a->dir = g_strdup(cache_dir);
#ifdef NSP_HAVE_PIXBUF
  a->fetch = fetch_enabled;
#else
  a->fetch = FALSE;
#endif
  a->app = app;
  g_queue_init(&a->queue);
  a->inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  a->failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (a->fetch) {
    a->session = nsp_http_session_new(10);
    if (g_mkdir_with_parents(a->dir, 0700) == 0)
      nsp_avatars_prune(a->dir, NSP_AVATAR_CACHE_MAX_BYTES, NSP_AVATAR_CACHE_MAX_FILES);
  }
  return a;
}

void nsp_avatars_free(NspAvatars *a) {
  if (!a) return;
  a->disposed = TRUE;
  if (a->session) soup_session_abort(a->session);
  avatars_unref(a);
}

char *nsp_avatars_path(NspAvatars *a, const char *url) {
  g_autofree char *h = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
  g_autofree char *base = g_strconcat(h, ".png", NULL);
  return g_build_filename(a->dir, base, NULL);
}

guint nsp_avatars_pending(NspAvatars *a) {
  return g_hash_table_size(a->inflight);
}

typedef struct {
  char *path;
  guint64 size;
  gint64 mtime;
} Entry;

static gint by_mtime(gconstpointer x, gconstpointer y) {
  const Entry *a = *(Entry *const *)x, *b = *(Entry *const *)y;
  return a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
}

static void entry_free(Entry *e) {
  g_free(e->path);
  g_free(e);
}

guint nsp_avatars_prune(const char *dir, guint64 max_bytes, guint max_files) {
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  if (!d) return 0;
  g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
  guint64 total = 0;
  const char *name;
  while ((name = g_dir_read_name(d))) {
    if (!g_str_has_suffix(name, ".png")) continue;
    char *path = g_build_filename(dir, name, NULL);
    GStatBuf st;
    if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      g_free(path);
      continue;
    }
    Entry *e = g_new0(Entry, 1);
    e->path = path;
    e->size = (guint64)st.st_size;
    e->mtime = (gint64)st.st_mtime;
    total += e->size;
    g_ptr_array_add(entries, e);
  }
  if (total <= max_bytes && entries->len <= max_files) return 0;
  g_ptr_array_sort(entries, by_mtime);
  guint64 target_bytes = max_bytes * 8 / 10;
  guint target_files = (guint)((guint64)max_files * 8 / 10), removed = 0, left = entries->len;
  for (guint i = 0; i < entries->len && (total > target_bytes || left > target_files); i++) {
    Entry *e = entries->pdata[i];
    if (g_unlink(e->path) == 0) {
      total -= e->size;
      left--;
      removed++;
    }
  }
  return removed;
}

#ifdef NSP_HAVE_PIXBUF
static void on_size_prepared(GdkPixbufLoader *l, gint w, gint h, gpointer user_data) {
  gboolean *too_big = user_data;
  if (w <= 0 || h <= 0 || w > MAX_SOURCE_DIM || h > MAX_SOURCE_DIM) {
    *too_big = TRUE;
    gdk_pixbuf_loader_set_size(l, 1, 1);
    return;
  }
  /* Decode at most 4x the target size to bound memory. */
  gint lim = NSP_AVATAR_SIZE * 4;
  if (w > lim || h > lim) {
    double s = (double)lim / MAX(w, h);
    gdk_pixbuf_loader_set_size(l, MAX(1, (gint)(w * s)), MAX(1, (gint)(h * s)));
  }
}
#endif

gboolean nsp_avatar_transcode(GBytes *image, const char *out_path, GError **error) {
#ifdef NSP_HAVE_PIXBUF
  gboolean too_big = FALSE;
  g_autoptr(GdkPixbufLoader) l = gdk_pixbuf_loader_new();
  g_signal_connect(l, "size-prepared", G_CALLBACK(on_size_prepared), &too_big);
  gboolean ok = gdk_pixbuf_loader_write_bytes(l, image, error);
  gboolean closed = gdk_pixbuf_loader_close(l, ok ? error : NULL);
  if (!ok || !closed) return FALSE;
  if (too_big) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "image dimensions out of range");
    return FALSE;
  }
  GdkPixbuf *src = gdk_pixbuf_loader_get_pixbuf(l);
  if (!src) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not an image");
    return FALSE;
  }
  int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src), side = MIN(w, h);
  g_autoptr(GdkPixbuf) square = gdk_pixbuf_new_subpixbuf(src, (w - side) / 2, (h - side) / 2, side, side);
  g_autoptr(GdkPixbuf) scaled =
      gdk_pixbuf_scale_simple(square, NSP_AVATAR_SIZE, NSP_AVATAR_SIZE, GDK_INTERP_BILINEAR);
  if (!scaled) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "scale failed");
    return FALSE;
  }
  gchar *png = NULL;
  gsize png_len = 0;
  if (!gdk_pixbuf_save_to_buffer(scaled, &png, &png_len, "png", error, NULL)) return FALSE;
  gboolean saved = g_file_set_contents_full(out_path, png, (gssize)png_len,
                                            G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
  g_free(png);
  return saved;
#else
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "built without gdk-pixbuf");
  return FALSE;
#endif
}

static void pump(NspAvatars *a);

typedef struct {
  NspAvatars *a;
  char *url;
  char *path;
  GBytes *body;
} Job;

static void job_free(Job *j) {
  g_free(j->url);
  g_free(j->path);
  if (j->body) g_bytes_unref(j->body);
  g_free(j);
}

static void job_done(Job *j, gboolean ok) {
  NspAvatars *a = j->a;
  g_hash_table_remove(a->inflight, j->url);
  if (!ok) {
    gint64 *t = g_new(gint64, 1);
    *t = g_get_monotonic_time();
    g_hash_table_replace(a->failed, g_strdup(j->url), t);
  } else if (++a->saves % PRUNE_EVERY == 0) {
    nsp_avatars_prune(a->dir, NSP_AVATAR_CACHE_MAX_BYTES, NSP_AVATAR_CACHE_MAX_FILES);
  }
  a->active--;
  if (a->app) g_application_release(a->app);
  if (!a->disposed) pump(a);
  job_free(j);
  avatars_unref(a);
}

static void transcode_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
  Job *j = data;
  g_autoptr(GError) err = NULL;
  gboolean ok = nsp_avatar_transcode(j->body, j->path, &err);
  if (!ok) g_debug("nostr-search-provider: avatar rejected: %s", err->message);
  g_task_return_boolean(task, ok);
}

static void on_transcoded(GObject *src, GAsyncResult *res, gpointer user_data) {
  job_done(user_data, g_task_propagate_boolean(G_TASK(res), NULL));
}

static void on_downloaded(GObject *src, GAsyncResult *res, gpointer user_data) {
  Job *j = user_data;
  g_autoptr(GError) err = NULL;
  g_autofree char *ct = NULL;
  j->body = nsp_http_get_finish(res, &ct, &err);
  if (!j->body || !ct || !g_str_has_prefix(ct, "image/") || j->a->disposed) {
    if (err) g_debug("nostr-search-provider: avatar download failed: %s", err->message);
    job_done(j, FALSE);
    return;
  }
  /* Decoding can take a while for big images: keep it off the main loop
   * so D-Bus calls stay responsive. */
  GTask *t = g_task_new(NULL, NULL, on_transcoded, j);
  g_task_set_task_data(t, j, NULL);
  g_task_run_in_thread(t, transcode_thread);
  g_object_unref(t);
}

static void pump(NspAvatars *a) {
  while (a->active < MAX_ACTIVE && !g_queue_is_empty(&a->queue)) {
    Job *j = g_new0(Job, 1);
    j->a = a;
    j->url = g_queue_pop_head(&a->queue);
    j->path = nsp_avatars_path(a, j->url);
    a->active++;
    a->refs++;
    if (a->app) g_application_hold(a->app);
    nsp_http_get_async(a->session, j->url, "image/*", NSP_AVATAR_MAX_DOWNLOAD, TRUE, NULL,
                       on_downloaded, j);
  }
}

GIcon *nsp_avatars_lookup(NspAvatars *a, const char *url) {
  if (!a || !url || !g_str_has_prefix(url, "https://")) return NULL;
  g_autofree char *path = nsp_avatars_path(a, url);
  if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    g_autoptr(GFile) f = g_file_new_for_path(path);
    return g_file_icon_new(f);
  }
  if (!a->fetch || g_hash_table_contains(a->inflight, url)) return NULL;
  gint64 *failed = g_hash_table_lookup(a->failed, url);
  if (failed && g_get_monotonic_time() - *failed < (gint64)NEGATIVE_TTL_S * G_USEC_PER_SEC)
    return NULL;
  if (g_queue_get_length(&a->queue) >= MAX_QUEUED) return NULL;
  g_hash_table_add(a->inflight, g_strdup(url));
  g_queue_push_tail(&a->queue, g_strdup(url));
  pump(a);
  return NULL;
}
