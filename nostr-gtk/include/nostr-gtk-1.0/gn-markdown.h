#ifndef GN_MARKDOWN_H
#define GN_MARKDOWN_H

#include <glib.h>

G_BEGIN_DECLS

/* The parser is GTK-free. No token contains markup or causes I/O. */
typedef enum {
  GN_MARKDOWN_TEXT,
  GN_MARKDOWN_CODE,
  GN_MARKDOWN_RAW_URL,
  GN_MARKDOWN_LINK,
  GN_MARKDOWN_NOSTR_REFERENCE,
  GN_MARKDOWN_HEADING,
  GN_MARKDOWN_LIST_ITEM,
  GN_MARKDOWN_QUOTE,
  GN_MARKDOWN_SEPARATOR,
  GN_MARKDOWN_LINE_BREAK,
  /* GFM extensions (appended: earlier values are stable). */
  GN_MARKDOWN_IMAGE,           /* ![alt](target): never fetched; text = alt */
  GN_MARKDOWN_TABLE_CELL       /* one cell of a GFM pipe table, see below */
} GnMarkdownTokenKind;

typedef enum {
  GN_MARKDOWN_ALIGN_NONE = 0,
  GN_MARKDOWN_ALIGN_LEFT,
  GN_MARKDOWN_ALIGN_CENTER,
  GN_MARKDOWN_ALIGN_RIGHT
} GnMarkdownAlign;

typedef enum {
  GN_MARKDOWN_TASK_NONE = 0,
  GN_MARKDOWN_TASK_OPEN,       /* - [ ] item */
  GN_MARKDOWN_TASK_DONE        /* - [x] item */
} GnMarkdownTask;

typedef enum {
  GN_MARKDOWN_STYLE_NONE = 0,
  GN_MARKDOWN_STYLE_EMPHASIS = 1 << 0,
  GN_MARKDOWN_STYLE_STRONG = 1 << 1,
  GN_MARKDOWN_STYLE_STRIKETHROUGH = 1 << 2  /* ~~text~~ */
} GnMarkdownStyle;

/* Block tokens (HEADING, LIST_ITEM, QUOTE, TABLE_CELL) carry their raw inline
 * source in text; gn_markdown_parse_inline() tokenizes it (emphasis, code,
 * links) for display.
 *
 * Tables: a header row, a delimiter row (---, :--, --:, :-:) and body rows,
 * each row's cells as consecutive TABLE_CELL tokens with level = column
 * (0-based, every row padded or cut to the delimiter's column count),
 * ordered = TRUE in the header row, align = GnMarkdownAlign of the column
 * and columns = the column count. Rows are separated by LINE_BREAK tokens;
 * the delimiter row emits nothing. */

typedef struct {
  GnMarkdownTokenKind kind;
  gchar *text;                 /* valid UTF-8, never Pango markup */
  gchar *target;               /* URL/nostr URI for link/reference tokens */
  guint style;                 /* GnMarkdownStyle flags */
  guint level;                 /* heading level 1..6; ordered list number;
                                  table column; else 0 */
  gboolean ordered;            /* list item; table header cell */
  /* Appended fields (GFM extensions). */
  guint indent;                /* list item nesting depth, 0 = top level */
  guint task;                  /* list item GnMarkdownTask */
  guint align;                 /* table cell GnMarkdownAlign */
  guint columns;               /* table cell: columns in its table */
} GnMarkdownToken;

typedef struct {
  GPtrArray *tokens;           /* GnMarkdownToken*, in display order */
  gchar *source;               /* bounded, valid UTF-8 source */
  gboolean truncated;          /* input/token budget was reached */
} GnMarkdownDocument;

#define GN_MARKDOWN_MAX_INPUT_BYTES (64u * 1024u)
#define GN_MARKDOWN_MAX_TOKENS 4096u
#define GN_MARKDOWN_MAX_DEPTH 8u
#define GN_MARKDOWN_MAX_TABLE_COLUMNS 32u
#define GN_MARKDOWN_MAX_LIST_INDENT 8u

GnMarkdownDocument *gn_markdown_parse(const gchar *input, gssize length);
/* Inline tokens only (TEXT, CODE, LINK, IMAGE, RAW_URL, NOSTR_REFERENCE):
 * no block structure, used for the text of block tokens. */
GnMarkdownDocument *gn_markdown_parse_inline(const gchar *input, gssize length);
void gn_markdown_document_free(GnMarkdownDocument *document);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnMarkdownDocument, gn_markdown_document_free)

G_END_DECLS
#endif
