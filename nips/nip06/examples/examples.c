#include "nip06.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Example NIP-06 usage
    char* seed_words = nostr_nip06_generate_mnemonic();
    if (seed_words) {
        printf("Generated Seed Words: %s\n", seed_words);

        // Validate seed words
        if (nostr_nip06_validate_mnemonic(seed_words)) {
            printf("Seed words are valid.\n");

            // Generate seed from words
            unsigned char* seed = nostr_nip06_seed_from_mnemonic(seed_words);
            if (seed) {
                printf("Seed generated from words.\n");

                // Generate private key from seed
                char* private_key = nostr_nip06_private_key_from_seed(seed);
                if (private_key) {
                    printf("Private Key: %s\n", private_key);
                    free(private_key);
                }

                free(seed);
            }
        } else {
            printf("Seed words are invalid.\n");
        }

        free(seed_words);
    } else {
        printf("Failed to generate seed words.\n");
    }

    return 0;
}
