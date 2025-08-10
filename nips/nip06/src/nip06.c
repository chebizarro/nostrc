#include "nip06.h"
#include <wally_bip39.h>
#include <wally_core.h>
#include <wally_bip32.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

char *nostr_nip06_generate_mnemonic(void) {
    uint8_t entropy[32];
    if (RAND_bytes(entropy, (int)sizeof(entropy)) != 1) {
        return NULL;
    }
    char *mnemonic = NULL;
    if (bip39_mnemonic_from_bytes(NULL, entropy, sizeof(entropy), &mnemonic) != WALLY_OK) {
        return NULL;
    }
    return mnemonic; /* libwally allocs; caller must free */
}

bool nostr_nip06_validate_mnemonic(const char *mnemonic) {
    return mnemonic && bip39_mnemonic_validate(NULL, mnemonic) == WALLY_OK;
}

unsigned char *nostr_nip06_seed_from_mnemonic(const char *mnemonic) {
    if (!mnemonic) return NULL;
    unsigned char *seed = (unsigned char *)malloc(BIP39_SEED_LEN_512);
    if (!seed) return NULL;
    if (bip39_mnemonic_to_seed(mnemonic, "", seed, BIP39_SEED_LEN_512, NULL) != WALLY_OK) {
        free(seed);
        return NULL;
    }
    return seed;
}

static char *hex_from_priv_key32(const unsigned char *key32) {
    char *hex = NULL;
    if (wally_hex_from_bytes(key32, 32, &hex) != WALLY_OK) return NULL;
    return hex; /* libwally allocs; caller must free */
}

char *nostr_nip06_private_key_from_seed_account(const unsigned char *seed, unsigned int account) {
    if (!seed) return NULL;
    struct ext_key master_key;
    if (bip32_key_from_seed(seed, BIP39_SEED_LEN_512, BIP32_VER_MAIN_PRIVATE, 0, &master_key) != WALLY_OK) {
        return NULL;
    }
    uint32_t path[] = {
        BIP32_INITIAL_HARDENED_CHILD + 44,
        BIP32_INITIAL_HARDENED_CHILD + 1237,
        BIP32_INITIAL_HARDENED_CHILD + account,
        0,
        0,
    };
    struct ext_key derived;
    if (bip32_key_from_parent_path(&master_key, path, sizeof(path)/sizeof(path[0]), BIP32_FLAG_KEY_PRIVATE, &derived) != WALLY_OK) {
        return NULL;
    }
    /* ext_key.priv_key is 33 bytes with a leading 0; use last 32 bytes */
    return hex_from_priv_key32(derived.priv_key + 1);
}

char *nostr_nip06_private_key_from_seed(const unsigned char *seed) {
    return nostr_nip06_private_key_from_seed_account(seed, 0u);
}
