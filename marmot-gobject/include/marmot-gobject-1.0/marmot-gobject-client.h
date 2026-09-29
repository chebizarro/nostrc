/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectClient: Main GObject interface for the Marmot protocol.
 * Provides asynchronous (GTask-based) wrappers around libmarmot's C API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_CLIENT_H
#define MARMOT_GOBJECT_CLIENT_H

#include <glib-object.h>
#include <gio/gio.h>
#include "marmot-gobject-group.h"
#include "marmot-gobject-message.h"
#include "marmot-gobject-welcome.h"
#include "marmot-gobject-storage.h"
#include "marmot-gobject-enums.h"
#include "marmot-gobject-boxed.h"

G_BEGIN_DECLS

#define MARMOT_GOBJECT_TYPE_CLIENT (marmot_gobject_client_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectClient, marmot_gobject_client, MARMOT_GOBJECT, CLIENT, GObject)

/**
 * MarmotGobjectClient:
 *
 * Main Marmot protocol client. Wraps the underlying C Marmot* instance
 * and provides asynchronous operations via GTask.
 *
 * ## Construction
 *
 * Use marmot_gobject_client_new() with a storage backend:
 *
 * |[<!-- language="C" -->
 * MarmotGobjectMemoryStorage *storage = marmot_gobject_memory_storage_new();
 * MarmotGobjectClient *client = marmot_gobject_client_new(
 *     MARMOT_GOBJECT_STORAGE(storage));
 * ]|
 *
 * ## Signals
 *
 * - #MarmotGobjectClient::group-joined - Emitted when a group is joined via welcome
 * - #MarmotGobjectClient::message-received - Emitted when a message is decrypted
 * - #MarmotGobjectClient::welcome-received - Emitted when a welcome is processed
 *
 * Since: 1.0
 */

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_new:
 * @storage: (transfer none): a #MarmotGobjectStorage implementation
 *
 * Creates a new MarmotGobjectClient with default configuration.
 * The storage is borrowed — the client keeps a reference.
 *
 * Returns: (transfer full): a new #MarmotGobjectClient
 */
MarmotGobjectClient *marmot_gobject_client_new(MarmotGobjectStorage *storage);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-00: Key Package (async)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_create_key_package_async:
 * @self: a #MarmotGobjectClient
 * @nostr_pubkey_hex: user's Nostr public key as hex string
 * @nostr_sk_hex: user's Nostr secret key as hex string
 * @relay_urls: (array zero-terminated=1) (nullable): relay URLs
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback to invoke when complete
 * @user_data: data for @callback
 *
 * Asynchronously creates an MLS KeyPackage wrapped in a signed kind:30443
 * (#MARMOT_GOBJECT_KIND_KEY_PACKAGE, addressable) event. Every KeyPackage of
 * an account reuses the account's stable `d` publication slot, so publishing
 * a new one replaces the previous event on relays.
 * Requires the user's secret key for MLS credential signing.
 *
 * For signer-only flows where the caller does not hold the secret key,
 * use marmot_gobject_client_create_key_package_unsigned_async() instead.
 */
void marmot_gobject_client_create_key_package_async(MarmotGobjectClient *self,
                                                     const gchar *nostr_pubkey_hex,
                                                     const gchar *nostr_sk_hex,
                                                     const gchar * const *relay_urls,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer user_data);

/**
 * marmot_gobject_client_create_key_package_unsigned_async:
 * @self: a #MarmotGobjectClient
 * @nostr_pubkey_hex: user's Nostr public key as hex string
 * @relay_urls: (array zero-terminated=1) (nullable): relay URLs
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback to invoke when complete
 * @user_data: data for @callback
 *
 * Asynchronously creates an MLS KeyPackage wrapped in an *unsigned*
 * kind:30443 event (same tags and stable `d` slot as
 * marmot_gobject_client_create_key_package_async()). The caller is
 * responsible for signing the event
 * externally (e.g., via a D-Bus signer service) before publication.
 *
 * This is the preferred API for signer-only architectures where
 * the plugin does not hold the user's secret key.
 *
 * Since: 1.0
 */
void marmot_gobject_client_create_key_package_unsigned_async(MarmotGobjectClient *self,
                                                              const gchar *nostr_pubkey_hex,
                                                              const gchar * const *relay_urls,
                                                              GCancellable *cancellable,
                                                              GAsyncReadyCallback callback,
                                                              gpointer user_data);

/**
 * marmot_gobject_client_create_key_package_unsigned_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Finishes an async unsigned key package creation.
 *
 * Returns: (transfer full) (nullable): the unsigned key package event JSON,
 *   or NULL on error. The caller must sign this event before publishing.
 */
gchar *marmot_gobject_client_create_key_package_unsigned_finish(MarmotGobjectClient *self,
                                                                 GAsyncResult *result,
                                                                 GError **error);

/**
 * marmot_gobject_client_create_key_package_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Finishes an async key package creation.
 *
 * Returns: (transfer full) (nullable): the key package event JSON, or NULL on error
 */
gchar *marmot_gobject_client_create_key_package_finish(MarmotGobjectClient *self,
                                                        GAsyncResult *result,
                                                        GError **error);

/**
 * marmot_gobject_select_key_package_event:
 * @event_jsons: (array zero-terminated=1): candidate kind:30443 KeyPackage
 *   event JSONs, e.g. everything a multi-relay fetch returned
 * @owner_pubkey_hex: (nullable): when set, only events authored by this
 *   account (64 hex chars) are considered
 * @error: (nullable): return location for a #GError
 *
 * Chooses the KeyPackage event to consume for an invite. Within each
 * `(pubkey, d)` slot the newest authenticated event wins (ties: lower event
 * id), and a slot whose winner fails validation yields nothing. Across slots
 * the newest valid event wins (ties: lower KeyPackageRef). Unsigned, forged
 * and legacy kind:443 events are ignored. See libmarmot's
 * marmot_select_key_package_event() for the full rules.
 *
 * Returns: the index into @event_jsons of the selected event, or -1 with
 *   @error set when no valid candidate exists or an argument is invalid
 *
 * Since: 1.1
 */
gint marmot_gobject_select_key_package_event(const gchar * const *event_jsons,
                                             const gchar *owner_pubkey_hex,
                                             GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Creation (async)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_create_group_async:
 * @self: a #MarmotGobjectClient
 * @creator_pubkey_hex: creator's Nostr pubkey as hex
 * @key_package_jsons: (array zero-terminated=1): JSON strings of signed
 *   kind:30443 events (see marmot_gobject_select_key_package_event())
 * @group_name: (nullable): group name
 * @group_description: (nullable): group description
 * @admin_pubkey_hexes: (array zero-terminated=1) (nullable): admin pubkeys as hex
 * @relay_urls: (array zero-terminated=1) (nullable): relay URLs for the group
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously creates a new MLS group.
 */
void marmot_gobject_client_create_group_async(MarmotGobjectClient *self,
                                               const gchar *creator_pubkey_hex,
                                               const gchar * const *key_package_jsons,
                                               const gchar *group_name,
                                               const gchar *group_description,
                                               const gchar * const *admin_pubkey_hexes,
                                               const gchar * const *relay_urls,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

/**
 * marmot_gobject_client_create_group_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @out_welcome_jsons: (out) (array zero-terminated=1) (transfer full) (nullable):
 *   welcome rumor JSONs (one per invited member)
 * @out_evolution_json: (out) (transfer full) (nullable): evolution event JSON
 * @error: (nullable): return location for a #GError
 *
 * Finishes async group creation.
 *
 * Returns: (transfer full) (nullable): the created #MarmotGobjectGroup, or NULL on error
 */
MarmotGobjectGroup *marmot_gobject_client_create_group_finish(MarmotGobjectClient *self,
                                                               GAsyncResult *result,
                                                               gchar ***out_welcome_jsons,
                                                               gchar **out_evolution_json,
                                                               GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-02: Welcome Processing (async)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_process_welcome_async:
 * @self: a #MarmotGobjectClient
 * @wrapper_event_id_hex: the gift-wrap event ID as hex
 * @rumor_event_json: JSON of the unwrapped kind:444 event
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously processes a welcome message.
 */
void marmot_gobject_client_process_welcome_async(MarmotGobjectClient *self,
                                                  const gchar *wrapper_event_id_hex,
                                                  const gchar *rumor_event_json,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

/**
 * marmot_gobject_client_process_welcome_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the #MarmotGobjectWelcome, or NULL on error
 */
MarmotGobjectWelcome *marmot_gobject_client_process_welcome_finish(MarmotGobjectClient *self,
                                                                     GAsyncResult *result,
                                                                     GError **error);

/**
 * marmot_gobject_client_accept_welcome_async:
 * @self: a #MarmotGobjectClient
 * @welcome: the welcome to accept
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 */
void marmot_gobject_client_accept_welcome_async(MarmotGobjectClient *self,
                                                 MarmotGobjectWelcome *welcome,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);

/**
 * marmot_gobject_client_accept_welcome_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean marmot_gobject_client_accept_welcome_finish(MarmotGobjectClient *self,
                                                      GAsyncResult *result,
                                                      GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-03: Messages (async)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_send_message_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: target group MLS ID as hex
 * @inner_event_json: JSON of the unsigned event to encrypt
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously encrypts and wraps an event for the group.
 */
void marmot_gobject_client_send_message_async(MarmotGobjectClient *self,
                                               const gchar *mls_group_id_hex,
                                               const gchar *inner_event_json,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data);

/**
 * marmot_gobject_client_send_message_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the encrypted group event JSON, or NULL on error
 */
gchar *marmot_gobject_client_send_message_finish(MarmotGobjectClient *self,
                                                  GAsyncResult *result,
                                                  GError **error);

/**
 * marmot_gobject_client_process_message_async:
 * @self: a #MarmotGobjectClient
 * @group_event_json: JSON of the kind:445 event (after NIP-59 unwrap)
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously processes a received group message.
 */
void marmot_gobject_client_process_message_async(MarmotGobjectClient *self,
                                                  const gchar *group_event_json,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);

/**
 * marmot_gobject_client_process_message_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @out_result_type: (out) (nullable): the message result type
 * @error: (nullable): return location for a #GError
 *
 * Finishes async message processing.
 *
 * When *out_result_type is APPLICATION, returns the decrypted inner event JSON.
 * When *out_result_type is COMMIT, returns NULL (group state updated internally;
 * #MarmotGobjectClient::group-updated carries the updated group).
 * When *out_result_type is OWN_MESSAGE, returns NULL (skip).
 *
 * Returns: (transfer full) (nullable): decrypted inner event JSON, or NULL
 */
gchar *marmot_gobject_client_process_message_finish(MarmotGobjectClient *self,
                                                     GAsyncResult *result,
                                                     MarmotGobjectMessageResultType *out_result_type,
                                                     GError **error);

/**
 * marmot_gobject_client_update_group_metadata_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: hex-encoded MLS group ID
 * @name: (nullable): the new group name, or %NULL to keep it
 * @description: (nullable): the new description, or %NULL to keep it
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously makes a Commit changing the group's metadata (admins only;
 * marmot_update_group_metadata()).  The Commit is pending: the group does
 * not change until marmot_gobject_client_merge_pending_commit_async().
 *
 * Since: 1.2
 */
void marmot_gobject_client_update_group_metadata_async(MarmotGobjectClient *self,
                                                        const gchar *mls_group_id_hex,
                                                        const gchar *name,
                                                        const gchar *description,
                                                        GCancellable *cancellable,
                                                        GAsyncReadyCallback callback,
                                                        gpointer user_data);

/**
 * marmot_gobject_client_update_group_metadata_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns the kind:445 Commit event, signed by a fresh ephemeral key.
 * Publish it to the group relays; once one accepts it (NIP-01 OK) call
 * marmot_gobject_client_merge_pending_commit_async(), otherwise
 * marmot_gobject_client_clear_pending_commit_async().
 *
 * Returns: (transfer full) (nullable): the Commit event JSON, or %NULL on error
 *
 * Since: 1.2
 */
gchar *marmot_gobject_client_update_group_metadata_finish(MarmotGobjectClient *self,
                                                           GAsyncResult *result,
                                                           GError **error);

/**
 * marmot_gobject_client_merge_pending_commit_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: hex-encoded MLS group ID
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Applies the group's pending Commit once a relay accepted its event
 * (marmot_merge_pending_commit()) and emits #MarmotGobjectClient::group-updated.
 * Fails with %MARMOT_ERR_WRONG_EPOCH when a competing Commit won meanwhile;
 * the group then follows that Commit (also announced by ::group-updated).
 *
 * Since: 1.2
 */
void marmot_gobject_client_merge_pending_commit_async(MarmotGobjectClient *self,
                                                       const gchar *mls_group_id_hex,
                                                       GCancellable *cancellable,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

/**
 * marmot_gobject_client_merge_pending_commit_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE when the Commit was applied
 *
 * Since: 1.2
 */
gboolean marmot_gobject_client_merge_pending_commit_finish(MarmotGobjectClient *self,
                                                            GAsyncResult *result,
                                                            GError **error);

/**
 * marmot_gobject_client_clear_pending_commit_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: hex-encoded MLS group ID
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Discards the group's pending Commit when no relay accepted it
 * (marmot_clear_pending_commit()); the group stays in its epoch, apart from
 * member Commits that were deferred behind ours and now apply.
 *
 * Since: 1.2
 */
void marmot_gobject_client_clear_pending_commit_async(MarmotGobjectClient *self,
                                                       const gchar *mls_group_id_hex,
                                                       GCancellable *cancellable,
                                                       GAsyncReadyCallback callback,
                                                       gpointer user_data);

/**
 * marmot_gobject_client_clear_pending_commit_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 *
 * Since: 1.2
 */
gboolean marmot_gobject_client_clear_pending_commit_finish(MarmotGobjectClient *self,
                                                            GAsyncResult *result,
                                                            GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-04: Media Encryption (async)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_encrypt_media_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: target group MLS ID as hex
 * @file_data: raw file data
 * @mime_type: (nullable): MIME type
 * @filename: (nullable): original filename
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously encrypts media for sharing in an MLS group.
 */
void marmot_gobject_client_encrypt_media_async(MarmotGobjectClient *self,
                                                const gchar *mls_group_id_hex,
                                                GBytes *file_data,
                                                const gchar *mime_type,
                                                const gchar *filename,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

/**
 * marmot_gobject_client_encrypt_media_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): encrypted media metadata and data,
 *   or NULL on error
 */
MarmotGobjectEncryptedMedia *marmot_gobject_client_encrypt_media_finish(MarmotGobjectClient *self,
                                                                         GAsyncResult *result,
                                                                         GError **error);

/**
 * marmot_gobject_client_decrypt_media_async:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: group MLS ID as hex
 * @encrypted_data: encrypted file data
 * @mime_type: (nullable): MIME type from imeta tag
 * @filename: (nullable): filename from imeta tag
 * @original_size: original plaintext size
 * @file_hash: (nullable): SHA-256 hash of plaintext (32 bytes)
 * @nonce: (nullable): ChaCha20-Poly1305 nonce (12 bytes)
 * @epoch: MLS epoch when encryption key was derived
 * @cancellable: (nullable): a #GCancellable
 * @callback: callback
 * @user_data: data for @callback
 *
 * Asynchronously decrypts media from an MLS group.
 */
void marmot_gobject_client_decrypt_media_async(MarmotGobjectClient *self,
                                                const gchar *mls_group_id_hex,
                                                GBytes *encrypted_data,
                                                const gchar *mime_type,
                                                const gchar *filename,
                                                gsize original_size,
                                                const guint8 file_hash[32],
                                                const guint8 nonce[12],
                                                guint64 epoch,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);

/**
 * marmot_gobject_client_decrypt_media_finish:
 * @self: a #MarmotGobjectClient
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): decrypted data as #GBytes, or NULL on error
 */
GBytes *marmot_gobject_client_decrypt_media_finish(MarmotGobjectClient *self,
                                                    GAsyncResult *result,
                                                    GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * Synchronous queries
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_get_group:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: MLS group ID as hex
 * @error: (nullable): return location for a #GError
 *
 * Gets a group by MLS group ID. Synchronous (fast, local storage lookup).
 *
 * Returns: (transfer full) (nullable): the #MarmotGobjectGroup, or NULL
 */
MarmotGobjectGroup *marmot_gobject_client_get_group(MarmotGobjectClient *self,
                                                     const gchar *mls_group_id_hex,
                                                     GError **error);

/**
 * marmot_gobject_client_get_all_groups:
 * @self: a #MarmotGobjectClient
 * @error: (nullable): return location for a #GError
 *
 * Gets all groups. Synchronous.
 *
 * Returns: (transfer full) (element-type MarmotGobjectGroup) (nullable):
 *   a #GPtrArray of #MarmotGobjectGroup, or NULL on error
 */
GPtrArray *marmot_gobject_client_get_all_groups(MarmotGobjectClient *self,
                                                 GError **error);

/**
 * marmot_gobject_client_get_messages:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: MLS group ID as hex
 * @limit: maximum number of messages (0 for default)
 * @offset: pagination offset
 * @error: (nullable): return location for a #GError
 *
 * Gets messages for a group. Synchronous.
 *
 * Returns: (transfer full) (element-type MarmotGobjectMessage) (nullable):
 *   a #GPtrArray of #MarmotGobjectMessage, or NULL on error
 */
GPtrArray *marmot_gobject_client_get_messages(MarmotGobjectClient *self,
                                               const gchar *mls_group_id_hex,
                                               guint limit,
                                               guint offset,
                                               GError **error);

/**
 * marmot_gobject_client_get_pending_welcomes:
 * @self: a #MarmotGobjectClient
 * @error: (nullable): return location for a #GError
 *
 * Gets all pending welcomes. Synchronous.
 *
 * Returns: (transfer full) (element-type MarmotGobjectWelcome) (nullable):
 *   a #GPtrArray of #MarmotGobjectWelcome, or NULL on error
 */
GPtrArray *marmot_gobject_client_get_pending_welcomes(MarmotGobjectClient *self,
                                                       GError **error);

/* ══════════════════════════════════════════════════════════════════════════
 * Internal access (for plugins needing raw libmarmot API)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_gobject_client_get_marmot:
 * @self: a #MarmotGobjectClient
 *
 * Get the underlying libmarmot Marmot instance for direct API access.
 * This is intended for advanced use cases like MIP-04 media encryption
 * where the GObject wrapper doesn't yet expose the functionality.
 *
 * The returned pointer is owned by the client and must not be freed.
 *
 * Returns: (transfer none) (nullable): the underlying Marmot instance
 */
struct Marmot *marmot_gobject_client_get_marmot(MarmotGobjectClient *self);

/**
 * marmot_gobject_client_get_group_relay_urls:
 * @self: a #MarmotGobjectClient
 * @mls_group_id_hex: MLS group ID as hex string
 * @out_count: (out) (nullable): number of relay URLs returned
 *
 * Get the relay URLs associated with a group from storage.
 * Returns NULL if no group-specific relays are configured.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable):
 *   NULL-terminated array of relay URL strings, or NULL. Free with g_strfreev().
 */
gchar **marmot_gobject_client_get_group_relay_urls(MarmotGobjectClient *self,
                                                    const gchar *mls_group_id_hex,
                                                    gsize *out_count);

G_END_DECLS

#endif /* MARMOT_GOBJECT_CLIENT_H */
