/*
 * nh_porthome_wrapkey.h — portable-home Phase 2: production wrap-seed
 * derivation (broker-owned). Bead nostrc-ck6i, sibling of nostrc-89rj.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. Gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL.
 *
 * Two functions live here, one per provider case (design §4.3):
 *
 *   nh_porthome_wrap_seed_from_ikm(ikm, ikm_len, out_seed)
 *       HKDF-SHA256(salt="porthome/v1/wrap", ikm, info="") -> 32-byte
 *       wrap_seed. Used by the LOCAL vault provider: after successful
 *       vault_open the child hands the parent an unlocked private key,
 *       which is the ikm. The caller MUST wipe both `ikm` and `out_seed`
 *       (once fed to nh_porthome_key_derive) with OPENSSL_cleanse().
 *
 *   nh_porthome_wrap_seed_from_nip44_plaintext(pt, pt_len, out_seed)
 *       For the NIP-46 case the signer returns the 32-byte wrap_seed
 *       directly as the plaintext of nip44_decrypt(wrapped_home_key_ct).
 *       This helper validates that plaintext is exactly 32 bytes
 *       (either raw or 64-lowercase-hex) and copies it into out_seed.
 *
 * home_key = nh_porthome_key_derive(out_seed). Both seed and home_key
 * live in broker memory only, in an mlock'd secure_buf. Never written
 * to disk. Never returned to PAM.
 *
 * Enrollment (design §4.3 first-login path): the caller mints a fresh
 * random 32-byte seed with nh_porthome_wrap_seed_random and asks the
 * signer to nip44_encrypt it. The resulting ciphertext is stored as
 * the provider record's wrapped_home_key (a public metadata field).
 */

#ifndef NH_PORTHOME_WRAPKEY_H
#define NH_PORTHOME_WRAPKEY_H

#include <stddef.h>
#include <stdint.h>

#include "nh_porthome_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HKDF salt for the local-vault path. Bumped with NH_PORTHOME_WIRE_VERSION. */
#define NH_PORTHOME_WRAPKEY_SALT_LOCAL "porthome/v1/wrap"

/* Derive a 32-byte wrap_seed from an arbitrary IKM (typically the local
 * vault's unlocked account_private_key, 32 bytes). Returns 0 on success.
 * The caller MUST wipe the ikm buffer after this call. */
int nh_porthome_wrap_seed_from_ikm(const uint8_t *ikm, size_t ikm_len,
                                   uint8_t out_seed[NH_PORTHOME_KEY_LEN]);

/* Validate and copy a NIP-46 nip44_decrypt plaintext into a wrap_seed.
 * Accepts either 32 raw bytes or 64 lowercase-hex chars (with or without
 * trailing NUL). Any other length or non-hex content is refused with
 * NH_PORTHOME_ERR_ARG so a signer that returns junk cannot be silently
 * accepted. */
int nh_porthome_wrap_seed_from_nip44_plaintext(const uint8_t *pt, size_t pt_len,
                                               uint8_t out_seed[NH_PORTHOME_KEY_LEN]);

/* Generate a fresh random wrap_seed (enrollment, design §4.3). Uses the
 * OpenSSL CSPRNG. Returns 0 on success. */
int nh_porthome_wrap_seed_random(uint8_t out_seed[NH_PORTHOME_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_WRAPKEY_H */
