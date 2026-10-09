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
  GN_MARKDOWN_LINE_BREAK
} GnMarkdownTokenKind;

typedef enum {
  GN_MARKDOWN_STYLE_NONE = 0,
  GN_MARKDOWN_STYLE_EMPHASIS = 1 << 0,
  GN_MARKDOWN_STYLE_STRONG = 1 << 1
} GnMarkdownStyle;

typedef struct {
  GnMarkdownTokenKind kind;
  gchar *text;                 /* valid UTF-8, never Pango markup */
  gchar *target;               /* URL/nostr URI for link/reference tokens */
  guint style;                 /* GnMarkdownStyle flags */
  guint level;                 /* heading level 1..6, else 0 */
  gboolean ordered;            /* list item */
} GnMarkdownToken;

typedef struct {
  GPtrArray *tokens;           /* GnMarkdownToken*, in display order */
  gchar *source;               /* bounded, valid UTF-8 source */
  gboolean truncated;          /* input/token budget was reached */
} GnMarkdownDocument;

#define GN_MARKDOWN_MAX_INPUT_BYTES (64u * 1024u)
#define GN_MARKDOWN_MAX_TOKENS 4096u
#define GN_MARKDOWN_MAX_DEPTH 8u

GnMarkdownDocument *gn_markdown_parse(const gchar *input, gssize length);
void gn_markdown_document_free(GnMarkdownDocument *document);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnMarkdownDocument, gn_markdown_document_free)

G_END_DECLS
#endif
