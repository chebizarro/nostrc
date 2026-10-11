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

/* Blocks: text around a table, the table as cells (nostrc-p15n5.1). */
static void
table_blocks(void)
{
  const char *text =
    "\n\nAll cron jobs now on gpt-5.6-sol\n\n"
    "| Job                 | Now          |\n"
    "| ------------------- | -----------: |\n"
    "| LN Node Daily       | **gpt-5.6**  |\n"
    "\nafter";
  g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(text, -1);
  g_autoptr(GPtrArray) blocks = gn_markdown_pango_format_blocks(doc, NULL);
  g_assert_true(gn_markdown_pango_blocks_have_table(blocks));
  g_assert_cmpuint(blocks->len, ==, 3);
  GnMarkdownPangoBlock *intro = g_ptr_array_index(blocks, 0);
  g_assert_cmpint(intro->kind, ==, GN_MARKDOWN_PANGO_BLOCK_TEXT);
  g_assert_cmpstr(intro->markup, ==, "All cron jobs now on gpt-5.6-sol");
  GnMarkdownPangoBlock *table = g_ptr_array_index(blocks, 1);
  g_assert_cmpint(table->kind, ==, GN_MARKDOWN_PANGO_BLOCK_TABLE);
  g_assert_cmpuint(table->columns, ==, 2);
  g_assert_cmpuint(table->rows, ==, 2);
  g_assert_true(table->header);
  g_assert_cmpstr(table->cells[0], ==, "Job");
  g_assert_cmpstr(table->cells[2], ==, "LN Node Daily");
  g_assert_cmpstr(table->cells[3], ==, "<b>gpt-5.6</b>");
  g_assert_null(table->cells[4]);
  g_assert_cmpuint(table->aligns[1], ==, GN_MARKDOWN_ALIGN_RIGHT);
  GnMarkdownPangoBlock *after = g_ptr_array_index(blocks, 2);
  g_assert_cmpstr(after->markup, ==, "after");
  g_autoptr(GnMarkdownDocument) plain = gn_markdown_parse("no | table", -1);
  g_autoptr(GPtrArray) one = gn_markdown_pango_format_blocks(plain, NULL);
  g_assert_false(gn_markdown_pango_blocks_have_table(one));
  g_assert_cmpuint(one->len, ==, 1);
}

/* The owner's bot message (nostrc, fenced tables): a table sent inside a
 * code fence. */
static const char *const fenced_bot =
  "**Muse Glimmer IQ3_M + dflash speculator** \xe2\x80\x94 loaded and running on port **8080**:\n"
  "\n"
  "```\n"
  "| Metric     | Value                          |\n"
  "| ---------- | ------------------------------ |\n"
  "| Gen speed  | 22.3 tok/s                     |\n"
  "| Prompt     | 28.3 tok/s                     |\n"
  "| VRAM       | 17.0 + 15.8 = ~33 GB           |\n"
  "| Speculator | dflash, n_max=3, block_size=16 |\n"
  "```\n"
  "\n"
  "Also a vision model \xe2\x80\x94 ...";

static const GnMarkdownPangoOptions fenced = {
  .mode = GN_MARKDOWN_PANGO_FULL,
  .flags = GN_MARKDOWN_PANGO_TABLE_GRID | GN_MARKDOWN_PANGO_FENCED_TABLES |
           GN_MARKDOWN_PANGO_CODE_BLOCKS,
};

static GPtrArray *
blocks_of(const char *text, const GnMarkdownPangoOptions *options)
{
  g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(text, -1);
  return gn_markdown_pango_format_blocks(doc, options);
}

static void
fenced_table_blocks(void)
{
  g_autoptr(GPtrArray) blocks = blocks_of(fenced_bot, &fenced);
  g_assert_cmpuint(blocks->len, ==, 3);
  GnMarkdownPangoBlock *table = g_ptr_array_index(blocks, 1);
  g_assert_cmpint(table->kind, ==, GN_MARKDOWN_PANGO_BLOCK_TABLE);
  g_assert_cmpuint(table->columns, ==, 2);
  g_assert_cmpuint(table->rows, ==, 5);
  g_assert_true(table->header);
  g_assert_cmpstr(table->cells[0], ==, "Metric");
  g_assert_cmpstr(table->cells[9], ==, "dflash, n_max=3, block_size=16");
  /* Copying keeps the lines as written. */
  g_assert_nonnull(table->source);
  g_assert_true(g_str_has_prefix(table->source, "| Metric     | Value"));
  g_assert_true(g_str_has_suffix(table->source, "block_size=16 |"));
  g_assert_cmpstr(((GnMarkdownPangoBlock *)g_ptr_array_index(blocks, 2))->markup, ==,
                  "Also a vision model \xe2\x80\x94 ...");
  /* An unfenced table has no source. */
  g_autoptr(GPtrArray) plain = blocks_of("| a | b |\n| - | - |\n| 1 | 2 |", &fenced);
  g_assert_null(((GnMarkdownPangoBlock *)g_ptr_array_index(plain, 0))->source);

  /* Without the flag, the fence stays code. */
  const GnMarkdownPangoOptions off = { .flags = GN_MARKDOWN_PANGO_CODE_BLOCKS };
  g_autoptr(GPtrArray) code = blocks_of(fenced_bot, &off);
  g_assert_false(gn_markdown_pango_blocks_have_table(code));
  GnMarkdownPangoBlock *block = g_ptr_array_index(code, 1);
  g_assert_cmpint(block->kind, ==, GN_MARKDOWN_PANGO_BLOCK_CODE);
  g_assert_true(g_str_has_prefix(block->markup, "<tt>| Metric     | Value"));
  assert_valid_markup(block->markup);

  /* ~~~ fences, info strings md/markdown/table, blank lines in the fence,
   * blank lines around it, and a ragged 5-column table are tables. */
  static const char *const tables[] = {
    "~~~\n| a | b |\n|---|---|\n| 1 | 2 |\n~~~",
    "```md\n| a | b |\n|---|---|\n| 1 | 2 |\n```",
    "```Markdown\n| a | b |\n|---|---|\n| 1 | 2 |\n```",
    "```table\n| a | b |\n|---|---|\n| 1 | 2 |\n```",
    "before\n\n\n```\n\n| a | b |\n\n|---|---|\n| 1 | 2 |\n\n```\n\n\nafter",
    "``` \n| # | Pubkey (short) | Operator | Location | Notes |\n"
    "|---|---|---|---|---|\n"
    "| 1 | 3bf0c63f\xe2\x80\xa6 | relay.damus.io | US | very long note that goes on |\n"
    "| 2 | 82341f88 | nos.lol | DE |\n"
    "| 3 | a | b | c | d | e |\n```",
  };
  for (guint i = 0; i < G_N_ELEMENTS(tables); i++) {
    g_autoptr(GPtrArray) b = blocks_of(tables[i], &fenced);
    g_assert_true(gn_markdown_pango_blocks_have_table(b));
    g_autoptr(GnMarkdownDocument) doc = gn_markdown_parse(tables[i], -1);
    g_autofree gchar *why = gn_markdown_explain_no_table(tables[i], -1);
    g_assert_null(why);
    g_autofree gchar *markup = gn_markdown_pango_format(doc, &fenced);
    assert_valid_markup(markup);
    g_assert_nonnull(strstr(markup, "\xe2\x94\x82")); /* the grid's bar */
  }
  g_autoptr(GPtrArray) wide = blocks_of(tables[5], &fenced);
  GnMarkdownPangoBlock *five = g_ptr_array_index(wide, 0);
  g_assert_cmpuint(five->columns, ==, 5);
  g_assert_cmpuint(five->rows, ==, 4);
  g_assert_cmpstr(five->cells[5 + 2], ==, "relay.damus.io");
  g_assert_cmpstr(five->cells[10 + 4], ==, ""); /* padded */
  g_assert_cmpstr(five->cells[15 + 4], ==, "d"); /* extra dropped */

  /* Anything else in a fence stays code. */
  static const char *const code_only[] = {
    "```sh\n| a | b |\n|---|---|\n```",
    "```\n| a | b |\n|---|---|\nplain\n```",
    "```\n| a | b |\n|---|\n```",
    "```\n| a | b |\n```",
    "```\nint x = a | b;\n```",
    "```\n| a | b |\n|---|---|\n```\n```\n|c|\n```",
  };
  for (guint i = 0; i < G_N_ELEMENTS(code_only); i++) {
    g_autoptr(GPtrArray) b = blocks_of(code_only[i], &fenced);
    if (i < 5)
      g_assert_false(gn_markdown_pango_blocks_have_table(b));
    gboolean code_seen = FALSE;
    for (guint k = 0; k < b->len; k++)
      code_seen |= ((GnMarkdownPangoBlock *)g_ptr_array_index(b, k))->kind ==
                   GN_MARKDOWN_PANGO_BLOCK_CODE;
    g_assert_true(code_seen);
  }
}

/* Fenced code is one block, its lines (blank ones too) as written. */
static void
code_blocks(void)
{
  const char *text = "intro\n```c\nint  a;\n\n  <b>x</b>\n```\noutro `inline`";
  g_autoptr(GPtrArray) blocks = blocks_of(text, &fenced);
  g_assert_cmpuint(blocks->len, ==, 3);
  GnMarkdownPangoBlock *code = g_ptr_array_index(blocks, 1);
  g_assert_cmpint(code->kind, ==, GN_MARKDOWN_PANGO_BLOCK_CODE);
  g_assert_cmpstr(code->markup, ==, "<tt>int  a;\n\n  &lt;b&gt;x&lt;/b&gt;</tt>");
  g_assert_cmpstr(((GnMarkdownPangoBlock *)g_ptr_array_index(blocks, 2))->markup, ==,
                  "outro <tt>inline</tt>");
  /* Without CODE_BLOCKS the code stays inline markup, as before. */
  g_autoptr(GPtrArray) inline_code = blocks_of(text, NULL);
  g_assert_cmpuint(inline_code->len, ==, 1);
  g_assert_nonnull(strstr(((GnMarkdownPangoBlock *)g_ptr_array_index(inline_code, 0))->markup,
                          "<tt>int  a;\n\n  &lt;b&gt;x&lt;/b&gt;</tt>"));
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
  g_test_add_func("/nostr-gtk/markdown-pango/table-blocks", table_blocks);
  g_test_add_func("/nostr-gtk/markdown-pango/fenced-table-blocks", fenced_table_blocks);
  g_test_add_func("/nostr-gtk/markdown-pango/code-blocks", code_blocks);
  return g_test_run();
}
