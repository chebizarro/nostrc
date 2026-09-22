/* auth.conf parser + greeter-artifact drop (design D12 + §5.3).
 *
 * The auth broker owns exactly one flat config file today. Format is
 * key=value, one per line, `#` or `;` comments, no sections. Unknown keys
 * are ignored — the design deliberately makes parse failures non-fatal so a
 * typo in nip46_qr_max_concurrent cannot brick logins on a machine that
 * still has a working local-passphrase provider.
 *
 * The greeter-artifact helpers publish /run/nostr-auth/greeter/current.json
 * (and, when present, current.png) — the contract the gnome-shell greeter
 * extension consumes for live QR rendering. See auth_broker.h. */
#define _GNU_SOURCE
#include "auth_broker.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GREETER_DIR_DEFAULT "/run/nostr-auth/greeter"
#define GREETER_JSON_NAME   "current.json"
#define GREETER_PNG_NAME    "current.png"

static char g_greeter_dir[512];

static const char *greeter_dir(void) {
  return g_greeter_dir[0] ? g_greeter_dir : GREETER_DIR_DEFAULT;
}

void nh_broker_greeter_artifact_set_dir(const char *dir) {
  if (!dir || !dir[0]) { g_greeter_dir[0] = '\0'; return; }
  size_t n = strlen(dir);
  if (n >= sizeof g_greeter_dir) { g_greeter_dir[0] = '\0'; return; }
  memcpy(g_greeter_dir, dir, n + 1);
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) { s[--n] = '\0'; }
  return s;
}

/* Copy value into the caller-supplied buffer, capped and NUL-terminated.
 * Returns 0 on success, -1 if the value would overflow (silently dropped). */
static int copy_bounded(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n >= cap) return -1;
  memcpy(dst, src, n + 1);
  return 0;
}

static int parse_uint32(const char *s, uint32_t *out) {
  if (!s || !*s) return -1;
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || !end || *end || v > 0xffffffffUL) return -1;
  *out = (uint32_t)v;
  return 0;
}

static void parse_relays_csv(nh_auth_conf *out, char *value) {
  out->nip46_qr_relays_count = 0;
  char *save = NULL;
  for (char *tok = strtok_r(value, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    tok = trim(tok);
    if (!*tok) continue;
    if (out->nip46_qr_relays_count >= NH_AUTH_CONF_RELAYS_MAX) break;
    if (strncmp(tok, "ws://", 5) != 0 && strncmp(tok, "wss://", 6) != 0)
      continue;
    if (copy_bounded(out->nip46_qr_relays[out->nip46_qr_relays_count],
                     NH_AUTH_CONF_RELAY_URL_MAX + 1, tok) == 0)
      out->nip46_qr_relays_count++;
  }
}

int nh_auth_conf_load(const char *path, nh_auth_conf *out) {
  if (!out) return 0;
  memset(out, 0, sizeof *out);
  if (!path || !*path) return 0;
  FILE *f = fopen(path, "re");
  if (!f) return 0; /* missing file is not an error */
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    /* strip trailing newline */
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = '\0';
    p = trim(p);
    if (!*p || *p == '#' || *p == ';') continue;
    char *eq = strchr(p, '=');
    if (!eq) continue; /* silently skip malformed lines */
    *eq = '\0';
    char *key = trim(p);
    char *value = trim(eq + 1);
    if (!*key || !*value) continue;
    if (!strcmp(key, "nip46_qr_relays")) {
      parse_relays_csv(out, value);
    } else if (!strcmp(key, "nip46_qr_wait_ms")) {
      (void)parse_uint32(value, &out->nip46_qr_wait_ms);
    } else if (!strcmp(key, "nip46_qr_render")) {
      (void)copy_bounded(out->nip46_qr_render, sizeof out->nip46_qr_render,
                         value);
    } else if (!strcmp(key, "nip46_qr_max_concurrent")) {
      (void)parse_uint32(value, &out->nip46_qr_max_concurrent);
    } else {
      /* Non-fatal: unknown keys are ignored. */
    }
  }
  fclose(f);
  return 0;
}

/* Ensure the greeter dir exists (best effort — a systemd tmpfiles.d entry
 * or the daemon's ExecStartPre normally owns creation). */
static int ensure_dir(const char *dir) {
  struct stat st;
  if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
  if (mkdir(dir, 0755) == 0) return 0;
  return -1;
}

/* Best-effort atomic write: write to <dst>.tmp, chmod, then rename. */
static int write_atomic(const char *dst, mode_t mode, const void *data,
                        size_t len) {
  char tmp[600];
  int nprint = snprintf(tmp, sizeof tmp, "%s.tmp", dst);
  if (nprint <= 0 || (size_t)nprint >= sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return -1;
  ssize_t off = 0;
  while ((size_t)off < len) {
    ssize_t w = write(fd, (const char *)data + off, len - (size_t)off);
    if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
    off += w;
  }
  if (fchmod(fd, mode) != 0) { /* not fatal — mode came from O_CREAT */ }
  if (close(fd) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, dst) != 0) { unlink(tmp); return -1; }
  return 0;
}

int nh_broker_greeter_artifact_write(const char *tx_id,
                                     const char *display_json) {
  if (!display_json || !display_json[0]) return -1;
  const char *dir = greeter_dir();
  if (ensure_dir(dir) != 0) return -1;

  /* Parse the provider's DISPLAY_REQUIRED payload so we can copy the fields
   * the greeter extension consumes verbatim and add the artifact-specific
   * ones (tx_id, png filename). */
  json_error_t je;
  json_t *root = json_loads(display_json, 0, &je);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return -1; }

  json_t *uri = json_object_get(root, "uri");
  json_t *pairing = json_object_get(root, "pairing_code");
  json_t *hint = json_object_get(root, "hint");
  json_t *expires = json_object_get(root, "expires_in_ms");
  if (!uri || !json_is_string(uri) || !pairing || !json_is_string(pairing)) {
    json_decref(root);
    return -1;
  }

  json_t *out = json_object();
  if (!out) { json_decref(root); return -1; }
  int bad =
      json_object_set_new(out, "tx_id", json_string(tx_id ? tx_id : "")) ||
      json_object_set_new(out, "png", json_string(GREETER_PNG_NAME)) ||
      json_object_set_new(out, "uri", json_string(json_string_value(uri))) ||
      json_object_set_new(out, "pairing_code",
                          json_string(json_string_value(pairing))) ||
      json_object_set_new(out, "hint",
                          hint && json_is_string(hint)
                              ? json_string(json_string_value(hint))
                              : json_string("")) ||
      json_object_set_new(out, "expires_at",
                          expires && json_is_integer(expires)
                              ? json_integer(json_integer_value(expires))
                              : json_integer(0));
  json_decref(root);
  if (bad) { json_decref(out); return -1; }

  char *j = json_dumps(out, JSON_COMPACT);
  json_decref(out);
  if (!j) return -1;
  char path[600];
  int nprint = snprintf(path, sizeof path, "%s/%s", dir, GREETER_JSON_NAME);
  int rc = -1;
  if (nprint > 0 && (size_t)nprint < sizeof path)
    rc = write_atomic(path, 0644, j, strlen(j));
  free(j);
  return rc;
}

void nh_broker_greeter_artifact_remove(void) {
  const char *dir = greeter_dir();
  char path[600];
  if (snprintf(path, sizeof path, "%s/%s", dir, GREETER_JSON_NAME) > 0)
    (void)unlink(path);
  if (snprintf(path, sizeof path, "%s/%s", dir, GREETER_PNG_NAME) > 0)
    (void)unlink(path);
}

/* Forward the QR default relays into the provider's module-scoped fallback.
 * Kept as a broker-owned setter so callers don't need to include the
 * provider header. */
extern void nh_auth_provider_nip46_qr_set_default_relays(
    const char *const *relays, size_t n_relays);

void nh_auth_broker_set_qr_default_relays(const char *const *relays,
                                          size_t n_relays) {
  nh_auth_provider_nip46_qr_set_default_relays(relays, n_relays);
}
