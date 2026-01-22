/**
 * @file imeta.h
 * @brief NIP-92 imeta tag parser for inline media attachments
 *
 * Parses imeta tags from Nostr events to extract media metadata:
 * - url: Media URL (required)
 * - m: MIME type (e.g., "image/jpeg")
 * - dim: Dimensions as "WIDTHxHEIGHT"
 * - alt: Alt text for accessibility
 * - x: SHA-256 hash of media content
 * - blurhash: Blurhash placeholder string
 * - fallback: Alternative URLs
 */

#ifndef GNOSTR_IMETA_H
#define GNOSTR_IMETA_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

G_BEGIN_DECLS

/**
 * Media type classification based on MIME type
 */
typedef enum {
  GNOSTR_MEDIA_TYPE_UNKNOWN = 0,
  GNOSTR_MEDIA_TYPE_IMAGE,
  GNOSTR_MEDIA_TYPE_VIDEO,
  GNOSTR_MEDIA_TYPE_AUDIO,
} GnostrMediaType;

/**
 * Parsed imeta tag data structure
 */
typedef struct {
  char *url;           /* Primary media URL (required) */
  char *mime_type;     /* MIME type, e.g., "image/jpeg" */
  int width;           /* Width from dim field, 0 if not set */
  int height;          /* Height from dim field, 0 if not set */
  char *alt;           /* Alt text for accessibility */
  char *sha256;        /* SHA-256 hash (hex) */
  char *blurhash;      /* Blurhash string for placeholder */
  char **fallback_urls; /* NULL-terminated array of fallback URLs */
  size_t fallback_count; /* Number of fallback URLs */
  GnostrMediaType media_type; /* Derived from MIME type */
} GnostrImeta;

/**
 * List of parsed imeta entries from an event
 */
typedef struct {
  GnostrImeta **items;
  size_t count;
  size_t capacity;
} GnostrImetaList;

/**
 * gnostr_imeta_new:
 *
 * Creates a new empty imeta structure.
 *
 * Returns: (transfer full): Newly allocated GnostrImeta, free with gnostr_imeta_free()
 */
GnostrImeta *gnostr_imeta_new(void);

/**
 * gnostr_imeta_free:
 * @imeta: (transfer full) (nullable): imeta to free
 *
 * Frees an imeta structure and all its fields.
 */
void gnostr_imeta_free(GnostrImeta *imeta);

/**
 * gnostr_imeta_list_new:
 *
 * Creates a new empty imeta list.
 *
 * Returns: (transfer full): Newly allocated list, free with gnostr_imeta_list_free()
 */
GnostrImetaList *gnostr_imeta_list_new(void);

/**
 * gnostr_imeta_list_free:
 * @list: (transfer full) (nullable): list to free
 *
 * Frees an imeta list and all contained imeta entries.
 */
void gnostr_imeta_list_free(GnostrImetaList *list);

/**
 * gnostr_imeta_list_append:
 * @list: list to append to
 * @imeta: (transfer full): imeta entry to append (ownership transferred)
 *
 * Appends an imeta entry to the list.
 */
void gnostr_imeta_list_append(GnostrImetaList *list, GnostrImeta *imeta);

/**
 * gnostr_imeta_parse_tag:
 * @tag_values: NULL-terminated array of tag values (first element is "imeta")
 * @n_values: number of elements in the array
 *
 * Parses a single imeta tag into a GnostrImeta structure.
 * The tag format is: ["imeta", "url <url>", "m <mime>", "dim WxH", ...]
 *
 * Returns: (transfer full) (nullable): Parsed imeta or NULL on error
 */
GnostrImeta *gnostr_imeta_parse_tag(const char **tag_values, size_t n_values);

/**
 * gnostr_imeta_parse_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses all imeta tags from a JSON tags array.
 *
 * Returns: (transfer full) (nullable): List of parsed imeta entries
 */
GnostrImetaList *gnostr_imeta_parse_tags_json(const char *tags_json);

/**
 * gnostr_imeta_find_by_url:
 * @list: list to search
 * @url: URL to find
 *
 * Finds an imeta entry by its URL.
 *
 * Returns: (transfer none) (nullable): Found imeta or NULL
 */
GnostrImeta *gnostr_imeta_find_by_url(GnostrImetaList *list, const char *url);

/**
 * gnostr_imeta_get_media_type:
 * @mime_type: MIME type string (e.g., "image/jpeg")
 *
 * Determines the media type from a MIME type string.
 *
 * Returns: Media type classification
 */
GnostrMediaType gnostr_imeta_get_media_type(const char *mime_type);

/**
 * gnostr_imeta_get_media_type_from_url:
 * @url: URL to analyze
 *
 * Attempts to determine media type from URL extension as fallback.
 *
 * Returns: Media type classification
 */
GnostrMediaType gnostr_imeta_get_media_type_from_url(const char *url);

G_END_DECLS

#endif /* GNOSTR_IMETA_H */
