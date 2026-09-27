/* nwa-caller.h - identify the application behind a D-Bus caller
 *
 * SPDX-License-Identifier: MIT
 *
 * The caller's identity is derived only from what the bus daemon attests
 * (org.freedesktop.DBus.GetConnectionCredentials: UnixUserID, ProcessID and,
 * where the bus supports it, ProcessFD) plus /proc — never from a string the
 * caller passes. Resolution order:
 *
 *   1. Flatpak   /proc/<pid>/root/.flatpak-info [Application] name   (attested)
 *   2. Snap      cgroup "snap.<name>.<app>..."                      (unverified)
 *   3. systemd   cgroup "app-[<launcher>-]<app-id>[-<rnd>].scope" or
 *                "app-[<launcher>-]<app-id>[@<rnd>].service"      (unverified)
 *   4. exe       "exe:" + readlink(/proc/<pid>/exe)                   (unverified)
 *
 * "Attested" identities are set by the sandbox and cannot be chosen by the
 * sandboxed app. For Flatpak the bus peer is the app's xdg-dbus-proxy,
 * whose root carries the app's .flatpak-info, so the lookup is the same one
 * xdg-desktop-portal performs. Unverified identities are only as trustworthy as the
 * caller's own user account: an unsandboxed process running as the same
 * user can pick its systemd scope name or read the keyring directly, so for
 * those callers budgets are a guard against mistakes, not a security
 * boundary (see README "Security model").
 *
 * PID reuse: when the bus returns a ProcessFD (pidfd), the identity is only
 * accepted if the process is still alive after /proc was read; otherwise the
 * identity falls back to "unidentified".
 *
 * Web origins (org.nostr.Wallet1 *For methods): the browser bridge
 * nostr-signer-webext-host inherits the browser's cgroup, so every site
 * would share the browser's identity and budget. A caller whose executable
 * is the installed bridge (compared by path AND device/inode of
 * /proc/<pid>/exe, never for Flatpak/Snap callers) may name the page origin
 * it acts for; the call then runs as a NWA_CALLER_WEB_ORIGIN principal whose
 * app id is the origin itself. See README "Web origins".
 */
#ifndef NWA_CALLER_H
#define NWA_CALLER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  NWA_CALLER_UNKNOWN,
  NWA_CALLER_FLATPAK,
  NWA_CALLER_SNAP,
  NWA_CALLER_SYSTEMD_SCOPE,
  NWA_CALLER_EXE,
  NWA_CALLER_SELF,          /* the agent's own scheme-handler flow */
  NWA_CALLER_WEB_ORIGIN,    /* a web origin named by the trusted browser bridge */
} NwaCallerKind;

typedef struct {
  gchar         *sender;       /* unique bus name */
  gchar         *app_id;       /* NULL when unidentified */
  gchar         *display_name; /* human-readable, never NULL */
  NwaCallerKind  kind;
  gboolean       attested;     /* identity set by a sandbox */
  gboolean       same_uid;
  guint32        uid;
  guint32        pid;
  /* readlink(/proc/<pid>/exe) and the dev/inode it resolves to, for
   * unsandboxed same-user callers; NULL / 0 otherwise. */
  gchar         *exe;
  guint64        exe_dev;
  guint64        exe_ino;
  gchar         *via;          /* WEB_ORIGIN: the bridge's display name (nullable) */
} NwaCaller;

NwaCaller *nwa_caller_new_self(void);
NwaCaller *nwa_caller_copy(const NwaCaller *c);
void       nwa_caller_free(NwaCaller *c);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NwaCaller, nwa_caller_free)

const gchar *nwa_caller_kind_to_string(NwaCallerKind kind);

void       nwa_caller_identify_async(GDBusConnection *bus, const gchar *sender,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
NwaCaller *nwa_caller_identify_finish(GAsyncResult *result, GError **error);

/* ---- web origins asserted by the browser bridge ---- */

/* TRUE iff @origin is a browser-serialized secure-context origin:
 * "https://<host>[:port]" or "http://" + localhost / *.localhost /
 * 127.0.0.1 / [::1], lowercase ASCII, no default port, no userinfo, path,
 * query or fragment. Such ids contain "://" and so can never collide with
 * a reverse-DNS, "snap." or "exe:" application id. */
gboolean   nwa_caller_is_web_origin(const gchar *origin);

/* Absolute paths of executables allowed to assert a web origin: the
 * build-time NWA_WEBEXT_HOST_PATH (<libexecdir>/nostr-signer-webext-host).
 * Test builds (NWA_ORIGIN_BRIDGE_ENV) honour NOSTR_WALLET_AGENT_ORIGIN_BRIDGES
 * (colon-separated) instead. */
GStrv      nwa_caller_origin_bridges(void);

/* TRUE iff @c is an unsandboxed same-uid process (kind EXE or SYSTEMD_SCOPE;
 * never Flatpak/Snap/self/web origin/unidentified) whose /proc/<pid>/exe is
 * one of @paths by path AND by dev/inode (so a hard link or copy elsewhere,
 * a binary replaced on disk, "(deleted)", or a path that only exists inside
 * another mount namespace does not qualify). The shared rule behind the
 * browser bridge and the settings app. */
gboolean   nwa_caller_exe_is(const NwaCaller *c, const gchar *const *paths);

/* TRUE iff @c may act for a web origin: nwa_caller_exe_is(@c, @bridges). */
gboolean   nwa_caller_may_assert_origin(const NwaCaller *c, const gchar *const *bridges);

/* Absolute paths of the settings application(s) trusted to manage other
 * applications' grants and budgets: the build-time NWA_SETTINGS_PATH
 * (<bindir>/nostr-settings). Test builds (NWA_ORIGIN_BRIDGE_ENV) honour
 * NOSTR_WALLET_AGENT_SETTINGS_APPS (colon-separated) instead. */
GStrv      nwa_caller_settings_apps(void);

/* New WEB_ORIGIN principal for @origin, acting through @bridge. */
NwaCaller *nwa_caller_for_origin(const NwaCaller *bridge, const gchar *origin);

/* ---- pure helpers (exposed for tests) ---- */

/* [Application] name= from a .flatpak-info key file, or NULL. */
gchar *nwa_caller_parse_flatpak_info(const gchar *keyfile_data);

/* App id from /proc/<pid>/cgroup contents. Sets *kind to SNAP or
 * SYSTEMD_SCOPE on success; NULL if no app unit is found. */
gchar *nwa_caller_parse_cgroup(const gchar *cgroup_data, NwaCallerKind *kind);

G_END_DECLS

#endif /* NWA_CALLER_H */
