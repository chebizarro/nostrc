#include <fips/identity/encoding.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

static fips_secp *secp;
static void fixed_fill(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (n) memset(buf, 0x5a, n);
}
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    fips_entropy entropy = {fixed_fill, NULL};
    if (fips_identity_init_secp(entropy, &secp) != FIPS_IDENTITY_OK) abort();
    return 0;
}
static unsigned char fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c+'a'-'A') : c;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *text = (const char *)data;
    fips_xonly_pubkey key, before;
    memset(&key, 0xa5, sizeof(key)); before = key;
    fips_identity_status status = fips_decode_npub(secp, text, size, &key);
    char canonical[64];
    if (status == FIPS_IDENTITY_OK) {
        if (fips_encode_npub(&key, canonical, sizeof(canonical)) != FIPS_IDENTITY_OK || size != 63) abort();
        for (size_t i = 0; i < size; ++i) if (fold(data[i]) != (unsigned char)canonical[i]) abort();
        fips_xonly_pubkey round;
        if (fips_decode_npub(secp, canonical, 63, &round) != FIPS_IDENTITY_OK || memcmp(&key, &round, sizeof(key))) abort();
    } else if (memcmp(&key, &before, sizeof(key))) abort();
    uint8_t secret[32], expected[32]; memset(secret, 0xa5, sizeof(secret)); memcpy(expected, secret, 32);
    status = fips_decode_nsec(secp, text, size, secret);
    if (status == FIPS_IDENTITY_OK) {
        if (fips_encode_nsec(secp, secret, canonical, sizeof(canonical)) != FIPS_IDENTITY_OK || size != 63) abort();
        for (size_t i = 0; i < size; ++i) if (fold(data[i]) != (unsigned char)canonical[i]) abort();
        uint8_t round[32];
        if (fips_decode_nsec(secp, canonical, 63, round) != FIPS_IDENTITY_OK || memcmp(secret, round, 32)) abort();
        sodium_memzero(round, sizeof(round));
    } else if (memcmp(secret, expected, sizeof(secret))) abort();
    sodium_memzero(secret, sizeof(secret)); sodium_memzero(canonical, sizeof(canonical));
    return 0;
}
