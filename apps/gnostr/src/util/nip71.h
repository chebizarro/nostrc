/*
 * nip71.h - NIP-71 Video Events Utilities
 *
 * NIP-71 defines video events with kind 34235 (horizontal) and 34236 (vertical).
 * This module provides utilities for parsing and extracting video metadata
 * from event tags.
 *
 * Required tags for kind 34235/34236:
 * - "url" - video URL
 * - "m" - MIME type (e.g., "video/mp4")
 *
 * Optional tags:
 * - "x" - SHA-256 hash of the video file
 * - "thumb" - thumbnail image URL
 * - "title" - video title
 * - "summary" - video description
 * - "duration" - duration in seconds
 * - "dim" - dimensions as "WxH" (e.g., "1920x1080")
 * - "size" - file size in bytes
 * - "blurhash" - blurhash string for placeholder
 * - "t" - hashtags/topics (multiple allowed)
 * - "d" - unique identifier for addressable events
 */

#ifndef NIP71_H
#define NIP71_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind numbers for video events */
#define NOSTR_KIND_VIDEO_HORIZONTAL 34235
#define NOSTR_KIND_VIDEO_VERTICAL   34236

/*
 * GnostrVideoOrientation:
 * Video orientation based on event kind.
 */
typedef enum {
  GNOSTR_VIDEO_HORIZONTAL = 0,
  GNOSTR_VIDEO_VERTICAL = 1
} GnostrVideoOrientation;

/*
 * GnostrVideoMeta:
 * Structure containing parsed NIP-71 video metadata.
 * All strings are owned by the structure and freed with gnostr_video_meta_free().
 */
typedef struct {
  gchar *d_tag;           /* Unique identifier (for addressable events) */
  gchar *url;             /* Video URL (required) */
  gchar *mime_type;       /* MIME type (e.g., "video/mp4") */
  gchar *file_hash;       /* SHA-256 hash ("x" tag) */
  gchar *thumb_url;       /* Thumbnail image URL */
  gchar *title;           /* Video title */
  gchar *summary;         /* Video description */
  gint64 duration;        /* Duration in seconds (0 if not specified) */
  gint width;             /* Video width (0 if not specified) */
  gint height;            /* Video height (0 if not specified) */
  gint64 size;            /* File size in bytes (0 if not specified) */
  gchar *blurhash;        /* Blurhash placeholder string */
  gchar **hashtags;       /* NULL-terminated array of hashtags (without #) */
  gsize hashtags_count;   /* Number of hashtags */
  GnostrVideoOrientation orientation; /* Horizontal or vertical */
  gint64 published_at;    /* Publication timestamp (0 if not specified) */
} GnostrVideoMeta;

/*
 * gnostr_video_meta_new:
 *
 * Creates a new empty video metadata structure.
 * Use gnostr_video_meta_free() to free.
 *
 * Returns: (transfer full): New video metadata.
 */
GnostrVideoMeta *gnostr_video_meta_new(void);

/*
 * gnostr_video_meta_free:
 * @meta: The metadata to free, may be NULL.
 *
 * Frees a video metadata structure and all its contents.
 */
void gnostr_video_meta_free(GnostrVideoMeta *meta);

/*
 * gnostr_video_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @kind: Event kind (34235 or 34236) to determine orientation.
 *
 * Parses NIP-71 specific tags from an event's tags array.
 * The tags_json should be the JSON representation of the tags array.
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrVideoMeta *gnostr_video_parse_tags(const char *tags_json, int kind);

/*
 * gnostr_video_is_video:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a video event (34235 or 34236).
 */
gboolean gnostr_video_is_video(int kind);

/*
 * gnostr_video_is_horizontal:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is horizontal video (34235).
 */
gboolean gnostr_video_is_horizontal(int kind);

/*
 * gnostr_video_is_vertical:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is vertical video (34236).
 */
gboolean gnostr_video_is_vertical(int kind);

/*
 * gnostr_video_format_duration:
 * @duration_seconds: Duration in seconds.
 *
 * Formats duration as human-readable string (e.g., "3:45" or "1:23:45").
 *
 * Returns: (transfer full): Formatted duration string.
 */
char *gnostr_video_format_duration(gint64 duration_seconds);

/*
 * gnostr_video_build_naddr:
 * @kind: Event kind (34235 or 34236).
 * @pubkey_hex: Author's public key in hex format.
 * @d_tag: The "d" tag value (for addressable events).
 * @relays: NULL-terminated array of relay URLs (may be NULL).
 *
 * Builds a NIP-19 naddr bech32 string for referencing this video.
 *
 * Returns: (transfer full) (nullable): Bech32 naddr string or NULL on error.
 */
char *gnostr_video_build_naddr(int kind, const char *pubkey_hex,
                                const char *d_tag, const char **relays);

/*
 * gnostr_video_build_a_tag:
 * @kind: Event kind.
 * @pubkey_hex: Author's public key.
 * @d_tag: The "d" tag value.
 *
 * Builds an "a" tag value for referencing this video.
 * Format: "kind:pubkey:d-tag"
 *
 * Returns: (transfer full): "a" tag value string.
 */
char *gnostr_video_build_a_tag(int kind, const char *pubkey_hex,
                                const char *d_tag);

/*
 * gnostr_video_event_create_tags:
 * @meta: Video metadata.
 *
 * Creates a JSON array string of tags for a video event.
 * Useful when creating new video events.
 *
 * Returns: (transfer full) (nullable): JSON array string or NULL on error.
 */
char *gnostr_video_event_create_tags(const GnostrVideoMeta *meta);

/*
 * gnostr_video_detect_mime_type:
 * @file_path: Path to the video file.
 *
 * Detects MIME type from video file extension.
 * Common video types: video/mp4, video/webm, video/quicktime, etc.
 *
 * Returns: MIME type string (static, do not free), or NULL if not a recognized video format.
 */
const char *gnostr_video_detect_mime_type(const char *file_path);

/*
 * gnostr_video_is_video_mime:
 * @mime_type: MIME type string to check.
 *
 * Checks if a MIME type is a video format.
 *
 * Returns: TRUE if this is a video MIME type.
 */
gboolean gnostr_video_is_video_mime(const char *mime_type);

/*
 * gnostr_video_detect_orientation:
 * @width: Video width.
 * @height: Video height.
 *
 * Determines video orientation based on dimensions.
 *
 * Returns: GNOSTR_VIDEO_VERTICAL if height > width, else GNOSTR_VIDEO_HORIZONTAL.
 */
GnostrVideoOrientation gnostr_video_detect_orientation(int width, int height);

/*
 * gnostr_video_get_event_kind:
 * @orientation: Video orientation.
 *
 * Gets the appropriate NIP-71 event kind for the orientation.
 *
 * Returns: 34235 for horizontal, 34236 for vertical.
 */
int gnostr_video_get_event_kind(GnostrVideoOrientation orientation);

G_END_DECLS

#endif /* NIP71_H */
