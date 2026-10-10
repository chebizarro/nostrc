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

static GnMarkdownToken *nth_kind(GnMarkdownDocument *doc, GnMarkdownTokenKind kind, guint nth) {
  for (guint i = 0; i < doc->tokens->len; i++) {
    GnMarkdownToken *t = g_ptr_array_index(doc->tokens, i);
    if (t->kind == kind && nth-- == 0) return t;
  }
  return NULL;
}

/* GFM blocks and inlines (nostrc-p15n5.1). */
static void markdown_gfm(void) {
  g_autoptr(GnMarkdownDocument) table = gn_markdown_parse(
    "| Name | Qty | Note |\n|:-----|----:|:---:|\n| a | 1 | **x** |\n| b \\| c | 22 |\nafter", -1);
  GnMarkdownToken *h0 = nth_kind(table, GN_MARKDOWN_TABLE_CELL, 0);
  g_assert_nonnull(h0);
  g_assert_cmpstr(h0->text, ==, "Name");
  g_assert_true(h0->ordered);
  g_assert_cmpuint(h0->columns, ==, 3);
  g_assert_cmpuint(h0->align, ==, GN_MARKDOWN_ALIGN_LEFT);
  g_assert_cmpuint(nth_kind(table, GN_MARKDOWN_TABLE_CELL, 1)->align, ==, GN_MARKDOWN_ALIGN_RIGHT);
  g_assert_cmpuint(nth_kind(table, GN_MARKDOWN_TABLE_CELL, 2)->align, ==, GN_MARKDOWN_ALIGN_CENTER);
  GnMarkdownToken *body = nth_kind(table, GN_MARKDOWN_TABLE_CELL, 5);
  g_assert_cmpstr(body->text, ==, "**x**");
  g_assert_false(body->ordered);
  g_assert_cmpuint(body->level, ==, 2);
  /* An escaped pipe stays in its cell; a short row is padded. */
  g_assert_cmpstr(nth_kind(table, GN_MARKDOWN_TABLE_CELL, 6)->text, ==, "b \\| c");
  g_assert_cmpstr(nth_kind(table, GN_MARKDOWN_TABLE_CELL, 8)->text, ==, "");
  g_assert_null(nth_kind(table, GN_MARKDOWN_TABLE_CELL, 9));
  g_assert_cmpstr(nth_kind(table, GN_MARKDOWN_TEXT, 0)->text, ==, "after");
  /* The delimiter row is not text, and no line break stands for it. */
  guint breaks = 0;
  for (guint i = 0; i < table->tokens->len; i++)
    breaks += ((GnMarkdownToken *)g_ptr_array_index(table->tokens, i))->kind == GN_MARKDOWN_LINE_BREAK;
  g_assert_cmpuint(breaks, ==, 3);

  /* A pipe without a delimiter row is just text. */
  g_autoptr(GnMarkdownDocument) plain = gn_markdown_parse("a | b\nc | d", -1);
  g_assert_null(nth_kind(plain, GN_MARKDOWN_TABLE_CELL, 0));

  g_autoptr(GnMarkdownDocument) lists = gn_markdown_parse(
    "- [ ] open\n- [x] done\n  - nested\n3. third\n4) fourth", -1);
  GnMarkdownToken *open = nth_kind(lists, GN_MARKDOWN_LIST_ITEM, 0);
  g_assert_cmpuint(open->task, ==, GN_MARKDOWN_TASK_OPEN);
  g_assert_cmpstr(open->text, ==, "open");
  g_assert_cmpuint(nth_kind(lists, GN_MARKDOWN_LIST_ITEM, 1)->task, ==, GN_MARKDOWN_TASK_DONE);
  GnMarkdownToken *nested = nth_kind(lists, GN_MARKDOWN_LIST_ITEM, 2);
  g_assert_cmpuint(nested->indent, ==, 1);
  g_assert_cmpstr(nested->text, ==, "nested");
  GnMarkdownToken *third = nth_kind(lists, GN_MARKDOWN_LIST_ITEM, 3);
  g_assert_true(third->ordered);
  g_assert_cmpuint(third->level, ==, 3);
  g_assert_cmpuint(nth_kind(lists, GN_MARKDOWN_LIST_ITEM, 4)->level, ==, 4);

  g_autoptr(GnMarkdownDocument) inl = gn_markdown_parse(
    "~~gone~~ snake_case_name 2 * 3 * 4 \\*lit\\* <https://a.example/x> ![cat](https://img.example/c.png)", -1);
  GnMarkdownToken *strike = nth_kind(inl, GN_MARKDOWN_TEXT, 0);
  g_assert_cmpstr(strike->text, ==, "gone");
  g_assert_true(strike->style & GN_MARKDOWN_STYLE_STRIKETHROUGH);
  for (guint i = 0; i < inl->tokens->len; i++) {
    GnMarkdownToken *t = g_ptr_array_index(inl->tokens, i);
    /* Intraword "_" and spaced "*" are literal. */
    g_assert_false(t->style & (GN_MARKDOWN_STYLE_EMPHASIS | GN_MARKDOWN_STYLE_STRONG));
  }
  GString *joined = g_string_new(NULL);
  for (guint i = 0; i < inl->tokens->len; i++) {
    GnMarkdownToken *t = g_ptr_array_index(inl->tokens, i);
    if (t->kind == GN_MARKDOWN_TEXT) g_string_append(joined, t->text);
  }
  g_assert_nonnull(strstr(joined->str, "snake_case_name 2 * 3 * 4 *lit* "));
  g_assert_null(strstr(joined->str, "<"));
  g_string_free(joined, TRUE);
  GnMarkdownToken *autolink = nth_kind(inl, GN_MARKDOWN_RAW_URL, 0);
  g_assert_cmpstr(autolink->target, ==, "https://a.example/x");
  GnMarkdownToken *image = nth_kind(inl, GN_MARKDOWN_IMAGE, 0);
  g_assert_cmpstr(image->text, ==, "cat");
  g_assert_cmpstr(image->target, ==, "https://img.example/c.png");

  g_autoptr(GnMarkdownDocument) fence = gn_markdown_parse("~~~\n| a | b |\n|---|---|\n~~~", -1);
  g_assert_null(nth_kind(fence, GN_MARKDOWN_TABLE_CELL, 0));
  g_assert_cmpstr(nth_kind(fence, GN_MARKDOWN_CODE, 0)->text, ==, "| a | b |");

  g_autoptr(GnMarkdownDocument) only = gn_markdown_parse_inline("# not a heading *em*", -1);
  g_assert_cmpint(((GnMarkdownToken *)g_ptr_array_index(only->tokens, 0))->kind, ==, GN_MARKDOWN_TEXT);
  g_assert_true(nth_kind(only, GN_MARKDOWN_TEXT, 1)->style & GN_MARKDOWN_STYLE_EMPHASIS);

  /* Hostile tables stay within the token budget. */
  GString *wide = g_string_new(NULL);
  for (guint r = 0; r < 4000; r++)
    g_string_append(wide, r == 1 ? "|-|-|-|-|-|-|-|-|-|-|\n" : "|a|b|c|d|e|f|g|h|i|j|\n");
  g_autoptr(GnMarkdownDocument) big = gn_markdown_parse(wide->str, -1);
  g_assert_cmpuint(big->tokens->len, <=, GN_MARKDOWN_MAX_TOKENS);
  g_string_free(wide, TRUE);
}

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
  g_test_add_func("/portable/markdown-gfm", markdown_gfm);
  g_test_add_func("/portable/references", references);
  g_test_add_func("/portable/widgets", widgets);
  g_test_add_func("/portable/animated-gif", animated);
  return g_test_run();
}
