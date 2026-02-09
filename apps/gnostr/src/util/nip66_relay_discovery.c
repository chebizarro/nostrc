/**
 * NIP-66 Relay Discovery and Monitoring Implementation
 *
 * Provides parsing, caching, and querying for NIP-66 relay discovery events.
 */

#include "nip66_relay_discovery.h"
#include "nostr_json.h"
#include "json.h"
#include <string.h>
#include <time.h>

#ifndef GNOSTR_NIP66_TEST_ONLY
#include "relays.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr_pool.h"
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

/* Well-known NIP-66 relay monitor pubkeys.
 * These are pubkeys of services that publish kind 30166 relay metadata events.
 * nostrc-ns2k: Verified active with `nak req -k 30166 -a <pk> wss://relay.damus.io` */
static const gchar *s_known_monitors[] = {
  /* nostr.watch Amsterdam monitor - most prolific NIP-66 data source */
  "9bbbb845e5b6c831c29789900769843ab43bb5047abe697870cb50b6fc9bf923",
  /* Active monitor (publishes to relay.damus.io, verified 2026-02-08) */
  "0b01aa38c2cc9abfbe4a10d54b182793479fb80da14a91d13be38ea555b22bfd",
  /* Active monitor (publishes to relay.nostr.watch, verified 2026-02-08) */
  "9ba1d7892cd057f5aca5d629a5a601f64bc3e0f1fc6ed9c939845e25d5e1e254",
  /* relay.tools monitor */
  "d35e8b4ac79a66a4c47ef2f35a8b5057c5d72f1094c83c0ebf9c5d1eb1f9b9ff",
  NULL
};

/* Relays known to host NIP-66 monitor data.
 * nostrc-ns2k: Verified with `nak req -k 30166 --limit 5 <url>` on 2026-02-08.
 * relay.nostr.band was returning 502; purplepag.es had 0 kind 30166 events. */
static const gchar *s_known_monitor_relays[] = {
  "wss://relay.damus.io",       /* Confirmed: has kind 30166 from multiple monitors */
  "wss://relay.nostr.watch",    /* Dedicated NIP-66 relay, confirmed working */
  "wss://nos.lol",              /* Large general relay, has some kind 30166 */
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

/* ============== JSON Parsing Helpers (NostrJsonInterface-based) ============== */

/* Helper: get string from JSON object (returns newly allocated string or NULL) */
static gchar *nip66_get_string_or_null(const char *json, const gchar *key)
{
  char *val = NULL;
  val = gnostr_json_get_string(json, key, NULL);
  if (val) {
    return val;
  }
  return NULL;
}

/* Helper: get string from nested object */
static gchar *nip66_get_string_at_or_null(const char *json, const gchar *obj_key, const gchar *key)
{
  char *val = NULL;
  if ((val = gnostr_json_get_string_at(json, obj_key, key, NULL)) != NULL ) {
    return val;
  }
  return NULL;
}

/* Helper: get int64 from JSON object (returns 0 on failure) */
static gint64 nip66_get_int_or_zero(const char *json, const gchar *key)
{
  int64_t val = 0;
  val = gnostr_json_get_int64(json, key, NULL);
  return val;
}

/* Helper: get int from nested object */
static gint nip66_get_int_at_or_zero(const char *json, const gchar *obj_key, const gchar *key)
{
  int val = 0;
  val = gnostr_json_get_int_at(json, obj_key, key, NULL);
  return val;
}

/* Helper: get double from JSON object (returns 0.0 on failure) */
static gdouble nip66_get_double_or_zero(const char *json, const gchar *key)
{
  double val = 0.0;
  val = gnostr_json_get_double(json, key, NULL);
  return val;
}

/* Helper: get bool from JSON object (returns FALSE on failure) */
static gboolean nip66_get_bool_or_false(const char *json, const gchar *key)
{
  gboolean val = FALSE;
  val = gnostr_json_get_boolean(json, key, NULL);
  return val;
}

/* Helper: get bool from nested object */
static gboolean nip66_get_bool_at_or_false(const char *json, const gchar *obj_key, const gchar *key)
{
  gboolean val = FALSE;
  val = gnostr_json_get_bool_at(json, obj_key, key, NULL);
  return val;
}

/* Context for collecting tag values */
typedef struct {
  const gchar *tag_name;
  gint value_index;
  gchar *found_value;
  gboolean first_only;
} FindTagCtx;

typedef struct {
  const gchar *tag_name;
  GPtrArray *values;
} FindAllTagsCtx;

/* Callback for finding a specific tag value */
static gboolean find_tag_value_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  FindTagCtx *ctx = (FindTagCtx *)user_data;

  if (ctx->found_value) return FALSE; /* Already found, stop */
  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  if (g_strcmp0(tag_name, ctx->tag_name) == 0) {
    char *val = NULL;
    if ((val = gnostr_json_get_array_string(element_json, NULL, (size_t)ctx->value_index, NULL)) != NULL) {
      ctx->found_value = val;
    }
    g_free(tag_name);
    return !ctx->first_only; /* Stop if first_only */
  }

  g_free(tag_name);
  return TRUE;
}

/* Callback for finding all values for a tag name */
static gboolean find_all_tag_values_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  FindAllTagsCtx *ctx = (FindAllTagsCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  if (g_strcmp0(tag_name, ctx->tag_name) == 0) {
    char *val = NULL;
    val = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (val) {
      g_ptr_array_add(ctx->values, val);
    }
  }

  g_free(tag_name);
  return TRUE;
}

/* Find first tag value by name (returns newly allocated string or NULL) */
static gchar *find_tag_value(const char *tags_json, const gchar *tag_name, gint value_index)
{
  if (!tags_json) return NULL;

  FindTagCtx ctx = { .tag_name = tag_name, .value_index = value_index, .found_value = NULL, .first_only = TRUE };
  gnostr_json_array_foreach_root(tags_json, find_tag_value_cb, &ctx);
  return ctx.found_value;
}

/* Find all values for a tag name */
static GPtrArray *find_all_tag_values(const char *tags_json, const gchar *tag_name)
{
  GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
  if (!tags_json) return values;

  FindAllTagsCtx ctx = { .tag_name = tag_name, .values = values };
  gnostr_json_array_foreach_root(tags_json, find_all_tag_values_cb, &ctx);
  return values;
}

/* Callback for parsing rtt (round-trip time) tags
 * NIP-66 format: ["rtt-open", 234], ["rtt-read", 150], ["rtt-write", 200]
 * The tag name includes the type, value is at index 1 */
static gboolean parse_rtt_tag_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  GnostrNip66RelayMeta *meta = (GnostrNip66RelayMeta *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  /* Get tag name */
  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  /* Check if this is an rtt-* tag */
  if (!g_str_has_prefix(tag_name, "rtt")) {
    g_free(tag_name);
    return TRUE;
  }

  /* Get value at index 1 - can be string or number */
  char *rtt_val = NULL;
  rtt_val = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (!rtt_val) {
    g_free(tag_name);
    return TRUE;
  }

  gint64 latency = g_ascii_strtoll(rtt_val, NULL, 10);
  g_free(rtt_val);

  /* NIP-66 uses rtt-open, rtt-read, rtt-write as tag names */
  if (g_strcmp0(tag_name, "rtt-open") == 0) {
    meta->latency_open_ms = latency;
    /* Use open latency as the primary latency if not set */
    if (meta->latency_ms == 0) meta->latency_ms = latency;
  } else if (g_strcmp0(tag_name, "rtt-read") == 0) {
    meta->latency_read_ms = latency;
  } else if (g_strcmp0(tag_name, "rtt-write") == 0) {
    meta->latency_write_ms = latency;
  } else if (g_strcmp0(tag_name, "rtt") == 0) {
    /* Legacy format: ["rtt", "<type>", "<ms>"] - try to parse */
    char *rtt_type = NULL;
    char *rtt_ms = NULL;
    rtt_type = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (rtt_type) {
      rtt_ms = gnostr_json_get_array_string(element_json, NULL, 2, NULL);
      if (rtt_ms) {
        gint64 legacy_latency = g_ascii_strtoll(rtt_ms, NULL, 10);
        if (g_strcmp0(rtt_type, "open") == 0) {
          meta->latency_open_ms = legacy_latency;
          if (meta->latency_ms == 0) meta->latency_ms = legacy_latency;
        } else if (g_strcmp0(rtt_type, "read") == 0) {
          meta->latency_read_ms = legacy_latency;
        } else if (g_strcmp0(rtt_type, "write") == 0) {
          meta->latency_write_ms = legacy_latency;
        }
        g_free(rtt_ms);
      }
      g_free(rtt_type);
    }
  }

  g_free(tag_name);
  return TRUE; /* Continue to find more rtt tags */
}

/* Context for collecting int array */
typedef struct {
  GArray *arr;
} IntArrayCtx;

static gboolean collect_int_array_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  IntArrayCtx *ctx = (IntArrayCtx *)user_data;

  /* Try to parse as integer directly from the raw JSON element */
  int64_t val = 0;
  char *endptr = NULL;
  val = g_ascii_strtoll(element_json, &endptr, 10);
  if (endptr && endptr != element_json) {
    gint int_val = (gint)val;
    g_array_append_val(ctx->arr, int_val);
  }
  return TRUE;
}

/* Convert JSON array to int array */
static gint *nip66_array_to_int_array(const char *arr_json, gsize *out_count)
{
  if (!arr_json || !out_count) return NULL;

  GArray *arr = g_array_new(FALSE, FALSE, sizeof(gint));
  IntArrayCtx ctx = { .arr = arr };
  gnostr_json_array_foreach_root(arr_json, collect_int_array_cb, &ctx);

  *out_count = arr->len;
  if (arr->len == 0) {
    g_array_free(arr, TRUE);
    return NULL;
  }
  return (gint *)g_array_free(arr, FALSE);
}

/* ============== Parsing: Relay Metadata (kind 30166) ============== */

GnostrNip66RelayMeta *gnostr_nip66_parse_relay_meta(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  if (!gnostr_json_is_valid(event_json)) {
    g_debug("nip66: failed to parse relay meta JSON");
    return NULL;
  }

  /* Validate kind */
  int64_t kind_val = gnostr_json_get_int64(event_json, "kind", NULL);
  if (kind_val != GNOSTR_NIP66_KIND_RELAY_META) {
    return NULL;
  }

  GnostrNip66RelayMeta *meta = g_new0(GnostrNip66RelayMeta, 1);

  /* Extract basic event fields */
  meta->event_id_hex = nip66_get_string_or_null(event_json, "id");
  meta->pubkey_hex = nip66_get_string_or_null(event_json, "pubkey");
  meta->created_at = nip66_get_int_or_zero(event_json, "created_at");
  meta->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (tags_json) {
    /* d tag = relay URL */
    gchar *d_val = find_tag_value(tags_json, "d", 1);
    if (d_val) {
      meta->d_tag = d_val;
      meta->relay_url = g_strdup(d_val);
    }

    /* r tag = alternative relay URL */
    if (!meta->relay_url) {
      gchar *r_val = find_tag_value(tags_json, "r", 1);
      if (r_val) meta->relay_url = r_val;
    }

    /* n tag = network type */
    gchar *n_val = find_tag_value(tags_json, "n", 1);
    if (n_val) {
      meta->network = gnostr_nip66_parse_network(n_val);
      g_free(n_val);
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
    GPtrArray *nip_values = find_all_tag_values(tags_json, "N");
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

    /* Geographic tags - parse geohash to lat/lon (nostrc-n63f) */
    gchar *geo_val = find_tag_value(tags_json, "g", 1);  /* geohash or coordinates */
    if (geo_val && *geo_val) {
      /* Simple geohash decoder - decodes center point of geohash cell */
      static const char base32[] = "0123456789bcdefghjkmnpqrstuvwxyz";
      gdouble lat_min = -90.0, lat_max = 90.0;
      gdouble lon_min = -180.0, lon_max = 180.0;
      gboolean is_lon = TRUE;  /* geohash alternates: lon bit, lat bit, lon bit, ... */

      for (const gchar *p = geo_val; *p; p++) {
        const gchar *idx = strchr(base32, g_ascii_tolower(*p));
        if (!idx) break;  /* invalid character */

        int val = (int)(idx - base32);
        for (int bit = 4; bit >= 0; bit--) {
          if (is_lon) {
            gdouble mid = (lon_min + lon_max) / 2.0;
            if (val & (1 << bit)) lon_min = mid;
            else lon_max = mid;
          } else {
            gdouble mid = (lat_min + lat_max) / 2.0;
            if (val & (1 << bit)) lat_min = mid;
            else lat_max = mid;
          }
          is_lon = !is_lon;
        }
      }

      meta->latitude = (lat_min + lat_max) / 2.0;
      meta->longitude = (lon_min + lon_max) / 2.0;
      meta->has_geolocation = TRUE;
    }
    g_free(geo_val);

    gchar *country_val = find_tag_value(tags_json, "G", 1);  /* Country code */
    if (country_val) {
      meta->country_code = country_val;
      meta->region = g_strdup(gnostr_nip66_get_region_for_country(country_val));
    }

    /* t tags = generic tags */
    GPtrArray *tag_values = find_all_tag_values(tags_json, "t");
    if (tag_values->len > 0) {
      meta->tags = g_new0(gchar*, tag_values->len + 1);
      meta->tags_count = tag_values->len;
      for (guint i = 0; i < tag_values->len; i++) {
        meta->tags[i] = g_strdup(g_ptr_array_index(tag_values, i));
      }
    }
    g_ptr_array_unref(tag_values);

    /* L/l tags for status */
    gchar *status_val = find_tag_value(tags_json, "l", 1);
    if (status_val) {
      meta->has_status = TRUE;
      meta->is_online = (g_ascii_strcasecmp(status_val, "online") == 0);
      g_free(status_val);
    }

    /* rtt tags = round-trip time / latency
     * Format: ["rtt", "<type>", "<milliseconds>"]
     * where type is "open", "read", or "write"
     * Need to iterate all rtt tags since there may be multiple */
    gnostr_json_array_foreach_root(tags_json, parse_rtt_tag_cb, meta);

    g_free(tags_json);
  }

  /* Parse content JSON (NIP-11 style info) */
  char *content_str = NULL;
  content_str = gnostr_json_get_string(event_json, "content", NULL);
  if (content_str) {
    if (gnostr_json_is_valid(content_str)) {
      meta->name = nip66_get_string_or_null(content_str, "name");
      meta->description = nip66_get_string_or_null(content_str, "description");
      meta->pubkey = nip66_get_string_or_null(content_str, "pubkey");
      meta->contact = nip66_get_string_or_null(content_str, "contact");
      meta->software = nip66_get_string_or_null(content_str, "software");
      meta->version = nip66_get_string_or_null(content_str, "version");
      meta->icon = nip66_get_string_or_null(content_str, "icon");

      /* supported_nips from content (if not in tags) */
      if (!meta->supported_nips) {
        char *nips_arr_json = NULL;
        nips_arr_json = gnostr_json_get_raw(content_str, "supported_nips", NULL);
        if (nips_arr_json) {
          meta->supported_nips = nip66_array_to_int_array(nips_arr_json, &meta->supported_nips_count);
          g_free(nips_arr_json);
        }
      }

      /* limitations object */
      meta->max_message_length = nip66_get_int_at_or_zero(content_str, "limitation", "max_message_length");
      meta->max_content_length = nip66_get_int_at_or_zero(content_str, "limitation", "max_content_length");
      meta->max_event_tags = nip66_get_int_at_or_zero(content_str, "limitation", "max_event_tags");
      meta->max_subscriptions = nip66_get_int_at_or_zero(content_str, "limitation", "max_subscriptions");
      meta->auth_required = nip66_get_bool_at_or_false(content_str, "limitation", "auth_required");
      meta->payment_required = nip66_get_bool_at_or_false(content_str, "limitation", "payment_required");
      meta->restricted_writes = nip66_get_bool_at_or_false(content_str, "limitation", "restricted_writes");

      /* Monitoring stats from content */
      meta->uptime_percent = nip66_get_double_or_zero(content_str, "uptime");
      if (meta->latency_ms == 0) {
        meta->latency_ms = nip66_get_int_or_zero(content_str, "latency");
      }
      if (meta->last_seen == 0) {
        meta->last_seen = nip66_get_int_or_zero(content_str, "last_seen");
      }
      if (meta->first_seen == 0) {
        meta->first_seen = nip66_get_int_or_zero(content_str, "first_seen");
      }

      /* Geographic info from content */
      if (!meta->country_code) {
        meta->country_code = nip66_get_string_or_null(content_str, "country_code");
        if (meta->country_code) {
          meta->region = g_strdup(gnostr_nip66_get_region_for_country(meta->country_code));
        }
      }
      if (!meta->city) {
        meta->city = nip66_get_string_or_null(content_str, "city");
      }

      /* Check for geolocation */
      double lat = gnostr_json_get_double(content_str, "latitude", NULL);
      double lon = gnostr_json_get_double(content_str, "longitude", NULL);
      if (lat != 0.0 || lon != 0.0) {
        meta->latitude = lat;
        meta->longitude = lon;
        meta->has_geolocation = TRUE;
      }
    }
    g_free(content_str);
  }

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

  if (!gnostr_json_is_valid(event_json)) {
    g_debug("nip66: failed to parse relay monitor JSON");
    return NULL;
  }

  /* Validate kind */
  int64_t kind_val = gnostr_json_get_int64(event_json, "kind", NULL);
  if (kind_val != GNOSTR_NIP66_KIND_RELAY_MONITOR) {
    return NULL;
  }

  GnostrNip66RelayMonitor *monitor = g_new0(GnostrNip66RelayMonitor, 1);

  /* Extract basic event fields */
  monitor->event_id_hex = nip66_get_string_or_null(event_json, "id");
  monitor->pubkey_hex = nip66_get_string_or_null(event_json, "pubkey");
  monitor->created_at = nip66_get_int_or_zero(event_json, "created_at");
  monitor->cached_at = g_get_real_time() / G_USEC_PER_SEC;

  /* Parse tags */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (tags_json) {
    /* frequency tag */
    gchar *freq_val = find_tag_value(tags_json, "frequency", 1);
    if (freq_val) monitor->frequency = freq_val;

    /* relay hints (r tags) */
    GPtrArray *relay_hints = find_all_tag_values(tags_json, "r");
    if (relay_hints->len > 0) {
      monitor->relay_hints = g_new0(gchar*, relay_hints->len + 1);
      monitor->relay_hints_count = relay_hints->len;
      for (guint i = 0; i < relay_hints->len; i++) {
        monitor->relay_hints[i] = g_strdup(g_ptr_array_index(relay_hints, i));
      }
    }
    g_ptr_array_unref(relay_hints);

    /* region tags */
    GPtrArray *regions = find_all_tag_values(tags_json, "g");
    if (regions->len > 0) {
      monitor->monitored_regions = g_new0(gchar*, regions->len + 1);
      monitor->monitored_regions_count = regions->len;
      for (guint i = 0; i < regions->len; i++) {
        monitor->monitored_regions[i] = g_strdup(g_ptr_array_index(regions, i));
      }
    }
    g_ptr_array_unref(regions);

    g_free(tags_json);
  }

  /* Parse content JSON */
  char *content_str = NULL;
  content_str = gnostr_json_get_string(event_json, "content", NULL);
  if (content_str) {
    if (gnostr_json_is_valid(content_str)) {
      monitor->name = nip66_get_string_or_null(content_str, "name");
      monitor->description = nip66_get_string_or_null(content_str, "description");
      monitor->operator_pubkey = nip66_get_string_or_null(content_str, "pubkey");
      monitor->contact = nip66_get_string_or_null(content_str, "contact");
      monitor->website = nip66_get_string_or_null(content_str, "website");

      if (!monitor->frequency) {
        monitor->frequency = nip66_get_string_or_null(content_str, "frequency");
      }
    }
    g_free(content_str);
  }

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
      /* Check flags - only filter out relays that are explicitly offline
       * (has_status=TRUE and is_online=FALSE). Treat unknown status as possibly online. */
      if ((filter->flags & GNOSTR_NIP66_FILTER_ONLINE_ONLY) &&
          meta->has_status && !meta->is_online) {
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
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kinds");
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_int(builder, GNOSTR_NIP66_KIND_RELAY_META);
  gnostr_json_builder_end_array(builder);

  if (relay_urls && n_urls > 0) {
    gnostr_json_builder_set_key(builder, "#d");
    gnostr_json_builder_begin_array(builder);
    for (gsize i = 0; i < n_urls; i++) {
      gnostr_json_builder_add_string(builder, relay_urls[i]);
    }
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_set_key(builder, "limit");
  gnostr_json_builder_add_int(builder, limit > 0 ? limit : 500);

  gnostr_json_builder_end_object(builder);
  gchar *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);
  return result;
}

gchar *gnostr_nip66_build_monitor_filter(const gchar **monitor_pubkeys, gsize n_pubkeys)
{
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kinds");
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_int(builder, GNOSTR_NIP66_KIND_RELAY_MONITOR);
  gnostr_json_builder_end_array(builder);

  if (monitor_pubkeys && n_pubkeys > 0) {
    gnostr_json_builder_set_key(builder, "authors");
    gnostr_json_builder_begin_array(builder);
    for (gsize i = 0; i < n_pubkeys; i++) {
      gnostr_json_builder_add_string(builder, monitor_pubkeys[i]);
    }
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_set_key(builder, "limit");
  gnostr_json_builder_add_int(builder, 50);

  gnostr_json_builder_end_object(builder);
  gchar *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);
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

/* Two-phase discovery context */
typedef struct {
  GnostrNip66DiscoveryCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GPtrArray *relays_found;
  GPtrArray *monitors_found;
  gint pending_queries;
  gboolean phase2_started;
} Nip66DiscoveryCtx;

static GNostrPool *g_nip66_pool = NULL;

static GNostrPool *get_nip66_pool(void)
{
  if (!g_nip66_pool) g_nip66_pool = gnostr_pool_new();
  return g_nip66_pool;
}

static void nip66_discovery_ctx_free(Nip66DiscoveryCtx *ctx)
{
  if (!ctx) return;
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->relays_found) g_ptr_array_unref(ctx->relays_found);
  if (ctx->monitors_found) g_ptr_array_unref(ctx->monitors_found);
  g_free(ctx);
}

static void on_phase2_relay_meta_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void start_phase2_relay_discovery(Nip66DiscoveryCtx *ctx);

/* Phase 1 callback: collect monitors, then start phase 2 */
static void on_phase1_monitors_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip66DiscoveryCtx *ctx = (Nip66DiscoveryCtx*)user_data;
  if (!ctx) return;

  ctx->pending_queries--;

  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("nip66 phase1: query failed: %s", err->message);
    }
    g_error_free(err);
  } else if (results && results->len > 0) {
    g_debug("nip66 phase1: received %u events", results->len);
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);

      /* Parse as monitor (kind 10166) */
      GnostrNip66RelayMonitor *monitor = gnostr_nip66_parse_relay_monitor(json);
      if (monitor) {
        g_debug("nip66 phase1: found monitor %s with %zu relay hints",
                monitor->pubkey_hex ? monitor->pubkey_hex : "(null)",
                monitor->relay_hints_count);
        g_ptr_array_add(ctx->monitors_found, monitor);
        /* Cache it */
        GnostrNip66RelayMonitor *monitor_copy = gnostr_nip66_parse_relay_monitor(json);
        if (monitor_copy) gnostr_nip66_cache_add_monitor(monitor_copy);
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  /* Phase 1 complete - start phase 2 */
  if (ctx->pending_queries <= 0 && !ctx->phase2_started) {
    ctx->phase2_started = TRUE;
    start_phase2_relay_discovery(ctx);
  }
}

/* Phase 2: Query relay metadata from each monitor's relay hints */
static void start_phase2_relay_discovery(Nip66DiscoveryCtx *ctx)
{
  if (!ctx) return;

  /* Check if cancelled */
  if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
    if (ctx->callback) {
      ctx->callback(ctx->relays_found, ctx->monitors_found, NULL, ctx->user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    }
    nip66_discovery_ctx_free(ctx);
    return;
  }

  /* Collect unique relay URLs and monitor pubkeys from discovered monitors */
  GHashTable *relay_url_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *pubkey_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < ctx->monitors_found->len; i++) {
    GnostrNip66RelayMonitor *monitor = g_ptr_array_index(ctx->monitors_found, i);
    if (!monitor) continue;

    /* Add monitor pubkey */
    if (monitor->pubkey_hex && *monitor->pubkey_hex) {
      g_hash_table_add(pubkey_set, g_strdup(monitor->pubkey_hex));
    }

    /* Add relay hints */
    for (gsize j = 0; j < monitor->relay_hints_count; j++) {
      if (monitor->relay_hints[j] && *monitor->relay_hints[j]) {
        g_hash_table_add(relay_url_set, g_strdup(monitor->relay_hints[j]));
      }
    }
  }

  guint n_relays = g_hash_table_size(relay_url_set);
  guint n_pubkeys = g_hash_table_size(pubkey_set);

  g_debug("nip66 phase2: %u monitors, %u relay hints, %u unique pubkeys",
          ctx->monitors_found->len, n_relays, n_pubkeys);

  /* If no relay hints found, fall back to known relay URLs */
  if (n_relays == 0) {
    g_debug("nip66 phase2: no relay hints, using known monitor relays");
    for (const gchar **p = s_known_monitor_relays; *p; p++) {
      g_hash_table_add(relay_url_set, g_strdup(*p));
    }
    n_relays = g_hash_table_size(relay_url_set);
  }

  /* nostrc-q42: If no monitors discovered in phase 1, fall back to known monitor pubkeys */
  if (n_pubkeys == 0) {
    g_debug("nip66 phase2: no monitors discovered, using known monitor pubkeys");
    for (const gchar **p = s_known_monitors; *p; p++) {
      g_hash_table_add(pubkey_set, g_strdup(*p));
    }
    n_pubkeys = g_hash_table_size(pubkey_set);
  }

  /* If still no relays or no pubkeys, complete with what we have */
  if (n_relays == 0 || n_pubkeys == 0) {
    g_debug("nip66 phase2: no relays or pubkeys, completing");
    g_hash_table_destroy(relay_url_set);
    g_hash_table_destroy(pubkey_set);
    if (ctx->callback) {
      ctx->callback(ctx->relays_found, ctx->monitors_found, NULL, ctx->user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    }
    nip66_discovery_ctx_free(ctx);
    return;
  }

  /* Build URL array */
  const gchar **urls = g_new0(const gchar*, n_relays + 1);
  GHashTableIter iter;
  gpointer key;
  guint idx = 0;
  g_hash_table_iter_init(&iter, relay_url_set);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    urls[idx++] = (const gchar*)key;
  }

  /* Build pubkey array */
  const gchar **pubkeys = g_new0(const gchar*, n_pubkeys + 1);
  idx = 0;
  g_hash_table_iter_init(&iter, pubkey_set);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    pubkeys[idx++] = (const gchar*)key;
  }

  /* Build filter for relay metadata from discovered monitors */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, pubkeys, n_pubkeys);
  nostr_filter_set_limit(filter, 500);

  ctx->pending_queries = 1;

    gnostr_pool_sync_relays(get_nip66_pool(), (const gchar **)urls, n_relays);
  {
    /* nostrc-ns2k: Use unique key to avoid freeing filters from concurrent queries */
    static gint _qf_counter_n66p2 = 0;
    int _qfid = g_atomic_int_add(&_qf_counter_n66p2, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-n66p2-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(get_nip66_pool()), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(get_nip66_pool(), _qf, ctx->cancellable, on_phase2_relay_meta_done, ctx);
  }

  g_free(urls);
  g_free(pubkeys);
  g_hash_table_destroy(relay_url_set);
  g_hash_table_destroy(pubkey_set);
  nostr_filter_free(filter);
}

/* Phase 2 callback: collect relay metadata */
static void on_phase2_relay_meta_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip66DiscoveryCtx *ctx = (Nip66DiscoveryCtx*)user_data;
  if (!ctx) return;

  ctx->pending_queries--;

  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("nip66 phase2: query failed: %s", err->message);
    }
    g_error_free(err);
  } else if (results && results->len > 0) {
    g_debug("nip66 phase2: received %u events from relays", results->len);
    guint parsed_count = 0;
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);

      /* Parse as relay metadata (kind 30166) */
      GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(json);
      if (meta) {
        /* Filter out localhost/127.0.0.1 relays */
        if (meta->relay_url &&
            (g_str_has_prefix(meta->relay_url, "ws://127.0.0.1") ||
             g_str_has_prefix(meta->relay_url, "wss://127.0.0.1") ||
             g_str_has_prefix(meta->relay_url, "ws://localhost") ||
             g_str_has_prefix(meta->relay_url, "wss://localhost") ||
             g_str_has_prefix(meta->relay_url, "ws://[::1]") ||
             g_str_has_prefix(meta->relay_url, "wss://[::1]"))) {
          gnostr_nip66_relay_meta_free(meta);
          continue;
        }

        /* Check for duplicate relay URLs */
        gboolean is_duplicate = FALSE;
        for (guint j = 0; j < ctx->relays_found->len; j++) {
          GnostrNip66RelayMeta *existing = g_ptr_array_index(ctx->relays_found, j);
          if (existing && existing->relay_url && meta->relay_url &&
              g_ascii_strcasecmp(existing->relay_url, meta->relay_url) == 0) {
            is_duplicate = TRUE;
            break;
          }
        }
        if (is_duplicate) {
          gnostr_nip66_relay_meta_free(meta);
          continue;
        }

        parsed_count++;
        g_ptr_array_add(ctx->relays_found, meta);
        /* Cache it */
        GnostrNip66RelayMeta *meta_copy = gnostr_nip66_parse_relay_meta(json);
        if (meta_copy) gnostr_nip66_cache_add_relay(meta_copy);
      } else if (i < 3) {
        /* Log first few parse failures to help debug */
        g_warning("nip66 phase2: failed to parse event %u: %.200s...", i, json ? json : "(null)");
      }
    }
    g_debug("nip66 phase2: parsed %u/%u events as relay metadata", parsed_count, results->len);
  } else {
    g_warning("nip66 phase2: no results returned (results=%p, len=%u)",
              (void*)results, results ? results->len : 0);
  }

  if (results) g_ptr_array_unref(results);

  /* All done - invoke callback */
  if (ctx->pending_queries <= 0) {
    g_debug("nip66: discovery complete - %u relays found, %u monitors found",
              ctx->relays_found ? ctx->relays_found->len : 0,
              ctx->monitors_found ? ctx->monitors_found->len : 0);
    if (ctx->callback) {
      g_debug("nip66: invoking callback with results");
      ctx->callback(ctx->relays_found, ctx->monitors_found, NULL, ctx->user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    } else {
      g_warning("nip66: no callback set!");
    }
    nip66_discovery_ctx_free(ctx);
  } else {
    g_debug("nip66: still waiting for %d queries", ctx->pending_queries);
  }
}

void gnostr_nip66_discover_relays_async(GnostrNip66DiscoveryCallback callback,
                                         gpointer user_data,
                                         GCancellable *cancellable)
{
  ensure_cache_init();

  /* Clean up any stale connections in the NIP-66 pool before starting.
   * Even though query_single uses temporary connections, we ensure a clean state. */
  gnostr_pool_disconnect_all(get_nip66_pool());

  Nip66DiscoveryCtx *ctx = g_new0(Nip66DiscoveryCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->relays_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_meta_free);
  ctx->monitors_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_monitor_free);
  ctx->phase2_started = TRUE;  /* Skip phase 1 - direct query */

  /* Get relay URLs to query */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  /* Add known monitor relays - these are relays likely to have kind 30166 events */
  for (const gchar **p = s_known_monitor_relays; *p; p++) {
    g_ptr_array_add(relay_urls, g_strdup(*p));
  }

  /* Also add user's configured relays */
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
  const gchar **urls = g_new0(const gchar*, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  g_debug("nip66: querying %u relays for kind 30166 relay metadata (direct)", relay_urls->len);
  for (guint i = 0; i < relay_urls->len && i < 5; i++) {
    g_debug("nip66:   relay[%u] = %s", i, (const gchar*)g_ptr_array_index(relay_urls, i));
  }

  /* nostrc-q42: Simplified discovery - query kind 30166 directly WITHOUT author filter.
   * Relay metadata events are addressable (d-tag = relay URL) and don't need
   * the complex two-phase monitor discovery. */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_limit(filter, 500);

  ctx->pending_queries = 1;

    gnostr_pool_sync_relays(get_nip66_pool(), (const gchar **)urls, relay_urls->len);
  {
    static gint _qf_counter_n66d = 0;
    int _qfid = g_atomic_int_add(&_qf_counter_n66d, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-n66d-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(get_nip66_pool()), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(get_nip66_pool(), _qf, ctx->cancellable, on_phase2_relay_meta_done, ctx);
  }

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(filter);
}

void gnostr_nip66_discover_from_monitors_async(const gchar **monitor_pubkeys,
                                                 gsize n_pubkeys,
                                                 GnostrNip66DiscoveryCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable)
{
  ensure_cache_init();

  /* If no specific monitors, use full two-phase discovery */
  if (!monitor_pubkeys || n_pubkeys == 0) {
    gnostr_nip66_discover_relays_async(callback, user_data, cancellable);
    return;
  }

  /* Clean up any stale connections in the NIP-66 pool before starting. */
  gnostr_pool_disconnect_all(get_nip66_pool());

  Nip66DiscoveryCtx *ctx = g_new0(Nip66DiscoveryCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->relays_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_meta_free);
  ctx->monitors_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_monitor_free);
  ctx->phase2_started = TRUE;  /* Skip phase 1 since we already have monitor pubkeys */

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

  const gchar **urls = g_new0(const gchar*, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Build filter for relay metadata from specific monitors */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, monitor_pubkeys, n_pubkeys);
  nostr_filter_set_limit(filter, 500);

  ctx->pending_queries = 1;

    gnostr_pool_sync_relays(get_nip66_pool(), (const gchar **)urls, relay_urls->len);
  {
    /* nostrc-ns2k: Use unique key per query to avoid freeing filters still in use
     * by a concurrent query thread (use-after-free on overlapping fetches). */
    static gint _qf_counter_n66m = 0;
    int _qfid = g_atomic_int_add(&_qf_counter_n66m, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-n66m-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    g_object_set_data_full(G_OBJECT(get_nip66_pool()), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(get_nip66_pool(), _qf, ctx->cancellable, on_phase2_relay_meta_done, ctx);
  }

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(filter);
}

/* ============== Streaming Discovery Implementation ============== */

/* Context for streaming discovery */
typedef struct {
  GnostrNip66RelayFoundCallback on_relay_found;
  GnostrNip66DiscoveryCallback on_complete;
  gpointer user_data;
  GCancellable *cancellable;
  GPtrArray *relays_found;
  GPtrArray *monitors_found;
  GHashTable *seen_urls;  /* For deduplication */
  GNostrPool *pool;
  gulong events_handler_id;
  NostrFilter *filter;  /* Single filter for query_single_streaming */
  char **urls;
  size_t url_count;
} Nip66StreamingCtx;

static void nip66_streaming_ctx_free(Nip66StreamingCtx *ctx)
{
  if (!ctx) return;
  if (ctx->events_handler_id && ctx->pool) {
    g_signal_handler_disconnect(ctx->pool, ctx->events_handler_id);
  }
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->relays_found) g_ptr_array_unref(ctx->relays_found);
  if (ctx->monitors_found) g_ptr_array_unref(ctx->monitors_found);
  if (ctx->seen_urls) g_hash_table_destroy(ctx->seen_urls);
  if (ctx->filter) nostr_filter_free(ctx->filter);
  if (ctx->urls) {
    for (size_t i = 0; i < ctx->url_count; i++) g_free(ctx->urls[i]);
    g_free(ctx->urls);
  }
  g_free(ctx);
}

/* Check if relay URL should be filtered out */
static gboolean nip66_should_filter_url(const gchar *url)
{
  if (!url) return TRUE;
  return (g_str_has_prefix(url, "ws://127.0.0.1") ||
          g_str_has_prefix(url, "wss://127.0.0.1") ||
          g_str_has_prefix(url, "ws://localhost") ||
          g_str_has_prefix(url, "wss://localhost") ||
          g_str_has_prefix(url, "ws://[::1]") ||
          g_str_has_prefix(url, "wss://[::1]"));
}

/* Handle batch of events from pool signal */
static void on_streaming_events(GNostrPool *pool, GPtrArray *events, gpointer user_data)
{
  (void)pool;
  Nip66StreamingCtx *ctx = (Nip66StreamingCtx *)user_data;
  if (!ctx || !events) return;

  g_message("nip66 streaming: received batch of %u events", events->len);

  /* Check cancellation */
  if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) return;

  for (guint i = 0; i < events->len; i++) {
    NostrEvent *ev = g_ptr_array_index(events, i);
    if (!ev) continue;

    /* Get event JSON */
    char *json = nostr_event_serialize(ev);
    if (!json) continue;

    /* Parse as relay metadata */
    GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(json);
    free(json);

    if (!meta) continue;

    /* Filter localhost */
    if (nip66_should_filter_url(meta->relay_url)) {
      gnostr_nip66_relay_meta_free(meta);
      continue;
    }

    /* Deduplicate by URL */
    gchar *url_lower = g_ascii_strdown(meta->relay_url, -1);
    if (g_hash_table_contains(ctx->seen_urls, url_lower)) {
      g_debug("nip66 streaming: skipping duplicate %s", meta->relay_url);
      g_free(url_lower);
      gnostr_nip66_relay_meta_free(meta);
      continue;
    }
    g_hash_table_add(ctx->seen_urls, url_lower);
    g_message("nip66 streaming: new relay %s (total: %u)", meta->relay_url, ctx->relays_found->len + 1);

    /* Cache it (parse a fresh copy for the cache) */
    char *cache_json = nostr_event_serialize(ev);
    if (cache_json) {
      GnostrNip66RelayMeta *meta_copy = gnostr_nip66_parse_relay_meta(cache_json);
      if (meta_copy) gnostr_nip66_cache_add_relay(meta_copy);
      free(cache_json);
    }

    /* Invoke per-relay callback before adding to results
     * (callback receives the meta we're about to store) */
    if (ctx->on_relay_found) {
      g_debug("nip66 streaming: invoking on_relay_found callback for %s", meta->relay_url);
      ctx->on_relay_found(meta, ctx->user_data);
    } else {
      g_warning("nip66 streaming: no on_relay_found callback set!");
    }

    /* Add to results */
    g_ptr_array_add(ctx->relays_found, meta);
  }
}

/* nostrc-ns2k: Query completion callback - sole handler for streaming results.
 *
 * Previously, a separate 10s g_timeout_add_seconds raced with this callback:
 * the timeout would fire first, call on_complete with 0 results, cancel the
 * GCancellable, and then GTask would discard the thread's actual results
 * (returning G_IO_ERROR_CANCELLED instead). This made NIP-66 discovery always
 * return 0 relays.
 *
 * Fix: Removed the separate timeout. The pool's default_timeout (set to 10s
 * for the NIP-66 pool) governs how long the query thread polls for events.
 * This callback is the sole completion handler - no race possible. */
static void on_streaming_query_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip66StreamingCtx *ctx = (Nip66StreamingCtx *)user_data;
  if (!ctx) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    g_warning("nip66 streaming: query finished with error: %s (domain=%s code=%d)",
              error->message, g_quark_to_string(error->domain), error->code);
    g_error_free(error);
  }

  g_warning("nip66 streaming: query returned %u raw results", results ? results->len : 0);

  /* hq-r248b: Process results here (previously handled via streaming signal).
   * Results are JSON strings - parse each as relay metadata. */
  if (results && results->len > 0) {
    g_warning("nip66 streaming: processing %u results from query", results->len);
    for (guint i = 0; i < results->len; i++) {
      const char *json = g_ptr_array_index(results, i);
      if (!json) continue;
      GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(json);
      if (!meta) continue;
      if (nip66_should_filter_url(meta->relay_url)) {
        gnostr_nip66_relay_meta_free(meta);
        continue;
      }
      gchar *url_lower = g_ascii_strdown(meta->relay_url, -1);
      if (g_hash_table_contains(ctx->seen_urls, url_lower)) {
        g_free(url_lower);
        gnostr_nip66_relay_meta_free(meta);
        continue;
      }
      g_hash_table_add(ctx->seen_urls, url_lower);
      /* Cache it */
      GnostrNip66RelayMeta *meta_copy = gnostr_nip66_parse_relay_meta(json);
      if (meta_copy) gnostr_nip66_cache_add_relay(meta_copy);
      if (ctx->on_relay_found) ctx->on_relay_found(meta, ctx->user_data);
      g_ptr_array_add(ctx->relays_found, meta);
    }
  }
  if (results) g_ptr_array_unref(results);

  /* Disconnect signal handler */
  if (ctx->events_handler_id && ctx->pool) {
    g_signal_handler_disconnect(ctx->pool, ctx->events_handler_id);
    ctx->events_handler_id = 0;
  }

  g_debug("nip66 streaming: query complete with %u relays",
            ctx->relays_found ? ctx->relays_found->len : 0);

  /* Invoke completion callback */
  if (ctx->on_complete) {
    ctx->on_complete(ctx->relays_found, ctx->monitors_found, NULL, ctx->user_data);
    ctx->relays_found = NULL;
    ctx->monitors_found = NULL;
  }

  nip66_streaming_ctx_free(ctx);
}

void gnostr_nip66_discover_relays_streaming_async(GnostrNip66RelayFoundCallback on_relay_found,
                                                    GnostrNip66DiscoveryCallback on_complete,
                                                    gpointer user_data,
                                                    GCancellable *cancellable)
{
  ensure_cache_init();

  /* Get the NIP-66 pool and ensure any stale connections are cleaned up.
   * Even though streaming queries create temporary connections, we clean up
   * the pool in case previous queries left anything behind (e.g., if cancelled
   * before all relays sent EOSE). */
  GNostrPool *pool = get_nip66_pool();
  gnostr_pool_disconnect_all(pool);

  /* nostrc-ns2k: Use pool's built-in timeout (10s) instead of a separate
   * g_timeout_add that races with the query thread. The old design had a 10s
   * main-thread timeout that would call on_complete with 0 results BEFORE the
   * query thread returned, then GTask would discard the thread's results because
   * the cancellable was already cancelled. */
  /* nostrc-ns2k: 15s timeout to allow time for WS handshake + relay response.
   * Each relay needs: DNS + TCP + TLS + WS upgrade (~1-3s), then REQ send + response.
   * With 3 relays connecting sequentially, 10s was too tight. */
  gnostr_pool_set_default_timeout(pool, 15000);

  Nip66StreamingCtx *ctx = g_new0(Nip66StreamingCtx, 1);
  ctx->on_relay_found = on_relay_found;
  ctx->on_complete = on_complete;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->relays_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_meta_free);
  ctx->monitors_found = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip66_relay_monitor_free);
  ctx->seen_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx->pool = pool;

  /* Collect relay URLs */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  for (const gchar **p = s_known_monitor_relays; *p; p++) {
    g_ptr_array_add(relay_urls, g_strdup(*p));
  }
  gnostr_load_relays_into(relay_urls);

  if (relay_urls->len == 0) {
    g_ptr_array_unref(relay_urls);
    if (on_complete) {
      on_complete(ctx->relays_found, ctx->monitors_found, NULL, user_data);
      ctx->relays_found = NULL;
      ctx->monitors_found = NULL;
    }
    nip66_streaming_ctx_free(ctx);
    return;
  }

  /* Copy URLs for ctx ownership */
  ctx->url_count = relay_urls->len;
  ctx->urls = g_new0(char*, relay_urls->len);
  const gchar **url_ptrs = g_new0(const gchar*, relay_urls->len + 1);
  for (guint i = 0; i < relay_urls->len; i++) {
    ctx->urls[i] = g_strdup(g_ptr_array_index(relay_urls, i));
    url_ptrs[i] = ctx->urls[i];
  }

  /* nostrc-ns2k: Log relay list at warning level for diagnostics */
  g_warning("nip66 streaming: querying %zu relays for kind 30166", ctx->url_count);
  for (guint i = 0; i < relay_urls->len; i++) {
    g_warning("nip66 streaming: relay[%u] = %s", i, (const gchar *)g_ptr_array_index(relay_urls, i));
  }

  /* Build filter - single filter for query_single_streaming */
  ctx->filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_NIP66_KIND_RELAY_META };
  nostr_filter_set_kinds(ctx->filter, kinds, 1);
  nostr_filter_set_limit(ctx->filter, 500);

  /* hq-r248b: No streaming signal in GNostrPool - events processed in completion callback.
   * nostrc-ns2k: Pool timeout (10s) governs query duration instead of a separate timer. */

  /* Query relays - results processed in on_streaming_query_complete (hq-r248b) */
  gnostr_pool_sync_relays(ctx->pool, (const gchar **)url_ptrs, ctx->url_count);
  {
    /* nostrc-ns2k: Use unique key per query to avoid freeing filters still in use
     * by a concurrent query thread (use-after-free on overlapping fetches). */
    static gint _qf_counter_n66s = 0;
    int _qfid = g_atomic_int_add(&_qf_counter_n66s, 1);
    char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-n66s-%d", _qfid);
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, ctx->filter);
    g_object_set_data_full(G_OBJECT(ctx->pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
    gnostr_pool_query_async(ctx->pool, _qf, ctx->cancellable, on_streaming_query_complete, ctx);
  }

  g_free(url_ptrs);
  g_ptr_array_unref(relay_urls);
}

#endif /* GNOSTR_NIP66_TEST_ONLY */
