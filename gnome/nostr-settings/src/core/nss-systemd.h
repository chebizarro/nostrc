/* nss-systemd.h — systemd --user unit control for the Relays and
 * Notifications pages (org.freedesktop.systemd1 on the SESSION bus; no
 * polkit involved for the user manager).
 * SPDX-License-Identifier: MIT
 *
 * The meaning of the switches is decided by pure functions below so it is
 * tested without a bus (tests/test_systemd.c also replays the plans
 * against a mock Manager):
 *
 *  - Session relay switch = the *persistent intent*: the socket unit's
 *    UnitFileState is enabled. The status row carries runtime truth
 *    (Off / Listening / Running / Starting / Failed / Enabled-but-stopped).
 *  - ON  = EnableUnitFiles([socket]) → Reload → StartUnit(socket).
 *  - OFF = DisableUnitFiles([socket]) → Reload → StopUnit(service) →
 *          StopUnit(socket). Disabling first means a client connecting in
 *          between cannot socket-activate the relay back after the user
 *          turned it off. A still-enabled nostr-notify.service (Wants= the
 *          relay) will start it again on its next start; the page says so.
 *  - Masked units: switch insensitive.
 */
#ifndef NSS_SYSTEMD_H
#define NSS_SYSTEMD_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSS_RELAY_SOCKET  "nostr-session-relay.socket"
#define NSS_RELAY_SERVICE "nostr-session-relay.service"
#define NSS_NOTIFY_SERVICE "nostr-notify.service"

typedef struct {
  gchar *load_state;       /* loaded | not-found | masked | error … */
  gchar *active_state;     /* active | inactive | activating | failed … */
  gchar *sub_state;        /* running | listening | auto-restart … */
  gchar *unit_file_state;  /* enabled | disabled | masked | static … ("" if unknown) */
} NssUnitState;

void nss_unit_state_clear(NssUnitState *s);

typedef enum {
  NSS_SVC_NOT_INSTALLED = 0,
  NSS_SVC_MASKED,
  NSS_SVC_OFF,               /* not enabled, not running */
  NSS_SVC_STOPPED,           /* enabled but not running/listening now */
  NSS_SVC_LISTENING,         /* socket up, daemon idle (starts on first connection) */
  NSS_SVC_STARTING,
  NSS_SVC_RUNNING,
  NSS_SVC_FAILED,            /* failed or crash-looping (auto-restart) */
} NssServiceStatus;

gboolean         nss_unit_file_enabled(const NssUnitState *s);
/* @socket may be NULL for plain services (nostr-notify). */
NssServiceStatus nss_service_status(const NssUnitState *socket, const NssUnitState *service);
const gchar     *nss_service_status_label(NssServiceStatus st, gboolean socket_activated);
gboolean         nss_service_can_restart(NssServiceStatus st);

typedef enum {
  NSS_OP_ENABLE, NSS_OP_DISABLE, NSS_OP_RELOAD, NSS_OP_START, NSS_OP_STOP, NSS_OP_RESTART,
} NssOpKind;

typedef struct {
  NssOpKind    kind;
  const gchar *unit;  /* static string; NULL for RELOAD */
} NssUnitOp;

/* Plans (static arrays, terminated by {.unit=NULL, .kind=-1}-free length). */
const NssUnitOp *nss_relay_plan(gboolean on, guint *n_ops);
const NssUnitOp *nss_notify_plan(gboolean on, guint *n_ops);

/* Read one unit's state (sync; bounded by 5 s per call). A unit systemd
 * does not know yields load_state "not-found" without an error. */
gboolean nss_systemd_get_state(GDBusConnection *bus, const gchar *unit,
                               NssUnitState *out, GError **error);

/* Run @ops in order on a worker thread; stops at the first failure. */
void     nss_systemd_run_async(GDBusConnection *bus, const NssUnitOp *ops, guint n_ops,
                               GCancellable *cancellable, GAsyncReadyCallback cb,
                               gpointer user_data);
gboolean nss_systemd_run_finish(GAsyncResult *res, GError **error);
/* Sync variant (tests, worker threads). */
gboolean nss_systemd_run(GDBusConnection *bus, const NssUnitOp *ops, guint n_ops,
                         GError **error);

G_END_DECLS

#endif /* NSS_SYSTEMD_H */
