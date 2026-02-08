/* Unit tests for YouTube URL detection and video ID extraction (nostrc-1du) */
#include <glib.h>
#include "../src/util/youtube_url.h"

static void test_is_youtube_standard(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
  g_assert_true(gnostr_youtube_url_is_youtube("https://youtube.com/watch?v=dQw4w9WgXcQ"));
  g_assert_true(gnostr_youtube_url_is_youtube("http://www.youtube.com/watch?v=dQw4w9WgXcQ"));
}

static void test_is_youtube_short_url(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://youtu.be/dQw4w9WgXcQ"));
}

static void test_is_youtube_shorts(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://www.youtube.com/shorts/dQw4w9WgXcQ"));
}

static void test_is_youtube_embed(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://www.youtube.com/embed/dQw4w9WgXcQ"));
}

static void test_is_youtube_music(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://music.youtube.com/watch?v=dQw4w9WgXcQ"));
}

static void test_is_youtube_mobile(void) {
  g_assert_true(gnostr_youtube_url_is_youtube("https://m.youtube.com/watch?v=dQw4w9WgXcQ"));
}

static void test_is_not_youtube(void) {
  g_assert_false(gnostr_youtube_url_is_youtube("https://example.com/watch?v=abc"));
  g_assert_false(gnostr_youtube_url_is_youtube("https://notyoutube.com/watch?v=abc"));
  g_assert_false(gnostr_youtube_url_is_youtube("https://github.com/foo/bar"));
  g_assert_false(gnostr_youtube_url_is_youtube(NULL));
  g_assert_false(gnostr_youtube_url_is_youtube(""));
  g_assert_false(gnostr_youtube_url_is_youtube("not a url"));
}

static void test_extract_watch(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_watch_with_params(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42&list=PLfoo");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_short_url(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://youtu.be/dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_short_url_with_timestamp(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://youtu.be/dQw4w9WgXcQ?t=42");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_shorts(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://www.youtube.com/shorts/dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_embed(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://www.youtube.com/embed/dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_live(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://www.youtube.com/live/dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_music(void) {
  char *id = gnostr_youtube_url_extract_video_id("https://music.youtube.com/watch?v=dQw4w9WgXcQ");
  g_assert_nonnull(id);
  g_assert_cmpstr(id, ==, "dQw4w9WgXcQ");
  g_free(id);
}

static void test_extract_null_for_non_youtube(void) {
  g_assert_null(gnostr_youtube_url_extract_video_id("https://example.com/video"));
  g_assert_null(gnostr_youtube_url_extract_video_id(NULL));
  g_assert_null(gnostr_youtube_url_extract_video_id(""));
}

static void test_extract_null_for_channel_url(void) {
  /* Channel/user pages don't have video IDs */
  g_assert_null(gnostr_youtube_url_extract_video_id("https://www.youtube.com/@username"));
  g_assert_null(gnostr_youtube_url_extract_video_id("https://www.youtube.com/channel/UCxyz"));
}

static void test_build_embed(void) {
  char *url = gnostr_youtube_url_build_embed("dQw4w9WgXcQ");
  g_assert_nonnull(url);
  g_assert_cmpstr(url, ==, "https://www.youtube.com/embed/dQw4w9WgXcQ?autoplay=1");
  g_free(url);
}

static void test_build_embed_null(void) {
  g_assert_null(gnostr_youtube_url_build_embed(NULL));
  g_assert_null(gnostr_youtube_url_build_embed(""));
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  /* is_youtube tests */
  g_test_add_func("/youtube/is_youtube/standard", test_is_youtube_standard);
  g_test_add_func("/youtube/is_youtube/short_url", test_is_youtube_short_url);
  g_test_add_func("/youtube/is_youtube/shorts", test_is_youtube_shorts);
  g_test_add_func("/youtube/is_youtube/embed", test_is_youtube_embed);
  g_test_add_func("/youtube/is_youtube/music", test_is_youtube_music);
  g_test_add_func("/youtube/is_youtube/mobile", test_is_youtube_mobile);
  g_test_add_func("/youtube/is_youtube/not_youtube", test_is_not_youtube);

  /* extract_video_id tests */
  g_test_add_func("/youtube/extract/watch", test_extract_watch);
  g_test_add_func("/youtube/extract/watch_with_params", test_extract_watch_with_params);
  g_test_add_func("/youtube/extract/short_url", test_extract_short_url);
  g_test_add_func("/youtube/extract/short_url_timestamp", test_extract_short_url_with_timestamp);
  g_test_add_func("/youtube/extract/shorts", test_extract_shorts);
  g_test_add_func("/youtube/extract/embed", test_extract_embed);
  g_test_add_func("/youtube/extract/live", test_extract_live);
  g_test_add_func("/youtube/extract/music", test_extract_music);
  g_test_add_func("/youtube/extract/non_youtube", test_extract_null_for_non_youtube);
  g_test_add_func("/youtube/extract/channel_url", test_extract_null_for_channel_url);

  /* build_embed tests */
  g_test_add_func("/youtube/build_embed/valid", test_build_embed);
  g_test_add_func("/youtube/build_embed/null", test_build_embed_null);

  return g_test_run();
}
