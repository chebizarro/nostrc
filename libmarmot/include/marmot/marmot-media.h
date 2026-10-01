/*
 * libmarmot - encrypted media v2 (MIP-04) and group image components
 *
 * Spec: marmot-protocol/marmot 07da8ffb, features/encrypted-media.md
 * (encrypted-media-v2), app-components/group-blossom-image-v1.md (0x8002)
 * and app-components/group-avatar-url-v1.md (0x8007).  Checked against
 * MDK v0.11.0 (946e0547) with the fixtures in tests/vectors/media/.
 *
 * Encrypted media v2, per attachment:
 *
 *   media_secret = MLS-Exporter("marmot", "encrypted-media", 32) of the
 *                  source epoch (the epoch of the carrying app message)
 *   file_key     = HKDF-Expand-SHA256(media_secret, "encrypted-media-v2" 0x00
 *                  plaintext_sha256 0x00 media_type 0x00 filename 0x00 "key", 32)
 *   aad          = "encrypted-media-v2" 0x00 plaintext_sha256 0x00
 *                  media_type 0x00 filename
 *   ciphertext   = ChaCha20-Poly1305(file_key, random 12-byte nonce, aad)
 *
 * The source epoch is not in the imeta tag: a sender must send the carrying
 * message in MarmotMediaUpload.source_epoch, and a receiver passes the epoch
 * the message was decrypted in (MarmotMessageResult.app_msg.epoch).
 *
 * The frozen encrypted-media-v1 and libmarmot's own pre-0.12 format
 * (marmot_encrypt_media) are not produced.  A v1 imeta tag is reported as
 * MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_MEDIA_H
#define MARMOT_MEDIA_H

#include "marmot-error.h"
#include "marmot-types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARMOT_MEDIA_V2_VERSION          "encrypted-media-v2"
#define MARMOT_MEDIA_LOCATOR_BLOSSOM_V1  "blossom-v1"
#define MARMOT_MEDIA_FILENAME_MAX        255u
#define MARMOT_MEDIA_TYPE_MAX            128u

/* GroupContext app_data_dictionary component ids */
#define MARMOT_COMPONENT_GROUP_BLOSSOM_IMAGE_V1    0x8002u
#define MARMOT_COMPONENT_GROUP_AVATAR_URL_V1       0x8007u
#define MARMOT_COMPONENT_GROUP_ENCRYPTED_MEDIA_V2  0x800bu

#define MARMOT_GROUP_AVATAR_URL_MAX   2048u
#define MARMOT_GROUP_AVATAR_HINT_MAX  256u

/* ── Encrypted media v2 ─────────────────────────────────────────────────── */

typedef struct {
    char *kind;    /**< e.g. MARMOT_MEDIA_LOCATOR_BLOSSOM_V1 */
    char *value;   /**< e.g. the Blossom blob URL, stored exactly as received */
} MarmotMediaLocator;

/**
 * MarmotMediaReference:
 *
 * One encrypted-media-v2 attachment: the fields of one `imeta` tag.
 * All strings are owned; free with marmot_media_reference_clear().
 */
typedef struct {
    MarmotMediaLocator *locators;
    size_t              locator_count;
    uint8_t ciphertext_sha256[32];
    uint8_t plaintext_sha256[32];
    uint8_t nonce[12];
    char   *media_type;   /**< canonical (marmot_media_type_canonicalize) */
    char   *filename;     /**< 1..255 bytes of UTF-8, preserved exactly */
    char   *dim;          /**< optional render hint; NULL when absent */
    char   *thumbhash;    /**< optional render hint; NULL when absent */
} MarmotMediaReference;

/**
 * MarmotMediaUpload:
 *
 * Result of marmot_media_encrypt(): upload `ciphertext` (its SHA-256 is
 * reference.ciphertext_sha256), add the returned URL with
 * marmot_media_reference_add_locator(), then put the imeta tag from
 * marmot_media_imeta_build() on a kind-9 message sent in `source_epoch`.
 */
typedef struct {
    uint8_t             *ciphertext;
    size_t               ciphertext_len;
    uint64_t             source_epoch;
    MarmotMediaReference reference;   /**< no locators yet */
} MarmotMediaUpload;

/**
 * Apply the shared Marmot media-type canonicalization (foundation/
 * canonical-encoding.md): parameters dropped, HTAB/LF/FF/CR/SP trimmed,
 * ASCII-lowercased, exactly one '/', token bytes only, 64/64/128 byte bounds,
 * image/jpg -> image/jpeg.  *out is malloc'd.  MARMOT_ERR_INVALID_INPUT if the
 * value has no canonical form.
 */
MarmotError marmot_media_type_canonicalize(const char *input, char **out);

/**
 * Encrypt one attachment for the group's current epoch.  media_type is
 * canonicalized (an uncanonicalizable type is MARMOT_ERR_INVALID_INPUT);
 * filename must be 1..255 bytes of UTF-8 and is used exactly as given.
 * An empty file is MARMOT_ERR_INVALID_INPUT.  A fresh random nonce is drawn
 * on every call, including a resend of the same file.
 */
MarmotError marmot_media_encrypt(Marmot *m,
                                 const MarmotGroupId *mls_group_id,
                                 const uint8_t *plaintext, size_t plaintext_len,
                                 const char *media_type,
                                 const char *filename,
                                 MarmotMediaUpload *out);

/**
 * Decrypt one attachment whose carrying message was in `source_epoch`
 * (the current epoch or any retained earlier epoch whose exporter secret
 * storage still holds).  Order: the reference is validated, the ciphertext
 * SHA-256 is checked (MARMOT_ERR_MEDIA_CIPHERTEXT_HASH), the AEAD is opened
 * (MARMOT_ERR_MEDIA_DECRYPT), then the plaintext SHA-256 is checked
 * (MARMOT_ERR_MEDIA_HASH_MISMATCH).  An epoch whose secret is gone is
 * MARMOT_ERR_STORAGE_NOT_FOUND.  Each failure only makes this attachment
 * unavailable; it never invalidates the message that carried it.
 */
MarmotError marmot_media_decrypt(Marmot *m,
                                 const MarmotGroupId *mls_group_id,
                                 uint64_t source_epoch,
                                 const MarmotMediaReference *reference,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint8_t **plaintext_out, size_t *plaintext_len);

/**
 * MARMOT_OK when a message created now would be sent in `source_epoch`
 * (MarmotMediaUpload.source_epoch), else MARMOT_ERR_MEDIA_EPOCH_CHANGED:
 * encrypt and upload again.  Like marmot_create_message() it first repairs
 * a group record left behind by an interrupted epoch transition, so the
 * epoch compared is the one the message will really use.  Call it in the
 * same uninterrupted turn as marmot_create_message() (no Commit processed
 * in between).
 */
MarmotError marmot_media_check_epoch(Marmot *m, const MarmotGroupId *mls_group_id,
                                     uint64_t source_epoch);

/** Append a locator (copied).  Structure is checked by imeta_build. */
MarmotError marmot_media_reference_add_locator(MarmotMediaReference *reference,
                                               const char *kind,
                                               const char *value);

/** Set or clear (NULL) the optional dim and thumbhash hints (copied). */
MarmotError marmot_media_reference_set_hints(MarmotMediaReference *reference,
                                             const char *dim,
                                             const char *thumbhash);

/**
 * Build the ordered v2 imeta tag: "imeta", "v encrypted-media-v2",
 * "locator <kind> <value>"..., "ciphertext_sha256 <hex>",
 * "plaintext_sha256 <hex>", "nonce <hex>", "m <type>", "filename <name>",
 * then "dim <v>" and "thumbhash <v>" when set.  The reference must be valid
 * and every locator kind must be in `allowed_kinds` (a NULL-terminated list:
 * the group's encrypted-media-v2 allowed_locator_kinds), or blossom-v1 when
 * allowed_kinds is NULL or empty: a sender never emits a kind its group
 * policy forbids.  *out_fields is a NULL-terminated malloc'd array of
 * *out_count strings; free with marmot_media_imeta_fields_free().
 */
MarmotError marmot_media_imeta_build(const MarmotMediaReference *reference,
                                     const char *const *allowed_kinds,
                                     char ***out_fields, size_t *out_count);

/**
 * Parse and validate one imeta tag (fields[0] must be "imeta").
 * field_lens may be NULL; when given, a field with an embedded NUL byte is
 * rejected (a NUL-terminated copy would otherwise lose bytes).  Errors:
 * MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION when `v` is absent or not
 * encrypted-media-v2 (including v1), MARMOT_ERR_MEDIA_INVALID_REFERENCE for
 * every other structural rule of the spec (duplicates, missing fields, bad
 * hashes/nonce, non-canonical m, filename profile, blurhash, locators).
 */
MarmotError marmot_media_imeta_parse(const char *const *fields,
                                     const size_t *field_lens,
                                     size_t field_count,
                                     MarmotMediaReference *out);

/**
 * The Blossom fallback fetch URL for an endpoint: base_url with trailing '/'
 * removed, '/', lowercase hex of ciphertext_sha256.  *out is malloc'd.
 */
MarmotError marmot_media_blossom_fallback_url(const char *base_url,
                                              const uint8_t ciphertext_sha256[32],
                                              char **out);

void marmot_media_reference_clear(MarmotMediaReference *reference);
void marmot_media_upload_clear(MarmotMediaUpload *upload);
void marmot_media_imeta_fields_free(char **fields, size_t count);

/* ── 0x8002 marmot.group.blossom.image.v1 ──────────────────────────────── */

/**
 * MarmotGroupBlossomImage: the component state.  `present` false is the
 * absent (all-empty) state; otherwise every field is set and media_type is
 * canonical.  image_key and image_upload_key are secrets.
 */
typedef struct {
    bool     present;
    uint8_t  image_hash[32];        /**< SHA-256 of the encrypted blob */
    uint8_t  image_key[32];
    uint8_t  image_nonce[12];
    uint8_t  image_upload_key[32];  /**< Blossom write key (secp256k1 secret) */
    char    *media_type;            /**< of the decrypted image */
} MarmotGroupBlossomImage;

MarmotError marmot_group_blossom_image_encode(const MarmotGroupBlossomImage *image,
                                              uint8_t **out, size_t *out_len);
/** Exact decode; rejects partial state, bad lengths, a non-canonical media
 *  type, non-shortest length prefixes and trailing bytes. */
MarmotError marmot_group_blossom_image_decode(const uint8_t *data, size_t len,
                                              MarmotGroupBlossomImage *out);

/**
 * Encrypt a group image under a fresh random image_key, image_nonce and
 * image_upload_key (AAD "marmot-group-image-v1" 0x00 media_type) and fill
 * the component state.  Upload *ciphertext under out->image_hash.
 */
MarmotError marmot_group_image_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                                       const char *media_type,
                                       MarmotGroupBlossomImage *out,
                                       uint8_t **ciphertext, size_t *ciphertext_len);

/** Check the blob against image_hash first, then open it. */
MarmotError marmot_group_image_decrypt(const MarmotGroupBlossomImage *image,
                                       const uint8_t *ciphertext, size_t ciphertext_len,
                                       uint8_t **plaintext, size_t *plaintext_len);

void marmot_group_blossom_image_clear(MarmotGroupBlossomImage *image);

/* ── 0x8007 marmot.group.avatar-url.v1 ─────────────────────────────────── */

/**
 * MarmotGroupAvatarUrl: url NULL is the absent state (then no hints).
 * dim and thumbhash are opaque render hints of at most 256 bytes.
 *
 * url_unverified (decode only): the stored URL is accepted state but lies
 * outside the subset of the WHATWG serializer libmarmot can verify (an IDNA
 * or '_' host, '^ | [ ]' in the path, ...).  Validity is consensus and
 * rendering is local: such a URL is kept byte for byte, never rewritten, and
 * never contacted -- render a placeholder (MARMOT_GROUP_AVATAR_URL_PLACEHOLDER).
 */
typedef struct {
    char    *url;
    uint8_t *dim;
    size_t   dim_len;
    uint8_t *thumbhash;
    size_t   thumbhash_len;
    bool     url_unverified;
} MarmotGroupAvatarUrl;

/**
 * Normalize an https avatar URL to the bytes the WHATWG URL serializer
 * produces, for producing state.  libmarmot implements a strict subset:
 * ASCII hosts (DNS names of letters, digits, '-' and '.', canonical IPv4,
 * bracketed IPv6), optional port, path and query without characters whose
 * serialization differs between WHATWG versions.  Anything outside it,
 * including non-ASCII or punycode (xn--) hosts, http, userinfo and
 * fragments, is MARMOT_ERR_INVALID_INPUT: libmarmot only produces bytes it
 * can prove canonical.
 */
MarmotError marmot_group_avatar_url_normalize(const char *raw, char **out);

/**
 * Encode normalizes url (an unverified or non-normalizable URL is
 * MARMOT_ERR_MEDIA_INVALID_REFERENCE: libmarmot never produces it).
 *
 * Decode is three-way, so a Commit is never refused for a URL another
 * implementation's WHATWG parser stores as canonical:
 *   - valid: inside libmarmot's subset and byte-equal to its normalization;
 *   - invalid (MARMOT_ERR_MEDIA_INVALID_REFERENCE): provably not the output
 *     of any WHATWG serializer version -- not UTF-8, a byte outside printable
 *     ASCII, not "https://", userinfo or '#', an uppercase or '%' host, a
 *     default/empty/zero-padded port, a missing path, a dot segment, '\'
 *     before the query, an unencoded space, '"', '<', '>', '`', '{' or '}' in
 *     the path, space, '"', '<', '>' or '\'' in the query, or a value inside the
 *     subset whose normalization differs;
 *   - unverified: everything else; accepted with url_unverified set.
 */
MarmotError marmot_group_avatar_url_encode(const MarmotGroupAvatarUrl *avatar,
                                           uint8_t **out, size_t *out_len);
MarmotError marmot_group_avatar_url_decode(const uint8_t *data, size_t len,
                                           MarmotGroupAvatarUrl *out);
void marmot_group_avatar_url_clear(MarmotGroupAvatarUrl *avatar);

typedef enum {
    MARMOT_GROUP_AVATAR_NONE = 0,
    MARMOT_GROUP_AVATAR_URL,          /**< 0x8007 present: it wins */
    MARMOT_GROUP_AVATAR_BLOSSOM,      /**< only 0x8002 present */
    MARMOT_GROUP_AVATAR_URL_PLACEHOLDER, /**< 0x8007 wins but is unverified:
                                          *   show a placeholder, fetch nothing */
} MarmotGroupAvatarSource;

/** Rendering precedence (group-avatar-url-v1.md): either may be NULL. */
MarmotGroupAvatarSource marmot_group_avatar_select(const MarmotGroupAvatarUrl *avatar_url,
                                                   const MarmotGroupBlossomImage *blossom_image);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_MEDIA_H */
