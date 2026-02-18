/**
 * NIP-89 App Handlers Implementation
 *
 * Provides parsing, caching, and querying for NIP-89 app handler events.
 */

#include "nip89_handlers.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <string.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-filter.h"
#include <nostr-gobject-1.0/nostr_pool.h>

/* ============== Cache Configuration ============== */

#define NIP89_CACHE_MAX_HANDLERS      500
#define NIP89_CACHE_MAX_RECOMMENDATIONS 1000
#define NIP89_CACHE_TTL_SECONDS       (60 * 60 * 24)  /* 24 hours */

/* GSettings schema for persisting preferences */
#define NIP89_SETTINGS_SCHEMA "org.gnostr.Client"
#define NIP89_SETTINGS_KEY_PREFERENCES "nip89-handler-preferences"

/* Forward declarations for preference persistence */
static void load_preferences_from_settings(void);
static void save_preferences_to_settings(void);

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

  /* Parse with NostrEvent API */
  NostrEvent event = {0};
  if (!nostr_event_deserialize_compact(&event, event_json, NULL)) {
    g_debug("nip89: failed to parse handler info JSON");
    return NULL;
  }

  /* Validate kind */
  if (nostr_event_get_kind(&event) != GNOSTR_NIP89_KIND_HANDLER_INFO) {
    nostr_event_free(&event);
    return NULL;
  }

  GnostrNip89HandlerInfo *info = g_new0(GnostrNip89HandlerInfo, 1);

  /* Extract basic fields */
  char *id_str = nostr_event_get_id(&event);
  const char *pubkey_str = nostr_event_get_pubkey(&event);

  if (id_str) {
    info->event_id_hex = id_str;  /* takes ownership */
  }
  if (pubkey_str)
    info->pubkey_hex = g_strdup(pubkey_str);
  info->created_at = nostr_event_get_created_at(&event);
  info->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse content as profile-like JSON */
  const char *content_str = nostr_event_get_content(&event);
  if (content_str && *content_str) {
    char *val = NULL;
    val = gnostr_json_get_string(content_str, "name", NULL);
    if (val) {
      info->name = val;
    }
    val = gnostr_json_get_string(content_str, "display_name", NULL);
    if (val) {
      info->display_name = val;
    }
    val = gnostr_json_get_string(content_str, "picture", NULL);
    if (val) {
      info->picture = val;
    }
    val = gnostr_json_get_string(content_str, "about", NULL);
    if (val) {
      info->about = val;
    }
    val = gnostr_json_get_string(content_str, "banner", NULL);
    if (val) {
      info->banner = val;
    }
    val = gnostr_json_get_string(content_str, "website", NULL);
    if (val) {
      info->website = val;
    }
    val = gnostr_json_get_string(content_str, "nip05", NULL);
    if (val) {
      info->nip05 = val;
    }
    val = gnostr_json_get_string(content_str, "lud16", NULL);
    if (val) {
      info->lud16 = val;
    }
  }

  /* Parse tags using NostrTags API */
  NostrTags *tags = nostr_event_get_tags(&event);
  if (tags) {
    info->platforms = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip89_platform_handler_free);
    GArray *kinds_array = g_array_new(FALSE, FALSE, sizeof(guint));

    size_t tag_count = nostr_tags_size(tags);
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag) continue;

      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
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
        if (tag_len >= 3) {
          const char *third = nostr_tag_get(tag, 2);
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

  /* Parse with NostrEvent API */
  NostrEvent event = {0};
  if (!nostr_event_deserialize_compact(&event, event_json, NULL)) {
    g_debug("nip89: failed to parse recommendation JSON");
    return NULL;
  }

  /* Validate kind */
  if (nostr_event_get_kind(&event) != GNOSTR_NIP89_KIND_HANDLER_RECOMMEND) {
    return NULL;
  }

  GnostrNip89Recommendation *rec = g_new0(GnostrNip89Recommendation, 1);

  /* Extract basic fields */
  char *id_str = nostr_event_get_id(&event);
  const char *pubkey_str = nostr_event_get_pubkey(&event);

  if (id_str) {
    rec->event_id_hex = id_str;  /* takes ownership */
  }
  if (pubkey_str)
    rec->pubkey_hex = g_strdup(pubkey_str);
  rec->created_at = nostr_event_get_created_at(&event);
  rec->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags using NostrTags API */
  NostrTags *tags = nostr_event_get_tags(&event);
  if (tags) {
    size_t tag_count = nostr_tags_size(tags);
    for (size_t i = 0; i < tag_count; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (!tag) continue;

      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
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
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && (g_str_has_prefix(relay, "wss://") || g_str_has_prefix(relay, "ws://"))) {
            g_free(rec->relay_hint);
            rec->relay_hint = g_strdup(relay);
          }
        }
      }
    }
  }

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

  /* Load persisted preferences from GSettings */
  load_preferences_from_settings();
}

/* Load preferences from GSettings into the hash table */
static void load_preferences_from_settings(void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return;

  GSettingsSchema *schema = g_settings_schema_source_lookup(source, NIP89_SETTINGS_SCHEMA, TRUE);
  if (!schema) {
    g_debug("nip89: settings schema %s not found, skipping preference load", NIP89_SETTINGS_SCHEMA);
    return;
  }
  g_settings_schema_unref(schema);

  g_autoptr(GSettings) settings = g_settings_new(NIP89_SETTINGS_SCHEMA);
  if (!settings) return;

  gchar **prefs = g_settings_get_strv(settings, NIP89_SETTINGS_KEY_PREFERENCES);
  if (prefs) {
    for (gsize i = 0; prefs[i]; i++) {
      /* Parse "kind:a_tag" format */
      gchar *sep = strchr(prefs[i], ':');
      if (sep && sep != prefs[i]) {
        guint kind = (guint)g_ascii_strtoull(prefs[i], NULL, 10);
        const gchar *a_tag = sep + 1;
        if (kind > 0 && *a_tag) {
          g_hash_table_insert(g_user_preferences,
                              GUINT_TO_POINTER(kind),
                              g_strdup(a_tag));
          g_debug("nip89: loaded preference for kind %u: %s", kind, a_tag);
        }
      }
    }
    g_strfreev(prefs);
  }

}

/* Save preferences from hash table to GSettings */
static void save_preferences_to_settings(void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return;

  GSettingsSchema *schema = g_settings_schema_source_lookup(source, NIP89_SETTINGS_SCHEMA, TRUE);
  if (!schema) {
    g_debug("nip89: settings schema %s not found, skipping preference save", NIP89_SETTINGS_SCHEMA);
    return;
  }
  g_settings_schema_unref(schema);

  g_autoptr(GSettings) settings = g_settings_new(NIP89_SETTINGS_SCHEMA);
  if (!settings) return;

  /* Build string array from hash table */
  guint n_prefs = g_hash_table_size(g_user_preferences);
  gchar **prefs = g_new0(gchar*, n_prefs + 1);

  GHashTableIter iter;
  gpointer key, value;
  guint i = 0;

  g_hash_table_iter_init(&iter, g_user_preferences);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    guint kind = GPOINTER_TO_UINT(key);
    const gchar *a_tag = (const gchar*)value;
    prefs[i++] = g_strdup_printf("%u:%s", kind, a_tag);
  }

  g_settings_set_strv(settings, NIP89_SETTINGS_KEY_PREFERENCES, (const gchar* const*)prefs);
  g_debug("nip89: saved %u preferences to settings", n_prefs);

  g_strfreev(prefs);
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

  /* Persist to GSettings */
  save_preferences_to_settings();

  g_mutex_unlock(&g_nip89_cache_mutex);
}

void gnostr_nip89_clear_all_preferences(void)
{
  ensure_cache_initialized();
  g_mutex_lock(&g_nip89_cache_mutex);

  g_hash_table_remove_all(g_user_preferences);
  g_debug("nip89: cleared all handler preferences");

  /* Persist to GSettings */
  save_preferences_to_settings();

  g_mutex_unlock(&g_nip89_cache_mutex);
}

/* ============== Filter Building ============== */

char *gnostr_nip89_build_handler_filter(const guint *kinds, gsize n_kinds)
{
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* kinds: [31989] */
  gnostr_json_builder_set_key(builder, "kinds");
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_int(builder, GNOSTR_NIP89_KIND_HANDLER_INFO);
  gnostr_json_builder_end_array(builder);

  /* limit: 100 */
  gnostr_json_builder_set_key(builder, "limit");
  gnostr_json_builder_add_int(builder, 100);

  /* If specific kinds are requested, add them as #k tag filter */
  if (kinds && n_kinds > 0) {
    gnostr_json_builder_set_key(builder, "#k");
    gnostr_json_builder_begin_array(builder);
    for (gsize i = 0; i < n_kinds; i++) {
      char kind_str[16];
      snprintf(kind_str, sizeof(kind_str), "%u", kinds[i]);
      gnostr_json_builder_add_string(builder, kind_str);
    }
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_object(builder);
  char *result = gnostr_json_builder_finish(builder);
  return result;
}

char *gnostr_nip89_build_recommendation_filter(guint event_kind,
                                                const char **followed_pubkeys,
                                                gsize n_pubkeys)
{
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* kinds: [31990] */
  gnostr_json_builder_set_key(builder, "kinds");
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_int(builder, GNOSTR_NIP89_KIND_HANDLER_RECOMMEND);
  gnostr_json_builder_end_array(builder);

  /* Filter by d-tag (the event kind being recommended) */
  gnostr_json_builder_set_key(builder, "#d");
  gnostr_json_builder_begin_array(builder);
  char kind_str[16];
  snprintf(kind_str, sizeof(kind_str), "%u", event_kind);
  gnostr_json_builder_add_string(builder, kind_str);
  gnostr_json_builder_end_array(builder);

  /* Optionally filter by authors (followed users) */
  if (followed_pubkeys && n_pubkeys > 0) {
    gnostr_json_builder_set_key(builder, "authors");
    gnostr_json_builder_begin_array(builder);
    for (gsize i = 0; i < n_pubkeys; i++) {
      gnostr_json_builder_add_string(builder, followed_pubkeys[i]);
    }
    gnostr_json_builder_end_array(builder);
  }

  /* limit: 50 */
  gnostr_json_builder_set_key(builder, "limit");
  gnostr_json_builder_add_int(builder, 50);

  gnostr_json_builder_end_object(builder);
  char *result = gnostr_json_builder_finish(builder);
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

/* ============== Async Query with Relay Support ============== */

/* NIP-89 event kinds */
#define NIP89_KIND_HANDLER_INFO    31989
#define NIP89_KIND_RECOMMENDATION  31990

/* Static pool for NIP-89 queries */
static GNostrPool *s_nip89_pool = NULL;

/* Context for relay query */
typedef struct {
  GnostrNip89QueryCallback callback;
  gpointer user_data;
  guint target_kind;
  GCancellable *cancellable;
  GPtrArray *handlers;       /* Accumulated handlers */
  GPtrArray *recommendations; /* Accumulated recommendations */
  gint pending_queries;      /* Number of pending relay queries */
} Nip89QueryContext;

static void nip89_query_context_free(Nip89QueryContext *ctx) {
  if (!ctx) return;
  if (ctx->handlers) g_ptr_array_unref(ctx->handlers);
  if (ctx->recommendations) g_ptr_array_unref(ctx->recommendations);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

/* Callback for idle dispatch to main thread */
typedef struct {
  GnostrNip89QueryCallback callback;
  gpointer user_data;
  GPtrArray *handlers;
  GPtrArray *recommendations;
} Nip89QueryCallbackData;

static gboolean
on_nip89_query_idle(gpointer data)
{
  Nip89QueryCallbackData *cd = data;
  cd->callback(cd->handlers, cd->recommendations, NULL, cd->user_data);
  g_free(cd);
  return G_SOURCE_REMOVE;
}

/* Callback when relay query completes */
static void on_nip89_relay_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip89QueryContext *ctx = (Nip89QueryContext *)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(
      GNOSTR_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("nip89: relay query error: %s", err->message);
    }
    g_error_free(err);
  } else if (results && results->len > 0) {
    g_debug("nip89: received %u events from relays", results->len);

    /* Parse each event and add to results */
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (!json) continue;

      /* Try parsing as handler info (kind 31989) */
      GnostrNip89HandlerInfo *handler = gnostr_nip89_parse_handler_info(json);
      if (handler) {
        /* Check if it handles our target kind */
        gboolean handles_target = FALSE;
        if (handler->handled_kinds && handler->n_handled_kinds > 0) {
          for (gsize k = 0; k < handler->n_handled_kinds; k++) {
            if (handler->handled_kinds[k] == ctx->target_kind) {
              handles_target = TRUE;
              break;
            }
          }
        }

        if (handles_target) {
          g_ptr_array_add(ctx->handlers, handler);
          /* Also add to cache */
          gnostr_nip89_cache_add_handler(handler);
        } else {
          gnostr_nip89_handler_info_free(handler);
        }
        continue;
      }

      /* Try parsing as recommendation (kind 31990) */
      GnostrNip89Recommendation *rec = gnostr_nip89_parse_recommendation(json);
      if (rec && rec->recommended_kind == ctx->target_kind) {
        g_ptr_array_add(ctx->recommendations, rec);
        /* Also add to cache */
        gnostr_nip89_cache_add_recommendation(rec);
      } else if (rec) {
        gnostr_nip89_recommendation_free(rec);
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  /* Decrement pending count and check if done */
  ctx->pending_queries--;
  if (ctx->pending_queries <= 0) {
    /* All queries complete - invoke callback */
    Nip89QueryCallbackData *cb_data = g_new0(Nip89QueryCallbackData, 1);
    cb_data->callback = ctx->callback;
    cb_data->user_data = ctx->user_data;
    /* Transfer ownership of arrays to callback */
    cb_data->handlers = ctx->handlers;
    cb_data->recommendations = ctx->recommendations;
    ctx->handlers = NULL;
    ctx->recommendations = NULL;

    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_nip89_query_idle, cb_data, NULL);
    nip89_query_context_free(ctx);
  }
}

void gnostr_nip89_query_handlers_async(guint event_kind,
                                        GnostrNip89QueryCallback callback,
                                        gpointer user_data,
                                        GCancellable *cancellable)
{
  if (!callback) return;

  /* Start with cached results */
  GPtrArray *cached_handlers = gnostr_nip89_cache_get_handlers_for_kind(event_kind);
  GPtrArray *cached_recommendations = gnostr_nip89_cache_get_recommendations_for_kind(event_kind, NULL);

  /* Get relay URLs */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_get_read_relay_urls_into(relay_arr);

  if (relay_arr->len == 0) {
    /* No relays configured - return cached results only */
    g_ptr_array_unref(relay_arr);
    Nip89QueryCallbackData *data = g_new0(Nip89QueryCallbackData, 1);
    data->callback = callback;
    data->user_data = user_data;
    data->handlers = cached_handlers;
    data->recommendations = cached_recommendations;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, on_nip89_query_idle, data, NULL);
    return;
  }

  /* Create query context */
  Nip89QueryContext *ctx = g_new0(Nip89QueryContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->target_kind = event_kind;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  /* Initialize with cached results */
  ctx->handlers = cached_handlers ? cached_handlers : g_ptr_array_new_with_free_func(
      (GDestroyNotify)gnostr_nip89_handler_info_free);
  ctx->recommendations = cached_recommendations ? cached_recommendations : g_ptr_array_new_with_free_func(
      (GDestroyNotify)gnostr_nip89_recommendation_free);

  /* Build filter for kind 31989 (handler info) events with "k" tag matching target kind */
  NostrFilter *filter = nostr_filter_new();
  int kinds[2] = { NIP89_KIND_HANDLER_INFO, NIP89_KIND_RECOMMENDATION };
  nostr_filter_set_kinds(filter, kinds, 2);

  /* Add #k tag filter for the target kind */
  char kind_str[16];
  g_snprintf(kind_str, sizeof(kind_str), "%u", event_kind);
  nostr_filter_tags_append(filter, "k", kind_str, NULL);

  nostr_filter_set_limit(filter, 50);

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Initialize pool */
  if (!s_nip89_pool) {
    s_nip89_pool = gnostr_pool_new();
  }

  g_debug("nip89: querying %u relays for kind %u handlers", relay_arr->len, event_kind);

  ctx->pending_queries = 1;

    gnostr_pool_sync_relays(s_nip89_pool, (const gchar **)urls, relay_arr->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(s_nip89_pool, _qf, ctx->cancellable, on_nip89_relay_query_done, ctx);
  }

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
}
