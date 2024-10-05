#ifndef NOSTR_NIP04_H
#define NOSTR_NIP04_H

#include <stddef.h>

// Function prototypes
char* compute_shared_secret(const char *pub, const char *sk);
char* encrypt_message(const char *message, const char *key);
char* decrypt_message(const char *content, const char *key);

#endif // NOSTR_NIP04_H
