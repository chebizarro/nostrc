#ifndef GH_NIP46_PAIR_DIALOG_H
#define GH_NIP46_PAIR_DIALOG_H

#include <adwaita.h>
#include "gh-account-controller.h"
#include "gh-nip46-session.h"

G_BEGIN_DECLS

#define GH_TYPE_NIP46_PAIR_DIALOG (gh_nip46_pair_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhNip46PairDialog, gh_nip46_pair_dialog, GH, NIP46_PAIR_DIALOG, AdwDialog)

/* The dialog owns an attempt session and its cancellable. The store may be
 * NULL to use the platform's default Secret Service or Keychain. Optional
 * transports are for deterministic UI tests. */
typedef const gchar *(*GhNip46PairNameFunc)(gpointer source, const gchar *pubkey_hex);
/* Deterministic test seam; production always uses GhNip46CredentialStore. */
typedef void (*GhNip46PairTestSaveFunc)(const GhNip46Credential *credential,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data,
                                        gpointer user_data);

typedef struct {
  GhAccountController *accounts;
  GSettings *settings;
  GhNip46CredentialStore *credentials;
  GhNip46PairNameFunc display_name; /* nullable cached kind-0 name */
  GObject *name_source;             /* required when display_name is set */
  const GhRelayTransport *scope_transport;
  const GhRelayAuthTransport *scope_auth;
  const GhRelayPublishTransport *publish_transport;
  const GhRelayPublishAuthTransport *publish_auth;
  gpointer transport_data;
  GhNip46PairTestSaveFunc test_save;
  gpointer test_save_data;
} GhNip46PairConfig;

/* Emits "confirmation-presented" with the inline confirmation page (a
 * GtkWidget holding the "Save Remote Signer" button) after the remote
 * account key is verified and before anything is saved or activated. Save
 * makes the account active on the live session at once
 * (gh_account_controller_adopt_remote()) and stores the credential alongside;
 * the dialog closes once the keyring confirms the write. */
GhNip46PairDialog *gh_nip46_pair_dialog_new(const GhNip46PairConfig *config);
/* For UI tests: never returns the sensitive URI or the transport secret. */
gboolean gh_nip46_pair_dialog_qr_is_visible(GhNip46PairDialog *self);

/* Test seam: read the status text. */
const gchar *gh_nip46_pair_dialog_get_status_for_test(GhNip46PairDialog *self);

G_END_DECLS
#endif
