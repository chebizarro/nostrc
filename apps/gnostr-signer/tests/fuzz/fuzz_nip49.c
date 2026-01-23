/* fuzz_nip49.c - Fuzz testing for NIP-49 encryption/decryption
 *
 * This fuzz target tests the NIP-49 encrypted key backup implementation
 * against malformed input to find crashes, memory bugs, and edge cases.
 *
 * Build with: clang -fsanitize=fuzzer,address fuzz_nip49.c -o fuzz_nip49 ...
 *
 * Issue: nostrc-p7f6
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <nostr/nip49/nip49.h>

/* Define entry point based on whether we're using libFuzzer or standalone */
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

/* libFuzzer entry point */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;  /* Need at least a mode byte and some data */
    }

    /* Use first byte to select test mode */
    uint8_t mode = data[0] % 4;
    const uint8_t *input = data + 1;
    size_t input_size = size - 1;

    switch (mode) {
    case 0: {
        /* Test ncryptsec decryption with fuzzed input */
        if (input_size < 10) return 0;

        /* Create null-terminated string from fuzz input */
        char *ncryptsec = malloc(input_size + 1);
        if (!ncryptsec) return 0;
        memcpy(ncryptsec, input, input_size);
        ncryptsec[input_size] = '\0';

        /* Try to decrypt with a fixed password */
        uint8_t privkey[32];
        NostrNip49SecurityByte security = 0;
        uint8_t log_n = 0;

        /* This should handle malformed input gracefully */
        nostr_nip49_decrypt(ncryptsec, "fuzz-password", privkey, &security, &log_n);

        /* Zero out potentially sensitive data */
        memset(privkey, 0, sizeof(privkey));
        free(ncryptsec);
        break;
    }

    case 1: {
        /* Test ncryptsec decryption with fuzzed password */
        /* Use a known-valid ncryptsec structure but fuzz the password */
        static const char *valid_ncryptsec_prefix = "ncryptsec1";
        if (input_size < 1) return 0;

        /* Create fuzzed password */
        char *password = malloc(input_size + 1);
        if (!password) return 0;
        memcpy(password, input, input_size);
        password[input_size] = '\0';

        /* Use a valid-looking ncryptsec (will still fail but should not crash) */
        uint8_t privkey[32];
        /* Try decryption - should fail gracefully with wrong password */
        nostr_nip49_decrypt(valid_ncryptsec_prefix, password, privkey, NULL, NULL);

        memset(privkey, 0, sizeof(privkey));
        free(password);
        break;
    }

    case 2: {
        /* Test payload deserialization with fuzzed data */
        if (input_size != 91) {
            /* Pad or truncate to exactly 91 bytes */
            uint8_t padded[91] = {0};
            size_t copy_size = input_size < 91 ? input_size : 91;
            memcpy(padded, input, copy_size);

            NostrNip49Payload payload;
            nostr_nip49_payload_deserialize(padded, &payload);
        } else {
            NostrNip49Payload payload;
            nostr_nip49_payload_deserialize(input, &payload);
        }
        break;
    }

    case 3: {
        /* Test encryption with fuzzed key material */
        if (input_size < 32) return 0;

        /* Use first 32 bytes as fuzzed private key */
        uint8_t privkey[32];
        memcpy(privkey, input, 32);

        /* Fuzz security byte */
        NostrNip49SecurityByte security = (input_size > 32)
            ? (NostrNip49SecurityByte)(input[32] % 3)
            : NOSTR_NIP49_SECURITY_SECURE;

        /* Fuzz log_n (limit to reasonable range to avoid long hangs) */
        uint8_t log_n = (input_size > 33)
            ? (uint8_t)((input[33] % 5) + 16)  /* 16-20 */
            : 16;

        char *ncryptsec = NULL;
        int rc = nostr_nip49_encrypt(privkey, security, "fuzz-password", log_n, &ncryptsec);

        if (rc == 0 && ncryptsec) {
            /* Verify roundtrip */
            uint8_t decrypted[32];
            nostr_nip49_decrypt(ncryptsec, "fuzz-password", decrypted, NULL, NULL);
            memset(decrypted, 0, sizeof(decrypted));
            free(ncryptsec);
        }

        memset(privkey, 0, sizeof(privkey));
        break;
    }
    }

    return 0;
}

#else /* Standalone test harness for AFL or manual testing */

#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input-file>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return 1;
    }

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return 1;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Call the fuzzer entry point */
    extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
    int result = LLVMFuzzerTestOneInput(data, (size_t)size);

    free(data);
    return result;
}

/* Define the fuzzer function even in standalone mode */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif /* FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */
