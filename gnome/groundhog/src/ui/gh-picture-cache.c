#include "gh-picture-cache.h"

typedef struct {
  GdkTexture *texture;     /* loaded */
  gboolean loading;
  gboolean failed;
  GPtrArray *pubkeys;      /* who asked, for "picture-changed" */
} Entry;

struct _GhPictureCache {
  GObject parent_instance;
  const GhPictureConsentBackend *consent; /* nullable */
  gpointer consent_data;
  GhWebContent *web;
  GHashTable *by_uri;      /* uri -> Entry */
  GHashTable *allowed;     /* pubkey (lowercase) -> 1 */
  GCancellable *cancellable;
};
G_DEFINE_FINAL_TYPE(GhPictureCache, gh_picture_cache, G_TYPE_OBJECT)

enum { SIGNAL_PICTURE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
entry_free(gpointer data)
{
  Entry *entry = data;
  g_clear_object(&entry->texture);
  g_ptr_array_unref(entry->pubkeys);
  g_free(entry);
}

static void
load_consent(GhPictureCache *self)
{
  g_hash_table_remove_all(self->allowed);
  if (!self->consent)
    return;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) keys = self->consent->list(self->consent_data, &error);
  if (!keys) {
    g_warning("profile-picture consent: %s", error ? error->message : "?");
    return;
  }
  for (guint i = 0; keys[i]; i++)
    g_hash_table_add(self->allowed, g_ascii_strdown(keys[i], -1));
}

static void
save_allow(GhPictureCache *self, const gchar *pubkey)
{
  if (!self->consent)
    return;
  g_autoptr(GError) error = NULL;
  if (!self->consent->set(self->consent_data, pubkey, g_get_real_time() / G_USEC_PER_SEC, &error))
    g_warning("profile-picture consent for %s: %s", pubkey, error ? error->message : "?");
}

gboolean
gh_picture_cache_is_allowed(GhPictureCache *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), FALSE);
  if (!pubkey) return FALSE;
  g_autofree gchar *key = g_ascii_strdown(pubkey, -1);
  return g_hash_table_contains(self->allowed, key);
}

void
gh_picture_cache_allow(GhPictureCache *self, const gchar *pubkey)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  if (!pubkey || !*pubkey) return;
  g_autofree gchar *key = g_ascii_strdown(pubkey, -1);
  g_hash_table_add(self->allowed, g_strdup(key));
  save_allow(self, key);
  /* A failed URL may be tried again now that someone asked anew. */
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->by_uri);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    ((Entry *)value)->failed = FALSE;
}

void
gh_picture_cache_revoke_all(GhPictureCache *self)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_autoptr(GPtrArray) who = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->by_uri);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Entry *entry = value;
    for (guint i = 0; i < entry->pubkeys->len; i++)
      g_ptr_array_add(who, g_strdup(g_ptr_array_index(entry->pubkeys, i)));
  }
  g_hash_table_remove_all(self->by_uri);
  g_hash_table_iter_init(&iter, self->allowed);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(who, g_strdup(key));
  g_hash_table_remove_all(self->allowed);
  if (self->consent) {
    g_autoptr(GError) error = NULL;
    if (!self->consent->clear(self->consent_data, &error))
      g_warning("profile-picture consent: %s", error ? error->message : "?");
  }
  for (guint i = 0; i < who->len; i++)
    g_signal_emit(self, signals[SIGNAL_PICTURE_CHANGED], 0, g_ptr_array_index(who, i));
}

typedef struct {
  GWeakRef cache;
  gchar *uri;
} Fetch;

static void
on_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Fetch *fetch = data;
  g_autoptr(GhPictureCache) self = g_weak_ref_get(&fetch->cache);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhWebResult) loaded = gh_web_content_load_finish(GH_WEB_CONTENT(source), result, &error);
  if (self) {
    Entry *entry = g_hash_table_lookup(self->by_uri, fetch->uri);
    if (entry && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      entry->loading = FALSE;
      if (loaded && loaded->texture) {
        g_set_object(&entry->texture, loaded->texture);
      } else {
        entry->failed = TRUE;
        g_debug("picture %s: %s", fetch->uri, error ? error->message : "no image");
      }
      g_autoptr(GPtrArray) who = g_ptr_array_new_with_free_func(g_free);
      for (guint i = 0; i < entry->pubkeys->len; i++)
        g_ptr_array_add(who, g_strdup(g_ptr_array_index(entry->pubkeys, i)));
      for (guint i = 0; i < who->len; i++)
        g_signal_emit(self, signals[SIGNAL_PICTURE_CHANGED], 0, g_ptr_array_index(who, i));
    }
  }
  g_weak_ref_clear(&fetch->cache);
  g_free(fetch->uri);
  g_free(fetch);
}

GdkTexture *
gh_picture_cache_get(GhPictureCache *self, const gchar *pubkey, const gchar *uri)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), NULL);
  if (!pubkey || !uri || !*uri || !gh_picture_cache_is_allowed(self, pubkey))
    return NULL;
  Entry *entry = g_hash_table_lookup(self->by_uri, uri);
  if (!entry) {
    entry = g_new0(Entry, 1);
    entry->pubkeys = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(self->by_uri, g_strdup(uri), entry);
  }
  if (!g_ptr_array_find_with_equal_func(entry->pubkeys, pubkey, g_str_equal, NULL))
    g_ptr_array_add(entry->pubkeys, g_strdup(pubkey));
  if (entry->texture)
    return entry->texture;
  if (entry->loading || entry->failed || !self->web)
    return NULL;
  entry->loading = TRUE;
  if (!self->cancellable)
    self->cancellable = g_cancellable_new();
  Fetch *fetch = g_new0(Fetch, 1);
  g_weak_ref_init(&fetch->cache, self);
  fetch->uri = g_strdup(uri);
  gh_web_content_load_async(self->web, uri, GH_WEB_PICTURE, self->cancellable, on_loaded, fetch);
  return NULL;
}

gboolean
gh_picture_cache_is_loading(GhPictureCache *self, const gchar *uri)
{
  Entry *entry = uri ? g_hash_table_lookup(self->by_uri, uri) : NULL;
  return entry && entry->loading;
}

gboolean
gh_picture_cache_has_failed(GhPictureCache *self, const gchar *uri)
{
  Entry *entry = uri ? g_hash_table_lookup(self->by_uri, uri) : NULL;
  return entry && entry->failed;
}

static void
gh_picture_cache_dispose(GObject *object)
{
  GhPictureCache *self = GH_PICTURE_CACHE(object);
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_clear_object(&self->web);
  self->consent = NULL;
  G_OBJECT_CLASS(gh_picture_cache_parent_class)->dispose(object);
}

static void
gh_picture_cache_finalize(GObject *object)
{
  GhPictureCache *self = GH_PICTURE_CACHE(object);
  g_hash_table_unref(self->by_uri);
  g_hash_table_unref(self->allowed);
  G_OBJECT_CLASS(gh_picture_cache_parent_class)->finalize(object);
}

static void
gh_picture_cache_class_init(GhPictureCacheClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_picture_cache_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_picture_cache_finalize;
  signals[SIGNAL_PICTURE_CHANGED] = g_signal_new("picture-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_picture_cache_init(GhPictureCache *self)
{
  self->by_uri = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
  self->allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GhPictureCache *
gh_picture_cache_new(GhWebContent *web)
{
  GhPictureCache *self = g_object_new(GH_TYPE_PICTURE_CACHE, NULL);
  self->web = web ? g_object_ref(web) : NULL;
  return self;
}

void
gh_picture_cache_set_consent(GhPictureCache *self, const GhPictureConsentBackend *backend,
                             gpointer data)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_return_if_fail(!backend || (backend->list && backend->set && backend->clear));
  if (self->consent == backend && self->consent_data == data)
    return;
  /* Another account's pictures and consent never carry over. */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  g_hash_table_remove_all(self->by_uri);
  self->consent = backend;
  self->consent_data = data;
  load_consent(self);
}
