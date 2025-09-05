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

            // Generate seed from words (secure variant)
            nostr_secure_buf sb = nostr_nip06_seed_secure(seed_words);
            if (sb.ptr && sb.len == 64) {
                printf("Seed generated from words (secure).\n");

                // Generate private key from seed (use sb.ptr)
                char* private_key = nostr_nip06_private_key_from_seed((const unsigned char*)sb.ptr);
                if (private_key) {
                    printf("Private Key: %s\n", private_key);
                    free(private_key);
                }
                secure_free(&sb);
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
