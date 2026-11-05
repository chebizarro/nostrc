#pragma once
#include "ui/approval_dialog.h"
#include <adwaita.h>

G_BEGIN_DECLS
#define GROTTO_TYPE_APPROVAL_REQUEST (grotto_approval_request_get_type())
G_DECLARE_FINAL_TYPE(GrottoApprovalRequest, grotto_approval_request, GROTTO, APPROVAL_REQUEST,
                     GObject)
typedef enum {
  GROTTO_APPROVAL_FETCHING,
  GROTTO_APPROVAL_READY,
  GROTTO_APPROVAL_SUBMITTING,
  GROTTO_APPROVAL_RESOLVED,
  GROTTO_APPROVAL_UNAVAILABLE
} GrottoApprovalState;
const char *grotto_approval_request_get_id(GrottoApprovalRequest *request);
GVariant *grotto_approval_request_get_info(GrottoApprovalRequest *request); /* borrowed */
GrottoApprovalState grotto_approval_request_get_state(GrottoApprovalRequest *request);

#define GROTTO_TYPE_APPROVAL_CONTROLLER (grotto_approval_controller_get_type())
G_DECLARE_FINAL_TYPE(GrottoApprovalController, grotto_approval_controller, GROTTO,
                     APPROVAL_CONTROLLER, GObject)
/* Create on the GUI main context. Subscribes before the first snapshot.
 * Application owns the controller and must stop it on shutdown. Signals:
 * message(string), availability-changed(), notifications-changed(),
 * reconciled(boolean success), including terminal unavailable/error results;
 * screen-lock-changed(boolean), only positively observed desktop lock state. */
GrottoApprovalController *grotto_approval_controller_new(GApplication *app, GDBusConnection *bus);
/* Optional host metadata lookup; returns an allocated local label for this exact
 * daemon-pinned npub. It never selects an identity or authorizes a request. */
typedef gchar *(*GrottoApprovalIdentityLabelFunc)(const gchar *npub);
/* Thread-safe, display-only host provenance for an exact daemon/request ID.
 * Does not modify the daemon snapshot, decision modes, deadlines or authority. */
void grotto_approval_controller_set_profile_provenance(GrottoApprovalController *self,
    const char *daemon_owner, const char *request_id, const char *const *destinations,
    const char *const *bootstrap_relays);
void grotto_approval_controller_set_identity_label_func(GrottoApprovalController *self,
    GrottoApprovalIdentityLabelFunc lookup);
GListModel *grotto_approval_controller_get_pending(GrottoApprovalController *self); /* borrowed */
void grotto_approval_controller_reconcile(GrottoApprovalController *self);
void grotto_approval_controller_stop(GrottoApprovalController *self);
guint grotto_approval_controller_get_holds(GrottoApprovalController *self);
gboolean grotto_approval_controller_notifications_available(GrottoApprovalController *self);
/* Empty ID reconciles the inbox without opening an arbitrary request. The
 * application's empty review-request action presents its hosted pending view.
 * A nonempty stale ID never substitutes a request. */
void grotto_approval_controller_review(GrottoApprovalController *self, const char *id);
void grotto_approval_controller_decide_async(GrottoApprovalController *self, const char *id,
                                             gboolean allow, gboolean remember, guint64 ttl,
                                             GCancellable *cancel, GAsyncReadyCallback callback,
                                             gpointer data);
void grotto_approval_controller_decide_flow_async(GrottoApprovalController *self, const char *id,
    gboolean encrypt, gboolean seals, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
gboolean grotto_approval_controller_decide_finish(GrottoApprovalController *self,
                                                  GAsyncResult *result, GError **error);
/* Reusable pending-request inbox group; only visible while requests are pending. */
GtkWidget *grotto_approval_pending_group_new(GrottoApprovalController *self);
G_END_DECLS
