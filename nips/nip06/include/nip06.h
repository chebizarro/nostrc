#ifndef NOSTR_NIP06_H
#define NOSTR_NIP06_H

#include <stdbool.h>

// Function prototypes for NIP-06
char* generate_seed_words();
unsigned char* seed_from_words(const char* words);
char* private_key_from_seed(const unsigned char* seed);
bool validate_words(const char* words);

#endif // NOSTR_NIP06_H
