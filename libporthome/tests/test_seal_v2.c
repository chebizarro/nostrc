/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"

/* Independent reference: Python hmac/hashlib RFC5869 + cryptography's
 * ChaCha20Poly1305. seed=00..1f, child node_id=20..3f, zero bucket fill.
 * Compare whole envelopes, not just self-roundtrips or generated hashes. */
static void chunk_vector(const uint8_t key[32], const uint8_t *pt, size_t n,
                         const char *hex, const char *address) {
    uint8_t *blob = NULL, *opened = NULL, *again = NULL, hash[32];
    size_t len = 0, got = 0, again_len = 0;
    porthome_ring_entry_t entry = {.epoch = 3};
    memcpy(entry.node_key, key, 32);
    porthome_ring_t ring = {&entry, 1};
    assert(porthome_encrypt_chunk_v2(key, pt, n, &blob, &len, hash) == 0);
    expect_hex(blob, len, hex);
    expect_hex(hash, 32, address);
    assert(porthome_encrypt_chunk_v2(key, pt, n, &again, &again_len, NULL) == 0);
    assert(len == again_len && !memcmp(blob, again, len));
    free(again);
    assert(porthome_decrypt_chunk_v2(&ring, 3, blob, len, n, &opened, &got) == 0);
    assert(got == n && (!n || !memcmp(opened, pt, n)));
    free(opened);
    assert(porthome_decrypt_chunk(key, blob, len, &opened, &got) == PORTHOME_ERR_VERSION);
    assert(!opened && got == 0);
    assert(porthome_decrypt_manifest_v2(&ring, 3, blob, len, &opened, &got) == PORTHOME_ERR_CRYPTO);
    assert(!opened && got == 0);
    /* Version mutation must not redirect the v2 ciphertext into the v1 key domain. */
    blob[0] = PORTHOME_WIRE_VERSION;
    assert(porthome_decrypt_chunk(key, blob, len, &opened, &got) == PORTHOME_ERR_CRYPTO);
    blob[0] = PORTHOME_WIRE_VERSION_V2;
    for (size_t i = 1; i < len; ++i) {
        blob[i] ^= 1;
        assert(porthome_decrypt_chunk_v2(&ring, 3, blob, len, n, &opened, &got) == PORTHOME_ERR_CRYPTO);
        assert(!opened && got == 0);
        blob[i] ^= 1;
    }
    for (size_t i = 0; i < len; ++i) {
        assert(porthome_decrypt_chunk_v2(&ring, 3, blob, i, n, &opened, &got) < 0);
        assert(!opened && got == 0);
    }
    free(blob);
}

int main(void) {
    uint8_t root[32], key[32], seed[32], home[32], hash[32];
    fixture(root, key);
    chunk_vector(key, NULL, 0,
        "0226809564499106596a9f18169cdc77a69fb310bd96f28a962b92800f78eb8130e8c8bc6888680c52ea9c9a91aebe8a49630e040410c8bbc9850e2926",
        "66a051107eaf6bfaad6d3a2d46228c19e903aa78ab2c72b6ed0942a1134a955b");
    const uint8_t text[] = "shared vault PCS v2\n";
    chunk_vector(key, text, sizeof(text)-1,
        "020c7561865ae0ab7fc0af846aee333b2056b51b6647f6896bc0e36b27d7c757aac3f51c7f19bb9ab723b32dfa18464171a69614aca6b9b5bbe546c1a7",
        "81d541623c41de9fdee71f94e3ce854bda447722a2bdb183d7f5df6b5eae61b4");
    const uint8_t zero_tail[] = {'a', 0};
    chunk_vector(key, zero_tail, sizeof(zero_tail),
        "0286f61591f1108800761e913374dd684e54cfb2434eab1fb634587e3c90e8de1a585185094b621cb69e1bce6029f0bc31ab03cd1c2dc51f9fc94ad82a",
        "5714531823eb0fbba6f6baf9bb64758465f4a673e094dd559f3f7c595ee14789");
    uint8_t binary[65];
    for (size_t i = 0; i < sizeof(binary); ++i) binary[i] = (uint8_t)i;
    chunk_vector(key, binary, sizeof(binary),
        "02dabb3ddb0c7f95663abab8885d563265b5479cc3588bc383f3a424cb187ad3d08c4fc04e4a289fdcc8421947e6b4a4cf54a86570452c6a4423744571ea518c25d4be8029f12707274c0214495af4a985e60f8aa28a9a87cabdad14945117865bdb062b6e4d67c70dfdd071484f5ba6c8eaf2e67b2ee03de6a78b6567",
        "f383b7350894ac925d35759e0c9dca482b8e2b4c184337dadf767e02b09633bc");

    for (size_t i = 0; i < 32; ++i) seed[i] = (uint8_t)i;
    assert(porthome_key_derive(seed, home) == 0);
    expect_hex(home, 32, "8ca5194b0eac0a08ed4d8dccda5027902680303c0d1be6a59be4c4d35b1a5f5d");
    assert(memcmp(home, root, 32));
    uint8_t *blob = NULL, *out = NULL;
    size_t len = 0, got = 0;
    assert(porthome_encrypt_chunk(home, text, sizeof(text)-1, &blob, &len, hash) == 0);
    expect_hex(blob, len, "01ec4b043e7329a5aa63010a79b6d0c4f543d979514a500785b03c0cd3754184270b898d900dd3f9eb658f0ee52e91e5cc");
    expect_hex(hash, 32, "0aeb3f3813e92b3a27297db8ddb87af9c055a0e4694b319a72207843cc392af5");
    assert(porthome_decrypt_chunk(home, blob, len, &out, &got) == 0);
    assert(got == sizeof(text)-1 && !memcmp(out, text, got));
    free(out);
    porthome_ring_entry_t entry = {.epoch = 1};
    memcpy(entry.node_key, home, 32);
    porthome_ring_t ring = {&entry, 1};
    assert(porthome_decrypt_chunk_v2(&ring, 1, blob, len, got, &out, &got) == PORTHOME_ERR_VERSION);
    blob[0] = 2;
    assert(porthome_decrypt_manifest_v2(&ring, 1, blob, len, &out, &got) == PORTHOME_ERR_CRYPTO);
    free(blob);

    const uint8_t manifest[] = {0xa1, 0x61, 0x78, 0x01};
    memcpy(entry.node_key, key, 32);
    assert(porthome_encrypt_manifest_v2(key, manifest, sizeof(manifest), &blob, &len, hash) == 0);
    expect_hex(blob, len, "026ee674a59a34654effb8954a263ee30c528405c427f447cc62e262f7ae7adc4b");
    expect_hex(hash, 32, "95155aa446f02385dd25e81332ae6635b8ecf12479116da61cf378cc24958124");
    assert(porthome_decrypt_manifest_v2(&ring, 1, blob, len, &out, &got) == 0);
    assert(got == sizeof(manifest) && !memcmp(out, manifest, got));
    free(out);
    assert(porthome_decrypt_manifest(key, blob, len, &out, &got) == PORTHOME_ERR_VERSION);
    blob[len-1] ^= 1;
    assert(porthome_decrypt_manifest_v2(&ring, 1, blob, len, &out, &got) == PORTHOME_ERR_CRYPTO);
    assert(!out && !got);
    free(blob);
    return 0;
}
