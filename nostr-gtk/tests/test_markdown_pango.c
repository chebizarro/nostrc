/* Tests for the portable Markdown to Pango emitter (gn-markdown-pango). */
#include <nostr-gtk-1.0/gn-markdown-pango.h>
#include <pango/pango.h>
#include <string.h>

static void
assert_valid_markup(const gchar *markup)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(g_utf8_validate(markup, -1, NULL));
  if (!pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, &error))
    g_error("invalid markup %s: %s", markup, error->message);
}

static const GnMarkdownPangoOptions article = {
  .mode = GN_MARKDOWN_PANGO_FULL,
  .flags = GN_MARKDOWN_PANGO_HEADING_SIZES | GN_MARKDOWN_PANGO_TABLE_GRID,
};

static void
article_golden(void)
{
  /* The gnostr article look (formerly markdown_to_pango), re-baselined on
   * the portable parser: no trailing newline is added. */
  struct { const char *input; const char *markup; } cases[] = {
    { "Hello", "Hello" },
    { "**bold** and *italic*", "<b>bold</b> and <i>italic</i>" },
    { "`<code>`", "<tt>&lt;code&gt;</tt>" },
    { "[label](https://example.org)", "label (https://example.org)" },
    { "- item", "  • item" },
    { "3. third", "  3. third" },
    { "> quote", "<span alpha=\"80%\" style=\"italic\">quote</span>" },
    { "# Title", "<span size=\"xx-large\" weight=\"bold\">Title</span>" },
    { "### Small", "<span size=\"large\" weight=\"bold\">Small</span>" },
    { "---", "<span alpha=\"50%\">---</span>" },
    { "```\na < b\nc\n```", "\n<tt>a &lt; b\nc</tt>\n" },
    { "~~gone~~ snake_case 2 * 3", "<s>gone</s> snake_case 2 * 3" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree char *actual = gn_markdown_pango_from_text(cases[i].input, &article);
    g_assert_cmpstr(actual, ==, cases[i].markup);
    assert_valid_markup(actual);
  }
}

static void
glyph_look(void)
{
  GnMarkdownPangoOptions opts = {
    .flags = GN_MARKDOWN_PANGO_GLYPH_BLOCKS | GN_MARKDOWN_PANGO_TABLE_GRID,
  };
  g_autofree char *m = gn_markdown_pango_from_text(
    "# H\n> q\n---\n- a\n  - b\n- [x] done\n\n| A | B |\n|---|--:|\n| x | 10 |", &opts);
  g_assert_cmpstr(m, ==,
    "<b>H</b>\n│ q\n────────\n• a\n  ◦ b\n☑ done\n\n"
    "<tt><b>A</b> │  <b>B</b>\n──┼───\nx │ 10</tt>");
  assert_valid_markup(m);
  /* Without the grid, cells are joined by a bar. */
  opts.flags = 0;
  g_autofree char *plain = gn_markdown_pango_from_text("| A | B |\n|---|---|\n| x | y |", &opts);
  g_assert_cmpstr(plain, ==, "<b>A</b> | <b>B</b>\nx | y");
}

static gboolean
bracket_https(GString *out, const gchar *text, gpointer user_data)
{
  guint *calls = user_data;
  (*calls)++;
  if (!g_str_has_prefix(text, "https://"))
    return FALSE;
  g_autofree gchar *escaped = g_markup_escape_text(text, -1);
  g_string_append_printf(out, "<a href=\"%s\">%s</a>", escaped, escaped);
  return TRUE;
}

static void
format_text_hook(void)
{
  guint calls = 0;
  GnMarkdownPangoOptions opts = { .format_text = bracket_https, .user_data = &calls };
  g_autofree char *m = gn_markdown_pango_from_text(
    "[click](https://a.example) `https://code` https://b.example", &opts);
  /* The label and code are never handed to the host; the target and the raw
   * address are. */
  g_assert_cmpstr(m, ==,
    "click (<a href=\"https://a.example\">https://a.example</a>) <tt>https://code</tt> "
    "<a href=\"https://b.example\">https://b.example</a>");
  g_assert_cmpuint(calls, >, 0);
  /* A summary never links. */
  calls = 0;
  opts.mode = GN_MARKDOWN_PANGO_SUMMARY;
  g_autofree char *s = gn_markdown_pango_from_text("see https://b.example", &opts);
  g_assert_cmpstr(s, ==, "see https://b.example");
  g_assert_cmpuint(calls, ==, 0);
}

static void
summary_mode(void)
{
  GnMarkdownPangoOptions opts = { .mode = GN_MARKDOWN_PANGO_SUMMARY };
  g_autofree char *s = gn_markdown_pango_from_text(
    "# Heading\n\nSome   *text*\nmore 2 * 3 #tag [label](https://x.example)\n```\ncode\n```\n- item",
    &opts);
  g_assert_cmpstr(s, ==, "Some <i>text</i> more 2 * 3 #tag label item");
  assert_valid_markup(s);
  opts.max_chars = 7;
  g_autofree char *capped = gn_markdown_pango_from_text("Some **bold words** here", &opts);
  g_assert_cmpstr(capped, ==, "Some <b>bo…</b>");
  assert_valid_markup(capped);
}

static void
cap_never_breaks_markup(void)
{
  GnMarkdownPangoOptions opts = article;
  const char *input = "# T&itle\n**b <x> *c* é**\n| a | b |\n|---|---|\n| 1 | 2 |";
  for (gsize cap = 1; cap < 40; cap++) {
    opts.max_chars = cap;
    g_autofree char *m = gn_markdown_pango_from_text(input, &opts);
    assert_valid_markup(m);
  }
  opts.max_chars = 3;
  g_autofree char *m = gn_markdown_pango_from_text("a&b é", &opts);
  g_assert_cmpstr(m, ==, "a&amp;b…");
}

static void
fuzz(void)
{
  GRand *rand = g_rand_new_with_seed(0x6d61726b);
  const char alphabet[] = "*_~`#>-|[]()!:\n <&;\"\x27" "abc\xc3\xa9\xff";
  for (guint iter = 0; iter < 3000; iter++) {
    gsize n = g_rand_int_range(rand, 0, 200);
    g_autofree char *buf = g_malloc(n + 1);
    for (gsize i = 0; i < n; i++)
      buf[i] = g_rand_boolean(rand)
        ? alphabet[g_rand_int_range(rand, 0, sizeof alphabet - 1)]
        : (char)g_rand_int_range(rand, 1, 256);
    buf[n] = 0;
    GnMarkdownPangoOptions opts = {
      .mode = g_rand_boolean(rand) ? GN_MARKDOWN_PANGO_SUMMARY : GN_MARKDOWN_PANGO_FULL,
      .flags = g_rand_int_range(rand, 0, 8),
      .max_chars = g_rand_int_range(rand, 0, 50),
    };
    g_autofree char *m = gn_markdown_pango_from_text(buf, &opts);
    assert_valid_markup(m);
    g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(buf, -1);
    g_autofree char *plain = gn_markdown_plain_text(doc, opts.max_chars);
    g_assert_true(g_utf8_validate(plain, -1, NULL));
  }
  /* Deep nesting stays bounded. */
  GString *deep = g_string_new(NULL);
  for (guint i = 0; i < 5000; i++)
    g_string_append(deep, "> **[*_`");
  g_autofree char *m = gn_markdown_pango_from_text(deep->str, &article);
  assert_valid_markup(m);
  g_string_free(deep, TRUE);
  g_rand_free(rand);
}

static void
truncated_fallback(void)
{
  GString *big = g_string_new(NULL);
  while (big->len <= GN_MARKDOWN_MAX_INPUT_BYTES)
    g_string_append(big, "*a* <b> ");
  g_autofree char *m = gn_markdown_pango_from_text(big->str, &article);
  g_autofree char *escaped = g_markup_escape_text(big->str, -1);
  g_assert_cmpstr(m, ==, escaped);
  g_string_free(big, TRUE);
}

static void
images_and_plain(void)
{
  g_autofree char *m = gn_markdown_pango_from_text("![a cat](https://x.example/c.png)", &article);
  g_assert_cmpstr(m, ==, "a cat (https://x.example/c.png)");
  g_assert_null(strstr(m, "<a"));
  g_assert_null(strstr(m, "<img"));

  g_autoptr(GnMarkdownDocument) none = gn_markdown_parse("no image here", -1);
  g_assert_null(gn_markdown_dup_first_image_target(none));
  g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(
    "# T\n- **b** [l](https://u.example) ![one](https://i.example/1.png)\n---\n![x](https://i.example/2.png)", -1);
  g_autofree char *first = gn_markdown_dup_first_image_target(doc);
  g_assert_cmpstr(first, ==, "https://i.example/1.png");
  g_autofree char *plain = gn_markdown_plain_text(doc, 0);
  g_assert_cmpstr(plain, ==, "T\nb l one\n\nx");
  g_autofree char *capped = gn_markdown_plain_text(doc, 3);
  g_assert_cmpstr(capped, ==, "T\nb…");
}

static void
worst_case_timing(void)
{
  GString *big = g_string_new(NULL);
  while (big->len + 16 < GN_MARKDOWN_MAX_INPUT_BYTES)
    g_string_append(big, "| **a** | `b` |\n");
  g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(big->str, -1);
  gint64 start = g_get_monotonic_time();
  g_autofree char *m = gn_markdown_pango_format(doc, &article);
  gint64 elapsed = g_get_monotonic_time() - start;
  g_assert_nonnull(m);
  /* Generous: a 64 KiB document must format well within a frame budget
   * multiple even on slow CI. */
  g_assert_cmpint(elapsed, <, 2 * G_USEC_PER_SEC);
  g_string_free(big, TRUE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gtk/markdown-pango/article-golden", article_golden);
  g_test_add_func("/nostr-gtk/markdown-pango/glyph-look", glyph_look);
  g_test_add_func("/nostr-gtk/markdown-pango/format-text-hook", format_text_hook);
  g_test_add_func("/nostr-gtk/markdown-pango/summary", summary_mode);
  g_test_add_func("/nostr-gtk/markdown-pango/cap-valid-markup", cap_never_breaks_markup);
  g_test_add_func("/nostr-gtk/markdown-pango/fuzz", fuzz);
  g_test_add_func("/nostr-gtk/markdown-pango/truncated-fallback", truncated_fallback);
  g_test_add_func("/nostr-gtk/markdown-pango/images-and-plain", images_and_plain);
  g_test_add_func("/nostr-gtk/markdown-pango/worst-case-timing", worst_case_timing);
  return g_test_run();
}
