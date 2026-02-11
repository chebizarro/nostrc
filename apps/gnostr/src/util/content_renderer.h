#pragma once

#include <glib.h>

/**
 * Content Renderer - NDB content block-based rendering
 *
 * Replaces manual whitespace tokenization with NDB's pre-parsed content blocks
 * (BLOCK_HASHTAG, BLOCK_MENTION_BECH32, BLOCK_URL, BLOCK_INVOICE, BLOCK_TEXT).
 */

/**
 * gnostr_render_content_markup:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 *
 * Parses content into NDB blocks and renders to Pango markup.
 * Handles: hashtags (#tag), mentions (nostr:npub/nprofile/note/nevent/naddr),
 * URLs (http/https), invoices (lnbc), and plain text.
 *
 * Returns: newly allocated Pango markup string. Caller must g_free().
 */
char *gnostr_render_content_markup(const char *content, int content_len);

/**
 * gnostr_extract_media_urls:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 *
 * Extracts image and video URLs from content blocks.
 *
 * Returns: (transfer full): GPtrArray of URL strings (g_free each + g_ptr_array_unref).
 *          NULL if no media found.
 */
GPtrArray *gnostr_extract_media_urls(const char *content, int content_len);

/**
 * gnostr_strip_zwsp:
 * @str: string to modify in-place (may be NULL)
 *
 * Strips Zero-Width Space (U+200B) characters from a string in-place.
 * ZWS in Pango text/markup corrupts PangoLayout's internal line list,
 * causing SEGV in pango_layout_line_unref during gtk_widget_allocate.
 *
 * Returns: @str (same pointer, for chaining)
 */
char *gnostr_strip_zwsp(char *str);
