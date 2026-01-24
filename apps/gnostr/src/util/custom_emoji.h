/**
 * @file custom_emoji.h
 * @brief NIP-30 Custom Emoji tag parser and cache
 *
 * Parses emoji tags from Nostr events to support custom emoji shortcodes:
 * - Tag format: ["emoji", "shortcode", "url"]
 * - Usage: :shortcode: in content gets replaced with inline image
 *
 * Example tag: ["emoji", "soapbox", "https://example.com/soapbox.png"]
 * Example content: "Hello :soapbox: world" renders with custom emoji inline
 */

#ifndef GNOSTR_CUSTOM_EMOJI_H
#define GNOSTR_CUSTOM_EMOJI_H

#include <glib.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>

G_BEGIN_DECLS

/**
 * Parsed emoji tag data structure
 */
typedef struct {
  char *shortcode;    /* Shortcode without colons (e.g., "soapbox") */
  char *url;          /* Image URL for the emoji */
} GnostrCustomEmoji;

/**
 * List of parsed emoji entries from an event
 */
typedef struct {
  GnostrCustomEmoji **items;
  size_t count;
  size_t capacity;
} GnostrEmojiList;

/**
 * gnostr_custom_emoji_new:
 * @shortcode: the shortcode (without colons)
 * @url: the image URL
 *
 * Creates a new custom emoji structure.
 *
 * Returns: (transfer full): Newly allocated GnostrCustomEmoji, free with gnostr_custom_emoji_free()
 */
GnostrCustomEmoji *gnostr_custom_emoji_new(const char *shortcode, const char *url);

/**
 * gnostr_custom_emoji_free:
 * @emoji: (transfer full) (nullable): emoji to free
 *
 * Frees a custom emoji structure and all its fields.
 */
void gnostr_custom_emoji_free(GnostrCustomEmoji *emoji);

/**
 * gnostr_emoji_list_new:
 *
 * Creates a new empty emoji list.
 *
 * Returns: (transfer full): Newly allocated list, free with gnostr_emoji_list_free()
 */
GnostrEmojiList *gnostr_emoji_list_new(void);

/**
 * gnostr_emoji_list_free:
 * @list: (transfer full) (nullable): list to free
 *
 * Frees an emoji list and all contained emoji entries.
 */
void gnostr_emoji_list_free(GnostrEmojiList *list);

/**
 * gnostr_emoji_list_append:
 * @list: list to append to
 * @emoji: (transfer full): emoji entry to append (ownership transferred)
 *
 * Appends an emoji entry to the list.
 */
void gnostr_emoji_list_append(GnostrEmojiList *list, GnostrCustomEmoji *emoji);

/**
 * gnostr_emoji_parse_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses all emoji tags from a JSON tags array.
 * Looks for tags in format: ["emoji", "shortcode", "url"]
 *
 * Returns: (transfer full) (nullable): List of parsed emoji entries
 */
GnostrEmojiList *gnostr_emoji_parse_tags_json(const char *tags_json);

/**
 * gnostr_emoji_find_by_shortcode:
 * @list: list to search
 * @shortcode: shortcode to find (without colons)
 *
 * Finds an emoji entry by its shortcode.
 *
 * Returns: (transfer none) (nullable): Found emoji or NULL
 */
GnostrCustomEmoji *gnostr_emoji_find_by_shortcode(GnostrEmojiList *list, const char *shortcode);

/**
 * gnostr_emoji_replace_shortcodes:
 * @content: the content text containing :shortcode: patterns
 * @emoji_list: list of custom emojis to use for replacement
 *
 * Replaces :shortcode: patterns in content with Pango markup image tags.
 * For GTK4, we use special markup that will be processed during rendering.
 *
 * NOTE: GTK labels don't support inline images in markup directly.
 * This function returns markup with placeholder spans that can be
 * post-processed or the caller can use a custom rendering approach.
 *
 * Returns: (transfer full): New string with shortcodes replaced, or NULL if no replacements needed
 */
gchar *gnostr_emoji_replace_shortcodes(const char *content, GnostrEmojiList *emoji_list);

/* ========== Emoji Image Cache ========== */

/**
 * Emoji cache metrics for monitoring.
 */
typedef struct {
  guint64 requests_total;     /* total emoji image fetch attempts */
  guint64 mem_cache_hits;     /* in-memory texture cache hits */
  guint64 disk_cache_hits;    /* disk cache hits promoted to memory */
  guint64 http_start;         /* HTTP fetches started */
  guint64 http_ok;            /* HTTP fetches successfully completed */
  guint64 http_error;         /* HTTP fetches failed */
  guint64 cache_write_error;  /* errors writing fetched bytes to disk */
} GnostrEmojiCacheMetrics;

/**
 * gnostr_emoji_cache_prefetch:
 * @url: URL of the emoji image to prefetch
 *
 * Prefetches an emoji image and stores it in cache without any UI.
 * Use this to warm the cache when parsing emoji tags.
 */
void gnostr_emoji_cache_prefetch(const char *url);

/**
 * gnostr_emoji_try_load_cached:
 * @url: URL of the emoji image
 *
 * Tries to load an emoji texture from cache (memory or disk).
 *
 * Returns: (transfer full) (nullable): GdkTexture if cached, NULL otherwise
 */
GdkTexture *gnostr_emoji_try_load_cached(const char *url);

/**
 * gnostr_emoji_cache_metrics_get:
 * @out: (out): metrics structure to fill
 *
 * Gets current emoji cache metrics.
 */
void gnostr_emoji_cache_metrics_get(GnostrEmojiCacheMetrics *out);

/**
 * gnostr_emoji_cache_metrics_log:
 *
 * Logs current emoji cache metrics.
 */
void gnostr_emoji_cache_metrics_log(void);

G_END_DECLS

#endif /* GNOSTR_CUSTOM_EMOJI_H */
