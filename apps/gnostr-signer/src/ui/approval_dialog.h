/**
 * GnostrApprovalDialog - Modern AdwDialog-based approval dialog for event signing requests
 *
 * This header provides the public API for the approval dialog widget.
 */

#ifndef GNOSTR_APPROVAL_DIALOG_H
#define GNOSTR_APPROVAL_DIALOG_H

#include <adwaita.h>
#include <gtk/gtk.h>
#include "../accounts_store.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_APPROVAL_DIALOG (gnostr_approval_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrApprovalDialog, gnostr_approval_dialog, GNOSTR,
                     APPROVAL_DIALOG, AdwDialog)

/**
 * GnostrApprovalCallback:
 * @decision: TRUE if approved, FALSE if denied
 * @remember: TRUE if the decision should be persisted
 * @selected_identity: the selected identity npub (may be NULL)
 * @ttl_seconds: time-to-live for the policy (0 = forever or no TTL)
 * @user_data: user data passed to the callback
 *
 * Callback signature for approval dialog decisions.
 */
typedef void (*GnostrApprovalCallback)(gboolean decision, gboolean remember,
                                       const char *selected_identity,
                                       guint64 ttl_seconds, gpointer user_data);

/**
 * gnostr_approval_dialog_new:
 *
 * Creates a new approval dialog.
 *
 * Returns: (transfer full): a new #GnostrApprovalDialog
 */
GnostrApprovalDialog *gnostr_approval_dialog_new(void);

/**
 * gnostr_approval_dialog_set_event_type:
 * @self: a #GnostrApprovalDialog
 * @kind: the Nostr event kind number
 *
 * Sets the event type display based on the kind number.
 */
void gnostr_approval_dialog_set_event_type(GnostrApprovalDialog *self, int kind);

/**
 * gnostr_approval_dialog_set_app_name:
 * @self: a #GnostrApprovalDialog
 * @app_name: the requesting application name
 *
 * Sets the requesting application name.
 */
void gnostr_approval_dialog_set_app_name(GnostrApprovalDialog *self,
                                         const char *app_name);

/**
 * gnostr_approval_dialog_set_identity:
 * @self: a #GnostrApprovalDialog
 * @identity_npub: the identity npub string
 *
 * Sets the identity display.
 */
void gnostr_approval_dialog_set_identity(GnostrApprovalDialog *self,
                                         const char *identity_npub);

/**
 * gnostr_approval_dialog_set_timestamp:
 * @self: a #GnostrApprovalDialog
 * @timestamp: Unix timestamp, or 0 for current time
 *
 * Sets the timestamp display.
 */
void gnostr_approval_dialog_set_timestamp(GnostrApprovalDialog *self,
                                          guint64 timestamp);

/**
 * gnostr_approval_dialog_set_content:
 * @self: a #GnostrApprovalDialog
 * @content: the event content to preview
 *
 * Sets the content preview. Long content will be truncated with an expand option.
 */
void gnostr_approval_dialog_set_content(GnostrApprovalDialog *self,
                                        const char *content);

/**
 * gnostr_approval_dialog_set_accounts:
 * @self: a #GnostrApprovalDialog
 * @as: the accounts store
 * @selected_npub: the initially selected npub, or NULL
 *
 * Populates the identity dropdown with available accounts.
 */
void gnostr_approval_dialog_set_accounts(GnostrApprovalDialog *self,
                                         AccountsStore *as,
                                         const char *selected_npub);

/**
 * gnostr_approval_dialog_set_callback:
 * @self: a #GnostrApprovalDialog
 * @callback: the callback function
 * @user_data: user data for the callback
 *
 * Sets the callback to be invoked when the user makes a decision.
 */
void gnostr_approval_dialog_set_callback(GnostrApprovalDialog *self,
                                         GnostrApprovalCallback callback,
                                         gpointer user_data);

/**
 * gnostr_show_approval_dialog:
 * @parent: the parent widget
 * @identity_npub: the requesting identity npub
 * @app_name: the requesting application name
 * @preview: the event content preview
 * @as: the accounts store
 * @cb: callback for the decision
 * @user_data: user data for callback
 *
 * Convenience function to show an approval dialog.
 * This is the legacy API maintained for compatibility.
 */
void gnostr_show_approval_dialog(GtkWidget *parent, const char *identity_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as, GnostrApprovalCallback cb,
                                 gpointer user_data);

/**
 * gnostr_show_approval_dialog_full:
 * @parent: the parent widget
 * @identity_npub: the requesting identity npub
 * @app_name: the requesting application name
 * @content: the event content
 * @event_kind: the Nostr event kind
 * @timestamp: the event timestamp (0 for current time)
 * @as: the accounts store
 * @cb: callback for the decision
 * @user_data: user data for callback
 *
 * Full-featured approval dialog with all event metadata.
 */
void gnostr_show_approval_dialog_full(GtkWidget *parent,
                                      const char *identity_npub,
                                      const char *app_name, const char *content,
                                      int event_kind, guint64 timestamp,
                                      AccountsStore *as,
                                      GnostrApprovalCallback cb,
                                      gpointer user_data);

/**
 * gnostr_approval_dialog_set_client_pubkey:
 * @self: a #GnostrApprovalDialog
 * @client_pubkey: the client's public key (hex format)
 *
 * Sets the client public key for session management integration.
 * This allows the dialog to check for existing sessions and
 * create new sessions when "remember" is selected.
 */
void gnostr_approval_dialog_set_client_pubkey(GnostrApprovalDialog *self,
                                              const char *client_pubkey);

/**
 * gnostr_show_approval_dialog_with_session:
 * @parent: the parent widget
 * @client_pubkey: the client's public key (hex)
 * @identity_npub: the requesting identity npub
 * @app_name: the requesting application name
 * @content: the event content
 * @event_kind: the Nostr event kind
 * @timestamp: the event timestamp (0 for current time)
 * @as: the accounts store
 * @cb: callback for the decision
 * @user_data: user data for callback
 *
 * Shows approval dialog with session management integration.
 * If an active session exists for the client+identity, this may
 * auto-approve based on session state. The callback will include
 * session creation when "remember" is checked.
 *
 * Returns: %TRUE if dialog was shown, %FALSE if auto-approved by session
 */
gboolean gnostr_show_approval_dialog_with_session(GtkWidget *parent,
                                                  const char *client_pubkey,
                                                  const char *identity_npub,
                                                  const char *app_name,
                                                  const char *content,
                                                  int event_kind,
                                                  guint64 timestamp,
                                                  AccountsStore *as,
                                                  GnostrApprovalCallback cb,
                                                  gpointer user_data);

G_END_DECLS

#endif /* GNOSTR_APPROVAL_DIALOG_H */
