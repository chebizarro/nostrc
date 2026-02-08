#include "youtube_url.h"
#include <string.h>
#include <ctype.h>

/* Valid YouTube video IDs are 11 characters: [A-Za-z0-9_-] */
static gboolean is_valid_video_id_char(char c) {
  return g_ascii_isalnum(c) || c == '_' || c == '-';
}

/* Extract a video ID starting at `start`, stopping at non-ID chars or end.
 * Returns NULL if fewer than 11 valid chars found. */
static char *extract_id_at(const char *start) {
  if (!start) return NULL;

  int len = 0;
  while (start[len] && is_valid_video_id_char(start[len]))
    len++;

  /* YouTube video IDs are exactly 11 characters */
  if (len < 11) return NULL;
  return g_strndup(start, 11);
}

/* Check if host matches a YouTube domain.
 * `host` should point past "://" and before the next '/'. */
static gboolean is_youtube_host(const char *url, const char **path_out) {
  if (!url) return FALSE;

  /* Skip scheme */
  const char *p = strstr(url, "://");
  if (!p) return FALSE;
  p += 3;

  /* Skip www. prefix */
  if (g_str_has_prefix(p, "www."))
    p += 4;

  /* Check known YouTube hosts */
  static const char *hosts[] = {
    "youtube.com/",
    "youtu.be/",
    "m.youtube.com/",
    "music.youtube.com/",
    NULL
  };

  for (int i = 0; hosts[i]; i++) {
    if (g_str_has_prefix(p, hosts[i])) {
      *path_out = p + strlen(hosts[i]);
      return TRUE;
    }
  }

  return FALSE;
}

/* Extract query parameter value from a URL path/query string */
static char *get_query_param(const char *url, const char *param) {
  if (!url || !param) return NULL;

  const char *query = strchr(url, '?');
  if (!query) return NULL;
  query++; /* skip '?' */

  size_t param_len = strlen(param);
  const char *p = query;

  while (*p) {
    if (g_str_has_prefix(p, param) && p[param_len] == '=') {
      return extract_id_at(p + param_len + 1);
    }
    /* Skip to next parameter */
    const char *amp = strchr(p, '&');
    if (!amp) break;
    p = amp + 1;
  }

  return NULL;
}

gboolean gnostr_youtube_url_is_youtube(const char *url) {
  if (!url || !*url) return FALSE;

  const char *path;
  return is_youtube_host(url, &path);
}

char *gnostr_youtube_url_extract_video_id(const char *url) {
  if (!url || !*url) return NULL;

  const char *path;
  if (!is_youtube_host(url, &path))
    return NULL;

  /* youtu.be/VIDEO_ID */
  if (strstr(url, "youtu.be/")) {
    /* path points right after "youtu.be/" */
    const char *id_start = path;
    /* Skip any leading slash (shouldn't happen but be safe) */
    while (*id_start == '/') id_start++;
    return extract_id_at(id_start);
  }

  /* /watch?v=VIDEO_ID */
  if (g_str_has_prefix(path, "watch")) {
    return get_query_param(url, "v");
  }

  /* /shorts/VIDEO_ID */
  if (g_str_has_prefix(path, "shorts/")) {
    return extract_id_at(path + 7);
  }

  /* /embed/VIDEO_ID */
  if (g_str_has_prefix(path, "embed/")) {
    return extract_id_at(path + 6);
  }

  /* /live/VIDEO_ID */
  if (g_str_has_prefix(path, "live/")) {
    return extract_id_at(path + 5);
  }

  /* /v/VIDEO_ID (legacy) */
  if (g_str_has_prefix(path, "v/")) {
    return extract_id_at(path + 2);
  }

  return NULL;
}

char *gnostr_youtube_url_build_embed(const char *video_id) {
  if (!video_id || !*video_id) return NULL;
  return g_strdup_printf("https://www.youtube.com/embed/%s?autoplay=1", video_id);
}
