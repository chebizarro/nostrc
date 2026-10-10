#ifndef GH_NIP46_CREDENTIALS_H
#define GH_NIP46_CREDENTIALS_H

#include "gh-identity.h"

G_BEGIN_DECLS

#define GH_NIP46_CREDENTIAL_SCHEMA "org.nostr.Groundhog.Nip46Credential"
#define GH_NIP46_CREDENTIAL_LABEL "Groundhog remote signer"
#define GH_NIP46_CREDENTIAL_VERSION "1"
#define GH_NIP46_CREDENTIAL_ERROR (gh_nip46_credential_error_quark())
GQuark gh_nip46_credential_error_quark(void);

typedef enum {
  GH_NIP46_CREDENTIAL_ERROR_LOCKED,
  GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE,
  GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND,
  GH_NIP46_CREDENTIAL_ERROR_INVALID,
  GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION,
  GH_NIP46_CREDENTIAL_ERROR_FAILED
} GhNip46CredentialError;

typedef struct _GhNip46Credential GhNip46Credential;

/* All hex inputs must be lowercase. The client key is wiped on release. */
GhNip46Credential *gh_nip46_credential_new(const gchar *user_pubkey_hex,
                                            const gchar *remote_signer_pubkey_hex,
                                            const gchar *client_secret_hex,
                                            const gchar * const *relays,
                                            GError **error);
void gh_nip46_credential_free(GhNip46Credential *credential);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip46Credential, gh_nip46_credential_free)
const gchar *gh_nip46_credential_get_user_pubkey_hex(const GhNip46Credential *credential);
const gchar *gh_nip46_credential_get_remote_signer_pubkey_hex(const GhNip46Credential *credential);
const gchar *gh_nip46_credential_get_client_secret_hex(const GhNip46Credential *credential);
const gchar * const *gh_nip46_credential_get_relays(const GhNip46Credential *credential);

#define GH_TYPE_NIP46_CREDENTIAL_STORE (gh_nip46_credential_store_get_type())
G_DECLARE_FINAL_TYPE(GhNip46CredentialStore, gh_nip46_credential_store,
                     GH, NIP46_CREDENTIAL_STORE, GObject)

/* NULL selects Keychain on macOS, Secret Service elsewhere. */
GhNip46CredentialStore *gh_nip46_credential_store_new(void);
/* Explicit Secret Service selection for a private-bus test on macOS. */
GhNip46CredentialStore *gh_nip46_credential_store_new_secret_service(void);

/* A list of GhIdentityInfo. Locked items are listed from public attributes. */
void gh_nip46_credential_store_list_async(GhNip46CredentialStore *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);
GPtrArray *gh_nip46_credential_store_list_finish(GhNip46CredentialStore *self,
                                                  GAsyncResult *result, GError **error);
void gh_nip46_credential_store_lookup_async(GhNip46CredentialStore *self,
                                            const gchar *user_pubkey_hex,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
/* As lookup_async, but @interactive lets the backend prompt to unlock the
 * keyring or ask for Keychain access (the explicit Unlock action). */
void gh_nip46_credential_store_lookup_full_async(GhNip46CredentialStore *self,
                                                 const gchar *account, gboolean interactive,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);
GhNip46Credential *gh_nip46_credential_store_lookup_finish(GhNip46CredentialStore *self,
                                                            GAsyncResult *result, GError **error);
void gh_nip46_credential_store_store_async(GhNip46CredentialStore *self,
                                           const GhNip46Credential *credential,
                                           gboolean interactive, GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_nip46_credential_store_store_finish(GhNip46CredentialStore *self,
                                                GAsyncResult *result, GError **error);
void gh_nip46_credential_store_delete_async(GhNip46CredentialStore *self,
                                            const gchar *user_pubkey_hex,
                                            gboolean interactive, GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_nip46_credential_store_delete_finish(GhNip46CredentialStore *self,
                                                 GAsyncResult *result, GError **error);

G_END_DECLS
#endif
