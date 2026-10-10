#include <nostr-gtk-1.0/content_renderer.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <nostr-gtk-1.0/gn-markdown.h>
#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
static GBytes *two_frame_gif(void);
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include <string.h>
#include "nostrc-test-gdk-frame.h"

static void markdown(void) {
  g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse("# Hi\n**bold** [label](https://example.org) `code`", -1);
  g_assert_nonnull(doc);
  g_assert_cmpuint(doc->tokens->len, >, 4);
  GnMarkdownToken *heading = g_ptr_array_index(doc->tokens, 0);
  g_assert_cmpint(heading->kind, ==, GN_MARKDOWN_HEADING);
  g_assert_cmpuint(heading->level, ==, 1);
  gboolean link = FALSE, strong = FALSE;
  for (guint i = 0; i < doc->tokens->len; i++) {
    GnMarkdownToken *t = g_ptr_array_index(doc->tokens, i);
    if (t->kind == GN_MARKDOWN_LINK) link = g_strcmp0(t->target, "https://example.org") == 0;
    if (t->style & GN_MARKDOWN_STYLE_STRONG) strong = TRUE;
  }
  g_assert_true(link);
  g_assert_true(strong);
  g_autoptr(GnMarkdownDocument) invalid = gn_markdown_parse("bad\xff", 4);
  g_assert_true(g_utf8_validate(((GnMarkdownToken *)g_ptr_array_index(invalid->tokens, 0))->text, -1, NULL));
  g_autofree gchar *long_text = g_strnfill(GN_MARKDOWN_MAX_INPUT_BYTES + 100, 'a');
  g_autoptr(GnMarkdownDocument) bounded = gn_markdown_parse(long_text, -1);
  g_assert_true(bounded->truncated);
  g_autofree gchar *hostile = g_strnfill(GN_MARKDOWN_MAX_INPUT_BYTES, '*');
  g_autoptr(GnMarkdownDocument) nested = gn_markdown_parse(hostile, -1);
  g_assert_cmpuint(nested->tokens->len, <=, GN_MARKDOWN_MAX_TOKENS);
}

static void references(void) {
#ifndef NOSTR_GTK_MESON_EXTERNAL_CORE
  /* CMake standalone link pulls the repository core; Meson's standalone
   * build can use a prebuilt nostr-gobject with optional symbols unresolved. */
  g_autoptr(GError) error = NULL;
  GnContentRenderResult *content = gn_content_parse("portable", -1, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(content);
  gnostr_content_render_result_free(content);
#endif
  GnNostrReference r = { .type = GN_NOSTR_REFERENCE_EVENT,
    .id = "0000000000000000000000000000000000000000000000000000000000000000",
    .author = "1111111111111111111111111111111111111111111111111111111111111111",
    .kind = 1 };
  g_autofree gchar *repost = gn_nostr_build_repost_template(&r, NULL);
  g_autofree gchar *quote = gn_nostr_build_quote_template(&r, "comment");
  g_assert_nonnull(repost);
  g_assert_nonnull(quote);
  g_assert_nonnull(strstr(quote, "nostr:nevent1"));
  g_autoptr(GnNostrRepostDescriptor) parsed = gn_nostr_repost_descriptor_parse(repost, FALSE);
  g_assert_nonnull(parsed);
  g_assert_cmpint(parsed->source_kind, ==, 6);
  g_assert_cmpstr(parsed->target->id, ==, r.id);
  g_assert_true(g_str_has_prefix(parsed->target->uri, "nostr:nevent1"));
  g_assert_cmpint(parsed->target->kind, ==, 1);
  g_autoptr(GnNostrRepostDescriptor) q = gn_nostr_repost_descriptor_parse(quote, FALSE);
  g_assert_nonnull(q);
  g_assert_true(q->quote);
  r.kind = 30023;
  g_autofree gchar *generic = gn_nostr_build_repost_template(&r, NULL);
  g_assert_nonnull(strstr(generic, "\"kind\":16"));
  g_assert_nonnull(strstr(generic, "\"k\""));
  g_assert_null(gn_nostr_reference_parse("nostr:nsec1invalid"));
  g_assert_null(gn_nostr_repost_descriptor_parse(
    "{\"kind\":\"6\",\"tags\":[]}", FALSE));
  g_assert_null(gn_nostr_repost_descriptor_parse(
    "{\"kind\":6,\"tags\":{}}", FALSE));
  guint8 id_bytes[32] = {0};
  char *note = NULL;
  g_assert_cmpint(nostr_nip19_encode_note(id_bytes, &note), ==, 0);
  g_autofree gchar *uri = g_strdup_printf("nostr:%s", note);
  free(note);
  g_autoptr(GnNostrReference) decoded = gn_nostr_reference_parse(uri);
  g_assert_nonnull(decoded);
  g_assert_cmpstr(decoded->id, ==, r.id);
  NostrEntityPointer entity = { .public_key = r.author, .kind = 30023,
    .identifier = "article" };
  char *naddr = NULL;
  g_assert_cmpint(nostr_nip19_encode_naddr(&entity, &naddr), ==, 0);
  g_autoptr(GnNostrReference) address = gn_nostr_reference_parse(naddr);
  free(naddr);
  g_assert_nonnull(address);
  g_assert_cmpint(address->type, ==, GN_NOSTR_REFERENCE_ADDRESS);
  g_assert_cmpstr(address->id, ==, "article");
  g_autofree gchar *address_repost = gn_nostr_build_repost_template(address, NULL);
  g_assert_nonnull(strstr(address_repost, "\"a\""));
  g_autoptr(GnNostrRepostDescriptor) address_parsed =
    gn_nostr_repost_descriptor_parse(address_repost, FALSE);
  g_assert_nonnull(address_parsed);
  g_assert_cmpstr(address_parsed->target->id, ==, "article");
  g_assert_true(g_str_has_prefix(address_parsed->target->uri, "nostr:naddr1"));
}

static guint loads, image_loads, viewer_loads;
static void viewer_loaded(GnMediaViewer *viewer, guint index, const char *url, gpointer data) {
  (void)viewer; (void)data;
  g_assert_cmpuint(index, ==, 1);
  g_assert_cmpstr(url, ==, "https://example.org/b");
  viewer_loads++;
}
static void loaded(GnOgPreviewCard *card, const char *url, gpointer data) {
  (void)card; (void)data;
  g_assert_cmpstr(url, ==, "https://example.org");
  loads++;
}
static void image_loaded(GnOgPreviewCard *card, const char *url, gpointer data) {
  (void)card; (void)data;
  g_assert_cmpstr(url, ==, "https://example.org/img.png");
  image_loads++;
}
static void widgets(void) {
  if (!gtk_init_check()) { g_test_skip("GTK display unavailable"); return; }
  GnOgPreviewCard *card = gn_og_preview_card_new();
  g_signal_connect(card, "load-requested", G_CALLBACK(loaded), NULL);
  g_signal_connect(card, "image-load-requested", G_CALLBACK(image_loaded), NULL);
  gn_og_preview_card_set_url(card, "https://example.org");
  g_assert_cmpuint(loads, ==, 0);
  gn_og_preview_card_request_load(card);
  g_assert_cmpuint(loads, ==, 1);
  gn_og_preview_card_set_result(card, "Title", "Description", "Site", "https://example.org/img.png");
  g_assert_cmpuint(image_loads, ==, 0);
  gn_og_preview_card_request_image(card);
  g_assert_cmpuint(image_loads, ==, 1);
  gn_og_preview_card_set_url(card, "https://another.example");
  gn_og_preview_card_set_result_for_url(card, "https://example.org",
                                         "stale", "stale", "stale", NULL);
  g_assert_cmpstr(gn_og_preview_card_get_url(card), ==, "https://another.example");
  gn_og_preview_card_clear(card);
  g_assert_true(g_cancellable_is_cancelled(gn_og_preview_card_get_cancellable(card)) == FALSE);
  g_object_ref_sink(card);
  g_object_unref(card);

  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  const char *urls[] = { "https://example.org/a", "https://example.org/b", NULL };
  g_signal_connect(viewer, "load-requested", G_CALLBACK(viewer_loaded), NULL);
  gn_media_viewer_set_gallery(viewer, urls, 0);
  guint64 old_generation = gn_media_viewer_get_generation(viewer);
  g_assert_true(gn_media_viewer_navigate(viewer, 1));
  g_assert_cmpuint(viewer_loads, ==, 0);
  g_assert_cmpuint(gn_media_viewer_get_generation(viewer), >, old_generation);
  gn_media_viewer_request_load(viewer);
  g_assert_cmpuint(viewer_loads, ==, 1);
  g_assert_cmpuint(gn_media_viewer_get_index(viewer), ==, 1);
  g_assert_false(gn_media_viewer_navigate(viewer, 1));
  gn_media_viewer_set_zoom(viewer, 2.0);
  g_assert_cmpfloat(gn_media_viewer_get_zoom(viewer), ==, 2.0);
  /* nostrc-p15n5.5/.8: any paintable; an animation travels on its texture. */
  g_assert_null(gn_media_viewer_get_paintable(viewer));
  g_autoptr(GBytes) gif = two_frame_gif();
  g_autoptr(GnAnimatedImage) anim = gn_animated_image_new_from_bytes(gif, 64, NULL);
  g_assert_nonnull(anim);
  g_autoptr(GdkTexture) still = g_object_ref(gn_animated_image_get_current_texture(anim));
  gn_animated_image_set_for_texture(still, anim);
  gn_media_viewer_set_texture(viewer, 1, still);
  g_assert_true(gn_media_viewer_get_paintable(viewer) == GDK_PAINTABLE(anim));
  gn_media_viewer_set_paintable(viewer, 1, GDK_PAINTABLE(still));
  g_assert_true(gn_media_viewer_get_paintable(viewer) == GDK_PAINTABLE(still));
  gn_media_viewer_request_load(viewer);
  g_assert_cmpuint(viewer_loads, ==, 1); /* loaded: nothing to ask */
  gtk_window_destroy(GTK_WINDOW(viewer));

  GnNip34IssueFields *fields = gn_nip34_issue_fields_new();
  GnNip34IssueFieldsSnapshot initial = { .steps = "Step 1", .expected = "yes",
    .actual = "no", .labels = "bug", .related_commits = "abcdef", .attachment_urls = "https://example.org/log" };
  gn_nip34_issue_fields_set_snapshot(fields, &initial);
  g_autoptr(GnNip34IssueFieldsSnapshot) snapshot = gn_nip34_issue_fields_snapshot(fields);
  g_assert_cmpstr(snapshot->steps, ==, "Step 1");
  g_assert_cmpstr(snapshot->attachment_urls, ==, "https://example.org/log");
  g_object_ref_sink(fields);
  g_object_unref(fields);
}
/* 2x1, red/blue then blue/red, 100 ms per frame. */
static GBytes *two_frame_gif(void) {
  static const guint8 gif[] = {
    'G','I','F','8','9','a', 2,0, 1,0, 0x80, 0, 0,
    0xFF,0,0, 0,0,0xFF,
    0x21,0xF9,4, 0,10,0, 0,0,
    0x2C, 0,0,0,0, 2,0,1,0, 0, 2, 2, 0x44,0x0A, 0,
    0x21,0xF9,4, 0,10,0, 0,0,
    0x2C, 0,0,0,0, 2,0,1,0, 0, 2, 2, 0x0C,0x0A, 0,
    0x3B };
  return g_bytes_new_static(gif, sizeof gif);
}
static guint8 first_red(GnAnimatedImage *anim) {
  guint8 px[8];
  gdk_texture_download(gn_animated_image_get_current_texture(anim), px, 8);
  /* cairo ARGB32 in memory order B,G,R,A on little endian */
  return px[2];
}
static void animated(void) {
  g_autoptr(GBytes) gif = two_frame_gif();
  guint w = 0, h = 0;
  g_assert_true(gn_animated_image_probe(gif, &w, &h));
  g_assert_cmpuint(w, ==, 2);
  g_assert_cmpuint(h, ==, 1);
  g_autoptr(GBytes) png = g_bytes_new_static("\x89PNG\r\n\x1a\n....", 12);
  g_assert_false(gn_animated_image_probe(png, NULL, NULL));
  g_autoptr(GError) error = NULL;
  g_assert_null(gn_animated_image_new_from_bytes(png, 64, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  /* The header's screen size is refused before anything is decoded. */
  g_assert_null(gn_animated_image_new_from_bytes(gif, 1, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  /* One frame is a still image: the caller's texture path shows it. */
  gsize len = 0;
  const guint8 *d = g_bytes_get_data(gif, &len);
  g_autofree guint8 *still = g_memdup2(d, 43);
  still[42] = 0x3B;
  g_autoptr(GBytes) one = g_bytes_new(still, 43);
  g_assert_null(gn_animated_image_new_from_bytes(one, 64, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  /* Garbage after a valid header never crashes. */
  g_autofree guint8 *junk = g_memdup2(d, len);
  for (gsize i = 19; i < len; i++) junk[i] = (guint8)(i * 37);
  g_autoptr(GBytes) damaged = g_bytes_new(junk, len);
  g_autoptr(GnAnimatedImage) none = gn_animated_image_new_from_bytes(damaged, 64, NULL);
  g_assert_null(none);

  g_autoptr(GnAnimatedImage) anim = gn_animated_image_new_from_bytes(gif, 64, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(gn_animated_image_get_n_frames(anim), ==, 2);
  g_assert_cmpint(gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(anim)), ==, 2);
  g_assert_cmpuint(first_red(anim), ==, 0xFF);
  gn_animated_image_advance(anim);
  g_assert_cmpuint(gn_animated_image_get_frame(anim), ==, 1);
  g_assert_cmpuint(first_red(anim), ==, 0);
  /* Plays only while a widget showing it is mapped. */
  GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(anim));
  g_object_ref_sink(picture);
  gn_animated_image_attach(picture, GDK_PAINTABLE(anim));
  g_assert_false(gn_animated_image_get_playing(anim));
  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), picture);
  gtk_window_present(GTK_WINDOW(window));
  while (!gtk_widget_get_mapped(picture)) g_main_context_iteration(NULL, TRUE);
  g_assert_true(gn_animated_image_get_playing(anim));
  gn_animated_image_attach(picture, NULL);
  g_assert_false(gn_animated_image_get_playing(anim));
  gn_animated_image_attach(picture, GDK_PAINTABLE(anim));
  g_assert_true(gn_animated_image_get_playing(anim));
  /* Taken off screen (a recycled row): paused. */
  gtk_window_set_child(GTK_WINDOW(window), NULL);
  g_assert_false(gn_animated_image_get_playing(anim));
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(picture);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/portable/markdown", markdown);
  g_test_add_func("/portable/references", references);
  g_test_add_func("/portable/widgets", widgets);
  g_test_add_func("/portable/animated-gif", animated);
  return g_test_run();
}
