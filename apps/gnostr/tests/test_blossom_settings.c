/**
 * @file test_blossom_settings.c
 * @brief Unit tests for Blossom settings and kind 10063 event handling
 *
 * Tests cover:
 * - Kind 10063 event parsing (server list from event)
 * - Kind 10063 event generation (settings to event)
 * - Server list management (add, remove, get)
 * - Default server handling
 */

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

/* Include the header we're testing */
#include "../src/util/blossom_settings.h"

/* Kind constant for user server list */
#define NOSTR_KIND_USER_SERVER_LIST 10063

/* ============================================================================
 * Kind 10063 Event Parsing Tests
 * ============================================================================ */

static void
test_parse_kind_10063_basic(void)
{
  const char *event_json = "{"
    "\"kind\": 10063,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": ["
      "[\"server\", \"https://blossom1.example.com\"],"
      "[\"server\", \"https://blossom2.example.com\"],"
      "[\"server\", \"https://backup.example.com\"]"
    "]"
  "}";

  gboolean result = gnostr_blossom_settings_from_event(event_json);
  g_assert_true(result);

  /* Get servers and verify */
  gsize count = 0;
  GnostrBlossomServer **servers = gnostr_blossom_settings_get_servers(&count);
  g_assert_nonnull(servers);
  g_assert_cmpuint(count, ==, 3);

  g_assert_cmpstr(servers[0]->url, ==, "https://blossom1.example.com");
  g_assert_cmpstr(servers[1]->url, ==, "https://blossom2.example.com");
  g_assert_cmpstr(servers[2]->url, ==, "https://backup.example.com");

  gnostr_blossom_servers_free(servers, count);
}

static void
test_parse_kind_10063_empty_tags(void)
{
  const char *event_json = "{"
    "\"kind\": 10063,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": []"
  "}";

  gboolean result = gnostr_blossom_settings_from_event(event_json);
  /* Should succeed but have empty server list (or fall back to default) */
  g_assert_true(result);
}

static void
test_parse_kind_10063_wrong_kind(void)
{
  /* Event with wrong kind should be rejected */
  const char *event_json = "{"
    "\"kind\": 10002,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": [[\"server\", \"https://test.com\"]]"
  "}";

  /* Expect a warning about wrong kind */
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*not kind 10063*");
  gboolean result = gnostr_blossom_settings_from_event(event_json);
  g_test_assert_expected_messages();
  g_assert_false(result);
}

static void
test_parse_kind_10063_invalid_json(void)
{
  /* Expect a warning about parse failure */
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*Failed to parse*");
  gboolean result = gnostr_blossom_settings_from_event("not valid json");
  g_test_assert_expected_messages();
  g_assert_false(result);
}

static void
test_parse_kind_10063_null_input(void)
{
  gboolean result = gnostr_blossom_settings_from_event(NULL);
  g_assert_false(result);
}

static void
test_parse_kind_10063_with_other_tags(void)
{
  /* Event with mixed tags - should only pick up server tags */
  const char *event_json = "{"
    "\"kind\": 10063,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": ["
      "[\"server\", \"https://blossom.example.com\"],"
      "[\"r\", \"wss://relay.example.com\"],"
      "[\"d\", \"some-identifier\"],"
      "[\"server\", \"https://backup.example.com\"]"
    "]"
  "}";

  gboolean result = gnostr_blossom_settings_from_event(event_json);
  g_assert_true(result);

  gsize count = 0;
  GnostrBlossomServer **servers = gnostr_blossom_settings_get_servers(&count);
  g_assert_nonnull(servers);
  g_assert_cmpuint(count, ==, 2);

  g_assert_cmpstr(servers[0]->url, ==, "https://blossom.example.com");
  g_assert_cmpstr(servers[1]->url, ==, "https://backup.example.com");

  gnostr_blossom_servers_free(servers, count);
}

/* ============================================================================
 * Kind 10063 Event Generation Tests
 * ============================================================================ */

static void
test_generate_kind_10063_event(void)
{
  /* First set up some servers */
  const char *setup_json = "{"
    "\"kind\": 10063,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": ["
      "[\"server\", \"https://primary.example.com\"],"
      "[\"server\", \"https://secondary.example.com\"]"
    "]"
  "}";

  gnostr_blossom_settings_from_event(setup_json);

  /* Now generate event */
  char *event_json = gnostr_blossom_settings_to_event();
  g_assert_nonnull(event_json);

  /* Parse and verify */
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  gboolean parsed = json_parser_load_from_data(parser, event_json, -1, &error);
  g_assert_no_error(error);
  g_assert_true(parsed);

  JsonNode *root = json_parser_get_root(parser);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(root));

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind */
  g_assert_cmpint(json_object_get_int_member(obj, "kind"), ==, NOSTR_KIND_USER_SERVER_LIST);

  /* Verify content is empty */
  g_assert_cmpstr(json_object_get_string_member(obj, "content"), ==, "");

  /* Verify tags contain our servers */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  g_assert_nonnull(tags);

  guint server_count = 0;
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (json_array_get_length(tag) >= 2) {
      const char *tag_name = json_array_get_string_element(tag, 0);
      if (g_strcmp0(tag_name, "server") == 0) {
        server_count++;
      }
    }
  }
  g_assert_cmpuint(server_count, ==, 2);

  g_object_unref(parser);
  g_free(event_json);
}

static void
test_event_roundtrip(void)
{
  /* Test that parsing and generating produces consistent results */
  const char *original_json = "{"
    "\"kind\": 10063,"
    "\"created_at\": 1700000000,"
    "\"content\": \"\","
    "\"tags\": ["
      "[\"server\", \"https://server1.com\"],"
      "[\"server\", \"https://server2.com\"],"
      "[\"server\", \"https://server3.com\"]"
    "]"
  "}";

  /* Parse original */
  gboolean result = gnostr_blossom_settings_from_event(original_json);
  g_assert_true(result);

  /* Generate new event */
  char *generated = gnostr_blossom_settings_to_event();
  g_assert_nonnull(generated);

  /* Parse generated event back and compare servers */
  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, generated, -1, NULL);
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  JsonArray *tags = json_object_get_array_member(obj, "tags");

  /* Extract server URLs */
  GPtrArray *urls = g_ptr_array_new();
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (json_array_get_length(tag) >= 2) {
      const char *tag_name = json_array_get_string_element(tag, 0);
      if (g_strcmp0(tag_name, "server") == 0) {
        g_ptr_array_add(urls, (gpointer)json_array_get_string_element(tag, 1));
      }
    }
  }

  /* Verify all three servers are present */
  g_assert_cmpuint(urls->len, ==, 3);

  gboolean has_server1 = FALSE, has_server2 = FALSE, has_server3 = FALSE;
  for (guint i = 0; i < urls->len; i++) {
    const char *url = g_ptr_array_index(urls, i);
    if (g_strcmp0(url, "https://server1.com") == 0) has_server1 = TRUE;
    if (g_strcmp0(url, "https://server2.com") == 0) has_server2 = TRUE;
    if (g_strcmp0(url, "https://server3.com") == 0) has_server3 = TRUE;
  }

  g_assert_true(has_server1);
  g_assert_true(has_server2);
  g_assert_true(has_server3);

  g_ptr_array_free(urls, TRUE);
  g_object_unref(parser);
  g_free(generated);
}

/* ============================================================================
 * Default Server Tests
 * ============================================================================ */

static void
test_default_server_constant(void)
{
  /* Verify the default server constant is reasonable */
  g_assert_nonnull(GNOSTR_BLOSSOM_DEFAULT_SERVER);
  g_assert_true(g_str_has_prefix(GNOSTR_BLOSSOM_DEFAULT_SERVER, "https://"));
}

static void
test_get_default_server_fallback(void)
{
  /* When no server is configured, should return the constant default */
  const char *server = gnostr_blossom_settings_get_default_server();
  g_assert_nonnull(server);
  g_assert_true(g_str_has_prefix(server, "https://"));
}

/* ============================================================================
 * Server Free Tests
 * ============================================================================ */

static void
test_server_free_null(void)
{
  /* Should not crash */
  gnostr_blossom_server_free(NULL);
}

static void
test_servers_free_empty(void)
{
  /* Should handle empty array */
  GnostrBlossomServer **empty = g_new0(GnostrBlossomServer *, 1);
  empty[0] = NULL;
  gnostr_blossom_servers_free(empty, 0);
}

static void
test_servers_free_null(void)
{
  /* Should not crash */
  gnostr_blossom_servers_free(NULL, 0);
  gnostr_blossom_servers_free(NULL, 5);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  /* Kind 10063 parsing tests */
  g_test_add_func("/blossom_settings/parse_10063/basic", test_parse_kind_10063_basic);
  g_test_add_func("/blossom_settings/parse_10063/empty_tags", test_parse_kind_10063_empty_tags);
  g_test_add_func("/blossom_settings/parse_10063/wrong_kind", test_parse_kind_10063_wrong_kind);
  g_test_add_func("/blossom_settings/parse_10063/invalid_json", test_parse_kind_10063_invalid_json);
  g_test_add_func("/blossom_settings/parse_10063/null_input", test_parse_kind_10063_null_input);
  g_test_add_func("/blossom_settings/parse_10063/mixed_tags", test_parse_kind_10063_with_other_tags);

  /* Kind 10063 generation tests */
  g_test_add_func("/blossom_settings/generate_10063/basic", test_generate_kind_10063_event);
  g_test_add_func("/blossom_settings/roundtrip", test_event_roundtrip);

  /* Default server tests */
  g_test_add_func("/blossom_settings/default/constant", test_default_server_constant);
  g_test_add_func("/blossom_settings/default/fallback", test_get_default_server_fallback);

  /* Memory management tests */
  g_test_add_func("/blossom_settings/free/server_null", test_server_free_null);
  g_test_add_func("/blossom_settings/free/servers_empty", test_servers_free_empty);
  g_test_add_func("/blossom_settings/free/servers_null", test_servers_free_null);

  return g_test_run();
}
