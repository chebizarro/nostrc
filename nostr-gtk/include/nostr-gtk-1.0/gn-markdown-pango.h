/* gn-markdown-pango.h — Pango markup for bounded portable Markdown.
 *
 * GTK-free (GLib only) and I/O-free: nothing is fetched, no image is
 * embedded, and no markup from the message reaches the output. A link or
 * image label is never itself a link; its literal target is shown after it.
 * Raw addresses become links only if the host's format_text callback says
 * so, so link policy, mention names and consent stay in the application.
 *
 * Safe on any thread as long as format_text is; the document can be parsed
 * off the UI thread and formatted later.
 */
#pragma once

#include <glib.h>
#include "gn-markdown.h"

G_BEGIN_DECLS

typedef enum {
  GN_MARKDOWN_PANGO_FULL = 0,  /* block structure, sizes, code, tables */
  GN_MARKDOWN_PANGO_SUMMARY    /* one paragraph: whitespace collapsed, no
                                  headings/code blocks/rules/tables, emphasis
                                  kept, link labels only, never linked */
} GnMarkdownPangoMode;

typedef enum {
  GN_MARKDOWN_PANGO_HEADING_SIZES = 1 << 0, /* xx-large..medium bold; off = <b> */
  GN_MARKDOWN_PANGO_GLYPH_BLOCKS  = 1 << 1, /* "│ " quote, "────" rule, flush
                                               lists; off = italic dimmed quote,
                                               dimmed "---" rule, indented lists */
  GN_MARKDOWN_PANGO_TABLE_GRID    = 1 << 2, /* aligned monospace grid; off =
                                               cells joined by " | " */
} GnMarkdownPangoFlags;

/* Called (FULL mode only) for plain text and every raw address: text tokens,
 * RAW_URL, NOSTR_REFERENCE and link/image targets, never labels or code.
 * Append Pango markup for the whole of @text to @out and return TRUE, or
 * return FALSE (appending nothing) to have it escaped literally. Must not
 * perform I/O. */
typedef gboolean (*GnMarkdownLinkFormatFunc)(GString *out, const gchar *text,
                                             gpointer user_data);

typedef struct {
  GnMarkdownPangoMode mode;
  guint flags;                          /* GnMarkdownPangoFlags */
  gsize max_chars;                      /* visible characters, 0 = no cap; a
                                           cut ends with "…" and closes tags */
  GnMarkdownLinkFormatFunc format_text; /* nullable: everything literal */
  gpointer user_data;
  /*< private >*/
  gpointer reserved[4];                 /* zero; room for growth */
} GnMarkdownPangoOptions;

/* Markup for a parsed document; NULL options = FULL, no flags. A truncated
 * document is formatted as far as it was parsed: use
 * gn_markdown_pango_from_text() for the escaped full-text fallback. */
gchar *gn_markdown_pango_format(const GnMarkdownDocument *document,
                                const GnMarkdownPangoOptions *options);

/* Parses @text and formats it. When the parser's input or token budget is
 * reached, the whole text is shown escaped instead (never silently cut). */
gchar *gn_markdown_pango_from_text(const gchar *text,
                                   const GnMarkdownPangoOptions *options);

/* The text a reader sees, without markup or decorative glyphs, for
 * accessible descriptions. @max_chars 0 = no cap. */
gchar *gn_markdown_plain_text(const GnMarkdownDocument *document, gsize max_chars);

/* The target of the first image, or NULL. Never fetches: whether to load it
 * is the caller's (consent) decision. */
gchar *gn_markdown_dup_first_image_target(const GnMarkdownDocument *document);

G_END_DECLS
