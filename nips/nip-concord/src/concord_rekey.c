/* nip-concord: rekeys and Refoundings (CORD-06).
 *
 * Key rotation is asynchronous and non-ratcheted: a Rotator mints one fresh
 * key and hands it to everyone who keeps it, and a removed member is severed
 * by the plain fact of receiving nothing. Everything here serves that one
 * discipline — an address a current keyholder can precompute, a locator a
 * recipient finds their own blob by, a fixed-width plaintext whose width is
 * its own format declaration, and a continuity commitment that keeps honest
 * members advancing along one chain.
 */

#include "concord_internal.h"

#include <string.h>

#include <openssl/crypto.h>

static const uint8_t concord_zero_id[32] = { 0 };

static void concord_put_u64_be(uint64_t value, uint8_t out[8]) {
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
}

static uint64_t concord_get_u64_be(const uint8_t in[8]) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | in[i];
    return value;
}

/* ---------------- scope ---------------- */

void nostr_concord_base_scope_id(uint8_t out[32]) {
    if (!out) return;
    memcpy(out, concord_zero_id, 32);
}

bool nostr_concord_scope_is_base(const uint8_t scope_id[32]) {
    if (!scope_id) return false;
    return concord_memeq_32(scope_id, concord_zero_id);
}

/* ---------------- rekey addresses (§2) ---------------- */

nostr_concord_status_t nostr_concord_channel_rekey_key(
    const uint8_t prior_community_root[32], const uint8_t channel_id[32],
    uint64_t new_epoch, nostr_concord_group_key_t *out) {
    /* The *prior* root at the *new* epoch: a member who missed the previous
     * rotation cannot even compute where this one lands, which is the removal
     * signal rather than an error. */
    return nostr_concord_group_key(CONCORD_LABEL_REKEY_PSEUDONYM,
                                   prior_community_root, 32, channel_id, true,
                                   new_epoch, out);
}

nostr_concord_status_t nostr_concord_base_rekey_key(
    const uint8_t prior_community_root[32], const uint8_t community_id[32],
    uint64_t new_epoch, nostr_concord_group_key_t *out) {
    return nostr_concord_group_key(CONCORD_LABEL_BASE_REKEY_PSEUDONYM,
                                   prior_community_root, 32, community_id,
                                   true, new_epoch, out);
}

nostr_concord_status_t nostr_concord_rekey_locator(
    const uint8_t rotator_xonly[32], const uint8_t recipient_xonly[32],
    const uint8_t scope_id[32], uint64_t new_epoch, uint8_t locator_out[32]) {
    if (!rotator_xonly || !recipient_xonly || !scope_id || !locator_out) {
        return NOSTR_CONCORD_ERR_NULL;
    }
    /* A.6: the ikm is the two x-only keys concatenated, in that order — the
     * only place in Concord where an hkdf secret is 64 bytes, which is why
     * the length is an explicit parameter. */
    uint8_t ikm[64];
    memcpy(ikm, rotator_xonly, 32);
    memcpy(ikm + 32, recipient_xonly, 32);
    nostr_concord_status_t status =
        nostr_concord_hkdf(CONCORD_LABEL_RECIPIENT_PSEUDONYM, ikm, sizeof(ikm),
                           scope_id, true, new_epoch, locator_out);
    OPENSSL_cleanse(ikm, sizeof(ikm));
    return status;
}

/* ---------------- continuity (CORD-02 A.5) ---------------- */

nostr_concord_status_t nostr_concord_epoch_commitment(
    uint64_t prev_epoch, const uint8_t prev_key[32],
    uint8_t commitment_out[32]) {
    if (!prev_key || !commitment_out) return NOSTR_CONCORD_ERR_NULL;

    const char *label = CONCORD_LABEL_EPOCH_COMMITMENT;
    size_t label_len = strlen(label);
    /* A plain SHA-256 over utf8(label) || prev_epoch_be || prev_key: every
     * field fixed-width behind a constant label, so no length prefixes. */
    uint8_t preimage[64 + 8 + 32];
    if (label_len + 40 > sizeof(preimage)) return NOSTR_CONCORD_ERR_CRYPTO;

    memcpy(preimage, label, label_len);
    concord_put_u64_be(prev_epoch, preimage + label_len);
    memcpy(preimage + label_len + 8, prev_key, 32);

    int rc = concord_sha256(preimage, label_len + 40, commitment_out);
    OPENSSL_cleanse(preimage, sizeof(preimage));
    return rc == 0 ? NOSTR_CONCORD_OK : NOSTR_CONCORD_ERR_CRYPTO;
}

/* ---------------- blobs (§1) ---------------- */

void nostr_concord_rekey_blob_clear(nostr_concord_rekey_blob_t *blob) {
    if (!blob) return;
    OPENSSL_cleanse(blob, sizeof(*blob));
}

static size_t concord_blob_width(nostr_concord_rekey_form_t form) {
    switch (form) {
    case NOSTR_CONCORD_REKEY_CHANNEL:
    case NOSTR_CONCORD_REKEY_BASE_LEGACY:
        return CONCORD_REKEY_BLOB_CHANNEL_BYTES;
    case NOSTR_CONCORD_REKEY_BASE_MEMBER:
        return CONCORD_REKEY_BLOB_MEMBER_BYTES;
    case NOSTR_CONCORD_REKEY_BASE_STAFF:
        return CONCORD_REKEY_BLOB_STAFF_BYTES;
    }
    return 0;
}

nostr_concord_status_t nostr_concord_rekey_blob_pack(
    const nostr_concord_rekey_blob_t *blob, uint8_t *out, size_t out_cap,
    size_t *out_len) {
    if (!blob || !out || !out_len) return NOSTR_CONCORD_ERR_NULL;

    size_t width = concord_blob_width(blob->form);
    if (!width || out_cap < width) return NOSTR_CONCORD_ERR_BAD_CONTENT;

    /* Form and payload must agree. A minter that leaves the control pair out
     * of a 136-byte staff blob would ship 32 zero bytes as a control_root,
     * and every recipient would reject the rotation for a pair that cannot
     * derive — better to refuse before it is wrapped 120 times. */
    bool base = blob->form != NOSTR_CONCORD_REKEY_CHANNEL;
    if (base != nostr_concord_scope_is_base(blob->scope_id))
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    if (blob->has_control_pk !=
        (blob->form == NOSTR_CONCORD_REKEY_BASE_MEMBER ||
         blob->form == NOSTR_CONCORD_REKEY_BASE_STAFF))
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    if (blob->has_control_root !=
        (blob->form == NOSTR_CONCORD_REKEY_BASE_STAFF))
        return NOSTR_CONCORD_ERR_BAD_CONTENT;

    size_t at = 0;
    memcpy(out + at, blob->scope_id, 32);
    at += 32;
    concord_put_u64_be(blob->epoch, out + at);
    at += 8;
    memcpy(out + at, blob->new_key, 32);
    at += 32;
    if (blob->has_control_pk) {
        memcpy(out + at, blob->new_control_pk, 32);
        at += 32;
    }
    if (blob->has_control_root) {
        memcpy(out + at, blob->new_control_root, 32);
        at += 32;
    }
    *out_len = at;
    return NOSTR_CONCORD_OK;
}

nostr_concord_status_t nostr_concord_rekey_blob_parse(
    const uint8_t *plaintext, size_t len, const uint8_t community_id[32],
    const uint8_t expect_scope[32], uint64_t expect_epoch,
    nostr_concord_rekey_blob_t *out) {
    if (!plaintext || !community_id || !expect_scope || !out) {
        return NOSTR_CONCORD_ERR_NULL;
    }

    /* The width is the format declaration, so anything else is not a blob in
     * an unknown dialect — it is malformed, and dropped (§1). */
    if (len != CONCORD_REKEY_BLOB_CHANNEL_BYTES &&
        len != CONCORD_REKEY_BLOB_MEMBER_BYTES &&
        len != CONCORD_REKEY_BLOB_STAFF_BYTES) {
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    }

    nostr_concord_rekey_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    memcpy(blob.scope_id, plaintext, 32);
    blob.epoch = concord_get_u64_be(plaintext + 32);
    memcpy(blob.new_key, plaintext + 40, 32);

    /* Scope and epoch live *inside* the ciphertext and are checked against the
     * event's tags, never read from them: that is the whole reason a blob
     * minted for one channel cannot be spliced into another's rotation. */
    if (!concord_memeq_32(blob.scope_id, expect_scope)) {
        nostr_concord_rekey_blob_clear(&blob);
        return NOSTR_CONCORD_ERR_CHANNEL;
    }
    if (blob.epoch != expect_epoch) {
        nostr_concord_rekey_blob_clear(&blob);
        return NOSTR_CONCORD_ERR_EPOCH;
    }

    bool base = nostr_concord_scope_is_base(blob.scope_id);
    if (len == CONCORD_REKEY_BLOB_CHANNEL_BYTES) {
        /* One width, two forms, told apart by the scope alone: a base-scoped
         * 72 is the pre-split rotation, honored when reading old epochs. */
        blob.form = base ? NOSTR_CONCORD_REKEY_BASE_LEGACY
                         : NOSTR_CONCORD_REKEY_CHANNEL;
        *out = blob;
        return NOSTR_CONCORD_OK;
    }

    /* A Channel rotation carries no Control Plane keys — the Control Plane
     * rotates with the base and nothing else. */
    if (!base) {
        nostr_concord_rekey_blob_clear(&blob);
        return NOSTR_CONCORD_ERR_BAD_CONTENT;
    }

    memcpy(blob.new_control_pk, plaintext + 72, 32);
    blob.has_control_pk = true;
    blob.form = NOSTR_CONCORD_REKEY_BASE_MEMBER;

    if (len == CONCORD_REKEY_BLOB_STAFF_BYTES) {
        memcpy(blob.new_control_root, plaintext + 104, 32);
        blob.has_control_root = true;
        blob.form = NOSTR_CONCORD_REKEY_BASE_STAFF;

        /* A staff recipient gets both halves of the next epoch's Control
         * Plane, so it can check them against each other. A pair that does
         * not derive would leave this staffer writing at an address no member
         * reads; refuse the blob instead of adopting a split plane. */
        nostr_concord_group_key_t signer;
        nostr_concord_status_t status = nostr_concord_control_signer_key(
            blob.new_control_root, community_id, blob.epoch, &signer);
        if (status != NOSTR_CONCORD_OK) {
            nostr_concord_rekey_blob_clear(&blob);
            return status;
        }
        bool matches = concord_memeq_32(signer.pk, blob.new_control_pk);
        nostr_concord_group_key_clear(&signer);
        if (!matches) {
            nostr_concord_rekey_blob_clear(&blob);
            return NOSTR_CONCORD_ERR_CONTROL_PAIR;
        }
    }

    *out = blob;
    return NOSTR_CONCORD_OK;
}
