/**
 * @file nip68_picture.h
 * @brief NIP-68 Picture-first Feeds Parser
 *
 * NIP-68 defines kind 20 events for picture-first posts (like Instagram).
 * These events contain image media as the primary content.
 *
 * Required structure:
 * - kind: 20
 * - content: Caption/description text
 * - tags: imeta tags for image metadata (per NIP-92)
 *
 * Supported tags:
 * - "imeta": Image metadata (url, m, dim, alt, x, blurhash, fallback)
 * - "p": Mentioned pubkeys
 * - "t": Hashtags/topics
 * - "expiration": Unix timestamp when post expires (NIP-40)
 * - "content-warning": Content warning label (NIP-36)
 *
 * Event content field contains the caption text for the picture(s).
 */

#ifndef GNOSTR_NIP68_PICTURE_H
#define GNOSTR_NIP68_PICTURE_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include "imeta.h"

G_BEGIN_DECLS

/* Kind number for picture events */
#define NOSTR_KIND_PICTURE 20

/**
 * GnostrPictureImage:
 * Image entry in a picture event with metadata from imeta tags.
 */
typedef struct {
  char *url;           /* Primary image URL */
  char *mime_type;     /* MIME type, e.g., "image/jpeg" */
  int width;           /* Image width (0 if not specified) */
  int height;          /* Image height (0 if not specified) */
  char *alt;           /* Alt text for accessibility */
  char *sha256;        /* SHA-256 hash (hex) */
  char *blurhash;      /* Blurhash placeholder string */
  char **fallback_urls; /* NULL-terminated array of fallback URLs */
  size_t fallback_count; /* Number of fallback URLs */
} GnostrPictureImage;

/**
 * GnostrPictureMeta:
 * Parsed NIP-68 picture event metadata.
 * All strings are owned by the structure and freed with gnostr_picture_meta_free().
 */
typedef struct {
  char *event_id;       /* Event ID (hex) */
  char *pubkey;         /* Author pubkey (hex) */
  char *caption;        /* Caption text (from content field) */
  gint64 created_at;    /* Event creation timestamp */

  /* Images from imeta tags */
  GnostrPictureImage **images;
  size_t image_count;

  /* Hashtags from t tags */
  char **hashtags;
  size_t hashtag_count;

  /* Mentioned pubkeys from p tags */
  char **mentions;
  size_t mention_count;

  /* Optional metadata */
  char *content_warning; /* Content warning label (NIP-36) */
  gint64 expiration;     /* Expiration timestamp (NIP-40), 0 if not set */

  /* Reaction counts (populated by caller) */
  int like_count;        /* Number of kind 7 reactions */
  int zap_count;         /* Number of zaps */
  gint64 zap_amount;     /* Total zap amount in sats */
  int reply_count;       /* Number of replies */
  int repost_count;      /* Number of reposts */
} GnostrPictureMeta;

/**
 * gnostr_picture_image_new:
 *
 * Creates a new empty picture image structure.
 *
 * Returns: (transfer full): New picture image.
 */
GnostrPictureImage *gnostr_picture_image_new(void);

/**
 * gnostr_picture_image_free:
 * @image: The image to free, may be NULL.
 *
 * Frees a picture image structure and all its contents.
 */
void gnostr_picture_image_free(GnostrPictureImage *image);

/**
 * gnostr_picture_meta_new:
 *
 * Creates a new empty picture metadata structure.
 *
 * Returns: (transfer full): New picture metadata.
 */
GnostrPictureMeta *gnostr_picture_meta_new(void);

/**
 * gnostr_picture_meta_free:
 * @meta: The metadata to free, may be NULL.
 *
 * Frees a picture metadata structure and all its contents.
 */
void gnostr_picture_meta_free(GnostrPictureMeta *meta);

/**
 * gnostr_picture_meta_copy:
 * @meta: The metadata to copy.
 *
 * Creates a deep copy of picture metadata.
 *
 * Returns: (transfer full) (nullable): Copy of the metadata or NULL on error.
 */
GnostrPictureMeta *gnostr_picture_meta_copy(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_parse_event:
 * @event_id: Event ID (hex string).
 * @pubkey: Author pubkey (hex string).
 * @content: Event content (caption text).
 * @tags_json: JSON array string containing event tags.
 * @created_at: Event creation timestamp.
 *
 * Parses a NIP-68 picture event into a GnostrPictureMeta structure.
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrPictureMeta *gnostr_picture_parse_event(const char *event_id,
                                               const char *pubkey,
                                               const char *content,
                                               const char *tags_json,
                                               gint64 created_at);

/**
 * gnostr_picture_is_picture:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a picture event (kind 20).
 */
gboolean gnostr_picture_is_picture(int kind);

/**
 * gnostr_picture_get_primary_image:
 * @meta: Picture metadata.
 *
 * Gets the first/primary image from the picture event.
 *
 * Returns: (transfer none) (nullable): Primary image or NULL if no images.
 */
const GnostrPictureImage *gnostr_picture_get_primary_image(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_get_thumbnail_url:
 * @meta: Picture metadata.
 *
 * Gets the URL of the primary image for use as a thumbnail.
 * Useful for grid views.
 *
 * Returns: (transfer none) (nullable): Image URL or NULL if no images.
 */
const char *gnostr_picture_get_thumbnail_url(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_get_aspect_ratio:
 * @meta: Picture metadata.
 *
 * Calculates aspect ratio of the primary image.
 *
 * Returns: Aspect ratio (width/height) or 1.0 if dimensions unknown.
 */
double gnostr_picture_get_aspect_ratio(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_has_content_warning:
 * @meta: Picture metadata.
 *
 * Checks if the picture has a content warning.
 *
 * Returns: TRUE if content warning is set.
 */
gboolean gnostr_picture_has_content_warning(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_is_expired:
 * @meta: Picture metadata.
 *
 * Checks if the picture event has expired (NIP-40).
 *
 * Returns: TRUE if expired.
 */
gboolean gnostr_picture_is_expired(const GnostrPictureMeta *meta);

/**
 * gnostr_picture_build_nevent:
 * @meta: Picture metadata.
 * @relays: (nullable): NULL-terminated array of relay URLs.
 *
 * Builds a NIP-19 nevent bech32 string for referencing this picture.
 *
 * Returns: (transfer full) (nullable): Bech32 nevent string or NULL on error.
 */
char *gnostr_picture_build_nevent(const GnostrPictureMeta *meta,
                                   const char **relays);

/**
 * gnostr_picture_format_caption:
 * @caption: Raw caption text.
 * @max_length: Maximum length for truncation (0 for no limit).
 *
 * Formats the caption text for display, handling newlines and truncation.
 *
 * Returns: (transfer full): Formatted caption string.
 */
char *gnostr_picture_format_caption(const char *caption, size_t max_length);

/**
 * gnostr_picture_get_all_image_urls:
 * @meta: Picture metadata.
 * @count: (out): Number of URLs returned.
 *
 * Gets all image URLs from the picture event (for gallery view).
 *
 * Returns: (transfer full) (array zero-terminated=1): NULL-terminated array
 *          of image URLs. Free with g_strfreev().
 */
char **gnostr_picture_get_all_image_urls(const GnostrPictureMeta *meta,
                                          size_t *count);

G_END_DECLS

#endif /* GNOSTR_NIP68_PICTURE_H */
