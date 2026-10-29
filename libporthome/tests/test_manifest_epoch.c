/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"
#include <porthome/dir-node.h>
#include <stdio.h>

static int failures;

static void expect_error(const char *label, int rc, int expected,
                         porthome_manifest *out) {
    if (rc != expected || out != NULL) {
        fprintf(stderr, "%s: expected %d and NULL, got %d and %s\n",
                label, expected, rc, out ? "non-NULL" : "NULL");
        ++failures;
    }
    if (out && out != (void *)1) {
        porthome_manifest_dispose(out);
        free(out);
    }
}

int main(void) {
    uint8_t root[32], key[32];
    fixture(root, key);
    porthome_manifest *out;
    uint8_t *cbor = NULL, *sealed = NULL;
    size_t cbor_len = 0, sealed_len = 0;
    porthome_dir_node node = {.epoch = 3};
    assert(porthome_dir_node_encode(&node, &cbor, &cbor_len) == PORTHOME_OK);
    out = (void *)1;
    int rc = porthome_manifest_decode(cbor, cbor_len, &out);
    expect_error("PCS v2 node CBOR", rc, PORTHOME_ERR_EPOCH, out);
    /* A malformed node is still an argument error, not an epoch refusal. */
    out = (void *)1;
    rc = porthome_manifest_decode(cbor, cbor_len - 1, &out);
    expect_error("truncated node CBOR", rc, PORTHOME_ERR_ARG, out);
    free(cbor);
    assert(porthome_dir_node_seal(&node, key, &sealed, &sealed_len, NULL) == PORTHOME_OK);
    out = (void *)1;
    rc = porthome_manifest_decode_sealed(sealed, sealed_len, key, &out);
    expect_error("PCS v2 sealed node", rc, PORTHOME_ERR_EPOCH, out);
    sealed[0] = 3;
    out = (void *)1;
    rc = porthome_manifest_decode_sealed(sealed, sealed_len, key, &out);
    expect_error("unknown sealed version", rc, PORTHOME_ERR_VERSION, out);
    free(sealed);

    /* The old flat name-schema v2 also uses the PCS v1 manifest codec. */
    for (unsigned version = 1; version <= 2; ++version) {
        porthome_manifest legacy;
        assert((version == 1 ? porthome_manifest_init(&legacy, root)
                            : porthome_manifest_init_v2(&legacy, root)) == PORTHOME_OK);
        char path[49]; memset(path, 'a', 48); path[48] = 0;
        porthome_chunk chunk = {.size = 32, .chunk_key_id = 0};
        assert((version == 1
            ? porthome_manifest_add_file(&legacy, strdup(path), 0600, 0, 0, 0, 3, &chunk, 1)
            : porthome_manifest_add_file_v2(&legacy, strdup(path), strdup("file"), 0600, 0, 0, 0, 3, &chunk, 1)) == PORTHOME_OK);
        /* First prove the same payload with key ID zero still round-trips. */
        assert(porthome_manifest_encode_sealed(&legacy, key, &sealed, &sealed_len) == PORTHOME_OK);
        out = NULL;
        assert(porthome_manifest_decode_sealed(sealed, sealed_len, key, &out) == PORTHOME_OK);
        assert(out->entries[0].chunks[0].chunk_key_id == 0);
        porthome_manifest_dispose(out); free(out); free(sealed);
        const uint32_t epochs[] = {1, UINT32_MAX};
        for (size_t i = 0; i < sizeof(epochs) / sizeof(epochs[0]); ++i) {
            legacy.entries[0].chunks[0].chunk_key_id = epochs[i];
            assert(porthome_manifest_encode(&legacy, &cbor, &cbor_len) == PORTHOME_OK);
            out = (void *)1;
            rc = porthome_manifest_decode(cbor, cbor_len, &out);
            expect_error("nonzero chunk epoch CBOR", rc, PORTHOME_ERR_EPOCH, out);
            free(cbor);
            assert(porthome_manifest_encode_sealed(&legacy, key, &sealed, &sealed_len) == PORTHOME_OK);
            out = (void *)1;
            rc = porthome_manifest_decode_sealed(sealed, sealed_len, key, &out);
            expect_error("nonzero chunk epoch sealed", rc, PORTHOME_ERR_EPOCH, out);
            sealed[sealed_len - 1] ^= 1;
            out = (void *)1;
            rc = porthome_manifest_decode_sealed(sealed, sealed_len, key, &out);
            expect_error("corrupt v1 AEAD", rc, PORTHOME_ERR_CRYPTO, out);
            free(sealed);
        }
        porthome_manifest_dispose(&legacy);
    }
    return failures ? 1 : 0;
}
