/**
 * NIP-73 External Content IDs for gnostr
 *
 * Implements parsing and URL generation for external content identifiers.
 */

#include "nip73_external_ids.h"
#include <jansson.h>
#include <string.h>

/* Type string mappings */
static const struct {
  const char *name;
  GnostrNip73Type type;
  const char *display_name;
  const char *icon_name;
} type_info[] = {
  { "isbn",         GNOSTR_NIP73_TYPE_ISBN,         "ISBN",        "accessories-dictionary-symbolic" },
  { "doi",          GNOSTR_NIP73_TYPE_DOI,          "DOI",         "document-page-setup-symbolic" },
  { "imdb",         GNOSTR_NIP73_TYPE_IMDB,         "IMDB",        "video-x-generic-symbolic" },
  { "tmdb",         GNOSTR_NIP73_TYPE_TMDB,         "TMDB",        "video-x-generic-symbolic" },
  { "spotify",      GNOSTR_NIP73_TYPE_SPOTIFY,      "Spotify",     "audio-x-generic-symbolic" },
  { "youtube",      GNOSTR_NIP73_TYPE_YOUTUBE,      "YouTube",     "media-playback-start-symbolic" },
  { "podcast",      GNOSTR_NIP73_TYPE_PODCAST_GUID, "Podcast",     "audio-x-generic-symbolic" },
  { NULL,           GNOSTR_NIP73_TYPE_UNKNOWN,      "Unknown",     "emblem-documents-symbolic" }
};

/* URL templates for each type */
/* Primary URLs */
static const char *ISBN_OPENLIBRARY_URL = "https://openlibrary.org/isbn/%s";
static const char *ISBN_GOODREADS_URL = "https://www.goodreads.com/search?q=%s";
static const char *DOI_URL = "https://doi.org/%s";
static const char *IMDB_URL = "https://www.imdb.com/title/%s/";
static const char *TMDB_MOVIE_URL = "https://www.themoviedb.org/movie/%s";
static const char *TMDB_TV_URL = "https://www.themoviedb.org/tv/%s";
static const char *SPOTIFY_URL = "https://open.spotify.com/%s/%s";
static const char *YOUTUBE_URL = "https://www.youtube.com/watch?v=%s";
static const char *PODCAST_INDEX_URL = "https://podcastindex.org/podcast/%s";

/* Service names */
static const char *SERVICE_OPENLIBRARY = "Open Library";
static const char *SERVICE_GOODREADS = "Goodreads";
static const char *SERVICE_DOI = "DOI.org";
static const char *SERVICE_IMDB = "IMDB";
static const char *SERVICE_TMDB = "TMDB";
static const char *SERVICE_SPOTIFY = "Spotify";
static const char *SERVICE_YOUTUBE = "YouTube";
static const char *SERVICE_PODCAST_INDEX = "Podcast Index";


GnostrNip73Type gnostr_nip73_type_from_string(const char *type_str) {
  if (!type_str || !*type_str) {
    return GNOSTR_NIP73_TYPE_UNKNOWN;
  }

  /* Handle podcast:guid specially - check for "podcast" prefix */
  if (g_str_has_prefix(type_str, "podcast")) {
    return GNOSTR_NIP73_TYPE_PODCAST_GUID;
  }

  for (int i = 0; type_info[i].name != NULL; i++) {
    if (g_ascii_strcasecmp(type_str, type_info[i].name) == 0) {
      return type_info[i].type;
    }
  }

  return GNOSTR_NIP73_TYPE_UNKNOWN;
}

const char *gnostr_nip73_type_to_string(GnostrNip73Type type) {
  for (int i = 0; ; i++) {
    if (type_info[i].type == type) {
      return type_info[i].name ? type_info[i].name : "unknown";
    }
    if (type_info[i].name == NULL) break;
  }
  return "unknown";
}

const char *gnostr_nip73_get_type_icon(GnostrNip73Type type) {
  for (int i = 0; ; i++) {
    if (type_info[i].type == type) {
      return type_info[i].icon_name;
    }
    if (type_info[i].name == NULL) break;
  }
  return "emblem-documents-symbolic";
}

const char *gnostr_nip73_get_type_display_name(GnostrNip73Type type) {
  for (int i = 0; ; i++) {
    if (type_info[i].type == type) {
      return type_info[i].display_name;
    }
    if (type_info[i].name == NULL) break;
  }
  return "Unknown";
}

/**
 * Parse Spotify identifier to extract subtype and ID.
 * Spotify format: spotify:track:xxx or spotify:album:xxx, etc.
 * Also handles open.spotify.com URLs embedded in the identifier.
 */
static GnostrNip73SpotifyType parse_spotify_subtype(const char *identifier) {
  if (!identifier) return GNOSTR_NIP73_SPOTIFY_UNKNOWN;

  if (g_str_has_prefix(identifier, "track:") ||
      strstr(identifier, "/track/")) {
    return GNOSTR_NIP73_SPOTIFY_TRACK;
  }
  if (g_str_has_prefix(identifier, "album:") ||
      strstr(identifier, "/album/")) {
    return GNOSTR_NIP73_SPOTIFY_ALBUM;
  }
  if (g_str_has_prefix(identifier, "artist:") ||
      strstr(identifier, "/artist/")) {
    return GNOSTR_NIP73_SPOTIFY_ARTIST;
  }
  if (g_str_has_prefix(identifier, "playlist:") ||
      strstr(identifier, "/playlist/")) {
    return GNOSTR_NIP73_SPOTIFY_PLAYLIST;
  }
  if (g_str_has_prefix(identifier, "episode:") ||
      strstr(identifier, "/episode/")) {
    return GNOSTR_NIP73_SPOTIFY_EPISODE;
  }
  if (g_str_has_prefix(identifier, "show:") ||
      strstr(identifier, "/show/")) {
    return GNOSTR_NIP73_SPOTIFY_SHOW;
  }

  return GNOSTR_NIP73_SPOTIFY_UNKNOWN;
}

/**
 * Parse TMDB identifier to extract subtype and ID.
 * TMDB format: tmdb:movie/278 or tmdb:tv/1396
 */
static GnostrNip73TmdbType parse_tmdb_subtype(const char *identifier) {
  if (!identifier) return GNOSTR_NIP73_TMDB_UNKNOWN;

  if (g_str_has_prefix(identifier, "movie/") ||
      g_str_has_prefix(identifier, "movie:")) {
    return GNOSTR_NIP73_TMDB_MOVIE;
  }
  if (g_str_has_prefix(identifier, "tv/") ||
      g_str_has_prefix(identifier, "tv:")) {
    return GNOSTR_NIP73_TMDB_TV;
  }

  /* Default to movie if no prefix */
  return GNOSTR_NIP73_TMDB_MOVIE;
}

GnostrExternalContentId *gnostr_nip73_parse_id(const char *tag_value) {
  if (!tag_value || !*tag_value) {
    return NULL;
  }

  /* Format: "type:identifier" or "podcast:guid:xxxxx" */
  const char *first_colon = strchr(tag_value, ':');
  if (!first_colon || first_colon == tag_value) {
    g_debug("nip73: invalid id format (no colon): %s", tag_value);
    return NULL;
  }

  char *type_str = g_strndup(tag_value, first_colon - tag_value);
  const char *identifier = first_colon + 1;

  /* Handle podcast:guid: specially */
  if (g_ascii_strcasecmp(type_str, "podcast") == 0) {
    /* Check for guid: prefix in identifier */
    if (g_str_has_prefix(identifier, "guid:")) {
      identifier = identifier + 5; /* Skip "guid:" */
    }
  }

  if (!identifier || !*identifier) {
    g_free(type_str);
    g_debug("nip73: invalid id format (empty identifier): %s", tag_value);
    return NULL;
  }

  GnostrNip73Type type = gnostr_nip73_type_from_string(type_str);

  /* Skip unknown types - this might be a NIP-39 identity tag */
  if (type == GNOSTR_NIP73_TYPE_UNKNOWN) {
    g_free(type_str);
    return NULL;
  }

  GnostrExternalContentId *result = g_new0(GnostrExternalContentId, 1);
  result->type_name = type_str;
  result->type = type;
  result->identifier = g_strdup(identifier);
  result->raw_value = g_strdup(tag_value);

  /* Parse subtypes */
  if (type == GNOSTR_NIP73_TYPE_SPOTIFY) {
    result->subtype.spotify_type = parse_spotify_subtype(identifier);
  } else if (type == GNOSTR_NIP73_TYPE_TMDB) {
    result->subtype.tmdb_type = parse_tmdb_subtype(identifier);
  }

  g_debug("nip73: parsed id type=%s identifier=%s", type_str, identifier);

  return result;
}

GnostrExternalContentId *gnostr_nip73_create_id(const char *type_str,
                                                  const char *identifier) {
  if (!type_str || !identifier) {
    return NULL;
  }

  char *raw_value = g_strdup_printf("%s:%s", type_str, identifier);
  GnostrExternalContentId *result = gnostr_nip73_parse_id(raw_value);
  g_free(raw_value);

  return result;
}

void gnostr_external_content_id_free(GnostrExternalContentId *content_id) {
  if (!content_id) return;
  g_free(content_id->type_name);
  g_free(content_id->identifier);
  g_free(content_id->raw_value);
  g_free(content_id);
}

/**
 * Check if an "i" tag is a NIP-73 external content ID vs NIP-39 identity.
 * NIP-39 identities use platforms like "github", "twitter", etc.
 * NIP-73 uses "isbn", "doi", "imdb", etc.
 */
static gboolean is_nip73_tag(const char *tag_value) {
  if (!tag_value) return FALSE;

  const char *colon = strchr(tag_value, ':');
  if (!colon || colon == tag_value) return FALSE;

  char *type_str = g_strndup(tag_value, colon - tag_value);
  GnostrNip73Type type = gnostr_nip73_type_from_string(type_str);
  g_free(type_str);

  return type != GNOSTR_NIP73_TYPE_UNKNOWN;
}

GPtrArray *gnostr_nip73_parse_ids_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return NULL;
  }

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags || !json_is_array(tags)) {
    if (tags) json_decref(tags);
    return NULL;
  }

  GPtrArray *content_ids = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gnostr_external_content_id_free);

  size_t i;
  json_t *tag;
  json_array_foreach(tags, i, tag) {
    if (!json_is_array(tag) || json_array_size(tag) < 2) {
      continue;
    }

    /* Check if this is an "i" tag */
    json_t *tag_key = json_array_get(tag, 0);
    if (!json_is_string(tag_key) || strcmp(json_string_value(tag_key), "i") != 0) {
      continue;
    }

    /* Get the identifier value */
    json_t *tag_value = json_array_get(tag, 1);
    if (!json_is_string(tag_value)) {
      continue;
    }

    const char *value = json_string_value(tag_value);

    /* Only parse if this is a NIP-73 tag (not NIP-39) */
    if (!is_nip73_tag(value)) {
      continue;
    }

    GnostrExternalContentId *content_id = gnostr_nip73_parse_id(value);
    if (content_id) {
      g_ptr_array_add(content_ids, content_id);
    }
  }

  json_decref(tags);

  if (content_ids->len == 0) {
    g_ptr_array_unref(content_ids);
    return NULL;
  }

  return content_ids;
}

GPtrArray *gnostr_nip73_parse_ids_from_event(const char *event_json_str) {
  if (!event_json_str || !*event_json_str) {
    return NULL;
  }

  json_error_t error;
  json_t *root = json_loads(event_json_str, 0, &error);
  if (!root) {
    g_warning("nip73: failed to parse event JSON: %s", error.text);
    return NULL;
  }

  /* Get tags array */
  json_t *tags = json_object_get(root, "tags");
  if (!tags || !json_is_array(tags)) {
    json_decref(root);
    return NULL;
  }

  char *tags_json = json_dumps(tags, JSON_COMPACT);
  json_decref(root);

  if (!tags_json) {
    return NULL;
  }

  GPtrArray *result = gnostr_nip73_parse_ids_from_tags_json(tags_json);
  g_free(tags_json);

  return result;
}

/**
 * Extract the Spotify ID from an identifier that may have subtype prefix.
 * E.g., "track:abc123" -> "abc123"
 */
static const char *get_spotify_id(const char *identifier) {
  const char *colon = strchr(identifier, ':');
  return colon ? colon + 1 : identifier;
}

/**
 * Get the Spotify subtype string for URL building.
 */
static const char *get_spotify_subtype_string(GnostrNip73SpotifyType subtype) {
  switch (subtype) {
    case GNOSTR_NIP73_SPOTIFY_TRACK:    return "track";
    case GNOSTR_NIP73_SPOTIFY_ALBUM:    return "album";
    case GNOSTR_NIP73_SPOTIFY_ARTIST:   return "artist";
    case GNOSTR_NIP73_SPOTIFY_PLAYLIST: return "playlist";
    case GNOSTR_NIP73_SPOTIFY_EPISODE:  return "episode";
    case GNOSTR_NIP73_SPOTIFY_SHOW:     return "show";
    default: return "track";
  }
}

/**
 * Extract the TMDB ID from an identifier that may have subtype prefix.
 * E.g., "movie/278" -> "278"
 */
static const char *get_tmdb_id(const char *identifier) {
  const char *slash = strchr(identifier, '/');
  if (slash) return slash + 1;
  const char *colon = strchr(identifier, ':');
  return colon ? colon + 1 : identifier;
}

char *gnostr_nip73_get_url(const GnostrExternalContentId *content_id) {
  if (!content_id || !content_id->identifier) {
    return NULL;
  }

  switch (content_id->type) {
    case GNOSTR_NIP73_TYPE_ISBN:
      return g_strdup_printf(ISBN_OPENLIBRARY_URL, content_id->identifier);

    case GNOSTR_NIP73_TYPE_DOI:
      return g_strdup_printf(DOI_URL, content_id->identifier);

    case GNOSTR_NIP73_TYPE_IMDB:
      return g_strdup_printf(IMDB_URL, content_id->identifier);

    case GNOSTR_NIP73_TYPE_TMDB: {
      const char *id = get_tmdb_id(content_id->identifier);
      if (content_id->subtype.tmdb_type == GNOSTR_NIP73_TMDB_TV) {
        return g_strdup_printf(TMDB_TV_URL, id);
      }
      return g_strdup_printf(TMDB_MOVIE_URL, id);
    }

    case GNOSTR_NIP73_TYPE_SPOTIFY: {
      const char *subtype = get_spotify_subtype_string(content_id->subtype.spotify_type);
      const char *id = get_spotify_id(content_id->identifier);
      return g_strdup_printf(SPOTIFY_URL, subtype, id);
    }

    case GNOSTR_NIP73_TYPE_YOUTUBE:
      return g_strdup_printf(YOUTUBE_URL, content_id->identifier);

    case GNOSTR_NIP73_TYPE_PODCAST_GUID:
      return g_strdup_printf(PODCAST_INDEX_URL, content_id->identifier);

    default:
      return NULL;
  }
}

char *gnostr_nip73_get_alt_url(const GnostrExternalContentId *content_id) {
  if (!content_id || !content_id->identifier) {
    return NULL;
  }

  /* Only ISBN has an alternative URL for now */
  if (content_id->type == GNOSTR_NIP73_TYPE_ISBN) {
    return g_strdup_printf(ISBN_GOODREADS_URL, content_id->identifier);
  }

  return NULL;
}

const char *gnostr_nip73_get_url_service_name(const GnostrExternalContentId *content_id) {
  if (!content_id) return NULL;

  switch (content_id->type) {
    case GNOSTR_NIP73_TYPE_ISBN:         return SERVICE_OPENLIBRARY;
    case GNOSTR_NIP73_TYPE_DOI:          return SERVICE_DOI;
    case GNOSTR_NIP73_TYPE_IMDB:         return SERVICE_IMDB;
    case GNOSTR_NIP73_TYPE_TMDB:         return SERVICE_TMDB;
    case GNOSTR_NIP73_TYPE_SPOTIFY:      return SERVICE_SPOTIFY;
    case GNOSTR_NIP73_TYPE_YOUTUBE:      return SERVICE_YOUTUBE;
    case GNOSTR_NIP73_TYPE_PODCAST_GUID: return SERVICE_PODCAST_INDEX;
    default: return NULL;
  }
}

const char *gnostr_nip73_get_alt_url_service_name(const GnostrExternalContentId *content_id) {
  if (!content_id) return NULL;

  /* Only ISBN has an alternative service */
  if (content_id->type == GNOSTR_NIP73_TYPE_ISBN) {
    return SERVICE_GOODREADS;
  }

  return NULL;
}

char *gnostr_nip73_format_display(const GnostrExternalContentId *content_id) {
  if (!content_id || !content_id->identifier) {
    return NULL;
  }

  const char *type_name = gnostr_nip73_get_type_display_name(content_id->type);

  /* For Spotify, include the subtype */
  if (content_id->type == GNOSTR_NIP73_TYPE_SPOTIFY) {
    const char *subtype = get_spotify_subtype_string(content_id->subtype.spotify_type);
    const char *id = get_spotify_id(content_id->identifier);
    return g_strdup_printf("%s %s: %s", type_name, subtype, id);
  }

  /* For TMDB, include movie/tv */
  if (content_id->type == GNOSTR_NIP73_TYPE_TMDB) {
    const char *subtype = content_id->subtype.tmdb_type == GNOSTR_NIP73_TMDB_TV ? "TV" : "Movie";
    const char *id = get_tmdb_id(content_id->identifier);
    return g_strdup_printf("%s %s: %s", type_name, subtype, id);
  }

  return g_strdup_printf("%s: %s", type_name, content_id->identifier);
}

char *gnostr_nip73_get_tooltip(const GnostrExternalContentId *content_id) {
  if (!content_id) return NULL;

  GString *tooltip = g_string_new(NULL);

  /* Type and identifier */
  char *display = gnostr_nip73_format_display(content_id);
  g_string_append(tooltip, display);
  g_free(display);

  /* Primary URL */
  const char *service = gnostr_nip73_get_url_service_name(content_id);
  if (service) {
    g_string_append_printf(tooltip, "\nOpen in %s", service);
  }

  /* Alternative URL */
  const char *alt_service = gnostr_nip73_get_alt_url_service_name(content_id);
  if (alt_service) {
    g_string_append_printf(tooltip, " or %s", alt_service);
  }

  return g_string_free(tooltip, FALSE);
}

gboolean gnostr_nip73_is_media_type(GnostrNip73Type type) {
  switch (type) {
    case GNOSTR_NIP73_TYPE_IMDB:
    case GNOSTR_NIP73_TYPE_TMDB:
    case GNOSTR_NIP73_TYPE_SPOTIFY:
    case GNOSTR_NIP73_TYPE_YOUTUBE:
    case GNOSTR_NIP73_TYPE_PODCAST_GUID:
      return TRUE;
    default:
      return FALSE;
  }
}

gboolean gnostr_nip73_is_reference_type(GnostrNip73Type type) {
  switch (type) {
    case GNOSTR_NIP73_TYPE_ISBN:
    case GNOSTR_NIP73_TYPE_DOI:
      return TRUE;
    default:
      return FALSE;
  }
}

/**
 * Callback for opening the primary URL when badge is clicked.
 */
static void on_badge_clicked(GtkGestureClick *gesture G_GNUC_UNUSED,
                             int n_press G_GNUC_UNUSED,
                             double x G_GNUC_UNUSED,
                             double y G_GNUC_UNUSED,
                             gpointer user_data) {
  const char *url = (const char *)user_data;
  if (url && *url) {
    GError *error = NULL;
    gtk_show_uri(NULL, url, GDK_CURRENT_TIME);
    if (error) {
      g_warning("nip73: failed to open URL %s: %s", url, error->message);
      g_error_free(error);
    }
  }
}

/**
 * Callback to free the URL data when badge is destroyed.
 */
static void on_badge_destroy(gpointer data, GClosure *closure G_GNUC_UNUSED) {
  g_free(data);
}

GtkWidget *gnostr_nip73_create_badge(const GnostrExternalContentId *content_id) {
  if (!content_id) return NULL;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, "nip73-badge");
  gtk_widget_add_css_class(box, "pill");

  /* Add appropriate CSS class based on type */
  switch (content_id->type) {
    case GNOSTR_NIP73_TYPE_ISBN:
    case GNOSTR_NIP73_TYPE_DOI:
      gtk_widget_add_css_class(box, "reference-badge");
      break;
    case GNOSTR_NIP73_TYPE_IMDB:
    case GNOSTR_NIP73_TYPE_TMDB:
    case GNOSTR_NIP73_TYPE_YOUTUBE:
      gtk_widget_add_css_class(box, "video-badge");
      break;
    case GNOSTR_NIP73_TYPE_SPOTIFY:
    case GNOSTR_NIP73_TYPE_PODCAST_GUID:
      gtk_widget_add_css_class(box, "audio-badge");
      break;
    default:
      break;
  }

  /* Icon */
  const char *icon_name = gnostr_nip73_get_type_icon(content_id->type);
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(box), icon);

  /* Type label */
  const char *type_name = gnostr_nip73_get_type_display_name(content_id->type);
  GtkWidget *type_label = gtk_label_new(type_name);
  gtk_widget_add_css_class(type_label, "caption");
  gtk_widget_add_css_class(type_label, "dim-label");
  gtk_box_append(GTK_BOX(box), type_label);

  /* Identifier (truncated if too long) */
  GtkWidget *id_label = gtk_label_new(content_id->identifier);
  gtk_widget_add_css_class(id_label, "caption");
  gtk_label_set_max_width_chars(GTK_LABEL(id_label), 20);
  gtk_label_set_ellipsize(GTK_LABEL(id_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(box), id_label);

  /* Set tooltip */
  char *tooltip = gnostr_nip73_get_tooltip(content_id);
  if (tooltip) {
    gtk_widget_set_tooltip_text(box, tooltip);
    g_free(tooltip);
  }

  /* Make clickable */
  char *url = gnostr_nip73_get_url(content_id);
  if (url) {
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect_data(click, "pressed",
                          G_CALLBACK(on_badge_clicked),
                          url,
                          on_badge_destroy,
                          0);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(box, "pointer");
  }

  return box;
}

GtkWidget *gnostr_nip73_create_badges_box(GPtrArray *content_ids) {
  if (!content_ids || content_ids->len == 0) {
    return NULL;
  }

  GtkWidget *flow_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 5);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_box), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_box), 4);
  gtk_widget_add_css_class(flow_box, "nip73-badges-container");

  for (guint i = 0; i < content_ids->len; i++) {
    GnostrExternalContentId *content_id = g_ptr_array_index(content_ids, i);
    GtkWidget *badge = gnostr_nip73_create_badge(content_id);
    if (badge) {
      gtk_flow_box_append(GTK_FLOW_BOX(flow_box), badge);
    }
  }

  return flow_box;
}

char *gnostr_nip73_build_tag_json(const GnostrExternalContentId *content_id) {
  if (!content_id || !content_id->raw_value) {
    return NULL;
  }

  json_t *tag = json_array();
  json_array_append_new(tag, json_string("i"));
  json_array_append_new(tag, json_string(content_id->raw_value));

  char *result = json_dumps(tag, JSON_COMPACT);
  json_decref(tag);

  return result;
}
