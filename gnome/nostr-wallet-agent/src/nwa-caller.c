/* nwa-caller.c - see nwa-caller.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-caller.h"
#include "nwa-error.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <poll.h>
#endif
#ifdef G_OS_UNIX
#include <gio/gunixfdlist.h>
#endif
#ifdef __linux__
#include <gio/gdesktopappinfo.h>
#endif

NwaCaller *
nwa_caller_new_self(void)
{
  NwaCaller *c = g_new0(NwaCaller, 1);
  c->sender = g_strdup("(self)");
  c->app_id = g_strdup("org.nostr.Wallet");
  c->display_name = g_strdup("Link opened on this computer");
  c->kind = NWA_CALLER_SELF;
  c->attested = TRUE;
  c->same_uid = TRUE;
  c->uid = (guint32)getuid();
  c->pid = (guint32)getpid();
  return c;
}

NwaCaller *
nwa_caller_copy(const NwaCaller *c)
{
  NwaCaller *n = g_new0(NwaCaller, 1);
  *n = *c;
  n->sender = g_strdup(c->sender);
  n->app_id = g_strdup(c->app_id);
  n->display_name = g_strdup(c->display_name);
  n->exe = g_strdup(c->exe);
  n->via = g_strdup(c->via);
  return n;
}

void
nwa_caller_free(NwaCaller *c)
{
  if (!c) return;
  g_free(c->sender);
  g_free(c->app_id);
  g_free(c->display_name);
  g_free(c->exe);
  g_free(c->via);
  g_free(c);
}

const gchar *
nwa_caller_kind_to_string(NwaCallerKind kind)
{
  switch (kind) {
    case NWA_CALLER_UNKNOWN:       return "unidentified";
    case NWA_CALLER_FLATPAK:       return "flatpak";
    case NWA_CALLER_SNAP:          return "snap";
    case NWA_CALLER_SYSTEMD_SCOPE: return "systemd-scope";
    case NWA_CALLER_EXE:           return "executable";
    case NWA_CALLER_SELF:          return "self";
    case NWA_CALLER_WEB_ORIGIN:    return "website";
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
nwa_caller_parse_flatpak_info(const gchar *keyfile_data)
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

/* Parse one unit name per the XDG/systemd application-unit convention:
 *   app[-<launcher>]-<ApplicationID>[-<RANDOM>].scope
 *   app[-<launcher>]-<ApplicationID>[@<RANDOM>].service
 * Because the launcher and random parts are optional, the app id is taken
 * as the *dotted* (reverse-DNS) component; units without one are ignored. */
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
nwa_caller_parse_cgroup(const gchar *cgroup_data, NwaCallerKind *kind)
{
  if (!cgroup_data) return NULL;
  g_auto(GStrv) lines = g_strsplit(cgroup_data, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    /* "hierarchy-id:controllers:path"; cgroup v2 is "0::/path" */
    const gchar *path = strchr(lines[i], ':');
    if (!path || !(path = strchr(path + 1, ':'))) continue;
    path++;
    g_auto(GStrv) comps = g_strsplit(path, "/", -1);
    /* deepest component wins: it is the unit the process actually runs in */
    for (gint j = (gint)g_strv_length(comps) - 1; j >= 0; j--) {
      const gchar *c = comps[j];
      if (g_str_has_prefix(c, "snap.")) {
        /* "snap.<name>.<app>..." */
        g_auto(GStrv) sp = g_strsplit(c, ".", 3);
        if (sp[0] && sp[1] && sp[2] && *sp[1] && is_valid_app_id(sp[1])) {
          if (kind) *kind = NWA_CALLER_SNAP;
          return g_strconcat("snap.", sp[1], NULL);
        }
      }
      gchar *id = app_id_from_unit(c);
      if (id) {
        if (kind) *kind = NWA_CALLER_SYSTEMD_SCOPE;
        return id;
      }
    }
  }
  return NULL;
}

/* ---- web origins ---- */

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
nwa_caller_is_web_origin(const gchar *origin)
{
  if (!origin) return FALSE;
  gsize len = strlen(origin);
  if (len > 512) return FALSE;
  gboolean https = g_str_has_prefix(origin, "https://");
  if (!https && !g_str_has_prefix(origin, "http://")) return FALSE;
  const gchar *host = origin + (https ? 8 : 7);
  const gchar *end = origin + len;

  /* Split an optional ":port" (the last ':' after any IPv6 literal). */
  const gchar *hend = end;
  const gchar *colon = NULL;
  const gchar *search = host;
  if (*host == '[') {
    search = strchr(host, ']');
    if (!search) return FALSE;
  }
  colon = strchr(search, ':');
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
    if (v == (https ? 443u : 80u)) return FALSE; /* browsers omit the default port */
  }
  gsize hlen = (gsize)(hend - host);
  gboolean host_ok = is_dns_host(host, hlen) || is_ipv6_literal(host, hlen);
  if (!host_ok) return FALSE;
  return https || is_loopback_host(host, hlen);
}

GStrv
nwa_caller_origin_bridges(void)
{
#ifdef NWA_ORIGIN_BRIDGE_ENV
  /* Test builds only (CMake NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV, default =
   * BUILD_TESTING; packages build with it off): lets CTest run the build-tree
   * bridge against the real agent. */
  const gchar *env = g_getenv("NOSTR_WALLET_AGENT_ORIGIN_BRIDGES");
  if (env && *env) {
    g_message("nostr-wallet-agent: NOSTR_WALLET_AGENT_ORIGIN_BRIDGES overrides the browser bridge "
              "path (test build): %s", env);
    return g_strsplit(env, ":", -1);
  }
#endif
#ifdef NWA_WEBEXT_HOST_PATH
  return g_strdupv((gchar *[]){ (gchar *)NWA_WEBEXT_HOST_PATH, NULL });
#else
  return g_new0(gchar *, 1);
#endif
}

GStrv
nwa_caller_settings_apps(void)
{
#ifdef NWA_ORIGIN_BRIDGE_ENV
  /* Test builds only, like NOSTR_WALLET_AGENT_ORIGIN_BRIDGES. */
  const gchar *env = g_getenv("NOSTR_WALLET_AGENT_SETTINGS_APPS");
  if (env && *env) {
    g_message("nostr-wallet-agent: NOSTR_WALLET_AGENT_SETTINGS_APPS overrides the settings app "
              "path (test build): %s", env);
    return g_strsplit(env, ":", -1);
  }
#endif
#ifdef NWA_SETTINGS_PATH
  return g_strdupv((gchar *[]){ (gchar *)NWA_SETTINGS_PATH, NULL });
#else
  return g_new0(gchar *, 1);
#endif
}

gboolean
nwa_caller_exe_is(const NwaCaller *c, const gchar *const *paths)
{
  if (!c || !c->same_uid || !c->exe || c->exe_ino == 0 || !paths) return FALSE;
  /* The dev/inode match is the credential; the kind is a pre-filter. Only
   * the two shapes an unsandboxed program has: its bare executable, or the
   * app scope it was launched (or inherited) in. Sandboxed (Flatpak/Snap —
   * also when they exec a host binary: the child keeps the sandbox's
   * identity), synthetic and partially resolved callers never qualify. */
  if (c->kind != NWA_CALLER_EXE && c->kind != NWA_CALLER_SYSTEMD_SCOPE) return FALSE;
  for (guint i = 0; paths[i]; i++) {
    if (!g_path_is_absolute(paths[i]) || strcmp(paths[i], c->exe) != 0) continue;
    GStatBuf st;
    if (g_stat(paths[i], &st) == 0 &&
        (guint64)st.st_dev == c->exe_dev && (guint64)st.st_ino == c->exe_ino)
      return TRUE;
  }
  return FALSE;
}

gboolean
nwa_caller_may_assert_origin(const NwaCaller *c, const gchar *const *bridges)
{
  return nwa_caller_exe_is(c, bridges);
}

NwaCaller *
nwa_caller_for_origin(const NwaCaller *bridge, const gchar *origin)
{
  NwaCaller *n = nwa_caller_copy(bridge);
  g_free(n->app_id);
  n->app_id = g_strdup(origin);
  g_free(n->via);
  /* Name the browser when the bridge ran in an identified app scope; a
   * bare "exe:" identity is just the bridge binary itself. */
  n->via = (bridge->app_id && bridge->kind != NWA_CALLER_EXE) ? g_strdup(bridge->display_name) : NULL;
  g_free(n->display_name);
  n->display_name = g_strdup(strstr(origin, "://") ? strstr(origin, "://") + 3 : origin);
  n->kind = NWA_CALLER_WEB_ORIGIN;
  n->attested = FALSE;
  return n;
}

/* ---- async identification ---- */

typedef struct {
  GDBusConnection *bus;
  gchar     *sender;
  guint32    uid;
  guint32    pid;
  gint       pidfd;
  gboolean   have_pid;
  gboolean   have_uid;
  NwaCaller *caller;   /* built after the first credentials call */
} IdentifyData;

static gchar *display_name_for(const gchar *app_id, NwaCallerKind kind);

static void
identify_data_free(IdentifyData *d)
{
  if (d->pidfd >= 0) close(d->pidfd);
  g_clear_object(&d->bus);
  nwa_caller_free(d->caller);
  g_free(d->sender);
  g_free(d);
}

static void
identify_finish_task(GTask *task, IdentifyData *d)
{
  NwaCaller *c = g_steal_pointer(&d->caller);
  c->display_name = display_name_for(c->app_id, c->kind);
  g_task_return_pointer(task, c, (GDestroyNotify)nwa_caller_free);
  g_object_unref(task);
}

static gchar *
display_name_for(const gchar *app_id, NwaCallerKind kind)
{
#ifdef __linux__
  if (app_id && kind != NWA_CALLER_EXE) {
    g_autofree gchar *desktop = g_strconcat(app_id, ".desktop", NULL);
    g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(desktop);
    if (info) {
      const gchar *name = g_app_info_get_display_name(G_APP_INFO(info));
      if (name && *name) return g_strdup(name);
    }
  }
#endif
  if (app_id && kind == NWA_CALLER_EXE)
    return g_path_get_basename(app_id + strlen("exe:"));
  if (app_id) return g_strdup(app_id);
  return g_strdup("Unidentified application");
}

#ifdef __linux__
static gboolean
pidfd_alive(gint pidfd)
{
  if (pidfd < 0) return TRUE; /* no pidfd: best effort */
  struct pollfd p = { .fd = pidfd, .events = POLLIN };
  return poll(&p, 1, 0) == 0; /* readable => process exited */
}

static void
resolve_from_proc(guint32 pid, gchar **out_id, NwaCallerKind *out_kind, gboolean *out_attested,
                  gchar **out_exe, guint64 *out_dev, guint64 *out_ino)
{
  g_autofree gchar *fp_path = g_strdup_printf("/proc/%u/root/.flatpak-info", pid);
  g_autofree gchar *fp = NULL;
  if (g_file_get_contents(fp_path, &fp, NULL, NULL)) {
    gchar *id = nwa_caller_parse_flatpak_info(fp);
    if (id) {
      *out_id = id;
      *out_kind = NWA_CALLER_FLATPAK;
      *out_attested = TRUE;
      return;
    }
  }
  /* Executable of every unsandboxed caller (also recorded when a cgroup
   * names the app): nwa_caller_may_assert_origin() checks it. stat()
   * follows the magic link to the file actually executing. */
  g_autofree gchar *exe_path = g_strdup_printf("/proc/%u/exe", pid);
  GError *err = NULL;
  g_autofree gchar *exe = g_file_read_link(exe_path, &err);
  if (err) {
    static gboolean warned;
    if (!warned && (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_ACCES) ||
                    g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_PERM))) {
      warned = TRUE;
      g_warning("nostr-wallet-agent: cannot inspect caller processes (%s); every caller will be "
                "unidentified. Is the agent running in a user namespace (systemd sandboxing)?",
                err->message);
    }
    g_clear_error(&err);
  }
  GStatBuf st;
  if (exe && g_path_is_absolute(exe) && g_stat(exe_path, &st) == 0) {
    *out_exe = g_strdup(exe);
    *out_dev = (guint64)st.st_dev;
    *out_ino = (guint64)st.st_ino;
  }

  g_autofree gchar *cg_path = g_strdup_printf("/proc/%u/cgroup", pid);
  g_autofree gchar *cg = NULL;
  if (g_file_get_contents(cg_path, &cg, NULL, NULL)) {
    NwaCallerKind k = NWA_CALLER_UNKNOWN;
    gchar *id = nwa_caller_parse_cgroup(cg, &k);
    if (id) {
      *out_id = id;
      *out_kind = k;
      /* cgroup names (snap.* included) can be chosen by any unsandboxed
       * process of the same user via systemd-run; only Flatpak's
       * .flatpak-info is set by the sandbox itself. */
      *out_attested = FALSE;
      return;
    }
  }
  if (exe && g_path_is_absolute(exe)) {
    /* " (deleted)" suffix means the binary was replaced; keep it visible */
    *out_id = g_strconcat("exe:", exe, NULL);
    *out_kind = NWA_CALLER_EXE;
    *out_attested = FALSE;
  }
}
#endif

#ifdef __linux__
static void
on_recheck(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  IdentifyData *d = g_task_get_task_data(task);
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);
  guint32 pid = 0;
  if (reply) g_variant_get(reply, "(u)", &pid);
  if (!reply || pid != d->pid) {
    g_debug("nostr-wallet-agent: %s went away while being identified; treating as unidentified",
            d->sender);
    g_clear_pointer(&d->caller->app_id, g_free);
    g_clear_pointer(&d->caller->exe, g_free);
    d->caller->exe_dev = d->caller->exe_ino = 0;
    d->caller->kind = NWA_CALLER_UNKNOWN;
    d->caller->attested = FALSE;
  }
  identify_finish_task(task, d);
}
#endif

static void
on_credentials(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  IdentifyData *d = g_task_get_task_data(task);
  GError *err = NULL;
  GUnixFDList *fds = NULL;
  g_autoptr(GVariant) reply =
    g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), &fds, res, &err);
  if (!reply) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_DENIED,
                            "cannot identify caller: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    g_object_unref(task);
    return;
  }

  g_autoptr(GVariant) dict = g_variant_get_child_value(reply, 0);
  GVariantIter it;
  const gchar *key;
  GVariant *val;
  g_variant_iter_init(&it, dict);
  while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
    if (g_str_equal(key, "UnixUserID") && g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32)) {
      d->uid = g_variant_get_uint32(val);
      d->have_uid = TRUE;
    } else if (g_str_equal(key, "ProcessID") && g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32)) {
      d->pid = g_variant_get_uint32(val);
      d->have_pid = TRUE;
    }
#ifdef G_OS_UNIX
    else if (g_str_equal(key, "ProcessFD") && g_variant_is_of_type(val, G_VARIANT_TYPE_HANDLE) && fds) {
      gint idx = g_variant_get_handle(val);
      d->pidfd = g_unix_fd_list_get(fds, idx, NULL);
    }
#endif
    g_variant_unref(val);
  }
  g_clear_object(&fds);

  NwaCaller *c = g_new0(NwaCaller, 1);
  d->caller = c;
  c->sender = g_strdup(d->sender);
  c->uid = d->uid;
  c->pid = d->pid;
  c->same_uid = d->have_uid && d->uid == (guint32)getuid();
  c->kind = NWA_CALLER_UNKNOWN;

#ifdef __linux__
  if (c->same_uid && d->have_pid && d->pid > 0) {
    gchar *id = NULL;
    NwaCallerKind kind = NWA_CALLER_UNKNOWN;
    gboolean attested = FALSE;
    gchar *exe = NULL;
    guint64 exe_dev = 0, exe_ino = 0;
    resolve_from_proc(d->pid, &id, &kind, &attested, &exe, &exe_dev, &exe_ino);
    if (id && d->pidfd >= 0) {
      /* pidfd: the identity holds iff the process outlived the reads */
      if (pidfd_alive(d->pidfd)) {
        c->app_id = id;
        c->kind = kind;
        c->attested = attested;
        c->exe = exe;
        c->exe_dev = exe_dev;
        c->exe_ino = exe_ino;
      } else {
        g_free(id);
        g_free(exe);
      }
    } else if (id) {
      /* No pidfd (dbus-daemon < 1.15): ask the bus again. If the sender
       * still exists with the same PID, that process was alive throughout
       * the /proc reads, so its PID cannot have been recycled. */
      c->app_id = id;
      c->kind = kind;
      c->attested = attested;
      c->exe = exe;
      c->exe_dev = exe_dev;
      c->exe_ino = exe_ino;
      g_dbus_connection_call(d->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                             "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                             g_variant_new("(s)", d->sender), G_VARIANT_TYPE("(u)"),
                             G_DBUS_CALL_FLAGS_NONE, 5000, g_task_get_cancellable(task),
                             on_recheck, task);
      return;
    } else {
      g_free(exe);
    }
  }
#endif
  identify_finish_task(task, d);
}

void
nwa_caller_identify_async(GDBusConnection *bus, const gchar *sender,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  IdentifyData *d = g_new0(IdentifyData, 1);
  d->bus = g_object_ref(bus);
  d->sender = g_strdup(sender);
  d->pidfd = -1;
  g_task_set_task_data(task, d, (GDestroyNotify)identify_data_free);

  if (!sender || !*sender) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_DENIED, "no sender");
    g_object_unref(task);
    return;
  }
  g_dbus_connection_call_with_unix_fd_list(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                           "org.freedesktop.DBus", "GetConnectionCredentials",
                                           g_variant_new("(s)", sender),
                                           G_VARIANT_TYPE("(a{sv})"),
                                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                           cancellable, on_credentials, task);
}

NwaCaller *
nwa_caller_identify_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}
