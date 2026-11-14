#include <fips/noise/aead.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) abort(); } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Key, counter, AD length; short inputs are zero padded. */
    uint8_t header[41] = {0};
    size_t used = size < sizeof(header) ? size : sizeof(header);
    if (used) memcpy(header, data, used);
    data += used;
    size -= used;
    uint64_t counter = 0;
    for (unsigned i = 0; i < 8; ++i) counter |= (uint64_t)header[32 + i] << (8 * i);
    size_t ad_len = header[40] < size ? header[40] : size;
    const uint8_t *ad = data;
    data += ad_len;
    size -= ad_len;
    fips_aead_key key;
    REQUIRE(fips_aead_key_init(&key, header) == FIPS_AEAD_OK);
    uint8_t out[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    uint8_t wire[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    uint8_t plain[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    memset(out, 0xa5, sizeof(out));
    size_t got = SIZE_MAX;
    fips_aead_status rc = fips_aead_open(&key, counter, ad, ad_len, data, size,
                                        out, sizeof(out) - 1, &got);
    if (rc == FIPS_AEAD_OK) {
        REQUIRE(size >= FIPS_NOISE_TAG_SIZE && got == size - FIPS_NOISE_TAG_SIZE);
    } else {
        REQUIRE(got == SIZE_MAX);
        size_t zeroed = (rc == FIPS_AEAD_ERR_AUTH && size >= FIPS_NOISE_TAG_SIZE)
                        ? size - FIPS_NOISE_TAG_SIZE : 0;
        for (size_t i = 0; i < sizeof(out); ++i)
            REQUIRE(out[i] == (i < zeroed ? 0 : 0xa5));
    }
    REQUIRE(out[sizeof(out) - 1] == 0xa5);

    size_t n = size;
    if (n > FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE)
        n = FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE;
    size_t clen = n + FIPS_NOISE_TAG_SIZE;
    REQUIRE(fips_aead_seal(&key, counter, ad, ad_len, data, n,
                           wire, clen, &got) == FIPS_AEAD_OK && got == clen);
    REQUIRE(fips_aead_open(&key, counter, ad, ad_len, wire, clen,
                           plain, n, &got) == FIPS_AEAD_OK && got == n);
    REQUIRE(memcmp(plain, data, n) == 0);
    /* Exercise in-place authentication failure and its no-plaintext guarantee. */
    wire[clen - 1] ^= 1;
    got = SIZE_MAX;
    REQUIRE(fips_aead_open(&key, counter, ad, ad_len, wire, clen,
                           wire, n, &got) == FIPS_AEAD_ERR_AUTH && got == SIZE_MAX);
    for (size_t i = 0; i < n; ++i) REQUIRE(wire[i] == 0);
    memset(out, 0xa5, sizeof(out));
    REQUIRE(fips_aead_seal(&key, counter, ad, ad_len, data, n,
                           out, clen - 1, &got) == FIPS_AEAD_ERR_RANGE);
    REQUIRE(got == SIZE_MAX);
    for (size_t i = 0; i < sizeof(out); ++i) REQUIRE(out[i] == 0xa5);
    fips_aead_key_destroy(&key);
    for (size_t i = 0; i < sizeof(key.bytes); ++i) REQUIRE(key.bytes[i] == 0);
    return 0;
}
