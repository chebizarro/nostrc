/* sheet-create-account.h - Account creation wizard dialog
 *
 * Multi-step wizard for creating a new Nostr identity with:
 * - Step 1: Enter display name (optional)
 * - Step 2: Create password with strength indicator
 * - Step 3: Show generated BIP-39 seed phrase
 * - Step 4: Verify seed phrase (user enters random words)
 * - Step 5: Success - show npub with copy option
 *
 * Uses AdwDialog with AdwNavigationView for step navigation.
 * Integrates with secret_store for secure key storage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_CREATE_ACCOUNT (sheet_create_account_get_type())
G_DECLARE_FINAL_TYPE(SheetCreateAccount, sheet_create_account, SHEET, CREATE_ACCOUNT, AdwDialog)

/**
 * SheetCreateAccountCallback:
 * @npub: the public key of the created identity
 * @user_data: user data passed to the callback
 *
 * Callback type for successful account creation.
 */
typedef void (*SheetCreateAccountCallback)(const gchar *npub, gpointer user_data);

/**
 * sheet_create_account_new:
 *
 * Creates a new account creation wizard dialog.
 *
 * Returns: (transfer full): a new #SheetCreateAccount
 */
SheetCreateAccount *sheet_create_account_new(void);

/**
 * sheet_create_account_set_on_created:
 * @self: the dialog instance
 * @callback: function to call on successful account creation
 * @user_data: data to pass to callback
 *
 * Sets a callback to be invoked when an account is successfully created.
 */
void sheet_create_account_set_on_created(SheetCreateAccount *self,
                                          SheetCreateAccountCallback callback,
                                          gpointer user_data);

/**
 * sheet_create_account_set_word_count:
 * @self: the dialog instance
 * @word_count: number of words for mnemonic (12 or 24)
 *
 * Sets the number of words for the generated seed phrase.
 * Default is 12 words.
 */
void sheet_create_account_set_word_count(SheetCreateAccount *self, gint word_count);

G_END_DECLS
