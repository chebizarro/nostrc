/* nip-concord: the frozen derivations of CORD-02 Appendix A. */

#include "concord_internal.h"

#include <stdlib.h>
#include <string.h>

#include <nostr/nip44/nip44.h>
#include <openssl/crypto.h>

/* label || 0x00 || id[32] || epoch[8] || counter[1], with the longest label
 * currently defined well under 64 bytes. */
#define CONCORD_INFO_CAP 128

const char *nostr_concord_status_string(nostr_concord_status_t status) {
    switch (status) {
    case NOSTR_CONCORD_OK: return "ok";
    case NOSTR_CONCORD_ERR_NULL: return "null argument";
    case NOSTR_CONCORD_ERR_OOM: return "out of memory";
    case NOSTR_CONCORD_ERR_WRONG_KIND: return "wrong kind";
    case NOSTR_CONCORD_ERR_BAD_PUBKEY: return "malformed pubkey";
    case NOSTR_CONCORD_ERR_BAD_CONTENT: return "malformed content";
    case NOSTR_CONCORD_ERR_BAD_TAG: return "malformed tag";
    case NOSTR_CONCORD_ERR_CARDINALITY: return "tag cardinality violation";
    case NOSTR_CONCORD_ERR_BAD_NIP44: return "NIP-44 decryption failed";
    case NOSTR_CONCORD_ERR_BAD_SIGNATURE: return "signature verification failed";
    case NOSTR_CONCORD_ERR_COMMUNITY_ID: return "community_id does not commit to the owner";
    case NOSTR_CONCORD_ERR_EPOCH: return "epoch mismatch";
    case NOSTR_CONCORD_ERR_CHANNEL: return "channel mismatch";
    case NOSTR_CONCORD_ERR_INVITE_EXPIRED: return "invite expired";
    case NOSTR_CONCORD_ERR_LIST_FULL: return "list is full";
    case NOSTR_CONCORD_ERR_SNAPSHOT_MALFORMED: return "malformed snapshot";
    case NOSTR_CONCORD_ERR_CRYPTO: return "cryptographic operation failed";
    case NOSTR_CONCORD_ERR_BAD_FRAGMENT: return "malformed invite fragment";
    case NOSTR_CONCORD_ERR_UNSUPPORTED_VERSION: return "unsupported invite version";
    }
    return "unknown status";
}

/* ---------------- encoding ---------------- */

bool nostr_concord_is_lower_hex_32(const char *value) {
    if (!value) return false;
    size_t i = 0;
    for (; i < 64; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return value[64] == '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool nostr_concord_hex_decode_32(const char *hex, uint8_t out[32]) {
    if (!hex || !out || !nostr_concord_is_lower_hex_32(hex)) return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void nostr_concord_hex_encode_32(const uint8_t in[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    if (!in || !out) return;
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[64] = '\0';
}

static const char b64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

char *nostr_concord_b64url_encode(const uint8_t *data, size_t len) {
    if (!data && len) return NULL;
    size_t out_len = (len / 3) * 4 + ((len % 3) ? (len % 3) + 1 : 0);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                     data[i + 2];
        out[o++] = b64url_alphabet[(v >> 18) & 0x3f];
        out[o++] = b64url_alphabet[(v >> 12) & 0x3f];
        out[o++] = b64url_alphabet[(v >> 6) & 0x3f];
        out[o++] = b64url_alphabet[v & 0x3f];
    }
    if (i < len) {
        size_t rem = len - i;
        uint32_t v = (uint32_t)data[i] << 16;
        if (rem == 2) v |= (uint32_t)data[i + 1] << 8;
        out[o++] = b64url_alphabet[(v >> 18) & 0x3f];
        out[o++] = b64url_alphabet[(v >> 12) & 0x3f];
        if (rem == 2) out[o++] = b64url_alphabet[(v >> 6) & 0x3f];
    }
    out[o] = '\0';
    return out;
}

static int b64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

uint8_t *nostr_concord_b64url_decode(const char *text, size_t *out_len) {
    if (!text || !out_len) return NULL;
    size_t len = strlen(text);
    /* A length of 1 mod 4 can never be produced by unpadded base64url. */
    if (len % 4 == 1) return NULL;

    size_t decoded_len = (len / 4) * 3;
    if (len % 4 == 2) decoded_len += 1;
    else if (len % 4 == 3) decoded_len += 2;

    uint8_t *out = malloc(decoded_len ? decoded_len : 1);
    if (!out) return NULL;

    size_t o = 0;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        int a = b64url_value(text[i]), b = b64url_value(text[i + 1]);
        int c = b64url_value(text[i + 2]), d = b64url_value(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) { free(out); return NULL; }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                     ((uint32_t)c << 6) | (uint32_t)d;
        out[o++] = (uint8_t)(v >> 16);
        out[o++] = (uint8_t)(v >> 8);
        out[o++] = (uint8_t)v;
    }
    if (i < len) {
        size_t rem = len - i;
        int a = b64url_value(text[i]);
        int b = b64url_value(text[i + 1]);
        if (a < 0 || b < 0) { free(out); return NULL; }
        if (rem == 2) {
            /* The trailing 4 bits must be zero, or the encoding is not
             * canonical and two distinct strings would decode alike. */
            if ((b & 0x0f) != 0) { free(out); return NULL; }
            out[o++] = (uint8_t)((a << 2) | (b >> 4));
        } else {
            int c = b64url_value(text[i + 2]);
            if (c < 0) { free(out); return NULL; }
            if ((c & 0x03) != 0) { free(out); return NULL; }
            out[o++] = (uint8_t)((a << 2) | (b >> 4));
            out[o++] = (uint8_t)((b << 4) | (c >> 2));
        }
    }
    *out_len = o;
    return out;
}

/* ---------------- derivations ---------------- */

nostr_concord_status_t nostr_concord_derive_community_id(
    const uint8_t owner_xonly[32], const uint8_t owner_salt[32],
    uint8_t community_id_out[32]) {
    if (!owner_xonly || !owner_salt || !community_id_out)
        return NOSTR_CONCORD_ERR_NULL;

    /* A.4 is a plain SHA-256 commitment, not the HKDF construction. */
    static const char label[] = CONCORD_LABEL_COMMUNITY;
#define CONCORD_COMMUNITY_LABEL_LEN (sizeof(label) - 1)
    uint8_t buf[CONCORD_COMMUNITY_LABEL_LEN + 64];
    const size_t label_len = CONCORD_COMMUNITY_LABEL_LEN;
    memcpy(buf, label, label_len);
    memcpy(buf + label_len, owner_xonly, 32);
    memcpy(buf + label_len + 32, owner_salt, 32);

    if (concord_sha256(buf, sizeof(buf), community_id_out) != 0)
        return NOSTR_CONCORD_ERR_CRYPTO;
    return NOSTR_CONCORD_OK;
#undef CONCORD_COMMUNITY_LABEL_LEN
}

bool nostr_concord_verify_community_id(const uint8_t community_id[32],
                                       const uint8_t owner_xonly[32],
                                       const uint8_t owner_salt[32]) {
    if (!community_id || !owner_xonly || !owner_salt) return false;
    uint8_t recomputed[32];
    if (nostr_concord_derive_community_id(owner_xonly, owner_salt,
                                          recomputed) != NOSTR_CONCORD_OK) {
        return false;
    }
    bool ok = concord_memeq_32(community_id, recomputed);
    OPENSSL_cleanse(recomputed, sizeof(recomputed));
    return ok;
}

nostr_concord_status_t nostr_concord_hkdf(const char *label,
                                          const uint8_t *secret,
                                          size_t secret_len,
                                          const uint8_t id[32],
                                          bool has_epoch, uint64_t epoch,
                                          uint8_t out[32]) {
    if (!label || !secret || !id || !out) return NOSTR_CONCORD_ERR_NULL;
    if (!secret_len) return NOSTR_CONCORD_ERR_NULL;

    uint8_t info[CONCORD_INFO_CAP];
    size_t info_len = concord_build_info(label, id, has_epoch, epoch, -1, info,
                                         sizeof(info));
    if (!info_len) return NOSTR_CONCORD_ERR_CRYPTO;

    if (concord_hkdf_sha256(secret, secret_len, info, info_len, out) != 0)
        return NOSTR_CONCORD_ERR_CRYPTO;
    return NOSTR_CONCORD_OK;
}

nostr_concord_status_t nostr_concord_group_key(
    const char *label, const uint8_t *secret, size_t secret_len,
    const uint8_t id[32], bool has_epoch, uint64_t epoch,
    nostr_concord_group_key_t *out) {
    if (!label || !secret || !id || !out) return NOSTR_CONCORD_ERR_NULL;
    if (!secret_len) return NOSTR_CONCORD_ERR_NULL;

    memset(out, 0, sizeof(*out));

    /* A.3 scalar_normalize: the first attempt appends no counter byte; each
     * retry appends one incrementing byte to the *info*, starting at 0. The
     * reject branch is ~2^-128 rare, so the loop is a determinism guarantee
     * across implementations rather than a hot path. */
    bool derived = false;
    for (int counter = -1; counter < 256; counter++) {
        uint8_t info[CONCORD_INFO_CAP];
        size_t info_len = concord_build_info(label, id, has_epoch, epoch,
                                             counter, info, sizeof(info));
        if (!info_len) return NOSTR_CONCORD_ERR_CRYPTO;

        uint8_t seed[32];
        if (concord_hkdf_sha256(secret, secret_len, info, info_len, seed) != 0)
            return NOSTR_CONCORD_ERR_CRYPTO;

        if (concord_seckey_verify(seed)) {
            memcpy(out->sk, seed, 32);
            OPENSSL_cleanse(seed, sizeof(seed));
            derived = true;
            break;
        }
        OPENSSL_cleanse(seed, sizeof(seed));
    }
    if (!derived) return NOSTR_CONCORD_ERR_CRYPTO;

    if (concord_xonly_pubkey(out->sk, out->pk) != 0) {
        nostr_concord_group_key_clear(out);
        return NOSTR_CONCORD_ERR_CRYPTO;
    }
    /* The wraps are encrypted under the stream key's NIP-44 self-ECDH, never
     * the p-tagged key (CORD-01). */
    if (nostr_nip44_convkey(out->sk, out->pk, out->conv_key) != 0) {
        nostr_concord_group_key_clear(out);
        return NOSTR_CONCORD_ERR_CRYPTO;
    }
    return NOSTR_CONCORD_OK;
}

void nostr_concord_group_key_clear(nostr_concord_group_key_t *key) {
    if (!key) return;
    OPENSSL_cleanse(key, sizeof(*key));
}

nostr_concord_status_t nostr_concord_channel_key(
    const uint8_t channel_secret[32], const uint8_t channel_id[32],
    uint64_t epoch, nostr_concord_group_key_t *out) {
    return nostr_concord_group_key(CONCORD_LABEL_CHANNEL, channel_secret, 32,
                                   channel_id, true, epoch, out);
}

nostr_concord_status_t nostr_concord_control_read_key(
    const uint8_t community_root[32], const uint8_t community_id[32],
    uint64_t epoch, nostr_concord_group_key_t *out) {
    return nostr_concord_group_key(CONCORD_LABEL_CONTROL, community_root, 32,
                                   community_id, true, epoch, out);
}

nostr_concord_status_t nostr_concord_control_signer_key(
    const uint8_t control_root[32], const uint8_t community_id[32],
    uint64_t epoch, nostr_concord_group_key_t *out) {
    return nostr_concord_group_key(CONCORD_LABEL_CONTROL_SIGNER, control_root,
                                   32, community_id, true, epoch, out);
}

nostr_concord_status_t nostr_concord_guestbook_key(
    const uint8_t community_root[32], const uint8_t community_id[32],
    uint64_t epoch, nostr_concord_group_key_t *out) {
    return nostr_concord_group_key(CONCORD_LABEL_GUESTBOOK, community_root, 32,
                                   community_id, true, epoch, out);
}

nostr_concord_status_t nostr_concord_dissolved_key(
    const uint8_t community_id[32], nostr_concord_group_key_t *out) {
    /* No secret and no epoch: the community_id itself is the ikm and the id
     * is all-zeroes, so every member past or present resolves the grave. */
    static const uint8_t zero_id[32] = { 0 };
    return nostr_concord_group_key(CONCORD_LABEL_DISSOLVED, community_id, 32,
                                   zero_id, false, 0, out);
}

/* ---------------- control editions (CORD-04) ---------------- */

static void concord_put_u64_be(uint64_t value, uint8_t out[8]) {
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
}

nostr_concord_status_t nostr_concord_edition_hash(const uint8_t entity_id[32],
                                                  uint64_t version,
                                                  const uint8_t *prev,
                                                  const uint8_t *content,
                                                  size_t content_len,
                                                  uint8_t hash_out[32]) {
    if (!entity_id || !hash_out || (!content && content_len)) {
        return NOSTR_CONCORD_ERR_NULL;
    }

    const char *label = CONCORD_LABEL_EDITION;
    size_t label_len = strlen(label);
    /* len64(label) || label || eid[32] || version[8] || 1 + prev[32]
     * || len64(content). Every field is fixed-width or length-prefixed, so
     * distinct inputs can never collide. */
    size_t header_len = 8 + label_len + 32 + 8 + 1 + 32 + 8;
    size_t total = header_len + content_len;
    if (total < header_len) return NOSTR_CONCORD_ERR_OOM; /* overflow */

    uint8_t *preimage = malloc(total);
    if (!preimage) return NOSTR_CONCORD_ERR_OOM;

    size_t at = 0;
    concord_put_u64_be((uint64_t)label_len, preimage + at);
    at += 8;
    memcpy(preimage + at, label, label_len);
    at += label_len;
    memcpy(preimage + at, entity_id, 32);
    at += 32;
    concord_put_u64_be(version, preimage + at);
    at += 8;
    /* The absent-prev branch is a distinct flag byte followed by zeroes, not a
     * shorter buffer: a first edition and one citing an all-zero prev must not
     * share a preimage. */
    preimage[at++] = prev ? 0x01u : 0x00u;
    if (prev) {
        memcpy(preimage + at, prev, 32);
    } else {
        memset(preimage + at, 0, 32);
    }
    at += 32;
    concord_put_u64_be((uint64_t)content_len, preimage + at);
    at += 8;
    if (content_len) memcpy(preimage + at, content, content_len);
    at += content_len;

    int rc = concord_sha256(preimage, at, hash_out);
    OPENSSL_cleanse(preimage, at);
    free(preimage);
    return rc == 0 ? NOSTR_CONCORD_OK : NOSTR_CONCORD_ERR_CRYPTO;
}

nostr_concord_status_t nostr_concord_grant_locator(
    const uint8_t community_id[32], const uint8_t member_xonly[32],
    uint8_t eid_out[32]) {
    if (!community_id || !member_xonly || !eid_out) {
        return NOSTR_CONCORD_ERR_NULL;
    }
    /* A.6: ikm is the community_id, the id is the member, and no epoch — the
     * coordinate survives every Refounding. */
    return nostr_concord_hkdf(CONCORD_LABEL_GRANT, community_id, 32,
                              member_xonly, false, 0, eid_out);
}

nostr_concord_status_t nostr_concord_banlist_locator(
    const uint8_t community_id[32], uint8_t eid_out[32]) {
    if (!community_id || !eid_out) return NOSTR_CONCORD_ERR_NULL;
    static const uint8_t zero_id[32] = { 0 };
    return nostr_concord_hkdf(CONCORD_LABEL_BANLIST, community_id, 32, zero_id,
                              false, 0, eid_out);
}

bool nostr_concord_parse_permissions(const char *decimal, uint64_t *out) {
    if (!decimal || !out) return false;
    size_t len = strlen(decimal);
    if (len == 0 || len > 20) return false;
    /* Canonical decimal: no sign, no leading zeros (CORD-01 "Encoding"). */
    if (len > 1 && decimal[0] == '0') return false;
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (decimal[i] < '0' || decimal[i] > '9') return false;
        uint64_t digit = (uint64_t)(decimal[i] - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

/* ---------------- ordering ---------------- */

bool nostr_concord_parse_ms(const char *value, int *out) {
    if (!value || !out) return false;
    size_t len = strlen(value);
    if (len == 0 || len > 3) return false;
    /* Tag values are decimal with no leading zeros (CORD-01 "Encoding"). */
    if (len > 1 && value[0] == '0') return false;
    int result = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + (value[i] - '0');
    }
    if (result < 0 || result > 999) return false;
    *out = result;
    return true;
}

bool nostr_concord_order_key(int64_t created_at, int ms, int64_t *out_ms_time) {
    if (!out_ms_time) return false;
    /* An ms outside 0..999 is malformed: the entry is dropped, never
     * interpreted, or the excess would smuggle a forged future past a clock
     * check (CORD-02 §5). */
    if (ms < 0 || ms > 999) return false;
    *out_ms_time = created_at * 1000 + ms;
    return true;
}
