/* fuzz_bip39.c - Fuzz testing for BIP-39 mnemonic parsing
 *
 * This fuzz target tests the BIP-39 mnemonic validation and seed derivation
 * against malformed input to find crashes, memory bugs, and edge cases.
 *
 * Build with: clang -fsanitize=fuzzer,address fuzz_bip39.c -o fuzz_bip39 ...
 *
 * Issue: nostrc-p7f6
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <nostr/crypto/bip39.h>

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
        /* Test mnemonic validation with fuzzed input */
        if (input_size == 0) return 0;

        /* Create null-terminated string from fuzz input */
        char *mnemonic = malloc(input_size + 1);
        if (!mnemonic) return 0;
        memcpy(mnemonic, input, input_size);
        mnemonic[input_size] = '\0';

        /* Replace non-printable chars with spaces to simulate word separation */
        for (size_t i = 0; i < input_size; i++) {
            if (mnemonic[i] < 32 || mnemonic[i] > 126) {
                mnemonic[i] = ' ';
            }
        }

        /* This should handle malformed input gracefully */
        bool valid = nostr_bip39_validate(mnemonic);
        (void)valid;  /* Ignore result, we're testing for crashes */

        free(mnemonic);
        break;
    }

    case 1: {
        /* Test seed derivation with fuzzed mnemonic */
        if (input_size < 10) return 0;

        /* Create null-terminated mnemonic */
        char *mnemonic = malloc(input_size + 1);
        if (!mnemonic) return 0;
        memcpy(mnemonic, input, input_size);
        mnemonic[input_size] = '\0';

        /* Sanitize to printable ASCII */
        for (size_t i = 0; i < input_size; i++) {
            if (mnemonic[i] < 32 || mnemonic[i] > 126) {
                mnemonic[i] = ' ';
            }
        }

        /* Try seed derivation - should fail gracefully on invalid mnemonic */
        uint8_t seed[64];
        bool success = nostr_bip39_seed(mnemonic, "", seed);
        (void)success;  /* Ignore result */

        memset(seed, 0, sizeof(seed));
        free(mnemonic);
        break;
    }

    case 2: {
        /* Test seed derivation with fuzzed passphrase */
        /* Use a valid mnemonic but fuzz the passphrase */
        static const char *valid_mnemonic =
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about";

        if (input_size == 0) return 0;

        /* Create fuzzed passphrase */
        char *passphrase = malloc(input_size + 1);
        if (!passphrase) return 0;
        memcpy(passphrase, input, input_size);
        passphrase[input_size] = '\0';

        /* Seed derivation with fuzzed passphrase should not crash */
        uint8_t seed[64];
        bool success = nostr_bip39_seed(valid_mnemonic, passphrase, seed);
        (void)success;

        memset(seed, 0, sizeof(seed));
        free(passphrase);
        break;
    }

    case 3: {
        /* Test validation with word-like structure */
        /* This creates strings that look more like actual mnemonics */
        if (input_size < 24) return 0;

        /* Build a mnemonic-like string from fuzz input */
        /* Each 2 bytes selects a word index (0-2047) */
        static const char *sample_words[] = {
            "abandon", "ability", "able", "about", "above", "absent",
            "absorb", "abstract", "absurd", "abuse", "access", "accident",
            "account", "accuse", "achieve", "acid", "acoustic", "acquire",
            "across", "act", "action", "actor", "actress", "actual",
            "adapt", "add", "addict", "address", "adjust", "admit",
            "adult", "advance", "advice", "aerobic", "affair", "afford",
            "afraid", "again", "age", "agent", "agree", "ahead",
            "aim", "air", "airport", "aisle", "alarm", "album"
        };
        const size_t num_sample_words = sizeof(sample_words) / sizeof(sample_words[0]);

        /* Build 12-word mnemonic from fuzz bytes */
        char mnemonic[1024] = {0};
        size_t offset = 0;

        for (int i = 0; i < 12 && offset < sizeof(mnemonic) - 50; i++) {
            size_t idx = (input[i * 2] * 256 + input[i * 2 + 1]) % num_sample_words;
            size_t word_len = strlen(sample_words[idx]);
            memcpy(mnemonic + offset, sample_words[idx], word_len);
            offset += word_len;
            if (i < 11) {
                mnemonic[offset++] = ' ';
            }
        }

        /* Validate the constructed mnemonic */
        bool valid = nostr_bip39_validate(mnemonic);

        /* If valid (unlikely with random words), try seed derivation */
        if (valid) {
            uint8_t seed[64];
            nostr_bip39_seed(mnemonic, "", seed);
            memset(seed, 0, sizeof(seed));
        }
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
