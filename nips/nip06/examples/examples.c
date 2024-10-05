#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <nostr/nip05.h>
#include <nostr/nip06.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Set up and initialize the JSON interface
    nostr_set_json_interface(&cjson_interface);
    nostr_json_init();

    // Example NIP-06 usage
    char* seed_words = generate_seed_words();
    if (seed_words) {
        printf("Generated Seed Words: %s\n", seed_words);

        // Validate seed words
        if (validate_words(seed_words)) {
            printf("Seed words are valid.\n");

            // Generate seed from words
            unsigned char* seed = seed_from_words(seed_words);
            if (seed) {
                printf("Seed generated from words.\n");

                // Generate private key from seed
                char* private_key = private_key_from_seed(seed);
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

    // Clean up
    nostr_json_cleanup();

    return 0;
}
