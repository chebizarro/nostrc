/* signer_caller.h - who is calling org.nostr.Signer (nostrc-phk4, nostrc-1e31)
 *
 * A caller's principal is derived from what the bus daemon attests about the
 * connection (org.freedesktop.DBus.GetConnectionCredentials: UnixUserID,
 * ProcessID and, where the bus supports it, ProcessFD) plus /proc (Linux) or
 * proc_pidpath (macOS). It is never taken from an argument. Mirrors
 * gnome/nostr-wallet-agent/src/nwa-caller.c; the differences are noted inline.
 *
 * Principal strings (the ACL key, and ApprovalRequested's app_id):
 *
 *   flatpak:<app-id>            /proc/<pid>/root/.flatpak-info (attested)
 *   app:<app-id>;exe:<path>     systemd app scope/service + executable
 *   snap:<name>;exe:<path>      snap cgroup + executable
 *   exe:<path>                  executable only
 *   <web origin>                "https://site" asserted by the browser bridge
 *
 * Unlike the wallet agent (scope id wins over the executable) a scoped caller
 * is keyed on both: every program started from a terminal emulator inherits
 * the terminal's app scope, and every GJS/Python app shares its interpreter's
 * executable, so either part alone would let one program inherit another's
 * grants. Launching the same program differently only costs a new prompt.
 *
 * NULL principal = unidentified (other uid, /proc unreadable, process gone,
 * or a path that cannot be stored as an ACL key). Unidentified callers can
 * still be prompted, but nothing is ever remembered for them.
 *
 * Platforms whose bus cannot attest a PID (macOS: dbus-daemon reports only
 * UnixUserID for clients) mark same-user callers `unattested`: their
 * principal is "claimed:<app_id argument>" - the pre-0.4.0 model, labelled as
 * unverified - and any same-user process may answer approvals. Never set on
 * Linux, where a missing PID leaves the caller unidentified.
 *
 * Trust: only an unsandboxed same-user process can hold an exe/app principal,
 * and such a process can also edit the ACL file, so for those the ACL is a
 * guard against mistakes and confused deputies, not a security boundary. The
 * boundary is against sandboxed (Flatpak) apps and against a local process
 * claiming another app's or a website's identity over the bus. Consequences
 * accepted under that model: an unsandboxed same-user process can run a
 * trusted binary itself (e.g. drive the browser bridge's stdin to speak for
 * any origin, or inject code with LD_PRELOAD) and so inherit its grants;
 * without a pidfd (dbus-daemon < 1.15) a process that hands its bus socket to
 * another and exits could, after PID reuse, be read as a different process.
 * Only Flatpak-attested principals resist both.
 */
#ifndef NIP55L_SIGNER_CALLER_H
#define NIP55L_SIGNER_CALLER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  SIGNER_CALLER_UNKNOWN = 0,
  SIGNER_CALLER_FLATPAK,
  SIGNER_CALLER_SNAP,
  SIGNER_CALLER_SYSTEMD_SCOPE,
  SIGNER_CALLER_EXE,
  SIGNER_CALLER_WEB_ORIGIN,
} SignerCallerKind;

typedef struct {
  gchar            *sender;     /* unique bus name */
  gchar            *principal;  /* ACL key part; NULL = unidentified */
  gchar            *app_id;     /* flatpak/scope/snap id, or origin; nullable */
  SignerCallerKind  kind;
  gboolean          attested;   /* identity set by a sandbox (Flatpak) */
  gboolean          same_uid;
  guint32           pid;
  gchar            *exe;        /* unsandboxed same-uid callers; nullable */
  guint64           exe_dev;
  guint64           exe_ino;
  gchar            *via;        /* WEB_ORIGIN: the bridge's principal */
  gboolean          unattested; /* same uid, but the bus cannot attest the process */
} SignerCaller;

void          signer_caller_free(SignerCaller *c);
SignerCaller *signer_caller_copy(const SignerCaller *c);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignerCaller, signer_caller_free)

const gchar  *signer_caller_kind_to_string(SignerCallerKind kind);

/* Identify @sender on @bus. Blocking (one or two bus round trips, then
 * /proc); results are cached per unique name, which the bus never reuses.
 * Never NULL: an unidentified caller has principal == NULL. Borrowed. */
const SignerCaller *signer_caller_lookup(GDBusConnection *bus, const gchar *sender);
/* Identify again, bypassing the cache (the process may have exec'd since
 * it was first seen). For privileged checks such as approvals. */
SignerCaller *signer_caller_identify_fresh(GDBusConnection *bus, const gchar *sender);
/* Drop the cache entry (call when the unique name leaves the bus). */
void          signer_caller_forget(const gchar *sender);

/* TRUE iff @c may name the web origin it acts for: same uid, EXE or
 * SYSTEMD_SCOPE kind, and its executable is an installed browser bridge by
 * path AND device/inode (see signer_caller_origin_bridges). */
gboolean      signer_caller_may_assert_origin(const SignerCaller *c);
/* TRUE iff @c may answer ApproveRequest / read GetApprovalInfo: its
 * executable is an installed approval UI (path AND device/inode), or it is
 * the attested Flatpak app of that UI. */
gboolean      signer_caller_is_approver(const SignerCaller *c);
/* New WEB_ORIGIN principal for @origin acting through @bridge. */
SignerCaller *signer_caller_for_origin(const SignerCaller *bridge, const gchar *origin);
/* Unattested caller (see above): copy whose principal is "claimed:<app_id>"
 * (NULL if @app_id cannot be stored). */
SignerCaller *signer_caller_for_claim(const SignerCaller *base, const gchar *app_id);

/* "https://<host>[:port]" or "http://" + localhost / *.localhost /
 * 127.0.0.1 / [::1]: lowercase, no default port, userinfo, path, query or
 * fragment (browser-serialized secure-context origin). Same rule as
 * nwa_caller_is_web_origin. */
gboolean      signer_caller_is_web_origin(const gchar *origin);

/* Executables allowed to assert web origins / approve requests: build-time
 * NIP55L_ORIGIN_BRIDGE_PATHS / NIP55L_APPROVER_PATHS (colon-separated). Test
 * builds (NIP55L_TEST_TRUST_ENV) take NOSTR_SIGNER_TEST_ORIGIN_BRIDGES /
 * NOSTR_SIGNER_TEST_APPROVERS instead when set. */
GStrv         signer_caller_origin_bridges(void);
GStrv         signer_caller_approvers(void);

/* ---- pure helpers (exposed for tests) ---- */
gchar        *signer_caller_parse_flatpak_info(const gchar *keyfile_data);
gchar        *signer_caller_parse_cgroup(const gchar *cgroup_data, SignerCallerKind *kind);
/* Principal for the resolved parts, or NULL if it cannot be an ACL key
 * (empty, control characters, '=', '[', ']', '|', surrounding blanks). */
gchar        *signer_caller_build_principal(SignerCallerKind kind, const gchar *app_id,
                                            const gchar *exe);

G_END_DECLS

#endif /* NIP55L_SIGNER_CALLER_H */
