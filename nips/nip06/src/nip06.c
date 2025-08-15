#include "nip06.h"
#include <nostr/crypto/bip39.h>
#include <nostr/crypto/bip32.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

/* Generate a 24-word English mnemonic per BIP-39. */
char *nostr_nip06_generate_mnemonic(void) {
    return nostr_bip39_generate(24);
}

bool nostr_nip06_validate_mnemonic(const char *mnemonic) {
    return nostr_bip39_validate(mnemonic);
}

unsigned char *nostr_nip06_seed_from_mnemonic(const char *mnemonic) {
    if (!mnemonic) return NULL;
    unsigned char *seed = (unsigned char *)malloc(64);
    if (!seed) return NULL;
    if (!nostr_bip39_seed(mnemonic, "", seed)) {
        free(seed);
        return NULL;
    }
    return seed;
}

static char *hex_from_priv_key32(const unsigned char *key32) {
    static const char *hexchars = "0123456789abcdef";
    char *hex = (char *)malloc(65);
    if (!hex) return NULL;
    for (size_t i = 0; i < 32; ++i) {
        hex[2*i]   = hexchars[(key32[i] >> 4) & 0xF];
        hex[2*i+1] = hexchars[key32[i] & 0xF];
    }
    hex[64] = '\0';
    return hex;
}

char *nostr_nip06_private_key_from_seed_account(const unsigned char *seed, unsigned int account) {
    if (!seed) return NULL;
    uint32_t path[] = {
        0x80000000u | 44u,
        0x80000000u | 1237u,
        0x80000000u | account,
        0u,
        0u,
    };
    unsigned char out32[32];
    if (!nostr_bip32_priv_from_master_seed(seed, 64, path, sizeof(path)/sizeof(path[0]), out32)) {
        return NULL;
    }
    return hex_from_priv_key32(out32);
}

char *nostr_nip06_private_key_from_seed(const unsigned char *seed) {
    return nostr_nip06_private_key_from_seed_account(seed, 0u);
}
