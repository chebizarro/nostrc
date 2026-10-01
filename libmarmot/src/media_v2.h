/*
 * libmarmot - encrypted media v2 internals (tests drive these with fixed
 * secrets and nonces; the public API is in marmot/marmot-media.h).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_MEDIA_V2_H
#define MARMOT_MEDIA_V2_H

#include <marmot/marmot-media.h>

/* media_secret = MLS-Exporter("marmot", "encrypted-media", 32) */
int marmot_media_secret_from_exporter(const uint8_t exporter_secret[32],
                                      uint8_t media_secret[32]);

/* The exact v2 HKDF info / AAD bytes (malloc'd). */
int marmot_media_v2_key_info(const uint8_t plaintext_sha256[32], const char *media_type,
                             const char *filename, uint8_t **out, size_t *out_len);
int marmot_media_v2_aad(const uint8_t plaintext_sha256[32], const char *media_type,
                        const char *filename, uint8_t **out, size_t *out_len);
int marmot_media_v2_file_key(const uint8_t media_secret[32],
                             const uint8_t plaintext_sha256[32], const char *media_type,
                             const char *filename, uint8_t file_key[32]);

/* Encrypt with a given nonce; media_type must already be canonical.  Fills
 * every reference field but the locators and hints. */
MarmotError marmot_media_v2_seal(const uint8_t media_secret[32], const uint8_t nonce[12],
                                 const uint8_t *plaintext, size_t plaintext_len,
                                 const char *media_type, const char *filename,
                                 uint8_t **ciphertext, size_t *ciphertext_len,
                                 MarmotMediaReference *reference);

/* Receive order of marmot_media_decrypt(), with the media secret given. */
MarmotError marmot_media_v2_open(const uint8_t media_secret[32],
                                 const MarmotMediaReference *reference,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint8_t **plaintext, size_t *plaintext_len);

/* Ingest validation of a reference (locator structure included). */
MarmotError marmot_media_reference_validate(const MarmotMediaReference *reference);

/* Shared with the group image codecs. */
bool marmot_media_utf8_valid(const uint8_t *s, size_t len);

/* 0x8002 image AEAD with a given key/nonce (tests); media_type canonical. */
MarmotError marmot_group_image_seal(const uint8_t key[32], const uint8_t nonce[12],
                                    const uint8_t *plaintext, size_t plaintext_len,
                                    const char *media_type,
                                    uint8_t **ciphertext, size_t *ciphertext_len);

#endif /* MARMOT_MEDIA_V2_H */
