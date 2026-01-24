/**
 * NIP-66 Relay Discovery and Monitoring Implementation
 *
 * Provides parsing, caching, and querying for NIP-66 relay discovery events.
 */

#include "nip66_relay_discovery.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

#ifndef GNOSTR_NIP66_TEST_ONLY
#include "relays.h"
#include "nostr_simple_pool.h"
#include "nostr-filter.h"
#endif

/* ============== Cache Configuration ============== */

#define NIP66_CACHE_MAX_RELAYS    2000
#define NIP66_CACHE_MAX_MONITORS  100
#define NIP66_CACHE_TTL_SECONDS   (60 * 60 * 6)  /* 6 hours */

/* ============== Global Cache State ============== */

static GHashTable *g_relay_cache = NULL;    /* key: relay_url, value: GnostrNip66RelayMeta* */
static GHashTable *g_monitor_cache = NULL;  /* key: pubkey_hex, value: GnostrNip66RelayMonitor* */
static GMutex g_nip66_cache_mutex;
static gboolean g_nip66_initialized = FALSE;

/* ============== Well-Known Monitors ============== */

/* Well-known relay monitor pubkeys (these are example/placeholder values) */
static const gchar *s_known_monitors[] = {
  /* relay.tools monitor */
  "52b4a076bcbbbdc3a1aefa3735816f4f5a6cfb91e4f2f1af8e5c2e7e9c0e4c5a",
  /* nostr.watch monitor */
  "e1055729d0cf1f1c4bdf8e8b2a44a8b6f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9",
  NULL
};

/* Relays known to host NIP-66 monitor data */
static const gchar *s_known_monitor_relays[] = {
  "wss://relay.damus.io",
  "wss://nos.lol",
  "wss://relay.nostr.band",
  "wss://purplepag.es",
  NULL
};

/* ============== Memory Management ============== */

void gnostr_nip66_relay_meta_free(GnostrNip66RelayMeta *meta)
{
  if (!meta) return;

  g_free(meta->event_id_hex);
  g_free(meta->pubkey_hex);
  g_free(meta->d_tag);
  g_free(meta->relay_url);
  g_free(meta->name);
  g_free(meta->description);
  g_free(meta->pubkey);
  g_free(meta->contact);
  g_free(meta->software);
  g_free(meta->version);
  g_free(meta->icon);
  g_free(meta->supported_nips);
  g_free(meta->country_code);
  g_free(meta->region);
  g_free(meta->city);

  if (meta->language_tags) {
    for (gsize i = 0; i < meta->language_tags_count; i++)
      g_free(meta->language_tags[i]);
    g_free(meta->language_tags);
  }

  if (meta->tags) {
    for (gsize i = 0; i < meta->tags_count; i++)
      g_free(meta->tags[i]);
    g_free(meta->tags);
  }

  g_free(meta);
}

void gnostr_nip66_relay_monitor_free(GnostrNip66RelayMonitor *monitor)
{
  if (!monitor) return;

  g_free(monitor->event_id_hex);
  g_free(monitor->pubkey_hex);
  g_free(monitor->name);
  g_free(monitor->description);
  g_free(monitor->operator_pubkey);
  g_free(monitor->contact);
  g_free(monitor->website);
  g_free(monitor->frequency);

  if (monitor->monitored_regions) {
    for (gsize i = 0; i < monitor->monitored_regions_count; i++)
      g_free(monitor->monitored_regions[i]);
    g_free(monitor->monitored_regions);
  }

  if (monitor->relay_hints) {
    for (gsize i = 0; i < monitor->relay_hints_count; i++)
      g_free(monitor->relay_hints[i]);
    g_free(monitor->relay_hints);
  }

  g_free(monitor);
}

/* ============== Network Type Helpers ============== */

GnostrNip66RelayNetwork gnostr_nip66_parse_network(const gchar *network_str)
{
  if (!network_str) return GNOSTR_NIP66_NETWORK_UNKNOWN;

  if (g_ascii_strcasecmp(network_str, "clearnet") == 0 ||
      g_ascii_strcasecmp(network_str, "internet") == 0)
    return GNOSTR_NIP66_NETWORK_CLEARNET;
  if (g_ascii_strcasecmp(network_str, "tor") == 0 ||
      g_ascii_strcasecmp(network_str, "onion") == 0)
    return GNOSTR_NIP66_NETWORK_TOR;
  if (g_ascii_strcasecmp(network_str, "i2p") == 0)
    return GNOSTR_NIP66_NETWORK_I2P;

  return GNOSTR_NIP66_NETWORK_UNKNOWN;
}

const gchar *gnostr_nip66_network_to_string(GnostrNip66RelayNetwork network)
{
  switch (network) {
    case GNOSTR_NIP66_NETWORK_CLEARNET: return "Clearnet";
    case GNOSTR_NIP66_NETWORK_TOR:      return "Tor";
    case GNOSTR_NIP66_NETWORK_I2P:      return "I2P";
    default:                            return "Unknown";
  }
}

/* ============== JSON Parsing Helpers ============== */

static gchar *json_get_string_or_null(json_t *obj, const gchar *key)
{
  json_t *val = json_object_get(obj, key);
  if (val && json_is_string(val)) {
    return g_strdup(json_string_value(val));
  }
  return NULL;
}

static gint64 json_get_int_or_zero(json_t *obj, const gchar *key)
{
  json_t *val = json_object_get(obj, key);
  if (val && json_is_integer(val)) {
    return json_integer_value(val);
  }
  return 0;
}

static gdouble json_get_double_or_zero(json_t *obj, const gchar *key)
{
  json_t *val = json_object_get(obj, key);
  if (val && json_is_real(val)) {
    return json_real_value(val);
  }
  if (val && json_is_integer(val)) {
    return (gdouble)json_integer_value(val);
  }
  return 0.0;
}

static gboolean json_get_bool_or_false(json_t *obj, const gchar *key)
{
  json_t *val = json_object_get(obj, key);
  if (val) {
    if (json_is_boolean(val)) return json_is_true(val);
    if (json_is_integer(val)) return json_integer_value(val) != 0;
    if (json_is_string(val)) {
      const gchar *s = json_string_value(val);
      return g_ascii_strcasecmp(s, "true") == 0 || g_strcmp0(s, "1") == 0;
    }
  }
  return FALSE;
}

static gint *json_array_to_int_array(json_t *arr, gsize *out_count)
{
  if (!arr || !json_is_array(arr) || !out_count) return NULL;

  gsize len = json_array_size(arr);
  if (len == 0) { *out_count = 0; return NULL; }

  gint *result = g_new(gint, len);
  gsize actual = 0;

  for (gsize i = 0; i < len; i++) {
    json_t *elem = json_array_get(arr, i);
    if (elem && json_is_integer(elem)) {
      result[actual++] = (gint)json_integer_value(elem);
    }
  }

  *out_count = actual;
  return result;
}

static gchar **json_array_to_string_array(json_t *arr, gsize *out_count)
{
  if (!arr || !json_is_array(arr) || !out_count) return NULL;

  gsize len = json_array_size(arr);
  if (len == 0) { *out_count = 0; return NULL; }

  gchar **result = g_new0(gchar*, len + 1);
  gsize actual = 0;

  for (gsize i = 0; i < len; i++) {
    json_t *elem = json_array_get(arr, i);
    if (elem && json_is_string(elem)) {
      result[actual++] = g_strdup(json_string_value(elem));
    }
  }

  *out_count = actual;
  return result;
}

/* ============== Tag Parsing Helpers ============== */

static const gchar *find_tag_value(json_t *tags, const gchar *tag_name, gint value_index)
{
  if (!tags || !json_is_array(tags)) return NULL;

  gsize n = json_array_size(tags);
  for (gsize i = 0; i < n; i++) {
    json_t *tag = json_array_get(tags, i);
    if (!tag || !json_is_array(tag)) continue;

    gsize tag_len = json_array_size(tag);
    if (tag_len < 1) continue;

    json_t *name_elem = json_array_get(tag, 0);
    if (!name_elem || !json_is_string(name_elem)) continue;

    if (g_strcmp0(json_string_value(name_elem), tag_name) == 0) {
      if ((gint)tag_len > value_index) {
        json_t *val = json_array_get(tag, value_index);
        if (val && json_is_string(val)) {
          return json_string_value(val);
        }
      }
      return NULL;
    }
  }
  return NULL;
}

static GPtrArray *find_all_tag_values(json_t *tags, const gchar *tag_name)
{
  GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
  if (!tags || !json_is_array(tags)) return values;

  gsize n = json_array_size(tags);
  for (gsize i = 0; i < n; i++) {
    json_t *tag = json_array_get(tags, i);
    if (!tag || !json_is_array(tag) || json_array_size(tag) < 2) continue;

    json_t *name_elem = json_array_get(tag, 0);
    if (!name_elem || !json_is_string(name_elem)) continue;

    if (g_strcmp0(json_string_value(name_elem), tag_name) == 0) {
      json_t *val = json_array_get(tag, 1);
      if (val && json_is_string(val)) {
        g_ptr_array_add(values, g_strdup(json_string_value(val)));
      }
    }
  }
  return values;
}

/* ============== Parsing: Relay Metadata (kind 30166) ============== */

GnostrNip66RelayMeta *gnostr_nip66_parse_relay_meta(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("nip66: failed to parse relay meta JSON: %s", err.text);
    return NULL;
  }

  /* Validate kind */
  json_t *kind_j = json_object_get(root, "kind");
  if (!json_is_integer(kind_j) || json_integer_value(kind_j) != GNOSTR_NIP66_KIND_RELAY_META) {
    json_decref(root);
    return NULL;
  }

  GnostrNip66RelayMeta *meta = g_new0(GnostrNip66RelayMeta, 1);

  /* Extract basic event fields */
  meta->event_id_hex = json_get_string_or_null(root, "id");
  meta->pubkey_hex = json_get_string_or_null(root, "pubkey");
  meta->created_at = json_get_int_or_zero(root, "created_at");
  meta->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    /* d tag = relay URL */
    const gchar *d_val = find_tag_value(tags, "d", 1);
    if (d_val) {
      meta->d_tag = g_strdup(d_val);
      meta->relay_url = g_strdup(d_val);
    }

    /* r tag = alternative relay URL */
    if (!meta->relay_url) {
      const gchar *r_val = find_tag_value(tags, "r", 1);
      if (r_val) meta->relay_url = g_strdup(r_val);
    }

    /* n tag = network type */
    const gchar *n_val = find_tag_value(tags, "n", 1);
    if (n_val) {
      meta->network = gnostr_nip66_parse_network(n_val);
    } else {
      /* Infer network from URL */
      if (meta->relay_url) {
        if (g_str_has_suffix(meta->relay_url, ".onion") ||
            strstr(meta->relay_url, ".onion/")) {
          meta->network = GNOSTR_NIP66_NETWORK_TOR;
        } else if (g_str_has_suffix(meta->relay_url, ".i2p") ||
                   strstr(meta->relay_url, ".i2p/")) {
          meta->network = GNOSTR_NIP66_NETWORK_I2P;
        } else {
          meta->network = GNOSTR_NIP66_NETWORK_CLEARNET;
        }
      }
    }

    /* N tag = supported NIPs (can be multiple) */
    GPtrArray *nip_values = find_all_tag_values(tags, "N");
    if (nip_values->len > 0) {
      meta->supported_nips = g_new(gint, nip_values->len);
      meta->supported_nips_count = 0;
      for (guint i = 0; i < nip_values->len; i++) {
        const gchar *nip_str = g_ptr_array_index(nip_values, i);
        gint nip_num = atoi(nip_str);
        if (nip_num > 0) {
          meta->supported_nips[meta->supported_nips_count++] = nip_num;
        }
      }
    }
    g_ptr_array_unref(nip_values);

    /* Geographic tags */
    const gchar *geo_val = find_tag_value(tags, "g", 1);  /* geohash or coordinates */
    (void)geo_val; /* TODO: parse geohash if present */

    const gchar *country_val = find_tag_value(tags, "G", 1);  /* Country code */
    if (country_val) {
      meta->country_code = g_strdup(country_val);
      meta->region = g_strdup(gnostr_nip66_get_region_for_country(country_val));
    }

    /* t tags = generic tags */
    GPtrArray *tag_values = find_all_tag_values(tags, "t");
    if (tag_values->len > 0) {
      meta->tags = g_new0(gchar*, tag_values->len + 1);
      meta->tags_count = tag_values->len;
      for (guint i = 0; i < tag_values->len; i++) {
        meta->tags[i] = g_strdup(g_ptr_array_index(tag_values, i));
      }
    }
    g_ptr_array_unref(tag_values);

    /* L/l tags for status */
    const gchar *status_val = find_tag_value(tags, "l", 1);
    if (status_val) {
      meta->is_online = (g_ascii_strcasecmp(status_val, "online") == 0);
    }

    /* rtt tag = round-trip time / latency */
    const gchar *rtt_val = find_tag_value(tags, "rtt", 1);
    if (rtt_val) {
      gchar *subtype = NULL;
      const gchar *rtt_type = find_tag_value(tags, "rtt", 2);
      if (g_strcmp0(rtt_type, "open") == 0) {
        meta->latency_open_ms = g_ascii_strtoll(rtt_val, NULL, 10);
      } else if (g_strcmp0(rtt_type, "read") == 0) {
        meta->latency_read_ms = g_ascii_strtoll(rtt_val, NULL, 10);
      } else if (g_strcmp0(rtt_type, "write") == 0) {
        meta->latency_write_ms = g_ascii_strtoll(rtt_val, NULL, 10);
      } else {
        meta->latency_ms = g_ascii_strtoll(rtt_val, NULL, 10);
      }
    }
  }

  /* Parse content JSON (NIP-11 style info) */
  json_t *content_j = json_object_get(root, "content");
  if (content_j && json_is_string(content_j)) {
    const gchar *content_str = json_string_value(content_j);
    json_t *content_obj = json_loads(content_str, 0, NULL);
    if (content_obj) {
      meta->name = json_get_string_or_null(content_obj, "name");
      meta->description = json_get_string_or_null(content_obj, "description");
      meta->pubkey = json_get_string_or_null(content_obj, "pubkey");
      meta->contact = json_get_string_or_null(content_obj, "contact");
      meta->software = json_get_string_or_null(content_obj, "software");
      meta->version = json_get_string_or_null(content_obj, "version");
      meta->icon = json_get_string_or_null(content_obj, "icon");

      /* supported_nips from content (if not in tags) */
      if (!meta->supported_nips) {
        json_t *nips_arr = json_object_get(content_obj, "supported_nips");
        if (nips_arr) {
          meta->supported_nips = json_array_to_int_array(nips_arr, &meta->supported_nips_count);
        }
      }

      /* limitations object */
      json_t *lim = json_object_get(content_obj, "limitation");
      if (lim && json_is_object(lim)) {
        meta->max_message_length = (gint)json_get_int_or_zero(lim, "max_message_length");
        meta->max_content_length = (gint)json_get_int_or_zero(lim, "max_content_length");
        meta->max_event_tags = (gint)json_get_int_or_zero(lim, "max_event_tags");
        meta->max_subscriptions = (gint)json_get_int_or_zero(lim, "max_subscriptions");
        meta->auth_required = json_get_bool_or_false(lim, "auth_required");
        meta->payment_required = json_get_bool_or_false(lim, "payment_required");
        meta->restricted_writes = json_get_bool_or_false(lim, "restricted_writes");
      }

      /* Monitoring stats from content */
      meta->uptime_percent = json_get_double_or_zero(content_obj, "uptime");
      if (meta->latency_ms == 0) {
        meta->latency_ms = json_get_int_or_zero(content_obj, "latency");
      }
      if (meta->last_seen == 0) {
        meta->last_seen = json_get_int_or_zero(content_obj, "last_seen");
      }
      if (meta->first_seen == 0) {
        meta->first_seen = json_get_int_or_zero(content_obj, "first_seen");
      }

      /* Geographic info from content */
      if (!meta->country_code) {
        meta->country_code = json_get_string_or_null(content_obj, "country_code");
        if (meta->country_code) {
          meta->region = g_strdup(gnostr_nip66_get_region_for_country(meta->country_code));
        }
      }
      if (!meta->city) {
        meta->city = json_get_string_or_null(content_obj, "city");
      }

      json_t *lat_j = json_object_get(content_obj, "latitude");
      json_t *lon_j = json_object_get(content_obj, "longitude");
      if ((lat_j && (json_is_real(lat_j) || json_is_integer(lat_j))) &&
          (lon_j && (json_is_real(lon_j) || json_is_integer(lon_j)))) {
        meta->latitude = json_get_double_or_zero(content_obj, "latitude");
        meta->longitude = json_get_double_or_zero(content_obj, "longitude");
        meta->has_geolocation = TRUE;
      }

      json_decref(content_obj);
    }
  }

  json_decref(root);

  /* Validate: must have relay URL */
  if (!meta->relay_url || !*meta->relay_url) {
    gnostr_nip66_relay_meta_free(meta);
    return NULL;
  }

  return meta;
}

/* ============== Parsing: Relay Monitor (kind 10166) ============== */

GnostrNip66RelayMonitor *gnostr_nip66_parse_relay_monitor(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("nip66: failed to parse relay monitor JSON: %s", err.text);
    return NULL;
  }

  /* Validate kind */
  json_t *kind_j = json_object_get(root, "kind");
  if (!json_is_integer(kind_j) || json_integer_value(kind_j) != GNOSTR_NIP66_KIND_RELAY_MONITOR) {
    json_decref(root);
    return NULL;
  }

  GnostrNip66RelayMonitor *monitor = g_new0(GnostrNip66RelayMonitor, 1);

  /* Extract basic event fields */
  monitor->event_id_hex = json_get_string_or_null(root, "id");
  monitor->pubkey_hex = json_get_string_or_null(root, "pubkey");
  monitor->created_at = json_get_int_or_zero(root, "created_at");
  monitor->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    /* frequency tag */
    const gchar *freq_val = find_tag_value(tags, "frequency", 1);
    if (freq_val) monitor->frequency = g_strdup(freq_val);

    /* relay hints (r tags) */
    GPtrArray *relay_hints = find_all_tag_values(tags, "r");
    if (relay_hints->len > 0) {
      monitor->relay_hints = g_new0(gchar*, relay_hints->len + 1);
      monitor->relay_hints_count = relay_hints->len;
      for (guint i = 0; i < relay_hints->len; i++) {
        monitor->relay_hints[i] = g_strdup(g_ptr_array_index(relay_hints, i));
      }
    }
    g_ptr_array_unref(relay_hints);

    /* region tags */
    GPtrArray *regions = find_all_tag_values(tags, "g");
    if (regions->len > 0) {
      monitor->monitored_regions = g_new0(gchar*, regions->len + 1);
      monitor->monitored_regions_count = regions->len;
      for (guint i = 0; i < regions->len; i++) {
        monitor->monitored_regions[i] = g_strdup(g_ptr_array_index(regions, i));
      }
    }
    g_ptr_array_unref(regions);
  }

  /* Parse content JSON */
  json_t *content_j = json_object_get(root, "content");
  if (content_j && json_is_string(content_j)) {
    const gchar *content_str = json_string_value(content_j);
    json_t *content_obj = json_loads(content_str, 0, NULL);
    if (content_obj) {
      monitor->name = json_get_string_or_null(content_obj, "name");
      monitor->description = json_get_string_or_null(content_obj, "description");
      monitor->operator_pubkey = json_get_string_or_null(content_obj, "pubkey");
      monitor->contact = json_get_string_or_null(content_obj, "contact");
      monitor->website = json_get_string_or_null(content_obj, "website");

      if (!monitor->frequency) {
        monitor->frequency = json_get_string_or_null(content_obj, "frequency");
      }

      json_decref(content_obj);
    }
  }

  json_decref(root);

  return monitor;
}

/* ============== Cache Management ============== */

static void ensure_cache_init(void)
{
  if (G_UNLIKELY(!g_nip66_initialized)) {
    g_mutex_init(&g_nip66_cache_mutex);
    g_relay_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, (GDestroyNotify)gnostr_nip66_relay_meta_free);
    g_monitor_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)gnostr_nip66_relay_monitor_free);
    g_nip66_initialized = TRUE;
  }
}

void gnostr_nip66_cache_init(void)
{
  ensure_cache_init();
}

void gnostr_nip66_cache_shutdown(void)
{
  if (!g_nip66_initialized) return;

  g_mutex_lock(&g_nip66_cache_mutex);
  if (g_relay_cache) {
    g_hash_table_destroy(g_relay_cache);
    g_relay_cache = NULL;
  }
  if (g_monitor_cache) {
    g_hash_table_destroy(g_monitor_cache);
    g_monitor_cache = NULL;
  }
  g_mutex_unlock(&g_nip66_cache_mutex);

  g_nip66_initialized = FALSE;
}

void gnostr_nip66_cache_add_relay(GnostrNip66RelayMeta *meta)
{
  if (!meta || !meta->relay_url) return;
  ensure_cache_init();

  g_mutex_lock(&g_nip66_cache_mutex);

  /* Check cache size limit */
  if (g_hash_table_size(g_relay_cache) >= NIP66_CACHE_MAX_RELAYS) {
    /* Remove oldest entries (simple approach: clear and rebuild) */
    /* In production, would use LRU eviction */
    g_hash_table_remove_all(g_relay_cache);
  }

  /* Normalize URL as key */
  gchar *key = g_ascii_strdown(meta->relay_url, -1);

  /* Take ownership of meta */
  g_hash_table_replace(g_relay_cache, key, meta);

  g_mutex_unlock(&g_nip66_cache_mutex);
}

void gnostr_nip66_cache_add_monitor(GnostrNip66RelayMonitor *monitor)
{
  if (!monitor || !monitor->pubkey_hex) return;
  ensure_cache_init();

  g_mutex_lock(&g_nip66_cache_mutex);

  gchar *key = g_strdup(monitor->pubkey_hex);
  g_hash_table_replace(g_monitor_cache, key, monitor);

  g_mutex_unlock(&g_nip66_cache_mutex);
}

GnostrNip66RelayMeta *gnostr_nip66_cache_get_relay(const gchar *relay_url)
{
  if (!relay_url) return NULL;
  ensure_cache_init();

  g_mutex_lock(&g_nip66_cache_mutex);

  gchar *key = g_ascii_strdown(relay_url, -1);
  GnostrNip66RelayMeta *meta = g_hash_table_lookup(g_relay_cache, key);
  g_free(key);

  /* Check TTL */
  if (meta) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (now - meta->cached_at > NIP66_CACHE_TTL_SECONDS) {
      meta = NULL; /* Expired */
    }
  }

  g_mutex_unlock(&g_nip66_cache_mutex);
  return meta;
}

GPtrArray *gnostr_nip66_cache_get_all_relays(void)
{
  ensure_cache_init();

  GPtrArray *result = g_ptr_array_new();

  g_mutex_lock(&g_nip66_cache_mutex);

  GHashTableIter iter;
  gpointer key, value;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;

  g_hash_table_iter_init(&iter, g_relay_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrNip66RelayMeta *meta = (GnostrNip66RelayMeta*)value;
    /* Skip expired entries */
    if (now - meta->cached_at <= NIP66_CACHE_TTL_SECONDS) {
      g_ptr_array_add(result, meta);
    }
  }

  g_mutex_unlock(&g_nip66_cache_mutex);
  return result;
}

GPtrArray *gnostr_nip66_cache_get_all_monitors(void)
{
  ensure_cache_init();

  GPtrArray *result = g_ptr_array_new();

  g_mutex_lock(&g_nip66_cache_mutex);

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, g_monitor_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_ptr_array_add(result, value);
  }

  g_mutex_unlock(&g_nip66_cache_mutex);
  return result;
}

void gnostr_nip66_cache_clear(void)
{
  ensure_cache_init();

  g_mutex_lock(&g_nip66_cache_mutex);
  g_hash_table_remove_all(g_relay_cache);
  g_hash_table_remove_all(g_monitor_cache);
  g_mutex_unlock(&g_nip66_cache_mutex);
}

/* ============== Query/Filter API ============== */

gboolean gnostr_nip66_relay_supports_nip(const GnostrNip66RelayMeta *meta, gint nip)
{
  if (!meta || !meta->supported_nips) return FALSE;

  for (gsize i = 0; i < meta->supported_nips_count; i++) {
    if (meta->supported_nips[i] == nip) return TRUE;
  }
  return FALSE;
}

GPtrArray *gnostr_nip66_filter_relays(const GnostrNip66RelayFilter *filter)
{
  GPtrArray *all = gnostr_nip66_cache_get_all_relays();
  GPtrArray *result = g_ptr_array_new();

  for (guint i = 0; i < all->len; i++) {
    GnostrNip66RelayMeta *meta = g_ptr_array_index(all, i);
    gboolean matches = TRUE;

    if (filter) {
      /* Check flags */
      if ((filter->flags & GNOSTR_NIP66_FILTER_ONLINE_ONLY) && !meta->is_online) {
        matches = FALSE;
      }
      if ((filter->flags & GNOSTR_NIP66_FILTER_FREE_ONLY) && meta->payment_required) {
        matches = FALSE;
      }
      if ((filter->flags & GNOSTR_NIP66_FILTER_NO_AUTH) && meta->auth_required) {
        matches = FALSE;
      }
      if ((filter->flags & GNOSTR_NIP66_FILTER_CLEARNET_ONLY) &&
          meta->network != GNOSTR_NIP66_NETWORK_CLEARNET) {
        matches = FALSE;
      }

      /* Check region */
      if (filter->region && *filter->region) {
        if (!meta->region || g_ascii_strcasecmp(meta->region, filter->region) != 0) {
          matches = FALSE;
        }
      }

      /* Check country */
      if (filter->country_code && *filter->country_code) {
        if (!meta->country_code || g_ascii_strcasecmp(meta->country_code, filter->country_code) != 0) {
          matches = FALSE;
        }
      }

      /* Check required NIPs */
      if (filter->required_nips && filter->required_nips_count > 0) {
        for (gsize j = 0; j < filter->required_nips_count; j++) {
          if (!gnostr_nip66_relay_supports_nip(meta, filter->required_nips[j])) {
            matches = FALSE;
            break;
          }
        }
      }

      /* Check uptime */
      if (filter->min_uptime_percent > 0 && meta->uptime_percent < filter->min_uptime_percent) {
        matches = FALSE;
      }

      /* Check latency */
      if (filter->max_latency_ms > 0 && meta->latency_ms > 0 &&
          meta->latency_ms > filter->max_latency_ms) {
        matches = FALSE;
      }
    }

    if (matches) {
      g_ptr_array_add(result, meta);
    }
  }

  g_ptr_array_unref(all);
  return result;
}

/* ============== Well-Known Monitors ============== */

const gchar **gnostr_nip66_get_known_monitors(void)
{
  return s_known_monitors;
}

const gchar **gnostr_nip66_get_known_monitor_relays(void)
{
  return s_known_monitor_relays;
}

/* ============== Filter JSON Building ============== */

gchar *gnostr_nip66_build_relay_meta_filter(const gchar **relay_urls,
                                             gsize n_urls,
                                             gint limit)
{
  json_t *filter = json_object();
  json_t *kinds = json_array();
  json_array_append_new(kinds, json_integer(GNOSTR_NIP66_KIND_RELAY_META));
  json_object_set_new(filter, "kinds", kinds);

  if (relay_urls && n_urls > 0) {
    json_t *d_tags = json_array();
    for (gsize i = 0; i < n_urls; i++) {
      json_array_append_new(d_tags, json_string(relay_urls[i]));
    }
    json_object_set_new(filter, "#d", d_tags);
  }

  if (limit > 0) {
    json_object_set_new(filter, "limit", json_integer(limit));
  } else {
    json_object_set_new(filter, "limit", json_integer(500));
  }

  gchar *result = json_dumps(filter, JSON_COMPACT);
  json_decref(filter);
  return result;
}

gchar *gnostr_nip66_build_monitor_filter(const gchar **monitor_pubkeys, gsize n_pubkeys)
{
  json_t *filter = json_object();
  json_t *kinds = json_array();
  json_array_append_new(kinds, json_integer(GNOSTR_NIP66_KIND_RELAY_MONITOR));
  json_object_set_new(filter, "kinds", kinds);

  if (monitor_pubkeys && n_pubkeys > 0) {
    json_t *authors = json_array();
    for (gsize i = 0; i < n_pubkeys; i++) {
      json_array_append_new(authors, json_string(monitor_pubkeys[i]));
    }
    json_object_set_new(filter, "authors", authors);
  }

  json_object_set_new(filter, "limit", json_integer(50));

  gchar *result = json_dumps(filter, JSON_COMPACT);
  json_decref(filter);
  return result;
}

/* ============== Formatting Helpers ============== */

gchar *gnostr_nip66_format_uptime(gdouble uptime_percent)
{
  if (uptime_percent <= 0) return g_strdup("N/A");
  return g_strdup_printf("%.1f%%", uptime_percent);
}

gchar *gnostr_nip66_format_latency(gint64 latency_ms)
{
  if (latency_ms <= 0) return g_strdup("N/A");
  if (latency_ms < 1000) return g_strdup_printf("%" G_GINT64_FORMAT "ms", latency_ms);
  return g_strdup_printf("%.1fs", latency_ms / 1000.0);
}

gchar *gnostr_nip66_format_last_seen(gint64 last_seen)
{
  if (last_seen <= 0) return g_strdup("Never");

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gint64 diff = now - last_seen;

  if (diff < 0) return g_strdup("Just now");
  if (diff < 60) return g_strdup("Just now");
  if (diff < 3600) return g_strdup_printf("%" G_GINT64_FORMAT " min ago", diff / 60);
  if (diff < 86400) return g_strdup_printf("%" G_GINT64_FORMAT " hours ago", diff / 3600);
  return g_strdup_printf("%" G_GINT64_FORMAT " days ago", diff / 86400);
}

gchar *gnostr_nip66_format_nips(const GnostrNip66RelayMeta *meta)
{
  if (!meta || !meta->supported_nips || meta->supported_nips_count == 0) {
    return g_strdup("(none)");
  }

  GString *str = g_string_new(NULL);
  for (gsize i = 0; i < meta->supported_nips_count; i++) {
    if (i > 0) g_string_append(str, ", ");
    g_string_append_printf(str, "%d", meta->supported_nips[i]);
  }
  return g_string_free(str, FALSE);
}

const gchar *gnostr_nip66_get_region_for_country(const gchar *country_code)
{
  if (!country_code || strlen(country_code) != 2) return "Unknown";

  /* North America */
  if (g_strcmp0(country_code, "US") == 0 ||
      g_strcmp0(country_code, "CA") == 0 ||
      g_strcmp0(country_code, "MX") == 0) {
    return "North America";
  }

  /* Europe */
  static const gchar *europe[] = {
    "GB", "DE", "FR", "IT", "ES", "PT", "NL", "BE", "CH", "AT",
    "PL", "CZ", "SK", "HU", "RO", "BG", "GR", "HR", "SI", "RS",
    "SE", "NO", "DK", "FI", "IE", "LU", "EE", "LV", "LT", "UA",
    "BY", "MD", "AL", "MK", "BA", "ME", "XK", "MT", "CY", "IS",
    NULL
  };
  for (const gchar **p = europe; *p; p++) {
    if (g_strcmp0(country_code, *p) == 0) return "Europe";
  }

  /* Asia Pacific */
  static const gchar *asia_pacific[] = {
    "JP", "CN", "KR", "IN", "AU", "NZ", "SG", "HK", "TW", "MY",
    "TH", "VN", "PH", "ID", "PK", "BD", "LK", "NP", "MM", "KH",
    "LA", "MN", "KZ", "UZ", "KG", "TJ", "TM", "AZ", "GE", "AM",
    NULL
  };
  for (const gchar **p = asia_pacific; *p; p++) {
    if (g_strcmp0(country_code, *p) == 0) return "Asia Pacific";
  }

  /* South America */
  static const gchar *south_america[] = {
    "BR", "AR", "CL", "CO", "PE", "VE", "EC", "BO", "PY", "UY",
    "GY", "SR", NULL
  };
  for (const gchar **p = south_america; *p; p++) {
    if (g_strcmp0(country_code, *p) == 0) return "South America";
  }

  /* Middle East */
  static const gchar *middle_east[] = {
    "AE", "SA", "IL", "TR", "IR", "IQ", "SY", "JO", "LB", "KW",
    "QA", "BH", "OM", "YE", NULL
  };
  for (const gchar **p = middle_east; *p; p++) {
    if (g_strcmp0(country_code, *p) == 0) return "Middle East";
  }

  /* Africa */
  static const gchar *africa[] = {
    "ZA", "EG", "NG", "KE", "MA", "GH", "TZ", "UG", "DZ", "TN",
    "ET", "SD", "LY", "AO", "MZ", "ZW", "BW", "NA", "SN", "CI",
    NULL
  };
  for (const gchar **p = africa; *p; p++) {
    if (g_strcmp0(country_code, *p) == 0) return "Africa";
  }

  return "Other";
}

/* ============== Async Discovery Implementation ============== */

#ifndef GNOSTR_NIP66_TEST_ONLY

typedef struct {
  GnostrNip66DiscoveryCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GPtrArray *relays_found;
  GPtrArray *monitors_found;
  gint pending_queries;
} Nip66DiscoveryCtx;

static void nip66_discovery_ctx_free(Nip66DiscoveryCtx *ctx)
{
  if (!ctx) return;
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->relays_found) g_ptr_array_unref(ctx->relays_found);
  if (ctx->monitors_found) g_ptr_array_unref(ctx->monitors_found);
  g_free(ctx);
}

static void on_nip66_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip66DiscoveryCtx *ctx = (Nip66DiscoveryCtx*)user_data;
  if (!ctx) return;

  ctx->pending_queries--;

  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("nip66: query failed: %s", err->message);
    }
    g_error_free(err);
  } else if (results && results->len > 0) {
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);

      /* Try parsing as relay metadata first */
      GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(json);
      if (meta) {
        g_ptr_array_add(ctx->relays_found, meta);
        /* Also add to cache (cache takes ownership of a copy) */
        GnostrNip66RelayMeta *meta_copy = gnostr_nip66_parse_relay_meta(json);
        if (meta_copy) gnostr_nip66_cache_add_relay(meta_copy);
        continue;
      }

      /* Try parsing as monitor */
      GnostrNip66RelayMonitor *monitor = gnostr_nip66_parse_relay_monitor(json);
      if (monitor) {
        g_ptr_array_add(ctx->monitors_found, monitor);
        GnostrNip66RelayMonitor *monitor_copy = gnostr_nip66_parse_relay_monitor(json);
        if (monitor_copy) gnostr_nip66_cache_add_monitor(monitor_copy);
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  /* Check if all queries are done */
  if (ctx->pending_queries <= 0) {
    if (ctx->callback) {
      ctx->callback(ctx->relays_found, ctx->monitors_found, NULL, ctx->user_data);
    }
    /* Don't free arrays - callback takes ownership */
    ctx->relays_found = NULL;
    ctx->monitors_found = NULL;
    nip66_discovery_ctx_free(ctx);
  }
}

void gnostr_nip66_discover_relays_async(GnostrNip66DiscoveryCallback callback,
                                         gpointer user_data,
                                         GCancellable *cancellable)
{
  ensure_cache_init();

  Nip66DiscoveryCtx *ctx = g_new0(Nip66DiscoveryCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->relays_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_meta_free);
  ctx->monitors_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_monitor_free);

  /* Get relay URLs to query */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  /* Add known monitor relays */
  for (const gchar **p = s_known_monitor_relays; *p; p++) {
    g_ptr_array_add(relay_urls, g_strdup(*p));
  }

  /* Also add configured relays */
  gnostr_load_relays_into(relay_urls);

  if (relay_urls->len == 0) {
    /* No relays configured */
    g_ptr_array_unref(relay_urls);
    if (callback) {
      callback(ctx->relays_found, ctx->monitors_found, NULL, user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    }
    nip66_discovery_ctx_free(ctx);
    return;
  }

  /* Build URL array for pool */
  const gchar **urls = g_new0(const gchar*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Build filters */
  NostrFilter *meta_filter = nostr_filter_new();
  int meta_kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(meta_filter, meta_kinds, 1);
  nostr_filter_set_limit(meta_filter, 500);

  NostrFilter *monitor_filter = nostr_filter_new();
  int monitor_kinds[1] = { GNOSTR_NIP66_KIND_RELAY_MONITOR };
  nostr_filter_set_kinds(monitor_filter, monitor_kinds, 1);
  nostr_filter_set_limit(monitor_filter, 50);

  /* Use static pool */
  static GnostrSimplePool *nip66_pool = NULL;
  if (!nip66_pool) nip66_pool = gnostr_simple_pool_new();

  /* Start queries */
  ctx->pending_queries = 2;

  gnostr_simple_pool_query_single_async(
    nip66_pool,
    urls,
    relay_urls->len,
    meta_filter,
    ctx->cancellable,
    on_nip66_query_done,
    ctx
  );

  gnostr_simple_pool_query_single_async(
    nip66_pool,
    urls,
    relay_urls->len,
    monitor_filter,
    ctx->cancellable,
    on_nip66_query_done,
    ctx
  );

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(meta_filter);
  nostr_filter_free(monitor_filter);
}

void gnostr_nip66_discover_from_monitors_async(const gchar **monitor_pubkeys,
                                                 gsize n_pubkeys,
                                                 GnostrNip66DiscoveryCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable)
{
  ensure_cache_init();

  /* If no specific monitors, use known ones */
  if (!monitor_pubkeys || n_pubkeys == 0) {
    gnostr_nip66_discover_relays_async(callback, user_data, cancellable);
    return;
  }

  Nip66DiscoveryCtx *ctx = g_new0(Nip66DiscoveryCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->relays_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_meta_free);
  ctx->monitors_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_monitor_free);

  /* Get relay URLs */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  for (const gchar **p = s_known_monitor_relays; *p; p++) {
    g_ptr_array_add(relay_urls, g_strdup(*p));
  }
  gnostr_load_relays_into(relay_urls);

  if (relay_urls->len == 0) {
    g_ptr_array_unref(relay_urls);
    if (callback) {
      callback(ctx->relays_found, ctx->monitors_found, NULL, user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    }
    nip66_discovery_ctx_free(ctx);
    return;
  }

  const gchar **urls = g_new0(const gchar*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Build filter for relay metadata from specific monitors */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, monitor_pubkeys, n_pubkeys);
  nostr_filter_set_limit(filter, 500);

  static GnostrSimplePool *nip66_pool = NULL;
  if (!nip66_pool) nip66_pool = gnostr_simple_pool_new();

  ctx->pending_queries = 1;

  gnostr_simple_pool_query_single_async(
    nip66_pool,
    urls,
    relay_urls->len,
    filter,
    ctx->cancellable,
    on_nip66_query_done,
    ctx
  );

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(filter);
}

#endif /* GNOSTR_NIP66_TEST_ONLY */
