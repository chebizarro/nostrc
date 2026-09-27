#ifndef GH_SIGNER_H
#define GH_SIGNER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GhSigner GhSigner;
typedef enum {
  GH_SIGNER_ERROR_INVALID_INPUT = 1,
  GH_SIGNER_ERROR_UNAVAILABLE,
  GH_SIGNER_ERROR_DENIED,
  GH_SIGNER_ERROR_TIMED_OUT,
  GH_SIGNER_ERROR_CANCELLED,
  GH_SIGNER_ERROR_KEY_MISMATCH,
  GH_SIGNER_ERROR_INVALID_RESULT,
  GH_SIGNER_ERROR_NO_APPROVER
} GhSignerError;
#define GH_SIGNER_ERROR gh_signer_error_quark()
GQuark gh_signer_error_quark(void);

/* The org.nostr.Signer service adapter only. NIP-46 sessions are not implied.
 * Calls always use the selected npub as the signer selector. Signed events are
 * independently verified; NIP-44 replies carry no pubkey, so their key binding
 * relies on the service contract rather than an attestation in the reply.
 * Errors are classified by D-Bus error name only. Each call opts in to typed
 * approval errors (nip55l >= 0.5.0): Error.ApprovalTimedOut -> TIMED_OUT,
 * Error.NoApprovalAgent -> NO_APPROVER, Error.IdentityChanged (approved, but
 * the selected npub no longer resolves to the approved key) -> KEY_MISMATCH,
 * Error.ApprovalDenied -> DENIED. A pre-0.5.0 service reports all of these
 * as ApprovalDenied, hence DENIED. Calls for one selected account use a
 * private connection to the same session bus. Cancelling any in-flight call
 * closes that connection, revoking all that account's pending approvals. */
GhSigner *gh_signer_new(GDBusConnection *bus, const gchar *selected_npub,
                        GError **error);
/* Revokes pending service approvals and cancels their D-Bus calls. The owning
 * thread-default main context must keep iterating until callbacks finish. */
void gh_signer_free(GhSigner *signer);
/* Revokes all outstanding calls before changing the selected public identity. */
gboolean gh_signer_select(GhSigner *signer, const gchar *npub, GError **error);

void gh_signer_sign_async(GhSigner *signer, const gchar *unsigned_event,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data);
gchar *gh_signer_sign_finish(GAsyncResult *result, GError **error);
void gh_signer_nip44_encrypt_async(GhSigner *signer, const gchar *plaintext,
                                    const gchar *peer_pubkey_hex,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);
void gh_signer_nip44_decrypt_async(GhSigner *signer, const gchar *ciphertext,
                                    const gchar *peer_pubkey_hex,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);
gchar *gh_signer_nip44_finish(GAsyncResult *result, GError **error);

G_END_DECLS
#endif
