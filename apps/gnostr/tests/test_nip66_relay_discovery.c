/**
 * NIP-66 Relay Discovery E2E Tests
 *
 * Tests the NIP-66 relay discovery system including:
 * - Parsing kind 30166 relay metadata events
 * - Parsing kind 10166 relay monitor events
 * - Cache management (add, get, filter)
 * - Filter functionality
 * - Format helpers
 */

#define GNOSTR_NIP66_TEST_ONLY 1

#include <glib.h>
#include <string.h>
#include "util/nip66_relay_discovery.h"

/* ============== Test Fixtures ============== */

/* Sample kind 30166 relay metadata event with full tag set */
static const gchar *SAMPLE_RELAY_META_FULL = "{"
  "\"id\":\"abc123\","
  "\"pubkey\":\"472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":["
    "[\"d\",\"wss://relay.damus.io\"],"
    "[\"n\",\"clearnet\"],"
    "[\"N\",\"1\"],"
    "[\"N\",\"4\"],"
    "[\"N\",\"11\"],"
    "[\"N\",\"42\"],"
    "[\"g\",\"u4pruydqqvj\"],"
    "[\"G\",\"US\"],"
    "[\"l\",\"online\"],"
    "[\"rtt\",\"open\",\"45\"],"
    "[\"rtt\",\"read\",\"12\"],"
    "[\"rtt\",\"write\",\"18\"],"
    "[\"t\",\"fast\"],"
    "[\"t\",\"reliable\"]"
  "],"
  "\"content\":\"{\\\"name\\\":\\\"Damus Relay\\\",\\\"description\\\":\\\"A fast, reliable relay\\\",\\\"pubkey\\\":\\\"abc\\\",\\\"contact\\\":\\\"admin@damus.io\\\",\\\"software\\\":\\\"strfry\\\",\\\"version\\\":\\\"1.0.0\\\"}\","
  "\"sig\":\"fakesig\""
"}";

/* Minimal relay metadata event */
static const gchar *SAMPLE_RELAY_META_MINIMAL = "{"
  "\"id\":\"def456\","
  "\"pubkey\":\"d35e8b4ac79a66a4c47ef2f35a8b5057c5d72f1094c83c0ebf9c5d1eb1f9b9ff\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":["
    "[\"d\",\"wss://nos.lol\"]"
  "],"
  "\"content\":\"\","
  "\"sig\":\"fakesig\""
"}";

/* Relay with offline status */
static const gchar *SAMPLE_RELAY_META_OFFLINE = "{"
  "\"id\":\"ghi789\","
  "\"pubkey\":\"472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":["
    "[\"d\",\"wss://offline.relay\"],"
    "[\"l\",\"offline\"],"
    "[\"G\",\"DE\"]"
  "],"
  "\"content\":\"\","
  "\"sig\":\"fakesig\""
"}";

/* Relay with payment required */
static const gchar *SAMPLE_RELAY_META_PAID = "{"
  "\"id\":\"jkl012\","
  "\"pubkey\":\"472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":["
    "[\"d\",\"wss://paid.relay\"],"
    "[\"l\",\"online\"],"
    "[\"G\",\"JP\"],"
    "[\"N\",\"1\"],"
    "[\"N\",\"42\"]"
  "],"
  "\"content\":\"{\\\"limitation\\\":{\\\"payment_required\\\":true}}\","
  "\"sig\":\"fakesig\""
"}";

/* Tor relay */
static const gchar *SAMPLE_RELAY_META_TOR = "{"
  "\"id\":\"mno345\","
  "\"pubkey\":\"472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":["
    "[\"d\",\"ws://abcdef.onion\"],"
    "[\"n\",\"tor\"],"
    "[\"l\",\"online\"]"
  "],"
  "\"content\":\"\","
  "\"sig\":\"fakesig\""
"}";

/* Sample kind 10166 relay monitor event */
static const gchar *SAMPLE_MONITOR = "{"
  "\"id\":\"mon123\","
  "\"pubkey\":\"472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244\","
  "\"created_at\":1704067200,"
  "\"kind\":10166,"
  "\"tags\":["
    "[\"d\",\"nostr-watch\"],"
    "[\"r\",\"wss://relay.nostr.watch\"],"
    "[\"r\",\"wss://history.nostr.watch\"],"
    "[\"frequency\",\"15m\"],"
    "[\"c\",\"admin@nostr.watch\"]"
  "],"
  "\"content\":\"{\\\"name\\\":\\\"nostr.watch\\\",\\\"description\\\":\\\"Global relay monitor\\\"}\","
  "\"sig\":\"fakesig\""
"}";

/* Invalid events for error handling tests */
static const gchar *INVALID_JSON = "not valid json at all";
static const gchar *WRONG_KIND = "{"
  "\"id\":\"wrong\","
  "\"pubkey\":\"abc\","
  "\"created_at\":1704067200,"
  "\"kind\":1,"
  "\"tags\":[],"
  "\"content\":\"\","
  "\"sig\":\"fakesig\""
"}";
static const gchar *MISSING_D_TAG = "{"
  "\"id\":\"nodtag\","
  "\"pubkey\":\"abc\","
  "\"created_at\":1704067200,"
  "\"kind\":30166,"
  "\"tags\":[[\"n\",\"clearnet\"]],"
  "\"content\":\"\","
  "\"sig\":\"fakesig\""
"}";

/* ============== Parsing Tests ============== */

static void test_parse_relay_meta_full(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL);

  g_assert_nonnull(meta);
  g_assert_cmpstr(meta->relay_url, ==, "wss://relay.damus.io");
  g_assert_cmpstr(meta->d_tag, ==, "wss://relay.damus.io");
  g_assert_cmpstr(meta->event_id_hex, ==, "abc123");
  g_assert_cmpstr(meta->pubkey_hex, ==, "472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244");

  /* Network type */
  g_assert_cmpint(meta->network, ==, GNOSTR_NIP66_NETWORK_CLEARNET);

  /* Supported NIPs */
  g_assert_cmpuint(meta->supported_nips_count, ==, 4);
  g_assert_cmpint(meta->supported_nips[0], ==, 1);
  g_assert_cmpint(meta->supported_nips[1], ==, 4);
  g_assert_cmpint(meta->supported_nips[2], ==, 11);
  g_assert_cmpint(meta->supported_nips[3], ==, 42);

  /* Country/region from G tag */
  g_assert_cmpstr(meta->country_code, ==, "US");
  g_assert_nonnull(meta->region);  /* Should be mapped to "North America" */

  /* Online status */
  g_assert_true(meta->has_status);
  g_assert_true(meta->is_online);

  /* Latency from rtt tags */
  g_assert_cmpint(meta->latency_open_ms, ==, 45);
  g_assert_cmpint(meta->latency_read_ms, ==, 12);
  g_assert_cmpint(meta->latency_write_ms, ==, 18);

  /* Content fields */
  g_assert_cmpstr(meta->name, ==, "Damus Relay");
  g_assert_cmpstr(meta->description, ==, "A fast, reliable relay");
  g_assert_cmpstr(meta->contact, ==, "admin@damus.io");
  g_assert_cmpstr(meta->software, ==, "strfry");
  g_assert_cmpstr(meta->version, ==, "1.0.0");

  /* Tags */
  g_assert_cmpuint(meta->tags_count, ==, 2);
  g_assert_cmpstr(meta->tags[0], ==, "fast");
  g_assert_cmpstr(meta->tags[1], ==, "reliable");

  gnostr_nip66_relay_meta_free(meta);
}

static void test_parse_relay_meta_minimal(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_MINIMAL);

  g_assert_nonnull(meta);
  g_assert_cmpstr(meta->relay_url, ==, "wss://nos.lol");
  g_assert_cmpstr(meta->d_tag, ==, "wss://nos.lol");

  /* Network is inferred from URL when no n tag - wss:// = clearnet */
  g_assert_cmpint(meta->network, ==, GNOSTR_NIP66_NETWORK_CLEARNET);
  g_assert_cmpuint(meta->supported_nips_count, ==, 0);
  g_assert_null(meta->country_code);
  g_assert_false(meta->has_status);
  g_assert_null(meta->name);

  gnostr_nip66_relay_meta_free(meta);
}

static void test_parse_relay_meta_offline(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_OFFLINE);

  g_assert_nonnull(meta);
  g_assert_cmpstr(meta->relay_url, ==, "wss://offline.relay");
  g_assert_true(meta->has_status);
  g_assert_false(meta->is_online);
  g_assert_cmpstr(meta->country_code, ==, "DE");

  gnostr_nip66_relay_meta_free(meta);
}

static void test_parse_relay_meta_paid(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID);

  g_assert_nonnull(meta);
  g_assert_cmpstr(meta->relay_url, ==, "wss://paid.relay");
  g_assert_true(meta->payment_required);
  g_assert_cmpstr(meta->country_code, ==, "JP");

  gnostr_nip66_relay_meta_free(meta);
}

static void test_parse_relay_meta_tor(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_TOR);

  g_assert_nonnull(meta);
  g_assert_cmpstr(meta->relay_url, ==, "ws://abcdef.onion");
  g_assert_cmpint(meta->network, ==, GNOSTR_NIP66_NETWORK_TOR);

  gnostr_nip66_relay_meta_free(meta);
}

static void test_parse_relay_meta_invalid(void) {
  /* Invalid JSON should return NULL */
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(INVALID_JSON);
  g_assert_null(meta);

  /* Wrong kind should return NULL */
  meta = gnostr_nip66_parse_relay_meta(WRONG_KIND);
  g_assert_null(meta);

  /* Missing d tag should return NULL */
  meta = gnostr_nip66_parse_relay_meta(MISSING_D_TAG);
  g_assert_null(meta);

  /* NULL input should return NULL */
  meta = gnostr_nip66_parse_relay_meta(NULL);
  g_assert_null(meta);

  /* Empty string should return NULL */
  meta = gnostr_nip66_parse_relay_meta("");
  g_assert_null(meta);
}

static void test_parse_monitor(void) {
  GnostrNip66RelayMonitor *monitor = gnostr_nip66_parse_relay_monitor(SAMPLE_MONITOR);

  g_assert_nonnull(monitor);
  g_assert_cmpstr(monitor->event_id_hex, ==, "mon123");
  g_assert_cmpstr(monitor->pubkey_hex, ==, "472a3c602c881f871ff5034e53c8353a4a52a64dd1b7d8b7d4d8d76e0be8a244");
  g_assert_cmpstr(monitor->name, ==, "nostr.watch");
  g_assert_cmpstr(monitor->description, ==, "Global relay monitor");
  g_assert_cmpstr(monitor->frequency, ==, "15m");

  /* Relay hints */
  g_assert_cmpuint(monitor->relay_hints_count, ==, 2);
  g_assert_cmpstr(monitor->relay_hints[0], ==, "wss://relay.nostr.watch");
  g_assert_cmpstr(monitor->relay_hints[1], ==, "wss://history.nostr.watch");

  gnostr_nip66_relay_monitor_free(monitor);
}

static void test_parse_monitor_invalid(void) {
  /* Invalid JSON */
  GnostrNip66RelayMonitor *monitor = gnostr_nip66_parse_relay_monitor(INVALID_JSON);
  g_assert_null(monitor);

  /* Wrong kind */
  monitor = gnostr_nip66_parse_relay_monitor(WRONG_KIND);
  g_assert_null(monitor);

  /* NULL/empty */
  monitor = gnostr_nip66_parse_relay_monitor(NULL);
  g_assert_null(monitor);
  monitor = gnostr_nip66_parse_relay_monitor("");
  g_assert_null(monitor);
}

/* ============== Network Parsing Tests ============== */

static void test_parse_network(void) {
  g_assert_cmpint(gnostr_nip66_parse_network("clearnet"), ==, GNOSTR_NIP66_NETWORK_CLEARNET);
  g_assert_cmpint(gnostr_nip66_parse_network("tor"), ==, GNOSTR_NIP66_NETWORK_TOR);
  g_assert_cmpint(gnostr_nip66_parse_network("i2p"), ==, GNOSTR_NIP66_NETWORK_I2P);
  g_assert_cmpint(gnostr_nip66_parse_network("unknown"), ==, GNOSTR_NIP66_NETWORK_UNKNOWN);
  g_assert_cmpint(gnostr_nip66_parse_network(NULL), ==, GNOSTR_NIP66_NETWORK_UNKNOWN);
  g_assert_cmpint(gnostr_nip66_parse_network(""), ==, GNOSTR_NIP66_NETWORK_UNKNOWN);

  /* Case insensitive */
  g_assert_cmpint(gnostr_nip66_parse_network("CLEARNET"), ==, GNOSTR_NIP66_NETWORK_CLEARNET);
  g_assert_cmpint(gnostr_nip66_parse_network("TOR"), ==, GNOSTR_NIP66_NETWORK_TOR);
}

/* ============== Cache Tests ============== */

static void test_cache_basic(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  /* Add a relay */
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL);
  g_assert_nonnull(meta);
  gnostr_nip66_cache_add_relay(meta);

  /* Retrieve it */
  GnostrNip66RelayMeta *cached = gnostr_nip66_cache_get_relay("wss://relay.damus.io");
  g_assert_nonnull(cached);
  g_assert_cmpstr(cached->relay_url, ==, "wss://relay.damus.io");
  g_assert_cmpstr(cached->name, ==, "Damus Relay");

  /* Non-existent relay */
  g_assert_null(gnostr_nip66_cache_get_relay("wss://nonexistent.relay"));

  /* Get all relays */
  GPtrArray *all = gnostr_nip66_cache_get_all_relays();
  g_assert_nonnull(all);
  g_assert_cmpuint(all->len, ==, 1);
  g_ptr_array_unref(all);

  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_cache_multiple_relays(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  /* Add multiple relays */
  GnostrNip66RelayMeta *meta1 = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL);
  GnostrNip66RelayMeta *meta2 = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_MINIMAL);
  GnostrNip66RelayMeta *meta3 = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_OFFLINE);
  GnostrNip66RelayMeta *meta4 = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID);

  gnostr_nip66_cache_add_relay(meta1);
  gnostr_nip66_cache_add_relay(meta2);
  gnostr_nip66_cache_add_relay(meta3);
  gnostr_nip66_cache_add_relay(meta4);

  /* Verify all are cached */
  GPtrArray *all = gnostr_nip66_cache_get_all_relays();
  g_assert_cmpuint(all->len, ==, 4);
  g_ptr_array_unref(all);

  /* Verify individual retrieval */
  g_assert_nonnull(gnostr_nip66_cache_get_relay("wss://relay.damus.io"));
  g_assert_nonnull(gnostr_nip66_cache_get_relay("wss://nos.lol"));
  g_assert_nonnull(gnostr_nip66_cache_get_relay("wss://offline.relay"));
  g_assert_nonnull(gnostr_nip66_cache_get_relay("wss://paid.relay"));

  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_cache_monitor(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  GnostrNip66RelayMonitor *monitor = gnostr_nip66_parse_relay_monitor(SAMPLE_MONITOR);
  g_assert_nonnull(monitor);
  gnostr_nip66_cache_add_monitor(monitor);

  GPtrArray *monitors = gnostr_nip66_cache_get_all_monitors();
  g_assert_nonnull(monitors);
  g_assert_cmpuint(monitors->len, ==, 1);
  g_ptr_array_unref(monitors);

  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

/* ============== Filter Tests ============== */

static void test_supports_nip(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL);
  g_assert_nonnull(meta);

  g_assert_true(gnostr_nip66_relay_supports_nip(meta, 1));
  g_assert_true(gnostr_nip66_relay_supports_nip(meta, 4));
  g_assert_true(gnostr_nip66_relay_supports_nip(meta, 11));
  g_assert_true(gnostr_nip66_relay_supports_nip(meta, 42));
  g_assert_false(gnostr_nip66_relay_supports_nip(meta, 99));
  g_assert_false(gnostr_nip66_relay_supports_nip(meta, 0));

  /* NULL meta should return FALSE */
  g_assert_false(gnostr_nip66_relay_supports_nip(NULL, 1));

  gnostr_nip66_relay_meta_free(meta);
}

static void test_filter_online_only(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  /* Add relays with different online status */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));      /* online */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_MINIMAL));   /* unknown status */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_OFFLINE));   /* offline */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID));      /* online */

  GnostrNip66RelayFilter filter = {0};
  filter.flags = GNOSTR_NIP66_FILTER_ONLINE_ONLY;

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);

  /* Should include online and unknown status, exclude explicit offline */
  /* Expected: relay.damus.io (online), nos.lol (unknown), paid.relay (online) = 3 */
  g_assert_cmpuint(results->len, ==, 3);

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_filter_free_only(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));      /* free */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID));      /* paid */

  GnostrNip66RelayFilter filter = {0};
  filter.flags = GNOSTR_NIP66_FILTER_FREE_ONLY;

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 1);

  GnostrNip66RelayMeta *meta = g_ptr_array_index(results, 0);
  g_assert_cmpstr(meta->relay_url, ==, "wss://relay.damus.io");

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_filter_clearnet_only(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));  /* clearnet */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_TOR));   /* tor */

  GnostrNip66RelayFilter filter = {0};
  filter.flags = GNOSTR_NIP66_FILTER_CLEARNET_ONLY;

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 1);

  GnostrNip66RelayMeta *meta = g_ptr_array_index(results, 0);
  g_assert_cmpint(meta->network, ==, GNOSTR_NIP66_NETWORK_CLEARNET);

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_filter_by_nip(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));      /* supports 1,4,11,42 */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID));      /* supports 1,42 */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_MINIMAL));   /* no NIPs listed */

  GnostrNip66RelayFilter filter = {0};
  gint required_nips[] = {42};
  filter.required_nips = required_nips;
  filter.required_nips_count = 1;

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 2);  /* relay.damus.io and paid.relay both support NIP-42 */

  g_ptr_array_unref(results);

  /* Filter by NIP-11 (only relay.damus.io) */
  gint nip11[] = {11};
  filter.required_nips = nip11;

  results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 1);

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_filter_by_region(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));      /* US -> North America */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_OFFLINE));   /* DE -> Europe */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID));      /* JP -> Asia Pacific */

  GnostrNip66RelayFilter filter = {0};
  filter.region = "North America";

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 1);

  GnostrNip66RelayMeta *meta = g_ptr_array_index(results, 0);
  g_assert_cmpstr(meta->relay_url, ==, "wss://relay.damus.io");

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

static void test_filter_combined(void) {
  gnostr_nip66_cache_init();
  gnostr_nip66_cache_clear();

  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL));      /* online, free, US */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_OFFLINE));   /* offline, free, DE */
  gnostr_nip66_cache_add_relay(gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_PAID));      /* online, paid, JP */

  GnostrNip66RelayFilter filter = {0};
  filter.flags = GNOSTR_NIP66_FILTER_ONLINE_ONLY | GNOSTR_NIP66_FILTER_FREE_ONLY;

  GPtrArray *results = gnostr_nip66_filter_relays(&filter);
  g_assert_nonnull(results);
  g_assert_cmpuint(results->len, ==, 1);

  GnostrNip66RelayMeta *meta = g_ptr_array_index(results, 0);
  g_assert_cmpstr(meta->relay_url, ==, "wss://relay.damus.io");

  g_ptr_array_unref(results);
  gnostr_nip66_cache_clear();
  gnostr_nip66_cache_shutdown();
}

/* ============== Format Helper Tests ============== */

static void test_format_uptime(void) {
  g_autofree gchar *s1 = gnostr_nip66_format_uptime(99.9);
  g_assert_nonnull(s1);
  g_assert_true(g_str_has_suffix(s1, "%"));

  g_autofree gchar *s2 = gnostr_nip66_format_uptime(0.0);
  g_assert_nonnull(s2);

  g_autofree gchar *s3 = gnostr_nip66_format_uptime(100.0);
  g_assert_nonnull(s3);
}

static void test_format_latency(void) {
  g_autofree gchar *s1 = gnostr_nip66_format_latency(45);
  g_assert_nonnull(s1);
  g_assert_true(strstr(s1, "ms") != NULL);

  g_autofree gchar *s2 = gnostr_nip66_format_latency(1500);
  g_assert_nonnull(s2);
  /* High latency might be formatted differently (seconds) */

  g_autofree gchar *s3 = gnostr_nip66_format_latency(0);
  g_assert_nonnull(s3);
}

static void test_format_last_seen(void) {
  /* Recent timestamp */
  gint64 recent = g_get_real_time() / G_USEC_PER_SEC - 60;  /* 1 minute ago */
  g_autofree gchar *s1 = gnostr_nip66_format_last_seen(recent);
  g_assert_nonnull(s1);

  /* Old timestamp */
  gint64 old = g_get_real_time() / G_USEC_PER_SEC - (60 * 60 * 24);  /* 1 day ago */
  g_autofree gchar *s2 = gnostr_nip66_format_last_seen(old);
  g_assert_nonnull(s2);

  /* Zero timestamp */
  g_autofree gchar *s3 = gnostr_nip66_format_last_seen(0);
  g_assert_nonnull(s3);
}

static void test_format_nips(void) {
  GnostrNip66RelayMeta *meta = gnostr_nip66_parse_relay_meta(SAMPLE_RELAY_META_FULL);
  g_assert_nonnull(meta);

  g_autofree gchar *nips = gnostr_nip66_format_nips(meta);
  g_assert_nonnull(nips);
  g_assert_true(strstr(nips, "1") != NULL);
  g_assert_true(strstr(nips, "42") != NULL);

  gnostr_nip66_relay_meta_free(meta);

  /* NULL meta */
  g_autofree gchar *nips_null = gnostr_nip66_format_nips(NULL);
  g_assert_nonnull(nips_null);  /* Should return empty or "None" */
}

/* ============== Region Mapping Tests ============== */

static void test_region_for_country(void) {
  /* North America */
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("US"), ==, "North America");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("CA"), ==, "North America");

  /* Europe */
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("DE"), ==, "Europe");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("FR"), ==, "Europe");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("GB"), ==, "Europe");

  /* Asia Pacific */
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("JP"), ==, "Asia Pacific");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("AU"), ==, "Asia Pacific");

  /* Unknown/NULL - returns "Other" for unrecognized codes, "Unknown" for invalid input */
  g_assert_cmpstr(gnostr_nip66_get_region_for_country("XX"), ==, "Other");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country(NULL), ==, "Unknown");
  g_assert_cmpstr(gnostr_nip66_get_region_for_country(""), ==, "Unknown");
}

/* ============== Main ============== */

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  /* Parsing tests */
  g_test_add_func("/nip66/parse/relay_meta_full", test_parse_relay_meta_full);
  g_test_add_func("/nip66/parse/relay_meta_minimal", test_parse_relay_meta_minimal);
  g_test_add_func("/nip66/parse/relay_meta_offline", test_parse_relay_meta_offline);
  g_test_add_func("/nip66/parse/relay_meta_paid", test_parse_relay_meta_paid);
  g_test_add_func("/nip66/parse/relay_meta_tor", test_parse_relay_meta_tor);
  g_test_add_func("/nip66/parse/relay_meta_invalid", test_parse_relay_meta_invalid);
  g_test_add_func("/nip66/parse/monitor", test_parse_monitor);
  g_test_add_func("/nip66/parse/monitor_invalid", test_parse_monitor_invalid);
  g_test_add_func("/nip66/parse/network", test_parse_network);

  /* Cache tests */
  g_test_add_func("/nip66/cache/basic", test_cache_basic);
  g_test_add_func("/nip66/cache/multiple_relays", test_cache_multiple_relays);
  g_test_add_func("/nip66/cache/monitor", test_cache_monitor);

  /* Filter tests */
  g_test_add_func("/nip66/filter/supports_nip", test_supports_nip);
  g_test_add_func("/nip66/filter/online_only", test_filter_online_only);
  g_test_add_func("/nip66/filter/free_only", test_filter_free_only);
  g_test_add_func("/nip66/filter/clearnet_only", test_filter_clearnet_only);
  g_test_add_func("/nip66/filter/by_nip", test_filter_by_nip);
  g_test_add_func("/nip66/filter/by_region", test_filter_by_region);
  g_test_add_func("/nip66/filter/combined", test_filter_combined);

  /* Format helper tests */
  g_test_add_func("/nip66/format/uptime", test_format_uptime);
  g_test_add_func("/nip66/format/latency", test_format_latency);
  g_test_add_func("/nip66/format/last_seen", test_format_last_seen);
  g_test_add_func("/nip66/format/nips", test_format_nips);

  /* Region mapping tests */
  g_test_add_func("/nip66/region/for_country", test_region_for_country);

  return g_test_run();
}
