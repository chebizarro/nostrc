/*
 * nip35_torrents.h - NIP-35 Torrent Event Utilities
 *
 * NIP-35 defines kind 2003 events for sharing BitTorrent content.
 * This module provides utilities for parsing and building torrent events,
 * as well as generating magnet URIs.
 *
 * Event structure:
 * - content: Long description of the torrent (pre-formatted text)
 * - "title" tag: Torrent name/title
 * - "x" tag: V1 BitTorrent infohash (40 hex chars)
 * - "file" tags: File entries with path and optional size
 * - "tracker" tags: Tracker URLs (optional)
 * - "i" tags: External references (tcat, imdb, tmdb, etc.)
 * - "t" tags: Hashtags/categories for searchability
 */

#ifndef NIP35_TORRENTS_H
#define NIP35_TORRENTS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind numbers for torrent-related events */
#define NOSTR_KIND_TORRENT 2003
#define NOSTR_KIND_TORRENT_COMMENT 2004

/**
 * GnostrTorrentFile:
 * Structure representing a single file within a torrent.
 */
typedef struct {
  gchar *path;       /* Full path within torrent (e.g., "info/example.txt") */
  gint64 size;       /* File size in bytes (-1 if unknown) */
} GnostrTorrentFile;

/**
 * GnostrTorrentReference:
 * Structure for external references (i tags).
 * Format: "prefix:value" (e.g., "imdb:tt15239678", "tmdb:movie:693134")
 */
typedef struct {
  gchar *prefix;     /* Reference type (imdb, tmdb, tcat, etc.) */
  gchar *value;      /* Reference value */
} GnostrTorrentReference;

/**
 * GnostrTorrent:
 * Structure containing parsed NIP-35 torrent event data.
 * All strings and arrays are owned by the structure.
 * Free with gnostr_torrent_free().
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;           /* Event ID (hex) */
  gchar *pubkey;             /* Author pubkey (hex) */
  gint64 created_at;         /* Event timestamp */

  /* Torrent data */
  gchar *title;              /* Torrent title from "title" tag */
  gchar *infohash;           /* V1 BitTorrent infohash from "x" tag (40 hex chars) */
  gchar *description;        /* Long description from content field */

  /* Files */
  GnostrTorrentFile **files; /* NULL-terminated array of files */
  gsize files_count;         /* Number of files */
  gint64 total_size;         /* Total size of all files (-1 if unknown) */

  /* Trackers */
  gchar **trackers;          /* NULL-terminated array of tracker URLs */
  gsize trackers_count;      /* Number of trackers */

  /* External references (i tags) */
  GnostrTorrentReference **references;  /* NULL-terminated array */
  gsize references_count;    /* Number of references */

  /* Categories/hashtags (t tags) */
  gchar **categories;        /* NULL-terminated array of categories */
  gsize categories_count;    /* Number of categories */
} GnostrTorrent;

/**
 * gnostr_torrent_new:
 *
 * Creates a new empty torrent structure.
 * Use gnostr_torrent_free() to free.
 *
 * Returns: (transfer full): New torrent structure.
 */
GnostrTorrent *gnostr_torrent_new(void);

/**
 * gnostr_torrent_free:
 * @torrent: The torrent to free, may be NULL.
 *
 * Frees a torrent structure and all its contents.
 */
void gnostr_torrent_free(GnostrTorrent *torrent);

/**
 * gnostr_torrent_file_new:
 * @path: File path within torrent.
 * @size: File size in bytes (-1 if unknown).
 *
 * Creates a new torrent file entry.
 *
 * Returns: (transfer full): New file entry.
 */
GnostrTorrentFile *gnostr_torrent_file_new(const char *path, gint64 size);

/**
 * gnostr_torrent_file_free:
 * @file: The file entry to free, may be NULL.
 *
 * Frees a torrent file entry.
 */
void gnostr_torrent_file_free(GnostrTorrentFile *file);

/**
 * gnostr_torrent_reference_new:
 * @prefix: Reference type prefix (e.g., "imdb", "tmdb").
 * @value: Reference value.
 *
 * Creates a new external reference.
 *
 * Returns: (transfer full): New reference entry.
 */
GnostrTorrentReference *gnostr_torrent_reference_new(const char *prefix, const char *value);

/**
 * gnostr_torrent_reference_free:
 * @ref: The reference to free, may be NULL.
 *
 * Frees an external reference entry.
 */
void gnostr_torrent_reference_free(GnostrTorrentReference *ref);

/**
 * gnostr_torrent_parse_from_json:
 * @event_json: JSON string containing the complete Nostr event.
 *
 * Parses a kind 2003 torrent event from JSON.
 * The JSON should be a complete Nostr event object with id, pubkey,
 * created_at, kind, tags, and content fields.
 *
 * Returns: (transfer full) (nullable): Parsed torrent or NULL on error.
 */
GnostrTorrent *gnostr_torrent_parse_from_json(const char *event_json);

/**
 * gnostr_torrent_parse_tags:
 * @tags_json: JSON array string containing event tags.
 * @content: Event content (description).
 *
 * Parses NIP-35 torrent data from event tags and content.
 * This is a lower-level function; prefer gnostr_torrent_parse_from_json().
 *
 * Returns: (transfer full) (nullable): Parsed torrent or NULL on error.
 */
GnostrTorrent *gnostr_torrent_parse_tags(const char *tags_json, const char *content);

/**
 * gnostr_torrent_build_event:
 * @torrent: Torrent data to serialize.
 * @out_tags_json: (out) (transfer full) (optional): Output for tags JSON array.
 * @out_content: (out) (transfer full) (optional): Output for content string.
 *
 * Builds the tags array and content for a kind 2003 event from torrent data.
 * The caller must handle event signing and finalization.
 *
 * Returns: TRUE if successful, FALSE on error.
 */
gboolean gnostr_torrent_build_event(const GnostrTorrent *torrent,
                                     char **out_tags_json,
                                     char **out_content);

/**
 * gnostr_torrent_generate_magnet:
 * @torrent: Torrent to generate magnet URI for.
 *
 * Generates a magnet URI from torrent data.
 * Format: magnet:?xt=urn:btih:HASH&dn=TITLE&tr=TRACKER1&tr=TRACKER2...
 *
 * Returns: (transfer full) (nullable): Magnet URI or NULL if infohash missing.
 */
gchar *gnostr_torrent_generate_magnet(const GnostrTorrent *torrent);

/**
 * gnostr_torrent_parse_magnet:
 * @magnet_uri: Magnet URI to parse.
 *
 * Parses a magnet URI into a torrent structure.
 * Extracts infohash, display name, and trackers.
 *
 * Returns: (transfer full) (nullable): Parsed torrent or NULL on error.
 */
GnostrTorrent *gnostr_torrent_parse_magnet(const char *magnet_uri);

/**
 * gnostr_torrent_validate_infohash:
 * @infohash: Infohash string to validate.
 *
 * Validates that the infohash is a valid 40-character hex string.
 *
 * Returns: TRUE if valid, FALSE otherwise.
 */
gboolean gnostr_torrent_validate_infohash(const char *infohash);

/**
 * gnostr_torrent_is_torrent_event:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a torrent event (2003).
 */
gboolean gnostr_torrent_is_torrent_event(int kind);

/**
 * gnostr_torrent_is_torrent_comment:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a torrent comment (2004).
 */
gboolean gnostr_torrent_is_torrent_comment(int kind);

/**
 * gnostr_torrent_add_file:
 * @torrent: Torrent to add file to.
 * @path: File path.
 * @size: File size (-1 if unknown).
 *
 * Adds a file entry to the torrent.
 */
void gnostr_torrent_add_file(GnostrTorrent *torrent, const char *path, gint64 size);

/**
 * gnostr_torrent_add_tracker:
 * @torrent: Torrent to add tracker to.
 * @tracker_url: Tracker URL (udp://, http://, https://).
 *
 * Adds a tracker URL to the torrent.
 */
void gnostr_torrent_add_tracker(GnostrTorrent *torrent, const char *tracker_url);

/**
 * gnostr_torrent_add_reference:
 * @torrent: Torrent to add reference to.
 * @prefix: Reference type (imdb, tmdb, tcat, etc.).
 * @value: Reference value.
 *
 * Adds an external reference (i tag) to the torrent.
 */
void gnostr_torrent_add_reference(GnostrTorrent *torrent,
                                   const char *prefix,
                                   const char *value);

/**
 * gnostr_torrent_add_category:
 * @torrent: Torrent to add category to.
 * @category: Category/hashtag (without # prefix).
 *
 * Adds a category/hashtag (t tag) to the torrent.
 */
void gnostr_torrent_add_category(GnostrTorrent *torrent, const char *category);

/**
 * gnostr_torrent_get_reference_url:
 * @ref: Reference to generate URL for.
 *
 * Generates a web URL for the external reference if possible.
 * Supports imdb, tmdb, ttvdb, mal, anilist.
 *
 * Returns: (transfer full) (nullable): URL or NULL if unknown prefix.
 */
gchar *gnostr_torrent_get_reference_url(const GnostrTorrentReference *ref);

/**
 * gnostr_torrent_format_size:
 * @size_bytes: Size in bytes.
 *
 * Formats file size as human-readable string (KB, MB, GB, TB).
 *
 * Returns: (transfer full): Formatted size string.
 */
gchar *gnostr_torrent_format_size(gint64 size_bytes);

G_END_DECLS

#endif /* NIP35_TORRENTS_H */
