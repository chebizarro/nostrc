/*
 * markdown_pango.h - Simple Markdown to Pango Markup Converter
 *
 * Converts common Markdown syntax to Pango markup for GTK label rendering.
 * Used by NIP-23 long-form content display.
 *
 * Supported markdown syntax:
 * - **bold** and __bold__
 * - *italic* and _italic_
 * - `inline code`
 * - # Heading 1 through ###### Heading 6
 * - [link text](url)
 * - > blockquote
 * - Horizontal rules (---, ***, ___)
 * - Line breaks preserved
 */

#ifndef MARKDOWN_PANGO_H
#define MARKDOWN_PANGO_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * markdown_to_pango:
 * @markdown: Input markdown text
 * @max_length: Maximum output length (0 for unlimited). If content exceeds
 *              this, it will be truncated with "..." appended.
 *
 * Converts markdown text to Pango markup suitable for GtkLabel.
 * Special characters are escaped, and markdown syntax is converted
 * to Pango span attributes.
 *
 * Returns: (transfer full): Newly allocated Pango markup string.
 *          Caller must free with g_free().
 */
char *markdown_to_pango(const char *markdown, gsize max_length);

/*
 * markdown_to_pango_summary:
 * @markdown: Input markdown text
 * @max_chars: Maximum number of characters in output
 *
 * Converts markdown to Pango but strips most formatting,
 * useful for article summaries/previews.
 * Preserves bold and italic but removes headings, links become
 * plain text, etc.
 *
 * Returns: (transfer full): Newly allocated Pango markup string.
 */
char *markdown_to_pango_summary(const char *markdown, gsize max_chars);

/*
 * markdown_extract_first_image:
 * @markdown: Input markdown text
 *
 * Extracts the URL of the first image found in markdown.
 * Looks for ![alt](url) syntax.
 *
 * Returns: (transfer full) (nullable): Image URL or NULL if not found.
 */
char *markdown_extract_first_image(const char *markdown);

/*
 * markdown_strip_to_plain:
 * @markdown: Input markdown text
 * @max_length: Maximum output length
 *
 * Strips all markdown formatting and returns plain text.
 * Useful for search indexing or accessibility text.
 *
 * Returns: (transfer full): Plain text string.
 */
char *markdown_strip_to_plain(const char *markdown, gsize max_length);

G_END_DECLS

#endif /* MARKDOWN_PANGO_H */
