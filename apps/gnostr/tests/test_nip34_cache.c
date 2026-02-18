/**
 * NIP-34 Cache Format Unit Tests
 *
 * Tests for the repository cache save/load format to ensure
 * cached repositories persist correctly across app restarts.
 *
 * These tests verify the fix for blank cards caused by a format
 * mismatch between save (simplified) and load (expected NIP-34 event).
 */

#include <glib.h>
#include <json-glib/json-glib.h>

/* Mock RepoInfo struct matching nip34-git-plugin.c */
typedef struct {
  char *id;
  char *d_tag;
  char *name;
  char *description;
  char *clone_url;
  char *web_url;
  char *pubkey;
  char **maintainers;
  char **relays;
  gint64 created_at;
  gint64 updated_at;
} MockRepoInfo;

static MockRepoInfo *mock_repo_info_new(const char *d_tag, const char *name)
{
  MockRepoInfo *info = g_new0(MockRepoInfo, 1);
  info->d_tag = g_strdup(d_tag);
  info->name = g_strdup(name);
  return info;
}

static void mock_repo_info_free(MockRepoInfo *info)
{
  if (!info) return;
  g_free(info->id);
  g_free(info->d_tag);
  g_free(info->name);
  g_free(info->description);
  g_free(info->clone_url);
  g_free(info->web_url);
  g_free(info->pubkey);
  g_strfreev(info->maintainers);
  g_strfreev(info->relays);
  g_free(info);
}

/* Build cache JSON using the same format as save_cached_repositories */
static char *build_cache_json(GPtrArray *repos)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  for (guint i = 0; i < repos->len; i++)
    {
      MockRepoInfo *info = g_ptr_array_index(repos, i);
      if (!info) continue;

      json_builder_begin_object(builder);
      if (info->id)
        {
          json_builder_set_member_name(builder, "id");
          json_builder_add_string_value(builder, info->id);
        }
      if (info->d_tag)
        {
          json_builder_set_member_name(builder, "d_tag");
          json_builder_add_string_value(builder, info->d_tag);
        }
      if (info->name)
        {
          json_builder_set_member_name(builder, "name");
          json_builder_add_string_value(builder, info->name);
        }
      if (info->clone_url)
        {
          json_builder_set_member_name(builder, "clone_url");
          json_builder_add_string_value(builder, info->clone_url);
        }
      if (info->description)
        {
          json_builder_set_member_name(builder, "description");
          json_builder_add_string_value(builder, info->description);
        }
      if (info->web_url)
        {
          json_builder_set_member_name(builder, "web_url");
          json_builder_add_string_value(builder, info->web_url);
        }
      if (info->pubkey)
        {
          json_builder_set_member_name(builder, "pubkey");
          json_builder_add_string_value(builder, info->pubkey);
        }
      json_builder_end_object(builder);
    }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  char *json = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  return json;
}

/* Parse cache JSON using the same format as load_cached_repositories (FIXED version) */
static GPtrArray *parse_cache_json(const char *json)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL))
    {
      return repos;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root))
    {
      return repos;
    }

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);

  for (guint i = 0; i < len; i++)
    {
      JsonNode *node = json_array_get_element(arr, i);
      if (!JSON_NODE_HOLDS_OBJECT(node))
        continue;

      JsonObject *obj = json_node_get_object(node);

      /* Parse simplified cache format directly (the FIX) */
      MockRepoInfo *info = g_new0(MockRepoInfo, 1);

      if (json_object_has_member(obj, "id"))
        info->id = g_strdup(json_object_get_string_member(obj, "id"));
      if (json_object_has_member(obj, "d_tag"))
        info->d_tag = g_strdup(json_object_get_string_member(obj, "d_tag"));
      if (json_object_has_member(obj, "name"))
        info->name = g_strdup(json_object_get_string_member(obj, "name"));
      if (json_object_has_member(obj, "description"))
        info->description = g_strdup(json_object_get_string_member(obj, "description"));
      if (json_object_has_member(obj, "clone_url"))
        info->clone_url = g_strdup(json_object_get_string_member(obj, "clone_url"));
      if (json_object_has_member(obj, "web_url"))
        info->web_url = g_strdup(json_object_get_string_member(obj, "web_url"));
      if (json_object_has_member(obj, "pubkey"))
        info->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));

      if (info->d_tag)
        g_ptr_array_add(repos, info);
      else
        mock_repo_info_free(info);
    }

  return repos;
}

/* BROKEN: The old parse function that expected NIP-34 event format */
static GPtrArray *parse_cache_json_broken(const char *json)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL))
    {
      return repos;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root))
    {
      return repos;
    }

  JsonArray *arr = json_node_get_array(root);
  guint len = json_array_get_length(arr);

  for (guint i = 0; i < len; i++)
    {
      JsonNode *node = json_array_get_element(arr, i);
      if (!JSON_NODE_HOLDS_OBJECT(node))
        continue;

      JsonObject *obj = json_node_get_object(node);

      /* BROKEN: Expected NIP-34 event format with "tags" array */
      if (!json_object_has_member(obj, "tags"))
        {
          /* This was the bug - cache format doesn't have "tags" */
          continue;  /* Skips all cached repos! */
        }

      /* Would parse tags here, but we never get here */
      MockRepoInfo *info = g_new0(MockRepoInfo, 1);
      /* ... would populate from tags ... */
      g_ptr_array_add(repos, info);
    }

  return repos;
}

/* Test: Cache round-trip preserves all fields */
static void test_cache_roundtrip(void)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);

  MockRepoInfo *repo1 = mock_repo_info_new("test-repo-1", "Test Repository");
  repo1->id = g_strdup("abc123");
  repo1->description = g_strdup("A test repository for unit testing");
  repo1->clone_url = g_strdup("https://github.com/test/repo.git");
  repo1->web_url = g_strdup("https://github.com/test/repo");
  repo1->pubkey = g_strdup("npub1abc123");
  g_ptr_array_add(repos, repo1);

  MockRepoInfo *repo2 = mock_repo_info_new("another-repo", "Another Repo");
  repo2->clone_url = g_strdup("git://example.com/another.git");
  g_ptr_array_add(repos, repo2);

  /* Save to JSON */
  char *json = build_cache_json(repos);
  g_assert_nonnull(json);

  /* Load from JSON (using FIXED parser) */
  GPtrArray *loaded = parse_cache_json(json);
  g_assert_cmpuint(loaded->len, ==, 2);

  /* Verify first repo */
  MockRepoInfo *loaded1 = g_ptr_array_index(loaded, 0);
  g_assert_cmpstr(loaded1->d_tag, ==, "test-repo-1");
  g_assert_cmpstr(loaded1->name, ==, "Test Repository");
  g_assert_cmpstr(loaded1->id, ==, "abc123");
  g_assert_cmpstr(loaded1->description, ==, "A test repository for unit testing");
  g_assert_cmpstr(loaded1->clone_url, ==, "https://github.com/test/repo.git");
  g_assert_cmpstr(loaded1->web_url, ==, "https://github.com/test/repo");
  g_assert_cmpstr(loaded1->pubkey, ==, "npub1abc123");

  /* Verify second repo */
  MockRepoInfo *loaded2 = g_ptr_array_index(loaded, 1);
  g_assert_cmpstr(loaded2->d_tag, ==, "another-repo");
  g_assert_cmpstr(loaded2->name, ==, "Another Repo");
  g_assert_cmpstr(loaded2->clone_url, ==, "git://example.com/another.git");

  g_free(json);
  g_ptr_array_unref(repos);
  g_ptr_array_unref(loaded);
}

/* Test: Broken parser fails to load cache (demonstrates the bug) */
static void test_broken_parser_fails(void)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);

  MockRepoInfo *repo = mock_repo_info_new("my-repo", "My Repository");
  repo->description = g_strdup("Should not load with broken parser");
  g_ptr_array_add(repos, repo);

  char *json = build_cache_json(repos);

  /* Try to load with broken parser (expected NIP-34 event format) */
  GPtrArray *loaded = parse_cache_json_broken(json);

  /* BROKEN: Returns 0 repos because cache format lacks "tags" array */
  g_assert_cmpuint(loaded->len, ==, 0);

  g_free(json);
  g_ptr_array_unref(repos);
  g_ptr_array_unref(loaded);
}

/* Test: Fixed parser correctly loads cache */
static void test_fixed_parser_works(void)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);

  MockRepoInfo *repo = mock_repo_info_new("my-repo", "My Repository");
  repo->description = g_strdup("Should load correctly with fixed parser");
  g_ptr_array_add(repos, repo);

  char *json = build_cache_json(repos);

  /* Load with fixed parser (handles simplified format) */
  GPtrArray *loaded = parse_cache_json(json);

  /* FIXED: Returns 1 repo with correct data */
  g_assert_cmpuint(loaded->len, ==, 1);

  MockRepoInfo *loaded_repo = g_ptr_array_index(loaded, 0);
  g_assert_cmpstr(loaded_repo->d_tag, ==, "my-repo");
  g_assert_cmpstr(loaded_repo->name, ==, "My Repository");
  g_assert_cmpstr(loaded_repo->description, ==, "Should load correctly with fixed parser");

  g_free(json);
  g_ptr_array_unref(repos);
  g_ptr_array_unref(loaded);
}

/* Test: Empty cache loads correctly */
static void test_empty_cache(void)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func((GDestroyNotify)mock_repo_info_free);
  char *json = build_cache_json(repos);

  g_assert_cmpstr(json, ==, "[]");

  GPtrArray *loaded = parse_cache_json(json);
  g_assert_cmpuint(loaded->len, ==, 0);

  g_free(json);
  g_ptr_array_unref(repos);
  g_ptr_array_unref(loaded);
}

/* Test: Repos without d_tag are skipped */
static void test_missing_d_tag_skipped(void)
{
  const char *json = "[{\"name\": \"No D-Tag Repo\", \"description\": \"Missing required field\"}]";

  GPtrArray *loaded = parse_cache_json(json);

  /* Should skip repo without d_tag */
  g_assert_cmpuint(loaded->len, ==, 0);

  g_ptr_array_unref(loaded);
}

int main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/nip34_cache/roundtrip", test_cache_roundtrip);
  g_test_add_func("/nip34_cache/broken_parser_fails", test_broken_parser_fails);
  g_test_add_func("/nip34_cache/fixed_parser_works", test_fixed_parser_works);
  g_test_add_func("/nip34_cache/empty_cache", test_empty_cache);
  g_test_add_func("/nip34_cache/missing_d_tag_skipped", test_missing_d_tag_skipped);

  return g_test_run();
}
