#ifndef NOSTR_KEYS_H
#define NOSTR_KEYS_H

#include <stdbool.h>

// key generation and validation
char *generate_private_key(void);
char *get_public_key(const char *sk);
bool is_valid_public_key_hex(const char *pk);
bool is_valid_public_key(const char *pk);

#endif // NOSTR_KEYS_H
