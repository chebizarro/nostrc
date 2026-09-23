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
#include "pam_qr_render.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define GREETER_DIR_DEFAULT "/run/nostr-auth/greeter"
#define GREETER_JSON_NAME   "current.json"
#define GREETER_PNG_NAME    "current.png"
#define GREETER_AVATAR_NAME "avatar.png"

/* Confidentiality: current.json embeds the full nostrconnect:// URI
 * including its one-time `secret=…` pairing token, and current.png
 * encodes the same URI in the QR modules. World-readable 0644 files
 * let any local unprivileged user race the phone signer and BURN the
 * single-use secret — the pubkey gate refuses their proof but the
 * victim's login/unlock fails (griefing / DoS). See
 * docs/reviews/greeter-artifact-secret-confidentiality-2026-09-22.md
 * and beads nostrc-3c7n. Publish the drop dir 0750 and the files
 * 0640, root:nostr-auth-greeter — only members of that group (gdm at
 * the greeter, the seated user during unlock via pam_group) can read
 * the manifest / QR. */
#define GREETER_GROUP_DEFAULT "nostr-auth-greeter"
#define GREETER_DIR_MODE      0750
#define GREETER_FILE_MODE     0640

static char g_greeter_dir[512];
static char g_greeter_group[64] = GREETER_GROUP_DEFAULT;

static const char *greeter_dir(void) {
  return g_greeter_dir[0] ? g_greeter_dir : GREETER_DIR_DEFAULT;
}

static const char *greeter_group(void) {
  return g_greeter_group[0] ? g_greeter_group : GREETER_GROUP_DEFAULT;
}

/* Resolve the configured group name to a gid. Returns (gid_t)-1 on
 * lookup failure and logs the miss at most once per name — the caller
 * treats -1 as "no chown", which keeps the artifact publishable
 * (root:root, still 0640 so unprivileged reads are denied) even on a
 * host where the sysusers.d snippet has not been applied yet. */
static gid_t greeter_gid(void) {
  static char logged_missing[64];
  const char *name = greeter_group();
  errno = 0;
  struct group *gr = getgrnam(name);
  if (gr) return gr->gr_gid;
  if (strncmp(logged_missing, name, sizeof logged_missing) != 0) {
    /* Best-effort remember-and-warn: subsequent calls with the same
     * name stay silent, so a persistently misconfigured host does not
     * flood the journal on every publish. A different group name
     * (typically only in tests) resets the memo. */
    snprintf(logged_missing, sizeof logged_missing, "%s", name);
    syslog(LOG_WARNING,
           "nostr-authd: greeter group '%s' not found (getgrnam errno=%d) — "
           "publishing artifact 0640 root:root; the greeter extension will "
           "not be able to read /run/nostr-auth/greeter/ until the "
           "nostr-auth-greeter group is created (see sysusers.d snippet in "
           "packaging/sysusers.d/).",
           name, errno);
  }
  return (gid_t)-1;
}

void nh_broker_greeter_artifact_set_dir(const char *dir) {
  if (!dir || !dir[0]) { g_greeter_dir[0] = '\0'; return; }
  size_t n = strlen(dir);
  if (n >= sizeof g_greeter_dir) { g_greeter_dir[0] = '\0'; return; }
  memcpy(g_greeter_dir, dir, n + 1);
}

void nh_broker_greeter_artifact_set_group(const char *name) {
  if (!name || !name[0]) {
    snprintf(g_greeter_group, sizeof g_greeter_group, "%s",
             GREETER_GROUP_DEFAULT);
    return;
  }
  snprintf(g_greeter_group, sizeof g_greeter_group, "%s", name);
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

/* Same shape as parse_relays_csv but populates profile_relays. Kept
 * separate so the two lists can diverge without accidental crossover. */
static void parse_profile_relays_csv(nh_auth_conf *out, char *value) {
  out->profile_relays_count = 0;
  char *save = NULL;
  for (char *tok = strtok_r(value, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    tok = trim(tok);
    if (!*tok) continue;
    if (out->profile_relays_count >= NH_AUTH_CONF_RELAYS_MAX) break;
    if (strncmp(tok, "ws://", 5) != 0 && strncmp(tok, "wss://", 6) != 0)
      continue;
    if (copy_bounded(out->profile_relays[out->profile_relays_count],
                     NH_AUTH_CONF_RELAY_URL_MAX + 1, tok) == 0)
      out->profile_relays_count++;
  }
}


static void parse_home_relays_csv(nh_auth_conf *out, char *value) {
  out->home_relays_count = 0;
  char *save = NULL;
  for (char *tok = strtok_r(value, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    tok = trim(tok);
    if (!*tok) continue;
    if (out->home_relays_count >= NH_AUTH_CONF_RELAYS_MAX) break;
    if (strncmp(tok, "ws://", 5) != 0 && strncmp(tok, "wss://", 6) != 0)
      continue;
    if (copy_bounded(out->home_relays[out->home_relays_count],
                     NH_AUTH_CONF_RELAY_URL_MAX + 1, tok) == 0)
      out->home_relays_count++;
  }
}

static void parse_blossom_servers_csv(nh_auth_conf *out, char *value) {
  out->blossom_servers_count = 0;
  char *save = NULL;
  for (char *tok = strtok_r(value, ",", &save); tok;
       tok = strtok_r(NULL, ",", &save)) {
    tok = trim(tok);
    if (!*tok) continue;
    if (out->blossom_servers_count >= NH_AUTH_CONF_RELAYS_MAX) break;
    if (strncmp(tok, "https://", 8) != 0) continue;
    if (copy_bounded(out->blossom_servers[out->blossom_servers_count],
                     NH_AUTH_CONF_RELAY_URL_MAX + 1, tok) == 0)
      out->blossom_servers_count++;
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
    } else if (!strcmp(key, "profile_relays")) {
      parse_profile_relays_csv(out, value);
    } else if (!strcmp(key, "profile_fetch")) {
      if (!strcasecmp(value, "on") || !strcmp(value, "1") ||
          !strcasecmp(value, "true") || !strcasecmp(value, "yes"))
        out->profile_fetch = 1;
      else if (!strcasecmp(value, "off") || !strcmp(value, "0") ||
               !strcasecmp(value, "false") || !strcasecmp(value, "no"))
        out->profile_fetch = 2;
    } else if (!strcmp(key, "profile_image_user")) {
      (void)copy_bounded(out->profile_image_user,
                         sizeof out->profile_image_user, value);
    } else if (!strcmp(key, "nip05_resolve")) {
      /* Tri-state: 0 = unset (default on), 1 = on, 2 = off. Same
       * shape as profile_fetch so a site admin can flip it with the
       * same idiom. */
      if (!strcasecmp(value, "on") || !strcmp(value, "1") ||
          !strcasecmp(value, "true") || !strcasecmp(value, "yes"))
        out->nip05_resolve = 1;
      else if (!strcasecmp(value, "off") || !strcmp(value, "0") ||
               !strcasecmp(value, "false") || !strcasecmp(value, "no"))
        out->nip05_resolve = 2;
    } else if (!strcmp(key, "nip05_cache_ttl") ||
               !strcmp(key, "nip05_cache_ttl_seconds")) {
      (void)parse_uint32(value, &out->nip05_cache_ttl_seconds);
    } else if (!strcmp(key, "nip05_image_user")) {
      (void)copy_bounded(out->nip05_image_user,
                         sizeof out->nip05_image_user, value);
    } else if (!strcmp(key, "home_relays")) {
      parse_home_relays_csv(out, value);
    } else if (!strcmp(key, "blossom_servers")) {
      parse_blossom_servers_csv(out, value);
    } else if (!strcmp(key, "porthome_bandwidth_bytes_per_load")) {
      char *end = NULL; errno = 0;
      unsigned long long v = strtoull(value, &end, 10);
      if (!errno && end && !*end)
        out->porthome_bandwidth_bytes_per_load = (uint64_t)v;
    } else if (!strcmp(key, "porthome_load_timeout_sec")) {
      (void)parse_uint32(value, &out->porthome_load_timeout_sec);
    } else if (!strcmp(key, "porthome_max_home_bytes")) {
      char *end = NULL; errno = 0;
      unsigned long long v = strtoull(value, &end, 10);
      if (!errno && end && !*end)
        out->porthome_max_home_bytes = (uint64_t)v;
    } else if (!strcmp(key, "porthome_enroll_wrap_key")) {
      if (!strcasecmp(value, "on") || !strcmp(value, "1") ||
          !strcasecmp(value, "true") || !strcasecmp(value, "yes"))
        out->porthome_enroll_wrap_key = 1;
      else if (!strcasecmp(value, "off") || !strcmp(value, "0") ||
               !strcasecmp(value, "false") || !strcasecmp(value, "no"))
        out->porthome_enroll_wrap_key = 2;
    } else {
      /* Non-fatal: unknown keys are ignored. */
    }
  }
  fclose(f);
  return 0;
}

/* Ensure the greeter dir exists and enforce the confidentiality
 * posture (mode 0750, group=greeter_gid). systemd's RuntimeDirectory=
 * / ExecStartPre normally owns creation on the production host; we
 * re-apply chmod+chgrp on every publish so a directory recreated by
 * hand (e.g. during headless tests, or after a stale-state cleanup)
 * still ends up with the right permissions before we drop the
 * pairing-secret-bearing manifest into it. */
static int ensure_dir(const char *dir) {
  struct stat st;
  int existed = (stat(dir, &st) == 0);
  if (existed) {
    if (!S_ISDIR(st.st_mode)) return -1;
  } else {
    if (mkdir(dir, GREETER_DIR_MODE) != 0) return -1;
  }
  /* chmod+chgrp after mkdir so umask cannot loosen the mode; on
   * pre-existing dirs we still re-tighten in case an operator or an
   * older broker left mode 0755. Failures are non-fatal — an EPERM
   * from chown means we're running unprivileged in a test (where the
   * broker's own uid already owns the tmpdir) or the group could not
   * be resolved, and either way the tighter chmod above still holds. */
  int rc_chmod = chmod(dir, GREETER_DIR_MODE);
  (void)rc_chmod;
  gid_t gid = greeter_gid();
  if (gid != (gid_t)-1) {
    int rc_chown = chown(dir, (uid_t)-1, gid);
    (void)rc_chown;
  }
  return 0;
}

/* Best-effort atomic write with confidentiality bits enforced BEFORE
 * the rename: create <dst>.tmp with mode 0 (so an interleaved reader
 * cannot observe a wider mode even for a scheduler tick), fchown to
 * the greeter group (when resolvable), fchmod to `mode`, then rename.
 * The final artifact is never briefly world-readable — this is the
 * property that closes the racing-signer griefing hole described at
 * docs/reviews/greeter-artifact-secret-confidentiality-2026-09-22.md. */
static int write_atomic_locked(const char *dst, mode_t mode, gid_t gid,
                               const void *data, size_t len) {
  char tmp[600];
  int nprint = snprintf(tmp, sizeof tmp, "%s.tmp", dst);
  if (nprint <= 0 || (size_t)nprint >= sizeof tmp) return -1;
  /* Open with mode 0 so umask cannot widen us and any concurrent
   * opener (there shouldn't be one, but a hostile local user watching
   * the dir with inotify could try) sees a strictly narrower window. */
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0);
  if (fd < 0) return -1;
  ssize_t off = 0;
  while ((size_t)off < len) {
    ssize_t w = write(fd, (const char *)data + off, len - (size_t)off);
    if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
    off += w;
  }
  /* Set group ownership BEFORE widening the mode — otherwise the file
   * spends a scheduler tick as root:root with mode 0640, which the
   * seated user (member of nostr-auth-greeter but not root) cannot
   * read. gid == (gid_t)-1 means "group lookup failed"; we leave the
   * file root:root and still ship it 0640 so the pairing secret stays
   * confidential (the greeter extension just won't be able to render
   * this publish; that is a UX regression, not a security regression). */
  if (gid != (gid_t)-1) {
    int rc_fchown = fchown(fd, (uid_t)-1, gid);
    /* Non-fatal — an EPERM here means we're headless / unprivileged
     * (the running uid does not own the target gid). The file is
     * still narrower than the pre-fix 0644 world-readable posture:
     * fchmod(0640) below denies "other". */
    (void)rc_fchown;
  }
  if (fchmod(fd, mode) != 0) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  if (close(fd) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, dst) != 0) { unlink(tmp); return -1; }
  return 0;
}

/* Render the URI's QR into a PNG and drop it next to current.json.
 * Design decision D3 said "keep the encoder out of the root daemon";
 * we relax that here because the greeter-extension consumer contract
 * requires current.png alongside current.json for every publish, and
 * having two disjoint producers (broker for JSON, PAM for PNG) leaves the
 * artifact incomplete whenever a broker-only test or a non-PAM caller
 * publishes. Vendored Nayuki qrcodegen is ~1 kLoC of pure arithmetic on
 * an input we generated ourselves; linking it into the runtime is safe.
 * Failure is non-fatal: a missing PNG leaves the extension hidden but
 * does not break login. */
static void publish_greeter_png(const char *dir, const char *uri) {
  if (!dir || !uri || !uri[0]) return;
  unsigned char *png = NULL;
  size_t plen = 0;
  if (nh_pam_qr_render_png(uri, 9, 8, &png, &plen) != NH_QR_RENDER_OK ||
      !png)
    return;
  char path[600];
  int n = snprintf(path, sizeof path, "%s/%s", dir, GREETER_PNG_NAME);
  if (n <= 0 || (size_t)n >= sizeof path) { free(png); return; }
  /* Same confidentiality rules as current.json — the QR encodes the
   * pairing secret so it must land 0640 root:nostr-auth-greeter. */
  (void)write_atomic_locked(path, GREETER_FILE_MODE, greeter_gid(), png, plen);
  free(png);
}

/* Copy a source PNG into <dir>/avatar.png (mode 0644). Returns 0 on
 * success. Non-fatal caller-side — greeter still gets the account
 * block without an avatar field. Bounded reads (16 KiB) so a very
 * large icon (AccountsService caps at 1 MiB but the extension only
 * renders a tile) doesn't push the greeter drop above the extension's
 * 8 KiB manifest budget (the avatar PNG is a sibling, not embedded).
 * We still cap at 512 KiB defensively because the avatar is copied on
 * every login attempt and we don't want a symlinked pathological file
 * to keep us busy. */
#define AVATAR_COPY_CAP (512u * 1024u)
static int copy_avatar(const char *dir, const char *src) {
  if (!dir || !src || !src[0]) return -1;
  FILE *in = fopen(src, "rb");
  if (!in) return -1;
  char dst[600];
  int n = snprintf(dst, sizeof dst, "%s/%s", dir, GREETER_AVATAR_NAME);
  if (n <= 0 || (size_t)n >= sizeof dst) { fclose(in); return -1; }
  unsigned char *buf = malloc(AVATAR_COPY_CAP);
  if (!buf) { fclose(in); return -1; }
  size_t total = fread(buf, 1, AVATAR_COPY_CAP, in);
  int overrun = (fgetc(in) != EOF);
  fclose(in);
  int rc = -1;
  /* Avatar mode matches the manifest (0640 root:nostr-auth-greeter):
   * although the avatar itself is not a secret, keeping the whole
   * drop directory at one mode/group means the greeter extension
   * either can read every file or none, so a partial-read failure
   * mode (label + missing face, or vice versa) cannot happen. */
  if (!overrun && total > 0)
    rc = write_atomic_locked(dst, GREETER_FILE_MODE, greeter_gid(), buf, total);
  free(buf);
  return rc;
}

int nh_broker_greeter_artifact_write(const char *tx_id,
                                     const char *display_json,
                                     const nh_broker_greeter_account *account) {
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
  /* Prefer the provider's absolute expires_at (unix seconds); fall back to
   * time(NULL) + expires_in_ms/1000 for older providers. The greeter
   * extension (greeter-extension/nostr-login-qr@nostrc/extension.js)
   * expects an ABSOLUTE unix-seconds timestamp and hides when
   * now_sec >= expires_at — emitting a relative duration would make the
   * QR look already expired. */
  json_t *expires_at_j = json_object_get(root, "expires_at");
  json_t *expires_in_j = json_object_get(root, "expires_in_ms");
  if (!uri || !json_is_string(uri) || !pairing || !json_is_string(pairing)) {
    json_decref(root);
    return -1;
  }
  int64_t expires_at = 0;
  if (expires_at_j && json_is_integer(expires_at_j)) {
    expires_at = (int64_t)json_integer_value(expires_at_j);
  } else if (expires_in_j && json_is_integer(expires_in_j)) {
    json_int_t rel = json_integer_value(expires_in_j);
    if (rel > 0)
      expires_at = (int64_t)time(NULL) + (int64_t)(rel / 1000);
  }

  /* Copy uri_str out of the parsed root so we can render the PNG after
   * the JSON tree is released. */
  char *uri_copy = strdup(json_string_value(uri));
  json_t *out = json_object();
  if (!out || !uri_copy) {
    if (out) json_decref(out);
    free(uri_copy);
    json_decref(root);
    return -1;
  }
  int bad =
      json_object_set_new(out, "tx_id", json_string(tx_id ? tx_id : "")) ||
      json_object_set_new(out, "png", json_string(GREETER_PNG_NAME)) ||
      json_object_set_new(out, "uri", json_string(uri_copy)) ||
      json_object_set_new(out, "pairing_code",
                          json_string(json_string_value(pairing))) ||
      json_object_set_new(out, "hint",
                          hint && json_is_string(hint)
                              ? json_string(json_string_value(hint))
                              : json_string("")) ||
      json_object_set_new(out, "expires_at",
                          json_integer((json_int_t)expires_at));
  json_decref(root);
  if (bad) { json_decref(out); free(uri_copy); return -1; }

  /* Attach the optional account block. Copy the avatar FIRST so we
   * only claim "avatar":"avatar.png" if the sibling file made it to
   * disk — otherwise the extension would flash a broken image tile
   * for the ~200 ms between the JSON publish and our failed copy.
   * A missing icon source is normal on first login (AccountsService
   * icon appears after the profile-refresh hook runs post-login);
   * we still emit username + display_name + identifier so the
   * greeter can show the label immediately. */
  if (account && account->username && account->username[0]) {
    int have_avatar = 0;
    if (account->icon_source_path && account->icon_source_path[0])
      have_avatar = (copy_avatar(dir, account->icon_source_path) == 0);

    json_t *ao = json_object();
    int abad = !ao;
    if (!abad)
      abad = json_object_set_new(ao, "username",
                                 json_string(account->username)) != 0;
    if (!abad && account->display_name && account->display_name[0])
      abad = json_object_set_new(ao, "display_name",
                                 json_string(account->display_name)) != 0;
    if (!abad && account->identifier && account->identifier[0])
      abad = json_object_set_new(ao, "identifier",
                                 json_string(account->identifier)) != 0;
    if (!abad && have_avatar)
      abad = json_object_set_new(ao, "avatar",
                                 json_string(GREETER_AVATAR_NAME)) != 0;
    if (!abad)
      abad = json_object_set_new(out, "account", ao) != 0;
    else if (ao)
      json_decref(ao);
    /* Failure to build the account block is non-fatal: publish the
     * base manifest without it rather than dropping the whole login
     * artifact. */
  }

  char *j = json_dumps(out, JSON_COMPACT);
  json_decref(out);
  if (!j) { free(uri_copy); return -1; }
  char path[600];
  int nprint = snprintf(path, sizeof path, "%s/%s", dir, GREETER_JSON_NAME);
  int rc = -1;
  if (nprint > 0 && (size_t)nprint < sizeof path)
    rc = write_atomic_locked(path, GREETER_FILE_MODE, greeter_gid(),
                             j, strlen(j));
  free(j);
  /* Best-effort PNG next to the manifest — the greeter extension expects
   * both files present at every publish. Non-fatal on failure. */
  if (rc == 0) publish_greeter_png(dir, uri_copy);
  free(uri_copy);
  return rc;
}

void nh_broker_greeter_artifact_remove(void) {
  const char *dir = greeter_dir();
  char path[600];
  if (snprintf(path, sizeof path, "%s/%s", dir, GREETER_JSON_NAME) > 0)
    (void)unlink(path);
  if (snprintf(path, sizeof path, "%s/%s", dir, GREETER_PNG_NAME) > 0)
    (void)unlink(path);
  /* Retire the account avatar too so a subsequent login for a
   * different user cannot flash the previous account's face. Safe
   * when nothing was written — unlink of a missing file is silent. */
  if (snprintf(path, sizeof path, "%s/%s", dir, GREETER_AVATAR_NAME) > 0)
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
