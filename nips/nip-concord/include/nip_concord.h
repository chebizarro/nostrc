#ifndef NOSTR_NIP_CONCORD_H
#define NOSTR_NIP_CONCORD_H

/*
 * nip-concord: Concord protocol core (NIP-CAS-0008).
 *
 * Concord is an adopted external standard (github.com/concord-protocol/concord).
 * This library implements the frozen cryptographic surface of the fleet-supported
 * core — CORD-01 (private streams), CORD-02 (communities, Appendix A derivations),
 * CORD-03 (channels) and CORD-05 (invites).
 *
 * Scope note: this header is deliberately buffer-oriented. Event assembly,
 * JSON document shaping and relay I/O live in the client layer (see the gnostr
 * concord-communities plugin), which already owns a Nostr event model and a JSON
 * parser. Keeping the library to pure, side-effect-free crypto makes it testable
 * against the CORD vectors without a relay or an event library.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Kind numbers (frozen; allocated by Concord, documented by NIP-CAS-0008)
 * ------------------------------------------------------------------ */

/* Relay-visible */
#define CONCORD_COMMUNITY_LIST ((uint16_t)13302)
#define CONCORD_INVITE_LIST ((uint16_t)13303)
#define CONCORD_INVITE_BUNDLE ((uint16_t)33301)
#define CONCORD_DIRECT_INVITE ((uint16_t)3313)

/* Inner rumor kinds (inside encrypted wraps) */
#define CONCORD_KIND_MESSAGE 9
#define CONCORD_KIND_THREADED_REPLY 1111
#define CONCORD_KIND_REACTION 7
#define CONCORD_KIND_DELETE 5
#define CONCORD_KIND_EDIT 3302
#define CONCORD_KIND_REKEY 3303
#define CONCORD_KIND_JOIN_LEAVE 3306
#define CONCORD_KIND_CONTROL_EDITION 3308
#define CONCORD_KIND_KICK 3309
#define CONCORD_KIND_SNAPSHOT 3312

/* Stream wrap / seal kinds (CORD-01) */
#define CONCORD_STREAM_WRAP 1059
#define CONCORD_EPHEMERAL_STREAM_WRAP 21059
#define CONCORD_SEAL_ENCRYPTED 20013
#define CONCORD_SEAL_PLAINTEXT 20014

/* Control edition sub-kinds (the `vsk` tag, CORD-02 Appendix B) */
#define CONCORD_VSK_METADATA 0
#define CONCORD_VSK_ROLE 1
#define CONCORD_VSK_CHANNEL 2
#define CONCORD_VSK_GRANT 3
#define CONCORD_VSK_BANLIST 4
#define CONCORD_VSK_INVITE_LIVE 6
#define CONCORD_VSK_INVITE_REGISTRY 8
#define CONCORD_VSK_INVITE_REVOKED 9
#define CONCORD_VSK_DISSOLVED 10
#define CONCORD_VSK_PINS 11

/* ------------------------------------------------------------------ *
 * Limits
 * ------------------------------------------------------------------ */

/* CORD-02 §8: the Community List is one NIP-44 event. */
#define CONCORD_MAX_COMMUNITIES_IN_LIST 50
/* CORD-05 §1: bundles are attacker-crafted input; bound before allocating. */
#define CONCORD_MAX_CHANNELS_IN_INVITE 256
/* CORD-02 §6: up to 5 stable relays recommended. */
#define CONCORD_MAX_RELAYS_IN_BUNDLE 5
/* CORD-05 §3: the fragment only needs to *find* the bundle. */
#define CONCORD_MAX_RELAYS_IN_FRAGMENT 3
/* CORD-02 §5: guestbook snapshots chunk at 400 members per event. */
#define CONCORD_SNAPSHOT_CHUNK_SIZE 400
/* CORD-02 §6: name caps at 64 bytes, description at 10000, UTF-8. */
#define CONCORD_MAX_NAME_BYTES 64
#define CONCORD_MAX_DESCRIPTION_BYTES 10000
/* NIP-44 hard-caps plaintext; enforce at every nesting layer (Appendix B). */
#define CONCORD_MAX_NIP44_PLAINTEXT 65535
/* CORD-06 §1: one rekey event carries up to 120 per-recipient blobs; a larger
 * rotation spans several, correlated by the `chunk` tag. */
#define CONCORD_REKEY_BLOBS_PER_EVENT 120
/* CORD-06 §1: the wrapped plaintext is fixed-width per form, and the width
 * *is* the form declaration — any other width is malformed and dropped. */
#define CONCORD_REKEY_BLOB_CHANNEL_BYTES 72
#define CONCORD_REKEY_BLOB_MEMBER_BYTES 104
#define CONCORD_REKEY_BLOB_STAFF_BYTES 136
/* CORD-05 §2: the off-network unlock token. */
#define CONCORD_INVITE_TOKEN_BYTES 16
/* CORD-05 §3: current fragment format / dictionary generation. */
#define CONCORD_INVITE_FRAGMENT_VERSION 4

/* ------------------------------------------------------------------ *
 * Derivation labels (frozen, CORD-02 Appendix A.6)
 * ------------------------------------------------------------------ */

#define CONCORD_LABEL_CHANNEL "concord/channel"
#define CONCORD_LABEL_CONTROL "concord/control"
#define CONCORD_LABEL_CONTROL_SIGNER "concord/control-signer"
#define CONCORD_LABEL_GUESTBOOK "concord/guestbook"
#define CONCORD_LABEL_DISSOLVED "concord/dissolved"
#define CONCORD_LABEL_GRANT "concord/grant"
#define CONCORD_LABEL_BANLIST "concord/banlist"
#define CONCORD_LABEL_PINS "concord/pins"
#define CONCORD_LABEL_INVITE_LINKS "concord/invite-links"
#define CONCORD_LABEL_INVITE_KEY "concord/invite-key"
#define CONCORD_LABEL_COMMUNITY "concord/community"
/* CORD-06: the rotation labels. The two rekey addresses derive from the
 * *prior* community_root at the *new* epoch, so a member holding the current
 * key precomputes exactly where the next rotation will land. */
#define CONCORD_LABEL_REKEY_PSEUDONYM "concord/rekey-pseudonym"
#define CONCORD_LABEL_BASE_REKEY_PSEUDONYM "concord/base-rekey-pseudonym"
#define CONCORD_LABEL_RECIPIENT_PSEUDONYM "concord/recipient-pseudonym"
/* CORD-02 A.5: the epoch-key commitment domain separator. Like the edition
 * label, not an HKDF label — it prefixes a SHA-256 preimage. */
#define CONCORD_LABEL_EPOCH_COMMITMENT "concord/epoch-key-commitment"
/* CORD-04 §1: the edition-hash domain separator. Not an HKDF label — it is
 * length-prefixed into a SHA-256 preimage — and frozen like the rest. */
#define CONCORD_LABEL_EDITION "vector-community/v1/edition"

/* ------------------------------------------------------------------ *
 * Permissions (CORD-04 §3, frozen bit positions)
 *
 * A new permission claims the next free bit; a retired one is burned, never
 * renumbered or reused. There is no all-powerful bit: an "admin" holds the
 * union of the management bits, so a Role granted everything today does not
 * inherit a permission added tomorrow.
 * ------------------------------------------------------------------ */

#define CONCORD_PERM_MANAGE_ROLES ((uint64_t)1 << 0)
#define CONCORD_PERM_MANAGE_CHANNELS ((uint64_t)1 << 1)
#define CONCORD_PERM_MANAGE_METADATA ((uint64_t)1 << 2)
#define CONCORD_PERM_KICK ((uint64_t)1 << 3)
#define CONCORD_PERM_BAN ((uint64_t)1 << 4)
#define CONCORD_PERM_MANAGE_MESSAGES ((uint64_t)1 << 5)
#define CONCORD_PERM_CREATE_INVITE ((uint64_t)1 << 6)
/* 1<<7 retired (was MANAGE_INVITES) */
#define CONCORD_PERM_VIEW_AUDIT_LOG ((uint64_t)1 << 8)
#define CONCORD_PERM_MENTION_EVERYONE ((uint64_t)1 << 9)
/* 1<<10, 1<<12 reserved (MANAGE_EMOJI, MANAGE_EVENTS) */
#define CONCORD_PERM_PIN_MESSAGES ((uint64_t)1 << 11)

/* CORD-04 §3: the six bits whose actions land as Control editions. A member
 * holding any of them — plus always the owner — is staff, the set that holds
 * the control_root. Normative: a future permission whose actions are Control
 * editions MUST amend this explicitly. */
#define CONCORD_PERMS_STAFF                                                    \
    (CONCORD_PERM_MANAGE_ROLES | CONCORD_PERM_MANAGE_CHANNELS |                \
     CONCORD_PERM_MANAGE_METADATA | CONCORD_PERM_BAN |                         \
     CONCORD_PERM_CREATE_INVITE | CONCORD_PERM_PIN_MESSAGES)

/* CORD-04 §2: the owner occupies position 0 and no Role may ever claim it —
 * an owner could otherwise create a peer nobody outranks. A roleless member
 * is effectively last. */
#define CONCORD_POSITION_OWNER ((uint32_t)0)
#define CONCORD_POSITION_LAST ((uint32_t)0xffffffffu)

/* CORD-04 §2: a Community carries at most 100 Roles (a client folds the 100
 * lowest role_ids and ignores the rest) and a member holds at most 64. */
#define CONCORD_MAX_ROLES_IN_COMMUNITY 100
#define CONCORD_MAX_ROLES_PER_MEMBER 64
/* CORD-04 §4: unbounded by rule, but an edition must fit its NIP-44 envelope
 * at every layer — a practical ceiling near 500 npubs. */
#define CONCORD_MAX_BANLIST_ENTRIES 500

typedef enum {
    NOSTR_CONCORD_OK = 0,
    NOSTR_CONCORD_ERR_NULL,
    NOSTR_CONCORD_ERR_OOM,
    NOSTR_CONCORD_ERR_WRONG_KIND,
    NOSTR_CONCORD_ERR_BAD_PUBKEY,
    NOSTR_CONCORD_ERR_BAD_CONTENT,
    NOSTR_CONCORD_ERR_BAD_TAG,
    NOSTR_CONCORD_ERR_CARDINALITY,
    NOSTR_CONCORD_ERR_BAD_NIP44,
    NOSTR_CONCORD_ERR_BAD_SIGNATURE,
    NOSTR_CONCORD_ERR_COMMUNITY_ID,
    NOSTR_CONCORD_ERR_EPOCH,
    NOSTR_CONCORD_ERR_CHANNEL,
    NOSTR_CONCORD_ERR_INVITE_EXPIRED,
    NOSTR_CONCORD_ERR_LIST_FULL,
    NOSTR_CONCORD_ERR_SNAPSHOT_MALFORMED,
    NOSTR_CONCORD_ERR_CRYPTO,
    NOSTR_CONCORD_ERR_BAD_FRAGMENT,
    NOSTR_CONCORD_ERR_UNSUPPORTED_VERSION,
    NOSTR_CONCORD_ERR_CONTROL_PAIR
} nostr_concord_status_t;

const char *nostr_concord_status_string(nostr_concord_status_t status);

/* ------------------------------------------------------------------ *
 * Encoding helpers (CORD-01 "Encoding": hex is lowercase, 64 chars)
 * ------------------------------------------------------------------ */

bool nostr_concord_is_lower_hex_32(const char *value);
/* Decodes exactly 64 lowercase hex chars into 32 bytes. */
bool nostr_concord_hex_decode_32(const char *hex, uint8_t out[32]);
/* Writes 64 lowercase hex chars plus a NUL. */
void nostr_concord_hex_encode_32(const uint8_t in[32], char out[65]);

/* base64url, no padding. Caller frees with free(). */
char *nostr_concord_b64url_encode(const uint8_t *data, size_t len);
/* Returns NULL on any non-canonical input (padding, stray chars). */
uint8_t *nostr_concord_b64url_decode(const char *text, size_t *out_len);

/* ------------------------------------------------------------------ *
 * Derivations (CORD-02 Appendix A, frozen)
 * ------------------------------------------------------------------ */

/* A.4: community_id = sha256("concord/community" || owner_xonly || owner_salt) */
nostr_concord_status_t nostr_concord_derive_community_id(
    const uint8_t owner_xonly[32],
    const uint8_t owner_salt[32],
    uint8_t community_id_out[32]);

/* Recomputes the id from the bundle's owner proof and compares in constant
 * time. CORD-05 §1: a bundle failing this is refused. */
bool nostr_concord_verify_community_id(const uint8_t community_id[32],
                                       const uint8_t owner_xonly[32],
                                       const uint8_t owner_salt[32]);

/* A.1: HKDF-SHA256(ikm=secret, salt=empty,
 *                  info=utf8(label) || 0x00 || id[32] || epoch_be[8]).
 * Pass has_epoch=false for the labels that omit the epoch entirely. */
nostr_concord_status_t nostr_concord_hkdf(const char *label,
                                          const uint8_t *secret,
                                          size_t secret_len,
                                          const uint8_t id[32],
                                          bool has_epoch,
                                          uint64_t epoch,
                                          uint8_t out[32]);

/* A.2 output: a plane's stream keypair plus the NIP-44 self-ECDH conversation
 * key that encrypts its wraps. */
typedef struct {
    uint8_t sk[32];       /* signs the plane's kind-1059 wraps */
    uint8_t pk[32];       /* x-only; the stream address (authors filter) */
    uint8_t conv_key[32]; /* nip44_conversation_key(sk, pk) */
} nostr_concord_group_key_t;

/* A.2 + A.3. `secret` is usually 32 bytes, but
 * concord/recipient-pseudonym feeds a 64-byte concatenation, so the length is
 * explicit. `id` is the raw 32-byte value (never hex); pass all-zeroes where a
 * label has no meaningful id. */
nostr_concord_status_t nostr_concord_group_key(const char *label,
                                               const uint8_t *secret,
                                               size_t secret_len,
                                               const uint8_t id[32],
                                               bool has_epoch,
                                               uint64_t epoch,
                                               nostr_concord_group_key_t *out);

void nostr_concord_group_key_clear(nostr_concord_group_key_t *key);

/* CORD-03 §1: a channel's plane, from the community_root (public channel) or
 * the channel's own independent key (private channel). */
nostr_concord_status_t nostr_concord_channel_key(
    const uint8_t channel_secret[32],
    const uint8_t channel_id[32],
    uint64_t epoch,
    nostr_concord_group_key_t *out);

/* CORD-02 §5: the Control Plane read key (its conv_key decrypts the wraps).
 * On pre-split epochs this derivation was the whole plane — its pk the address
 * and wrap signer too — so the sk/pk stay available for reading those. */
nostr_concord_status_t nostr_concord_control_read_key(
    const uint8_t community_root[32],
    const uint8_t community_id[32],
    uint64_t epoch,
    nostr_concord_group_key_t *out);

/* CORD-02 §5: the Control Plane signer. Its pk is the address; its sk is
 * staff-only. Members hold the pk from their invite and never derive it. */
nostr_concord_status_t nostr_concord_control_signer_key(
    const uint8_t control_root[32],
    const uint8_t community_id[32],
    uint64_t epoch,
    nostr_concord_group_key_t *out);

/* CORD-02 §5: the Guestbook Plane (member-writable). */
nostr_concord_status_t nostr_concord_guestbook_key(
    const uint8_t community_root[32],
    const uint8_t community_id[32],
    uint64_t epoch,
    nostr_concord_group_key_t *out);

/* CORD-02 §9: the dissolution tombstone address. Derives from the
 * community_id alone — no secret, no epoch — so every member past or present
 * resolves the same coordinate. */
nostr_concord_status_t nostr_concord_dissolved_key(
    const uint8_t community_id[32],
    nostr_concord_group_key_t *out);

/* ------------------------------------------------------------------ *
 * Rekeys and Refoundings (CORD-06)
 * ------------------------------------------------------------------ */

/* §1: a rotation names the key it replaces by a 32-byte scope id — a Channel
 * id, or all-zeroes for a base rotation, which no Channel id can collide
 * with. */
void nostr_concord_base_scope_id(uint8_t out[32]);
bool nostr_concord_scope_is_base(const uint8_t scope_id[32]);

/* §2: the address a rotation is published at, precomputable by every current
 * keyholder. Both derive from the *prior* community_root at the *new* epoch:
 * a member who missed the rotation therefore also missed the address, which
 * is exactly the removal signal.
 *
 * A Public Channel has no independent rekey — its key comes from the
 * community_root, so it rotates only when the base does (CORD-03). */
nostr_concord_status_t nostr_concord_channel_rekey_key(
    const uint8_t prior_community_root[32],
    const uint8_t channel_id[32],
    uint64_t new_epoch,
    nostr_concord_group_key_t *out);

nostr_concord_status_t nostr_concord_base_rekey_key(
    const uint8_t prior_community_root[32],
    const uint8_t community_id[32],
    uint64_t new_epoch,
    nostr_concord_group_key_t *out);

/* §2: where a recipient finds their own blob inside a rekey event. The inputs
 * are all public — two x-only pubkeys, the scope and the epoch — so a NIP-46
 * bunker account computes its locator without touching a private key. The
 * derivation is not a secret: it is unreachable to an outsider because the
 * locator list itself lives inside the encrypted event. */
nostr_concord_status_t nostr_concord_rekey_locator(
    const uint8_t rotator_xonly[32],
    const uint8_t recipient_xonly[32],
    const uint8_t scope_id[32],
    uint64_t new_epoch,
    uint8_t locator_out[32]);

/* CORD-02 A.5: the continuity check a receiver runs before adopting a new
 * key. Recompute it over the key currently held and require equality with the
 * event's `prevcommit`; a match proves the rotation extends this very key.
 * It is a convergence check, never a secrecy mechanism — post-removal secrecy
 * rests entirely on a removed member receiving no blob. */
nostr_concord_status_t nostr_concord_epoch_commitment(
    uint64_t prev_epoch,
    const uint8_t prev_key[32],
    uint8_t commitment_out[32]);

/* §1: one located, wrapped key, in the fixed-width plaintext form whose width
 * declares what it carries. */
typedef enum {
    /* 72 bytes, Channel scope: scope_id ‖ epoch ‖ new_key. */
    NOSTR_CONCORD_REKEY_CHANNEL = 0,
    /* 72 bytes, base scope: a legacy, pre-split base rotation. Honored when
     * reading old epochs and never minted anew — a compliant Rotator always
     * mints the control_root split (CORD-02 §2). */
    NOSTR_CONCORD_REKEY_BASE_LEGACY,
    /* 104 bytes: a base rotation to a plain member — the new root and the new
     * Control Plane address. */
    NOSTR_CONCORD_REKEY_BASE_MEMBER,
    /* 136 bytes: a base rotation to staff, appending the control_root that
     * writes at that address (CORD-04 §3). */
    NOSTR_CONCORD_REKEY_BASE_STAFF
} nostr_concord_rekey_form_t;

typedef struct {
    nostr_concord_rekey_form_t form;
    uint8_t scope_id[32];
    uint64_t epoch;
    uint8_t new_key[32]; /* the Channel key, or the new community_root */
    /* Base forms only; zeroed and flagged absent on the legacy and Channel
     * forms. */
    uint8_t new_control_pk[32];
    uint8_t new_control_root[32]; /* staff form only */
    bool has_control_pk;
    bool has_control_root;
} nostr_concord_rekey_blob_t;

void nostr_concord_rekey_blob_clear(nostr_concord_rekey_blob_t *blob);

/* Serializes @blob into the plaintext a Rotator wraps under its pairwise key
 * with the recipient. Refuses a blob whose form and populated fields
 * disagree, so a caller cannot mint a 136-byte staff blob with no
 * control_root in it. */
nostr_concord_status_t nostr_concord_rekey_blob_pack(
    const nostr_concord_rekey_blob_t *blob,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

/* Parses one unwrapped blob and *binds* it: @expect_scope and @expect_epoch
 * are the enclosing event's tag values, and a blob whose own scope or epoch
 * differs is refused rather than adopted. That is what makes a blob
 * unspliceable — one minted for a Channel can never be replayed against
 * another, nor a stale epoch's against the current one.
 *
 * On the staff form the control pair is verified too: @community_id must
 * derive @new_control_root to exactly @new_control_pk, so a recipient refuses
 * a plane split from its own readers rather than adopting it. Pass a real
 * community_id always; it is the id every recipient already holds. */
nostr_concord_status_t nostr_concord_rekey_blob_parse(
    const uint8_t *plaintext,
    size_t len,
    const uint8_t community_id[32],
    const uint8_t expect_scope[32],
    uint64_t expect_epoch,
    nostr_concord_rekey_blob_t *out);

/* ------------------------------------------------------------------ *
 * Control editions (CORD-04)
 * ------------------------------------------------------------------ */

/* §1: an edition's identity — what the next edition's `ep` cites — over a
 * length-prefixed, domain-separated preimage, so two clients holding the same
 * edition compute the same hash:
 *
 *   sha256( len64(label) || label || entity_id[32] || version_be[8]
 *           || (prev ? 0x01 || prev[32] : 0x00 || zero[32])
 *           || len64(content) || content )
 *
 * Every field is fixed-width or length-prefixed, so distinct inputs can never
 * collide. Pass `prev = NULL` on a first edition. `content` is the rumor's
 * content bytes, hashed verbatim and never re-serialized, so a compaction's
 * re-wrap preserves the hash. */
nostr_concord_status_t nostr_concord_edition_hash(const uint8_t entity_id[32],
                                                  uint64_t version,
                                                  const uint8_t *prev,
                                                  const uint8_t *content,
                                                  size_t content_len,
                                                  uint8_t hash_out[32]);

/* §1 Appendix A.6: the derived entity coordinates. Each binds to the
 * community_id and never to a key or an epoch, so they survive every
 * Refounding and a fresh joiner holding only the newest root derives the
 * same ones. */
nostr_concord_status_t nostr_concord_grant_locator(
    const uint8_t community_id[32],
    const uint8_t member_xonly[32],
    uint8_t eid_out[32]);

nostr_concord_status_t nostr_concord_banlist_locator(
    const uint8_t community_id[32],
    uint8_t eid_out[32]);

/* CORD-05 §5: a creator's invite Registry coordinate. Bound to the creator as
 * well as the Community, so each creator owns exactly their own list of live
 * link coordinates and nobody can forge entries into anyone else's. */
nostr_concord_status_t nostr_concord_invite_registry_locator(
    const uint8_t community_id[32],
    const uint8_t creator_xonly[32],
    uint8_t eid_out[32]);

/* §3: `permissions` rides the wire as a decimal string, never a bare number —
 * a JSON number is a 64-bit float in JavaScript and silently corrupts past
 * 2^53. A reader accepts either form; this parses the string one. Rejects a
 * leading sign, leading zeros, and anything that overflows a u64. */
bool nostr_concord_parse_permissions(const char *decimal, uint64_t *out);

/* ------------------------------------------------------------------ *
 * Invites (CORD-05)
 * ------------------------------------------------------------------ */

/* §2: bundle_key = hkdf(token, "concord/invite-key"). The result is used
 * directly as a NIP-44 conversation key — it is not a group_key. */
nostr_concord_status_t nostr_concord_invite_key(
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    uint8_t bundle_key_out[32]);

/* §3: the decoded `#fragment` of an invite URL. */
typedef struct {
    uint8_t token[CONCORD_INVITE_TOKEN_BYTES];
    /* NULL-terminated array of bootstrap relay URLs; free with
     * nostr_concord_invite_fragment_clear(). Empty when the stock-set flag is
     * set and the caller should use the built-in dictionary primaries. */
    char **relays;
    size_t n_relays;
    bool stock_relays;
    uint8_t version;
} nostr_concord_invite_fragment_t;

/* Decodes `[version][flags][relays?][token:16]` from base64url without
 * padding. Rejects versions below CONCORD_INVITE_FRAGMENT_VERSION rather than
 * decoding them against the wrong dictionary generation. */
nostr_concord_status_t nostr_concord_invite_fragment_parse(
    const char *fragment,
    nostr_concord_invite_fragment_t *out);

void nostr_concord_invite_fragment_clear(
    nostr_concord_invite_fragment_t *fragment);

/* §3: the inverse. Encodes `[version][flags][relays?][token:16]` at the
 * current version, choosing the shortest form per relay: a dictionary id
 * costs one byte, a `wss://` host drops its scheme, and anything else rides
 * verbatim. Pass @stock_relays to select the four built-in primaries, in
 * which case zero relay bytes follow and @relays is ignored — the common
 * invite then carries no relay bytes at all.
 *
 * A fragment is never sent to any server, so this is the only place the
 * unlock token is ever written down outside the creator's Invite List.
 *
 * Caller frees with free(). */
nostr_concord_status_t nostr_concord_invite_fragment_encode(
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    const char *const *relays,
    size_t n_relays,
    bool stock_relays,
    char **fragment_out);

/* The stock relay dictionary (CORD-05 §3), 1-based: id 1..4. Returns NULL for
 * an id outside the current generation. */
const char *nostr_concord_relay_dictionary_lookup(uint8_t id);
/* The four stock primaries selected by the fragment's stock-set flag. */
const char *const *nostr_concord_stock_relays(size_t *n_relays);

/* §2: decrypts the kind-33301 bundle content with the token-derived key.
 * Returns the bundle JSON, NUL-terminated; caller frees with free(). */
nostr_concord_status_t nostr_concord_invite_bundle_decrypt(
    const char *content_base64,
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    char **bundle_json_out);

/* §2: encrypts a bundle JSON document under the token-derived key, producing
 * the kind-33301 content. Caller frees with free(). */
nostr_concord_status_t nostr_concord_invite_bundle_encrypt(
    const char *bundle_json,
    const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
    char **content_base64_out);

/* ------------------------------------------------------------------ *
 * Stream payloads (CORD-01)
 * ------------------------------------------------------------------ */

/* Encrypts/decrypts one nesting layer under a plane's conversation key. Both
 * enforce CONCORD_MAX_NIP44_PLAINTEXT themselves: NIP-44 libraries are lenient,
 * and a lenient publisher mints events a strict reader cannot decrypt. */
nostr_concord_status_t nostr_concord_stream_seal(
    const uint8_t conv_key[32],
    const char *plaintext,
    char **base64_out);

nostr_concord_status_t nostr_concord_stream_open(
    const uint8_t conv_key[32],
    const char *base64_payload,
    char **plaintext_out);

/* CORD-02 §5: the ordering basis. created_at seconds and an ["ms", 0..999]
 * tag; the true time is created_at * 1000 + ms. Returns false when ms is out
 * of range — such an entry is malformed and dropped, not interpreted. */
bool nostr_concord_order_key(int64_t created_at, int ms, int64_t *out_ms_time);

/* Strict ["ms", …] tag parse: 0..999, decimal, no leading zeros. */
bool nostr_concord_parse_ms(const char *value, int *out);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP_CONCORD_H */
