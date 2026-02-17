/*
 * libmarmot - C implementation of the Marmot protocol (MLS + Nostr)
 *
 * Umbrella header — includes all public headers.
 *
 * libmarmot implements the Marmot protocol (MIP-00 through MIP-04) for
 * secure group messaging over Nostr using MLS (RFC 9420).
 *
 * Dependencies:
 *   - libsodium (X25519, Ed25519, ChaCha20-Poly1305, CSPRNG)
 *   - OpenSSL   (AES-128-GCM, HKDF-SHA256, SHA-256)
 *   - libnostr  (Nostr event creation, signing, verification)
 *   - NIP-44    (Content encryption for MIP-03 messages)
 *   - NIP-59    (Gift wrapping for MIP-02 welcomes and MIP-03 messages)
 *
 * Ciphersuite: MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519 (0x0001)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_H
#define MARMOT_H

#include "marmot-error.h"
#include "marmot-types.h"
#include "marmot-storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Media Encryption (MIP-04)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Encrypt media file for sharing in an MLS group.
 * Derives encryption key from group's exporter secret.
 */
MarmotError marmot_encrypt_media(Marmot *m,
                                  const MarmotGroupId *mls_group_id,
                                  const uint8_t *file_data, size_t file_len,
                                  const char *mime_type,
                                  const char *filename,
                                  MarmotEncryptedMedia *result);

/**
 * Decrypt media file encrypted for an MLS group.
 * Derives decryption key from group's exporter secret.
 */
MarmotError marmot_decrypt_media(Marmot *m,
                                  const MarmotGroupId *mls_group_id,
                                  const uint8_t *encrypted_data, size_t enc_len,
                                  const MarmotImetaInfo *imeta,
                                  uint8_t **plaintext_out, size_t *plaintext_len);

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_new:
 * @storage: (transfer full): storage backend (ownership taken)
 *
 * Create a new Marmot instance with default configuration.
 * The storage is owned by the Marmot instance and freed on marmot_free().
 *
 * Returns: (transfer full) (nullable): new Marmot instance, or NULL on error
 */
Marmot *marmot_new(MarmotStorage *storage);

/**
 * marmot_new_with_config:
 * @storage: (transfer full): storage backend (ownership taken)
 * @config: configuration (copied)
 *
 * Create a new Marmot instance with custom configuration.
 *
 * Returns: (transfer full) (nullable): new Marmot instance, or NULL on error
 */
Marmot *marmot_new_with_config(MarmotStorage *storage, const MarmotConfig *config);

/**
 * marmot_free:
 * @m: (transfer full) (nullable): Marmot instance to destroy
 *
 * Free a Marmot instance and its storage backend.
 */
void marmot_free(Marmot *m);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-00: Credentials & KeyPackages
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_create_key_package:
 * @m: Marmot instance
 * @nostr_pubkey: (array fixed-size=32): user's Nostr public key (x-only, 32 bytes)
 * @nostr_sk: (array fixed-size=32): user's Nostr secret key (32 bytes) for signing
 * @relay_urls: (array length=relay_count): relay URL strings
 * @relay_count: number of relay URLs
 * @result: (out): result containing the kind:443 event JSON
 *
 * Create an MLS KeyPackage and wrap it in a kind:443 Nostr event.
 * The event is unsigned — the caller must sign and publish it.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_key_package(Marmot *m,
                                       const uint8_t nostr_pubkey[32],
                                       const uint8_t nostr_sk[32],
                                       const char **relay_urls, size_t relay_count,
                                       MarmotKeyPackageResult *result);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Construction
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_create_group:
 * @m: Marmot instance
 * @creator_pubkey: (array fixed-size=32): creator's Nostr public key (32 bytes)
 * @key_package_event_jsons: (array length=kp_count): JSON strings of kind:443 events
 * @kp_count: number of key package events (members to invite)
 * @config: group configuration (name, description, admins, relays)
 * @result: (out): result containing group, welcome rumors, evolution event
 *
 * Create a new MLS group and generate welcome messages for each member.
 *
 * After creating a group, the caller must:
 * 1. Call marmot_merge_pending_commit() to finalize the group state
 * 2. Gift-wrap each welcome rumor (NIP-59) and send to the member
 * 3. Publish the evolution event to group relays
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_group(Marmot *m,
                                 const uint8_t creator_pubkey[32],
                                 const char **key_package_event_jsons, size_t kp_count,
                                 const MarmotGroupConfig *config,
                                 MarmotCreateGroupResult *result);

/**
 * marmot_merge_pending_commit:
 * @m: Marmot instance
 * @mls_group_id: the group to merge
 *
 * Merge the pending commit after group creation or member addition.
 * Must be called after marmot_create_group() or marmot_add_members().
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_merge_pending_commit(Marmot *m,
                                         const MarmotGroupId *mls_group_id);

/**
 * marmot_add_members:
 * @m: Marmot instance
 * @mls_group_id: the group to add members to
 * @key_package_event_jsons: (array length=kp_count): JSON strings of kind:443 events
 * @kp_count: number of members to add
 * @out_welcome_jsons: (out) (array length=out_welcome_count): welcome rumor JSONs
 * @out_welcome_count: (out): number of welcome rumors
 * @out_commit_json: (out) (transfer full): commit event JSON
 *
 * Add members to an existing group.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_add_members(Marmot *m,
                                const MarmotGroupId *mls_group_id,
                                const char **key_package_event_jsons, size_t kp_count,
                                char ***out_welcome_jsons, size_t *out_welcome_count,
                                char **out_commit_json);

/**
 * marmot_remove_members:
 * @m: Marmot instance
 * @mls_group_id: the group to remove members from
 * @member_pubkeys: (array length=count): 32-byte pubkeys of members to remove
 * @count: number of members
 * @out_commit_json: (out) (transfer full): commit event JSON
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_remove_members(Marmot *m,
                                   const MarmotGroupId *mls_group_id,
                                   const uint8_t (*member_pubkeys)[32], size_t count,
                                   char **out_commit_json);

/**
 * marmot_leave_group:
 * @m: Marmot instance
 * @mls_group_id: the group to leave
 *
 * Leave a group. The group state is set to Inactive locally.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_leave_group(Marmot *m,
                                const MarmotGroupId *mls_group_id);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-02: Welcome Events
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_process_welcome:
 * @m: Marmot instance
 * @wrapper_event_id: (array fixed-size=32): the gift-wrap event ID
 * @rumor_event_json: JSON of the unwrapped kind:444 rumor event
 * @out_welcome: (out) (transfer full): the stored welcome record
 *
 * Process a received welcome message. Validates structure per MIP-02,
 * extracts group info, and stores the welcome as pending.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_process_welcome(Marmot *m,
                                    const uint8_t wrapper_event_id[32],
                                    const char *rumor_event_json,
                                    MarmotWelcome **out_welcome);

/**
 * marmot_accept_welcome:
 * @m: Marmot instance
 * @welcome: the welcome to accept
 *
 * Accept a pending welcome and join the group.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome);

/**
 * marmot_decline_welcome:
 * @m: Marmot instance
 * @welcome: the welcome to decline
 *
 * Decline a pending welcome. The group state is set to Inactive.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome);

/**
 * marmot_get_pending_welcomes:
 * @m: Marmot instance
 * @pagination: (nullable): pagination parameters, or NULL for defaults
 * @out_welcomes: (out) (array length=out_count): pending welcomes
 * @out_count: (out): number of welcomes
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_get_pending_welcomes(Marmot *m,
                                         const MarmotPagination *pagination,
                                         MarmotWelcome ***out_welcomes, size_t *out_count);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-03: Group Messages
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_create_message:
 * @m: Marmot instance
 * @mls_group_id: the group to send to
 * @inner_event_json: JSON of the unsigned event to encrypt and send
 * @result: (out): outgoing message with encrypted event JSON
 *
 * Create an encrypted group message. The inner event is encrypted using
 * NIP-44 with the MLS exporter secret as the conversation key.
 *
 * The caller must gift-wrap the result and publish to group relays.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_message(Marmot *m,
                                   const MarmotGroupId *mls_group_id,
                                   const char *inner_event_json,
                                   MarmotOutgoingMessage *result);

/**
 * marmot_process_message:
 * @m: Marmot instance
 * @group_event_json: JSON of the kind:445 group event (rumor, after NIP-59 unwrap)
 * @result: (out): processing result
 *
 * Process a received group message. Handles:
 * - Application messages (decrypts content)
 * - Commits (updates group state)
 * - Proposals (queued for commit)
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_process_message(Marmot *m,
                                    const char *group_event_json,
                                    MarmotMessageResult *result);

/* ══════════════════════════════════════════════════════════════════════════
 * Group queries
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_get_group:
 * @m: Marmot instance
 * @mls_group_id: group to query
 * @out: (out) (transfer full) (nullable): the group, or NULL if not found
 *
 * Returns: MARMOT_OK on success (even if not found — check *out)
 */
MarmotError marmot_get_group(Marmot *m,
                              const MarmotGroupId *mls_group_id,
                              MarmotGroup **out);

/**
 * marmot_get_all_groups:
 * @m: Marmot instance
 * @out_groups: (out) (array length=out_count): all groups
 * @out_count: (out): number of groups
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_get_all_groups(Marmot *m,
                                   MarmotGroup ***out_groups, size_t *out_count);

/**
 * marmot_get_messages:
 * @m: Marmot instance
 * @mls_group_id: group to query
 * @pagination: (nullable): pagination parameters
 * @out_msgs: (out) (array length=out_count): messages
 * @out_count: (out): number of messages
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_get_messages(Marmot *m,
                                 const MarmotGroupId *mls_group_id,
                                 const MarmotPagination *pagination,
                                 MarmotMessage ***out_msgs, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_H */
