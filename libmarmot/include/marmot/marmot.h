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
 * @result: (out): result containing the kind:30443 event JSON
 *
 * Create an MLS KeyPackage and wrap it in a signed kind:30443 (addressable)
 * Nostr event. The event id, pubkey, and Schnorr signature are produced from
 * @nostr_sk.
 *
 * The event's `d` tag is the account's KeyPackage publication slot: a random
 * 32-byte id generated on the first call for @nostr_pubkey, persisted in the
 * MLS key store, and reused by every later call. Publishing the new event
 * therefore replaces the previous KeyPackage on relays (same
 * `(pubkey, 30443, d)` address) instead of accumulating stale packages; the
 * previous package is also marked inactive locally.
 *
 * Tags follow the MDK 0.8 Marmot transport profile pinned by
 * `tests/vectors/mdk/protocol-vectors.json`: `d`, `mls_protocol_version`,
 * `mls_ciphersuite`, `mls_extensions`, `mls_proposals`, `relays` (only when
 * @relay_count > 0), `i` (KeyPackageRef) and `encoding` = `base64`.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_key_package(Marmot *m,
                                       const uint8_t nostr_pubkey[32],
                                       const uint8_t nostr_sk[32],
                                       const char **relay_urls, size_t relay_count,
                                       MarmotKeyPackageResult *result);

/**
 * marmot_create_key_package_unsigned:
 * @m: Marmot instance
 * @nostr_pubkey: (array fixed-size=32): user's Nostr public key (x-only, 32 bytes)
 * @relay_urls: (array length=relay_count): relay URL strings
 * @relay_count: number of relay URLs
 * @result: (out): result containing the unsigned kind:30443 event JSON
 *
 * Create an MLS KeyPackage and wrap it in an *unsigned* kind:30443 Nostr
 * event with the same tags and stable `d` slot as marmot_create_key_package().
 * The MLS LeafNode uses a self-signed credential derived from the pubkey.
 * The caller must sign the Nostr event externally before publishing.
 *
 * This is the preferred API for signer-only architectures where the caller
 * delegates Nostr event signing to an external service (e.g., D-Bus signer).
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_key_package_unsigned(Marmot *m,
                                                const uint8_t nostr_pubkey[32],
                                                const char **relay_urls, size_t relay_count,
                                                MarmotKeyPackageResult *result);

/**
 * marmot_select_key_package_event:
 * @event_jsons: (array length=count): candidate KeyPackage event JSONs, e.g.
 *   the union of a kind:30443 fetch across several relays
 * @count: number of entries in @event_jsons (NULL entries are skipped)
 * @owner_pubkey: (array fixed-size=32) (nullable): when non-NULL, only
 *   events authored by this account are considered
 * @out_index: (out): index into @event_jsons of the selected event
 *
 * Choose the KeyPackage event to consume for an invite, applying the Marmot
 * Nostr transport's addressable-slot rules:
 *
 * 1. Entries that are not kind 30443, or whose NIP-01 id or signature does
 *    not verify, are ignored; unauthenticated input can never supersede a
 *    slot. Entries without exactly one single-valued `d` tag have no slot and
 *    are ignored.
 * 2. Within each `(pubkey, d)` slot the newest event by `created_at` wins;
 *    equal timestamps are broken by the lower event id.
 * 3. A slot winner that fails full KeyPackage validation empties its slot.
 *    Older events in the same slot are never resurrected, since their
 *    private init keys may already have been retired by the author.
 * 4. Among valid slot winners the newest `created_at` wins, then the lower
 *    KeyPackageRef (`i` tag, compared as bytes).
 *
 * The result can be passed directly to marmot_create_group() or
 * marmot_add_members().
 *
 * Returns: MARMOT_OK on success, MARMOT_ERR_INVALID_ARG for bad arguments,
 *   MARMOT_ERR_KEY_PACKAGE when no valid candidate remains, or
 *   MARMOT_ERR_MEMORY
 */
MarmotError marmot_select_key_package_event(const char **event_jsons,
                                             size_t count,
                                             const uint8_t owner_pubkey[32],
                                             size_t *out_index);

/* ──────────────────────────────────────────────────────────────────────────
 * KeyPackage profiles (nostrc-prqu.9) — opt-in; the functions above always
 * use MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotKeyPackageProfile:
 * @MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8: the default. The kind:30443 shape MDK
 *   0.8 emits and parses (tests/vectors/mdk/protocol-vectors.json): content
 *   is base64 of a bare KeyPackage with `encoding` = `base64`, optional
 *   `relays`, `mls_proposals` = `0x000a`; LeafNode capabilities 0x000a +
 *   0xf2ee.
 * @MARMOT_KEY_PACKAGE_PROFILE_ADOPTED: the adopted Marmot spec's strict
 *   cutover profile (marmot-protocol/marmot @26fa6a6: transports/nostr.md
 *   "KeyPackage publication", foundation/key-packages.md,
 *   app-components/account-identity-proof-v2.md). Content is base64 of an
 *   MLSMessage (wire_format mls_key_package) whose KeyPackageRef is still
 *   computed over the inner KeyPackage; no `encoding` or `relays` tags; an
 *   `app_components` id-list tag including `0x8009`; LeafNode capabilities
 *   advertise app_data_dictionary (0x0006) and app_data_update (0x0008);
 *   the LeafNode's app_data_dictionary carries app_components
 *   [0x0001, 0x8009], safe_aad [] and the 104-byte
 *   marmot.member.account-identity-proof.v2 signed by the Nostr account key
 *   over the leaf's MLS signature key; last-resort status is the empty
 *   `last_resort_key_package` (0x0004) entry of a KeyPackage-level
 *   app_data_dictionary; the Lifetime is current and spans at most
 *   7,261,200 s.
 *
 *   EXPERIMENTAL: this covers KeyPackage production, validation and
 *   selection only. libmarmot's group engine still implements the MDK 0.8
 *   group profile (0xf2ee group data, no AppDataUpdate / app-component
 *   group state), and a published ADOPTED KeyPackage would promise remote
 *   inviters that behaviour. Producing one therefore needs the build
 *   option MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER (CMake; meson
 *   `adopted_key_package_producer`), OFF by default, until the engine
 *   supports adopted-profile groups (Groundhog W2). Validation and
 *   selection are always available.
 *
 * Which kind:30443 profile to produce or accept.
 */
typedef enum {
    MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8 = 0,
    MARMOT_KEY_PACKAGE_PROFILE_ADOPTED = 1,
} MarmotKeyPackageProfile;

/**
 * MarmotAccountSignFunc:
 * @user_data: the pointer given alongside the callback
 * @unsigned_event_json: an unsigned NIP-01 event (kind 450, the
 *   account-identity-proof v2 signing template). It is a local-only
 *   template and MUST NOT be published.
 * @out_signed_event_json: (out) (transfer full): the same event signed by
 *   the account key (malloc()ed; libmarmot free()s it), e.g. the reply of
 *   org.nostr.Signer.SignEvent
 *
 * Signs with the Nostr account key for callers that do not hold the secret
 * key. libmarmot verifies the returned id, signature, pubkey and that every
 * signed field equals the template.
 *
 * Returns: 0 on success, non-zero if the signature was refused or failed
 */
typedef int (*MarmotAccountSignFunc)(void *user_data,
                                     const char *unsigned_event_json,
                                     char **out_signed_event_json);

/**
 * marmot_create_key_package_for_profile:
 * @m: Marmot instance
 * @profile: the kind:30443 profile to produce
 * @nostr_pubkey: (array fixed-size=32): the account's Nostr public key
 * @nostr_sk: (array fixed-size=32) (nullable): the account secret key. When
 *   given, the kind:30443 event is signed (as marmot_create_key_package());
 *   when NULL it is left unsigned (as marmot_create_key_package_unsigned()).
 * @account_sign: (scope call) (nullable): signs the ADOPTED profile's
 *   account-identity proof when @nostr_sk is NULL; called synchronously,
 *   at most once, before this function returns. Unused for MDK_0_8.
 * @sign_data: (closure account_sign): user data for @account_sign
 * @relay_urls: (array length=relay_count) (nullable): stored with the
 *   KeyPackage; also emitted as the `relays` tag in the MDK_0_8 profile only
 * @relay_count: number of relay URLs
 * @result: (out): the kind:30443 event JSON and KeyPackageRef
 *
 * marmot_create_key_package() with a selectable profile. Storage, the
 * stable `d` publication slot and rotation behave identically for both
 * profiles; MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8 produces exactly what
 * marmot_create_key_package() does.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_UNSUPPORTED for ADOPTED unless libmarmot
 *   was built with MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER;
 *   MARMOT_ERR_INVALID_ARG for an ADOPTED request with
 *   neither @nostr_sk nor @account_sign; MARMOT_ERR_CRYPTO or
 *   MARMOT_ERR_VALIDATION when the account signature fails or does not
 *   verify; other errors as marmot_create_key_package()
 */
MarmotError marmot_create_key_package_for_profile(Marmot *m,
                                                   MarmotKeyPackageProfile profile,
                                                   const uint8_t nostr_pubkey[32],
                                                   const uint8_t nostr_sk[32],
                                                   MarmotAccountSignFunc account_sign,
                                                   void *sign_data,
                                                   const char **relay_urls,
                                                   size_t relay_count,
                                                   MarmotKeyPackageResult *result);

/**
 * marmot_validate_key_package_event_json:
 * @event_json: a signed kind:30443 event
 * @profile: the profile the event must satisfy
 * @now: validation time (Unix seconds) for the ADOPTED Lifetime check; 0
 *   means the current time. Ignored by MDK_0_8.
 * @owner_out: (array fixed-size=32) (out) (optional): the account pubkey
 * @ref_out: (array fixed-size=32) (out) (optional): the KeyPackageRef
 *
 * Full receiver-side validation of one KeyPackage event under @profile: NIP-01
 * id and signature, the profile's tag rules, content framing, the decoded
 * KeyPackage's signatures, author binding and KeyPackageRef, and for
 * ADOPTED also the Lifetime bounds, required capabilities, app_components
 * support and the account-identity proof.
 *
 * Returns: MARMOT_OK if valid, else the first failure (MARMOT_ERR_VALIDATION,
 *   MARMOT_ERR_DESERIALIZATION, MARMOT_ERR_MLS, MARMOT_ERR_AUTHOR_MISMATCH,
 *   MARMOT_ERR_UNEXPECTED_EVENT, ...)
 */
MarmotError marmot_validate_key_package_event_json(const char *event_json,
                                                    MarmotKeyPackageProfile profile,
                                                    int64_t now,
                                                    uint8_t owner_out[32],
                                                    uint8_t ref_out[32]);

/**
 * marmot_select_key_package_event_for_profile:
 * @event_jsons: (array length=count): candidate kind:30443 event JSONs
 * @count: number of entries
 * @owner_pubkey: (array fixed-size=32) (nullable): only consider this author
 * @profile: the profile a candidate must satisfy
 * @out_index: (out): index of the selected event
 *
 * marmot_select_key_package_event() (same slot and ranking rules) with the
 * candidate validation of @profile.
 *
 * Returns: as marmot_select_key_package_event()
 */
MarmotError marmot_select_key_package_event_for_profile(const char **event_jsons,
                                                         size_t count,
                                                         const uint8_t owner_pubkey[32],
                                                         MarmotKeyPackageProfile profile,
                                                         size_t *out_index);

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Construction
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * marmot_create_group:
 * @m: Marmot instance
 * @creator_pubkey: (array fixed-size=32): creator's Nostr public key (32 bytes)
 * @key_package_event_jsons: (array length=kp_count): JSON strings of signed
 *   kind:30443 events (see marmot_select_key_package_event() for choosing one
 *   per invitee from a relay fetch)
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
 * @key_package_event_jsons: (array length=kp_count): JSON strings of signed
 *   kind:30443 events
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

/**
 * marmot_update_group_metadata:
 * @m: Marmot instance
 * @mls_group_id: the group to update
 * @config: new group configuration (non-NULL fields are applied)
 *
 * Update group metadata (name, description, admins, relays).
 * Only admins can update group metadata. The update modifies the
 * GroupData extension in the MLS GroupContext and creates a new
 * Commit (self-update) to announce the change.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_update_group_metadata(Marmot *m,
                                          const MarmotGroupId *mls_group_id,
                                          const MarmotGroupConfig *config);

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
 * marmot_accept_welcome_by_wrapper_id:
 * @m: Marmot instance
 * @wrapper_event_id: (array fixed-size=32): the gift-wrap event ID
 * @out_group: (out) (transfer full) (nullable): accepted group metadata
 *
 * Accept a pending welcome by wrapper event ID and optionally return the
 * accepted group. This avoids callers reconstructing partial MarmotWelcome
 * records just to carry the wrapper ID.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_accept_welcome_by_wrapper_id(Marmot *m,
                                                 const uint8_t wrapper_event_id[32],
                                                 MarmotGroup **out_group);

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
 * Create an encrypted group message. The inner event is first framed as an
 * MLS PrivateMessage, then encrypted using NIP-44 with the MLS exporter secret
 * as the conversation key.
 *
 * MLS group state is required by default. Setting
 * MarmotConfig.allow_legacy_raw_messages permits the legacy pre-framing path
 * that NIP-44-encrypts the raw inner JSON only when MLS state is unavailable.
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
 * marmot_save_created_message:
 * @m: Marmot instance
 * @mls_group_id: the group the message was sent to
 * @signed_group_event_json: signed outer kind:445 event JSON with real id
 * @inner_event_json: plaintext inner event JSON saved as local content
 *
 * Persist an outgoing message after the caller has filled the ephemeral pubkey
 * and signature for the kind:445 event. marmot_create_message() intentionally
 * does not persist CREATED rows because unsigned events have no stable Nostr
 * event ID.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_save_created_message(Marmot *m,
                                         const MarmotGroupId *mls_group_id,
                                         const char *signed_group_event_json,
                                         const char *inner_event_json);

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
 * MIP-03 messages require MLS PrivateMessage framing by default. The legacy
 * raw-JSON NIP-44 fallback is accepted only when
 * MarmotConfig.allow_legacy_raw_messages is enabled.
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
 * marmot_get_group_relay_urls:
 * @m: Marmot instance
 * @mls_group_id: group to query
 * @out_relays: (out) (array length=out_count) (transfer full): relay records
 * @out_count: (out): number of relays
 *
 * Returns group relay URLs through the Marmot handle without exposing storage.
 */
MarmotError marmot_get_group_relay_urls(Marmot *m,
                                         const MarmotGroupId *mls_group_id,
                                         MarmotGroupRelay **out_relays,
                                         size_t *out_count);

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
