#ifndef NOSTR_NIP06_H
#define NOSTR_NIP06_H

#include <stdbool.h>

// Canonical NIP-06 APIs
// All returned buffers are heap-allocated; caller must free() them.

// Generate a BIP39 English mnemonic (default 24 words)
char *nostr_nip06_generate_mnemonic(void);

// Validate a mnemonic against the BIP39 English wordlist
bool nostr_nip06_validate_mnemonic(const char *mnemonic);

// Derive 64-byte seed from mnemonic with empty passphrase ""
unsigned char *nostr_nip06_seed_from_mnemonic(const char *mnemonic);

// Derive private key hex from seed using path m/44'/1237'/account'/0/0
char *nostr_nip06_private_key_from_seed_account(const unsigned char *seed, unsigned int account);

// Convenience for account=0
char *nostr_nip06_private_key_from_seed(const unsigned char *seed);

#endif // NOSTR_NIP06_H
