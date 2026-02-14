
#ifndef NOSTR_BIP32_H
#define NOSTR_BIP32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Derive private key along a BIP32 path from an arbitrary-length master seed.
 * This matches BIP-32 test vectors where the seed is raw bytes (e.g., 16 bytes).
 */
bool nostr_bip32_priv_from_master_seed(const uint8_t *seed, size_t seed_len, const uint32_t *path, size_t path_len, uint8_t out32[32]);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_BIP32_H */
