#include "crypto_utils.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    // Test generate_private_key
    char *private_key = generate_private_key();
    assert(private_key != NULL);
    printf("Private Key: %s\n", private_key);

    // Test get_public_key
    char *public_key = get_public_key(private_key);
    assert(public_key != NULL);
    printf("Public Key: %s\n", public_key);

    // Test is_valid_public_key_hex
    assert(is_valid_public_key_hex(public_key) == true);
    assert(is_valid_public_key_hex("invalid_hex_string") == false);

    // Test is_valid_public_key
    assert(is_valid_public_key(public_key) == true);
    assert(is_valid_public_key("invalid_public_key") == false);

    free(private_key);
    free(public_key);

    printf("All tests passed!\n");
    return 0;
}