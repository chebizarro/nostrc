/**
 * NIP-89 App Handlers Implementation
 *
 * Provides parsing, caching, and querying for NIP-89 app handler events.
 */

#include "nip89_handlers.h"
#include "../storage_ndb.h"
#include <string.h>
#include <jansson.h>

/* ============== Cache Configuration ============== */

#define NIP89_CACHE_MAX_HANDLERS      500
#define NIP89_CACHE_MAX_RECOMMENDATIONS 1000
#define NIP89_CACHE_TTL_SECONDS       (60 * 60 * 24)  /* 24 hours */

/* ============== Global Cache State ============== */

static GHashTable *g_handler_cache = NULL;      /* key: "pubkey:d_tag", value: GnostrNip89HandlerInfo* */
static GHashTable *g_recommendation_cache = NULL; /* key: "pubkey:kind", value: GnostrNip89Recommendation* */
static GHashTable *g_kind_to_handlers = NULL;   /* key: GUINT_TO_POINTER(kind), value: GPtrArray* of handler keys */
static GHashTable *g_user_preferences = NULL;   /* key: GUINT_TO_POINTER(kind), value: char* a_tag */
static GMutex g_nip89_cache_mutex;
static gboolean g_nip89_initialized = FALSE;

/* ============== Memory Management ============== */

void gnostr_nip89_platform_handler_free(GnostrNip89PlatformHandler *handler)
{
  if (!handler) return;
  g_free(handler->platform_name);
  g_free(handler->url_template);
  g_free(handler->identifier);
  g_free(handler);
}

void gnostr_nip89_handler_info_free(GnostrNip89HandlerInfo *info)
{
  if (!info) return;

  g_free(info->event_id_hex);
  g_free(info->pubkey_hex);
  g_free(info->d_tag);
  g_free(info->name);
  g_free(info->display_name);
  g_free(info->picture);
  g_free(info->about);
  g_free(info->banner);
  g_free(info->website);
  g_free(info->nip05);
  g_free(info->lud16);
  g_free(info->handled_kinds);

  if (info->platforms) {
    g_ptr_array_unref(info->platforms);
  }

  g_free(info);
}

void gnostr_nip89_recommendation_free(GnostrNip89Recommendation *rec)
{
  if (!rec) return;

  g_free(rec->event_id_hex);
  g_free(rec->pubkey_hex);
  g_free(rec->d_tag);
  g_free(rec->handler_a_tag);
  g_free(rec->handler_pubkey);
  g_free(rec->handler_d_tag);
  g_free(rec->relay_hint);

  g_free(rec);
}

/* ============== Platform Helpers ============== */

GnostrNip89Platform gnostr_nip89_parse_platform(const char *platform_str)
{
  if (!platform_str) return GNOSTR_NIP89_PLATFORM_UNKNOWN;

  if (g_ascii_strcasecmp(platform_str, "web") == 0)
    return GNOSTR_NIP89_PLATFORM_WEB;
  if (g_ascii_strcasecmp(platform_str, "ios") == 0)
    return GNOSTR_NIP89_PLATFORM_IOS;
  if (g_ascii_strcasecmp(platform_str, "android") == 0)
    return GNOSTR_NIP89_PLATFORM_ANDROID;
  if (g_ascii_strcasecmp(platform_str, "macos") == 0)
    return GNOSTR_NIP89_PLATFORM_MACOS;
  if (g_ascii_strcasecmp(platform_str, "windows") == 0)
    return GNOSTR_NIP89_PLATFORM_WINDOWS;
  if (g_ascii_strcasecmp(platform_str, "linux") == 0)
    return GNOSTR_NIP89_PLATFORM_LINUX;

  return GNOSTR_NIP89_PLATFORM_UNKNOWN;
}

const char *gnostr_nip89_platform_to_string(GnostrNip89Platform platform)
{
  switch (platform) {
    case GNOSTR_NIP89_PLATFORM_WEB:     return "Web";
    case GNOSTR_NIP89_PLATFORM_IOS:     return "iOS";
    case GNOSTR_NIP89_PLATFORM_ANDROID: return "Android";
    case GNOSTR_NIP89_PLATFORM_MACOS:   return "macOS";
    case GNOSTR_NIP89_PLATFORM_WINDOWS: return "Windows";
    case GNOSTR_NIP89_PLATFORM_LINUX:   return "Linux";
    default:                             return "Unknown";
  }
}

GnostrNip89Platform gnostr_nip89_get_current_platform(void)
{
#if defined(__APPLE__)
#if TARGET_OS_IOS
  return GNOSTR_NIP89_PLATFORM_IOS;
#else
  return GNOSTR_NIP89_PLATFORM_MACOS;
#endif
#elif defined(_WIN32) || defined(_WIN64)
  return GNOSTR_NIP89_PLATFORM_WINDOWS;
#elif defined(__linux__)
  return GNOSTR_NIP89_PLATFORM_LINUX;
#elif defined(__ANDROID__)
  return GNOSTR_NIP89_PLATFORM_ANDROID;
#else
  return GNOSTR_NIP89_PLATFORM_UNKNOWN;
#endif
}

/* ============== Parsing: Handler Info (kind 31989) ============== */

GnostrNip89HandlerInfo *gnostr_nip89_parse_handler_info(const char *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("nip89: failed to parse handler info JSON: %s", err.text);
    return NULL;
  }

  /* Validate kind */
  json_t *kind_j = json_object_get(root, "kind");
  if (!json_is_integer(kind_j) || json_integer_value(kind_j) != GNOSTR_NIP89_KIND_HANDLER_INFO) {
    json_decref(root);
    return NULL;
  }

  GnostrNip89HandlerInfo *info = g_new0(GnostrNip89HandlerInfo, 1);

  /* Extract basic fields */
  json_t *id_j = json_object_get(root, "id");
  json_t *pubkey_j = json_object_get(root, "pubkey");
  json_t *created_at_j = json_object_get(root, "created_at");
  json_t *content_j = json_object_get(root, "content");

  if (json_is_string(id_j))
    info->event_id_hex = g_strdup(json_string_value(id_j));
  if (json_is_string(pubkey_j))
    info->pubkey_hex = g_strdup(json_string_value(pubkey_j));
  if (json_is_integer(created_at_j))
    info->created_at = json_integer_value(created_at_j);

  info->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse content as profile-like JSON */
  if (json_is_string(content_j)) {
    const char *content_str = json_string_value(content_j);
    json_t *content_obj = json_loads(content_str, 0, NULL);
    if (content_obj) {
      json_t *v;
      if ((v = json_object_get(content_obj, "name")) && json_is_string(v))
        info->name = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "display_name")) && json_is_string(v))
        info->display_name = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "picture")) && json_is_string(v))
        info->picture = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "about")) && json_is_string(v))
        info->about = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "banner")) && json_is_string(v))
        info->banner = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "website")) && json_is_string(v))
        info->website = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "nip05")) && json_is_string(v))
        info->nip05 = g_strdup(json_string_value(v));
      if ((v = json_object_get(content_obj, "lud16")) && json_is_string(v))
        info->lud16 = g_strdup(json_string_value(v));

      json_decref(content_obj);
    }
  }

  /* Parse tags */
  json_t *tags_j = json_object_get(root, "tags");
  if (json_is_array(tags_j)) {
    info->platforms = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip89_platform_handler_free);
    GArray *kinds_array = g_array_new(FALSE, FALSE, sizeof(guint));

    size_t tag_count = json_array_size(tags_j);
    for (size_t i = 0; i < tag_count; i++) {
      json_t *tag = json_array_get(tags_j, i);
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "d") == 0) {
        g_free(info->d_tag);
        info->d_tag = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "k") == 0) {
        /* Handled event kind */
        guint kind_num = (guint)g_ascii_strtoull(tag_value, NULL, 10);
        g_array_append_val(kinds_array, kind_num);
      }
      else if (strcmp(tag_name, "web") == 0 ||
               strcmp(tag_name, "ios") == 0 ||
               strcmp(tag_name, "android") == 0 ||
               strcmp(tag_name, "macos") == 0 ||
               strcmp(tag_name, "windows") == 0 ||
               strcmp(tag_name, "linux") == 0) {
        /* Platform-specific handler */
        GnostrNip89PlatformHandler *ph = g_new0(GnostrNip89PlatformHandler, 1);
        ph->platform = gnostr_nip89_parse_platform(tag_name);
        ph->platform_name = g_strdup(tag_name);
        ph->url_template = g_strdup(tag_value);

        /* Third element might be app store identifier */
        if (json_array_size(tag) >= 3) {
          const char *third = json_string_value(json_array_get(tag, 2));
          if (third) ph->identifier = g_strdup(third);
        }

        g_ptr_array_add(info->platforms, ph);
      }
    }

    /* Transfer kinds array */
    if (kinds_array->len > 0) {
      info->n_handled_kinds = kinds_array->len;
      info->handled_kinds = (guint *)g_array_free(kinds_array, FALSE);
    } else {
      g_array_free(kinds_array, TRUE);
    }
  }

  json_decref(root);

  /* Validate: must have d_tag and pubkey */
  if (!info->d_tag || !info->pubkey_hex) {
    gnostr_nip89_handler_info_free(info);
    return NULL;
  }

  g_debug("nip89: parsed handler info: %s (%s) - handles %zu kinds",
          info->name ? info->name : info->d_tag,
          info->pubkey_hex,
          info->n_handled_kinds);

  return info;
}

/* ============== Parsing: Recommendation (kind 31990) ============== */

GnostrNip89Recommendation *gnostr_nip89_parse_recommendation(const char *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("nip89: failed to parse recommendation JSON: %s", err.text);
    return NULL;
  }

  /* Validate kind */
  json_t *kind_j = json_object_get(root, "kind");
  if (!json_is_integer(kind_j) || json_integer_value(kind_j) != GNOSTR_NIP89_KIND_HANDLER_RECOMMEND) {
    json_decref(root);
    return NULL;
  }

  GnostrNip89Recommendation *rec = g_new0(GnostrNip89Recommendation, 1);

  /* Extract basic fields */
  json_t *id_j = json_object_get(root, "id");
  json_t *pubkey_j = json_object_get(root, "pubkey");
  json_t *created_at_j = json_object_get(root, "created_at");

  if (json_is_string(id_j))
    rec->event_id_hex = g_strdup(json_string_value(id_j));
  if (json_is_string(pubkey_j))
    rec->pubkey_hex = g_strdup(json_string_value(pubkey_j));
  if (json_is_integer(created_at_j))
    rec->created_at = json_integer_value(created_at_j);

  rec->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags */
  json_t *tags_j = json_object_get(root, "tags");
  if (json_is_array(tags_j)) {
    size_t tag_count = json_array_size(tags_j);
    for (size_t i = 0; i < tag_count; i++) {
      json_t *tag = json_array_get(tags_j, i);
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "d") == 0) {
        g_free(rec->d_tag);
        rec->d_tag = g_strdup(tag_value);
        rec->recommended_kind = (guint)g_ascii_strtoull(tag_value, NULL, 10);
      }
      else if (strcmp(tag_name, "a") == 0) {
        /* Handler reference: "31989:pubkey:d-tag" */
        g_free(rec->handler_a_tag);
        rec->handler_a_tag = g_strdup(tag_value);

        /* Parse the a-tag components */
        gchar **parts = g_strsplit(tag_value, ":", 3);
        if (parts && parts[0] && parts[1] && parts[2]) {
          g_free(rec->handler_pubkey);
          g_free(rec->handler_d_tag);
          rec->handler_pubkey = g_strdup(parts[1]);
          rec->handler_d_tag = g_strdup(parts[2]);
        }
        g_strfreev(parts);

        /* Optional relay hint in third position */
        if (json_array_size(tag) >= 3) {
          const char *relay = json_string_value(json_array_get(tag, 2));
          if (relay && (g_str_has_prefix(relay, "wss://") || g_str_has_prefix(relay, "ws://"))) {
            g_free(rec->relay_hint);
            rec->relay_hint = g_strdup(relay);
          }
        }
      }
    }
  }

  json_decref(root);

  /* Validate: must have d_tag and pubkey */
  if (!rec->d_tag || !rec->pubkey_hex) {
    gnostr_nip89_recommendation_free(rec);
    return NULL;
  }

  g_debug("nip89: parsed recommendation: kind %u by %s -> %s",
          rec->recommended_kind,
          rec->pubkey_hex,
          rec->handler_a_tag ? rec->handler_a_tag : "(none)");

  return rec;
}

/* ============== URL Generation ============== */

char *gnostr_nip89_build_handler_url(GnostrNip89HandlerInfo *handler,
                                      GnostrNip89Platform platform,
                                      const char *event_bech32)
{
  if (!handler || !handler->platforms || !event_bech32) return NULL;

  /* Find matching platform handler */
  for (guint i = 0; i < handler->platforms->len; i++) {
    GnostrNip89PlatformHandler *ph = g_ptr_array_index(handler->platforms, i);
    if (ph->platform == platform && ph->url_template) {
      /* Replace <bech32> placeholder */
      GString *url = g_string_new(NULL);
      const char *p = ph->url_template;
      const char *placeholder;

      while ((placeholder = strstr(p, "<bech32>")) != NULL) {
        g_string_append_len(url, p, placeholder - p);
        g_string_append(url, event_bech32);
        p = placeholder + 8; /* strlen("<bech32>") */
      }
      g_string_append(url, p);

      return g_string_free(url, FALSE);
    }
  }

  /* Fallback: try web platform */
  if (platform != GNOSTR_NIP89_PLATFORM_WEB) {
    return gnostr_nip89_build_handler_url(handler, GNOSTR_NIP89_PLATFORM_WEB, event_bech32);
  }

  return NULL;
}

/* ============== Cache Management ============== */

static void ensure_cache_initialized(void)
{
  if (g_nip89_initialized) return;

  g_mutex_init(&g_nip89_cache_mutex);

  g_handler_cache = g_hash_table_new_full(
    g_str_hash, g_str_equal,
    g_free,
    (GDestroyNotify)gnostr_nip89_handler_info_free
  );

  g_recommendation_cache = g_hash_table_new_full(
    g_str_hash, g_str_equal,
    g_free,
    (GDestroyNotify)gnostr_nip89_recommendation_free
  );

  g_kind_to_handlers = g_hash_table_new_full(
    g_direct_hash, g_direct_equal,
    NULL,
    (GDestroyNotify)g_ptr_array_unref
  );

  g_user_preferences = g_hash_table_new_full(
    g_direct_hash, g_direct_equal,
    NULL,
    g_free
  );

  g_nip89_initialized = TRUE;
  g_debug("nip89: cache initialized");
}

void gnostr_nip89_cache_init(void)
{
  ensure_cache_initialized();
}

void gnostr_nip89_cache_shutdown(void)
{
  if (!g_nip89_initialized) return;

  g_mutex_lock(&g_nip89_cache_mutex);

  g_clear_pointer(&g_handler_cache, g_hash_table_destroy);
  g_clear_pointer(&g_recommendation_cache, g_hash_table_destroy);
  g_clear_pointer(&g_kind_to_handlers, g_hash_table_destroy);
  g_clear_pointer(&g_user_preferences, g_hash_table_destroy);

  g_mutex_unlock(&g_nip89_cache_mutex);
  g_mutex_clear(&g_nip89_cache_mutex);

  g_nip89_initialized = FALSE;
  g_debug("nip89: cache shutdown");
}

static char *make_handler_key(const char *pubkey, const char *d_tag)
{
  return g_strdup_printf("%s:%s", pubkey, d_tag);
}

void gnostr_nip89_cache_add_handler(GnostrNip89HandlerInfo *info)
{
  if (!info || !info->pubkey_hex || !info->d_tag) {
    gnostr_nip89_handler_info_free(info);
    return;
  }

  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  char *key = make_handler_key(info->pubkey_hex, info->d_tag);

  /* Check if we already have a newer version */
  GnostrNip89HandlerInfo *existing = g_hash_table_lookup(g_handler_cache, key);
  if (existing && existing->created_at >= info->created_at) {
    g_free(key);
    gnostr_nip89_handler_info_free(info);
    g_mutex_unlock(&g_nip89_cache_mutex);
    return;
  }

  /* Remove old kind mappings if updating */
  if (existing) {
    for (gsize i = 0; i < existing->n_handled_kinds; i++) {
      guint kind = existing->handled_kinds[i];
      GPtrArray *handlers = g_hash_table_lookup(g_kind_to_handlers, GUINT_TO_POINTER(kind));
      if (handlers) {
        g_ptr_array_remove(handlers, key);
      }
    }
  }

  /* Add to cache */
  g_hash_table_insert(g_handler_cache, key, info);

  /* Update kind-to-handler mappings */
  for (gsize i = 0; i < info->n_handled_kinds; i++) {
    guint kind = info->handled_kinds[i];
    GPtrArray *handlers = g_hash_table_lookup(g_kind_to_handlers, GUINT_TO_POINTER(kind));
    if (!handlers) {
      handlers = g_ptr_array_new_with_free_func(g_free);
      g_hash_table_insert(g_kind_to_handlers, GUINT_TO_POINTER(kind), handlers);
    }
    g_ptr_array_add(handlers, g_strdup(key));
  }

  g_debug("nip89: cached handler %s (%zu kinds)", key, info->n_handled_kinds);
  g_mutex_unlock(&g_nip89_cache_mutex);
}

void gnostr_nip89_cache_add_recommendation(GnostrNip89Recommendation *rec)
{
  if (!rec || !rec->pubkey_hex || !rec->d_tag) {
    gnostr_nip89_recommendation_free(rec);
    return;
  }

  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  char *key = g_strdup_printf("%s:%s", rec->pubkey_hex, rec->d_tag);

  /* Check if we already have a newer version */
  GnostrNip89Recommendation *existing = g_hash_table_lookup(g_recommendation_cache, key);
  if (existing && existing->created_at >= rec->created_at) {
    g_free(key);
    gnostr_nip89_recommendation_free(rec);
    g_mutex_unlock(&g_nip89_cache_mutex);
    return;
  }

  g_hash_table_insert(g_recommendation_cache, key, rec);

  g_debug("nip89: cached recommendation %s", key);
  g_mutex_unlock(&g_nip89_cache_mutex);
}

GPtrArray *gnostr_nip89_cache_get_handlers_for_kind(guint event_kind)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  GPtrArray *result = g_ptr_array_new();
  GPtrArray *handler_keys = g_hash_table_lookup(g_kind_to_handlers, GUINT_TO_POINTER(event_kind));

  if (handler_keys) {
    for (guint i = 0; i < handler_keys->len; i++) {
      const char *key = g_ptr_array_index(handler_keys, i);
      GnostrNip89HandlerInfo *info = g_hash_table_lookup(g_handler_cache, key);
      if (info) {
        g_ptr_array_add(result, info);
      }
    }
  }

  g_mutex_unlock(&g_nip89_cache_mutex);
  return result;
}

GPtrArray *gnostr_nip89_cache_get_recommendations_for_kind(guint event_kind,
                                                            const char *user_pubkey)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  GPtrArray *result = g_ptr_array_new();

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, g_recommendation_cache);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrNip89Recommendation *rec = value;
    if (rec->recommended_kind == event_kind) {
      if (!user_pubkey || g_strcmp0(rec->pubkey_hex, user_pubkey) == 0) {
        g_ptr_array_add(result, rec);
      }
    }
  }

  g_mutex_unlock(&g_nip89_cache_mutex);
  return result;
}

GnostrNip89HandlerInfo *gnostr_nip89_cache_get_handler_by_a_tag(const char *a_tag)
{
  if (!a_tag) return NULL;

  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  /* Parse a_tag: "31989:pubkey:d-tag" */
  gchar **parts = g_strsplit(a_tag, ":", 3);
  GnostrNip89HandlerInfo *result = NULL;

  if (parts && parts[0] && parts[1] && parts[2]) {
    char *key = make_handler_key(parts[1], parts[2]);
    result = g_hash_table_lookup(g_handler_cache, key);
    g_free(key);
  }

  g_strfreev(parts);
  g_mutex_unlock(&g_nip89_cache_mutex);
  return result;
}

GPtrArray *gnostr_nip89_cache_get_all_handlers(void)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  GPtrArray *result = g_ptr_array_new();

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, g_handler_cache);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_ptr_array_add(result, value);
  }

  g_mutex_unlock(&g_nip89_cache_mutex);
  return result;
}

/* ============== User Preferences ============== */

GnostrNip89HandlerInfo *gnostr_nip89_get_preferred_handler(guint event_kind)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  char *a_tag = g_hash_table_lookup(g_user_preferences, GUINT_TO_POINTER(event_kind));
  GnostrNip89HandlerInfo *result = NULL;

  if (a_tag) {
    result = gnostr_nip89_cache_get_handler_by_a_tag(a_tag);
  }

  g_mutex_unlock(&g_nip89_cache_mutex);
  return result;
}

void gnostr_nip89_set_preferred_handler(guint event_kind, const char *handler_a_tag)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  if (handler_a_tag) {
    g_hash_table_insert(g_user_preferences,
                        GUINT_TO_POINTER(event_kind),
                        g_strdup(handler_a_tag));
    g_debug("nip89: set preferred handler for kind %u: %s", event_kind, handler_a_tag);
  } else {
    g_hash_table_remove(g_user_preferences, GUINT_TO_POINTER(event_kind));
    g_debug("nip89: cleared preferred handler for kind %u", event_kind);
  }

  g_mutex_unlock(&g_nip89_cache_mutex);

  /* TODO: Persist to GSettings or publish kind 31990 event */
}

void gnostr_nip89_clear_all_preferences(void)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  g_hash_table_remove_all(g_user_preferences);
  g_debug("nip89: cleared all handler preferences");

  g_mutex_unlock(&g_nip89_cache_mutex);
}

/* ============== Filter Building ============== */

char *gnostr_nip89_build_handler_filter(const guint *kinds, gsize n_kinds)
{
  json_t *filter = json_object();
  json_t *kinds_arr = json_array();

  json_array_append_new(kinds_arr, json_integer(GNOSTR_NIP89_KIND_HANDLER_INFO));
  json_object_set_new(filter, "kinds", kinds_arr);

  /* Add limit */
  json_object_set_new(filter, "limit", json_integer(100));

  /* If specific kinds are requested, add them as #k tag filter */
  if (kinds && n_kinds > 0) {
    json_t *k_arr = json_array();
    for (gsize i = 0; i < n_kinds; i++) {
      char kind_str[16];
      snprintf(kind_str, sizeof(kind_str), "%u", kinds[i]);
      json_array_append_new(k_arr, json_string(kind_str));
    }
    json_object_set_new(filter, "#k", k_arr);
  }

  char *result = json_dumps(filter, JSON_COMPACT);
  json_decref(filter);
  return result;
}

char *gnostr_nip89_build_recommendation_filter(guint event_kind,
                                                const char **followed_pubkeys,
                                                gsize n_pubkeys)
{
  json_t *filter = json_object();
  json_t *kinds_arr = json_array();

  json_array_append_new(kinds_arr, json_integer(GNOSTR_NIP89_KIND_HANDLER_RECOMMEND));
  json_object_set_new(filter, "kinds", kinds_arr);

  /* Filter by d-tag (the event kind being recommended) */
  json_t *d_arr = json_array();
  char kind_str[16];
  snprintf(kind_str, sizeof(kind_str), "%u", event_kind);
  json_array_append_new(d_arr, json_string(kind_str));
  json_object_set_new(filter, "#d", d_arr);

  /* Optionally filter by authors (followed users) */
  if (followed_pubkeys && n_pubkeys > 0) {
    json_t *authors = json_array();
    for (gsize i = 0; i < n_pubkeys; i++) {
      json_array_append_new(authors, json_string(followed_pubkeys[i]));
    }
    json_object_set_new(filter, "authors", authors);
  }

  json_object_set_new(filter, "limit", json_integer(50));

  char *result = json_dumps(filter, JSON_COMPACT);
  json_decref(filter);
  return result;
}

/* ============== Kind Description Helpers ============== */

const char *gnostr_nip89_get_kind_description(guint kind)
{
  switch (kind) {
    case 0:     return "Profile Metadata";
    case 1:     return "Short Text Note";
    case 2:     return "Relay Recommendation (deprecated)";
    case 3:     return "Contact List";
    case 4:     return "Encrypted Direct Message";
    case 5:     return "Event Deletion";
    case 6:     return "Repost";
    case 7:     return "Reaction";
    case 8:     return "Badge Award";
    case 16:    return "Generic Repost";
    case 40:    return "Channel Create";
    case 41:    return "Channel Metadata";
    case 42:    return "Channel Message";
    case 43:    return "Channel Hide Message";
    case 44:    return "Channel Mute User";
    case 1063:  return "File Metadata";
    case 1311:  return "Live Chat Message";
    case 1984:  return "Report";
    case 1985:  return "Label";
    case 4550:  return "Community Post Approval";
    case 9734:  return "Zap Request";
    case 9735:  return "Zap Receipt";
    case 10000: return "Mute List";
    case 10001: return "Pin List";
    case 10002: return "Relay List";
    case 10003: return "Bookmark List";
    case 10004: return "Communities List";
    case 10005: return "Public Chats List";
    case 10006: return "Blocked Relays List";
    case 10007: return "Search Relays List";
    case 10015: return "Interests List";
    case 10030: return "User Emoji List";
    case 13194: return "Wallet Connect Info";
    case 22242: return "Client Authentication";
    case 23194: return "Wallet Connect Request";
    case 23195: return "Wallet Connect Response";
    case 24133: return "NIP-46 Request";
    case 27235: return "HTTP Auth";
    case 30000: return "Follow Sets";
    case 30001: return "Generic Lists";
    case 30002: return "Relay Sets";
    case 30003: return "Bookmark Sets";
    case 30004: return "Curation Sets";
    case 30008: return "Profile Badges";
    case 30009: return "Badge Definition";
    case 30017: return "Stall";
    case 30018: return "Product";
    case 30023: return "Long-form Content";
    case 30024: return "Draft Long-form";
    case 30078: return "Application Data";
    case 30311: return "Live Event";
    case 30315: return "User Status";
    case 30402: return "Classified Listing";
    case 30403: return "Draft Classified";
    case 31922: return "Date Event";
    case 31923: return "Time Event";
    case 31924: return "Calendar";
    case 31925: return "Calendar RSVP";
    case 31989: return "App Handler Info";
    case 31990: return "App Recommendation";
    case 34235: return "Video (Horizontal)";
    case 34236: return "Video (Vertical)";
    case 34550: return "Community Definition";
    default:
      if (kind >= 10000 && kind < 20000) return "Replaceable Event";
      if (kind >= 20000 && kind < 30000) return "Ephemeral Event";
      if (kind >= 30000 && kind < 40000) return "Addressable Event";
      return "Unknown Event Kind";
  }
}

gboolean gnostr_nip89_is_replaceable_kind(guint kind)
{
  return (kind >= 10000 && kind < 20000) || kind == 0 || kind == 3;
}

gboolean gnostr_nip89_is_ephemeral_kind(guint kind)
{
  return kind >= 20000 && kind < 30000;
}

gboolean gnostr_nip89_is_addressable_kind(guint kind)
{
  return kind >= 30000 && kind < 40000;
}

/* ============== Async Query (placeholder - uses local cache for now) ============== */

void gnostr_nip89_query_handlers_async(guint event_kind,
                                        GnostrNip89QueryCallback callback,
                                        gpointer user_data,
                                        GCancellable *cancellable)
{
  if (!callback) return;

  (void)cancellable;

  /* For now, just return cached results.
   * TODO: Implement actual relay queries using NostrSimplePool */

  GPtrArray *handlers = gnostr_nip89_cache_get_handlers_for_kind(event_kind);
  GPtrArray *recommendations = gnostr_nip89_cache_get_recommendations_for_kind(event_kind, NULL);

  /* Schedule callback on main thread */
  typedef struct {
    GnostrNip89QueryCallback callback;
    gpointer user_data;
    GPtrArray *handlers;
    GPtrArray *recommendations;
  } CallbackData;

  CallbackData *data = g_new0(CallbackData, 1);
  data->callback = callback;
  data->user_data = user_data;
  data->handlers = handlers;
  data->recommendations = recommendations;

  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, (GSourceFunc)({
    gboolean inner(gpointer d) {
      CallbackData *cd = d;
      cd->callback(cd->handlers, cd->recommendations, NULL, cd->user_data);
      g_free(cd);
      return G_SOURCE_REMOVE;
    }
    inner;
  }), data, NULL);
}
