/*
 * nip_b0_bookmarks.h - NIP-B0 Web Bookmarks Utilities
 *
 * NIP-B0 (0xB0 = 176) defines kind 176 for web bookmark events -
 * allowing users to save and sync web bookmarks across devices via Nostr relays.
 *
 * Web Bookmark Event Structure:
 * - kind: 176 (0xB0)
 * - content: Optional description or notes about the bookmark
 * - tags:
 *   - ["r", "<url>"] - the bookmarked URL (required)
 *   - ["title", "<title>"] - page title
 *   - ["description", "<desc>"] - page description
 *   - ["image", "<image-url>"] - preview image URL
 *   - ["t", "<tag>"] - tags/categories (repeatable)
 *   - ["published_at", "<timestamp>"] - when originally saved
 */

#ifndef NIP_B0_BOOKMARKS_H
#define NIP_B0_BOOKMARKS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for web bookmark events (0xB0 = 176) */
#define NIPB0_KIND_BOOKMARK 176

/*
 * GnostrWebBookmark:
 * Structure containing parsed NIP-B0 web bookmark data.
 * All strings are owned by the structure and freed with gnostr_web_bookmark_free().
 */
typedef struct {
  /* Required field */
  gchar *url;             /* Bookmarked URL (from "r" tag) */

  /* Optional metadata */
  gchar *title;           /* Page title (from "title" tag) */
  gchar *description;     /* Page description (from "description" tag) */
  gchar *image;           /* Preview image URL (from "image" tag) */
  gchar *notes;           /* User notes (from content field) */

  /* Tags/categories */
  gchar **tags;           /* Array of tag strings (from "t" tags) */
  gsize tag_count;        /* Number of tags */

  /* Timestamps */
  gint64 published_at;    /* When originally saved (from "published_at" tag) */
  gint64 created_at;      /* Event created_at timestamp */

  /* Event metadata (for parsed events) */
  gchar *event_id;        /* Event ID (hex) */
  gchar *pubkey;          /* Creator's pubkey (hex) */
} GnostrWebBookmark;

/*
 * gnostr_web_bookmark_new:
 *
 * Creates a new empty web bookmark structure.
 * Use gnostr_web_bookmark_free() to free.
 *
 * Returns: (transfer full): New web bookmark structure.
 */
GnostrWebBookmark *gnostr_web_bookmark_new(void);

/*
 * gnostr_web_bookmark_free:
 * @bookmark: The bookmark to free, may be NULL.
 *
 * Frees a web bookmark structure and all its contents.
 */
void gnostr_web_bookmark_free(GnostrWebBookmark *bookmark);

/*
 * gnostr_web_bookmark_parse_json:
 * @event_json: JSON string of kind 176 event.
 *
 * Parses a web bookmark event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed bookmark or NULL on error.
 */
GnostrWebBookmark *gnostr_web_bookmark_parse_json(const char *event_json);

/*
 * gnostr_web_bookmark_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @content: The event content (notes/description).
 *
 * Parses NIP-B0 specific tags from an event's tags array.
 *
 * Returns: (transfer full) (nullable): Parsed bookmark or NULL on error.
 */
GnostrWebBookmark *gnostr_web_bookmark_parse_tags(const char *tags_json,
                                                   const char *content);

/*
 * gnostr_web_bookmark_build_tags:
 * @bookmark: The bookmark to build tags for.
 *
 * Builds a JSON array string of tags for publishing a bookmark event.
 * The resulting JSON is suitable for inclusion in an event's "tags" field.
 *
 * Returns: (transfer full) (nullable): JSON array string or NULL on error.
 *          Caller must free with g_free().
 */
gchar *gnostr_web_bookmark_build_tags(const GnostrWebBookmark *bookmark);

/*
 * gnostr_web_bookmark_build_event_json:
 * @bookmark: The bookmark to build an event for.
 *
 * Builds an unsigned web bookmark event JSON for signing.
 * Caller must sign the event before publishing.
 *
 * Returns: (transfer full) (nullable): Unsigned event JSON or NULL on error.
 *          Caller must free with g_free().
 */
gchar *gnostr_web_bookmark_build_event_json(const GnostrWebBookmark *bookmark);

/*
 * gnostr_web_bookmark_validate_url:
 * @url: URL string to validate.
 *
 * Validates that a URL is well-formed and uses a supported scheme
 * (http or https).
 *
 * Returns: TRUE if the URL is valid, FALSE otherwise.
 */
gboolean gnostr_web_bookmark_validate_url(const char *url);

/*
 * gnostr_web_bookmark_add_tag:
 * @bookmark: The bookmark to add a tag to.
 * @tag: The tag string to add.
 *
 * Adds a tag/category to the bookmark. The tag is duplicated internally.
 * Duplicate tags are silently ignored.
 */
void gnostr_web_bookmark_add_tag(GnostrWebBookmark *bookmark, const char *tag);

/*
 * gnostr_web_bookmark_remove_tag:
 * @bookmark: The bookmark to remove a tag from.
 * @tag: The tag string to remove.
 *
 * Removes a tag/category from the bookmark.
 *
 * Returns: TRUE if the tag was found and removed, FALSE otherwise.
 */
gboolean gnostr_web_bookmark_remove_tag(GnostrWebBookmark *bookmark, const char *tag);

/*
 * gnostr_web_bookmark_has_tag:
 * @bookmark: The bookmark to check.
 * @tag: The tag string to look for.
 *
 * Checks if the bookmark has a specific tag.
 *
 * Returns: TRUE if the bookmark has the tag, FALSE otherwise.
 */
gboolean gnostr_web_bookmark_has_tag(const GnostrWebBookmark *bookmark, const char *tag);

/*
 * gnostr_web_bookmark_copy:
 * @bookmark: The bookmark to copy.
 *
 * Creates a deep copy of a bookmark.
 *
 * Returns: (transfer full) (nullable): Copy of the bookmark or NULL on error.
 */
GnostrWebBookmark *gnostr_web_bookmark_copy(const GnostrWebBookmark *bookmark);

G_END_DECLS

#endif /* NIP_B0_BOOKMARKS_H */
