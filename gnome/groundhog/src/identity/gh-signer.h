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
  GH_SIGNER_ERROR_INVALID_RESULT
} GhSignerError;
#define GH_SIGNER_ERROR gh_signer_error_quark()
GQuark gh_signer_error_quark(void);

/* The org.nostr.Signer service adapter only. NIP-46 sessions are not implied.
 * Calls always use the selected npub as the signer selector. Signed events are
 * independently verified; NIP-44 replies carry no pubkey, so their key binding
 * relies on the service contract rather than an attestation in the reply. */
GhSigner *gh_signer_new(GDBusConnection *bus, const gchar *selected_npub,
                        GError **error);
void gh_signer_free(GhSigner *signer);
/* Cancels all outstanding calls before changing the selected public identity. */
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
