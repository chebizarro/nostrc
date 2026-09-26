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

/* ---- pure helpers (exposed for tests) ---- */

/* [Application] name= from a .flatpak-info key file, or NULL. */
gchar *nwa_caller_parse_flatpak_info(const gchar *keyfile_data);

/* App id from /proc/<pid>/cgroup contents. Sets *kind to SNAP or
 * SYSTEMD_SCOPE on success; NULL if no app unit is found. */
gchar *nwa_caller_parse_cgroup(const gchar *cgroup_data, NwaCallerKind *kind);

G_END_DECLS

#endif /* NWA_CALLER_H */
