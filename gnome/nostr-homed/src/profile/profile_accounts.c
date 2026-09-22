/* profile_accounts.c — install the icon + real name into AccountsService.
 *
 * Strategy (works for nostr-homed accounts, which live in the identity
 * authority's NSS projection, NOT /etc/passwd):
 *
 *   1. Icon: copy the re-encoded PNG into
 *      /var/lib/AccountsService/icons/<user> (mode 0644, root:root)
 *      atomically (write .tmp + rename). AccountsService's inotify
 *      watches the icons/ directory and reloads on the next
 *      FindUserByName / property read.
 *   2. Real name / icon path: write /var/lib/AccountsService/users/<user>
 *      as a keyfile with [User] / RealName= / Icon= / SystemAccount=false.
 *      Existing keys (Session=, InputSource0= etc.) are preserved by
 *      merging with the on-disk file if present.
 *
 * We deliberately do NOT drive SetIconFile / SetRealName on the D-Bus
 * API: SetRealName invokes /usr/sbin/usermod, which refuses to touch
 * an account that is not in /etc/passwd — every nostr-homed user hits
 * that path and the write fails silently on the D-Bus side. Writing the
 * keyfile is exactly what accountsservice ultimately does for
 * SetIconFile; doing it directly avoids the usermod dependency without
 * losing any AccountsService semantics — the resulting file is
 * indistinguishable from one accountsservice would have written itself.
 *
 * We still notify AccountsService via the D-Bus API after writing so a
 * live gnome-shell picks up the change on the next repaint (best-effort
 * — the inotify watch would eventually catch it anyway). */
#define _GNU_SOURCE
#include "nostr_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ACC_BUS         "org.freedesktop.Accounts"
#define ACC_ROOT        "/org/freedesktop/Accounts"
#define ACC_IF          "org.freedesktop.Accounts"
#define CALL_TIMEOUT_MS 5000
#define AS_USERS_DIR    "/var/lib/AccountsService/users"
#define AS_ICONS_DIR    "/var/lib/AccountsService/icons"

/* username must already have passed store validation; belt-and-braces:
 * refuse anything that could escape the containing directory. */
static int user_is_safe(const char *user) {
  if (!user || !*user) return 0;
  for (const char *p = user; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return 0;
  }
  if (user[0] == '.') return 0;
  return 1;
}

/* Atomic file write: <dst>.tmp + fchmod + rename. */
static int write_atomic(const char *dst, mode_t mode,
                        const void *data, size_t len) {
  char tmp[1024];
  int n = snprintf(tmp, sizeof tmp, "%s.tmp", dst);
  if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, (const char *)data + off, len - off);
    if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
    off += (size_t)w;
  }
  if (fchmod(fd, mode) != 0) { /* best-effort */ }
  if (close(fd) != 0) { unlink(tmp); return -1; }
  if (rename(tmp, dst) != 0) { unlink(tmp); return -1; }
  return 0;
}

/* Copy the PNG bytes into /var/lib/AccountsService/icons/<user>. */
static int copy_png_to_icons(const char *user, const char *png_path) {
  int in = open(png_path, O_RDONLY | O_CLOEXEC);
  if (in < 0) return -1;
  struct stat st;
  if (fstat(in, &st) != 0 || st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) {
    close(in); return -1;
  }
  void *buf = malloc((size_t)st.st_size);
  if (!buf) { close(in); return -1; }
  size_t got = 0;
  while (got < (size_t)st.st_size) {
    ssize_t r = read(in, (char *)buf + got, (size_t)st.st_size - got);
    if (r < 0) { if (errno == EINTR) continue; close(in); free(buf); return -1; }
    if (r == 0) break;
    got += (size_t)r;
  }
  close(in);

  if (mkdir(AS_ICONS_DIR, 0775) != 0 && errno != EEXIST) { free(buf); return -1; }
  char dst[512];
  int n = snprintf(dst, sizeof dst, "%s/%s", AS_ICONS_DIR, user);
  if (n <= 0 || (size_t)n >= sizeof dst) { free(buf); return -1; }
  int rc = write_atomic(dst, 0644, buf, got);
  free(buf);
  return rc;
}

/* Merge/write the AccountsService user keyfile so RealName= (optional)
 * and Icon= (optional) are set without stomping on unrelated keys like
 * Session= or InputSource0=. GKeyFile handles the merge; missing file
 * is fine. */
static int write_user_keyfile(const char *user, const char *real_name,
                              const char *icon_abs_path) {
  char path[512];
  int n = snprintf(path, sizeof path, "%s/%s", AS_USERS_DIR, user);
  if (n <= 0 || (size_t)n >= sizeof path) return -1;

  if (mkdir(AS_USERS_DIR, 0700) != 0 && errno != EEXIST) return -1;

  GKeyFile *kf = g_key_file_new();
  /* Load existing file if any; ignore errors (fresh install). */
  (void)g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  /* Always set SystemAccount=false for a nostr user account so the
   * greeter lists it. Existing "false" is left alone by g_key_file_set. */
  g_key_file_set_boolean(kf, "User", "SystemAccount", FALSE);
  if (real_name && *real_name)
    g_key_file_set_string(kf, "User", "RealName", real_name);
  if (icon_abs_path && *icon_abs_path)
    g_key_file_set_string(kf, "User", "Icon", icon_abs_path);

  gsize out_len = 0;
  gchar *data = g_key_file_to_data(kf, &out_len, NULL);
  g_key_file_free(kf);
  if (!data) return -1;
  int rc = write_atomic(path, 0644, data, out_len);
  g_free(data);
  return rc;
}

/* Poke AccountsService so a running gnome-shell reloads the properties.
 * Best-effort; a bus failure never fails the install. */
static void notify_accountsservice(const char *user) {
  GError *gerr = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &gerr);
  if (!bus) { g_clear_error(&gerr); return; }
  GVariant *reply = g_dbus_connection_call_sync(
      bus, ACC_BUS, ACC_ROOT, ACC_IF, "FindUserByName",
      g_variant_new("(s)", user), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, &gerr);
  if (reply) g_variant_unref(reply);
  else g_clear_error(&gerr);
  g_object_unref(bus);
}

nh_profile_rc nh_profile_accounts_install(const char *user,
                                          const char *png_path,
                                          const char *real_name) {
  if (!user_is_safe(user)) return NH_PROFILE_ERR_ARG;

  int did_icon = 0;
  char icon_dst[512] = {0};
  if (png_path && *png_path) {
    if (copy_png_to_icons(user, png_path) == 0) {
      snprintf(icon_dst, sizeof icon_dst, "%s/%s", AS_ICONS_DIR, user);
      did_icon = 1;
    }
  }

  int did_keyfile = write_user_keyfile(user, real_name,
                                       did_icon ? icon_dst : NULL) == 0;

  /* accounts-daemon sources RealName from pw_gecos, NOT from the User
   * keyfile. Update the NSS projection so the greeter's next lookup
   * returns @real_name. Best-effort — a projection rebuild during a
   * later enrolment will reset it (the broker's post-login hook then
   * re-applies it). */
  int did_gecos = 0;
  if (real_name && *real_name) {
    const char *pp = getenv("NH_IDENTITY_PROJECTION");
    if (!pp || !*pp) pp = "/var/lib/nostr-auth/nss.db";
    if (nh_profile_projection_update_gecos(pp, user, real_name) == NH_PROFILE_OK)
      did_gecos = 1;
  }

  notify_accountsservice(user);

  if (did_icon || did_keyfile || did_gecos) return NH_PROFILE_OK;
  return NH_PROFILE_ERR_ACCOUNTS;
}

nh_profile_rc nh_profile_projection_update_gecos(const char *projection_path,
                                                 const char *user,
                                                 const char *gecos) {
  if (!user_is_safe(user)) return NH_PROFILE_ERR_ARG;
  if (!gecos) gecos = "";
  /* accountsservice reads gecos as a UTF-8 string and passes it
   * unchanged to the greeter; the sanitiser already stripped controls.
   * Cap at 127 chars to match the projection reader's valid_field()
   * limit for the gecos column. */
  if (strlen(gecos) > 127) return NH_PROFILE_ERR_ARG;
  const char *pp = projection_path && *projection_path
                   ? projection_path
                   : "/var/lib/nostr-auth/nss.db";

  sqlite3 *db = NULL;
  if (sqlite3_open_v2(pp, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK || !db) {
    if (db) sqlite3_close(db);
    return NH_PROFILE_ERR_IO;
  }
  sqlite3_busy_timeout(db, 5000);
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(db,
      "UPDATE passwd SET gecos=? WHERE name=?", -1, &st, NULL);
  if (rc != SQLITE_OK) { sqlite3_close(db); return NH_PROFILE_ERR_IO; }
  sqlite3_bind_text(st, 1, gecos, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, user,  -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_close(db);
  return rc == SQLITE_DONE ? NH_PROFILE_OK : NH_PROFILE_ERR_IO;
}
