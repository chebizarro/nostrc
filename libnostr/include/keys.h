#ifndef NOSTR_KEYS_H
#define NOSTR_KEYS_H

#include <stdbool.h>

// Canonical key generation and validation API
char *nostr_key_generate_private(void);
char *nostr_key_get_public(const char *sk);
// Returns compressed SEC1 public key (33 bytes -> 66 hex)
char *nostr_key_get_public_sec1_compressed(const char *sk);
bool  nostr_key_is_valid_public_hex(const char *pk);
bool  nostr_key_is_valid_public(const char *pk);

#endif // NOSTR_KEYS_H
