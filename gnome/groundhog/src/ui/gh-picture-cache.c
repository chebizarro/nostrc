#include "gh-picture-cache.h"
#include "gh-display-name.h"

#define MASTER_KEY "load-profile-pictures"

typedef struct {
  guint64 serial;          /* this entry, for completions (same URI, new entry) */
  GdkTexture *texture;     /* loaded */
  gboolean loading;
  gboolean failed;
  GPtrArray *pubkeys;      /* who asked, for "picture-changed" */
  GCancellable *cancellable;
} Entry;

struct _GhPictureCache {
  GObject parent_instance;
  GhPictureConsentBackend consent; /* copied */
  gboolean has_consent;
  gpointer consent_data;
  GhPicturePolicy policy;          /* copied */
  gpointer policy_data;
  GSettings *settings;
  gboolean master;
  gboolean closed;
  GhWebContent *web;
  GhPictureUriFunc uri_func;
  GObject *uri_source;
  GHashTable *by_uri;      /* uri -> Entry */
  GHashTable *allowed;     /* explicit consent: pubkey (lowercase) -> 1 */
  GHashTable *own;         /* own identities: pubkey (lowercase) -> 1 */
  guint64 epoch;           /* bumped by every revocation or rebinding */
  guint64 serials;
};
G_DEFINE_FINAL_TYPE(GhPictureCache, gh_picture_cache, G_TYPE_OBJECT)

enum { SIGNAL_PICTURE_CHANGED, SIGNAL_POLICY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
entry_free(gpointer data)
{
  Entry *entry = data;
  if (entry->cancellable) {
    g_cancellable_cancel(entry->cancellable);
    g_object_unref(entry->cancellable);
  }
  g_clear_object(&entry->texture);
  g_ptr_array_unref(entry->pubkeys);
  g_free(entry);
}

static gchar *
lower(const gchar *pubkey)
{
  return g_ascii_strdown(pubkey, -1);
}

static void
load_consent(GhPictureCache *self)
{
  g_hash_table_remove_all(self->allowed);
  if (!self->has_consent)
    return;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) keys = self->consent.list(self->consent_data, &error);
  if (!keys) {
    g_warning("profile-picture consent: %s", error ? error->message : "?");
    return;
  }
  for (guint i = 0; keys[i]; i++)
    g_hash_table_add(self->allowed, lower(keys[i]));
}

static void
save_allow(GhPictureCache *self, const gchar *pubkey)
{
  if (!self->has_consent)
    return;
  g_autoptr(GError) error = NULL;
  if (!self->consent.set(self->consent_data, pubkey, g_get_real_time() / G_USEC_PER_SEC, &error))
    g_warning("profile-picture consent for %s: %s", pubkey, error ? error->message : "?");
}

/* The eligibility formula (gh-picture-cache.h), on a lowercase key. */
static gboolean
eligible(GhPictureCache *self, const gchar *key)
{
  if (self->closed || !self->master)
    return FALSE;
  if (self->policy.is_hidden && self->policy.is_hidden(self->policy_data, key))
    return FALSE;
  if (g_hash_table_contains(self->own, key) || g_hash_table_contains(self->allowed, key))
    return TRUE;
  return self->policy.is_followed && self->policy.is_followed(self->policy_data, key);
}

gboolean
gh_picture_cache_is_allowed(GhPictureCache *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), FALSE);
  if (!pubkey) return FALSE;
  g_autofree gchar *key = lower(pubkey);
  return eligible(self, key);
}

gboolean
gh_picture_cache_has_consent(GhPictureCache *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), FALSE);
  if (!pubkey) return FALSE;
  g_autofree gchar *key = lower(pubkey);
  return g_hash_table_contains(self->allowed, key);
}

gboolean
gh_picture_cache_is_followed(GhPictureCache *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), FALSE);
  if (!pubkey || !self->master || !self->policy.is_followed) return FALSE;
  g_autofree gchar *key = lower(pubkey);
  if (self->policy.is_hidden && self->policy.is_hidden(self->policy_data, key))
    return FALSE;
  return self->policy.is_followed(self->policy_data, key);
}

gboolean
gh_picture_cache_get_master(GhPictureCache *self)
{
  g_return_val_if_fail(GH_IS_PICTURE_CACHE(self), FALSE);
  return self->master;
}

void
gh_picture_cache_allow(GhPictureCache *self, const gchar *pubkey)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  if (!pubkey || !*pubkey) return;
  g_autofree gchar *key = lower(pubkey);
  g_hash_table_add(self->allowed, g_strdup(key));
  save_allow(self, key);
  /* A failed URL may be tried again now that someone asked anew. */
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->by_uri);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    ((Entry *)value)->failed = FALSE;
}

/* Every pubkey the cache knows: consented, own, or waiting on an entry. */
static void
collect_known(GhPictureCache *self, GHashTable *who)
{
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->by_uri);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Entry *entry = value;
    for (guint i = 0; i < entry->pubkeys->len; i++)
      g_hash_table_add(who, g_strdup(g_ptr_array_index(entry->pubkeys, i)));
  }
  g_hash_table_iter_init(&iter, self->allowed);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(who, g_strdup(key));
  g_hash_table_iter_init(&iter, self->own);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(who, g_strdup(key));
}

static void
emit_changed(GhPictureCache *self, GHashTable *who, gboolean policy)
{
  g_object_ref(self);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, who);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_signal_emit(self, signals[SIGNAL_PICTURE_CHANGED], 0, key);
  if (policy)
    g_signal_emit(self, signals[SIGNAL_POLICY_CHANGED], 0);
  g_object_unref(self);
}

/* Cancels every load and forgets every texture; nothing of the epoch that
 * ends here can complete. */
static void
drop_entries(GhPictureCache *self)
{
  self->epoch++;
  g_hash_table_remove_all(self->by_uri);
}

void
gh_picture_cache_set_uri_source(GhPictureCache *self, GhPictureUriFunc func, GObject *source)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  if (self->uri_func == func && self->uri_source == source)
    return;
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_known(self, who);
  drop_entries(self);
  self->uri_func = func;
  g_set_object(&self->uri_source, source);
  emit_changed(self, who, TRUE);
}

GdkTexture *
gh_picture_cache_get_for(GhPictureCache *self, const gchar *pubkey)
{
  if (!gh_picture_cache_is_allowed(self, pubkey) || !self->uri_func)
    return NULL;
  g_autofree gchar *uri = self->uri_func(pubkey, self->uri_source);
  return gh_picture_cache_get(self, pubkey, uri);
}

typedef struct {
  GhPictureCache *cache;
  AdwAvatar *avatar; /* owns this binding */
  gchar *pubkey;
} AvatarBinding;
#define AVATAR_BINDING "gh-picture-cache-avatar"

static void
avatar_refresh(AdwAvatar *avatar)
{
  AvatarBinding *binding = g_object_get_data(G_OBJECT(avatar), AVATAR_BINDING);
  if (!binding)
    return;
  GdkTexture *texture = gh_picture_cache_get_for(binding->cache, binding->pubkey);
  adw_avatar_set_custom_image(avatar, texture ? GDK_PAINTABLE(texture) : NULL);
}
static void
avatar_policy_changed(GhPictureCache *cache, AdwAvatar *avatar)
{
  (void)cache;
  avatar_refresh(avatar);
}
static void
avatar_picture_changed(GObject *source, const gchar *pubkey, AdwAvatar *avatar)
{
  (void)source;
  AvatarBinding *binding = g_object_get_data(G_OBJECT(avatar), AVATAR_BINDING);
  if (binding && (!pubkey || !g_strcmp0(pubkey, binding->pubkey)))
    avatar_refresh(avatar);
}
static void
avatar_binding_free(gpointer data)
{
  AvatarBinding *binding = data;
  g_signal_handlers_disconnect_by_data(binding->cache, binding->avatar);
  g_signal_handlers_disconnect_by_data(gh_display_name_get_notifier(), binding->avatar);
  g_object_unref(binding->cache);
  g_free(binding->pubkey);
  g_free(binding);
}
void
gh_picture_cache_bind_avatar(GhPictureCache *self, AdwAvatar *avatar, const gchar *pubkey)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_return_if_fail(ADW_IS_AVATAR(avatar));
  g_return_if_fail(pubkey != NULL);
  AvatarBinding *binding = g_new0(AvatarBinding, 1);
  binding->cache = g_object_ref(self);
  binding->avatar = avatar;
  binding->pubkey = lower(pubkey);
  g_object_set_data_full(G_OBJECT(avatar), AVATAR_BINDING, binding, avatar_binding_free);
  g_signal_connect_object(self, "picture-changed", G_CALLBACK(avatar_picture_changed), avatar, 0);
  g_signal_connect_object(self, "policy-changed", G_CALLBACK(avatar_policy_changed), avatar, 0);
  g_signal_connect_object(gh_display_name_get_notifier(), "changed", G_CALLBACK(avatar_picture_changed), avatar, 0);
  avatar_refresh(avatar);
}

void
gh_picture_cache_revoke_all(GhPictureCache *self)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_known(self, who);
  drop_entries(self);
  g_hash_table_remove_all(self->allowed);
  if (self->has_consent) {
    g_autoptr(GError) error = NULL;
    if (!self->consent.clear(self->consent_data, &error))
      g_warning("profile-picture consent: %s", error ? error->message : "?");
  }
  emit_changed(self, who, TRUE);
}

void
gh_picture_cache_set_master(GhPictureCache *self, gboolean enabled)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  if (self->closed)
    return;
  enabled = !!enabled;
  if (self->master == enabled)
    return;
  self->master = enabled;
  if (!enabled) {
    /* Off: cancel, drop and clear consent, as the preference promises. */
    gh_picture_cache_revoke_all(self);
    return;
  }
  /* On: own and followed pictures are eligible again; say so. */
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_known(self, who);
  emit_changed(self, who, TRUE);
}

static void
on_master_setting(GSettings *settings, const gchar *key, GhPictureCache *self)
{
  (void)key;
  gh_picture_cache_set_master(self, g_settings_get_boolean(settings, MASTER_KEY));
}

void
gh_picture_cache_bind_settings(GhPictureCache *self, GSettings *settings)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_return_if_fail(!settings || G_IS_SETTINGS(settings));
  if (self->settings)
    g_signal_handlers_disconnect_by_data(self->settings, self);
  g_set_object(&self->settings, settings);
  if (!settings)
    return;
  g_signal_connect_object(settings, "changed::" MASTER_KEY, G_CALLBACK(on_master_setting), self,
                          0);
  /* The start state, not a change: an off switch at startup fetches nothing
   * and leaves the persisted consents where they are (a later change to
   * off is what revokes them). */
  self->master = g_settings_get_boolean(settings, MASTER_KEY);
}

/* Drops the entries (or the pubkeys of entries) that lost eligibility;
 * who gets every pubkey affected. */
static void
drop_ineligible(GhPictureCache *self, const gchar *only, GHashTable *who)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->by_uri);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Entry *entry = value;
    for (guint i = 0; i < entry->pubkeys->len;) {
      const gchar *pubkey = g_ptr_array_index(entry->pubkeys, i);
      if ((only && g_strcmp0(pubkey, only) != 0) || eligible(self, pubkey)) {
        i++;
        continue;
      }
      g_hash_table_add(who, g_strdup(pubkey));
      g_ptr_array_remove_index(entry->pubkeys, i);
    }
    if (!entry->pubkeys->len)
      g_hash_table_iter_remove(&iter); /* nobody eligible asks: cancelled, dropped */
  }
}

void
gh_picture_cache_policy_changed(GhPictureCache *self, const gchar *pubkey)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autofree gchar *key = pubkey ? lower(pubkey) : NULL;
  drop_ineligible(self, key, who);
  if (key)
    g_hash_table_add(who, g_strdup(key)); /* it may be eligible now: ask again */
  emit_changed(self, who, TRUE);
}

void
gh_picture_cache_set_policy(GhPictureCache *self, const GhPicturePolicy *policy, gpointer data)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  self->policy = policy ? *policy : (GhPicturePolicy){ 0 };
  self->policy_data = policy ? data : NULL;
  gh_picture_cache_policy_changed(self, NULL);
}

void
gh_picture_cache_set_own(GhPictureCache *self, const gchar *const *pubkeys)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  g_autoptr(GHashTable) next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; pubkeys && pubkeys[i]; i++)
    if (*pubkeys[i])
      g_hash_table_add(next, lower(pubkeys[i]));
  /* The same set again (every account-switcher refresh) changes nothing. */
  gboolean same = g_hash_table_size(next) == g_hash_table_size(self->own);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, next);
  while (same && g_hash_table_iter_next(&iter, &key, NULL))
    same = g_hash_table_contains(self->own, key);
  if (same)
    return;
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_iter_init(&iter, self->own);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(who, g_strdup(key));
  g_hash_table_iter_init(&iter, next);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(who, g_strdup(key));
  g_hash_table_unref(self->own);
  self->own = g_steal_pointer(&next);
  drop_ineligible(self, NULL, who);
  emit_changed(self, who, TRUE);
}

typedef struct {
  GWeakRef cache;
  gchar *uri;
  guint64 epoch;
  guint64 serial;
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
    /* Only the exact entry that started this load, in the same epoch: a
     * revocation or rebinding in between, or a new entry for the same URI,
     * never takes a stale answer. */
    if (entry && entry->serial == fetch->serial && self->epoch == fetch->epoch &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      entry->loading = FALSE;
      g_clear_object(&entry->cancellable);
      if (loaded && loaded->texture) {
        g_set_object(&entry->texture, loaded->texture);
      } else {
        entry->failed = TRUE;
        g_debug("picture %s: %s", fetch->uri, error ? error->message : "no image");
      }
      g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      for (guint i = 0; i < entry->pubkeys->len; i++)
        g_hash_table_add(who, g_strdup(g_ptr_array_index(entry->pubkeys, i)));
      emit_changed(self, who, FALSE);
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
  if (!pubkey || !uri || !*uri)
    return NULL;
  g_autofree gchar *key = lower(pubkey);
  if (!eligible(self, key))
    return NULL;
  Entry *entry = g_hash_table_lookup(self->by_uri, uri);
  if (!entry) {
    entry = g_new0(Entry, 1);
    entry->serial = ++self->serials;
    entry->pubkeys = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(self->by_uri, g_strdup(uri), entry);
  }
  if (!g_ptr_array_find_with_equal_func(entry->pubkeys, key, g_str_equal, NULL))
    g_ptr_array_add(entry->pubkeys, g_strdup(key));
  if (entry->texture)
    return entry->texture;
  if (entry->loading || entry->failed || !self->web)
    return NULL;
  entry->loading = TRUE;
  entry->cancellable = g_cancellable_new();
  Fetch *fetch = g_new0(Fetch, 1);
  g_weak_ref_init(&fetch->cache, self);
  fetch->uri = g_strdup(uri);
  fetch->epoch = self->epoch;
  fetch->serial = entry->serial;
  gh_web_content_load_async(self->web, uri, GH_WEB_PICTURE, entry->cancellable, on_loaded, fetch);
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
close_cache(GhPictureCache *self, gboolean notify)
{
  if (self->closed)
    return;
  g_autoptr(GHashTable) who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_known(self, who);
  self->closed = TRUE;
  self->master = FALSE;
  self->policy = (GhPicturePolicy){ 0 };
  self->policy_data = NULL;
  self->has_consent = FALSE;
  self->consent_data = NULL;
  drop_entries(self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data(self->settings, self);
  g_clear_object(&self->settings);
  g_clear_object(&self->web);
  g_clear_object(&self->uri_source);
  self->uri_func = NULL;
  if (notify)
    emit_changed(self, who, TRUE);
}

void
gh_picture_cache_close(GhPictureCache *self)
{
  g_return_if_fail(GH_IS_PICTURE_CACHE(self));
  close_cache(self, TRUE);
}

static void
gh_picture_cache_dispose(GObject *object)
{
  close_cache(GH_PICTURE_CACHE(object), FALSE);
  G_OBJECT_CLASS(gh_picture_cache_parent_class)->dispose(object);
}

static void
gh_picture_cache_finalize(GObject *object)
{
  GhPictureCache *self = GH_PICTURE_CACHE(object);
  g_hash_table_unref(self->by_uri);
  g_hash_table_unref(self->allowed);
  g_hash_table_unref(self->own);
  G_OBJECT_CLASS(gh_picture_cache_parent_class)->finalize(object);
}

static void
gh_picture_cache_class_init(GhPictureCacheClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_picture_cache_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_picture_cache_finalize;
  signals[SIGNAL_PICTURE_CHANGED] = g_signal_new("picture-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_POLICY_CHANGED] = g_signal_new("policy-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_picture_cache_init(GhPictureCache *self)
{
  self->by_uri = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
  self->allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->own = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->master = TRUE; /* until settings are bound */
  self->epoch = 1;
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
  if (self->has_consent == (backend != NULL) && self->consent_data == data &&
      (!backend || (self->consent.list == backend->list && self->consent.set == backend->set &&
                    self->consent.clear == backend->clear)))
    return;
  /* Another account's pictures and consent never carry over. Notify both
   * the old and newly allowed pubkeys: views must drop old textures and ask
   * for the restored account's pictures again after consent is loaded. */
  g_autoptr(GHashTable) changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_known(self, changed);
  drop_entries(self);
  self->has_consent = backend != NULL;
  self->consent = backend ? *backend : (GhPictureConsentBackend){ 0 };
  self->consent_data = backend ? data : NULL;
  load_consent(self);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->allowed);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_hash_table_add(changed, g_strdup(key));
  emit_changed(self, changed, FALSE);
}
