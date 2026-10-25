/* Public-header consumer, also used for installed static/shared package checks. */
#define _POSIX_C_SOURCE 200809L
#include <porthome/crypto.h>
#include <porthome/manifest.h>
#include <porthome/wrapkey.h>
#include <porthome/cbor.h>
#include <porthome/index.h>
#include <porthome/compat-nh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

typedef struct { int present; porthome_index_entry row; } memory_index;
static porthome_index_status lookup(void *ctx, const char *oid, porthome_index_entry *out)
{
    memory_index *m = ctx;
    if (!m->present || strcmp(oid, m->row.git_oid)) return PORTHOME_INDEX_NOT_FOUND;
    *out = m->row;
    return PORTHOME_INDEX_OK;
}
static porthome_index_status insert(void *ctx, const porthome_index_entry *row)
{
    memory_index *m = ctx;
    m->present = 1;
    m->row = *row;
    return PORTHOME_INDEX_OK;
}
static porthome_index_status iterate(void *ctx, porthome_index_visit_fn visit, void *vctx)
{
    memory_index *m = ctx;
    return m->present ? visit(vctx, &m->row) : PORTHOME_INDEX_OK;
}
static porthome_index_status visit(void *ctx, const porthome_index_entry *row)
{
    unsigned *count = ctx;
    CHECK(row->chunk_key_id == 7 && row->sealed_sha256[0] == 0x42);
    ++*count;
    return PORTHOME_INDEX_OK;
}
static porthome_index_status stop(void *ctx, const porthome_index_entry *row)
{
    (void)ctx; (void)row;
    return PORTHOME_INDEX_ERR_STORAGE;
}
int main(void)
{
    uint8_t seed[32] = {1}, key[32], compat_key[32], digest[32];
    CHECK(porthome_key_derive(seed, key) == PORTHOME_OK);
    CHECK(nh_porthome_key_derive(seed, compat_key) == NH_PORTHOME_OK);
    CHECK(!memcmp(key, compat_key, sizeof(key)));
    CHECK(porthome_wrap_seed_from_ikm(seed, sizeof(seed), compat_key) == PORTHOME_OK);
    uint8_t *ct = NULL, *pt = NULL;
    size_t ct_len = 0, pt_len = 0;
    CHECK(porthome_encrypt_chunk(key, (const uint8_t *)"hello", 5, &ct, &ct_len, digest) == 0);
    CHECK(porthome_decrypt_chunk(key, ct, ct_len, &pt, &pt_len) == 0);
    CHECK(pt_len == 5 && !memcmp(pt, "hello", 5));
    free(pt);
    ct[ct_len - 1] ^= 1;
    CHECK(porthome_decrypt_chunk(key, ct, ct_len, &pt, &pt_len) < 0);
    free(ct);

    char name[PORTHOME_NAME_HEX_LEN];
    CHECK(porthome_encrypt_name(key, "notes", name) == 0);
    CHECK(strlen(name) == 48);
    CHECK(porthome_encrypt_name(key, "..", name) == PORTHOME_ERR_PATH);
    porthome_manifest manifest, *decoded = NULL;
    CHECK(porthome_manifest_init_v2(&manifest, digest) == 0);
    CHECK(porthome_manifest_add_dir_v2(&manifest, strdup(name), strdup("notes"), 0700, 0, 0, 0) == 0);
    CHECK(porthome_manifest_encode_sealed(&manifest, key, &ct, &ct_len) == 0);
    CHECK(porthome_manifest_decode_sealed(ct, ct_len, key, &decoded) == 0);
    CHECK(decoded->entries_len == 1 && !strcmp(decoded->entries[0].name_plain, "notes"));
    free(ct);
    porthome_manifest_dispose(decoded); free(decoded);
    porthome_manifest_dispose(&manifest);

    porthome_cbor_enc enc;
    CHECK(porthome_cbor_enc_init(&enc, 0) == 0);
    CHECK(porthome_cbor_write_uint(&enc, 123) == 0);
    CHECK(porthome_cbor_enc_take(&enc, &ct, &ct_len) == 0);
    porthome_cbor_dec dec;
    porthome_cbor_dec_init(&dec, ct, ct_len);
    uint64_t value = 0;
    CHECK(porthome_cbor_read_uint(&dec, &value) == 0 && value == 123);
    CHECK(porthome_cbor_dec_at_end(&dec));
    free(ct); porthome_cbor_enc_dispose(&enc);

    memory_index memory = {0};
    static const porthome_index_ops ops = { lookup, insert, iterate };
    porthome_index index = { &memory, &ops };
    porthome_index_entry row = { .git_oid = "0123456789012345678901234567890123456789",
                                 .sealed_sha256 = {0x42}, .chunk_key_id = 7 }, found;
    CHECK(porthome_index_lookup(&index, row.git_oid, &found) == PORTHOME_INDEX_NOT_FOUND);
    CHECK(porthome_index_insert(&index, &row) == PORTHOME_INDEX_OK);
    CHECK(porthome_index_lookup(&index, row.git_oid, &found) == PORTHOME_INDEX_OK);
    CHECK(found.chunk_key_id == row.chunk_key_id && !memcmp(found.sealed_sha256, row.sealed_sha256, 32));
    unsigned count = 0;
    CHECK(porthome_index_iterate(&index, visit, &count) == PORTHOME_INDEX_OK && count == 1);
    CHECK(porthome_index_iterate(&index, stop, NULL) == PORTHOME_INDEX_ERR_STORAGE);
    CHECK(porthome_index_insert(NULL, &row) == PORTHOME_INDEX_ERR_ARG);
    CHECK(porthome_index_lookup(&index, "../bad", &found) == PORTHOME_INDEX_ERR_ARG);
    CHECK(porthome_index_iterate(&index, NULL, NULL) == PORTHOME_INDEX_ERR_ARG);
    memset(row.git_oid, 'a', sizeof(row.git_oid));
    CHECK(porthome_index_insert(&index, &row) == PORTHOME_INDEX_ERR_ARG);
    puts("libporthome public API: OK");
    return 0;
}
