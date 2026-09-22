/* profile_metadata.c — parse + cache the sanitised kind-0 metadata.
 *
 * kind-0 content is a JSON object; NIP-01 spells `name`, `about`,
 * `picture`, `nip05`, `display_name` etc. We accept both `display_name`
 * and `displayName` (Damus writes the camelCase form). Every extracted
 * string flows through nh_profile_sanitize_text; the picture URL flows
 * through nh_profile_validate_picture_url. Rejected fields become empty
 * strings — they don't fail the parse. */
#define _GNU_SOURCE
#include "nostr_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Copy a JSON string @field from @obj through the sanitiser into
 * @dst[cap]. Silently zeroes @dst if the field is missing / not a string
 * / rejected. */
static void take_str(json_t *obj, const char *field, char *dst, size_t cap) {
  dst[0] = '\0';
  json_t *v = json_object_get(obj, field);
  if (!v || !json_is_string(v)) return;
  const char *raw = json_string_value(v);
  if (!raw) return;
  (void)nh_profile_sanitize_text(raw, dst, cap);
}

nh_profile_rc nh_profile_parse_content_json(const char *content,
                                            nh_profile_metadata *out) {
  if (!out) return NH_PROFILE_ERR_ARG;
  memset(out->name, 0, sizeof out->name);
  memset(out->display_name, 0, sizeof out->display_name);
  memset(out->nip05, 0, sizeof out->nip05);
  memset(out->picture_url, 0, sizeof out->picture_url);
  if (!content) return NH_PROFILE_ERR_PARSE;

  json_error_t jerr;
  json_t *root = json_loads(content, 0, &jerr);
  if (!root) return NH_PROFILE_ERR_PARSE;
  if (!json_is_object(root)) { json_decref(root); return NH_PROFILE_ERR_PARSE; }

  take_str(root, "name",         out->name,         sizeof out->name);
  /* Prefer explicit display_name; fall back to Damus-style displayName. */
  take_str(root, "display_name", out->display_name, sizeof out->display_name);
  if (out->display_name[0] == '\0')
    take_str(root, "displayName", out->display_name, sizeof out->display_name);
  take_str(root, "nip05",        out->nip05,        sizeof out->nip05);

  /* Picture URL: sanitise into a scratch buffer first, THEN pass the
   * result through the URL validator. If either step rejects, drop it. */
  {
    char scratch[NH_PROFILE_PICTURE_URL_MAX + 1] = {0};
    json_t *v = json_object_get(root, "picture");
    if (v && json_is_string(v)) {
      const char *raw = json_string_value(v);
      if (raw && nh_profile_sanitize_text(raw, scratch, sizeof scratch) == 0 &&
          nh_profile_validate_picture_url(scratch) == 0) {
        size_t n = strlen(scratch);
        if (n < sizeof out->picture_url) memcpy(out->picture_url, scratch, n + 1);
      }
    }
  }

  json_decref(root);
  return NH_PROFILE_OK;
}

nh_profile_rc nh_profile_parse_event_json(const char *event_json,
                                          nh_profile_metadata *out) {
  if (!event_json || !out) return NH_PROFILE_ERR_ARG;
  memset(out, 0, sizeof *out);

  json_error_t jerr;
  json_t *root = json_loads(event_json, 0, &jerr);
  if (!root) return NH_PROFILE_ERR_PARSE;
  nh_profile_rc rc = NH_PROFILE_ERR_PARSE;
  if (!json_is_object(root)) goto done;

  json_t *kind = json_object_get(root, "kind");
  if (!kind || !json_is_integer(kind) || json_integer_value(kind) != 0) goto done;

  json_t *pk = json_object_get(root, "pubkey");
  if (!pk || !json_is_string(pk)) goto done;
  const char *pks = json_string_value(pk);
  if (!pks || strlen(pks) != 64) goto done;
  memcpy(out->pubkey_hex, pks, 64);
  out->pubkey_hex[64] = '\0';

  json_t *ca = json_object_get(root, "created_at");
  if (ca && json_is_integer(ca)) out->created_at = (int64_t)json_integer_value(ca);

  json_t *content = json_object_get(root, "content");
  const char *cs = (content && json_is_string(content)) ? json_string_value(content) : NULL;
  /* An empty string content is valid (edge case) — parse returns OK with
   * everything empty. */
  rc = nh_profile_parse_content_json(cs ? cs : "{}", out);

done:
  json_decref(root);
  return rc;
}

/* ── Cache ─────────────────────────────────────────────────── */

static int user_is_safe(const char *user) {
  if (!user || !*user) return 0;
  for (const char *p = user; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return 0;
    if (c == '.' && p == user) return 0; /* no dotfiles */
  }
  return 1;
}

static int write_atomic_root(const char *path, const void *data, size_t len) {
  char tmp[1024];
  int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, (const char *)data + off, len - off);
    if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
    off += (size_t)w;
  }
  if (fchmod(fd, 0644) != 0) { /* best-effort */ }
  if (close(fd) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
  return 0;
}

nh_profile_rc nh_profile_cache_write(const char *cache_dir, const char *user,
                                     const nh_profile_metadata *meta) {
  if (!cache_dir || !user || !meta) return NH_PROFILE_ERR_ARG;
  if (!user_is_safe(user)) return NH_PROFILE_ERR_ARG;

  /* Ensure the cache directory exists (best-effort; parent must exist). */
  struct stat st;
  if (stat(cache_dir, &st) != 0) {
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) return NH_PROFILE_ERR_IO;
  } else if (!S_ISDIR(st.st_mode)) {
    return NH_PROFILE_ERR_IO;
  }

  char path[1024];
  int pn = snprintf(path, sizeof path, "%s/%s.json", cache_dir, user);
  if (pn <= 0 || (size_t)pn >= sizeof path) return NH_PROFILE_ERR_ARG;

  json_t *root = json_object();
  json_object_set_new(root, "user", json_string(user));
  json_object_set_new(root, "pubkey", json_string(meta->pubkey_hex));
  json_object_set_new(root, "name", json_string(meta->name));
  json_object_set_new(root, "display_name", json_string(meta->display_name));
  json_object_set_new(root, "nip05", json_string(meta->nip05));
  json_object_set_new(root, "picture", json_string(meta->picture_url));
  json_object_set_new(root, "created_at", json_integer(meta->created_at));
  json_object_set_new(root, "fetched_at", json_integer((json_int_t)time(NULL)));
  char *s = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
  json_decref(root);
  if (!s) return NH_PROFILE_ERR_IO;

  int rc = write_atomic_root(path, s, strlen(s));
  free(s);
  return rc == 0 ? NH_PROFILE_OK : NH_PROFILE_ERR_IO;
}

int64_t nh_profile_cache_last_refresh(const char *cache_dir, const char *user) {
  if (!cache_dir || !user_is_safe(user)) return 0;
  char path[1024];
  int pn = snprintf(path, sizeof path, "%s/%s.json", cache_dir, user);
  if (pn <= 0 || (size_t)pn >= sizeof path) return 0;
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (int64_t)st.st_mtime;
}
