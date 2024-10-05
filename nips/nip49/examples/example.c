#include <stdio.h>
#include "nip49.h"

int main() {
    const char *secret_key = "d0e5fa7c3ff1e8c90741bb4af8e63934b539cd545b5ed2041f8e07484e209f92";
    const char *password = "supersecurepassword";
    uint8_t logn = 10;
    KeySecurityByte ksb = NOT_KNOWN_TO_HAVE_BEEN_HANDLED_INSECURELY;

    char *encrypted_key;
    if (nip49_encrypt(secret_key, password, logn, ksb, &encrypted_key) != 0) {
        fprintf(stderr, "Encryption failed\n");
        return -1;
    }

    printf("Encrypted key: %s\n", encrypted_key);

    char *decrypted_key;
    if (nip49_decrypt(encrypted_key, password, &decrypted_key) != 0) {
        fprintf(stderr, "Decryption failed\n");
        free(encrypted_key);
        return -1;
    }

    printf("Decrypted key: %s\n", decrypted_key);

    free(encrypted_key);
    free(decrypted_key);
    return 0;
}
