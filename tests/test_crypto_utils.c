#include "crypto_utils.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    // Test nostr_key_generate_private
    char *private_key = nostr_key_generate_private();
    assert(private_key != NULL);
    printf("Private Key: %s\n", private_key);

    // Test nostr_key_get_public
    char *public_key = nostr_key_get_public(private_key);
    assert(public_key != NULL);
    printf("Public Key: %s\n", public_key);

    // Test nostr_key_is_valid_public_hex
    assert(nostr_key_is_valid_public_hex(public_key) == true);
    assert(nostr_key_is_valid_public_hex("invalid_hex_string") == false);

    // Test nostr_key_is_valid_public
    assert(nostr_key_is_valid_public(public_key) == true);
    assert(nostr_key_is_valid_public("invalid_public_key") == false);

    free(private_key);
    free(public_key);

    printf("All tests passed!\n");
    return 0;
}