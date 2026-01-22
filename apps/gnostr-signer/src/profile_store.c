/* profile_store.c - Nostr profile management implementation
 *
 * Caches profiles locally and provides helpers for editing.
 */
#include "profile_store.h"
#include <json-glib/json-glib.h>
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
  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_clear_error(&error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);
  NostrProfile *p = g_new0(NostrProfile, 1);

  /* Get pubkey */
  if (json_object_has_member(root, "pubkey")) {
    const gchar *pk = json_object_get_string_member(root, "pubkey");
    if (pk) {
      /* Convert hex to npub */
      p->npub = g_strdup(pk); /* For now, store as hex; convert later if needed */
    }
  }

  /* Get created_at */
  if (json_object_has_member(root, "created_at")) {
    p->created_at = json_object_get_int_member(root, "created_at");
  }

  /* Parse content (which is JSON string of metadata) */
  if (json_object_has_member(root, "content")) {
    const gchar *content_str = json_object_get_string_member(root, "content");
    if (content_str && *content_str) {
      JsonParser *meta_parser = json_parser_new();
      if (json_parser_load_from_data(meta_parser, content_str, -1, NULL)) {
        JsonNode *meta_node = json_parser_get_root(meta_parser);
        if (JSON_NODE_HOLDS_OBJECT(meta_node)) {
          JsonObject *meta = json_node_get_object(meta_node);

          if (json_object_has_member(meta, "name")) {
            p->name = g_strdup(json_object_get_string_member(meta, "name"));
          }
          if (json_object_has_member(meta, "about")) {
            p->about = g_strdup(json_object_get_string_member(meta, "about"));
          }
          if (json_object_has_member(meta, "picture")) {
            p->picture = g_strdup(json_object_get_string_member(meta, "picture"));
          }
          if (json_object_has_member(meta, "banner")) {
            p->banner = g_strdup(json_object_get_string_member(meta, "banner"));
          }
          if (json_object_has_member(meta, "nip05")) {
            p->nip05 = g_strdup(json_object_get_string_member(meta, "nip05"));
          }
          if (json_object_has_member(meta, "lud16")) {
            p->lud16 = g_strdup(json_object_get_string_member(meta, "lud16"));
          }
          if (json_object_has_member(meta, "website")) {
            p->website = g_strdup(json_object_get_string_member(meta, "website"));
          }
        }
      }
      g_object_unref(meta_parser);
    }
  }

  g_object_unref(parser);
  return p;
}

gchar *profile_store_build_event_json(const NostrProfile *profile) {
  if (!profile) return NULL;

  /* Build content object */
  JsonBuilder *content_builder = json_builder_new();
  json_builder_begin_object(content_builder);

  if (profile->name && *profile->name) {
    json_builder_set_member_name(content_builder, "name");
    json_builder_add_string_value(content_builder, profile->name);
  }
  if (profile->about && *profile->about) {
    json_builder_set_member_name(content_builder, "about");
    json_builder_add_string_value(content_builder, profile->about);
  }
  if (profile->picture && *profile->picture) {
    json_builder_set_member_name(content_builder, "picture");
    json_builder_add_string_value(content_builder, profile->picture);
  }
  if (profile->banner && *profile->banner) {
    json_builder_set_member_name(content_builder, "banner");
    json_builder_add_string_value(content_builder, profile->banner);
  }
  if (profile->nip05 && *profile->nip05) {
    json_builder_set_member_name(content_builder, "nip05");
    json_builder_add_string_value(content_builder, profile->nip05);
  }
  if (profile->lud16 && *profile->lud16) {
    json_builder_set_member_name(content_builder, "lud16");
    json_builder_add_string_value(content_builder, profile->lud16);
  }
  if (profile->website && *profile->website) {
    json_builder_set_member_name(content_builder, "website");
    json_builder_add_string_value(content_builder, profile->website);
  }

  json_builder_end_object(content_builder);
  JsonNode *content_node = json_builder_get_root(content_builder);
  JsonGenerator *content_gen = json_generator_new();
  json_generator_set_root(content_gen, content_node);
  gchar *content_str = json_generator_to_data(content_gen, NULL);
  g_object_unref(content_gen);
  json_node_unref(content_node);
  g_object_unref(content_builder);

  /* Build event object */
  JsonBuilder *event_builder = json_builder_new();
  json_builder_begin_object(event_builder);

  json_builder_set_member_name(event_builder, "kind");
  json_builder_add_int_value(event_builder, 0);

  json_builder_set_member_name(event_builder, "created_at");
  json_builder_add_int_value(event_builder, (gint64)time(NULL));

  json_builder_set_member_name(event_builder, "tags");
  json_builder_begin_array(event_builder);
  json_builder_end_array(event_builder);

  json_builder_set_member_name(event_builder, "content");
  json_builder_add_string_value(event_builder, content_str);

  json_builder_end_object(event_builder);

  JsonNode *event_node = json_builder_get_root(event_builder);
  JsonGenerator *event_gen = json_generator_new();
  json_generator_set_root(event_gen, event_node);
  gchar *result = json_generator_to_data(event_gen, NULL);

  g_object_unref(event_gen);
  json_node_unref(event_node);
  g_object_unref(event_builder);
  g_free(content_str);

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
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, -1, NULL)) {
    g_free(contents);
    g_object_unref(parser);
    return FALSE;
  }
  g_free(contents);

  JsonNode *root_node = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *root = json_node_get_object(root_node);
  NostrProfile *p = g_new0(NostrProfile, 1);
  p->npub = g_strdup(npub);

  if (json_object_has_member(root, "name")) {
    p->name = g_strdup(json_object_get_string_member(root, "name"));
  }
  if (json_object_has_member(root, "about")) {
    p->about = g_strdup(json_object_get_string_member(root, "about"));
  }
  if (json_object_has_member(root, "picture")) {
    p->picture = g_strdup(json_object_get_string_member(root, "picture"));
  }
  if (json_object_has_member(root, "banner")) {
    p->banner = g_strdup(json_object_get_string_member(root, "banner"));
  }
  if (json_object_has_member(root, "nip05")) {
    p->nip05 = g_strdup(json_object_get_string_member(root, "nip05"));
  }
  if (json_object_has_member(root, "lud16")) {
    p->lud16 = g_strdup(json_object_get_string_member(root, "lud16"));
  }
  if (json_object_has_member(root, "website")) {
    p->website = g_strdup(json_object_get_string_member(root, "website"));
  }
  if (json_object_has_member(root, "created_at")) {
    p->created_at = json_object_get_int_member(root, "created_at");
  }

  g_object_unref(parser);

  *out_profile = p;
  return TRUE;
}

void profile_store_save_cached(ProfileStore *ps, const NostrProfile *profile) {
  if (!ps || !profile || !profile->npub) return;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  if (profile->name) {
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, profile->name);
  }
  if (profile->about) {
    json_builder_set_member_name(builder, "about");
    json_builder_add_string_value(builder, profile->about);
  }
  if (profile->picture) {
    json_builder_set_member_name(builder, "picture");
    json_builder_add_string_value(builder, profile->picture);
  }
  if (profile->banner) {
    json_builder_set_member_name(builder, "banner");
    json_builder_add_string_value(builder, profile->banner);
  }
  if (profile->nip05) {
    json_builder_set_member_name(builder, "nip05");
    json_builder_add_string_value(builder, profile->nip05);
  }
  if (profile->lud16) {
    json_builder_set_member_name(builder, "lud16");
    json_builder_add_string_value(builder, profile->lud16);
  }
  if (profile->website) {
    json_builder_set_member_name(builder, "website");
    json_builder_add_string_value(builder, profile->website);
  }
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, profile->created_at);

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, root);
  gchar *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  json_node_unref(root);
  g_object_unref(builder);

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
  g_free(json_str);
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
