/* profile_store.c - Nostr profile management implementation
 *
 * Caches profiles locally and provides helpers for editing.
 */
#include "profile_store.h"
#include <json.h>
#include <string.h>
#include <time.h>

struct _ProfileStore {
  GHashTable *profiles;  /* npub -> NostrProfile* */
  gchar *cache_dir;
};

static const gchar *get_cache_dir(void) {
  static gchar *dir = NULL;
  if (!dir) {
    const gchar *cache = g_get_user_cache_dir();
    dir = g_build_filename(cache, "gnostr-signer", "profiles", NULL);
    g_mkdir_with_parents(dir, 0700);
  }
  return dir;
}

const gchar *profile_store_cache_dir(void) {
  return get_cache_dir();
}

void nostr_profile_free(NostrProfile *p) {
  if (!p) return;
  g_free(p->npub);
  g_free(p->name);
  g_free(p->about);
  g_free(p->picture);
  g_free(p->banner);
  g_free(p->nip05);
  g_free(p->lud16);
  g_free(p->website);
  g_free(p);
}

NostrProfile *nostr_profile_copy(const NostrProfile *p) {
  if (!p) return NULL;
  NostrProfile *copy = g_new0(NostrProfile, 1);
  copy->npub = g_strdup(p->npub);
  copy->name = g_strdup(p->name);
  copy->about = g_strdup(p->about);
  copy->picture = g_strdup(p->picture);
  copy->banner = g_strdup(p->banner);
  copy->nip05 = g_strdup(p->nip05);
  copy->lud16 = g_strdup(p->lud16);
  copy->website = g_strdup(p->website);
  copy->created_at = p->created_at;
  copy->dirty = p->dirty;
  return copy;
}

ProfileStore *profile_store_new(void) {
  ProfileStore *ps = g_new0(ProfileStore, 1);
  ps->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)nostr_profile_free);
  ps->cache_dir = g_strdup(get_cache_dir());
  return ps;
}

void profile_store_free(ProfileStore *ps) {
  if (!ps) return;
  g_hash_table_destroy(ps->profiles);
  g_free(ps->cache_dir);
  g_free(ps);
}

NostrProfile *profile_store_get(ProfileStore *ps, const gchar *npub) {
  if (!ps || !npub) return NULL;

  NostrProfile *p = g_hash_table_lookup(ps->profiles, npub);
  if (p) {
    return nostr_profile_copy(p);
  }

  /* Try loading from cache */
  NostrProfile *cached = NULL;
  if (profile_store_load_cached(ps, npub, &cached)) {
    g_hash_table_insert(ps->profiles, g_strdup(npub), nostr_profile_copy(cached));
    return cached;
  }

  /* Create empty profile */
  p = g_new0(NostrProfile, 1);
  p->npub = g_strdup(npub);
  p->created_at = 0;
  p->dirty = FALSE;

  g_hash_table_insert(ps->profiles, g_strdup(npub), nostr_profile_copy(p));
  return p;
}

void profile_store_update(ProfileStore *ps, const NostrProfile *profile) {
  if (!ps || !profile || !profile->npub) return;

  NostrProfile *copy = nostr_profile_copy(profile);
  copy->dirty = TRUE;

  g_hash_table_replace(ps->profiles, g_strdup(profile->npub), copy);

  /* Also save to cache */
  profile_store_save_cached(ps, profile);
}

NostrProfile *profile_store_parse_event(const gchar *event_json) {
  if (!event_json) return NULL;

  /* Parse JSON - looking for content field which is itself JSON */
  json_object *root = json_tokener_parse(event_json);
  if (!root) return NULL;

  NostrProfile *p = g_new0(NostrProfile, 1);

  /* Get pubkey */
  json_object *pubkey_obj = NULL;
  if (json_object_object_get_ex(root, "pubkey", &pubkey_obj)) {
    const gchar *pk = json_object_get_string(pubkey_obj);
    if (pk) {
      /* Convert hex to npub */
      p->npub = g_strdup(pk); /* For now, store as hex; convert later if needed */
    }
  }

  /* Get created_at */
  json_object *created_obj = NULL;
  if (json_object_object_get_ex(root, "created_at", &created_obj)) {
    p->created_at = json_object_get_int64(created_obj);
  }

  /* Parse content (which is JSON string of metadata) */
  json_object *content_obj = NULL;
  if (json_object_object_get_ex(root, "content", &content_obj)) {
    const gchar *content_str = json_object_get_string(content_obj);
    if (content_str && *content_str) {
      json_object *meta = json_tokener_parse(content_str);
      if (meta) {
        json_object *val;

        if (json_object_object_get_ex(meta, "name", &val)) {
          p->name = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "about", &val)) {
          p->about = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "picture", &val)) {
          p->picture = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "banner", &val)) {
          p->banner = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "nip05", &val)) {
          p->nip05 = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "lud16", &val)) {
          p->lud16 = g_strdup(json_object_get_string(val));
        }
        if (json_object_object_get_ex(meta, "website", &val)) {
          p->website = g_strdup(json_object_get_string(val));
        }

        json_object_put(meta);
      }
    }
  }

  json_object_put(root);
  return p;
}

gchar *profile_store_build_event_json(const NostrProfile *profile) {
  if (!profile) return NULL;

  /* Build content object */
  json_object *content = json_object_new_object();

  if (profile->name && *profile->name) {
    json_object_object_add(content, "name", json_object_new_string(profile->name));
  }
  if (profile->about && *profile->about) {
    json_object_object_add(content, "about", json_object_new_string(profile->about));
  }
  if (profile->picture && *profile->picture) {
    json_object_object_add(content, "picture", json_object_new_string(profile->picture));
  }
  if (profile->banner && *profile->banner) {
    json_object_object_add(content, "banner", json_object_new_string(profile->banner));
  }
  if (profile->nip05 && *profile->nip05) {
    json_object_object_add(content, "nip05", json_object_new_string(profile->nip05));
  }
  if (profile->lud16 && *profile->lud16) {
    json_object_object_add(content, "lud16", json_object_new_string(profile->lud16));
  }
  if (profile->website && *profile->website) {
    json_object_object_add(content, "website", json_object_new_string(profile->website));
  }

  const gchar *content_str = json_object_to_json_string(content);

  /* Build event object */
  json_object *event = json_object_new_object();
  json_object_object_add(event, "kind", json_object_new_int(0));
  json_object_object_add(event, "created_at", json_object_new_int64((int64_t)time(NULL)));
  json_object_object_add(event, "tags", json_object_new_array());
  json_object_object_add(event, "content", json_object_new_string(content_str));

  gchar *result = g_strdup(json_object_to_json_string(event));

  json_object_put(content);
  json_object_put(event);

  return result;
}

gboolean profile_store_load_cached(ProfileStore *ps, const gchar *npub,
                                   NostrProfile **out_profile) {
  if (!ps || !npub || !out_profile) return FALSE;
  *out_profile = NULL;

  /* Build cache file path using sanitized npub */
  gchar *safe_npub = g_strdup(npub);
  for (gchar *c = safe_npub; *c; c++) {
    if (*c == '/' || *c == '\\') *c = '_';
  }

  gchar *path = g_build_filename(ps->cache_dir, safe_npub, NULL);
  g_free(safe_npub);

  gchar *contents = NULL;
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_get_contents(path, &contents, &len, &err)) {
    if (err) g_clear_error(&err);
    g_free(path);
    return FALSE;
  }
  g_free(path);

  /* Parse cached JSON */
  json_object *root = json_tokener_parse(contents);
  g_free(contents);

  if (!root) return FALSE;

  NostrProfile *p = g_new0(NostrProfile, 1);
  p->npub = g_strdup(npub);

  json_object *val;
  if (json_object_object_get_ex(root, "name", &val)) {
    p->name = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "about", &val)) {
    p->about = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "picture", &val)) {
    p->picture = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "banner", &val)) {
    p->banner = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "nip05", &val)) {
    p->nip05 = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "lud16", &val)) {
    p->lud16 = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "website", &val)) {
    p->website = g_strdup(json_object_get_string(val));
  }
  if (json_object_object_get_ex(root, "created_at", &val)) {
    p->created_at = json_object_get_int64(val);
  }

  json_object_put(root);

  *out_profile = p;
  return TRUE;
}

void profile_store_save_cached(ProfileStore *ps, const NostrProfile *profile) {
  if (!ps || !profile || !profile->npub) return;

  json_object *root = json_object_new_object();

  if (profile->name) {
    json_object_object_add(root, "name", json_object_new_string(profile->name));
  }
  if (profile->about) {
    json_object_object_add(root, "about", json_object_new_string(profile->about));
  }
  if (profile->picture) {
    json_object_object_add(root, "picture", json_object_new_string(profile->picture));
  }
  if (profile->banner) {
    json_object_object_add(root, "banner", json_object_new_string(profile->banner));
  }
  if (profile->nip05) {
    json_object_object_add(root, "nip05", json_object_new_string(profile->nip05));
  }
  if (profile->lud16) {
    json_object_object_add(root, "lud16", json_object_new_string(profile->lud16));
  }
  if (profile->website) {
    json_object_object_add(root, "website", json_object_new_string(profile->website));
  }
  json_object_object_add(root, "created_at", json_object_new_int64(profile->created_at));

  const gchar *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);

  /* Build cache file path */
  gchar *safe_npub = g_strdup(profile->npub);
  for (gchar *c = safe_npub; *c; c++) {
    if (*c == '/' || *c == '\\') *c = '_';
  }

  gchar *path = g_build_filename(ps->cache_dir, safe_npub, NULL);
  g_free(safe_npub);

  GError *err = NULL;
  if (!g_file_set_contents(path, json_str, -1, &err)) {
    if (err) {
      g_warning("profile_store_save_cached: %s", err->message);
      g_clear_error(&err);
    }
  }

  g_free(path);
  json_object_put(root);
}

gboolean profile_store_is_dirty(ProfileStore *ps, const gchar *npub) {
  if (!ps || !npub) return FALSE;
  NostrProfile *p = g_hash_table_lookup(ps->profiles, npub);
  return p ? p->dirty : FALSE;
}

void profile_store_clear_dirty(ProfileStore *ps, const gchar *npub) {
  if (!ps || !npub) return;
  NostrProfile *p = g_hash_table_lookup(ps->profiles, npub);
  if (p) p->dirty = FALSE;
}
