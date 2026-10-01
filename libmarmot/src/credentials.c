/*
 * libmarmot - MIP-00: Credentials & KeyPackages
 *
 * Creates, parses and selects kind:30443 (addressable) KeyPackage events.
 *
 * Flow:
 *   1. Generate MLS keypairs (Ed25519 signing + X25519 HPKE)
 *   2. Create MlsKeyPackage with Nostr pubkey as BasicCredential identity
 *   3. TLS-serialize the KeyPackage
 *   4. Base64-encode the serialized bytes
 *   5. Build a kind:30443 NostrEvent with tags, in the MDK 0.8 order pinned
 *      by tests/vectors/mdk/protocol-vectors.json:
 *      - d = stable per-account publication slot (32 random bytes, hex)
 *      - mls_protocol_version = "1.0"
 *      - mls_ciphersuite = "0x0001"
 *      - mls_extensions = "0x0006" "0x000a" "0xf2ee" (0x0006: the LeafNode
 *        app_data_dictionary carrying the account-identity proof v2,
 *        nostrc-7vyi; MDK 0.8 requires 0x000a and 0xf2ee, allows others)
 *      - mls_proposals = "0x000a"
 *      - relays = relay URLs (omitted when none are given)
 *      - i = hex(KeyPackageRef)
 *      - encoding = "base64"
 *   6. Return the event JSON (signed or unsigned) + KeyPackageRef
 *
 * Spec: marmot-protocol/marmot transports/nostr.md "KeyPackage publication"
 * and "Event identity and tag cardinality"; foundation/key-packages.md.
 * The legacy kind 443 is not produced or accepted (strict cutover, as in the
 * adopted spec and MDK master; MDK 0.8 accepted 443 only until 2026-05-31).
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "kp_profile.h"
#include "kp_lifecycle.h"
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ──────────────────────────────────────────────────────────────────────── */

static void
clear_stack_event(NostrEvent *event)
{
    if (!event) return;
    free(event->id);
    free(event->pubkey);
    free(event->content);
    free(event->sig);
    nostr_tags_free(event->tags);
    memset(event, 0, sizeof(*event));
}

static bool
is_hex_len(const char *s, size_t len)
{
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static bool
is_lower_hex_len(const char *s, size_t len)
{
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

/*
 * Transport tag cardinality (marmot transports/nostr.md, "Event identity and
 * tag cardinality"): required tags appear exactly once, and a receiver must
 * not read the first match while ignoring later duplicates.
 */
static size_t
count_tags(NostrTags *tags, const char *key, NostrTag **out_first)
{
    size_t n = 0;
    if (out_first) *out_first = NULL;
    if (!tags || !key) return 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = tag ? nostr_tag_get_key(tag) : NULL;
        if (!tag_key || strcmp(tag_key, key) != 0) continue;
        if (n == 0 && out_first) *out_first = tag;
        n++;
    }
    return n;
}

/* Exactly one `key` tag carrying exactly one non-empty value. */
static const char *
singleton_tag_value(NostrTags *tags, const char *key)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1 || nostr_tag_size(tag) != 2)
        return NULL;
    const char *value = nostr_tag_get(tag, 1);
    return (value && value[0] != '\0') ? value : NULL;
}

/* An id-list value is "0x" + exactly four lowercase hex digits. */
static bool
is_id_list_value(const char *v)
{
    return v && v[0] == '0' && v[1] == 'x' && is_lower_hex_len(v + 2, 4);
}

/* Exactly one `key` tag with >= 1 well-formed, pairwise-distinct ids. */
static bool
id_list_tag_valid(NostrTags *tags, const char *key)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1) return false;
    size_t n = nostr_tag_size(tag);
    if (n < 2) return false;
    for (size_t i = 1; i < n; i++) {
        const char *v = nostr_tag_get(tag, i);
        if (!is_id_list_value(v)) return false;
        for (size_t j = 1; j < i; j++) {
            if (strcmp(v, nostr_tag_get(tag, j)) == 0) return false;
        }
    }
    return true;
}

/*
 * Relay URL profile (transports/nostr.md "Relay URL profile"): absolute
 * ws/wss URL with a host, <= 512 bytes, no userinfo and no fragment.
 */
static bool
is_relay_url(const char *url)
{
    if (!url || strlen(url) > 512) return false;
    const char *host;
    if (strncmp(url, "wss://", 6) == 0) host = url + 6;
    else if (strncmp(url, "ws://", 5) == 0) host = url + 5;
    else return false;
    size_t authority_len = strcspn(host, "/?#");
    if (authority_len == 0 || host[0] == ':') return false;
    if (memchr(host, '@', authority_len)) return false;
    return strchr(url, '#') == NULL;
}

/* `relays` is optional (the adopted profile drops it); if present, exactly
 * one tag with >= 1 valid relay URL. */
static bool
relays_tag_valid(NostrTags *tags)
{
    NostrTag *tag = NULL;
    size_t n = count_tags(tags, "relays", &tag);
    if (n == 0) return true;
    if (n != 1 || nostr_tag_size(tag) < 2) return false;
    for (size_t i = 1; i < nostr_tag_size(tag); i++) {
        if (!is_relay_url(nostr_tag_get(tag, i))) return false;
    }
    return true;
}

static MarmotError
verify_event_id_and_signature(NostrEvent *event)
{
    if (!event || !is_hex_len(event->id, 64) ||
        !is_hex_len(event->pubkey, 64) || !is_hex_len(event->sig, 128))
        return MARMOT_ERR_VALIDATION;

    char *claimed_id = event->id;
    event->id = NULL;
    char *computed_id = nostr_event_get_id(event);
    free(event->id); /* cached copy created by nostr_event_get_id() */
    event->id = claimed_id;
    if (!computed_id) return MARMOT_ERR_VALIDATION;

    bool id_ok = strcmp(computed_id, claimed_id) == 0;
    free(computed_id);
    if (!id_ok) return MARMOT_ERR_VALIDATION;

    return nostr_event_check_signature(event) ? MARMOT_OK : MARMOT_ERR_VALIDATION;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: ensure MLS identity is initialized
 * ──────────────────────────────────────────────────────────────────────── */

int
marmot_ensure_identity(Marmot *m)
{
    if (m->identity_ready) return 0;

    /* Generate Ed25519 signing keypair */
    if (mls_crypto_sign_keygen(m->ed25519_sk, m->ed25519_pk) != 0)
        return -1;

    /* Derive X25519 encryption keypair from Ed25519 */
    if (crypto_sign_ed25519_sk_to_curve25519(m->hpke_sk, m->ed25519_sk) != 0)
        return -1;
    if (crypto_sign_ed25519_pk_to_curve25519(m->hpke_pk, m->ed25519_pk) != 0)
        return -1;

    m->identity_ready = true;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: base64 encode/decode via libsodium
 * ──────────────────────────────────────────────────────────────────────── */

static char *
marmot_base64_encode(const uint8_t *data, size_t len)
{
    /* +1 for libsodium's null terminator */
    size_t b64_maxlen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(b64_maxlen);
    if (!out) return NULL;
    sodium_bin2base64(out, b64_maxlen, data, len, sodium_base64_VARIANT_ORIGINAL);
    return out;
}

static uint8_t *
marmot_base64_decode(const char *b64, size_t *out_len)
{
    if (!b64 || !out_len) return NULL;
    size_t b64_len = strlen(b64);
    /* Decoded size is at most 3/4 of base64 length */
    size_t max_bin = (b64_len / 4) * 3 + 3;
    uint8_t *out = malloc(max_bin);
    if (!out) return NULL;

    size_t bin_len = 0;
    if (sodium_base642bin(out, max_bin, b64, b64_len,
                          NULL, &bin_len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        free(out);
        return NULL;
    }
    *out_len = bin_len;
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: stable KeyPackage publication slot (the kind:30443 `d` tag)
 *
 * The spec requires the slot id to be generated once from 32 random bytes,
 * retained locally, and reused for every routine replacement; it must not be
 * derived from identity or key material. Legacy and adopted profiles have
 * independent slots, so publishing one cannot replace the other on a relay.
 * The legacy slot retains its existing storage label across upgrades.
 * ──────────────────────────────────────────────────────────────────────── */

#define MARMOT_KP_SLOT_LABEL_LEGACY  "kp_slot"
#define MARMOT_KP_SLOT_LABEL_ADOPTED "kp_slot_adopted"
#define MARMOT_KP_SLOT_LEN           32

static MarmotError
load_or_create_key_package_slot(Marmot *m, const uint8_t owner_pubkey[32],
                                bool adopted, uint8_t slot_out[MARMOT_KP_SLOT_LEN])
{
    const char *label = adopted ? MARMOT_KP_SLOT_LABEL_ADOPTED : MARMOT_KP_SLOT_LABEL_LEGACY;
    uint8_t *stored = NULL;
    size_t stored_len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, label,
                                           owner_pubkey, 32,
                                           &stored, &stored_len);
    if (err == MARMOT_OK) {
        bool ok = stored && stored_len == MARMOT_KP_SLOT_LEN;
        if (ok) memcpy(slot_out, stored, MARMOT_KP_SLOT_LEN);
        free(stored);
        return ok ? MARMOT_OK : MARMOT_ERR_STORAGE;
    }
    free(stored);
    if (err != MARMOT_ERR_STORAGE_NOT_FOUND) return err;

    randombytes_buf(slot_out, MARMOT_KP_SLOT_LEN);
    return m->storage->mls_store(m->storage->ctx, label,
                                 owner_pubkey, 32,
                                 slot_out, MARMOT_KP_SLOT_LEN);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: id-list tag ["name", "0x%04x", ...] from a u16 list
 * (transports/nostr.md: lowercase, zero-padded, at least one value).
 * ──────────────────────────────────────────────────────────────────────── */

static NostrTag *
id_list_tag_new(const char *name, const uint16_t *ids, size_t n)
{
    if (n == 0) return NULL;
    char v[7];
    snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[0]);
    NostrTag *tag = nostr_tag_new(name, v, NULL);
    for (size_t i = 1; tag && i < n; i++) {
        snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[i]);
        nostr_tag_append(tag, v);
    }
    return tag;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: build the GroupContext extensions for KeyPackage capabilities
 * ──────────────────────────────────────────────────────────────────────── */

static int
build_kp_extensions(uint8_t **out_data, size_t *out_len)
{
    /*
     * KeyPackage extensions are the capabilities of this client.
     * For Marmot, we need to advertise support for:
     *   - 0xF2EE (marmot_group_data)
     *   - 0x000A (last_resort)
     *
     * Extensions are TLS-serialized as:
     *   opaque extensions<V> — a vector of Extension structs:
     *     uint16 extension_type
     *     opaque extension_data<V>
     */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 64) != 0) return -1;

    /* Extension: last_resort (0x000A) — empty data */
    if (mls_tls_write_u16(&buf, 0x000A) != 0) goto fail;
    if (mls_tls_write_opaque16(&buf, NULL, 0) != 0) goto fail;

    *out_data = buf.data;
    *out_len = buf.len;
    buf.data = NULL;
    return 0;

fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: ADOPTED profile KeyPackage (nostrc-prqu.9; kp_profile.h)
 * ──────────────────────────────────────────────────────────────────────── */

/* KeyPackage-level extensions: app_data_dictionary with the empty-data
 * last_resort_key_package component (libmarmot KeyPackages are last-resort
 * packages: the same slot is reused and init keys survive a Welcome).
 * Without @with_last_resort (libmarmot's lifecycle tests only,
 * nostrc-0bdg): none. */
static int
build_kp_extensions_adopted(bool with_last_resort, uint8_t **out_data, size_t *out_len)
{
    if (!with_last_resort) {
        *out_data = NULL;
        *out_len = 0;
        return 0;
    }
    MlsTlsBuf dict, buf;
    if (mls_tls_buf_init(&dict, 16) != 0) return -1;
    if (mls_tls_buf_init(&buf, 16) != 0) {
        mls_tls_buf_free(&dict);
        return -1;
    }
    MarmotComponentData last_resort = {MARMOT_COMPONENT_LAST_RESORT_KP, NULL, 0};
    if (marmot_app_data_dict_encode(&last_resort, 1, &dict) != 0 ||
        mls_tls_write_u16(&buf, MARMOT_EXT_APP_DATA_DICTIONARY) != 0 ||
        mls_tls_write_opaque32(&buf, dict.data, dict.len) != 0) {
        mls_tls_buf_free(&dict);
        mls_tls_buf_free(&buf);
        return -1;
    }
    mls_tls_buf_free(&dict);
    *out_data = buf.data;
    *out_len = buf.len;
    return 0;
}

static int
replace_u16_vec(uint16_t **arr, size_t *count, const uint16_t *vals, size_t n)
{
    uint16_t *copy = malloc(n * sizeof(uint16_t));
    if (!copy) return -1;
    memcpy(copy, vals, n * sizeof(uint16_t));
    free(*arr);
    *arr = copy;
    *count = n;
    return 0;
}

/* Leaf app_data_dictionary: app_components [0x0001] plus the adopted
 * components libmarmot supports (MLS_ADOPTED_SUPPORTED_COMPONENTS, incl.
 * 0x8009), safe_aad [] and the account-identity proof over the leaf's
 * signature key -- the entries MDK's cgka-engine emits.  An MDK 0.11
 * inviter requires 0x8001, 0x8003 and 0x800c (and 0x8004 for a Nostr
 * group) of every invitee (nostrc-qp24.5.1), a White Noise (marmot-app)
 * one also 0x8006 and 0x800b (nostrc-qp24.5.2). */
static MarmotError
build_leaf_dictionary_adopted(Marmot *m, MlsKeyPackage *kp, MlsKeyPackagePrivate *priv,
                              const uint8_t account_pk[32], const uint8_t *account_sk,
                              MarmotAccountSignFunc sign_fn, void *sign_data)
{
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    MarmotError err;
    if (account_sk || sign_fn) {
        err = marmot_account_proof_create(
            account_pk, account_sk, sign_fn, sign_data, kp->cipher_suite,
            MARMOT_SIGNATURE_SCHEME_ED25519, kp->leaf_node.signature_key, MLS_SIG_PK_LEN,
            (uint64_t)time(NULL), proof);
    } else if (marmot_account_proof_lookup(m, account_pk, proof)) {
        /* A signer-only caller (Groundhog, nostrc-0bdg): the enrolled
         * instance key and its proof, as the MDK 0.8 profile does. */
        memcpy(kp->leaf_node.signature_key, m->ed25519_pk, MLS_SIG_PK_LEN);
        memcpy(priv->signature_key_private, m->ed25519_sk, MLS_SIG_SK_LEN);
        err = MARMOT_OK;
    } else {
        return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    }
    if (err == MARMOT_OK) err = marmot_leaf_set_adopted_proof(&kp->leaf_node, proof);
    sodium_memzero(proof, sizeof(proof));
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Account binding of MDK 0.8 profile leaves (nostrc-7vyi)
 *
 * The kind:30443 signature binds a KeyPackage to its account only for the
 * inviter, who sees the event. Every other member gets the bare leaf in the
 * Commit's Add, and a joiner gets it in the Welcome's tree: they can check
 * the binding only if the leaf carries it. So the MDK 0.8 profile leaf
 * carries marmot.member.account-identity-proof.v2 too, and receivers require
 * it (marmot_commit_authorize(), the Welcome join).
 * ──────────────────────────────────────────────────────────────────────── */

bool
marmot_account_proof_lookup(const Marmot *m, const uint8_t owner[32],
                            uint8_t out[MARMOT_ACCOUNT_PROOF_LEN])
{
    if (!m || !owner || !m->account_proof_ready ||
        memcmp(m->account_proof_owner, owner, 32) != 0)
        return false;
    if (out) memcpy(out, m->account_proof, MARMOT_ACCOUNT_PROOF_LEN);
    return true;
}

static void
account_proof_remember(Marmot *m, const uint8_t owner[32],
                       const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN])
{
    memcpy(m->account_proof_owner, owner, 32);
    memcpy(m->account_proof, proof, MARMOT_ACCOUNT_PROOF_LEN);
    m->account_proof_ready = true;
}

/* The proof on a new MDK 0.8 KeyPackage leaf: signed now with the account key
 * or @sign_fn over the leaf's fresh key; else this instance's enrolled key
 * and proof (marmot_set_account_proof()); else, only with
 * MarmotConfig.allow_unproven_self, no proof. The leaf is signed after. */
static MarmotError
prove_key_package_leaf(Marmot *m, MlsKeyPackage *kp, MlsKeyPackagePrivate *priv,
                       const uint8_t account_pk[32], const uint8_t *account_sk,
                       MarmotAccountSignFunc sign_fn, void *sign_data)
{
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    MarmotError err;
    if (account_sk || sign_fn) {
        err = marmot_account_proof_create(account_pk, account_sk, sign_fn, sign_data,
                                          kp->cipher_suite, MARMOT_SIGNATURE_SCHEME_ED25519,
                                          kp->leaf_node.signature_key, MLS_SIG_PK_LEN,
                                          (uint64_t)time(NULL), proof);
    } else if (marmot_account_proof_lookup(m, account_pk, proof)) {
        memcpy(kp->leaf_node.signature_key, m->ed25519_pk, MLS_SIG_PK_LEN);
        memcpy(priv->signature_key_private, m->ed25519_sk, MLS_SIG_SK_LEN);
        err = MARMOT_OK;
    } else {
        return m->config.allow_unproven_self ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    }
    if (err == MARMOT_OK) err = marmot_leaf_set_proof(&kp->leaf_node, proof);
    sodium_memzero(proof, sizeof(proof));
    return err;
}

/* With the account key at hand, also enroll this instance's leaf key once,
 * so the groups it creates start with a proven creator leaf. */
static MarmotError
enroll_with_account_key(Marmot *m, const uint8_t account_pk[32], const uint8_t account_sk[32])
{
    if (m->account_proof_ready) return MARMOT_OK;
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    MarmotError err = marmot_account_proof_create(account_pk, account_sk, NULL, NULL,
                                                  MARMOT_CIPHERSUITE,
                                                  MARMOT_SIGNATURE_SCHEME_ED25519,
                                                  m->ed25519_pk, MLS_SIG_PK_LEN,
                                                  (uint64_t)time(NULL), proof);
    if (err == MARMOT_OK) account_proof_remember(m, account_pk, proof);
    sodium_memzero(proof, sizeof(proof));
    return err;
}

MarmotError
marmot_account_proof_template(Marmot *m, const uint8_t account_pubkey[32],
                              char **out_unsigned_event_json)
{
    if (!m || !account_pubkey || !out_unsigned_event_json) return MARMOT_ERR_INVALID_ARG;
    *out_unsigned_event_json = NULL;
    if (marmot_ensure_identity(m) != 0) return MARMOT_ERR_CRYPTO;
    int64_t now = marmot_now();
    *out_unsigned_event_json = marmot_account_proof_template_json(
        account_pubkey, (uint64_t)(now > 0 ? now : 1), m->ed25519_pk, MLS_SIG_PK_LEN);
    return *out_unsigned_event_json ? MARMOT_OK : MARMOT_ERR_MEMORY;
}

MarmotError
marmot_set_account_proof(Marmot *m, const uint8_t account_pubkey[32],
                         const char *signed_event_json)
{
    if (!m || !account_pubkey || !signed_event_json) return MARMOT_ERR_INVALID_ARG;
    if (marmot_ensure_identity(m) != 0) return MARMOT_ERR_CRYPTO;
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    MarmotError err = marmot_account_proof_from_signed(account_pubkey, m->ed25519_pk,
                                                       MLS_SIG_PK_LEN, signed_event_json,
                                                       proof);
    if (err == MARMOT_OK) account_proof_remember(m, account_pubkey, proof);
    sodium_memzero(proof, sizeof(proof));
    return err;
}

bool
marmot_has_account_proof(Marmot *m, const uint8_t account_pubkey[32])
{
    return marmot_account_proof_lookup(m, account_pubkey, NULL);
}

static MarmotError
create_mls_key_package_adopted(Marmot *m, MlsKeyPackage *kp, MlsKeyPackagePrivate *priv,
                               const uint8_t account_pk[32], const uint8_t *account_sk,
                               MarmotAccountSignFunc sign_fn, void *sign_data,
                               bool last_resort)
{
    uint8_t *ext_data = NULL;
    size_t ext_len = 0;
    if (build_kp_extensions_adopted(last_resort, &ext_data, &ext_len) != 0)
        return MARMOT_ERR_MEMORY;
    int rc = mls_key_package_create_unsigned(kp, priv, account_pk, 32, ext_data, ext_len);
    free(ext_data);
    if (rc != 0) return MARMOT_ERR_MLS;

    /* Adopted-profile capabilities (foundation/key-packages.md "Capability
     * advertising"), those of every adopted libmarmot leaf
     * (mls_leaf_node_set_adopted_capabilities()): app_data_dictionary and
     * the agent-stream receive role 0xF2D1; app_data_update and self_remove
     * (nostrc-2um6) -- what an MDK 0.11 marmot-app (White Noise) creator
     * requires of an invitee (nostrc-qp24.5.2). No 0xf2ee, no last_resort
     * extension type (last resort is a component now). */
    MarmotError err = MARMOT_ERR_MEMORY;
    if (replace_u16_vec(&kp->leaf_node.cap_extensions, &kp->leaf_node.cap_extension_count,
                        MLS_ADOPTED_CAP_EXTENSIONS, MLS_ADOPTED_CAP_EXTENSION_COUNT) != 0 ||
        replace_u16_vec(&kp->leaf_node.proposals, &kp->leaf_node.proposal_count,
                        MLS_ADOPTED_CAP_PROPOSALS, MLS_ADOPTED_CAP_PROPOSAL_COUNT) != 0)
        goto fail;
    err = build_leaf_dictionary_adopted(m, kp, priv, account_pk, account_sk, sign_fn,
                                        sign_data);
    if (err != MARMOT_OK) goto fail;
    if (mls_key_package_sign(kp, priv) != 0) {
        err = MARMOT_ERR_CRYPTO;
        goto fail;
    }
    return MARMOT_OK;
fail:
    mls_key_package_clear(kp);
    mls_key_package_private_clear(priv);
    return err;
}

/* app_components ids of a leaf's app_data_dictionary (NULL/0 if none). */
static int
leaf_app_components(const MlsLeafNode *leaf, uint16_t **ids, size_t *n)
{
    *ids = NULL;
    *n = 0;
    const uint8_t *dict = NULL;
    size_t dlen = 0, count = 0;
    if (marmot_extensions_find(leaf->extensions_data, leaf->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &dict, &dlen, &count) != 0 ||
        count != 1)
        return -1;
    MarmotComponentData *entries = NULL;
    size_t ne = 0;
    if (marmot_app_data_dict_parse(dict, dlen, &entries, &ne) != 0) return -1;
    int rc = -1;
    for (size_t i = 0; i < ne; i++)
        if (entries[i].component_id == MARMOT_COMPONENT_APP_COMPONENTS)
            rc = marmot_components_list_decode(entries[i].data, entries[i].len, ids, n);
    free(entries);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_key_package
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
create_key_package_common_impl(Marmot *m,
                          MarmotKeyPackageProfile profile,
                          const uint8_t nostr_pubkey[32],
                          const uint8_t nostr_sk[32],
                          bool sign_event,
                          MarmotAccountSignFunc account_sign, void *sign_data,
                          const char **relay_urls, size_t relay_count,
                          bool last_resort,
                          MarmotKeyPackageResult *result)
{
    if (!m || !nostr_pubkey || !result || (sign_event && !nostr_sk))
        return MARMOT_ERR_INVALID_ARG;
    if (profile != MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8 &&
        profile != MARMOT_KEY_PACKAGE_PROFILE_ADOPTED)
        return MARMOT_ERR_INVALID_ARG;
    const bool adopted = profile == MARMOT_KEY_PACKAGE_PROFILE_ADOPTED;
    if (adopted && !nostr_sk && !account_sign &&
        !marmot_account_proof_lookup(m, nostr_pubkey, NULL))
        return MARMOT_ERR_INVALID_ARG;
    /* MDK 0.8 profile KeyPackages always carry the last_resort extension. */
    if (!adopted) last_resort = true;
    if (relay_count > 0 && !relay_urls)
        return MARMOT_ERR_INVALID_ARG;

    memset(result, 0, sizeof(*result));

    if (!m->storage || !m->storage->mls_store || !m->storage->mls_load ||
        !m->storage->mls_delete || !m->storage->save_key_package_info ||
        !m->storage->deactivate_key_packages)
        return MARMOT_ERR_STORAGE;

    /* Ensure MLS identity is ready */
    if (marmot_ensure_identity(m) != 0)
        return MARMOT_ERR_CRYPTO;

    /* Resolve the account's publication slot before creating key material,
     * so a storage failure here leaves nothing to clean up. */
    uint8_t slot[MARMOT_KP_SLOT_LEN];
    MarmotError slot_err = load_or_create_key_package_slot(m, nostr_pubkey, adopted, slot);
    if (slot_err != MARMOT_OK)
        return slot_err;
    /* A replacement is strictly newer than the slot's last event
     * (nostrc-0bdg, kp_lifecycle.c). */
    int64_t created_at = 0;
    slot_err = marmot_kp_lifecycle_created_at(m, nostr_pubkey, marmot_now(), &created_at);
    if (slot_err != MARMOT_OK)
        return slot_err;

    /* Create MLS KeyPackage */
    MlsKeyPackage kp;
    MlsKeyPackagePrivate kp_priv;
    memset(&kp, 0, sizeof(kp));
    memset(&kp_priv, 0, sizeof(kp_priv));

    if (adopted) {
        MarmotError aerr = create_mls_key_package_adopted(m, &kp, &kp_priv, nostr_pubkey,
                                                          nostr_sk, account_sign, sign_data,
                                                          last_resort);
        if (aerr != MARMOT_OK) return aerr;
    } else {
        /* Build KeyPackage extensions (last_resort) */
        uint8_t *ext_data = NULL;
        size_t ext_len = 0;
        if (build_kp_extensions(&ext_data, &ext_len) != 0)
            return MARMOT_ERR_MEMORY;
        int rc = mls_key_package_create_unsigned(&kp, &kp_priv,
                                                 nostr_pubkey, 32,
                                                 ext_data, ext_len);
        free(ext_data);
        if (rc != 0) return MARMOT_ERR_MLS;
        /* The leaf carries its account proof (nostrc-7vyi). */
        MarmotError perr = prove_key_package_leaf(m, &kp, &kp_priv, nostr_pubkey, nostr_sk,
                                                  account_sign, sign_data);
        if (perr == MARMOT_OK && nostr_sk)
            perr = enroll_with_account_key(m, nostr_pubkey, nostr_sk);
        if (perr == MARMOT_OK && mls_key_package_sign(&kp, &kp_priv) != 0)
            perr = MARMOT_ERR_CRYPTO;
        if (perr != MARMOT_OK) {
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return perr;
        }
    }

    /* Compute KeyPackageRef */
    uint8_t kp_ref[MLS_HASH_LEN];
    if (mls_key_package_ref(&kp, kp_ref) != 0) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MLS;
    }
    memcpy(result->key_package_ref, kp_ref, MLS_HASH_LEN);

    /* TLS-serialize the KeyPackage */
    MlsTlsBuf tls_buf;
    if (mls_tls_buf_init(&tls_buf, 1024) != 0) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }
    /* Content: the bare KeyPackage (MDK 0.8) or an MLSMessage with
     * wire_format mls_key_package (adopted profile). The KeyPackageRef above
     * is over the inner KeyPackage either way. */
    if ((adopted ? marmot_mls_message_frame_key_package(&kp, &tls_buf)
                 : mls_key_package_serialize(&kp, &tls_buf)) != 0) {
        mls_tls_buf_free(&tls_buf);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_TLS_CODEC;
    }

    /* Base64-encode */
    char *b64_content = marmot_base64_encode(tls_buf.data, tls_buf.len);
    mls_tls_buf_free(&tls_buf);
    if (!b64_content) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* Build the kind:30443 Nostr event */
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(b64_content);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* Set event fields */
    nostr_event_set_kind(event, MARMOT_KIND_KEY_PACKAGE);

    /* Set pubkey from the nostr pubkey */
    char *pubkey_hex = marmot_hex_encode(nostr_pubkey, 32);
    nostr_event_set_pubkey(event, pubkey_hex);
    free(pubkey_hex);

    /* Set content to base64-encoded KeyPackage */
    nostr_event_set_content(event, b64_content);
    free(b64_content);

    /* Set created_at */
    nostr_event_set_created_at(event, created_at);

    /* Build tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(event);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    /* d tag: stable publication slot (addressable replacement key) */
    char *slot_hex = marmot_hex_encode(slot, MARMOT_KP_SLOT_LEN);
    NostrTag *tag = slot_hex ? nostr_tag_new("d", slot_hex, NULL) : NULL;
    free(slot_hex);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_protocol_version tag */
    tag = nostr_tag_new("mls_protocol_version", "1.0", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_ciphersuite id-list tag */
    tag = nostr_tag_new("mls_ciphersuite", "0x0001", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_extensions id-list tag, derived from the signed LeafNode
     * capabilities so the advertisement cannot drift from what receivers
     * validate (nostrc-prqu.10): 0x0006 app_data_dictionary (the leaf's
     * account proof, nostrc-7vyi), 0x000a last_resort, 0xf2ee
     * marmot_group_data. MDK 0.8 requires the last two and allows others. */
    tag = id_list_tag_new("mls_extensions", kp.leaf_node.cap_extensions,
                          kp.leaf_node.cap_extension_count);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* mls_proposals id-list tag. MDK 0.8 parsers reject a kind:30443 whose
     * mls_proposals is anything but exactly ["0x000a"] (SelfRemove). Since
     * 0.12.0 the LeafNode lists SelfRemove too (nostrc-2um6), so tag and
     * leaf agree; before, the tag was only an advertisement the leaf did not
     * back.  MDK derives a group's required proposals from the decoded
     * leaves (intersection), so MDK groups with libmarmot members now
     * require SelfRemove. */
    tag = adopted ? id_list_tag_new("mls_proposals", kp.leaf_node.proposals,
                                    kp.leaf_node.proposal_count)
                  : nostr_tag_new("mls_proposals", "0x000a", NULL);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* app_components id-list tag (adopted profile only): the signed
     * LeafNode's private-use (>= 0x8000) app_components, includes 0x8009.
     * MDK 0.11 refuses a tag that is not exactly that set (nostrc-qp24.5.1). */
    if (adopted) {
        uint16_t *ids = NULL;
        size_t n_ids = 0;
        if (leaf_app_components(&kp.leaf_node, &ids, &n_ids) != 0) goto tag_fail;
        size_t n_private = 0;
        for (size_t i = 0; i < n_ids; i++)
            if (ids[i] >= MARMOT_COMPONENT_PRIVATE_USE_START) ids[n_private++] = ids[i];
        tag = id_list_tag_new("app_components", ids, n_private);
        free(ids);
        if (!tag) goto tag_fail;
        nostr_tags_append(tags, tag);
    }

    /* relays tag (MDK 0.8 profile only; omitted when no relays are
     * supplied). The adopted profile discovers KeyPackage relays through
     * kind 10002 and forbids repeating them here. */
    if (!adopted && relay_count > 0) {
        NostrTag *relay_tag = nostr_tag_new("relays", relay_urls[0], NULL);
        if (!relay_tag) goto tag_fail;
        for (size_t i = 1; i < relay_count; i++) {
            nostr_tag_append(relay_tag, relay_urls[i]);
        }
        nostr_tags_append(tags, relay_tag);
    }

    /* i tag: hex-encoded KeyPackageRef */
    char *kp_ref_hex = marmot_hex_encode(kp_ref, MLS_HASH_LEN);
    tag = kp_ref_hex ? nostr_tag_new("i", kp_ref_hex, NULL) : NULL;
    free(kp_ref_hex);
    if (!tag) goto tag_fail;
    nostr_tags_append(tags, tag);

    /* encoding tag (MDK 0.8 parsers require it; the adopted profile forbids
     * it: "A sender MUST NOT add an encoding tag") */
    if (!adopted) {
        tag = nostr_tag_new("encoding", "base64", NULL);
        if (!tag) goto tag_fail;
        nostr_tags_append(tags, tag);
    }

    /* No NIP-70 "-" tag: it is not part of the kind:30443 tag set, and
     * relays without NIP-42 AUTH reject protected events, which would make
     * the KeyPackage undiscoverable. */

    nostr_event_set_tags(event, tags);

    if (sign_event) {
        char *sk_hex = marmot_hex_encode(nostr_sk, 32);
        if (!sk_hex) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_MEMORY;
        }
        if (nostr_event_sign(event, sk_hex) != 0) {
            free(sk_hex);
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_CRYPTO;
        }
        free(sk_hex);

        char *expected_pubkey = marmot_hex_encode(nostr_pubkey, 32);
        if (!expected_pubkey) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_MEMORY;
        }
        bool pubkey_matches = event->pubkey && strcmp(event->pubkey, expected_pubkey) == 0;
        free(expected_pubkey);
        if (!pubkey_matches) {
            nostr_event_free(event);
            mls_key_package_clear(&kp);
            mls_key_package_private_clear(&kp_priv);
            return MARMOT_ERR_VALIDATION;
        }
    }

    result->event_json = nostr_event_serialize_compact(event);
    nostr_event_free(event);

    if (!result->event_json) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }

    goto success;

tag_fail:
    nostr_tags_free(tags);
    nostr_event_free(event);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&kp_priv);
    return MARMOT_ERR_MEMORY;

success:
    ;

    /* Store the KeyPackage private material in storage for later Welcome
     * processing. These writes are mandatory: accepting a Welcome for this
     * KeyPackage requires both the private-key blob and full KeyPackage. */
    MarmotError err;
    uint8_t priv_blob[MLS_KEM_SK_LEN + MLS_KEM_SK_LEN + MLS_SIG_SK_LEN];
    memcpy(priv_blob, kp_priv.init_key_private, MLS_KEM_SK_LEN);
    memcpy(priv_blob + MLS_KEM_SK_LEN, kp_priv.encryption_key_private, MLS_KEM_SK_LEN);
    memcpy(priv_blob + MLS_KEM_SK_LEN + MLS_KEM_SK_LEN,
           kp_priv.signature_key_private, MLS_SIG_SK_LEN);

    err = m->storage->mls_store(m->storage->ctx, "kp_priv",
                                kp_ref, MLS_HASH_LEN,
                                priv_blob, sizeof(priv_blob));
    sodium_memzero(priv_blob, sizeof(priv_blob));
    if (err != MARMOT_OK) {
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    MlsTlsBuf kp_buf;
    if (mls_tls_buf_init(&kp_buf, 1024) != 0) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_MEMORY;
    }
    if (mls_key_package_serialize(&kp, &kp_buf) != 0) {
        mls_tls_buf_free(&kp_buf);
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return MARMOT_ERR_TLS_CODEC;
    }
    err = m->storage->mls_store(m->storage->ctx, "kp_full",
                                kp_ref, MLS_HASH_LEN,
                                kp_buf.data, kp_buf.len);
    mls_tls_buf_free(&kp_buf);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    /* Transport lifecycle (nostrc-0bdg, kp_lifecycle.c): the newest,
     * unconfirmed KeyPackage of the account's slot. Older ones keep their
     * private material until a relay accepts a newer one
     * (marmot_key_package_confirm_published()). */
    err = marmot_kp_lifecycle_register(m, nostr_pubkey, kp_ref,
                                       kp.leaf_node.lifetime_not_after, last_resort, adopted,
                                       created_at);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        m->storage->mls_delete(m->storage->ctx, "kp_full", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    /* Deactivate previous key packages for this pubkey (rotation). Mandatory
     * so callers do not publish a new active package while older ones remain
     * active after a failed storage write. */
    err = m->storage->deactivate_key_packages(m->storage->ctx, nostr_pubkey);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        m->storage->mls_delete(m->storage->ctx, "kp_full", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    MarmotKeyPackageInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.ref, kp_ref, 32);
    memcpy(info.owner_pubkey, nostr_pubkey, 32);
    info.created_at = marmot_now();
    info.active = true;
    if (relay_count > 0 && relay_urls) {
        info.relay_urls = (char **)relay_urls;  /* borrowed for save */
        info.relay_count = relay_count;
    }
    err = m->storage->save_key_package_info(m->storage->ctx, &info);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "kp_priv", kp_ref, MLS_HASH_LEN);
        m->storage->mls_delete(m->storage->ctx, "kp_full", kp_ref, MLS_HASH_LEN);
        marmot_key_package_result_free(result);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&kp_priv);
        return err;
    }

    /* Clean up */
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&kp_priv);

    return MARMOT_OK;
}

/* The KeyPackage's private keys, its record and the account's slot are
 * stored in one transaction (nostrc-qp24.7). */
static MarmotError
create_key_package_common(Marmot *m,
                          MarmotKeyPackageProfile profile,
                          const uint8_t nostr_pubkey[32],
                          const uint8_t nostr_sk[32],
                          bool sign_event,
                          MarmotAccountSignFunc account_sign, void *sign_data,
                          const char **relay_urls, size_t relay_count,
                          bool last_resort,
                          MarmotKeyPackageResult *result)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = create_key_package_common_impl(m, profile, nostr_pubkey, nostr_sk, sign_event,
                                         account_sign, sign_data, relay_urls,
                                         relay_count, last_resort, result);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) marmot_key_package_result_free(result);
    return end;
}

MarmotError
marmot_create_key_package(Marmot *m,
                           const uint8_t nostr_pubkey[32],
                           const uint8_t nostr_sk[32],
                           const char **relay_urls, size_t relay_count,
                           MarmotKeyPackageResult *result)
{
    return create_key_package_common(m, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                     nostr_pubkey, nostr_sk, true, NULL, NULL,
                                     relay_urls, relay_count, true, result);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_key_package_unsigned
 *
 * Identical to marmot_create_key_package but does not require the secret
 * key.  The SK parameter was never consumed by the signed variant anyway
 * (it was only validated), so this is a thin wrapper that makes the
 * signer-only contract explicit.
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_key_package_unsigned(Marmot *m,
                                    const uint8_t nostr_pubkey[32],
                                    const char **relay_urls, size_t relay_count,
                                    MarmotKeyPackageResult *result)
{
    if (!m || !nostr_pubkey || !result)
        return MARMOT_ERR_INVALID_ARG;

    return create_key_package_common(m, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                     nostr_pubkey, NULL, false, NULL, NULL,
                                     relay_urls, relay_count, true, result);
}

MarmotError
marmot_create_key_package_for_profile(Marmot *m,
                                       MarmotKeyPackageProfile profile,
                                       const uint8_t nostr_pubkey[32],
                                       const uint8_t nostr_sk[32],
                                       MarmotAccountSignFunc account_sign,
                                       void *sign_data,
                                       const char **relay_urls, size_t relay_count,
                                       MarmotKeyPackageResult *result)
{
#ifndef MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER
    /* Built without the adopted producer (CMake MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER
     * off; on by default since 0.12.0, nostrc-lf62): MDK 0.8 KeyPackages only. */
    if (profile == MARMOT_KEY_PACKAGE_PROFILE_ADOPTED)
        return MARMOT_ERR_UNSUPPORTED;
#endif
    return create_key_package_common(m, profile, nostr_pubkey, nostr_sk, nostr_sk != NULL,
                                     account_sign, sign_data, relay_urls, relay_count, true,
                                     result);
}

/* Ungated producer for libmarmot's own tests (not in a public header). */
MarmotError
marmot_create_key_package_adopted_internal(Marmot *m, const uint8_t nostr_pubkey[32],
                                           const uint8_t nostr_sk[32],
                                           MarmotAccountSignFunc account_sign, void *sign_data,
                                           MarmotKeyPackageResult *result)
{
    return create_key_package_common(m, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, nostr_pubkey,
                                     nostr_sk, nostr_sk != NULL, account_sign, sign_data,
                                     NULL, 0, true, result);
}

/* The same with the last-resort marker chosen (lifecycle tests, nostrc-0bdg). */
MarmotError
marmot_create_key_package_adopted_internal_ex(Marmot *m, const uint8_t nostr_pubkey[32],
                                              const uint8_t nostr_sk[32],
                                              MarmotAccountSignFunc account_sign,
                                              void *sign_data, bool last_resort,
                                              MarmotKeyPackageResult *result)
{
    return create_key_package_common(m, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, nostr_pubkey,
                                     nostr_sk, nostr_sk != NULL, account_sign, sign_data,
                                     NULL, 0, last_resort, result);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: validate an authenticated kind:30443 event and extract the
 * MlsKeyPackage.
 *
 * The caller MUST have verified the NIP-01 id and signature first (see
 * marmot_parse_key_package_event); this function only checks the event
 * shape, the KeyPackage bytes, author binding and the KeyPackageRef. It is
 * exported (not in public headers) so interop tests can check MDK vector
 * events, which ship tags and content but no signature.
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_validate_key_package_event(NostrEvent *event,
                                  MlsKeyPackage *kp_out,
                                  uint8_t nostr_pubkey_out[32])
{
    if (!event || !kp_out) return MARMOT_ERR_INVALID_ARG;

    uint8_t event_pubkey[32];
    uint8_t expected_ref[MLS_HASH_LEN];

    /* Legacy kind 443 is rejected: removed by the adopted Marmot spec. */
    if (event->kind != MARMOT_KIND_KEY_PACKAGE)
        return MARMOT_ERR_UNEXPECTED_EVENT;

    if (!is_hex_len(event->pubkey, 64) ||
        marmot_hex_decode(event->pubkey, event_pubkey, sizeof(event_pubkey)) != 0)
        return MARMOT_ERR_VALIDATION;

    /* Addressable slot: exactly one d tag, 32 bytes as lowercase hex. */
    const char *d = singleton_tag_value(event->tags, "d");
    if (!is_lower_hex_len(d, 2 * MARMOT_KP_SLOT_LEN))
        return MARMOT_ERR_VALIDATION;

    const char *version = singleton_tag_value(event->tags, "mls_protocol_version");
    if (!version || strcmp(version, "1.0") != 0)
        return MARMOT_ERR_VALIDATION;

    if (!id_list_tag_valid(event->tags, "mls_ciphersuite") ||
        !id_list_tag_valid(event->tags, "mls_extensions") ||
        !id_list_tag_valid(event->tags, "mls_proposals"))
        return MARMOT_ERR_VALIDATION;

    if (!relays_tag_valid(event->tags))
        return MARMOT_ERR_VALIDATION;

    const char *ref_hex = singleton_tag_value(event->tags, "i");
    if (!is_lower_hex_len(ref_hex, 2 * MLS_HASH_LEN))
        return MARMOT_ERR_VALIDATION;

    /* Content encoding of the MDK 0.8 profile emitted by this library. */
    const char *encoding = singleton_tag_value(event->tags, "encoding");
    if (!encoding || strcmp(encoding, "base64") != 0)
        return MARMOT_ERR_VALIDATION;

    if (!event->content || event->content[0] == '\0')
        return MARMOT_ERR_DESERIALIZATION;

    size_t kp_len = 0;
    uint8_t *kp_data = marmot_base64_decode(event->content, &kp_len);
    if (!kp_data)
        return MARMOT_ERR_DESERIALIZATION;

    MarmotError err = MARMOT_OK;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, kp_data, kp_len);
    memset(kp_out, 0, sizeof(*kp_out));
    int rc = mls_key_package_deserialize(&reader, kp_out);
    free(kp_data);
    if (rc != 0)
        return MARMOT_ERR_MLS;

    rc = mls_key_package_validate(kp_out);
    if (rc != 0) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_kp;
    }

    if (kp_out->leaf_node.credential_identity_len != 32 ||
        !kp_out->leaf_node.credential_identity ||
        memcmp(kp_out->leaf_node.credential_identity, event_pubkey, 32) != 0) {
        err = MARMOT_ERR_AUTHOR_MISMATCH;
        goto fail_kp;
    }

    /* An account proof on the leaf must verify (nostrc-7vyi). Whether one
     * is required is the inviter's policy: see parse_key_packages(). */
    if (marmot_leaf_proof_status(&kp_out->leaf_node, kp_out->cipher_suite) ==
        MARMOT_LEAF_PROOF_INVALID) {
        err = MARMOT_ERR_KEY_PACKAGE_IDENTITY;
        goto fail_kp;
    }

    if (mls_key_package_ref(kp_out, expected_ref) != 0) {
        err = MARMOT_ERR_MLS;
        goto fail_kp;
    }
    char *expected_ref_hex = marmot_hex_encode(expected_ref, MLS_HASH_LEN);
    if (!expected_ref_hex) {
        err = MARMOT_ERR_MEMORY;
        goto fail_kp;
    }
    bool ref_ok = strcmp(expected_ref_hex, ref_hex) == 0;
    free(expected_ref_hex);
    if (!ref_ok) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_kp;
    }

    if (nostr_pubkey_out) memcpy(nostr_pubkey_out, event_pubkey, 32);
    return MARMOT_OK;

fail_kp:
    mls_key_package_clear(kp_out);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: ADOPTED profile validation (nostrc-prqu.9). Same contract as
 * marmot_validate_key_package_event() (id + signature verified by the
 * caller), plus the adopted rules of transports/nostr.md,
 * foundation/key-packages.md and account-identity-proof-v2.md @26fa6a6.
 * ──────────────────────────────────────────────────────────────────────── */

bool
marmot_mls_is_grease(uint16_t id)
{
    /* RFC 9420 section 13.5: 0x0A0A, 0x1A1A, ..., 0xEAEA. */
    return (id & 0x0F0F) == 0x0A0A && (id >> 12) == ((id >> 4) & 0xF) && id <= 0xEAEA;
}

/* The one `key` id-list tag names exactly @ids as a set (MDK 0.11
 * require_multi_value_key_package_tag_matches(); nostrc-0bdg review L4),
 * leaving out GREASE ids when @skip_grease (MDK's
 * advertised_capabilities_from_caps() does for extensions, review A2).
 * The tag's values are pairwise distinct (id_list_tag_valid()). */
bool
marmot_kp_id_list_tag_is_set(NostrTags *tags, const char *key, const uint16_t *ids, size_t n,
                             bool skip_grease)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1) return false;
    size_t distinct = 0;
    for (size_t i = 0; i < n; i++) {
        if (skip_grease && marmot_mls_is_grease(ids[i])) continue;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; j++) seen = ids[j] == ids[i];
        if (seen) continue;
        distinct++;
        char v[7];
        snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[i]);
        bool found = false;
        for (size_t k = 1; k < nostr_tag_size(tag) && !found; k++)
            found = strcmp(nostr_tag_get(tag, k), v) == 0;
        if (!found) return false;
    }
    return nostr_tag_size(tag) - 1 == distinct;
}

static bool
id_list_tag_contains(NostrTags *tags, const char *key, const char *value)
{
    NostrTag *tag = NULL;
    if (count_tags(tags, key, &tag) != 1) return false;
    for (size_t i = 1; i < nostr_tag_size(tag); i++)
        if (strcmp(nostr_tag_get(tag, i), value) == 0) return true;
    return false;
}

static MarmotError
validate_leaf_adopted(const MlsKeyPackage *kp)
{
    const MlsLeafNode *leaf = &kp->leaf_node;
    /* Capabilities: app_data_dictionary + app_data_update. */
    if (!marmot_u16_list_contains(leaf->cap_extensions, leaf->cap_extension_count,
                                  MARMOT_EXT_APP_DATA_DICTIONARY) ||
        !marmot_u16_list_contains(leaf->proposals, leaf->proposal_count,
                                  MARMOT_PROPOSAL_APP_DATA_UPDATE))
        return MARMOT_ERR_KEY_PACKAGE;

    /* Exactly one LeafNode app_data_dictionary, advertising 0x8009, whose
     * proof binds the credential identity to this leaf's signature key under
     * the KeyPackage ciphersuite. */
    if (marmot_leaf_proof_status(leaf, kp->cipher_suite) != MARMOT_LEAF_PROOF_VALID)
        return MARMOT_ERR_KEY_PACKAGE;

    MarmotError err = MARMOT_ERR_KEY_PACKAGE;
    /* KeyPackage-level extensions: the proof is invalid there; the only
     * dictionary entry we accept is the empty last-resort marker. */
    const uint8_t *kdict = NULL;
    size_t kdlen = 0, kcount = 0;
    if (marmot_extensions_find(kp->extensions_data, kp->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &kdict, &kdlen, &kcount) != 0 ||
        kcount > 1)
        goto out;
    if (kcount == 1) {
        MarmotComponentData *ke = NULL;
        size_t nke = 0;
        if (marmot_app_data_dict_parse(kdict, kdlen, &ke, &nke) != 0) goto out;
        bool ok = true;
        for (size_t i = 0; i < nke; i++)
            if (ke[i].component_id == MARMOT_COMPONENT_ACCOUNT_PROOF_V2 ||
                (ke[i].component_id == MARMOT_COMPONENT_LAST_RESORT_KP && ke[i].len != 0))
                ok = false;
        free(ke);
        if (!ok) goto out;
    }
    err = MARMOT_OK;
out:
    return err;
}

static MarmotError
validate_key_package_event_adopted(NostrEvent *event, int64_t now,
                                   MlsKeyPackage *kp_out, uint8_t nostr_pubkey_out[32])
{
    uint8_t event_pubkey[32];
    uint8_t expected_ref[MLS_HASH_LEN];

    if (event->kind != MARMOT_KIND_KEY_PACKAGE)
        return MARMOT_ERR_UNEXPECTED_EVENT;
    if (!is_hex_len(event->pubkey, 64) ||
        marmot_hex_decode(event->pubkey, event_pubkey, sizeof(event_pubkey)) != 0)
        return MARMOT_ERR_VALIDATION;

    /* Tag set (transports/nostr.md "KeyPackage publication"). */
    const char *d = singleton_tag_value(event->tags, "d");
    const char *version = singleton_tag_value(event->tags, "mls_protocol_version");
    const char *ref_hex = singleton_tag_value(event->tags, "i");
    if (!is_lower_hex_len(d, 2 * MARMOT_KP_SLOT_LEN) || !version ||
        strcmp(version, "1.0") != 0 || !is_lower_hex_len(ref_hex, 2 * MLS_HASH_LEN))
        return MARMOT_ERR_VALIDATION;
    /* mls_ciphersuite names the KeyPackage's one suite: a singleton
     * (nostrc-0bdg). */
    if (!id_list_tag_valid(event->tags, "mls_ciphersuite") ||
        !singleton_tag_value(event->tags, "mls_ciphersuite") ||
        !id_list_tag_valid(event->tags, "mls_extensions") ||
        !id_list_tag_valid(event->tags, "mls_proposals") ||
        !id_list_tag_valid(event->tags, "app_components") ||
        !id_list_tag_contains(event->tags, "app_components", "0x8009"))
        return MARMOT_ERR_VALIDATION;
    /* "A sender MUST NOT add an encoding tag"; KeyPackage events do not
     * repeat relays (discovery is kind 10002). */
    if (count_tags(event->tags, "encoding", NULL) != 0 ||
        count_tags(event->tags, "relays", NULL) != 0)
        return MARMOT_ERR_VALIDATION;

    if (!event->content || event->content[0] == '\0')
        return MARMOT_ERR_DESERIALIZATION;
    size_t len = 0;
    uint8_t *data = marmot_base64_decode(event->content, &len);
    if (!data)
        return MARMOT_ERR_DESERIALIZATION;
    int rc = marmot_mls_message_unframe_key_package(data, len, kp_out);
    free(data);
    if (rc != 0)
        return MARMOT_ERR_MLS;

    MarmotError err = MARMOT_ERR_VALIDATION;
    if (mls_key_package_validate(kp_out) != 0)
        goto fail_kp;
    if (kp_out->leaf_node.credential_identity_len != 32 ||
        !kp_out->leaf_node.credential_identity ||
        memcmp(kp_out->leaf_node.credential_identity, event_pubkey, 32) != 0) {
        err = MARMOT_ERR_AUTHOR_MISMATCH;
        goto fail_kp;
    }

    /* Lifetime: present (always, for key_package leaves), current, bounded. */
    uint64_t t = (uint64_t)(now > 0 ? now : (int64_t)time(NULL));
    uint64_t nb = kp_out->leaf_node.lifetime_not_before;
    uint64_t na = kp_out->leaf_node.lifetime_not_after;
    if (t < nb || t > na || na - nb > MARMOT_KP_LIFETIME_MAX_RANGE) {
        err = MARMOT_ERR_KEY_PACKAGE;
        goto fail_kp;
    }

    err = validate_leaf_adopted(kp_out);
    if (err != MARMOT_OK) goto fail_kp;

    /* The advertisement must not lie about the decoded KeyPackage: the
     * ciphersuite tag names the KeyPackage's suite and app_components is
     * exactly the leaf's private-use (>= 0x8000) app_components, as a set --
     * the upstream ids (0x0001 app_components itself) are discoverable from
     * the MLS capabilities and not published (MDK 0.11 marmot-app
     * key_package_records.rs, nostrc-qp24.5.1). */
    err = MARMOT_ERR_VALIDATION;
    {
        char suite[7];
        snprintf(suite, sizeof(suite), "0x%04x", (unsigned)kp_out->cipher_suite);
        if (!id_list_tag_contains(event->tags, "mls_ciphersuite", suite)) goto fail_kp;
        /* mls_extensions and mls_proposals are exactly the leaf's
         * capabilities (review L4, as MDK 0.11 checks). */
        if (!marmot_kp_id_list_tag_is_set(event->tags, "mls_extensions",
                                          kp_out->leaf_node.cap_extensions,
                                          kp_out->leaf_node.cap_extension_count, true) ||
            !marmot_kp_id_list_tag_is_set(event->tags, "mls_proposals",
                                          kp_out->leaf_node.proposals,
                                          kp_out->leaf_node.proposal_count, false))
            goto fail_kp;
        uint16_t *ids = NULL;
        size_t n_ids = 0;
        if (leaf_app_components(&kp_out->leaf_node, &ids, &n_ids) != 0) goto fail_kp;
        size_t n_private = 0;
        for (size_t i = 0; i < n_ids; i++)
            if (ids[i] >= MARMOT_COMPONENT_PRIVATE_USE_START) n_private++;
        NostrTag *ac = NULL;
        count_tags(event->tags, "app_components", &ac);
        bool same = ac && nostr_tag_size(ac) - 1 == n_private;
        for (size_t i = 0; same && i < n_ids; i++) {
            if (ids[i] < MARMOT_COMPONENT_PRIVATE_USE_START) continue;
            char v[7];
            snprintf(v, sizeof(v), "0x%04x", (unsigned)ids[i]);
            same = id_list_tag_contains(event->tags, "app_components", v);
        }
        free(ids);
        if (!same) goto fail_kp; /* tag ids are pairwise distinct (id_list_tag_valid) */
    }

    err = MARMOT_ERR_MLS;
    if (mls_key_package_ref(kp_out, expected_ref) != 0) goto fail_kp;
    err = MARMOT_ERR_MEMORY;
    char *expected_ref_hex = marmot_hex_encode(expected_ref, MLS_HASH_LEN);
    if (!expected_ref_hex) goto fail_kp;
    bool ref_ok = strcmp(expected_ref_hex, ref_hex) == 0;
    free(expected_ref_hex);
    if (!ref_ok) {
        err = MARMOT_ERR_VALIDATION;
        goto fail_kp;
    }
    if (nostr_pubkey_out) memcpy(nostr_pubkey_out, event_pubkey, 32);
    return MARMOT_OK;

fail_kp:
    mls_key_package_clear(kp_out);
    return err;
}

MarmotError
marmot_validate_key_package_event_profile(NostrEvent *event, MarmotKeyPackageProfile profile,
                                          int64_t now, MlsKeyPackage *kp_out,
                                          uint8_t nostr_pubkey_out[32])
{
    if (!event || !kp_out) return MARMOT_ERR_INVALID_ARG;
    switch (profile) {
    case MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8:
        return marmot_validate_key_package_event(event, kp_out, nostr_pubkey_out);
    case MARMOT_KEY_PACKAGE_PROFILE_ADOPTED:
        memset(kp_out, 0, sizeof(*kp_out));
        return validate_key_package_event_adopted(event, now, kp_out, nostr_pubkey_out);
    }
    return MARMOT_ERR_INVALID_ARG;
}

MarmotError
marmot_validate_key_package_event_json(const char *event_json,
                                        MarmotKeyPackageProfile profile,
                                        int64_t now,
                                        uint8_t owner_out[32],
                                        uint8_t ref_out[32])
{
    if (!event_json) return MARMOT_ERR_INVALID_ARG;
    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;
    MarmotError err = MARMOT_ERR_UNEXPECTED_EVENT;
    if (event.kind == MARMOT_KIND_KEY_PACKAGE) {
        err = verify_event_id_and_signature(&event);
        MlsKeyPackage kp;
        memset(&kp, 0, sizeof(kp));
        if (err == MARMOT_OK)
            err = marmot_validate_key_package_event_profile(&event, profile, now, &kp,
                                                            owner_out);
        if (err == MARMOT_OK) {
            if (ref_out && mls_key_package_ref(&kp, ref_out) != 0) err = MARMOT_ERR_MLS;
            mls_key_package_clear(&kp);
        }
    }
    clear_stack_event(&event);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: parse a signed kind:30443 event JSON and extract the
 * MlsKeyPackage (id + signature verified before any field is trusted).
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_parse_key_package_event_for_profile(const char *event_json,
                                           MarmotKeyPackageProfile profile, int64_t now,
                                           MlsKeyPackage *kp_out,
                                           uint8_t nostr_pubkey_out[32])
{
    if (!event_json || !kp_out) return MARMOT_ERR_INVALID_ARG;
    memset(kp_out, 0, sizeof(*kp_out));
    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;
    MarmotError err = MARMOT_ERR_UNEXPECTED_EVENT;
    if (event.kind == MARMOT_KIND_KEY_PACKAGE) {
        err = verify_event_id_and_signature(&event);
        if (err == MARMOT_OK)
            err = marmot_validate_key_package_event_profile(&event, profile, now, kp_out,
                                                            nostr_pubkey_out);
    }
    clear_stack_event(&event);
    return err;
}

MarmotError
marmot_parse_key_package_event(const char *event_json,
                                MlsKeyPackage *kp_out,
                                uint8_t nostr_pubkey_out[32])
{
    if (!event_json || !kp_out) return MARMOT_ERR_INVALID_ARG;

    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;

    MarmotError err = MARMOT_ERR_UNEXPECTED_EVENT;
    if (event.kind == MARMOT_KIND_KEY_PACKAGE) {
        err = verify_event_id_and_signature(&event);
        if (err == MARMOT_OK)
            err = marmot_validate_key_package_event(&event, kp_out, nostr_pubkey_out);
    }
    clear_stack_event(&event);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_key_package_event_has_account_proof
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_key_package_event_has_account_proof(const char *event_json, bool *out_proven)
{
    if (!event_json || !out_proven) return MARMOT_ERR_INVALID_ARG;
    *out_proven = false;
    MlsKeyPackage kp;
    memset(&kp, 0, sizeof(kp));
    MarmotError err = marmot_parse_key_package_event(event_json, &kp, NULL);
    if (err != MARMOT_OK) return err;
    *out_proven = marmot_leaf_proof_status(&kp.leaf_node, kp.cipher_suite) ==
                  MARMOT_LEAF_PROOF_VALID;
    mls_key_package_clear(&kp);
    return MARMOT_OK;
}

/* The KeyPackage an evidence event carries: base64 (MDK 0.8, the adopted
 * profile) or hex (the earliest kind:443 events), named by `encoding` or,
 * without one, hex only when the content is all hex; a bare KeyPackage or
 * an MLSMessage(mls_key_package) (adopted). */
static int
evidence_key_package(const NostrEvent *event, MlsKeyPackage *kp)
{
    const char *content = event->content;
    if (!content || !*content || strlen(content) > 2 * 65536) return -1;
    const char *encoding = singleton_tag_value(event->tags, "encoding");
    if (count_tags(event->tags, "encoding", NULL) > 1) return -1;
    size_t clen = strlen(content);
    bool hex = encoding ? strcmp(encoding, "hex") == 0
                        : (clen % 2 == 0 && is_hex_len(content, clen));
    if (encoding && !hex && strcmp(encoding, "base64") != 0) return -1;
    size_t len = 0;
    uint8_t *data = NULL;
    if (hex) {
        if (clen % 2 != 0) return -1;
        len = clen / 2;
        data = malloc(len ? len : 1);
        if (!data || marmot_hex_decode(content, data, len) != 0) {
            free(data);
            return -1;
        }
    } else {
        data = marmot_base64_decode(content, &len);
        if (!data) return -1;
    }
    int rc = -1;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);
    memset(kp, 0, sizeof(*kp));
    if (mls_key_package_deserialize(&reader, kp) == 0 && mls_tls_reader_done(&reader)) {
        rc = 0;
    } else {
        mls_key_package_clear(kp);
        memset(kp, 0, sizeof(*kp));
        rc = marmot_mls_message_unframe_key_package(data, len, kp) == 0 ? 0 : -1;
        if (rc != 0) mls_key_package_clear(kp);
    }
    free(data);
    return rc;
}

MarmotError
marmot_key_package_event_matches_member(const char *event_json,
                                         const MarmotMemberIdentity *member,
                                         bool *out_matches)
{
    if (!out_matches) return MARMOT_ERR_INVALID_ARG;
    *out_matches = false;
    if (!event_json || !member) return MARMOT_ERR_INVALID_ARG;

    NostrEvent event;
    memset(&event, 0, sizeof(event));
    if (!nostr_event_deserialize_compact(&event, event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;
    MlsKeyPackage kp;
    memset(&kp, 0, sizeof(kp));
    uint8_t author[32];
    bool have_kp = false;
    MarmotError err = MARMOT_OK;

    /* Kind 443 is no invitation KeyPackage any more (the strict 30443
     * parsers refuse it for Adds): here it is only evidence that its author
     * published this device's key. */
    if (event.kind != MARMOT_KIND_KEY_PACKAGE && event.kind != 443)
        err = MARMOT_ERR_UNEXPECTED_EVENT;
    if (err == MARMOT_OK) err = verify_event_id_and_signature(&event);
    if (err == MARMOT_OK && marmot_hex_decode(event.pubkey, author, 32) != 0)
        err = MARMOT_ERR_VALIDATION;
    if (err == MARMOT_OK) {
        if (evidence_key_package(&event, &kp) != 0) {
            err = MARMOT_ERR_KEY_PACKAGE;
        } else {
            have_kp = true;
            /* The KeyPackage's own signature and its leaf's (RFC 9420). */
            if (mls_key_package_validate(&kp) != 0)
                err = MARMOT_ERR_KEY_PACKAGE;
            else if (kp.leaf_node.credential_identity_len != 32 ||
                     !kp.leaf_node.credential_identity ||
                     memcmp(kp.leaf_node.credential_identity, author, 32) != 0)
                err = MARMOT_ERR_AUTHOR_MISMATCH;
            else if (marmot_leaf_proof_status(&kp.leaf_node, kp.cipher_suite) ==
                     MARMOT_LEAF_PROOF_INVALID)
                err = MARMOT_ERR_KEY_PACKAGE_IDENTITY;
        }
    }
    if (err == MARMOT_OK)
        *out_matches = sodium_memcmp(author, member->account_pubkey, 32) == 0 &&
                       sodium_memcmp(kp.leaf_node.signature_key, member->signature_key,
                                     32) == 0;
    if (have_kp) mls_key_package_clear(&kp);
    clear_stack_event(&event);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_select_key_package_event
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    NostrEvent event;
    uint8_t     author[32];
    const char *slot;          /* borrowed from event.tags */
    bool        authenticated; /* kind 30443, id + sig verified, has slot */
    bool        valid;         /* slot winner that passed full validation */
    uint8_t     ref[MLS_HASH_LEN];
} KeyPackageCandidate;

/* Is @a newer than @b under the addressable replacement rule? */
static bool
kp_candidate_newer(const KeyPackageCandidate *a, const KeyPackageCandidate *b)
{
    if (a->event.created_at != b->event.created_at)
        return a->event.created_at > b->event.created_at;
    return strcmp(a->event.id, b->event.id) < 0;
}

static MarmotError
select_key_package_event(const char **event_jsons, size_t count,
                         const uint8_t owner_pubkey[32],
                         MarmotKeyPackageProfile profile, size_t *out_index)
{
    if (!out_index || (count > 0 && !event_jsons))
        return MARMOT_ERR_INVALID_ARG;
    if (count == 0)
        return MARMOT_ERR_KEY_PACKAGE;

    KeyPackageCandidate *c = calloc(count, sizeof(*c));
    if (!c) return MARMOT_ERR_MEMORY;

    /* 1. Authenticate and locate each event's slot. */
    for (size_t i = 0; i < count; i++) {
        if (!event_jsons[i] ||
            !nostr_event_deserialize_compact(&c[i].event, event_jsons[i], NULL))
            continue;
        if (c[i].event.kind != MARMOT_KIND_KEY_PACKAGE ||
            verify_event_id_and_signature(&c[i].event) != MARMOT_OK)
            continue;
        if (marmot_hex_decode(c[i].event.pubkey, c[i].author, sizeof(c[i].author)) != 0 ||
            (owner_pubkey && memcmp(c[i].author, owner_pubkey, 32) != 0))
            continue;
        c[i].slot = singleton_tag_value(c[i].event.tags, "d");
        c[i].authenticated = c[i].slot != NULL;
    }

    /* 2 + 3. Keep each (pubkey, d) slot's newest event if it validates.
     * Authors compare as decoded bytes, slot ids as exact strings. */
    for (size_t i = 0; i < count; i++) {
        if (!c[i].authenticated) continue;
        bool superseded = false;
        for (size_t j = 0; j < count && !superseded; j++) {
            if (j == i || !c[j].authenticated) continue;
            if (memcmp(c[j].author, c[i].author, 32) != 0 ||
                strcmp(c[j].slot, c[i].slot) != 0)
                continue;
            superseded = kp_candidate_newer(&c[j], &c[i]);
        }
        if (superseded) continue;

        MlsKeyPackage kp;
        memset(&kp, 0, sizeof(kp));
        if (marmot_validate_key_package_event_profile(&c[i].event, profile, 0, &kp,
                                                      NULL) != MARMOT_OK)
            continue;
        mls_key_package_clear(&kp);
        const char *ref_hex = singleton_tag_value(c[i].event.tags, "i");
        c[i].valid = marmot_hex_decode(ref_hex, c[i].ref, sizeof(c[i].ref)) == 0;
    }

    /* 4. Newest valid slot winner; ties broken by lower KeyPackageRef. */
    size_t best = count;
    for (size_t i = 0; i < count; i++) {
        if (!c[i].valid) continue;
        if (best == count ||
            c[i].event.created_at > c[best].event.created_at ||
            (c[i].event.created_at == c[best].event.created_at &&
             memcmp(c[i].ref, c[best].ref, MLS_HASH_LEN) < 0))
            best = i;
    }

    for (size_t i = 0; i < count; i++)
        clear_stack_event(&c[i].event);
    free(c);

    if (best == count)
        return MARMOT_ERR_KEY_PACKAGE;
    *out_index = best;
    return MARMOT_OK;
}

MarmotError
marmot_select_key_package_event(const char **event_jsons,
                                size_t count,
                                const uint8_t owner_pubkey[32],
                                size_t *out_index)
{
    return select_key_package_event(event_jsons, count, owner_pubkey,
                                    MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8, out_index);
}

MarmotError
marmot_select_key_package_event_for_profile(const char **event_jsons,
                                            size_t count,
                                            const uint8_t owner_pubkey[32],
                                            MarmotKeyPackageProfile profile,
                                            size_t *out_index)
{
    if (profile != MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8 &&
        profile != MARMOT_KEY_PACKAGE_PROFILE_ADOPTED)
        return MARMOT_ERR_INVALID_ARG;
    return select_key_package_event(event_jsons, count, owner_pubkey, profile, out_index);
}
