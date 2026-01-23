/* sheet-delegation.h - NIP-26 delegation management dialog for gnostr-signer
 *
 * Provides UI for:
 * - Creating new NIP-26 delegation tokens
 * - Configuring delegation parameters (delegatee, kinds, time constraints)
 * - Viewing active delegations
 * - Revoking delegations
 * - Copying delegation tags for use in events
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_DELEGATION (sheet_delegation_get_type())
G_DECLARE_FINAL_TYPE(SheetDelegation, sheet_delegation, SHEET, DELEGATION, AdwDialog)

/**
 * sheet_delegation_new:
 *
 * Creates a new Delegation management dialog.
 *
 * Returns: (transfer full): a new #SheetDelegation
 */
SheetDelegation *sheet_delegation_new(void);

/**
 * sheet_delegation_set_account:
 * @self: the dialog instance
 * @npub: the npub of the delegator account
 *
 * Sets the account (npub) that will be the delegator.
 * This must be called before presenting the dialog.
 * The account must have private key access (not watch-only).
 */
void sheet_delegation_set_account(SheetDelegation *self, const gchar *npub);

/**
 * SheetDelegationChangedCb:
 * @npub: the delegator npub
 * @user_data: user data passed to the callback
 *
 * Callback type for when delegations are modified (created/revoked).
 * This can be used to refresh UI elsewhere in the app.
 */
typedef void (*SheetDelegationChangedCb)(const gchar *npub, gpointer user_data);

/**
 * sheet_delegation_set_on_changed:
 * @self: the dialog instance
 * @callback: function to call when delegations change
 * @user_data: data to pass to callback
 *
 * Sets a callback to be invoked when delegations are created or revoked.
 */
void sheet_delegation_set_on_changed(SheetDelegation *self,
                                      SheetDelegationChangedCb callback,
                                      gpointer user_data);

/**
 * sheet_delegation_refresh:
 * @self: the dialog instance
 *
 * Refresh the delegation list from storage.
 */
void sheet_delegation_refresh(SheetDelegation *self);

/**
 * sheet_delegation_show_create:
 * @self: the dialog instance
 *
 * Navigate directly to the create delegation page.
 */
void sheet_delegation_show_create(SheetDelegation *self);

G_END_DECLS
