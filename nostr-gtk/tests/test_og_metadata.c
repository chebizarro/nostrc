/* Shared Open Graph <head> parser (nostrc-8xfib.3). No display needed. */
#include <nostr-gtk-1.0/gn-og-preview.h>
#include <string.h>

static GnOgMetadata *
parse(const char *html, GnOgImagePolicy policy, GError **error)
{
  g_autoptr(GBytes) bytes = g_bytes_new(html, strlen(html));
  return gn_og_metadata_parse_html(bytes, "https://example.org/a/page", policy, NULL, error);
}

static void
test_precedence(void)
{
  g_autoptr(GError) error = NULL;
  /* Order in the document does not matter: og > twitter > plain. */
  g_autoptr(GnOgMetadata) m = parse(
    "<html><head><title>Plain</title><meta name='twitter:title' content='TW'>"
    "<meta name='description' content='plain d'>"
    "<meta name='twitter:description' content='tw d'>"
    "<meta property='og:title' content='OG'><meta property='og:site_name' content='Site'>"
    "</head><body></body></html>", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gn_og_metadata_get_title(m), ==, "OG");
  g_assert_cmpstr(gn_og_metadata_get_description(m), ==, "tw d");
  g_assert_cmpstr(gn_og_metadata_get_site_name(m), ==, "Site");
  g_assert_cmpstr(gn_og_metadata_get_source_url(m), ==, "https://example.org/a/page");
  g_assert_null(gn_og_metadata_get_image_url(m));
}

static void
test_body_ignored(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GnOgMetadata) m = parse(
    "<html><head><title>Head</title></head><body>"
    "<meta property='og:title' content='Body'>"
    "<script>var s = \"<meta property='og:description' content='script'>\";</script>"
    "</body></html>", NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gn_og_metadata_get_title(m), ==, "Head");
  g_assert_null(gn_og_metadata_get_description(m));
}

static void
test_truncated_and_invalid_utf8(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GnOgMetadata) m = parse(
    "<html><head><meta property='og:title' content='Bad \xff byte'>"
    "<meta property='og:description' content='cut", NULL, &error);
  g_assert_no_error(error);
  g_assert_true(g_utf8_validate(gn_og_metadata_get_title(m), -1, NULL));
  g_assert_true(g_str_has_prefix(gn_og_metadata_get_title(m), "Bad "));
}

static void
test_bounded(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *long_title = g_strnfill(2000, 'x');
  g_autofree char *html = g_strdup_printf(
    "<html><head><title>%s</title></head></html>", long_title);
  g_autoptr(GnOgMetadata) m = parse(html, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(g_utf8_strlen(gn_og_metadata_get_title(m), -1), ==, 512);
}

static gboolean
reject_tracker(const char *url, gpointer data)
{
  (void)data;
  return !strstr(url, "tracker");
}

static void
test_images(void)
{
  g_autoptr(GError) error = NULL;
  /* twitter:image is the fallback; relative addresses resolve. */
  g_autoptr(GnOgMetadata) tw = parse(
    "<html><head><title>T</title><meta name='twitter:image' content='/tw.png'></head></html>",
    NULL, &error);
  g_assert_cmpstr(gn_og_metadata_get_image_url(tw), ==, "https://example.org/tw.png");
  /* og:image wins over twitter:image in any order. */
  g_autoptr(GnOgMetadata) og = parse(
    "<html><head><title>T</title><meta property='og:image' content='https://cdn.example/og.png'>"
    "<meta name='twitter:image' content='/tw.png'></head></html>", NULL, &error);
  g_assert_cmpstr(gn_og_metadata_get_image_url(og), ==, "https://cdn.example/og.png");
  /* The policy rejects; a non-http scheme is rejected by default. */
  g_autoptr(GnOgMetadata) rejected = parse(
    "<html><head><title>T</title><meta property='og:image' content='https://tracker.example/x.png'>"
    "</head></html>", reject_tracker, &error);
  g_assert_null(gn_og_metadata_get_image_url(rejected));
  g_autoptr(GnOgMetadata) data_uri = parse(
    "<html><head><title>T</title><meta property='og:image' content='data:image/png;base64,AA'>"
    "</head></html>", NULL, &error);
  g_assert_null(gn_og_metadata_get_image_url(data_uri));
  g_assert_no_error(error);
}

static void
test_no_text(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GnOgMetadata) m = parse(
    "<html><head><meta property='og:image' content='https://e.org/x.png'></head></html>",
    NULL, &error);
  g_assert_null(m);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_autoptr(GBytes) empty = g_bytes_new(NULL, 0);
  g_assert_null(gn_og_metadata_parse_html(empty, NULL, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_no_network_entities(void)
{
  g_autoptr(GError) error = NULL;
  /* An external DTD/entity is neither fetched nor expanded (NONET). */
  g_autoptr(GnOgMetadata) m = parse(
    "<!DOCTYPE html SYSTEM \"http://127.0.0.1:9/never.dtd\">"
    "<html><head><title>Safe &ext;</title></head></html>", NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(gn_og_metadata_get_title(m));
  g_assert_null(strstr(gn_og_metadata_get_title(m), "never"));
}

static void
test_boxed(void)
{
  GnOgMetadata *m = gn_og_metadata_new("u", "t", NULL, NULL, NULL);
  GnOgMetadata *copy = g_boxed_copy(GN_TYPE_OG_METADATA, m);
  g_assert_true(copy == m);
  g_boxed_free(GN_TYPE_OG_METADATA, copy);
  g_assert_cmpstr(gn_og_metadata_get_title(m), ==, "t");
  gn_og_metadata_unref(m);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/og-metadata/precedence", test_precedence);
  g_test_add_func("/og-metadata/body-ignored", test_body_ignored);
  g_test_add_func("/og-metadata/truncated-invalid-utf8", test_truncated_and_invalid_utf8);
  g_test_add_func("/og-metadata/bounded", test_bounded);
  g_test_add_func("/og-metadata/images", test_images);
  g_test_add_func("/og-metadata/no-text", test_no_text);
  g_test_add_func("/og-metadata/nonet", test_no_network_entities);
  g_test_add_func("/og-metadata/boxed", test_boxed);
  return g_test_run();
}
