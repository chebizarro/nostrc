#ifndef APPS_GNOSTR_UTIL_CONTENT_RENDERER_H
#define APPS_GNOSTR_UTIL_CONTENT_RENDERER_H

#include <glib.h>
#include <nostr-gtk-1.0/nostr-gtk-error.h>

/**
 * Content Renderer - NDB content block-based rendering
 *
 * Replaces manual whitespace tokenization with NDB's pre-parsed content blocks
 * (BLOCK_HASHTAG, BLOCK_MENTION_BECH32, BLOCK_URL, BLOCK_INVOICE, BLOCK_TEXT).
 */

/**
 * GnContentDescriptorType:
 *
 * Structured rich-content classes emitted in source order by
 * gn_content_parse().
 */
typedef enum {
  GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE,
  GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO,
  GN_CONTENT_DESCRIPTOR_LINK_PREVIEW,
  GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF,
  GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF,
} GnContentDescriptorType;

/**
 * GnContentDescriptor:
 * @type: descriptor class
 * @url: media/link URL, when applicable
 * @original: original nostr: URI for reference descriptors
 * @id: event id hex, or naddr identifier
 * @pubkey: profile/author pubkey hex, when available
 * @relay_hints: (array zero-terminated=1) (element-type utf8): relay hints
 * @width: NIP-92 width, or 0
 * @height: NIP-92 height, or 0
 * @thumbnail_url: NIP-92 thumb/image URL, when available
 */
typedef struct GnContentDescriptor {
  GnContentDescriptorType type;
  gchar *url;
  gchar *original;
  gchar *id;
  gchar *pubkey;
  gchar **relay_hints;
  gint width;
  gint height;
  gchar *thumbnail_url;
} GnContentDescriptor;

/**
 * GnContentRenderResult:
 *
 * Unified result from a single-pass content parse. Collects elided Pango
 * markup, the corresponding plain display text, and ordered descriptors.
 * Legacy extraction fields remain populated for existing render consumers.
 */
typedef struct GnContentRenderResult {
  gchar     *markup;          /* elided Pango markup (transfer full, non-NULL) */
  gchar     *plain_text;      /* same elided display text without escaping */
  GPtrArray *descriptors;     /* ordered GnContentDescriptor* array (non-NULL) */
  GPtrArray *media_urls;      /* legacy image/video URLs (nullable, element: gchar*) */
  GPtrArray *all_urls;        /* legacy http(s) URLs (nullable, element: gchar*) */
  gchar     *first_nostr_ref; /* legacy first event nostr: URI (nullable) */
  gchar     *first_og_url;    /* legacy first non-media URL (nullable) */
  gboolean   used_block_fallback;
} GnContentRenderResult;

/**
 * gn_content_parse:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 * @tags_json: (nullable): event tags JSON used for NIP-92 enrichment
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Parses content once into elided markup/plain text plus typed descriptors.
 * Media URLs and event references are elided; normal links and profile
 * mentions remain inline.
 *
 * Returns: (transfer full) (nullable): parsed result, or %NULL on error.
 */
GnContentRenderResult *gn_content_parse(const char *content,
                                        int content_len,
                                        const char *tags_json,
                                        GError **error);

struct ndb_blocks;

/**
 * gn_content_parse_with_blocks:
 * @blocks: (transfer none): persisted nostrdb blocks whose offsets address
 *   @content
 *
 * Renders from an existing block stream without invoking ndb_parse_content().
 * Cached block slices are sanitized independently before entering markup so
 * offsets remain relative to the stored bytes.
 */
GnContentRenderResult *gn_content_parse_with_blocks(const char *content,
                                                    int content_len,
                                                    const char *tags_json,
                                                    struct ndb_blocks *blocks,
                                                    GError **error);

/* Lightweight instrumentation used by parse-once pipeline tests. */
void  gn_content_parser_reset_invocation_count(void);
guint gn_content_parser_get_invocation_count(void);

void gn_content_descriptor_free(GnContentDescriptor *descriptor);

/**
 * gnostr_render_content:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Compatibility wrapper over gn_content_parse() without event tags.
 *
 * Returns: (transfer full) (nullable): newly allocated result, or %NULL on
 *          error. Caller must free with gnostr_content_render_result_free().
 */
GnContentRenderResult *gnostr_render_content(const char *content, int content_len,
                                              GError **error);

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
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Convenience wrapper: returns only the Pango markup string.
 * Equivalent to calling gnostr_render_content() and extracting ->markup.
 *
 * Returns: (nullable): newly allocated Pango markup string, or %NULL on
 *          error. Caller must g_free().
 */
char *gnostr_render_content_markup(const char *content, int content_len,
                                    GError **error);

/**
 * gnostr_extract_media_urls:
 * @content: raw note content string
 * @content_len: length of content (-1 for strlen)
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Convenience wrapper: returns only image/video URLs.
 *
 * Returns: (transfer full) (nullable): GPtrArray of URL strings
 *          (g_free each + g_ptr_array_unref), or %NULL if no media found
 *          or on error.
 */
GPtrArray *gnostr_extract_media_urls(const char *content, int content_len,
                                      GError **error);

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

/**
 * gnostr_sanitize_utf8:
 * @str: input string (may contain invalid UTF-8)
 *
 * Validates UTF-8 and replaces invalid sequences with U+FFFD.
 * Also strips dangerous zero-width characters via gnostr_strip_zwsp().
 *
 * Returns: (transfer full): newly allocated valid UTF-8 string.
 *          Caller must g_free().
 */
char *gnostr_sanitize_utf8(const char *str);

/**
 * gnostr_safe_set_markup:
 * @label: a #GtkLabel (must include gtk/gtk.h before using this)
 * @markup: Pango markup string (may be invalid/malformed)
 *
 * Safely sets markup on a GtkLabel. Validates the markup with
 * pango_parse_markup() first — if it fails (malformed tags, invalid
 * UTF-8, etc.), falls back to gtk_label_set_text() with the raw
 * text extracted by stripping tags.
 *
 * This is the PRIMARY defense against relay-sourced content crashing
 * Pango during layout or finalization.
 *
 * Note: This is a static inline to avoid pulling GTK into the
 * content_renderer header. Include <gtk/gtk.h> before this header
 * to get the implementation.
 */
#ifdef GTK_LABEL
static inline void
gnostr_safe_set_markup(GtkLabel *label, const char *markup)
{
  if (!label || !GTK_IS_LABEL(label)) return;
  if (!markup || !*markup) {
    gtk_label_set_text(label, "");
    return;
  }

  /* First sanitize UTF-8 */
  g_autofree char *clean = gnostr_sanitize_utf8(markup);

  /* Try parsing - if it fails, fall back to plain text */
  GError *err = NULL;
  if (pango_parse_markup(clean, -1, 0, NULL, NULL, NULL, &err)) {
    gtk_label_set_markup(label, clean);
  } else {
    g_debug("gnostr_safe_set_markup: invalid markup, falling back to text: %s",
            err->message);
    g_clear_error(&err);

    /* nostrc-csaf: Cached regex patterns for fallback path performance.
     * Thread-local static ensures one-time init per thread without races. */
    static GRegex *tag_re = NULL, *amp_re = NULL, *lt_re = NULL;
    static GRegex *gt_re = NULL, *quot_re = NULL, *apos_re = NULL;
    if (G_UNLIKELY(tag_re == NULL)) {
      tag_re = g_regex_new("<[^>]*>", 0, 0, NULL);
      amp_re = g_regex_new("&amp;", 0, 0, NULL);
      lt_re = g_regex_new("&lt;", 0, 0, NULL);
      gt_re = g_regex_new("&gt;", 0, 0, NULL);
      quot_re = g_regex_new("&quot;", 0, 0, NULL);
      apos_re = g_regex_new("&apos;", 0, 0, NULL);
    }

    /* Strip all XML/Pango tags and set as plain text */
    g_autofree char *plaintext = NULL;
    if (tag_re) {
      plaintext = g_regex_replace_literal(tag_re, clean, -1, 0, "", 0, NULL);
    }

    /* Un-escape all common XML entities for the plain text fallback */
    if (plaintext) {
      g_autofree char *t1 = NULL, *t2 = NULL, *t3 = NULL, *t4 = NULL, *t5 = NULL;
      const char *cur = plaintext;

      if (amp_re) {
        t1 = g_regex_replace_literal(amp_re, cur, -1, 0, "&", 0, NULL);
        cur = t1;
      }
      if (lt_re && cur) {
        t2 = g_regex_replace_literal(lt_re, cur, -1, 0, "<", 0, NULL);
        cur = t2;
      }
      if (gt_re && cur) {
        t3 = g_regex_replace_literal(gt_re, cur, -1, 0, ">", 0, NULL);
        cur = t3;
      }
      if (quot_re && cur) {
        t4 = g_regex_replace_literal(quot_re, cur, -1, 0, "\"", 0, NULL);
        cur = t4;
      }
      if (apos_re && cur) {
        t5 = g_regex_replace_literal(apos_re, cur, -1, 0, "'", 0, NULL);
        cur = t5;
      }

      gtk_label_set_text(label, cur ? cur : plaintext);
    } else {
      gtk_label_set_text(label, clean);
    }
  }
}
#endif /* GTK_LABEL */

#endif /* APPS_GNOSTR_UTIL_CONTENT_RENDERER_H */
