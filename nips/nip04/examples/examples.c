#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <stdio.h>

int main() {
    // Set up and initialize the JSON interface
    nostr_set_json_interface(&cjson_interface);
    nostr_json_init();

    // Example keys
    const char *priv_key = "private-key-hex";
    const char *pub_key = "public-key-hex";

    // Compute shared secret
    char *shared_secret = compute_shared_secret(pub_key, priv_key);
    if (shared_secret) {
        printf("Shared Secret: %s\n", shared_secret);
    }

    // Encrypt message
    const char *message = "Hello, Nostr!";
    char *encrypted_message = encrypt_message(message, shared_secret);
    if (encrypted_message) {
        printf("Encrypted Message: %s\n", encrypted_message);
    }

    // Decrypt message
    char *decrypted_message = decrypt_message(encrypted_message, shared_secret);
    if (decrypted_message) {
        printf("Decrypted Message: %s\n", decrypted_message);
    }

    // Clean up
    nostr_event_free(event);
    nostr_json_cleanup();
    free(shared_secret);
    free(encrypted_message);
    free(decrypted_message);

    return 0;
}
