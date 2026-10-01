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
#include "marmot-media.h"
#include "marmot-group-profile.h"
#include "marmot-group-components.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * Media Encryption (MIP-04)
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Deprecated: libmarmot's pre-0.12 media format (HMAC "marmot-media-key",
 * MIME-only AAD) matches neither encrypted-media-v2 nor the frozen v1, so no
 * other Marmot client can read it.  It is never produced any more: this
 * returns MARMOT_ERR_MEDIA_LEGACY_FORMAT and leaves *result empty.  Use
 * marmot_media_encrypt() (marmot-media.h).
 */
MarmotError marmot_encrypt_media(Marmot *m,
                                  const MarmotGroupId *mls_group_id,
                                  const uint8_t *file_data, size_t file_len,
                                  const char *mime_type,
                                  const char *filename,
                                  MarmotEncryptedMedia *result);

/**
 * Deprecated, read-only: decrypts a reference in libmarmot's pre-0.12 media
 * format that is already stored locally.  New media is marmot_media_decrypt().
 * imeta->file_hash (the plaintext SHA-256) is required; an all-zero hash is
 * MARMOT_ERR_MEDIA_INVALID_REFERENCE.  The epoch is the caller's: never take
 * it from a received tag (the old format let the sender choose it).
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
 * Transactions (since 0.7.0): when the storage has the begin/commit/
 * rollback hooks (marmot-storage.h), every operation below that writes is
 * one storage transaction, all or nothing.  A storage with only some of
 * them is refused: NULL is returned and @storage is not taken.
 *
 * Thread safety: a Marmot instance, and the storage it owns, is NOT
 * thread-safe.  Calls on one instance must be serialized by the caller
 * (e.g. one mutex around every call, as marmot-gobject does per client):
 * Commit processing, merges and message handling are multi-step
 * read-modify-write transitions of the same group records.  Separate
 * instances with separate storage are independent.
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
 * Since 0.10.0 the LeafNode also carries marmot.member.account-identity-proof.v2
 * (a LeafNode app_data_dictionary; `mls_extensions` adds `0x0006`), signed
 * with @nostr_sk over the leaf's MLS signature key. Other members accept the
 * leaf only with it (nostrc-7vyi). With @nostr_sk at hand this call also
 * enrolls the instance once, as marmot_set_account_proof() would.
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
 * The LeafNode needs an account-identity proof (nostrc-7vyi), which this
 * call cannot sign: enroll the instance first (marmot_account_proof_template(),
 * sign, marmot_set_account_proof()). The KeyPackage then uses the enrolled
 * MLS signature key and its proof -- the same key for every such KeyPackage
 * of this instance, and for the groups it creates.  Signature keys must be
 * unique within a group (RFC 9420 section 7.3): a second such KeyPackage
 * cannot join a group that already holds the key (marmot_add_members()
 * refuses it).
 *
 * Returns: MARMOT_OK on success; MARMOT_ERR_KEY_PACKAGE_IDENTITY when the
 *   instance holds no account proof for @nostr_pubkey (unless
 *   MarmotConfig.allow_unproven_self, which yields a legacy KeyPackage
 *   without one)
 */
MarmotError marmot_create_key_package_unsigned(Marmot *m,
                                                const uint8_t nostr_pubkey[32],
                                                const char **relay_urls, size_t relay_count,
                                                MarmotKeyPackageResult *result);

/**
 * marmot_account_proof_template:
 * @m: Marmot instance
 * @account_pubkey: (array fixed-size=32): the account's Nostr public key
 * @out_unsigned_event_json: (out) (transfer full): the unsigned kind:450
 *   signing template (free() it)
 *
 * Enrollment, step 1 (nostrc-7vyi). A Marmot member leaf must carry
 * marmot.member.account-identity-proof.v2: the account's signature over the
 * leaf's MLS signature key (app-components/account-identity-proof-v2.md).
 * This returns the proof's signing template for this instance's MLS
 * signature key, created now. Sign it with the account key (e.g.
 * org.nostr.Signer.SignEvent) and pass the signed event to
 * marmot_set_account_proof(). The template is local-only: never publish it.
 *
 * The instance key is generated per Marmot instance and not stored, so
 * enroll again after every marmot_new().
 *
 * Returns: MARMOT_OK; MARMOT_ERR_INVALID_ARG; MARMOT_ERR_CRYPTO or
 *   MARMOT_ERR_MEMORY
 */
MarmotError marmot_account_proof_template(Marmot *m,
                                           const uint8_t account_pubkey[32],
                                           char **out_unsigned_event_json);

/**
 * marmot_set_account_proof:
 * @m: Marmot instance
 * @account_pubkey: (array fixed-size=32): the account's Nostr public key
 * @signed_event_json: the template from marmot_account_proof_template(),
 *   signed by @account_pubkey
 *
 * Enrollment, step 2 (nostrc-7vyi). Checks that @signed_event_json is
 * exactly a proof signing template for this instance's MLS signature key
 * (any created_at), with a valid id and signature by @account_pubkey, and
 * keeps the proof in memory. From then on:
 * - marmot_create_group() for @account_pubkey starts the group with a
 *   proven creator leaf, so any admin can later admit members;
 * - marmot_create_key_package_unsigned() for @account_pubkey produces
 *   KeyPackages that carry it.
 * One account per instance: a later call replaces the proof.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_VALIDATION when the event is not such a
 *   template or its signature does not verify; MARMOT_ERR_INVALID_ARG
 */
MarmotError marmot_set_account_proof(Marmot *m,
                                      const uint8_t account_pubkey[32],
                                      const char *signed_event_json);

/**
 * marmot_has_account_proof:
 * @m: Marmot instance
 * @account_pubkey: (array fixed-size=32): the account's Nostr public key
 *
 * Returns: whether this instance holds an account proof for @account_pubkey
 *   (marmot_set_account_proof(), or marmot_create_key_package() with the key)
 */
bool marmot_has_account_proof(Marmot *m, const uint8_t account_pubkey[32]);

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

/**
 * marmot_key_package_event_has_account_proof:
 * @event_json: a signed kind:30443 event (the MDK_0_8 profile, e.g. the
 *   event marmot_select_key_package_event() chose)
 * @out_proven: (out): whether its LeafNode carries a verified
 *   marmot.member.account-identity-proof.v2 for its author
 *
 * Tells, before any Commit, whether this KeyPackage can be invited outside
 * legacy mode: since 0.10.0 marmot_create_group() and marmot_add_members()
 * refuse a KeyPackage whose leaf has no proof (MDK 0.8, libmarmot 0.9.0 and
 * older) with MARMOT_ERR_KEY_PACKAGE_IDENTITY. Validation is that of
 * marmot_select_key_package_event() (id, signature, tags, KeyPackage,
 * author binding, KeyPackageRef); a proof that is present but does not
 * verify fails it. Nothing is stored.
 *
 * Returns: MARMOT_OK (with @out_proven set), MARMOT_ERR_INVALID_ARG, or the
 *   KeyPackage's validation failure (e.g. MARMOT_ERR_VALIDATION,
 *   MARMOT_ERR_KEY_PACKAGE_IDENTITY for a proof that does not verify)
 */
MarmotError marmot_key_package_event_has_account_proof(const char *event_json,
                                                        bool *out_proven);

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
 *   `app_components` id-list tag listing exactly the leaf's private-use
 *   (>= 0x8000) components, including `0x8009`; LeafNode capabilities
 *   advertise app_data_dictionary (0x0006) and app_data_update (0x0008);
 *   the LeafNode's app_data_dictionary carries app_components
 *   [0x0001, 0x8001, 0x8003, 0x8004, 0x8009, 0x800c] (since 0.12.0: the
 *   adopted components libmarmot supports, which an MDK 0.11 inviter
 *   requires), safe_aad [] and the 104-byte
 *   marmot.member.account-identity-proof.v2 signed by the Nostr account key
 *   over the leaf's MLS signature key; last-resort status is the empty
 *   `last_resort_key_package` (0x0004) entry of a KeyPackage-level
 *   app_data_dictionary; the Lifetime is current and spans at most
 *   7,261,200 s.
 *
 *   EXPERIMENTAL: since 0.12.0 libmarmot admits and creates adopted-profile
 *   groups (marmot_create_group_for_profile()), but cannot yet process
 *   their Commits (AppDataUpdate, membership changes), and a published
 *   ADOPTED KeyPackage would promise remote inviters that it can.
 *   Producing one therefore needs the build option
 *   MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER (CMake; meson
 *   `adopted_key_package_producer`), OFF by default, until it does
 *   (Groundhog W2). Validation and selection are always available.
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
 * @account_sign: (scope call) (nullable): signs the account-identity proof
 *   when @nostr_sk is NULL; called synchronously, at most once, before this
 *   function returns. Since 0.10.0 also used for MDK_0_8, whose leaf carries
 *   the proof too (without either, MDK_0_8 falls back to the enrolled
 *   instance key as marmot_create_key_package_unsigned() does).
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
 * All invitees are added by one Commit and share one Welcome (each rumor
 * names that invitee's KeyPackage event), so every invitee joins at the
 * same epoch.  The group state is stored immediately: nobody but the
 * joiners can see this Commit.
 *
 * After creating a group, the caller should:
 * 1. Call marmot_merge_pending_commit() (records the confirmation)
 * 2. Gift-wrap each welcome rumor (NIP-59) and send to the member
 * 3. Publish the evolution event (signed kind:445) to group relays
 *
 * Account binding (nostrc-7vyi): the creator's leaf carries the instance's
 * account proof, so the instance must be enrolled for @creator_pubkey
 * (marmot_set_account_proof()), unless MarmotConfig.allow_unproven_self.
 * An invitee's KeyPackage leaf may lack a proof only while
 * MarmotConfig.allow_unproven_members (the default; new groups are of the
 * legacy profile); a proof that does not verify is always refused.
 * Otherwise MARMOT_ERR_KEY_PACKAGE_IDENTITY and nothing is created.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_create_group(Marmot *m,
                                 const uint8_t creator_pubkey[32],
                                 const char **key_package_event_jsons, size_t kp_count,
                                 const MarmotGroupConfig *config,
                                 MarmotCreateGroupResult *result);

/**
 * marmot_create_group_for_profile:
 * @m: Marmot instance
 * @profile: the wire profile of the new group
 * @creator_pubkey: (array fixed-size=32): creator's Nostr public key
 * @creator_sk: (array fixed-size=32) (nullable): the creator's account
 *   secret key, used only to sign the creator leaf's account proof
 * @account_sign: (scope call) (nullable): signs the creator leaf's account
 *   proof when @creator_sk is NULL (see #MarmotAccountSignFunc); called
 *   synchronously, at most once
 * @sign_data: (closure account_sign): user data for @account_sign
 * @key_package_event_jsons: (array length=kp_count): signed kind:30443
 *   events, one per invitee
 * @kp_count: number of invitees
 * @config: name, description, extra admins and relays
 * @result: (out): the group, one welcome rumor per invitee, the evolution
 *   event
 *
 * marmot_create_group() with a selectable profile.
 *
 * %MARMOT_GROUP_PROFILE_LEGACY is exactly marmot_create_group(); @creator_sk
 * and @account_sign must be NULL (the creator leaf carries the enrolled
 * proof).
 *
 * %MARMOT_GROUP_PROFILE_ADOPTED creates a group of the adopted Marmot
 * specification (MDK 0.11; protocol-core/group-setup.md) -- since 0.12.0:
 * - GroupContext: required_capabilities {extensions [0x0006], proposals
 *   [0x0008]} and an app_data_dictionary whose app_components require
 *   0x8001 (profile: @config name and description), 0x8003 (admin policy:
 *   the creator plus @config admins, each of whom must be the creator or
 *   an invitee), 0x8004 (Nostr routing: a random nostr_group_id and the
 *   @config relays, 1 to 16 ws/wss URLs, sorted and deduplicated), 0x8009
 *   and 0x800c (lifecycle: active).
 * - Creator leaf: capabilities [0x0006] / [0x0008] and the account proof
 *   (marmot.member.account-identity-proof.v2) over this instance's MLS
 *   signature key, signed now by @creator_sk or through @account_sign;
 *   with neither, the proof this instance was enrolled with for
 *   @creator_pubkey (marmot_set_account_proof()), else
 *   %MARMOT_ERR_KEY_PACKAGE_IDENTITY.
 * - Invitees: KeyPackages valid under %MARMOT_KEY_PACKAGE_PROFILE_ADOPTED
 *   whose leaves advertise every required component and capability
 *   (%MARMOT_ERR_KEY_PACKAGE otherwise).
 * - Welcome rumors follow the adopted Nostr binding: content base64
 *   MLSMessage(mls_welcome), exactly one `e` (KeyPackage event id) and one
 *   `relays` tag, no `encoding` tag and no cleartext group preview.
 * The group is stored like marmot_create_group()'s; its profile is
 * persisted (marmot_get_group_profile()).
 *
 * Adopted groups are admitted, stored and loaded, and can exchange
 * application messages; libmarmot does not yet process or produce Commits
 * in them (AppDataUpdate, adds, removals, self-updates): those fail with
 * %MARMOT_ERR_UNSUPPORTED and leave the group unchanged -- also a Commit
 * that removes our own leaf: the removed member is not told and stays
 * active, its keys kept.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_INVALID_ARG (bad arguments or config);
 *   MARMOT_ERR_KEY_PACKAGE_IDENTITY; MARMOT_ERR_KEY_PACKAGE; KeyPackage
 *   validation errors; storage errors
 */
MarmotError marmot_create_group_for_profile(Marmot *m,
                                             MarmotGroupProfile profile,
                                             const uint8_t creator_pubkey[32],
                                             const uint8_t creator_sk[32],
                                             MarmotAccountSignFunc account_sign,
                                             void *sign_data,
                                             const char **key_package_event_jsons,
                                             size_t kp_count,
                                             const MarmotGroupConfig *config,
                                             MarmotCreateGroupResult *result);

/**
 * marmot_get_group_profile:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_profile: (out): its wire profile
 *
 * The profile a stored group was created or joined under (persisted with
 * its MLS state; groups stored before 0.12.0 are
 * %MARMOT_GROUP_PROFILE_LEGACY).  Since 0.12.0.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_GROUP_NOT_FOUND; MARMOT_ERR_DESERIALIZATION
 *   for a stored state that no longer validates
 */
MarmotError marmot_get_group_profile(Marmot *m,
                                      const MarmotGroupId *mls_group_id,
                                      MarmotGroupProfile *out_profile);

/**
 * marmot_merge_pending_commit:
 * @m: Marmot instance
 * @mls_group_id: the group to merge
 *
 * Apply the group's pending Commit, made by marmot_add_members(),
 * marmot_remove_members() or marmot_update_group_metadata(), once at least
 * one group relay accepted its kind:445 event (NIP-01 OK).  Until then the
 * group stays in its current epoch (MIP-03: a Commit is not applied before a
 * relay confirms it) and no other Commit can be made.  Send the Welcomes of
 * an Add only after this succeeds.  Without a pending Commit (e.g. after
 * marmot_create_group()) this only records the confirmation.
 *
 * The pending Commit is bound to the exact state it was built on: if a
 * competing Commit replaced that state -- even with another of the same
 * epoch -- the pending one can no longer be merged.  An Add's Welcomes move
 * to the unsent-Welcome outbox on merge (marmot_get_unsent_welcomes()).
 *
 * Returns: MARMOT_OK on success (also when the Commit was already applied,
 *   e.g. by its relay echo or before a crash); MARMOT_ERR_WRONG_EPOCH when a
 *   competing member's Commit won while ours was pending -- ours is
 *   discarded and the group follows the winner (do not send its Welcomes);
 *   an authorization error when the Commit can never apply (it is
 *   discarded); a storage error leaves it pending (merge again or clear)
 */
MarmotError marmot_merge_pending_commit(Marmot *m,
                                         const MarmotGroupId *mls_group_id);

/**
 * marmot_clear_pending_commit:
 * @m: Marmot instance
 * @mls_group_id: the group
 *
 * Discard the group's pending Commit when no relay accepted it, leaving the
 * group in its current epoch.  Commits from other members that arrived while
 * ours was pending and lost to it (marmot_process_message() returned
 * MARMOT_ERR_OWN_COMMIT_PENDING) are processed now.
 *
 * Returns: MARMOT_OK (also when nothing was pending)
 */
MarmotError marmot_clear_pending_commit(Marmot *m,
                                         const MarmotGroupId *mls_group_id);

/**
 * marmot_get_pending_commit:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_event_json: (out) (transfer full) (nullable): the pending Commit's
 *   signed kind:445 event, or NULL when nothing is pending
 * @out_superseded: (out) (optional): TRUE when a competing Commit replaced
 *   the state the pending one was built on (merging will return
 *   MARMOT_ERR_WRONG_EPOCH)
 *
 * Restart path for publish-before-merge: after a crash, or when a relay's
 * answer was lost, republish @out_event_json and merge on the first relay OK.
 * Receiving our own pending Commit back from a relay also merges it
 * (marmot_process_message() returns MARMOT_RESULT_COMMIT): the relay stored
 * it, so it is published.  Free @out_event_json with free().
 *
 * Returns: MARMOT_OK (also when nothing is pending)
 */
MarmotError marmot_get_pending_commit(Marmot *m,
                                       const MarmotGroupId *mls_group_id,
                                       char **out_event_json,
                                       bool *out_superseded);

/**
 * MarmotUnsentWelcome:
 * @id: stable identifier of this Welcome (for marmot_mark_welcomes_sent())
 * @recipient: the invitee's Nostr account key (gift-wrap recipient)
 * @rumor_json: the unsigned kind:444 Welcome rumor to gift-wrap (NIP-59)
 *
 * A Welcome of a merged Add that has not been confirmed as sent.
 */
typedef struct {
    uint8_t id[32];          /* SHA-256(recipient || rumor_json): stable */
    uint8_t recipient[32];
    char   *rumor_json;
} MarmotUnsentWelcome;

/** Free an array returned by marmot_get_unsent_welcomes(). */
void marmot_unsent_welcomes_free(MarmotUnsentWelcome *welcomes, size_t count);

/**
 * marmot_get_unsent_welcomes:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_welcomes: (out) (array length=out_count) (transfer full): the Welcomes
 *   of this group's merged Adds not yet marked sent (NULL when none)
 * @out_count: (out): number of Welcomes
 *
 * When a pending Add Commit is merged -- by marmot_merge_pending_commit(),
 * by its relay echo, or after a restart -- its Welcomes are appended here
 * (Welcomes of earlier Adds stay until marked sent).  Gift-wrap and send
 * each to its recipient, and once that send is confirmed mark that entry
 * with marmot_mark_welcomes_sent().
 * (The rumors marmot_add_members() returns are the same Welcomes; send one
 * copy only.)
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_get_unsent_welcomes(Marmot *m,
                                        const MarmotGroupId *mls_group_id,
                                        MarmotUnsentWelcome **out_welcomes,
                                        size_t *out_count);

/**
 * marmot_mark_welcomes_sent:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @ids: (array length=count): ids of Welcomes whose send was confirmed
 * @count: number of ids
 *
 * Remove exactly those Welcomes from the group's outbox; others -- including
 * Welcomes appended after they were read -- stay.  Unknown ids are ignored.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_mark_welcomes_sent(Marmot *m, const MarmotGroupId *mls_group_id,
                                       const uint8_t (*ids)[32], size_t count);

/**
 * marmot_group_account_proof_template:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_unsigned_event_json: (out) (transfer full): the unsigned kind:450
 *   signing template (free() it)
 *
 * The account-identity proof template for OUR leaf in this group: our
 * account and this group's leaf signature key (nostrc-rgb5).  For a leaf
 * that has none (made by libmarmot 0.9.0 or older, or in legacy mode): sign
 * it with the account key and pass it to marmot_self_update().  Local-only:
 * never publish it.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_GROUP_NOT_FOUND; MARMOT_ERR_MEMORY
 */
MarmotError marmot_group_account_proof_template(Marmot *m,
                                                const MarmotGroupId *mls_group_id,
                                                char **out_unsigned_event_json);

/**
 * marmot_self_update:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @signed_proof_json: (nullable): the template of
 *   marmot_group_account_proof_template(), signed by our account; NULL keeps
 *   our leaf's extensions as they are
 * @out_commit_json: (out) (transfer full): the signed kind:445 Commit event
 *
 * An ordinary Commit (any member, no admin needed) whose UpdatePath gives
 * us a new leaf encryption key and new path keys (RFC 9420 post-compromise
 * security; nostrc-yd0q).  With @signed_proof_json the new leaf also carries
 * our account-identity proof: this is how a leaf from before 0.10.0 becomes
 * proven (nostrc-rgb5), after which joiners accept it in anyone's Welcome.
 * Pending like every Commit: publish it, then marmot_merge_pending_commit()
 * or marmot_clear_pending_commit().
 *
 * Returns: MARMOT_OK; MARMOT_ERR_VALIDATION when @signed_proof_json is not
 *   the template for our leaf signed by our account;
 *   MARMOT_ERR_OWN_COMMIT_PENDING; other errors as marmot_add_members()
 */
MarmotError marmot_self_update(Marmot *m,
                               const MarmotGroupId *mls_group_id,
                               const char *signed_proof_json,
                               char **out_commit_json);

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
 * All KeyPackages are added by one Commit with one Welcome (nostrc-wc6v).
 * @out_commit_json is a kind:445 event carrying the Commit, NIP-44-encrypted
 * with the exporter secret of the current epoch (as application messages
 * are) and signed by a fresh ephemeral key.  The Commit is pending: publish
 * the event to the group relays, then call marmot_merge_pending_commit() once
 * one accepted it (and only then send the Welcomes), or
 * marmot_clear_pending_commit() if none did.  MARMOT_ERR_OWN_COMMIT_PENDING
 * while another Commit of ours is pending.  A KeyPackage leaf, and every
 * other current member's leaf, may lack the account-identity proof only in a
 * legacy-profile group while MarmotConfig.allow_unproven_members (the
 * default; MarmotGroupProfile).  Otherwise each must carry a valid one,
 * since every member checks it and a joiner that requires proofs accepts no
 * unproven leaf but ours (we send the Welcome).  A proof that does not
 * verify is always refused.  On refusal MARMOT_ERR_KEY_PACKAGE_IDENTITY and
 * nothing changes: no Add whose Welcome the joiner must reject is ever
 * published (nostrc-7vyi; see marmot_self_update() to prove existing
 * leaves).
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
 * @out_commit_json: (out) (transfer full): commit event JSON, as for
 *   marmot_add_members() (pending until merged)
 *
 * All members are removed by one Commit (nostrc-wc6v).
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
 * Leave a group on this device only: the group state is set to Inactive
 * locally and nothing is sent, so the other members keep counting us until
 * a Commit removes our leaf.  To leave for everyone, use
 * marmot_self_remove() where the group supports it.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_leave_group(Marmot *m,
                                const MarmotGroupId *mls_group_id);

/**
 * marmot_self_remove:
 * @m: Marmot instance
 * @mls_group_id: the group to leave
 * @out_event_json: (out) (transfer full): the signed kind:445 event carrying
 *   our leave proposal, to publish to the group relays
 *
 * Leave a group for everyone (since 0.12.0, nostrc-2um6; MIP-03 "Leaving a
 * group", Marmot protocol-core/member-departure.md).  Makes our leave
 * proposal for the current epoch, sealed like every kind:445, and records a
 * durable leave request: the group is Leaving.  As MDK 0.8 does, it is a
 * SelfRemove (draft-ietf-mls-extensions, 0x000a, an MLS PublicMessage that
 * any other member commits) when the group's required_capabilities list
 * SelfRemove, and otherwise a Remove of our own leaf (a PrivateMessage that
 * an admin commits; marmot_can_self_remove() says which).  A member cannot
 * commit its own leave: another member commits it by reference, and when
 * that Commit arrives marmot_process_message() ends the group for us
 * (MARMOT_RESULT_COMMIT, the group inactive, marmot_get_group_left() TRUE).
 *
 * While Leaving, nothing else may be sent: marmot_create_message() and every
 * Commit producer return MARMOT_ERR_LEAVING.  Call it again after every
 * Commit that keeps us (a proposal is bound to its epoch): it returns the
 * same bytes within an epoch and a fresh proposal for a new one, so it is
 * also what a restart republishes.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_UNSUPPORTED when the group requires
 *   SelfRemove yet a member does not support it, or for an adopted-profile
 *   group, which processes no Commits yet (leave with marmot_leave_group()
 *   then);
 *   MARMOT_ERR_ADMIN_CANNOT_LEAVE for an admin (step down first, with
 *   marmot_update_group_metadata()); MARMOT_ERR_OWN_COMMIT_PENDING while a
 *   Commit of ours awaits a relay; MARMOT_ERR_USE_AFTER_EVICTION for a group
 *   that is not active
 */
MarmotError marmot_self_remove(Marmot *m, const MarmotGroupId *mls_group_id,
                               char **out_event_json);

/**
 * marmot_can_self_remove:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_kind: (out) (optional): how marmot_self_remove() would leave
 *
 * What marmot_self_remove() would return now, without making or storing
 * anything (since 0.12.0): lets an application say before the user
 * confirms whether leaving reaches everyone, through whom, or only this
 * device.
 *
 * Returns: MARMOT_OK (also when already Leaving); otherwise the error
 *   marmot_self_remove() would return
 */
MarmotError marmot_can_self_remove(Marmot *m, const MarmotGroupId *mls_group_id,
                                   MarmotLeaveKind *out_kind);

/**
 * marmot_cancel_leave:
 * @m: Marmot instance
 * @mls_group_id: the group
 *
 * Drop our leave request (since 0.12.0): the group is no longer Leaving and
 * sends are allowed again.  A leave proposal already published may still be
 * committed by another member in its epoch.  For an application that cannot
 * keep leaving, e.g. when a new epoch's proposal can no longer be made
 * (the account became an admin).
 *
 * Returns: MARMOT_OK (also when not Leaving)
 */
MarmotError marmot_cancel_leave(Marmot *m, const MarmotGroupId *mls_group_id);

/**
 * marmot_is_leaving:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_leaving: (out): TRUE while our leave request (marmot_self_remove())
 *   waits for the Commit that removes us
 *
 * Returns: MARMOT_OK
 */
MarmotError marmot_is_leaving(Marmot *m, const MarmotGroupId *mls_group_id, bool *out_leaving);

/**
 * marmot_get_pending_proposals:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_proposals: (out) (array length=out_count) (transfer full): free with
 *   marmot_pending_proposals_free()
 * @out_count: (out): number of proposals
 *
 * The standalone Proposals of the current epoch no Commit consumed yet
 * (since 0.12.0): members' SelfRemoves (their leaves), a Remove a member
 * sent for itself (how MDK 0.8 leaves a group that does not require
 * SelfRemove), and other members' proposals kept for an admin's Commit.
 * marmot_process_message() keeps them (MARMOT_RESULT_PROPOSAL); a Commit
 * that moves the group to another epoch makes them stale.
 *
 * Returns: MARMOT_OK
 */
MarmotError marmot_get_pending_proposals(Marmot *m, const MarmotGroupId *mls_group_id,
                                         MarmotPendingProposal **out_proposals,
                                         size_t *out_count);

/** Free an array returned by marmot_get_pending_proposals(). */
void marmot_pending_proposals_free(MarmotPendingProposal *proposals);

/**
 * marmot_commit_pending_proposals:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_commit_json: (out) (transfer full) (nullable): the kind:445 Commit
 *   event to publish, NULL when there is nothing to commit
 *
 * Commit, by reference, the departures other members asked for in the
 * current epoch (since 0.12.0): every valid SelfRemove -- any member may
 * commit those, admin or not (MDK 0.8 and 0.11 auto-commit them;
 * member-departure.md "SelfRemove commits") -- and, when we are an admin, a
 * Remove a member sent for itself.  One proposal per leaving member: the one
 * whose serialized MLSMessage has the lowest SHA-256.  A SelfRemove from an
 * admin is never committed (MIP-03).  The Commit is pending like every
 * Commit: publish it, then marmot_merge_pending_commit() or
 * marmot_clear_pending_commit().
 *
 * Returns: MARMOT_OK (also with nothing to commit);
 *   MARMOT_ERR_OWN_COMMIT_PENDING; MARMOT_ERR_LEAVING;
 *   MARMOT_ERR_USE_AFTER_EVICTION for a group that is not active;
 *   MARMOT_ERR_UNSUPPORTED for an adopted-profile group (no Commits yet;
 *   its standalone proposals are not kept either)
 */
MarmotError marmot_commit_pending_proposals(Marmot *m, const MarmotGroupId *mls_group_id,
                                            char **out_commit_json);

/**
 * marmot_get_group_left:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_left: (out): TRUE when the Commit that removed our leaf (see
 *   marmot_get_group_removal()) committed our own SelfRemove: we left
 *
 * Since 0.12.0.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_DESERIALIZATION for a removal record that
 *   does not parse
 */
MarmotError marmot_get_group_left(Marmot *m, const MarmotGroupId *mls_group_id, bool *out_left);

/**
 * marmot_get_group_removal:
 * @m: Marmot instance
 * @mls_group_id: the group
 * @out_removed: (out): TRUE when an admin's Commit removed our own leaf
 * @out_remover: (out) (optional): the account (32-byte key) that committed it
 * @out_epoch: (out) (optional): the epoch that Commit left
 * @out_final: (out) (optional): TRUE once no competing Commit can undo it
 *
 * A member removed by an admin cannot enter the next epoch: the Commit's
 * UpdatePath is encrypted to the remaining members only.  When
 * marmot_process_message() receives an authenticated Commit (framing,
 * committer signature, membership tag) from an admin of the current
 * GroupData that removes our leaf, and no pending Commit of ours wins the
 * epoch (nor, for a competing Commit of the previous epoch, the Commit we
 * applied), it returns MARMOT_RESULT_COMMIT, the group turns inactive as
 * after marmot_leave_group(), our pending Commit is dropped and the removal
 * is kept here (nostrc-xrya).
 *
 * The removal is judged by the Commit ordering, not by arrival: while
 * another admin whose key sorts below the remover's could still publish a
 * winning Commit of that epoch (@out_final FALSE), marmot_process_message()
 * still judges that epoch's Commits of the inactive group -- everything
 * else is MARMOT_ERR_USE_AFTER_EVICTION.  A Commit that beats the removal
 * and keeps our leaf re-activates the group in its epoch (the removal is
 * forgotten; the result is MARMOT_RESULT_COMMIT, the group active); a
 * removal that beats it replaces it.  The removal also becomes final once
 * MARMOT_REMOVAL_FINAL_AFTER distinct kind:445 events of the group arrived
 * that none of our epochs' secrets opens (W22 review B2); a Commit that
 * could still beat the removal opens and is never counted itself.  A
 * winning Commit that reaches us only after MARMOT_REMOVAL_FINAL_AFTER such
 * events -- which may be the winner's own later messages, or junk anyone
 * can post with the group's h -- is refused: we then stay ended while the
 * group keeps our leaf (review B3; an application processing each relay's
 * stored answer oldest first narrows this).  Once final, the removed
 * epoch's secrets are deleted.  A later Welcome into the same group
 * clears it.
 * Without a removal (never removed, or left), *out_removed is FALSE.
 *
 * Returns: MARMOT_OK; MARMOT_ERR_DESERIALIZATION for a record that does
 * not parse (the group is inactive: say that it ended, not why)
 */
/** Later-epoch events after which a removal is final (see below). */
#define MARMOT_REMOVAL_FINAL_AFTER 5

MarmotError marmot_get_group_removal(Marmot *m,
                                     const MarmotGroupId *mls_group_id,
                                     bool *out_removed, uint8_t out_remover[32],
                                     uint64_t *out_epoch, bool *out_final);

/**
 * marmot_update_group_metadata:
 * @m: Marmot instance
 * @mls_group_id: the group to update
 * @config: new group configuration (non-NULL fields are applied)
 * @out_commit_json: (out) (transfer full): the kind:445 Commit event to
 *   publish, as for marmot_add_members()
 *
 * Update group metadata (name, description, admins, relays).
 * Only admins can update group metadata. The non-NULL fields of @config
 * (admins/relays when their count is non-zero) replace those of the group's
 * current GroupData extension; everything else is kept.  The new GroupData
 * is committed as an MLS GroupContextExtensions proposal (RFC 9420 §12.1.7)
 * with an UpdatePath, advancing the group to a new epoch.  If the change
 * cannot be committed (no MLS state, unsupported extensions) nothing is
 * changed.
 *
 * The Commit is pending (since 0.5.0; before, it was applied locally and
 * discarded): publish @out_commit_json, then call
 * marmot_merge_pending_commit() once a relay accepted it, or
 * marmot_clear_pending_commit() if none did.  The stored group changes only
 * on merge.
 *
 * Returns: MARMOT_OK on success, MARMOT_ERR_INVALID_ARG when @out_commit_json
 * is NULL, MARMOT_ERR_ADMIN_ONLY for non-admins, MARMOT_ERR_MLS /
 * MARMOT_ERR_UNSUPPORTED if the Commit cannot be made
 */
MarmotError marmot_update_group_metadata(Marmot *m,
                                          const MarmotGroupId *mls_group_id,
                                          const MarmotGroupConfig *config,
                                          char **out_commit_json);

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
 * @rumor_event_json must come from a NIP-59 gift wrap whose seal signature
 * the caller verified, with the rumor's `pubkey` equal to the seal's (as
 * NIP-59 requires). Since 0.10.0 that pubkey is the Welcome's author, whose
 * own leaf the join accepts without an account proof (see
 * marmot_accept_welcome()). Prefer marmot_process_welcome_from(), which
 * takes the seal's author explicitly instead of trusting the rumor.
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_process_welcome(Marmot *m,
                                    const uint8_t wrapper_event_id[32],
                                    const char *rumor_event_json,
                                    MarmotWelcome **out_welcome);

/**
 * marmot_process_welcome_from:
 * @m: Marmot instance
 * @wrapper_event_id: (array fixed-size=32): the gift-wrap event ID
 * @rumor_event_json: JSON of the unwrapped kind:444 rumor event
 * @sender_pubkey: (array fixed-size=32): the author of the NIP-59 seal,
 *   whose signature the caller verified
 * @out_welcome: (out) (transfer full): the stored welcome record
 *
 * marmot_process_welcome() with the Welcome's authenticated sender given by
 * the caller (review W20 N1): the join's exemption for the sender's own
 * leaf rests on @sender_pubkey, not on the rumor.  A rumor naming another
 * `pubkey` is refused with MARMOT_ERR_AUTHOR_MISMATCH (recorded as failed).
 *
 * Returns: MARMOT_OK on success
 */
MarmotError marmot_process_welcome_from(Marmot *m,
                                        const uint8_t wrapper_event_id[32],
                                        const char *rumor_event_json,
                                        const uint8_t sender_pubkey[32],
                                        MarmotWelcome **out_welcome);

/**
 * marmot_accept_welcome:
 * @m: Marmot instance
 * @welcome: the welcome to accept
 *
 * Accept a pending welcome and join the group.
 *
 * Every member leaf of the Welcome's ratchet tree must be bound to its
 * account (nostrc-7vyi; Marmot protocol-core/joining.md): by a valid
 * marmot.member.account-identity-proof.v2, or -- for the leaf that signed
 * the GroupInfo only, and only in a legacy-profile group -- by being the
 * account that sent the Welcome (the rumor's `pubkey`). Our own leaf is our
 * KeyPackage's. In a legacy-profile group any leaf may lack the proof while
 * MarmotConfig.allow_unproven_members (the default; such members are
 * MARMOT_MEMBER_IDENTITY_UNPROVEN). Otherwise the join fails with
 * MARMOT_ERR_KEY_PACKAGE_IDENTITY, nothing of the group is stored and the
 * Welcome is recorded as failed. A proof that does not verify is refused in
 * every profile and mode.
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
 * result->event_json is signed by a fresh ephemeral key (MIP-03; since
 * 0.5.0 -- before, the caller had to sign it) and can be published to the
 * group relays as is; marmot_save_created_message() persists it with its id.
 *
 * Key use (since 0.8.0, nostrc-ai04): every message uses the next generation
 * of our sender ratchet (RFC 9420 section 9.1: a key and nonce encrypt one
 * message).  The advanced ratchet is stored in the operation's storage
 * transaction before the event is returned: on any error, including a failed
 * commit, no event is returned, so nothing can be published under a
 * generation that is not stored.  An event returned but never published
 * (e.g. the process died) only leaves a gap receivers skip.  Before 0.8.0
 * every call restarted the ratchet: all messages of an epoch reused
 * generation 0.  "Stored" means committed by the storage: when the storage
 * runs its transactions as savepoints of an application transaction (as
 * Groundhog's GhStoreMarmot does inside gh_store_begin()), the step is
 * durable only once that outer transaction commits -- commit it before
 * publishing result->event_json, never roll it back afterwards.
 *
 * Author (since 0.9.0, nostrc-we6g): the inner event is authored by our
 * account, the identity of our leaf's credential.  An inner event without
 * a pubkey gets it (a declared id is recomputed; result->message->content
 * holds the event as sent); one with another account's pubkey is refused
 * with MARMOT_ERR_AUTHOR_MISMATCH, and inner JSON that is not an event with
 * MARMOT_ERR_EVENT.  The PrivateMessage carries an RFC 9420 section 6.3.1
 * signature by our leaf's key over the content.
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
 * Persist an outgoing message (the signed kind:445 event from
 * marmot_create_message()) under its event id, e.g. once it was published.
 * marmot_create_message() does not persist CREATED rows itself.
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
 * @group_event_json: JSON of the signed kind:445 group event, as a relay
 *   delivered it
 * @result: (out): processing result
 *
 * Process a received group message.  Since 0.6.0 (nostrc-6r6s) the event is
 * authenticated first, as the Marmot transport (transports/nostr.md)
 * requires before any decryption: its id must be the canonical NIP-01 hash
 * of its content (else MARMOT_ERR_EVENT) and its Schnorr signature by its
 * pubkey -- a fresh ephemeral key -- must verify (a missing, malformed or
 * wrong signature is MARMOT_ERR_SIGNATURE).  A rejected event changes
 * nothing.  The signature authenticates only the envelope; the sender is
 * authenticated by the exporter-keyed NIP-44 layer and MLS.  A kind:445
 * rumor taken out of a NIP-59 gift wrap is unsigned by design: pass it to
 * marmot_process_rumor_message() instead.
 *
 * Handles:
 * - Application messages (decrypts content): MARMOT_RESULT_APPLICATION_MESSAGE
 * - Commits (since 0.5.0): the Commit is applied through the same validated
 *   MLS path the producers use, then checked against MIP-01 (a Commit that
 *   adds or removes members or changes the GroupData needs a committer who is
 *   an admin of the pre-Commit GroupData, MARMOT_ERR_COMMIT_FROM_NON_ADMIN;
 *   nostr_group_id cannot change, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH; the
 *   committer keeps its account, MARMOT_ERR_IDENTITY_CHANGE; since 0.10.0
 *   every account it adds, or puts in another's slot, is bound by a valid
 *   account-identity proof in its leaf and no member leaf loses one,
 *   MARMOT_ERR_KEY_PACKAGE_IDENTITY, nostrc-7vyi).  The new MLS
 *   state, its exporter secret and the group record are then stored, and
 *   result->commit.updated_group holds the updated group
 *   (MARMOT_RESULT_COMMIT).  Epochs: a Commit for the current epoch advances
 *   the group -- unless our own Commit for this epoch is pending: then the
 *   lower CommitOrderingSuffix (below) wins now; a received Commit that
 *   loses is kept and MARMOT_ERR_OWN_COMMIT_PENDING returned, and it is
 *   processed again by marmot_clear_pending_commit().  One for the
 *   previous epoch competes with the Commit already
 *   applied from that parent: the same Commit again is
 *   MARMOT_RESULT_OWN_MESSAGE (e.g. our own, echoed); a different one
 *   replaces it only if it wins the Marmot same-epoch ordering (privileged
 *   before ordinary, then lower committer key, then lower SHA-256 of the
 *   Commit bytes; transport timestamps and ids never count), otherwise, like
 *   any older Commit, it is MARMOT_ERR_WRONG_EPOCH.  Since 0.10.0 that
 *   judgement is possible only until every member that could publish a
 *   winning competitor was seen sending at the new epoch (nostrc-yuj2);
 *   later, a different Commit for the previous epoch is
 *   MARMOT_ERR_WRONG_EPOCH too.  A Commit for a future
 *   epoch cannot be decrypted yet (MARMOT_ERR_NIP44); retry it after the
 *   missing Commits.  Every rejection leaves the group unchanged.
 * - Standalone proposals: MARMOT_ERR_UNSUPPORTED (not queued)
 *
 * Late messages (since 0.7.0): an application message of the previous
 * epoch that arrives after the next Commit was applied is read with the
 * retained parent state (the state that Commit was built on, kept for one
 * epoch).  Older ones fail with MARMOT_ERR_MLS.  Since 0.10.0 the retained
 * parent keeps its full state only while a competing Commit could still win;
 * then it keeps only what reads these late messages (nostrc-yuj2; see the
 * libmarmot README).
 *
 * Replays and reordering (since 0.8.0, nostrc-ai04): each sender's ratchet
 * is stored with the group state, so a generation decrypts once -- a message
 * re-published in a new envelope (another event id) fails with
 * MARMOT_ERR_MLS, in the current epoch and through the retained parent.  A
 * message's generation may be up to 32 below the newest one read from its
 * sender (it arrived late: its key was kept) or up to the group's
 * max_forward_distance (1000) above it; outside that window it fails with
 * MARMOT_ERR_MLS.  A message that fails to decrypt, or whose storage fails
 * later in the operation, consumes nothing.
 *
 * Sender authentication (since 0.9.0, nostrc-we6g): an application message
 * must carry the RFC 9420 section 6.3.1 signature of its MLS sender leaf
 * (else MARMOT_ERR_MLS), and its inner event's pubkey must be the account
 * identity of that leaf's credential (else MARMOT_ERR_AUTHOR_MISMATCH; no
 * pubkey, or inner JSON that is no event, too).  Either rejection delivers
 * and stores nothing.  result->app_msg.sender_pubkey_hex is therefore the
 * authenticated author.  Messages of libmarmot 0.8.0 and earlier (no
 * signature, empty sender-data AAD) are rejected.  An inner event already
 * delivered (the same NIP-01 id, in another envelope) is
 * MARMOT_RESULT_OWN_MESSAGE: a duplicate, not stored again.
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

/**
 * marmot_process_rumor_message:
 * @m: Marmot instance
 * @rumor_json: JSON of an unsigned kind:445 rumor
 * @result: (out): processing result
 *
 * Like marmot_process_message(), for a kind:445 that arrived as the rumor of
 * a NIP-59 gift wrap (kind:1059) the caller has unwrapped and whose seal
 * signature it has verified: the seal authenticates it, and a rumor carries
 * no signature by design.  Its id, when present, must still be the
 * canonical NIP-01 hash (else MARMOT_ERR_EVENT); a missing one is computed.
 * Never use this for events taken from a relay directly, which must be
 * signed: that path is marmot_process_message().
 *
 * Since: 0.6.0
 * Returns: as marmot_process_message()
 */
MarmotError marmot_process_rumor_message(Marmot *m,
                                          const char *rumor_json,
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
 * marmot_get_group_members:
 * @m: Marmot instance
 * @mls_group_id: group to query
 * @out_members: (out) (array length=out_count) (transfer full): the account
 *   keys (32-byte x-only Nostr pubkeys, the leaf credentials' identities) of
 *   the group's current members in our stored MLS state, in leaf order, each
 *   once; NULL when there are none. Free with free().
 * @out_count: (out): number of members
 *
 * The membership of the epoch we are in: an Add or Remove is counted only
 * once its Commit was merged or applied.  Read-only; nothing is written.
 *
 * Since: 0.9.0 (additive)
 * Returns: MARMOT_OK; MARMOT_ERR_GROUP_NOT_FOUND when we hold no MLS state
 *   for the group; MARMOT_ERR_MEMORY
 */
MarmotError marmot_get_group_members(Marmot *m,
                                      const MarmotGroupId *mls_group_id,
                                      uint8_t (**out_members)[32],
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

/* Member identity status and KeyPackage evidence (since 0.12.0). */
#include "marmot-members.h"

#endif /* MARMOT_H */
