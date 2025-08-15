#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BIP-39 API: mnemonic generation, validation (with checksum), and seed derivation. */

/*
 * Validate a mnemonic strictly per BIP-39:
 * - 12/15/18/21/24 words from the English wordlist
 * - lowercase ASCII words
 * - checksum bits verified
 */
bool nostr_bip39_validate(const char *mnemonic);

/* Generate a new mnemonic with 12/15/18/21/24 words. Caller must free() the returned string. */
char *nostr_bip39_generate(int word_count);

/* PBKDF2-HMAC-SHA512(mnemonic, "mnemonic" + passphrase, 2048) -> 64 bytes */
bool nostr_bip39_seed(const char *mnemonic, const char *passphrase, uint8_t out[64]);

#ifdef __cplusplus
}
#endif
