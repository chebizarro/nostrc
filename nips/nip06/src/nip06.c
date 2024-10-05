#include "nostr/nip06.h"
#include <wally_bip39.h>
#include <wally_core.h>
#include <stdlib.h>
#include <string.h>

char* generate_seed_words() {
    uint8_t entropy[32];
    if (wally_bip39_mnemonic_from_bytes(NULL, entropy, sizeof(entropy), NULL) != WALLY_OK) {
        return NULL;
    }

    char *mnemonic;
    if (wally_bip39_mnemonic_from_bytes(NULL, entropy, sizeof(entropy), &mnemonic) != WALLY_OK) {
        return NULL;
    }

    return mnemonic;
}

unsigned char* seed_from_words(const char* words) {
    unsigned char *seed = malloc(BIP39_SEED_LEN_512);
    if (!seed) {
        return NULL;
    }

    if (wally_bip39_mnemonic_to_seed(words, "", seed, BIP39_SEED_LEN_512, NULL) != WALLY_OK) {
        free(seed);
        return NULL;
    }

    return seed;
}

char* private_key_from_seed(const unsigned char* seed) {
    struct ext_key master_key;
    if (bip32_key_from_seed(seed, BIP39_SEED_LEN_512, BIP32_VER_MAIN_PRIVATE, 0, &master_key) != WALLY_OK) {
        return NULL;
    }

    uint32_t derivation_path[] = {
        BIP32_INITIAL_HARDENED_CHILD + 44,
        BIP32_INITIAL_HARDENED_CHILD + 1237,
        BIP32_INITIAL_HARDENED_CHILD + 0,
        0,
        0,
    };

    struct ext_key derived_key;
    if (bip32_key_from_parent_path(&master_key, derivation_path, sizeof(derivation_path) / sizeof(derivation_path[0]), BIP32_FLAG_KEY_PRIVATE, &derived_key) != WALLY_OK) {
        return NULL;
    }

    char *private_key = malloc(HEX_LEN(sizeof(derived_key.priv_key)));
    if (!private_key) {
        return NULL;
    }

    if (wally_hex_from_bytes(derived_key.priv_key, sizeof(derived_key.priv_key), &private_key) != WALLY_OK) {
        free(private_key);
        return NULL;
    }

    return private_key;
}

bool validate_words(const char* words) {
    return wally_bip39_mnemonic_validate(NULL, words) == WALLY_OK;
}
