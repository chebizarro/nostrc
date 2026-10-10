/* gn-markdown-pango.c — Pango markup for bounded portable Markdown.
 *
 * One emitter for every host: the safe Groundhog token formatter (literal
 * targets, host-decided links, GFM tables and lists) with the gnostr article
 * presentation (sized headings, dimmed quotes and rules, summary mode, a
 * visible-character cap) as options. See gn-markdown-pango.h.
 */
#include "gn-markdown-pango.h"

#include <string.h>

typedef struct {
  const GnMarkdownPangoOptions *options;
  gboolean summary;
  gboolean prev_space;  /* summary: last emitted visible char was a space */
} Ctx;

static void
append_text(GString *out, const gchar *text, gboolean linkify, const Ctx *ctx)
{
  text = text ? text : "";
  if (linkify && !ctx->summary && ctx->options->format_text &&
      ctx->options->format_text(out, text, ctx->options->user_data))
    return;
  g_autofree gchar *escaped = g_markup_escape_text(text, -1);
  g_string_append(out, escaped);
}

/* Summary text: runs of whitespace become one space. */
static void
append_collapsed(GString *out, const gchar *text, Ctx *ctx)
{
  for (const gchar *p = text ? text : ""; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isspace(c)) {
      if (!ctx->prev_space)
        g_string_append_c(out, ' ');
      ctx->prev_space = TRUE;
      continue;
    }
    gchar buf[8];
    gint len = g_unichar_to_utf8(c, buf);
    g_autofree gchar *escaped = g_markup_escape_text(buf, len);
    g_string_append(out, escaped);
    ctx->prev_space = FALSE;
  }
}

static void
append_styled(GString *out, const gchar *text, guint style, gboolean linkify, Ctx *ctx)
{
  if (style & GN_MARKDOWN_STYLE_STRONG)
    g_string_append(out, "<b>");
  if (style & GN_MARKDOWN_STYLE_EMPHASIS)
    g_string_append(out, "<i>");
  if (style & GN_MARKDOWN_STYLE_STRIKETHROUGH)
    g_string_append(out, "<s>");
  if (ctx->summary)
    append_collapsed(out, text, ctx);
  else
    append_text(out, text, linkify, ctx);
  if (style & GN_MARKDOWN_STYLE_STRIKETHROUGH)
    g_string_append(out, "</s>");
  if (style & GN_MARKDOWN_STYLE_EMPHASIS)
    g_string_append(out, "</i>");
  if (style & GN_MARKDOWN_STYLE_STRONG)
    g_string_append(out, "</b>");
}

/* An inline token (text, code, link, image, address); FALSE for others. */
static gboolean
format_inline_token(GString *out, const GnMarkdownToken *token, Ctx *ctx)
{
  const gchar *value = token->text ? token->text : "";
  switch (token->kind) {
  case GN_MARKDOWN_TEXT:
  case GN_MARKDOWN_RAW_URL:
  case GN_MARKDOWN_NOSTR_REFERENCE:
    append_styled(out, value, token->style, TRUE, ctx);
    return TRUE;
  case GN_MARKDOWN_CODE:
    if (ctx->summary) {
      append_collapsed(out, value, ctx);
      return TRUE;
    }
    g_string_append(out, "<tt>");
    append_text(out, value, FALSE, ctx);
    g_string_append(out, "</tt>");
    return TRUE;
  case GN_MARKDOWN_LINK:
  case GN_MARKDOWN_IMAGE:
    /* A label can say anything, so it must never disguise the destination;
     * an image is never fetched: its description and address are shown.
     * A summary shows the label only. */
    append_styled(out, value, token->style, FALSE, ctx);
    if (ctx->summary)
      return TRUE;
    g_string_append(out, " (");
    append_text(out, token->target, TRUE, ctx);
    g_string_append_c(out, ')');
    return TRUE;
  default:
    return FALSE;
  }
}

/* The inline Markdown of a block (heading, list item, quote, table cell). */
static void
append_block_inline(GString *out, const gchar *text, Ctx *ctx)
{
  g_autoptr(GnMarkdownDocument) inline_doc = gn_markdown_parse_inline(text, -1);
  for (guint i = 0; i < inline_doc->tokens->len; i++)
    format_inline_token(out, g_ptr_array_index(inline_doc->tokens, i), ctx);
}

/* Display columns of the text this markup shows (tags dropped, an entity
 * one character): a wide character takes two, a combining one none. */
static guint
markup_columns(const gchar *markup)
{
  guint columns = 0;
  for (const gchar *p = markup; *p;) {
    if (*p == '<') {
      const gchar *close = strchr(p, '>');
      if (!close)
        break;
      p = close + 1;
      continue;
    }
    if (*p == '&') {
      const gchar *semi = strchr(p, ';');
      columns++;
      p = semi ? semi + 1 : p + 1;
      continue;
    }
    gunichar c = g_utf8_get_char(p);
    columns += g_unichar_iszerowidth(c) ? 0 : g_unichar_iswide(c) ? 2 : 1;
    p = g_utf8_next_char(p);
  }
  return columns;
}

static void
append_spaces(GString *out, guint count)
{
  for (guint i = 0; i < count; i++)
    g_string_append_c(out, ' ');
}

/* Walks the table starting at @start, formatting its cells into @cells
 * when given; returns the index of the first token after the table. */
static guint
collect_table(const GnMarkdownDocument *document, guint start, guint columns,
              GPtrArray *cells, guint *aligns, Ctx *ctx)
{
  guint i = start;
  while (i < document->tokens->len) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    if (token->kind == GN_MARKDOWN_LINE_BREAK && i + 1 < document->tokens->len) {
      const GnMarkdownToken *after = g_ptr_array_index(document->tokens, i + 1);
      if (after->kind == GN_MARKDOWN_TABLE_CELL && after->level == 0 &&
          after->columns == columns && !after->ordered) {
        i++;
        continue;
      }
    }
    if (token->kind != GN_MARKDOWN_TABLE_CELL || token->columns != columns ||
        (i > start && token->ordered && token->level == 0))
      break;
    if (cells) {
      GString *cell = g_string_new(NULL);
      append_block_inline(cell, token->text ? token->text : "", ctx);
      aligns[MIN(token->level, columns - 1)] = token->align;
      g_ptr_array_add(cells, g_string_free(cell, FALSE));
    }
    i++;
  }
  return i;
}

/* A GFM table as an aligned monospace grid (one label, so the host keeps
 * one widget and its render cache), or as rows of cells joined by a bar.
 * Cells keep their inline formatting and the literal-address rule. */
static guint
format_table(GString *out, const GnMarkdownDocument *document, guint start, Ctx *ctx)
{
  const GnMarkdownToken *first = g_ptr_array_index(document->tokens, start);
  guint columns = MAX(first->columns, 1);
  g_autoptr(GPtrArray) cells = g_ptr_array_new_with_free_func(g_free);
  g_autofree guint *widths = g_new0(guint, columns);
  g_autofree guint *aligns = g_new0(guint, columns);
  gboolean header = first->ordered;
  gboolean grid = (ctx->options->flags & GN_MARKDOWN_PANGO_TABLE_GRID) != 0;
  guint end = collect_table(document, start, columns, cells, aligns, ctx);
  guint rows = cells->len / columns;
  for (guint k = 0; k < rows * columns; k++)
    widths[k % columns] = MAX(widths[k % columns],
                              markup_columns(g_ptr_array_index(cells, k)));
  if (grid)
    g_string_append(out, "<tt>");
  for (guint r = 0; r < rows; r++) {
    if (r)
      g_string_append_c(out, '\n');
    for (guint c = 0; c < columns; c++) {
      const gchar *cell = g_ptr_array_index(cells, r * columns + c);
      guint pad = grid ? widths[c] - markup_columns(cell) : 0;
      guint before = aligns[c] == GN_MARKDOWN_ALIGN_RIGHT ? pad
                   : aligns[c] == GN_MARKDOWN_ALIGN_CENTER ? pad / 2 : 0;
      if (c)
        g_string_append(out, grid ? " │ " : " | ");
      append_spaces(out, before);
      if (header && r == 0)
        g_string_append(out, "<b>");
      g_string_append(out, cell);
      if (header && r == 0)
        g_string_append(out, "</b>");
      if (c + 1 < columns)
        append_spaces(out, pad - before);
    }
    if (grid && header && r == 0) {
      g_string_append_c(out, '\n');
      for (guint c = 0; c < columns; c++) {
        if (c)
          g_string_append(out, "─┼─");
        for (guint k = 0; k < widths[c]; k++)
          g_string_append(out, "─");
      }
    }
  }
  if (grid)
    g_string_append(out, "</tt>");
  return end;
}

static const GnMarkdownToken *
token_at(const GnMarkdownDocument *document, guint i)
{
  return i < document->tokens->len ? g_ptr_array_index(document->tokens, i) : NULL;
}

/* A fenced code line: a CODE token alone on its line. */
static gboolean
is_code_line(const GnMarkdownDocument *document, guint i)
{
  const GnMarkdownToken *token = token_at(document, i);
  const GnMarkdownToken *before = i ? token_at(document, i - 1) : NULL;
  const GnMarkdownToken *after = token_at(document, i + 1);
  return token && token->kind == GN_MARKDOWN_CODE &&
         (!before || before->kind == GN_MARKDOWN_LINE_BREAK) &&
         (!after || after->kind == GN_MARKDOWN_LINE_BREAK);
}

/* Consecutive code lines are one <tt> run: a code block reads as one
 * monospace block. Returns the index after the last code line. */
static guint
format_code_block(GString *out, const GnMarkdownDocument *document, guint i, Ctx *ctx)
{
  g_string_append(out, "<tt>");
  while (TRUE) {
    append_text(out, token_at(document, i)->text, FALSE, ctx);
    i++;
    if (!token_at(document, i) || !is_code_line(document, i + 1))
      break;
    g_string_append_c(out, '\n');
    i++;
  }
  g_string_append(out, "</tt>");
  return i;
}

static const gchar *
heading_size(guint level)
{
  switch (level) {
  case 1: return "xx-large";
  case 2: return "x-large";
  case 3: return "large";
  default: return "medium";
  }
}

static void
format_full(GString *out, const GnMarkdownDocument *document, Ctx *ctx)
{
  guint flags = ctx->options->flags;
  gboolean glyphs = (flags & GN_MARKDOWN_PANGO_GLYPH_BLOCKS) != 0;
  gboolean sizes = (flags & GN_MARKDOWN_PANGO_HEADING_SIZES) != 0;
  guint i = 0;
  while (i < document->tokens->len) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    const gchar *value = token->text ? token->text : "";
    if (token->kind == GN_MARKDOWN_TABLE_CELL) {
      i = format_table(out, document, i, ctx);
      continue;
    }
    if (is_code_line(document, i)) {
      i = format_code_block(out, document, i, ctx);
      continue;
    }
    i++;
    if (format_inline_token(out, token, ctx))
      continue;
    switch (token->kind) {
    case GN_MARKDOWN_HEADING:
      if (sizes)
        g_string_append_printf(out, "<span size=\"%s\" weight=\"bold\">",
                               heading_size(token->level));
      else
        g_string_append(out, "<b>");
      append_block_inline(out, value, ctx);
      g_string_append(out, sizes ? "</span>" : "</b>");
      break;
    case GN_MARKDOWN_LIST_ITEM:
      append_spaces(out, 2 * token->indent + (glyphs ? 0 : 2));
      if (token->task == GN_MARKDOWN_TASK_DONE)
        g_string_append(out, "☑ ");
      else if (token->task == GN_MARKDOWN_TASK_OPEN)
        g_string_append(out, "☐ ");
      else if (token->ordered)
        g_string_append_printf(out, "%u. ", token->level);
      else
        g_string_append(out, token->indent ? "◦ " : "• ");
      append_block_inline(out, value, ctx);
      break;
    case GN_MARKDOWN_QUOTE:
      g_string_append(out, glyphs ? "│ " : "<span alpha=\"80%\" style=\"italic\">");
      append_block_inline(out, value, ctx);
      if (!glyphs)
        g_string_append(out, "</span>");
      break;
    case GN_MARKDOWN_SEPARATOR:
      g_string_append(out, glyphs ? "────────" : "<span alpha=\"50%\">---</span>");
      break;
    case GN_MARKDOWN_LINE_BREAK:
      g_string_append_c(out, '\n');
      break;
    default:
      break;
    }
  }
}

static void
format_summary(GString *out, const GnMarkdownDocument *document, Ctx *ctx)
{
  ctx->prev_space = TRUE;  /* no leading space */
  guint i = 0;
  while (i < document->tokens->len) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    if (token->kind == GN_MARKDOWN_TABLE_CELL) {
      i = collect_table(document, i, MAX(token->columns, 1), NULL, NULL, ctx);
      continue;
    }
    if (is_code_line(document, i)) {
      i++;
      continue;
    }
    i++;
    if (format_inline_token(out, token, ctx))
      continue;
    switch (token->kind) {
    case GN_MARKDOWN_LIST_ITEM:
    case GN_MARKDOWN_QUOTE:
      append_block_inline(out, token->text, ctx);
      break;
    case GN_MARKDOWN_LINE_BREAK:
      append_collapsed(out, " ", ctx);
      break;
    default:  /* headings and rules are not part of a summary */
      break;
    }
  }
  if (out->len && out->str[out->len - 1] == ' ')
    g_string_truncate(out, out->len - 1);
}

/* Cuts @markup after @max_chars visible characters (an entity is one), then
 * appends an ellipsis and closes every tag still open. Never splits a tag,
 * an entity or a UTF-8 sequence. */
static void
cap_markup(GString *markup, gsize max_chars)
{
  if (!max_chars)
    return;
  g_autoptr(GPtrArray) open = g_ptr_array_new_with_free_func(g_free);
  gsize visible = 0;
  const gchar *p = markup->str;
  while (*p) {
    if (*p == '<') {
      const gchar *close = strchr(p, '>');
      if (!close)
        break;
      if (p[1] == '/') {
        if (open->len)
          g_ptr_array_remove_index(open, open->len - 1);
      } else {
        g_ptr_array_add(open, g_strndup(p + 1, strcspn(p + 1, " >")));
      }
      p = close + 1;
      continue;
    }
    if (visible == max_chars)
      break;
    if (*p == '&') {
      const gchar *semi = strchr(p, ';');
      p = semi ? semi + 1 : p + 1;
    } else {
      p = g_utf8_next_char(p);
    }
    visible++;
  }
  if (!*p)
    return;
  g_string_truncate(markup, p - markup->str);
  g_string_append(markup, "…");
  for (guint i = open->len; i > 0; i--)
    g_string_append_printf(markup, "</%s>", (const gchar *)g_ptr_array_index(open, i - 1));
}

static const GnMarkdownPangoOptions default_options = { 0 };

gchar *
gn_markdown_pango_format(const GnMarkdownDocument *document,
                         const GnMarkdownPangoOptions *options)
{
  if (!document || !document->tokens)
    return g_strdup("");
  Ctx ctx = { options ? options : &default_options, FALSE, FALSE };
  ctx.summary = ctx.options->mode == GN_MARKDOWN_PANGO_SUMMARY;
  GString *out = g_string_sized_new(document->source ? strlen(document->source) + 64 : 64);
  if (ctx.summary)
    format_summary(out, document, &ctx);
  else
    format_full(out, document, &ctx);
  cap_markup(out, ctx.options->max_chars);
  return g_string_free(out, FALSE);
}

gchar *
gn_markdown_pango_from_text(const gchar *text, const GnMarkdownPangoOptions *options)
{
  g_autoptr(GnMarkdownDocument) document = gn_markdown_parse(text, -1);
  if (!document->truncated)
    return gn_markdown_pango_format(document, options);
  /* A budgeted parser must not silently drop text: show all of it, escaped. */
  g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
  g_autofree gchar *escaped = g_markup_escape_text(valid, -1);
  GString *out = g_string_new(escaped);
  cap_markup(out, options ? options->max_chars : 0);
  return g_string_free(out, FALSE);
}

static void
append_plain_inline(GString *out, const GnMarkdownToken *token)
{
  if ((token->kind == GN_MARKDOWN_IMAGE || token->kind == GN_MARKDOWN_LINK) &&
      (!token->text || !*token->text)) {
    if (token->target)
      g_string_append(out, token->target);
    return;
  }
  if (token->text)
    g_string_append(out, token->text);
}

static gboolean
is_block_with_inline(const GnMarkdownToken *token)
{
  return token->kind == GN_MARKDOWN_HEADING || token->kind == GN_MARKDOWN_LIST_ITEM ||
         token->kind == GN_MARKDOWN_QUOTE || token->kind == GN_MARKDOWN_TABLE_CELL;
}

gchar *
gn_markdown_plain_text(const GnMarkdownDocument *document, gsize max_chars)
{
  if (!document || !document->tokens)
    return g_strdup("");
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < document->tokens->len; i++) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    if (is_block_with_inline(token)) {
      if (token->kind == GN_MARKDOWN_TABLE_CELL && token->level)
        g_string_append(out, ", ");
      g_autoptr(GnMarkdownDocument) inline_doc = gn_markdown_parse_inline(token->text, -1);
      for (guint j = 0; j < inline_doc->tokens->len; j++)
        append_plain_inline(out, g_ptr_array_index(inline_doc->tokens, j));
    } else if (token->kind != GN_MARKDOWN_SEPARATOR) {
      append_plain_inline(out, token);
    }
  }
  if (max_chars && g_utf8_strlen(out->str, -1) > (glong)max_chars) {
    const gchar *cut = g_utf8_offset_to_pointer(out->str, (glong)max_chars);
    g_string_truncate(out, cut - out->str);
    g_string_append(out, "…");
  }
  return g_string_free(out, FALSE);
}

static gchar *
first_image_in(const GnMarkdownDocument *document)
{
  for (guint i = 0; i < document->tokens->len; i++) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    if (token->kind == GN_MARKDOWN_IMAGE && token->target && *token->target)
      return g_strdup(token->target);
    if (is_block_with_inline(token)) {
      g_autoptr(GnMarkdownDocument) inline_doc = gn_markdown_parse_inline(token->text, -1);
      gchar *found = first_image_in(inline_doc);
      if (found)
        return found;
    }
  }
  return NULL;
}

gchar *
gn_markdown_dup_first_image_target(const GnMarkdownDocument *document)
{
  return document && document->tokens ? first_image_in(document) : NULL;
}
