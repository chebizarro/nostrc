#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <stdlib.h>
#include <stdbool.h>

// Function prototypes for key generation and validation
char *generate_private_key();
char *get_public_key(const char *sk);
bool is_valid_public_key_hex(const char *pk);
bool is_valid_public_key(const char *pk);

#endif // CRYPTO_UTILS_H