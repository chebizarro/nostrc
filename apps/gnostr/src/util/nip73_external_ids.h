/**
 * NIP-73 External Content IDs for gnostr
 *
 * Implements parsing and URL generation for external content identifiers.
 * The "i" tag format: ["i", "<type>:<id>"]
 *
 * Supported identifier types:
 *   - isbn: Books (ISBN-10/13) -> OpenLibrary, Goodreads
 *   - doi: Academic papers -> doi.org
 *   - imdb: Movies/TV shows -> IMDB
 *   - tmdb: Movies -> TMDB
 *   - spotify: Music (track, album, artist, playlist) -> Spotify
 *   - youtube: Videos -> YouTube
 *   - podcast:guid: Podcasts -> Podcast Index
 */

#ifndef GNOSTR_NIP73_EXTERNAL_IDS_H
#define GNOSTR_NIP73_EXTERNAL_IDS_H

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * NIP-73 External Content Types
 */
typedef enum {
  GNOSTR_NIP73_TYPE_UNKNOWN = 0,
  GNOSTR_NIP73_TYPE_ISBN,           /* Books: isbn:978-0-13-468599-1 */
  GNOSTR_NIP73_TYPE_DOI,            /* Papers: doi:10.1000/xyz123 */
  GNOSTR_NIP73_TYPE_IMDB,           /* Movies/TV: imdb:tt0111161 */
  GNOSTR_NIP73_TYPE_TMDB,           /* Movies: tmdb:movie/278 or tmdb:tv/1396 */
  GNOSTR_NIP73_TYPE_SPOTIFY,        /* Music: spotify:track:xxx, spotify:album:xxx, etc. */
  GNOSTR_NIP73_TYPE_YOUTUBE,        /* Videos: youtube:dQw4w9WgXcQ */
  GNOSTR_NIP73_TYPE_PODCAST_GUID    /* Podcasts: podcast:guid:xxxxxx */
} GnostrNip73Type;

/**
 * Spotify content subtypes
 */
typedef enum {
  GNOSTR_NIP73_SPOTIFY_UNKNOWN = 0,
  GNOSTR_NIP73_SPOTIFY_TRACK,
  GNOSTR_NIP73_SPOTIFY_ALBUM,
  GNOSTR_NIP73_SPOTIFY_ARTIST,
  GNOSTR_NIP73_SPOTIFY_PLAYLIST,
  GNOSTR_NIP73_SPOTIFY_EPISODE,
  GNOSTR_NIP73_SPOTIFY_SHOW
} GnostrNip73SpotifyType;

/**
 * TMDB content subtypes
 */
typedef enum {
  GNOSTR_NIP73_TMDB_UNKNOWN = 0,
  GNOSTR_NIP73_TMDB_MOVIE,
  GNOSTR_NIP73_TMDB_TV
} GnostrNip73TmdbType;

/**
 * GnostrExternalContentId:
 *
 * Represents a single external content identifier from an "i" tag.
 * Format: ["i", "type:identifier"]
 */
typedef struct {
  GnostrNip73Type type;           /* Parsed type enum */
  char *type_name;                /* Original type string (e.g., "isbn") */
  char *identifier;               /* The identifier value (e.g., "978-0-13-468599-1") */
  char *raw_value;                /* Full raw value (e.g., "isbn:978-0-13-468599-1") */

  /* Subtype info for Spotify/TMDB */
  union {
    GnostrNip73SpotifyType spotify_type;
    GnostrNip73TmdbType tmdb_type;
  } subtype;
} GnostrExternalContentId;

/**
 * Parse an "i" tag value into an external content ID.
 *
 * @param tag_value The "i" tag value (format: "type:identifier")
 * @return Newly allocated content ID, or NULL on parse error.
 *         Caller must free with gnostr_external_content_id_free().
 */
GnostrExternalContentId *gnostr_nip73_parse_id(const char *tag_value);

/**
 * Parse all "i" tags from a JSON event and return external content IDs.
 * This is specifically for NIP-73 external content (not NIP-39 identities).
 *
 * @param event_json_str The full event JSON string
 * @return GPtrArray of GnostrExternalContentId* (caller owns array and elements).
 *         NULL on error or if no external content IDs found.
 */
GPtrArray *gnostr_nip73_parse_ids_from_event(const char *event_json_str);

/**
 * Parse external content IDs from a tags JSON array string.
 *
 * @param tags_json The JSON array string of event tags
 * @return GPtrArray of GnostrExternalContentId* (caller owns array and elements).
 *         NULL on error or if no external content IDs found.
 */
GPtrArray *gnostr_nip73_parse_ids_from_tags_json(const char *tags_json);

/**
 * Free an external content ID.
 *
 * @param content_id The content ID to free
 */
void gnostr_external_content_id_free(GnostrExternalContentId *content_id);

/**
 * Get the type enum from a type string.
 *
 * @param type_str The type string (e.g., "isbn", "doi")
 * @return The type enum value
 */
GnostrNip73Type gnostr_nip73_type_from_string(const char *type_str);

/**
 * Get a type string from the enum.
 *
 * @param type The type enum
 * @return Static string (do not free)
 */
const char *gnostr_nip73_type_to_string(GnostrNip73Type type);

/**
 * Get the icon name for a content type.
 *
 * @param type The content type enum
 * @return Static icon name string (do not free)
 */
const char *gnostr_nip73_get_type_icon(GnostrNip73Type type);

/**
 * Get a display-friendly name for a content type.
 *
 * @param type The content type enum
 * @return Static display name string (do not free)
 */
const char *gnostr_nip73_get_type_display_name(GnostrNip73Type type);

/**
 * Get the primary external URL for a content ID.
 * For most types, returns the canonical web URL.
 *
 * @param content_id The external content ID
 * @return Newly allocated URL string, or NULL if not applicable.
 *         Caller must g_free().
 */
char *gnostr_nip73_get_url(const GnostrExternalContentId *content_id);

/**
 * Get the secondary/alternative external URL for a content ID.
 * For example, ISBN can link to both OpenLibrary and Goodreads.
 *
 * @param content_id The external content ID
 * @return Newly allocated URL string, or NULL if not applicable.
 *         Caller must g_free().
 */
char *gnostr_nip73_get_alt_url(const GnostrExternalContentId *content_id);

/**
 * Get the name of the primary URL service.
 *
 * @param content_id The external content ID
 * @return Static string (do not free), or NULL if not applicable.
 */
const char *gnostr_nip73_get_url_service_name(const GnostrExternalContentId *content_id);

/**
 * Get the name of the alternative URL service.
 *
 * @param content_id The external content ID
 * @return Static string (do not free), or NULL if not applicable.
 */
const char *gnostr_nip73_get_alt_url_service_name(const GnostrExternalContentId *content_id);

/**
 * Format a content ID for display.
 * Returns a human-readable string like "ISBN: 978-0-13-468599-1" or "IMDB: tt0111161".
 *
 * @param content_id The content ID to format
 * @return Newly allocated string. Caller must g_free().
 */
char *gnostr_nip73_format_display(const GnostrExternalContentId *content_id);

/**
 * Get a tooltip description for a content ID.
 * Includes the type, identifier, and available links.
 *
 * @param content_id The content ID
 * @return Newly allocated string. Caller must g_free().
 */
char *gnostr_nip73_get_tooltip(const GnostrExternalContentId *content_id);

/**
 * Check if a type is for media content (videos, music).
 *
 * @param type The content type
 * @return TRUE if this is media content
 */
gboolean gnostr_nip73_is_media_type(GnostrNip73Type type);

/**
 * Check if a type is for reference content (books, papers).
 *
 * @param type The content type
 * @return TRUE if this is reference content
 */
gboolean gnostr_nip73_is_reference_type(GnostrNip73Type type);

/**
 * Create a chip/badge widget for displaying an external content ID.
 * The chip shows the type icon and identifier, and is clickable to open the URL.
 *
 * @param content_id The external content ID to display
 * @return A new GtkWidget containing the badge. Caller owns the reference.
 */
GtkWidget *gnostr_nip73_create_badge(const GnostrExternalContentId *content_id);

/**
 * Create a container widget showing all external content IDs.
 * Returns a horizontal box with badges for each ID.
 *
 * @param content_ids GPtrArray of GnostrExternalContentId*
 * @return A new GtkWidget containing all badges, or NULL if empty.
 *         Caller owns the reference.
 */
GtkWidget *gnostr_nip73_create_badges_box(GPtrArray *content_ids);

/**
 * Build an "i" tag JSON array from an external content ID.
 *
 * @param content_id The content ID
 * @return JSON string of the tag (e.g., '["i", "isbn:978-0-13-468599-1"]').
 *         Caller must g_free().
 */
char *gnostr_nip73_build_tag_json(const GnostrExternalContentId *content_id);

/**
 * Create an external content ID struct from type and identifier.
 *
 * @param type_str The type string (e.g., "isbn", "doi")
 * @param identifier The identifier value
 * @return Newly allocated content ID, or NULL on error.
 *         Caller must free with gnostr_external_content_id_free().
 */
GnostrExternalContentId *gnostr_nip73_create_id(const char *type_str,
                                                  const char *identifier);

G_END_DECLS

#endif /* GNOSTR_NIP73_EXTERNAL_IDS_H */
