#pragma once

#include <glib.h>

/**
 * Content Renderer - NDB content block-based rendering
 *
 * Replaces manual whitespace tokenization with NDB's pre-parsed content blocks
 * (BLOCK_HASHTAG, BLOCK_MENTION_BECH32, BLOCK_URL, BLOCK_INVOICE, BLOCK_TEXT).
 */

/**
 * GnContentRenderResult:
 *
 * Unified result from a single-pass content render.  Collects Pango markup,
 * media URLs, nostr references, and OG-preview URLs in one block iteration.
 */
typedef struct {
  gchar     *markup;          /* Pango markup (transfer full, non-NULL) */
  GPtrArray *media_urls;      /* image/video URLs by extension (nullable, element: gchar*) */
  GPtrArray *all_urls;        /* ALL http(s) URLs in document order (nullable, element: gchar*) */
  gchar     *first_nostr_ref; /* First nostr: URI for NIP-21 embed (nullable) */
  gchar     *first_og_url;    /* First non-media http(s) URL for OG preview (nullable) */
} GnContentRenderResult;

/**
 * gnostr_render_content:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 *
 * Single-pass NDB block iteration producing markup + extracted URLs.
 *
 * Returns: (transfer full): newly allocated result. Caller must free with
 *          gnostr_content_render_result_free().
 */
GnContentRenderResult *gnostr_render_content(const char *content, int content_len);

/**
 * gnostr_content_render_result_free:
 * @result: (nullable): result to free
 *
 * Frees all fields and the struct itself.
 */
void gnostr_content_render_result_free(GnContentRenderResult *result);

/**
 * gnostr_render_content_markup:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 *
 * Convenience wrapper: returns only the Pango markup string.
 * Equivalent to calling gnostr_render_content() and extracting ->markup.
 *
 * Returns: newly allocated Pango markup string. Caller must g_free().
 */
char *gnostr_render_content_markup(const char *content, int content_len);

/**
 * gnostr_extract_media_urls:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 *
 * Convenience wrapper: returns only image/video URLs.
 *
 * Returns: (transfer full): GPtrArray of URL strings (g_free each + g_ptr_array_unref).
 *          NULL if no media found.
 */
GPtrArray *gnostr_extract_media_urls(const char *content, int content_len);

/**
 * gnostr_strip_zwsp:
 * @str: string to modify in-place (may be NULL)
 *
 * Strips zero-width and invisible Unicode characters that corrupt Pango's
 * internal layout line list: U+200B (ZWS), U+200C (ZWNJ), U+2060 (WJ),
 * U+FEFF (BOM). Does NOT strip U+200D (ZWJ) used in emoji sequences.
 *
 * Returns: @str (same pointer, for chaining)
 */
char *gnostr_strip_zwsp(char *str);
