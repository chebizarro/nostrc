#ifndef GH_ACCOUNT_CONTROLLER_H
#define GH_ACCOUNT_CONTROLLER_H

#include <gio/gio.h>

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

/* Runs in a worker thread and returns GhIdentityInfo items (gh-identity.h). */
typedef GPtrArray *(*GhAccountListFunc)(gpointer user_data, GError **error);

#define GH_TYPE_ACCOUNT_CONTROLLER (gh_account_controller_get_type())
G_DECLARE_FINAL_TYPE(GhAccountController, gh_account_controller, GH,
                     ACCOUNT_CONTROLLER, GObject)

/* Groundhog's active account. It reads only signer-owned public metadata,
 * writes only the org.nostr.Groundhog current-npub key, and watches whether
 * org.nostr.Signer is reachable; it never loads secrets. bus may be NULL.
 * Emits "changed" on the main context after every state update. */
GhAccountController *gh_account_controller_new(GSettings *settings,
                                               GDBusConnection *bus);
GhAccountController *gh_account_controller_new_full(GSettings *settings,
                                                    GDBusConnection *bus,
                                                    GhAccountListFunc list,
                                                    gpointer list_data);
/* Re-lists identities; a result from an older listing is discarded. */
void gh_account_controller_refresh(GhAccountController *self);
gboolean gh_account_controller_select(GhAccountController *self,
                                      const gchar *npub, GError **error);

GhAccountState gh_account_controller_get_state(GhAccountController *self);
GhSignerAvailability gh_account_controller_get_signer_availability(GhAccountController *self);
/* NULL unless the state is ACTIVE. */
const gchar *gh_account_controller_get_active_npub(GhAccountController *self);
/* Borrowed; NULL until a listing succeeds. */
GPtrArray *gh_account_controller_get_identities(GhAccountController *self);

/* The generation changes whenever the active account (or its absence)
 * or its requested signer method changes. It is revoked before its cancellable is cancelled, so work bound
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
