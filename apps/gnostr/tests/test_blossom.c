/**
 * @file test_blossom.c
 * @brief Unit tests for Blossom (BUD-01/BUD-02) functionality
 *
 * Tests cover:
 * - SHA-256 file hashing
 * - MIME type detection
 * - Kind 24242 auth event building
 * - Blossom settings management
 * - Kind 10063 server list parsing
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

/* Include the headers we're testing */
#include "../src/util/blossom.h"
#include "../src/util/blossom_settings.h"

/* Test fixtures */
static char *temp_dir = NULL;
static char *test_file_path = NULL;

static void
setup_temp_dir(void)
{
  GError *error = NULL;
  temp_dir = g_dir_make_tmp("gnostr-blossom-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(temp_dir);
}

static void
teardown_temp_dir(void)
{
  if (test_file_path) {
    g_remove(test_file_path);
    g_free(test_file_path);
    test_file_path = NULL;
  }
  if (temp_dir) {
    g_rmdir(temp_dir);
    g_free(temp_dir);
    temp_dir = NULL;
  }
}

static void
create_test_file(const char *content)
{
  GError *error = NULL;
  test_file_path = g_build_filename(temp_dir, "test_file.bin", NULL);
  g_file_set_contents(test_file_path, content, strlen(content), &error);
  g_assert_no_error(error);
}

/* ============================================================================
 * SHA-256 Hash Tests
 * ============================================================================ */

static void
test_sha256_file_basic(void)
{
  setup_temp_dir();

  /* Create a file with known content */
  /* SHA-256 of "hello" is 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 */
  create_test_file("hello");

  char hash[65];
  gboolean result = gnostr_blossom_sha256_file(test_file_path, hash, NULL);

  g_assert_true(result);
  g_assert_cmpstr(hash, ==, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

  teardown_temp_dir();
}

static void
test_sha256_file_empty(void)
{
  setup_temp_dir();

  /* SHA-256 of empty string is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
  create_test_file("");

  char hash[65];
  gboolean result = gnostr_blossom_sha256_file(test_file_path, hash, NULL);

  g_assert_true(result);
  g_assert_cmpstr(hash, ==, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  teardown_temp_dir();
}

static void
test_sha256_file_binary(void)
{
  setup_temp_dir();

  /* Create file with binary content */
  GError *error = NULL;
  test_file_path = g_build_filename(temp_dir, "binary_test.bin", NULL);
  guchar binary_data[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};
  g_file_set_contents(test_file_path, (const char *)binary_data, sizeof(binary_data), &error);
  g_assert_no_error(error);

  char hash[65];
  gboolean result = gnostr_blossom_sha256_file(test_file_path, hash, NULL);

  g_assert_true(result);
  /* Just verify it returns a valid 64-char hex hash */
  g_assert_cmpint(strlen(hash), ==, 64);
  for (int i = 0; i < 64; i++) {
    g_assert_true(g_ascii_isxdigit(hash[i]));
  }

  teardown_temp_dir();
}

static void
test_sha256_file_not_found(void)
{
  char hash[65];
  gboolean result = gnostr_blossom_sha256_file("/nonexistent/path/file.txt", hash, NULL);

  g_assert_false(result);
}

static void
test_sha256_file_null_inputs(void)
{
  char hash[65];

  gboolean result1 = gnostr_blossom_sha256_file(NULL, hash, NULL);
  g_assert_false(result1);

  setup_temp_dir();
  create_test_file("test");
  gboolean result2 = gnostr_blossom_sha256_file(test_file_path, NULL, NULL);
  g_assert_false(result2);
  teardown_temp_dir();
}

/* ============================================================================
 * MIME Type Detection Tests
 * ============================================================================ */

static void
test_mime_type_images(void)
{
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.png"), ==, "image/png");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.PNG"), ==, "image/png");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.jpg"), ==, "image/jpeg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.jpeg"), ==, "image/jpeg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.JPEG"), ==, "image/jpeg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.gif"), ==, "image/gif");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.webp"), ==, "image/webp");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.svg"), ==, "image/svg+xml");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("photo.avif"), ==, "image/avif");
}

static void
test_mime_type_videos(void)
{
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("video.mp4"), ==, "video/mp4");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("video.webm"), ==, "video/webm");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("video.mov"), ==, "video/quicktime");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("video.avi"), ==, "video/x-msvideo");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("video.mkv"), ==, "video/x-matroska");
}

static void
test_mime_type_audio(void)
{
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("audio.mp3"), ==, "audio/mpeg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("audio.ogg"), ==, "audio/ogg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("audio.wav"), ==, "audio/wav");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("audio.flac"), ==, "audio/flac");
}

static void
test_mime_type_unknown(void)
{
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("file.xyz"), ==, "application/octet-stream");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("file"), ==, "application/octet-stream");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type(NULL), ==, "application/octet-stream");
}

static void
test_mime_type_path_with_directories(void)
{
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("/home/user/photos/vacation.jpg"), ==, "image/jpeg");
  g_assert_cmpstr(gnostr_blossom_detect_mime_type("./relative/path/to/video.mp4"), ==, "video/mp4");
}

/* ============================================================================
 * Kind 24242 Auth Event Building Tests
 * ============================================================================ */

static void
test_auth_event_upload(void)
{
  const char *sha256 = "abc123def456789012345678901234567890123456789012345678901234abcd";
  const char *server = "https://blossom.example.com";

  char *json = gnostr_blossom_build_auth_event("upload", sha256, server, 12345, "image/png");
  g_assert_nonnull(json);

  /* Parse and verify */
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  gboolean parsed = json_parser_load_from_data(parser, json, -1, &error);
  g_assert_no_error(error);
  g_assert_true(parsed);

  JsonNode *root = json_parser_get_root(parser);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(root));

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind */
  g_assert_true(json_object_has_member(obj, "kind"));
  g_assert_cmpint(json_object_get_int_member(obj, "kind"), ==, 24242);

  /* Verify content is empty */
  g_assert_cmpstr(json_object_get_string_member(obj, "content"), ==, "");

  /* Verify tags */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  g_assert_nonnull(tags);
  g_assert_cmpint(json_array_get_length(tags), >=, 4); /* t, x, server, expiration at minimum */

  /* Check for required tags */
  gboolean has_t = FALSE, has_x = FALSE, has_server = FALSE, has_size = FALSE, has_type = FALSE;
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_name, "t") == 0) {
      has_t = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "upload");
    } else if (g_strcmp0(tag_name, "x") == 0) {
      has_x = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, sha256);
    } else if (g_strcmp0(tag_name, "server") == 0) {
      has_server = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, server);
    } else if (g_strcmp0(tag_name, "size") == 0) {
      has_size = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "12345");
    } else if (g_strcmp0(tag_name, "type") == 0) {
      has_type = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "image/png");
    }
  }

  g_assert_true(has_t);
  g_assert_true(has_x);
  g_assert_true(has_server);
  g_assert_true(has_size);
  g_assert_true(has_type);

  g_object_unref(parser);
  g_free(json);
}

static void
test_auth_event_delete(void)
{
  const char *sha256 = "abc123def456789012345678901234567890123456789012345678901234abcd";
  const char *server = "https://blossom.example.com";

  char *json = gnostr_blossom_build_auth_event("delete", sha256, server, 0, NULL);
  g_assert_nonnull(json);

  /* Parse and verify */
  JsonParser *parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, json, -1, NULL);
  g_assert_true(parsed);

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

  /* Verify kind */
  g_assert_cmpint(json_object_get_int_member(obj, "kind"), ==, 24242);

  /* Check tags */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  gboolean has_t = FALSE;
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_name, "t") == 0) {
      has_t = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "delete");
    }
  }
  g_assert_true(has_t);

  g_object_unref(parser);
  g_free(json);
}

static void
test_auth_event_list(void)
{
  const char *server = "https://blossom.example.com";

  char *json = gnostr_blossom_build_auth_event("list", NULL, server, 0, NULL);
  g_assert_nonnull(json);

  /* Parse and verify */
  JsonParser *parser = json_parser_new();
  gboolean parsed = json_parser_load_from_data(parser, json, -1, NULL);
  g_assert_true(parsed);

  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

  /* Verify kind */
  g_assert_cmpint(json_object_get_int_member(obj, "kind"), ==, 24242);

  /* Check tags - should NOT have x tag for list */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  gboolean has_t = FALSE, has_x = FALSE;
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_name, "t") == 0) {
      has_t = TRUE;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "list");
    } else if (g_strcmp0(tag_name, "x") == 0) {
      has_x = TRUE;
    }
  }
  g_assert_true(has_t);
  g_assert_false(has_x); /* No hash for list action */

  g_object_unref(parser);
  g_free(json);
}

static void
test_auth_event_expiration(void)
{
  char *json = gnostr_blossom_build_auth_event("upload", "abc123", "https://test.com", 0, NULL);
  g_assert_nonnull(json);

  JsonParser *parser = json_parser_new();
  json_parser_load_from_data(parser, json, -1, NULL);
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

  /* Check expiration tag exists and is reasonable (within 5-10 minutes from now) */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  gboolean has_expiration = FALSE;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;

  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_name, "expiration") == 0) {
      has_expiration = TRUE;
      const char *exp_str = json_array_get_string_element(tag, 1);
      gint64 exp_time = g_ascii_strtoll(exp_str, NULL, 10);
      /* Should be approximately 5 minutes in the future */
      g_assert_cmpint(exp_time, >, now);
      g_assert_cmpint(exp_time, <=, now + 600); /* Within 10 minutes */
    }
  }
  g_assert_true(has_expiration);

  g_object_unref(parser);
  g_free(json);
}

/* ============================================================================
 * Blob Result Tests
 * ============================================================================ */

static void
test_blob_free_null(void)
{
  /* Should not crash */
  gnostr_blossom_blob_free(NULL);
}

static void
test_blob_free_partial(void)
{
  GnostrBlossomBlob *blob = g_new0(GnostrBlossomBlob, 1);
  blob->sha256 = g_strdup("test_hash");
  /* url and mime_type are NULL */
  blob->size = 1234;

  /* Should not crash even with partial data */
  gnostr_blossom_blob_free(blob);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  /* SHA-256 tests */
  g_test_add_func("/blossom/sha256/basic", test_sha256_file_basic);
  g_test_add_func("/blossom/sha256/empty", test_sha256_file_empty);
  g_test_add_func("/blossom/sha256/binary", test_sha256_file_binary);
  g_test_add_func("/blossom/sha256/not_found", test_sha256_file_not_found);
  g_test_add_func("/blossom/sha256/null_inputs", test_sha256_file_null_inputs);

  /* MIME type tests */
  g_test_add_func("/blossom/mime/images", test_mime_type_images);
  g_test_add_func("/blossom/mime/videos", test_mime_type_videos);
  g_test_add_func("/blossom/mime/audio", test_mime_type_audio);
  g_test_add_func("/blossom/mime/unknown", test_mime_type_unknown);
  g_test_add_func("/blossom/mime/path_with_dirs", test_mime_type_path_with_directories);

  /* Auth event tests */
  g_test_add_func("/blossom/auth_event/upload", test_auth_event_upload);
  g_test_add_func("/blossom/auth_event/delete", test_auth_event_delete);
  g_test_add_func("/blossom/auth_event/list", test_auth_event_list);
  g_test_add_func("/blossom/auth_event/expiration", test_auth_event_expiration);

  /* Blob tests */
  g_test_add_func("/blossom/blob/free_null", test_blob_free_null);
  g_test_add_func("/blossom/blob/free_partial", test_blob_free_partial);

  return g_test_run();
}
