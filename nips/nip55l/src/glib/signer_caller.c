/* signer_caller.c - see signer_caller.h
 *
 * The /proc resolution, the Flatpak/cgroup parsers and the web-origin rule
 * follow gnome/nostr-wallet-agent/src/nwa-caller.c (nostrc-phk4/1e31 asked
 * for the same model); this copy is synchronous, because the signer's
 * handlers are, and caches per unique bus name.
 */
#include "signer_caller.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <poll.h>
#endif
#ifdef NIP55L_HAVE_GIO_UNIX
#include <gio/gunixfdlist.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#endif

#ifndef NIP55L_ORIGIN_BRIDGE_PATHS
#define NIP55L_ORIGIN_BRIDGE_PATHS ""
#endif
#ifndef NIP55L_APPROVER_PATHS
#define NIP55L_APPROVER_PATHS ""
#endif
#ifndef NIP55L_APPROVER_FLATPAK_ID
#define NIP55L_APPROVER_FLATPAK_ID "org.nostr.Grotto"
#endif

void
signer_caller_free(SignerCaller *c)
{
  if (!c) return;
  g_free(c->sender);
  g_free(c->principal);
  g_free(c->app_id);
  g_free(c->exe);
  g_free(c->via);
  g_free(c);
}

SignerCaller *
signer_caller_copy(const SignerCaller *c)
{
  SignerCaller *n = g_new0(SignerCaller, 1);
  *n = *c;
  n->sender = g_strdup(c->sender);
  n->principal = g_strdup(c->principal);
  n->app_id = g_strdup(c->app_id);
  n->exe = g_strdup(c->exe);
  n->via = g_strdup(c->via);
  return n;
}

const gchar *
signer_caller_kind_to_string(SignerCallerKind kind)
{
  switch (kind) {
    case SIGNER_CALLER_UNKNOWN:       return "unidentified";
    case SIGNER_CALLER_FLATPAK:       return "flatpak";
    case SIGNER_CALLER_SNAP:          return "snap";
    case SIGNER_CALLER_SYSTEMD_SCOPE: return "systemd-scope";
    case SIGNER_CALLER_EXE:           return "executable";
    case SIGNER_CALLER_WEB_ORIGIN:    return "website";
  }
  return "unidentified";
}

/* ---- pure parsers ---- */

static gboolean
is_valid_app_id(const gchar *id)
{
  if (!id || !*id || strlen(id) > 255) return FALSE;
  for (const gchar *p = id; *p; p++)
    if (!(g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-'))
      return FALSE;
  return TRUE;
}

gchar *
signer_caller_parse_flatpak_info(const gchar *keyfile_data)
{
  if (!keyfile_data) return NULL;
  g_autoptr(GKeyFile) kf = g_key_file_new();
  if (!g_key_file_load_from_data(kf, keyfile_data, (gsize)-1, G_KEY_FILE_NONE, NULL))
    return NULL;
  gchar *name = g_key_file_get_string(kf, "Application", "name", NULL);
  if (!is_valid_app_id(name)) {
    g_free(name);
    return NULL;
  }
  return name;
}

/* systemd escapes '-' inside unit-name components as "\x2d". */
static gchar *
unescape_unit(const gchar *s)
{
  GString *o = g_string_new(NULL);
  for (const gchar *p = s; *p; p++) {
    if (p[0] == '\\' && p[1] == 'x' && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3])) {
      g_string_append_c(o, (gchar)(g_ascii_xdigit_value(p[2]) * 16 + g_ascii_xdigit_value(p[3])));
      p += 3;
    } else {
      g_string_append_c(o, *p);
    }
  }
  return g_string_free(o, FALSE);
}

/* app[-<launcher>]-<ApplicationID>[-<RANDOM>].scope / [@<RANDOM>].service:
 * the app id is the dotted (reverse-DNS) component. */
static gchar *
app_id_from_unit(const gchar *unit)
{
  if (!g_str_has_prefix(unit, "app-")) return NULL;
  g_autofree gchar *body = NULL;
  if (g_str_has_suffix(unit, ".scope"))
    body = g_strndup(unit + 4, strlen(unit) - 4 - strlen(".scope"));
  else if (g_str_has_suffix(unit, ".service"))
    body = g_strndup(unit + 4, strlen(unit) - 4 - strlen(".service"));
  else
    return NULL;
  gchar *at = strchr(body, '@');
  if (at) *at = '\0';
  g_auto(GStrv) parts = g_strsplit(body, "-", -1);
  for (guint i = 0; parts[i]; i++) {
    g_autofree gchar *cand = unescape_unit(parts[i]);
    if (strchr(cand, '.') && is_valid_app_id(cand))
      return g_steal_pointer(&cand);
  }
  return NULL;
}

gchar *
signer_caller_parse_cgroup(const gchar *cgroup_data, SignerCallerKind *kind)
{
  if (!cgroup_data) return NULL;
  g_auto(GStrv) lines = g_strsplit(cgroup_data, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const gchar *path = strchr(lines[i], ':');
    if (!path || !(path = strchr(path + 1, ':'))) continue;
    path++;
    g_auto(GStrv) comps = g_strsplit(path, "/", -1);
    for (gint j = (gint)g_strv_length(comps) - 1; j >= 0; j--) {
      const gchar *c = comps[j];
      if (g_str_has_prefix(c, "snap.")) {
        g_auto(GStrv) sp = g_strsplit(c, ".", 3);
        if (sp[0] && sp[1] && sp[2] && *sp[1] && is_valid_app_id(sp[1])) {
          if (kind) *kind = SIGNER_CALLER_SNAP;
          return g_strdup(sp[1]);
        }
      }
      gchar *id = app_id_from_unit(c);
      if (id) {
        if (kind) *kind = SIGNER_CALLER_SYSTEMD_SCOPE;
        return id;
      }
    }
  }
  return NULL;
}

/* A principal is stored as a GKeyFile key next to "|<npub>": no '=' (key/
 * value split), '[' ']' (locale suffix), '|' (our separator), control
 * characters, or surrounding blanks (GKeyFile strips them). */
static gboolean
principal_storable(const gchar *p)
{
  if (!p || !*p || strlen(p) > 1024 || !g_utf8_validate(p, -1, NULL)) return FALSE;
  if (g_ascii_isspace(p[0]) || g_ascii_isspace(p[strlen(p) - 1]) || p[0] == '#') return FALSE;
  for (const guchar *q = (const guchar *)p; *q; q++)
    if (*q < 0x20 || *q == 0x7f || *q == '=' || *q == '[' || *q == ']' || *q == '|')
      return FALSE;
  return TRUE;
}

gchar *
signer_caller_build_principal(SignerCallerKind kind, const gchar *app_id, const gchar *exe)
{
  gchar *p = NULL;
  switch (kind) {
    case SIGNER_CALLER_FLATPAK:
      if (is_valid_app_id(app_id)) p = g_strconcat("flatpak:", app_id, NULL);
      break;
    case SIGNER_CALLER_SNAP:
    case SIGNER_CALLER_SYSTEMD_SCOPE:
      /* The scope name is chosen by the process that created the scope;
       * without the executable it would identify nothing. */
      if (!is_valid_app_id(app_id) || !exe || !g_path_is_absolute(exe)) break;
      p = g_strconcat(kind == SIGNER_CALLER_SNAP ? "snap:" : "app:", app_id, ";exe:", exe, NULL);
      break;
    case SIGNER_CALLER_EXE:
      if (exe && g_path_is_absolute(exe)) p = g_strconcat("exe:", exe, NULL);
      break;
    case SIGNER_CALLER_WEB_ORIGIN:
      if (signer_caller_is_web_origin(app_id)) p = g_strdup(app_id);
      break;
    case SIGNER_CALLER_UNKNOWN:
      break;
  }
  if (p && !principal_storable(p)) g_clear_pointer(&p, g_free);
  return p;
}

/* ---- web origins (same rule as nwa_caller_is_web_origin) ---- */

static gboolean
is_dns_host(const gchar *h, gsize n)
{
  if (n == 0 || n > 253 || h[0] == '.' || h[n - 1] == '.') return FALSE;
  gsize label = 0;
  for (gsize i = 0; i < n; i++) {
    gchar ch = h[i];
    if (ch == '.') {
      if (label == 0 || h[i - 1] == '-') return FALSE;
      label = 0;
    } else if (g_ascii_islower(ch) || g_ascii_isdigit(ch) || ch == '-') {
      if (label == 0 && ch == '-') return FALSE;
      if (++label > 63) return FALSE;
    } else {
      return FALSE;
    }
  }
  return h[n - 1] != '-';
}

static gboolean
is_ipv6_literal(const gchar *h, gsize n)
{
  if (n < 4 || h[0] != '[' || h[n - 1] != ']') return FALSE;
  for (gsize i = 1; i + 1 < n; i++)
    if (!(g_ascii_isdigit(h[i]) || (h[i] >= 'a' && h[i] <= 'f') || h[i] == ':' || h[i] == '.'))
      return FALSE;
  return TRUE;
}

static gboolean
is_loopback_host(const gchar *h, gsize n)
{
  static const gchar sfx[] = ".localhost";
  if ((n == 9 && memcmp(h, "localhost", 9) == 0) ||
      (n == 9 && memcmp(h, "127.0.0.1", 9) == 0) ||
      (n == 5 && memcmp(h, "[::1]", 5) == 0))
    return TRUE;
  return n > sizeof sfx - 1 && memcmp(h + n - (sizeof sfx - 1), sfx, sizeof sfx - 1) == 0 &&
         is_dns_host(h, n);
}

gboolean
signer_caller_is_web_origin(const gchar *origin)
{
  if (!origin) return FALSE;
  gsize len = strlen(origin);
  if (len > 512) return FALSE;
  gboolean https = g_str_has_prefix(origin, "https://");
  if (!https && !g_str_has_prefix(origin, "http://")) return FALSE;
  const gchar *host = origin + (https ? 8 : 7);
  const gchar *end = origin + len;
  const gchar *hend = end;
  const gchar *search = host;
  if (*host == '[') {
    search = strchr(host, ']');
    if (!search) return FALSE;
  }
  const gchar *colon = strchr(search, ':');
  if (colon) {
    hend = colon;
    const gchar *port = colon + 1;
    gsize plen = (gsize)(end - port);
    if (plen == 0 || plen > 5 || port[0] == '0') return FALSE;
    guint64 v = 0;
    for (const gchar *p = port; p < end; p++) {
      if (!g_ascii_isdigit(*p)) return FALSE;
      v = v * 10 + (guint64)(*p - '0');
    }
    if (v == 0 || v > 65535) return FALSE;
    if (v == (https ? 443u : 80u)) return FALSE;
  }
  gsize hlen = (gsize)(hend - host);
  if (!(is_dns_host(host, hlen) || is_ipv6_literal(host, hlen))) return FALSE;
  return https || is_loopback_host(host, hlen);
}

/* ---- trusted executables ---- */

static GStrv
path_list(const gchar *test_env, const gchar *builtin)
{
#ifdef NIP55L_TEST_TRUST_ENV
  /* Test builds only (CMake NIP55L_TEST_TRUST_ENV, default = BUILD_TESTING;
   * distro packages configure BUILD_TESTING=OFF): lets CTest run build-tree
   * binaries as the bridge / approval UI against the real daemon. */
  const gchar *env = g_getenv(test_env);
  if (env && *env) return g_strsplit(env, ":", -1);
#else
  (void)test_env;
#endif
  return g_strsplit(builtin, ":", -1);
}

GStrv
signer_caller_origin_bridges(void)
{
  return path_list("NOSTR_SIGNER_TEST_ORIGIN_BRIDGES", NIP55L_ORIGIN_BRIDGE_PATHS);
}

GStrv
signer_caller_approvers(void)
{
  return path_list("NOSTR_SIGNER_TEST_APPROVERS", NIP55L_APPROVER_PATHS);
}

/* @c's executable is one of @paths by path and by device/inode, so a binary
 * replaced on disk, "(deleted)", or a path that only exists in another mount
 * namespace does not qualify. */
static gboolean
exe_in(const SignerCaller *c, GStrv paths)
{
  if (!c || !c->same_uid || !c->exe || c->exe_ino == 0 || !paths) return FALSE;
  for (guint i = 0; paths[i]; i++) {
    if (!*paths[i] || !g_path_is_absolute(paths[i]) || strcmp(paths[i], c->exe) != 0) continue;
    GStatBuf st;
    if (g_stat(paths[i], &st) == 0 &&
        (guint64)st.st_dev == c->exe_dev && (guint64)st.st_ino == c->exe_ino)
      return TRUE;
    g_message("nostr-signer: %s runs %s, but not the file installed there now (replaced by an "
              "upgrade?); restart it to be trusted", c->sender ? c->sender : "caller", paths[i]);
  }
  return FALSE;
}

gboolean
signer_caller_may_assert_origin(const SignerCaller *c)
{
  /* Only the two shapes the bridge has: its bare executable, or the
   * browser's app scope it inherited. Never Flatpak/Snap/unidentified. */
  if (!c || (c->kind != SIGNER_CALLER_EXE && c->kind != SIGNER_CALLER_SYSTEMD_SCOPE)) return FALSE;
  g_auto(GStrv) bridges = signer_caller_origin_bridges();
  return exe_in(c, bridges);
}

gboolean
signer_caller_is_approver(const SignerCaller *c)
{
  if (!c || !c->same_uid) return FALSE;
  /* No PID from the bus (macOS): the approval UI cannot be told apart. */
  if (c->unattested) return TRUE;
  if (c->kind == SIGNER_CALLER_FLATPAK) {
    static const gchar flatpak_id[] = NIP55L_APPROVER_FLATPAK_ID;
    return c->attested && flatpak_id[0] != '\0' && g_strcmp0(c->app_id, flatpak_id) == 0;
  }
  if (c->kind != SIGNER_CALLER_EXE && c->kind != SIGNER_CALLER_SYSTEMD_SCOPE) return FALSE;
  g_auto(GStrv) approvers = signer_caller_approvers();
  return exe_in(c, approvers);
}

SignerCaller *
signer_caller_for_origin(const SignerCaller *bridge, const gchar *origin)
{
  SignerCaller *n = signer_caller_copy(bridge);
  g_free(n->app_id);
  n->app_id = g_strdup(origin);
  g_free(n->via);
  n->via = g_strdup(bridge->principal);
  g_free(n->principal);
  n->principal = signer_caller_build_principal(SIGNER_CALLER_WEB_ORIGIN, origin, NULL);
  n->kind = SIGNER_CALLER_WEB_ORIGIN;
  n->attested = FALSE;
  return n;
}

SignerCaller *
signer_caller_for_claim(const SignerCaller *base, const gchar *app_id)
{
  SignerCaller *n = signer_caller_copy(base);
  g_free(n->app_id);
  n->app_id = g_strdup(app_id ? app_id : "");
  g_free(n->principal);
  n->principal = g_strconcat("claimed:", n->app_id, NULL);
  if (!principal_storable(n->principal)) g_clear_pointer(&n->principal, g_free);
  return n;
}

/* ---- resolution ---- */

#ifdef __linux__
static void
resolve_from_proc(guint32 pid, SignerCaller *c)
{
  g_autofree gchar *fp_path = g_strdup_printf("/proc/%u/root/.flatpak-info", pid);
  g_autofree gchar *fp = NULL;
  if (g_file_get_contents(fp_path, &fp, NULL, NULL)) {
    gchar *id = signer_caller_parse_flatpak_info(fp);
    if (id) {
      c->app_id = id;
      c->kind = SIGNER_CALLER_FLATPAK;
      c->attested = TRUE;
      return;
    }
  }
  g_autofree gchar *exe_path = g_strdup_printf("/proc/%u/exe", pid);
  GError *err = NULL;
  g_autofree gchar *exe = g_file_read_link(exe_path, &err);
  if (err) {
    static gboolean warned;
    if (!warned && (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_ACCES) ||
                    g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_PERM))) {
      warned = TRUE;
      g_warning("nostr-signer: cannot inspect caller processes (%s); every caller will be "
                "unidentified and approvals cannot be answered. Is the daemon running in a "
                "user namespace (systemd sandboxing)?", err->message);
    }
    g_clear_error(&err);
  }
  GStatBuf st;
  if (exe && g_path_is_absolute(exe) && g_stat(exe_path, &st) == 0) {
    c->exe = g_strdup(exe);
    c->exe_dev = (guint64)st.st_dev;
    c->exe_ino = (guint64)st.st_ino;
  }
  g_autofree gchar *cg_path = g_strdup_printf("/proc/%u/cgroup", pid);
  g_autofree gchar *cg = NULL;
  if (g_file_get_contents(cg_path, &cg, NULL, NULL)) {
    SignerCallerKind k = SIGNER_CALLER_UNKNOWN;
    gchar *id = signer_caller_parse_cgroup(cg, &k);
    if (id) {
      c->app_id = id;
      c->kind = k;
      return;
    }
  }
  if (c->exe) c->kind = SIGNER_CALLER_EXE;
}
#elif defined(__APPLE__)
static void
resolve_from_proc(guint32 pid, SignerCaller *c)
{
  char buf[PROC_PIDPATHINFO_MAXSIZE];
  if (proc_pidpath((int)pid, buf, sizeof buf) <= 0) return;
  GStatBuf st;
  if (!g_path_is_absolute(buf) || g_stat(buf, &st) != 0) return;
  c->exe = g_strdup(buf);
  c->exe_dev = (guint64)st.st_dev;
  c->exe_ino = (guint64)st.st_ino;
  c->kind = SIGNER_CALLER_EXE;
}
#else
static void
resolve_from_proc(guint32 pid, SignerCaller *c)
{
  (void)pid; (void)c;
}
#endif

/* Start time of @pid (0 = gone / unknown): a PID that keeps its start time
 * across a series of reads named one process throughout. */
static guint64
proc_start_time(guint32 pid)
{
#ifdef __linux__
  g_autofree gchar *path = g_strdup_printf("/proc/%u/stat", pid);
  g_autofree gchar *stat = NULL;
  if (!g_file_get_contents(path, &stat, NULL, NULL)) return 0;
  /* comm may contain spaces and ')': fields resume after the last ')'. */
  const gchar *p = strrchr(stat, ')');
  if (!p) return 0;
  g_auto(GStrv) f = g_strsplit(p + 2, " ", 0);
  /* f[0] is field 3 (state); starttime is field 22. */
  if (g_strv_length(f) < 20) return 0;
  return g_ascii_strtoull(f[19], NULL, 10);
#elif defined(__APPLE__)
  struct proc_bsdinfo bi;
  if (proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi) return 0;
  return (guint64)bi.pbi_start_tvsec * 1000000u + (guint64)bi.pbi_start_tvusec;
#else
  (void)pid;
  return 0;
#endif
}

static void
clear_identity(SignerCaller *c)
{
  g_clear_pointer(&c->app_id, g_free);
  g_clear_pointer(&c->exe, g_free);
  c->exe_dev = c->exe_ino = 0;
  c->kind = SIGNER_CALLER_UNKNOWN;
  c->attested = FALSE;
}

static SignerCaller *
identify(GDBusConnection *bus, const gchar *sender)
{
  SignerCaller *c = g_new0(SignerCaller, 1);
  c->sender = g_strdup(sender);
  if (!bus || !sender || !*sender) return c;

  GError *err = NULL;
  GUnixFDList *fds = NULL;
  g_autoptr(GVariant) reply = NULL;
#ifdef NIP55L_HAVE_GIO_UNIX
  reply = g_dbus_connection_call_with_unix_fd_list_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionCredentials", g_variant_new("(s)", sender), G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &fds, NULL, &err);
#else
  reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "GetConnectionCredentials", g_variant_new("(s)", sender), G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
#endif
  if (!reply) {
    g_debug("nostr-signer: cannot identify %s: %s", sender, err ? err->message : "?");
    g_clear_error(&err);
    return c;
  }
  gboolean have_uid = FALSE, have_pid = FALSE;
  guint32 uid = 0;
  gint pidfd = -1;
  g_autoptr(GVariant) dict = g_variant_get_child_value(reply, 0);
  GVariantIter it;
  const gchar *key;
  GVariant *val;
  g_variant_iter_init(&it, dict);
  while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
    if (g_str_equal(key, "UnixUserID") && g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32)) {
      uid = g_variant_get_uint32(val);
      have_uid = TRUE;
    } else if (g_str_equal(key, "ProcessID") && g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32)) {
      c->pid = g_variant_get_uint32(val);
      have_pid = TRUE;
    }
#ifdef NIP55L_HAVE_GIO_UNIX
    else if (g_str_equal(key, "ProcessFD") && g_variant_is_of_type(val, G_VARIANT_TYPE_HANDLE) && fds) {
      pidfd = g_unix_fd_list_get(fds, g_variant_get_handle(val), NULL);
    }
#endif
    g_variant_unref(val);
  }
  g_clear_object(&fds);
  c->same_uid = have_uid && uid == (guint32)getuid();
#ifndef __linux__
  if (c->same_uid && (!have_pid || c->pid == 0)) {
    static gboolean noted;
    if (!noted) {
      noted = TRUE;
      g_message("nostr-signer: the bus does not report caller PIDs on this platform; callers are "
                "identified by their claimed app_id only and any same-user process may answer "
                "approvals");
    }
    c->unattested = TRUE;
  }
#endif

  if (c->same_uid && have_pid && c->pid > 0) {
    resolve_from_proc(c->pid, c);
    gboolean alive = TRUE;
#ifdef __linux__
    if (pidfd >= 0) {
      /* pidfd: the identity holds iff the process outlived the reads */
      struct pollfd p = { .fd = pidfd, .events = POLLIN };
      alive = poll(&p, 1, 0) == 0;
    } else
#endif
    {
      /* No pidfd: if the sender still exists with the same PID, that
       * process was alive throughout the reads, so the PID was not reused. */
      g_autoptr(GVariant) again = g_dbus_connection_call_sync(
          bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
          "GetConnectionUnixProcessID", g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
      guint32 pid2 = 0;
      if (again) g_variant_get(again, "(u)", &pid2);
      alive = again && pid2 == c->pid;
    }
    if (!alive) {
      g_debug("nostr-signer: %s went away while being identified", sender);
      clear_identity(c);
    }
  }
  if (pidfd >= 0) close(pidfd);
  c->principal = signer_caller_build_principal(c->kind, c->app_id, c->exe);
  g_debug("nostr-signer: %s: uid=%s pid=%u kind=%s principal=%s", sender,
          have_uid ? (c->same_uid ? "same" : "other") : "unknown", c->pid,
          signer_caller_kind_to_string(c->kind), c->principal ? c->principal : "(none)");
  if (!c->principal && c->kind != SIGNER_CALLER_UNKNOWN)
    g_debug("nostr-signer: %s resolved (%s) but has no storable principal", sender,
            signer_caller_kind_to_string(c->kind));
  return c;
}

SignerCaller *
signer_caller_for_peer(const gchar *label, gboolean have_uid, guint32 uid, guint32 pid, gint pidfd)
{
  SignerCaller *c = g_new0(SignerCaller, 1);
  c->sender = g_strdup(label);
  c->pid = pid;
  c->same_uid = have_uid && uid == (guint32)getuid();
  if (c->same_uid && pid > 0) {
    guint64 started = proc_start_time(pid);
    resolve_from_proc(pid, c);
    gboolean alive;
#ifdef __linux__
    if (pidfd >= 0) {
      struct pollfd p = { .fd = pidfd, .events = POLLIN };
      alive = poll(&p, 1, 0) == 0;
    } else
#endif
    {
      (void)pidfd;
      alive = started != 0 && proc_start_time(pid) == started;
    }
    if (!alive) {
      g_debug("nostr-signer: %s (pid %u) went away while being identified", label, pid);
      clear_identity(c);
    }
  }
  c->principal = signer_caller_build_principal(c->kind, c->app_id, c->exe);
  g_debug("nostr-signer: %s: uid=%s pid=%u kind=%s principal=%s", label,
          have_uid ? (c->same_uid ? "same" : "other") : "unknown", pid,
          signer_caller_kind_to_string(c->kind), c->principal ? c->principal : "(none)");
  return c;
}

static GHashTable *cache; /* unique name -> SignerCaller* */

const SignerCaller *
signer_caller_lookup(GDBusConnection *bus, const gchar *sender)
{
  if (!cache)
    cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                  (GDestroyNotify)signer_caller_free);
  const gchar *key = sender ? sender : "";
  SignerCaller *c = g_hash_table_lookup(cache, key);
  if (c) return c;
  c = identify(bus, sender);
  /* Bounded: entries leave on NameOwnerChanged; this is a backstop. */
  if (g_hash_table_size(cache) >= 4096) g_hash_table_remove_all(cache);
  g_hash_table_insert(cache, g_strdup(key), c);
  return c;
}

SignerCaller *
signer_caller_identify_fresh(GDBusConnection *bus, const gchar *sender)
{
  return identify(bus, sender);
}

void
signer_caller_forget(const gchar *sender)
{
  if (cache && sender) g_hash_table_remove(cache, sender);
}
