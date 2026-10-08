#ifndef GH_ACCOUNT_CONTROLLER_H
#define GH_ACCOUNT_CONTROLLER_H

#include <gio/gio.h>
#include "gh-identity.h"
#include "gh-nip46-credentials.h"

G_BEGIN_DECLS

typedef enum {
  GH_ACCOUNT_STATE_DISCOVERING,       /* first identity listing in progress */
  GH_ACCOUNT_STATE_STORE_UNAVAILABLE, /* signer-owned metadata could not be read */
  GH_ACCOUNT_STATE_NO_IDENTITIES,
  GH_ACCOUNT_STATE_UNSELECTED,
  GH_ACCOUNT_STATE_SELECTED_MISSING,  /* current-npub is not in the listed store */
  GH_ACCOUNT_STATE_ACTIVE
} GhAccountState;

typedef enum {
  GH_SIGNER_AVAILABILITY_UNKNOWN,
  GH_SIGNER_AVAILABILITY_NO_BUS,
  GH_SIGNER_AVAILABILITY_ABSENT,
  GH_SIGNER_AVAILABILITY_ACTIVATABLE, /* not running; the bus can start it */
  GH_SIGNER_AVAILABILITY_RUNNING
} GhSignerAvailability;

typedef enum {
  GH_REMOTE_SIGNER_LOADING_CREDENTIAL,
  GH_REMOTE_SIGNER_CONNECTING,
  GH_REMOTE_SIGNER_READY,
  GH_REMOTE_SIGNER_OFFLINE,
  GH_REMOTE_SIGNER_LOCKED,
  GH_REMOTE_SIGNER_ERROR
} GhRemoteSignerState;

/* Runs in a worker thread and returns GhIdentityInfo items (gh-identity.h). */
typedef GPtrArray *(*GhAccountListFunc)(gpointer user_data, GError **error);
typedef struct _GhNip46Session GhNip46Session;
/* Test-only seam: return an owned fake-transport session for the selected npub. */
typedef GhNip46Session *(*GhAccountSessionFactory)(const gchar *npub, gpointer user_data);

#define GH_TYPE_ACCOUNT_CONTROLLER (gh_account_controller_get_type())
G_DECLARE_FINAL_TYPE(GhAccountController, gh_account_controller, GH,
                     ACCOUNT_CONTROLLER, GObject)

/* Groundhog's active (backend, npub) account. Grotto metadata and remote
 * credential metadata are listed independently. The controller owns the
 * active remote credential/session, but never exposes secret material.
 * bus may be NULL. Emits "changed" after state updates. */
GhAccountController *gh_account_controller_new(GSettings *settings,
                                               GDBusConnection *bus);
GhAccountController *gh_account_controller_new_full(GSettings *settings,
                                                    GDBusConnection *bus,
                                                    GhAccountListFunc list,
                                                    gpointer list_data);
GhAccountController *gh_account_controller_new_with_credentials(GSettings *settings,
                                           GDBusConnection *bus,
                                           GhNip46CredentialStore *credentials);
GhAccountController *gh_account_controller_new_full_with_credentials(GSettings *settings,
                                           GDBusConnection *bus, GhAccountListFunc list,
                                           gpointer list_data,
                                           GhNip46CredentialStore *credentials);
/* Test seam: substitutes public remote metadata listing without a keyring. */
GhAccountController *gh_account_controller_new_full_with_remote_list(GSettings *settings,
                                           GDBusConnection *bus, GhAccountListFunc grotto_list,
                                           gpointer grotto_data, GhAccountListFunc remote_list,
                                           gpointer remote_data);
/* Only for fake remote-list tests; set before selecting a remote account. */
void gh_account_controller_set_session_factory_for_test(GhAccountController *self,
                                           GhAccountSessionFactory factory,
                                           gpointer user_data);
/* Re-lists identities; a result from an older listing is discarded. */
void gh_account_controller_refresh(GhAccountController *self);
gboolean gh_account_controller_select(GhAccountController *self,
                                      const gchar *npub, GError **error);
gboolean gh_account_controller_select_backend(GhAccountController *self,
                                              GhSignerBackend backend,
                                              const gchar *npub, GError **error);

GhAccountState gh_account_controller_get_state(GhAccountController *self);
GhSignerAvailability gh_account_controller_get_signer_availability(GhAccountController *self);
GhSignerBackend gh_account_controller_get_active_backend(GhAccountController *self);
GhRemoteSignerState gh_account_controller_get_remote_state(GhAccountController *self);
/* Stale generations are ignored. Only a matching remote account can open the gate. */
void gh_account_controller_set_remote_storage_ready(GhAccountController *self,
                                                    guint64 generation,
                                                    gboolean ready);
gchar *gh_account_controller_describe_limits(GhAccountController *self,
                                             gboolean network_available);
/* NULL unless the state is ACTIVE. */
const gchar *gh_account_controller_get_active_npub(GhAccountController *self);
/* Borrowed; NULL until a listing succeeds. */
GPtrArray *gh_account_controller_get_identities(GhAccountController *self);

/* The generation changes whenever the active account pair (or its absence)
 * or remote network mode changes. It is revoked before its cancellable is cancelled, so work bound
 * to either sees itself as stale; callbacks must check is_current.
 * The controller owns its signer and cancels its operations on revocation;
 * GhSigner closes each private sender to revoke pending approvals. */
guint64 gh_account_controller_get_generation(GhAccountController *self);
GCancellable *gh_account_controller_get_cancellable(GhAccountController *self);
gboolean gh_account_controller_is_current(GhAccountController *self,
                                          guint64 generation);

/* Internal account-bound signer entry points. Results from a switched or
 * disposed generation are always cancelled, even if the bus replied first.
 * No signer pointer or private key escapes the controller. These do not imply
 * a NIP-17 transport or a message-send UI. */
void gh_account_controller_sign_async(GhAccountController *self,
                                      const gchar *unsigned_event,
                                      GAsyncReadyCallback callback, gpointer user_data);
/* As above, but caller cancellation revokes this operation's pending approval. */
void gh_account_controller_sign_with_cancellable_async(GhAccountController *self,
                                      const gchar *unsigned_event,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data);
gchar *gh_account_controller_sign_finish(GAsyncResult *result, GError **error);
void gh_account_controller_nip44_encrypt_async(GhAccountController *self,
                                                const gchar *plaintext,
                                                const gchar *peer_pubkey_hex,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
void gh_account_controller_nip44_encrypt_with_cancellable_async(GhAccountController *self,
                                                const gchar *plaintext,
                                                const gchar *peer_pubkey_hex,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
void gh_account_controller_nip44_decrypt_async(GhAccountController *self,
                                                const gchar *ciphertext,
                                                const gchar *peer_pubkey_hex,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
void gh_account_controller_nip44_decrypt_with_cancellable_async(GhAccountController *self,
                                                const gchar *ciphertext,
                                                const gchar *peer_pubkey_hex,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
/* W33: an older NIP-04 DM, decrypted read-only; finished with
 * gh_account_controller_nip44_finish(). */
void gh_account_controller_nip04_decrypt_async(GhAccountController *self,
                                                const gchar *ciphertext,
                                                const gchar *peer_pubkey_hex,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
gchar *gh_account_controller_nip44_finish(GAsyncResult *result, GError **error);

/* Why the account cannot send (a read-only state) or receive (offline),
 * or NULL when an active account with a reachable signer can: the account
 * part of the composer's reason (gh-send-ui.h) and the announcement of an
 * account-page transition. Offline is not a reason to refuse sending: the
 * outbox queues it ("Waiting for connection"). */
gchar *gh_account_describe_limits(GhAccountState state,
                                  GhSignerAvailability availability,
                                  const gchar *requested_method,
                                  gboolean network_available);

G_END_DECLS
#endif
