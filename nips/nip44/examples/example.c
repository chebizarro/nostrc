#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nip44.h"

// Sample public and private keys for demonstration purposes
const char *pubkey = "03a34b3d9e3c5e4b1eebba47c33b39bc14d2a947bb1f27c7b84d65fdd3f6b7a6ac";
const char *privkey = "5J3mBbAH58CERBBxgHiTr2Y29RbJ5jA63ZdG9yKL9jSJGhzwuoh";

// Utility function to convert a hex string to a byte array
void hex_to_bytes(const char *hex, uint8_t *bytes) {
    for (size_t i = 0; i < strlen(hex) / 2; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    }
}

int main() {
    // Generate the conversation key
    uint8_t conversation_key[32];
    nip44_generate_conversation_key(pubkey, privkey, conversation_key);

    // Message to encrypt
    const char *message = "Hello, Nostr!";

    // Encrypt the message
    char *encrypted_message = nip44_encrypt(message, conversation_key, NULL);
    if (!encrypted_message) {
        fprintf(stderr, "Encryption failed\n");
        return 1;
    }

    printf("Encrypted Message: %s\n", encrypted_message);

    // Decrypt the message
    char *decrypted_message = nip44_decrypt(encrypted_message, conversation_key);
    if (!decrypted_message) {
        fprintf(stderr, "Decryption failed\n");
        free(encrypted_message);
        return 1;
    }

    printf("Decrypted Message: %s\n", decrypted_message);

    // Clean up
    free(encrypted_message);
    free(decrypted_message);

    return 0;
}
