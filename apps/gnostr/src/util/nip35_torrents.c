/*
 * nip35_torrents.c - NIP-35 Torrent Event Utilities
 *
 * Implementation of torrent event parsing, building, and magnet URI generation.
 */

#include "nip35_torrents.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---- Memory Management ---- */

GnostrTorrentFile *gnostr_torrent_file_new(const char *path, gint64 size) {
  GnostrTorrentFile *file = g_new0(GnostrTorrentFile, 1);
  file->path = g_strdup(path);
  file->size = size;
  return file;
}

void gnostr_torrent_file_free(GnostrTorrentFile *file) {
  if (!file) return;
  g_free(file->path);
  g_free(file);
}

GnostrTorrentReference *gnostr_torrent_reference_new(const char *prefix, const char *value) {
  GnostrTorrentReference *ref = g_new0(GnostrTorrentReference, 1);
  ref->prefix = g_strdup(prefix);
  ref->value = g_strdup(value);
  return ref;
}

void gnostr_torrent_reference_free(GnostrTorrentReference *ref) {
  if (!ref) return;
  g_free(ref->prefix);
  g_free(ref->value);
  g_free(ref);
}

GnostrTorrent *gnostr_torrent_new(void) {
  GnostrTorrent *torrent = g_new0(GnostrTorrent, 1);
  torrent->total_size = -1;
  return torrent;
}

void gnostr_torrent_free(GnostrTorrent *torrent) {
  if (!torrent) return;

  g_free(torrent->event_id);
  g_free(torrent->pubkey);
  g_free(torrent->title);
  g_free(torrent->infohash);
  g_free(torrent->description);

  /* Free files */
  if (torrent->files) {
    for (gsize i = 0; i < torrent->files_count; i++) {
      gnostr_torrent_file_free(torrent->files[i]);
    }
    g_free(torrent->files);
  }

  /* Free trackers */
  if (torrent->trackers) {
    g_strfreev(torrent->trackers);
  }

  /* Free references */
  if (torrent->references) {
    for (gsize i = 0; i < torrent->references_count; i++) {
      gnostr_torrent_reference_free(torrent->references[i]);
    }
    g_free(torrent->references);
  }

  /* Free categories */
  if (torrent->categories) {
    g_strfreev(torrent->categories);
  }

  g_free(torrent);
}

/* ---- Validation ---- */

gboolean gnostr_torrent_validate_infohash(const char *infohash) {
  if (!infohash) return FALSE;

  size_t len = strlen(infohash);
  if (len != 40) return FALSE;

  for (size_t i = 0; i < 40; i++) {
    if (!g_ascii_isxdigit(infohash[i])) {
      return FALSE;
    }
  }

  return TRUE;
}

gboolean gnostr_torrent_is_torrent_event(int kind) {
  return kind == NOSTR_KIND_TORRENT;
}

gboolean gnostr_torrent_is_torrent_comment(int kind) {
  return kind == NOSTR_KIND_TORRENT_COMMENT;
}

/* ---- Adding Data ---- */

void gnostr_torrent_add_file(GnostrTorrent *torrent, const char *path, gint64 size) {
  if (!torrent || !path) return;

  gsize new_count = torrent->files_count + 1;
  torrent->files = g_realloc(torrent->files, sizeof(GnostrTorrentFile *) * (new_count + 1));
  torrent->files[torrent->files_count] = gnostr_torrent_file_new(path, size);
  torrent->files[new_count] = NULL;
  torrent->files_count = new_count;

  /* Update total size if known */
  if (size >= 0) {
    if (torrent->total_size < 0) {
      torrent->total_size = size;
    } else {
      torrent->total_size += size;
    }
  }
}

void gnostr_torrent_add_tracker(GnostrTorrent *torrent, const char *tracker_url) {
  if (!torrent || !tracker_url || !*tracker_url) return;

  gsize new_count = torrent->trackers_count + 1;
  torrent->trackers = g_realloc(torrent->trackers, sizeof(gchar *) * (new_count + 1));
  torrent->trackers[torrent->trackers_count] = g_strdup(tracker_url);
  torrent->trackers[new_count] = NULL;
  torrent->trackers_count = new_count;
}

void gnostr_torrent_add_reference(GnostrTorrent *torrent,
                                   const char *prefix,
                                   const char *value) {
  if (!torrent || !prefix || !value) return;

  gsize new_count = torrent->references_count + 1;
  torrent->references = g_realloc(torrent->references,
                                   sizeof(GnostrTorrentReference *) * (new_count + 1));
  torrent->references[torrent->references_count] = gnostr_torrent_reference_new(prefix, value);
  torrent->references[new_count] = NULL;
  torrent->references_count = new_count;
}

void gnostr_torrent_add_category(GnostrTorrent *torrent, const char *category) {
  if (!torrent || !category || !*category) return;

  /* Skip leading # if present */
  if (*category == '#') category++;
  if (!*category) return;

  gsize new_count = torrent->categories_count + 1;
  torrent->categories = g_realloc(torrent->categories, sizeof(gchar *) * (new_count + 1));
  torrent->categories[torrent->categories_count] = g_strdup(category);
  torrent->categories[new_count] = NULL;
  torrent->categories_count = new_count;
}

/* ---- Parsing ---- */

GnostrTorrent *gnostr_torrent_parse_from_json(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("NIP-35: Failed to parse event JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_warning("NIP-35: Event is not a JSON object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *event = json_node_get_object(root);

  /* Verify kind */
  gint64 kind = json_object_get_int_member(event, "kind");
  if (kind != NOSTR_KIND_TORRENT) {
    g_warning("NIP-35: Event kind %lld is not a torrent event", (long long)kind);
    g_object_unref(parser);
    return NULL;
  }

  GnostrTorrent *torrent = gnostr_torrent_new();

  /* Extract event metadata */
  if (json_object_has_member(event, "id")) {
    torrent->event_id = g_strdup(json_object_get_string_member(event, "id"));
  }
  if (json_object_has_member(event, "pubkey")) {
    torrent->pubkey = g_strdup(json_object_get_string_member(event, "pubkey"));
  }
  if (json_object_has_member(event, "created_at")) {
    torrent->created_at = json_object_get_int_member(event, "created_at");
  }

  /* Extract content (description) */
  if (json_object_has_member(event, "content")) {
    torrent->description = g_strdup(json_object_get_string_member(event, "content"));
  }

  /* Parse tags */
  if (json_object_has_member(event, "tags")) {
    JsonArray *tags = json_object_get_array_member(event, "tags");
    guint n_tags = json_array_get_length(tags);

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      const char *tag_value = json_array_get_string_element(tag, 1);

      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "title") == 0) {
        g_free(torrent->title);
        torrent->title = g_strdup(tag_value);
      } else if (strcmp(tag_name, "x") == 0) {
        if (gnostr_torrent_validate_infohash(tag_value)) {
          g_free(torrent->infohash);
          torrent->infohash = g_ascii_strdown(tag_value, -1);
        }
      } else if (strcmp(tag_name, "file") == 0) {
        gint64 size = -1;
        if (tag_len >= 3) {
          const char *size_str = json_array_get_string_element(tag, 2);
          if (size_str && *size_str) {
            char *endptr;
            gint64 parsed = g_ascii_strtoll(size_str, &endptr, 10);
            if (endptr != size_str && *endptr == '\0' && parsed >= 0) {
              size = parsed;
            }
          }
        }
        gnostr_torrent_add_file(torrent, tag_value, size);
      } else if (strcmp(tag_name, "tracker") == 0) {
        gnostr_torrent_add_tracker(torrent, tag_value);
      } else if (strcmp(tag_name, "i") == 0) {
        /* Parse external reference: "prefix:value" */
        const char *colon = strchr(tag_value, ':');
        if (colon && colon != tag_value) {
          gchar *prefix = g_strndup(tag_value, colon - tag_value);
          const char *value = colon + 1;
          if (*value) {
            gnostr_torrent_add_reference(torrent, prefix, value);
          }
          g_free(prefix);
        }
      } else if (strcmp(tag_name, "t") == 0) {
        gnostr_torrent_add_category(torrent, tag_value);
      }
    }
  }

  g_object_unref(parser);
  return torrent;
}

GnostrTorrent *gnostr_torrent_parse_tags(const char *tags_json, const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  /* Build a minimal event JSON for parsing */
  gchar *event_json = g_strdup_printf(
    "{\"kind\":2003,\"tags\":%s,\"content\":%s}",
    tags_json,
    content ? content : "\"\""
  );

  GnostrTorrent *torrent = gnostr_torrent_parse_from_json(event_json);
  g_free(event_json);

  return torrent;
}

/* ---- Building Events ---- */

gboolean gnostr_torrent_build_event(const GnostrTorrent *torrent,
                                     char **out_tags_json,
                                     char **out_content) {
  if (!torrent) return FALSE;

  /* Must have at least a title and infohash */
  if (!torrent->title || !torrent->infohash) {
    g_warning("NIP-35: Torrent must have title and infohash");
    return FALSE;
  }

  if (!gnostr_torrent_validate_infohash(torrent->infohash)) {
    g_warning("NIP-35: Invalid infohash format");
    return FALSE;
  }

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  /* Title tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "title");
  json_builder_add_string_value(builder, torrent->title);
  json_builder_end_array(builder);

  /* Infohash tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "x");
  json_builder_add_string_value(builder, torrent->infohash);
  json_builder_end_array(builder);

  /* File tags */
  if (torrent->files) {
    for (gsize i = 0; i < torrent->files_count; i++) {
      GnostrTorrentFile *file = torrent->files[i];
      if (!file || !file->path) continue;

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "file");
      json_builder_add_string_value(builder, file->path);
      if (file->size >= 0) {
        gchar *size_str = g_strdup_printf("%" G_GINT64_FORMAT, file->size);
        json_builder_add_string_value(builder, size_str);
        g_free(size_str);
      }
      json_builder_end_array(builder);
    }
  }

  /* Tracker tags */
  if (torrent->trackers) {
    for (gsize i = 0; i < torrent->trackers_count; i++) {
      if (!torrent->trackers[i]) continue;

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "tracker");
      json_builder_add_string_value(builder, torrent->trackers[i]);
      json_builder_end_array(builder);
    }
  }

  /* Reference tags (i tags) */
  if (torrent->references) {
    for (gsize i = 0; i < torrent->references_count; i++) {
      GnostrTorrentReference *ref = torrent->references[i];
      if (!ref || !ref->prefix || !ref->value) continue;

      gchar *combined = g_strdup_printf("%s:%s", ref->prefix, ref->value);
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "i");
      json_builder_add_string_value(builder, combined);
      json_builder_end_array(builder);
      g_free(combined);
    }
  }

  /* Category tags (t tags) */
  if (torrent->categories) {
    for (gsize i = 0; i < torrent->categories_count; i++) {
      if (!torrent->categories[i]) continue;

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "t");
      json_builder_add_string_value(builder, torrent->categories[i]);
      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);

  /* Generate JSON string */
  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);

  if (out_tags_json) {
    *out_tags_json = json_generator_to_data(generator, NULL);
  }

  if (out_content) {
    *out_content = g_strdup(torrent->description ? torrent->description : "");
  }

  json_node_unref(root);
  g_object_unref(generator);
  g_object_unref(builder);

  return TRUE;
}

/* ---- Magnet URI ---- */

gchar *gnostr_torrent_generate_magnet(const GnostrTorrent *torrent) {
  if (!torrent || !torrent->infohash) return NULL;

  if (!gnostr_torrent_validate_infohash(torrent->infohash)) return NULL;

  GString *magnet = g_string_new("magnet:?");

  /* Infohash (xt = exact topic) */
  g_string_append_printf(magnet, "xt=urn:btih:%s", torrent->infohash);

  /* Display name (dn) */
  if (torrent->title && *torrent->title) {
    gchar *escaped = g_uri_escape_string(torrent->title, NULL, TRUE);
    g_string_append_printf(magnet, "&dn=%s", escaped);
    g_free(escaped);
  }

  /* Trackers (tr) */
  if (torrent->trackers) {
    for (gsize i = 0; i < torrent->trackers_count; i++) {
      if (!torrent->trackers[i]) continue;
      gchar *escaped = g_uri_escape_string(torrent->trackers[i], NULL, TRUE);
      g_string_append_printf(magnet, "&tr=%s", escaped);
      g_free(escaped);
    }
  }

  /* Exact length (xl) - total size if known */
  if (torrent->total_size > 0) {
    g_string_append_printf(magnet, "&xl=%" G_GINT64_FORMAT, torrent->total_size);
  }

  return g_string_free(magnet, FALSE);
}

GnostrTorrent *gnostr_torrent_parse_magnet(const char *magnet_uri) {
  if (!magnet_uri) return NULL;

  /* Must start with "magnet:?" */
  if (!g_str_has_prefix(magnet_uri, "magnet:?")) {
    return NULL;
  }

  const char *params = magnet_uri + 8;  /* Skip "magnet:?" */
  GnostrTorrent *torrent = gnostr_torrent_new();

  /* Split by & and parse each parameter */
  gchar **parts = g_strsplit(params, "&", -1);

  for (gchar **p = parts; *p; p++) {
    gchar *eq = strchr(*p, '=');
    if (!eq) continue;

    gchar *key = g_strndup(*p, eq - *p);
    gchar *value = g_uri_unescape_string(eq + 1, NULL);

    if (!value) {
      g_free(key);
      continue;
    }

    if (strcmp(key, "xt") == 0) {
      /* Extract infohash from urn:btih:HASH */
      if (g_str_has_prefix(value, "urn:btih:")) {
        const char *hash = value + 9;
        if (gnostr_torrent_validate_infohash(hash)) {
          g_free(torrent->infohash);
          torrent->infohash = g_ascii_strdown(hash, -1);
        }
      }
    } else if (strcmp(key, "dn") == 0) {
      g_free(torrent->title);
      torrent->title = g_strdup(value);
    } else if (strcmp(key, "tr") == 0) {
      gnostr_torrent_add_tracker(torrent, value);
    } else if (strcmp(key, "xl") == 0) {
      char *endptr;
      gint64 size = g_ascii_strtoll(value, &endptr, 10);
      if (endptr != value && size > 0) {
        torrent->total_size = size;
      }
    }

    g_free(key);
    g_free(value);
  }

  g_strfreev(parts);

  /* Must have at least an infohash */
  if (!torrent->infohash) {
    gnostr_torrent_free(torrent);
    return NULL;
  }

  return torrent;
}

/* ---- Reference URL Generation ---- */

gchar *gnostr_torrent_get_reference_url(const GnostrTorrentReference *ref) {
  if (!ref || !ref->prefix || !ref->value) return NULL;

  const char *prefix = ref->prefix;
  const char *value = ref->value;

  if (strcmp(prefix, "imdb") == 0) {
    /* IMDB: tt12345678 -> imdb.com/title/tt12345678 */
    return g_strdup_printf("https://www.imdb.com/title/%s", value);
  } else if (strcmp(prefix, "tmdb") == 0) {
    /* TMDB: movie:693134 -> themoviedb.org/movie/693134 */
    /* Can be "movie:ID" or "tv:ID" */
    const char *colon = strchr(value, ':');
    if (colon) {
      gchar *type = g_strndup(value, colon - value);
      const char *id = colon + 1;
      gchar *url = g_strdup_printf("https://www.themoviedb.org/%s/%s", type, id);
      g_free(type);
      return url;
    }
    return g_strdup_printf("https://www.themoviedb.org/movie/%s", value);
  } else if (strcmp(prefix, "ttvdb") == 0) {
    /* TVDB: movie:290272 -> thetvdb.com/movies/ID or series/ID */
    const char *colon = strchr(value, ':');
    if (colon) {
      gchar *type = g_strndup(value, colon - value);
      const char *id = colon + 1;
      const char *path = strcmp(type, "movie") == 0 ? "movies" : "series";
      gchar *url = g_strdup_printf("https://thetvdb.com/%s/%s", path, id);
      g_free(type);
      return url;
    }
    return g_strdup_printf("https://thetvdb.com/search?query=%s", value);
  } else if (strcmp(prefix, "mal") == 0) {
    /* MyAnimeList: anime:9253 or manga:17517 */
    const char *colon = strchr(value, ':');
    if (colon) {
      gchar *type = g_strndup(value, colon - value);
      const char *id = colon + 1;
      gchar *url = g_strdup_printf("https://myanimelist.net/%s/%s", type, id);
      g_free(type);
      return url;
    }
    return g_strdup_printf("https://myanimelist.net/anime/%s", value);
  } else if (strcmp(prefix, "anilist") == 0) {
    /* AniList: anime/ID or manga/ID */
    const char *colon = strchr(value, ':');
    if (colon) {
      gchar *type = g_strndup(value, colon - value);
      const char *id = colon + 1;
      gchar *url = g_strdup_printf("https://anilist.co/%s/%s", type, id);
      g_free(type);
      return url;
    }
    return g_strdup_printf("https://anilist.co/anime/%s", value);
  }

  return NULL;
}

/* ---- Size Formatting ---- */

gchar *gnostr_torrent_format_size(gint64 size_bytes) {
  if (size_bytes < 0) {
    return g_strdup("Unknown");
  }

  const gint64 KB = 1024;
  const gint64 MB = KB * 1024;
  const gint64 GB = MB * 1024;
  const gint64 TB = GB * 1024;

  if (size_bytes >= TB) {
    return g_strdup_printf("%.2f TB", (double)size_bytes / TB);
  } else if (size_bytes >= GB) {
    return g_strdup_printf("%.2f GB", (double)size_bytes / GB);
  } else if (size_bytes >= MB) {
    return g_strdup_printf("%.2f MB", (double)size_bytes / MB);
  } else if (size_bytes >= KB) {
    return g_strdup_printf("%.2f KB", (double)size_bytes / KB);
  } else {
    return g_strdup_printf("%" G_GINT64_FORMAT " B", size_bytes);
  }
}
