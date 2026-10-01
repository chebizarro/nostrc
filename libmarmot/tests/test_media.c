/*
 * libmarmot - MIP-04 encrypted media v2 and group image component tests
 *
 * Fixtures: tests/vectors/media/ (MDK v0.11.0, see its README).
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "media_v2.h"
#include "mls/mls_key_schedule.h"

#include <assert.h>
#include <jansson.h>
#include <openssl/sha.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) do { printf("  %-56s", #name); name(); printf("PASS\n"); } while (0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

static json_t *
load_fixture(const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", MARMOT_MEDIA_VECTORS_DIR, name);
    json_error_t e;
    json_t *root = json_load_file(path, JSON_ALLOW_NUL, &e);
    if (!root) fprintf(stderr, "\n%s: %s (line %d)\n", path, e.text, e.line);
    assert(root);
    return root;
}

static const char *
jstr(json_t *obj, const char *key)
{
    json_t *v = json_object_get(obj, key);
    assert(v && json_is_string(v));
    return json_string_value(v);
}

/* Hex string -> malloc'd bytes. */
static uint8_t *
unhex(const char *hex, size_t *out_len)
{
    size_t n = strlen(hex);
    assert(n % 2 == 0);
    uint8_t *out = malloc(n / 2 + 1);
    assert(out);
    size_t bin_len = 0;
    assert(sodium_hex2bin(out, n / 2 + 1, hex, n, NULL, &bin_len, NULL) == 0);
    assert(bin_len == n / 2);
    if (out_len) *out_len = bin_len;
    return out;
}

static void
unhex_fixed(const char *hex, uint8_t *out, size_t n)
{
    size_t len = 0;
    uint8_t *b = unhex(hex, &len);
    assert(len == n);
    memcpy(out, b, n);
    free(b);
}

static void
assert_bytes_hex(const uint8_t *b, size_t n, const char *hex)
{
    size_t len = 0;
    uint8_t *want = unhex(hex, &len);
    assert(len == n);
    assert(memcmp(b, want, n) == 0);
    free(want);
}

/* A JSON string array -> fields + lengths (lengths keep embedded NULs). */
typedef struct {
    const char **f;
    size_t      *lens;
    size_t       n;
} Fields;

static Fields
fields_of(json_t *arr)
{
    Fields x = { 0 };
    x.n = json_array_size(arr);
    x.f = calloc(x.n + 1, sizeof *x.f);
    x.lens = calloc(x.n + 1, sizeof *x.lens);
    assert(x.f && x.lens);
    for (size_t i = 0; i < x.n; i++) {
        json_t *s = json_array_get(arr, i);
        x.f[i] = json_string_value(s);
        x.lens[i] = json_string_length(s);
    }
    return x;
}

static void
fields_free(Fields *x)
{
    free(x->f);
    free(x->lens);
}

static Marmot *
create_test_marmot(void)
{
    MarmotStorage *storage = marmot_storage_memory_new();
    assert(storage);
    MarmotConfig config = marmot_config_default();
    Marmot *m = marmot_new_with_config(storage, &config);
    assert(m);
    return m;
}

static void
save_group_at(Marmot *m, const MarmotGroupId *gid, uint64_t epoch)
{
    MarmotGroup *g = marmot_group_new();
    g->mls_group_id = marmot_group_id_new(gid->data, gid->len);
    memset(g->nostr_group_id, 0xAA, 32);
    g->name = strdup("Media Test Group");
    g->description = strdup("For media tests");
    g->state = MARMOT_GROUP_STATE_ACTIVE;
    g->epoch = epoch;
    assert(m->storage->save_group(m->storage->ctx, g) == MARMOT_OK);
    marmot_group_free(g);
}

static void
save_secret(Marmot *m, const MarmotGroupId *gid, uint64_t epoch, uint8_t fill)
{
    uint8_t secret[32];
    memset(secret, fill, sizeof secret);
    assert(m->storage->save_exporter_secret(m->storage->ctx, gid, epoch, secret) == MARMOT_OK);
}

static MarmotMediaReference
parsed_ref(json_t *tag)
{
    Fields f = fields_of(tag);
    MarmotMediaReference ref;
    MarmotError err = marmot_media_imeta_parse(f.f, f.lens, f.n, &ref);
    fields_free(&f);
    assert(err == MARMOT_OK);
    return ref;
}

/* ── MDK v0.11.0 crypto vectors ────────────────────────────────────────── */

static void
test_mdk_unit_file_key(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *u = json_object_get(root, "mdk_unit_vector");
    uint8_t secret[32], hash[32], key[32];
    unhex_fixed(jstr(u, "media_secret"), secret, 32);
    unhex_fixed(jstr(u, "plaintext_sha256"), hash, 32);
    assert(marmot_media_v2_file_key(secret, hash, jstr(u, "media_type"),
                                    jstr(u, "filename"), key) == 0);
    assert_bytes_hex(key, 32, jstr(u, "file_key"));
    json_decref(root);
}

static void
test_mdk_v2_cases_byte_exact(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *cases = json_object_get(root, "cases");
    assert(json_array_size(cases) >= 3);
    size_t i;
    json_t *c;
    json_array_foreach(cases, i, c) {
        uint8_t secret[32], nonce[12];
        unhex_fixed(jstr(c, "media_secret"), secret, 32);
        unhex_fixed(jstr(c, "nonce"), nonce, 12);
        size_t pt_len = 0;
        uint8_t *pt = unhex(jstr(c, "plaintext"), &pt_len);
        const char *mt = jstr(c, "media_type"), *fname = jstr(c, "filename");

        /* The producer canonicalizes the given MIME type. */
        char *canon = NULL;
        assert(marmot_media_type_canonicalize(jstr(c, "media_type_input"), &canon) == MARMOT_OK);
        assert(strcmp(canon, mt) == 0);
        free(canon);

        uint8_t hash[32], key[32];
        SHA256(pt, pt_len, hash);
        assert_bytes_hex(hash, 32, jstr(c, "plaintext_sha256"));
        uint8_t *info = NULL, *aad = NULL;
        size_t info_len = 0, aad_len = 0;
        assert(marmot_media_v2_key_info(hash, mt, fname, &info, &info_len) == 0);
        assert_bytes_hex(info, info_len, jstr(c, "key_info"));
        assert(marmot_media_v2_aad(hash, mt, fname, &aad, &aad_len) == 0);
        assert_bytes_hex(aad, aad_len, jstr(c, "aad"));
        assert(marmot_media_v2_file_key(secret, hash, mt, fname, key) == 0);
        assert_bytes_hex(key, 32, jstr(c, "file_key"));
        free(info);
        free(aad);

        /* Seal with the fixture nonce: MDK's exact ciphertext. */
        uint8_t *ct = NULL;
        size_t ct_len = 0;
        MarmotMediaReference ref;
        assert(marmot_media_v2_seal(secret, nonce, pt, pt_len, mt, fname, &ct, &ct_len,
                                    &ref) == MARMOT_OK);
        assert_bytes_hex(ct, ct_len, jstr(c, "ciphertext"));
        assert_bytes_hex(ref.ciphertext_sha256, 32, jstr(c, "ciphertext_sha256"));

        /* Our imeta is MDK's imeta, field for field and in order. */
        json_t *tag = json_object_get(c, "imeta");
        MarmotMediaReference theirs = parsed_ref(tag);
        assert(theirs.locator_count == 1);
        assert(marmot_media_reference_add_locator(&ref, theirs.locators[0].kind,
                                                  theirs.locators[0].value) == MARMOT_OK);
        assert(marmot_media_reference_set_hints(&ref, theirs.dim, theirs.thumbhash) == MARMOT_OK);
        char **built = NULL;
        size_t built_n = 0;
        assert(marmot_media_imeta_build(&ref, NULL, &built, &built_n) == MARMOT_OK);
        assert(built_n == json_array_size(tag));
        assert(built[built_n] == NULL);
        for (size_t k = 0; k < built_n; k++)
            assert(strcmp(built[k], json_string_value(json_array_get(tag, k))) == 0);
        marmot_media_imeta_fields_free(built, built_n);

        /* And MDK's reference opens to the plaintext. */
        uint8_t *out = NULL;
        size_t out_len = 0;
        assert(marmot_media_v2_open(secret, &theirs, ct, ct_len, &out, &out_len) == MARMOT_OK);
        assert(out_len == pt_len && memcmp(out, pt, pt_len) == 0);
        free(out);
        free(ct);
        free(pt);
        marmot_media_reference_clear(&ref);
        marmot_media_reference_clear(&theirs);
    }
    json_decref(root);
}

static MarmotError
expected_error(const char *verdict)
{
    if (strcmp(verdict, "ciphertext_hash_mismatch") == 0) return MARMOT_ERR_MEDIA_CIPHERTEXT_HASH;
    if (strcmp(verdict, "decrypt_failed") == 0) return MARMOT_ERR_MEDIA_DECRYPT;
    if (strcmp(verdict, "plaintext_hash_mismatch") == 0) return MARMOT_ERR_MEDIA_HASH_MISMATCH;
    if (strcmp(verdict, "reject:UnsupportedFormat") == 0) return MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION;
    assert(strncmp(verdict, "reject:", 7) == 0);
    return MARMOT_ERR_MEDIA_INVALID_REFERENCE;
}

static void
test_mdk_v2_negatives(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *negs = json_object_get(root, "negatives");
    static const char *const required[] = {
        "tampered-ciphertext", "tampered-ciphertext-rehashed", "wrong-epoch-secret",
        "noncanonical-media-type", "filename-mismatch", "media-type-mismatch",
        "ciphertext-hash-mismatch", "plaintext-hash-mismatch", NULL };
    size_t seen = 0, i;
    json_t *c;
    json_array_foreach(negs, i, c) {
        const char *name = jstr(c, "name");
        for (size_t k = 0; required[k]; k++) if (strcmp(required[k], name) == 0) seen++;
        MarmotError want = expected_error(jstr(c, "expect"));
        uint8_t secret[32];
        unhex_fixed(jstr(c, "media_secret"), secret, 32);
        size_t ct_len = 0;
        uint8_t *ct = unhex(jstr(c, "ciphertext"), &ct_len);
        Fields f = fields_of(json_object_get(c, "imeta"));
        MarmotMediaReference ref;
        MarmotError err = marmot_media_imeta_parse(f.f, f.lens, f.n, &ref);
        if (err == MARMOT_OK) {
            uint8_t *out = NULL;
            size_t out_len = 0;
            err = marmot_media_v2_open(secret, &ref, ct, ct_len, &out, &out_len);
            assert(out == NULL);
            marmot_media_reference_clear(&ref);
        }
        if (err != want) fprintf(stderr, "\n%s: got %d want %d\n", name, err, want);
        assert(err == want);
        fields_free(&f);
        free(ct);
    }
    assert(seen == 8);
    json_decref(root);
}

static void
test_mdk_media_type_canonicalization(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    size_t i;
    json_t *c;
    json_array_foreach(json_object_get(root, "media_type_canonicalization"), i, c) {
        json_t *want = json_object_get(c, "canonical");
        char *got = NULL;
        MarmotError err = marmot_media_type_canonicalize(jstr(c, "input"), &got);
        if (json_is_null(want)) {
            assert(err == MARMOT_ERR_INVALID_INPUT && got == NULL);
        } else {
            assert(err == MARMOT_OK && strcmp(got, json_string_value(want)) == 0);
        }
        free(got);
    }
    json_decref(root);
}

/* MDK's shared imeta fixture: the same verdict and exact wire round-trip. */
static void
test_mdk_imeta_fixture(void)
{
    json_t *root = load_fixture("imeta-v2.json");
    size_t i;
    json_t *c;
    json_array_foreach(json_object_get(root, "cases"), i, c) {
        json_t *tag = json_object_get(c, "tag");
        Fields f = fields_of(tag);
        MarmotMediaReference ref;
        MarmotError err = marmot_media_imeta_parse(f.f, f.lens, f.n, &ref);
        if (!json_is_true(json_object_get(c, "valid"))) {
            const char *kind = jstr(c, "rejection_kind");
            MarmotError want = strcmp(kind, "unsupported_format") == 0
                                   ? MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION
                                   : MARMOT_ERR_MEDIA_INVALID_REFERENCE;
            if (err != want) fprintf(stderr, "\n%s: got %d\n", jstr(c, "name"), err);
            assert(err == want);
            fields_free(&f);
            continue;
        }
        assert(err == MARMOT_OK);
        json_t *x = json_object_get(c, "expected");
        assert(strcmp(ref.media_type, jstr(x, "media_type")) == 0);
        assert(strcmp(ref.filename, jstr(x, "file_name")) == 0);
        assert_bytes_hex(ref.ciphertext_sha256, 32, jstr(x, "ciphertext_sha256"));
        assert_bytes_hex(ref.plaintext_sha256, 32, jstr(x, "plaintext_sha256"));
        assert_bytes_hex(ref.nonce, 12, jstr(x, "nonce_hex"));
        json_t *dim = json_object_get(x, "dim"), *th = json_object_get(x, "thumbhash");
        assert(json_is_null(dim) ? ref.dim == NULL : strcmp(ref.dim, json_string_value(dim)) == 0);
        assert(json_is_null(th) ? ref.thumbhash == NULL
                                : strcmp(ref.thumbhash, json_string_value(th)) == 0);
        json_t *locs = json_object_get(x, "locators");
        assert(ref.locator_count == json_array_size(locs));
        for (size_t k = 0; k < ref.locator_count; k++) {
            json_t *l = json_array_get(locs, k);
            assert(strcmp(ref.locators[k].kind, jstr(l, "kind")) == 0);
            assert(strcmp(ref.locators[k].value, jstr(l, "value")) == 0);
        }
        /* Exact wire round-trip, under a policy allowing its kinds. */
        const char *kinds[8] = { 0 };
        assert(ref.locator_count < 8);
        bool all_blossom = true;
        for (size_t k = 0; k < ref.locator_count; k++) {
            kinds[k] = ref.locators[k].kind;
            all_blossom &= strcmp(kinds[k], MARMOT_MEDIA_LOCATOR_BLOSSOM_V1) == 0;
        }
        char **built = NULL;
        size_t n = 0;
        if (!all_blossom)   /* the default policy refuses other kinds */
            assert(marmot_media_imeta_build(&ref, NULL, &built, &n) ==
                   MARMOT_ERR_MEDIA_INVALID_REFERENCE);
        assert(marmot_media_imeta_build(&ref, kinds, &built, &n) == MARMOT_OK);
        assert(n == f.n);
        for (size_t k = 0; k < n; k++) assert(strcmp(built[k], f.f[k]) == 0);
        marmot_media_imeta_fields_free(built, n);
        marmot_media_reference_clear(&ref);
        fields_free(&f);
    }
    json_decref(root);
}

/* ── imeta rules not in the fixtures ───────────────────────────────────── */

static const char *const BASE_TAG[] = {
    "imeta",
    "v encrypted-media-v2",
    "locator blossom-v1 https://blossom.example/abababababababababababababababababababababababababababababababab",
    "ciphertext_sha256 abababababababababababababababababababababababababababababababab",
    "plaintext_sha256 cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
    "nonce efefefefefefefefefefefef",
    "m image/png",
    "filename a.png",
};
#define BASE_N (sizeof BASE_TAG / sizeof BASE_TAG[0])

/* BASE_TAG with field `at` replaced (NULL: dropped) and `extra` appended. */
static MarmotError
parse_variant(size_t at, const char *replacement, const char *extra)
{
    const char *f[BASE_N + 1];
    size_t n = 0;
    for (size_t i = 0; i < BASE_N; i++) {
        if (i == at) {
            if (replacement) f[n++] = replacement;
        } else {
            f[n++] = BASE_TAG[i];
        }
    }
    if (extra) f[n++] = extra;
    MarmotMediaReference ref;
    MarmotError err = marmot_media_imeta_parse(f, NULL, n, &ref);
    if (err == MARMOT_OK) marmot_media_reference_clear(&ref);
    return err;
}

static void
test_imeta_rules(void)
{
    const MarmotError BAD = MARMOT_ERR_MEDIA_INVALID_REFERENCE;
    assert(parse_variant(SIZE_MAX, NULL, NULL) == MARMOT_OK);
    /* Version */
    assert(parse_variant(1, "v encrypted-media-v1", NULL) == MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION);
    assert(parse_variant(1, NULL, NULL) == MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION);
    assert(parse_variant(SIZE_MAX, NULL, "v encrypted-media-v2") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "v") == BAD);
    /* Every single-occurrence field rejects a duplicate. */
    assert(parse_variant(SIZE_MAX, NULL, "m image/png") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "filename a.png") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "nonce efefefefefefefefefefefef") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "dim 1x1") == MARMOT_OK);
    assert(parse_variant(SIZE_MAX, NULL, "blurhash LEHV6nWB2yk8") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "blurhash") == BAD);
    assert(parse_variant(SIZE_MAX, NULL, "unknown field") == MARMOT_OK);
    /* Hashes and nonce */
    assert(parse_variant(3, "ciphertext_sha256 abab", NULL) == BAD);
    assert(parse_variant(4, "plaintext_sha256 zzcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", NULL) == BAD);
    assert(parse_variant(5, "nonce efefefefefefefefefefefef00", NULL) == BAD);
    assert(parse_variant(5, "nonce EFEFEFEFEFEFEFEFEFEFEFEF", NULL) == MARMOT_OK);
    assert(parse_variant(6, NULL, NULL) == BAD);
    assert(parse_variant(6, "m ", NULL) == BAD);
    /* Media type and filename profiles */
    assert(parse_variant(6, "m image/jpg", NULL) == BAD);
    assert(parse_variant(6, "m image/png; q=1", NULL) == BAD);
    assert(parse_variant(7, "filename \xff.png", NULL) == BAD);
    char longname[9 + 257];
    memcpy(longname, "filename ", 9);
    memset(longname + 9, 'a', 256);
    longname[9 + 256] = 0;
    assert(parse_variant(7, longname, NULL) == BAD);
    longname[9 + 255] = 0;
    assert(parse_variant(7, longname, NULL) == MARMOT_OK);
    /* Locators: structure is validity, kind is only fetchability. */
    assert(parse_variant(2, NULL, NULL) == BAD);
    assert(parse_variant(2, "locator blossom-v1", NULL) == BAD);
    assert(parse_variant(2, "locator blossom-v1 ftp://example.com/x", NULL) == BAD);
    assert(parse_variant(2, "locator blossom-v1 not a url", NULL) == BAD);
    assert(parse_variant(2, "locator blossom-v1 https://", NULL) == BAD);
    assert(parse_variant(2, "locator blossom-v1 http://127.0.0.1:3000/x", NULL) == MARMOT_OK);
    assert(parse_variant(2, "locator  https://e.example/x", NULL) == BAD);
    assert(parse_variant(2, "locator ipfs-v1 ipfs://bafy", NULL) == MARMOT_OK);
    assert(parse_variant(SIZE_MAX, NULL, "locator ipfs-v1 ipfs://bafy") == MARMOT_OK);
    /* An embedded NUL never survives as a shorter C string. */
    const char *f[BASE_N];
    size_t lens[BASE_N];
    for (size_t i = 0; i < BASE_N; i++) {
        f[i] = BASE_TAG[i];
        lens[i] = strlen(BASE_TAG[i]);
    }
    static const char nul_name[] = "filename a\0b.png";
    f[7] = nul_name;
    lens[7] = sizeof nul_name - 1;
    MarmotMediaReference ref;
    assert(marmot_media_imeta_parse(f, lens, BASE_N, &ref) == BAD);
}

static void
test_imeta_build_requires_blossom_locator(void)
{
    const char *f[BASE_N + 1];
    for (size_t i = 0; i < BASE_N; i++) f[i] = BASE_TAG[i];
    f[BASE_N] = "locator ipfs-v1 ipfs://bafy";
    MarmotMediaReference ref;
    assert(marmot_media_imeta_parse(f, NULL, BASE_N + 1, &ref) == MARMOT_OK);
    char **built = NULL;
    size_t n = 0;
    assert(marmot_media_imeta_build(&ref, NULL, &built, &n) == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    assert(built == NULL && n == 0);
    static const char *const only_blossom[] = { MARMOT_MEDIA_LOCATOR_BLOSSOM_V1, NULL };
    assert(marmot_media_imeta_build(&ref, only_blossom, &built, &n) ==
           MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    static const char *const both[] = { MARMOT_MEDIA_LOCATOR_BLOSSOM_V1, "ipfs-v1", NULL };
    assert(marmot_media_imeta_build(&ref, both, &built, &n) == MARMOT_OK);
    assert(n == BASE_N + 1);
    marmot_media_imeta_fields_free(built, n);
    marmot_media_reference_clear(&ref);

    /* No locator yet: nothing to emit. */
    MarmotMediaReference empty = { 0 };
    assert(marmot_media_imeta_build(&empty, NULL, &built, &n) == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
}

static void
test_blossom_fallback_url(void)
{
    uint8_t h[32];
    memset(h, 0xab, sizeof h);
    char *url = NULL;
    assert(marmot_media_blossom_fallback_url("https://cdn.example//", h, &url) == MARMOT_OK);
    assert(strcmp(url, "https://cdn.example/"
                       "abababababababababababababababababababababababababababababababab") == 0);
    free(url);
    assert(marmot_media_blossom_fallback_url("///", h, &url) == MARMOT_ERR_INVALID_INPUT);
}

/* ── Group-bound encrypt / decrypt ─────────────────────────────────────── */

static const uint8_t GID_BYTES[32] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void
test_group_roundtrip_and_retained_epoch(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new(GID_BYTES, 32);
    save_group_at(m, &gid, 5);
    save_secret(m, &gid, 5, 0x55);

    static const uint8_t file[] = "an attachment for epoch five";
    MarmotMediaUpload up;
    assert(marmot_media_encrypt(m, &gid, file, sizeof file - 1, "Image/JPG; x=y", "pic.jpg",
                                &up) == MARMOT_OK);
    assert(up.source_epoch == 5);
    assert(strcmp(up.reference.media_type, "image/jpeg") == 0);
    assert(strcmp(up.reference.filename, "pic.jpg") == 0);
    assert(up.reference.locator_count == 0);
    uint8_t h[32];
    SHA256(up.ciphertext, up.ciphertext_len, h);
    assert(memcmp(h, up.reference.ciphertext_sha256, 32) == 0);
    SHA256(file, sizeof file - 1, h);
    assert(memcmp(h, up.reference.plaintext_sha256, 32) == 0);
    assert(marmot_media_reference_add_locator(&up.reference, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1,
                                              "https://blossom.example/x") == MARMOT_OK);

    /* The key is MLS-Exporter("marmot", "encrypted-media") of epoch 5 --
     * not the group-event exporter, not the raw exporter secret. */
    uint8_t exporter[32], media_secret[32], event_secret[32];
    memset(exporter, 0x55, sizeof exporter);
    assert(mls_exporter(exporter, "marmot", (const uint8_t *)"encrypted-media", 15,
                        media_secret, 32) == 0);
    assert(mls_exporter(exporter, "marmot", (const uint8_t *)"group-event", 11,
                        event_secret, 32) == 0);
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    assert(marmot_media_v2_open(media_secret, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_OK);
    free(pt);
    assert(marmot_media_v2_open(event_secret, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_ERR_MEDIA_DECRYPT);
    assert(marmot_media_v2_open(exporter, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_ERR_MEDIA_DECRYPT);

    assert(marmot_media_decrypt(m, &gid, 5, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_OK);
    assert(pt_len == sizeof file - 1 && memcmp(pt, file, pt_len) == 0);
    free(pt);

    /* The group moves on: epoch 5 is retained, so its media still opens. */
    save_secret(m, &gid, 6, 0x66);
    save_group_at(m, &gid, 6);
    assert(marmot_media_decrypt(m, &gid, 5, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_OK);
    free(pt);
    /* The wrong epoch's secret fails the AEAD; a gone epoch is not found. */
    assert(marmot_media_decrypt(m, &gid, 6, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_ERR_MEDIA_DECRYPT);
    assert(pt == NULL && pt_len == 0);
    assert(marmot_media_decrypt(m, &gid, 4, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_ERR_STORAGE_NOT_FOUND);
    /* Ciphertext hash is checked before the AEAD. */
    up.ciphertext[0] ^= 1;
    assert(marmot_media_decrypt(m, &gid, 5, &up.reference, up.ciphertext, up.ciphertext_len,
                                &pt, &pt_len) == MARMOT_ERR_MEDIA_CIPHERTEXT_HASH);
    up.ciphertext[0] ^= 1;

    /* New media is encrypted for the new epoch. */
    MarmotMediaUpload up6;
    assert(marmot_media_encrypt(m, &gid, file, sizeof file - 1, "image/jpeg", "pic.jpg",
                                &up6) == MARMOT_OK);
    assert(up6.source_epoch == 6);
    marmot_media_upload_clear(&up6);

    marmot_media_upload_clear(&up);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_group_encrypt_fresh_nonce_and_input_rules(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new(GID_BYTES, 32);
    save_group_at(m, &gid, 2);
    save_secret(m, &gid, 2, 0x22);

    static const uint8_t file[] = "same bytes";
    MarmotMediaUpload a, b;
    assert(marmot_media_encrypt(m, &gid, file, 10, "text/plain", "f.txt", &a) == MARMOT_OK);
    assert(marmot_media_encrypt(m, &gid, file, 10, "text/plain", "f.txt", &b) == MARMOT_OK);
    assert(memcmp(a.reference.nonce, b.reference.nonce, 12) != 0);
    assert(memcmp(a.reference.ciphertext_sha256, b.reference.ciphertext_sha256, 32) != 0);
    marmot_media_upload_clear(&a);
    marmot_media_upload_clear(&b);

    MarmotMediaUpload x;
    assert(marmot_media_encrypt(m, &gid, file, 0, "text/plain", "f.txt", &x) ==
           MARMOT_ERR_INVALID_INPUT);
    assert(marmot_media_encrypt(m, &gid, file, 10, "text", "f.txt", &x) ==
           MARMOT_ERR_INVALID_INPUT);
    assert(marmot_media_encrypt(m, &gid, file, 10, "text/plain", "", &x) ==
           MARMOT_ERR_INVALID_INPUT);
    assert(x.ciphertext == NULL);
    MarmotGroupId other = marmot_group_id_new((const uint8_t *)"nope", 4);
    assert(marmot_media_encrypt(m, &other, file, 10, "text/plain", "f.txt", &x) != MARMOT_OK);
    marmot_group_id_free(&other);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void
test_legacy_format_is_never_produced(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new(GID_BYTES, 32);
    save_group_at(m, &gid, 1);
    save_secret(m, &gid, 1, 0x11);
    MarmotEncryptedMedia old;
    memset(&old, 0x5a, sizeof old);
    static const uint8_t file[] = "x";
    assert(marmot_encrypt_media(m, &gid, file, 1, "image/png", "x.png", &old) ==
           MARMOT_ERR_MEDIA_LEGACY_FORMAT);
    assert(old.encrypted_data == NULL && old.encrypted_len == 0 && old.imeta.mime_type == NULL);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

/* Review N2: the exporter step, pinned.  The generator computed it with
 * openmls's crypto provider (self-checked on the RFC 9420 key-schedule
 * vectors); the first value is also the reviewer's independent one. */
static void
test_exporter_step_vectors(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *steps = json_object_get(root, "exporter_step");
    assert(json_array_size(steps) >= 2);
    size_t i;
    json_t *c;
    json_array_foreach(steps, i, c) {
        assert(strcmp(jstr(c, "label"), "marmot") == 0);
        assert(strcmp(jstr(c, "context"), "encrypted-media") == 0);
        uint8_t exporter[32], media[32], event[32];
        unhex_fixed(jstr(c, "exporter_secret"), exporter, 32);
        assert(marmot_media_secret_from_exporter(exporter, media) == 0);
        assert_bytes_hex(media, 32, jstr(c, "media_secret"));
        assert(mls_exporter(exporter, "marmot", (const uint8_t *)"group-event", 11, event, 32) == 0);
        assert_bytes_hex(event, 32, jstr(c, "group_event_secret"));
    }
    assert(strcmp(jstr(json_array_get(steps, 0), "media_secret"),
                  "73bd647f044b114b9b3dc182b780653dc65c1b3fbaa3eda7a30f1db93e506635") == 0);

    /* End to end through the public API: MDK sealed "from-exporter-epoch-9"
     * under the media secret of exporter_step[1]; store that exporter secret
     * at epoch 9 and decrypt. */
    json_t *step = json_array_get(steps, 1), *the_case = NULL;
    json_array_foreach(json_object_get(root, "cases"), i, c)
        if (strcmp(jstr(c, "name"), "from-exporter-epoch-9") == 0) the_case = c;
    assert(the_case);
    assert(strcmp(jstr(the_case, "media_secret"), jstr(step, "media_secret")) == 0);
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new((const uint8_t *)"exporter-e2e", 12);
    save_group_at(m, &gid, 10);
    uint8_t exporter[32];
    unhex_fixed(jstr(step, "exporter_secret"), exporter, 32);
    assert(m->storage->save_exporter_secret(m->storage->ctx, &gid, 9, exporter) == MARMOT_OK);
    MarmotMediaReference ref = parsed_ref(json_object_get(the_case, "imeta"));
    size_t ct_len = 0, pt_len = 0;
    uint8_t *ct = unhex(jstr(the_case, "ciphertext"), &ct_len);
    uint8_t *want = unhex(jstr(the_case, "plaintext"), &pt_len);
    uint8_t *pt = NULL;
    size_t got_len = 0;
    assert(marmot_media_decrypt(m, &gid, 9, &ref, ct, ct_len, &pt, &got_len) == MARMOT_OK);
    assert(got_len == pt_len && memcmp(pt, want, pt_len) == 0);
    free(pt);
    free(ct);
    free(want);
    marmot_media_reference_clear(&ref);
    marmot_group_id_free(&gid);
    marmot_free(m);
    json_decref(root);
}

/* Review L3: the epoch check before a send. */
static void
test_check_epoch(void)
{
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new(GID_BYTES, 32);
    save_group_at(m, &gid, 7);
    assert(marmot_media_check_epoch(m, &gid, 7) == MARMOT_OK);
    assert(marmot_media_check_epoch(m, &gid, 6) == MARMOT_ERR_MEDIA_EPOCH_CHANGED);
    MarmotGroupId other = marmot_group_id_new((const uint8_t *)"none", 4);
    assert(marmot_media_check_epoch(m, &other, 7) == MARMOT_ERR_GROUP_NOT_FOUND);
    marmot_group_id_free(&other);
    marmot_group_id_free(&gid);
    marmot_free(m);
}

/* Review L5: the retained pre-0.12 reader, on a fixture built by an
 * independent implementation (legacy-pre-0.12.json). */
static void
test_legacy_reader(void)
{
    json_t *f = load_fixture("legacy-pre-0.12.json");
    Marmot *m = create_test_marmot();
    MarmotGroupId gid = marmot_group_id_new(GID_BYTES, 32);
    uint64_t epoch = (uint64_t)json_integer_value(json_object_get(f, "epoch"));
    save_group_at(m, &gid, epoch);
    uint8_t exporter[32];
    unhex_fixed(jstr(f, "exporter_secret"), exporter, 32);
    assert(m->storage->save_exporter_secret(m->storage->ctx, &gid, epoch, exporter) == MARMOT_OK);
    size_t ct_len = 0, want_len = 0;
    uint8_t *ct = unhex(jstr(f, "ciphertext"), &ct_len);
    uint8_t *want = unhex(jstr(f, "plaintext"), &want_len);
    MarmotImetaInfo imeta = { .mime_type = (char *)jstr(f, "mime_type"), .epoch = epoch };
    unhex_fixed(jstr(f, "nonce"), imeta.nonce, 12);
    unhex_fixed(jstr(f, "file_hash"), imeta.file_hash, 32);
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &imeta, &pt, &pt_len) == MARMOT_OK);
    assert(pt_len == want_len && memcmp(pt, want, pt_len) == 0);
    free(pt);

    /* The hash is not optional. */
    MarmotImetaInfo no_hash = imeta;
    memset(no_hash.file_hash, 0, 32);
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &no_hash, &pt, &pt_len) ==
           MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    MarmotImetaInfo wrong_hash = imeta;
    wrong_hash.file_hash[0] ^= 1;
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &wrong_hash, &pt, &pt_len) ==
           MARMOT_ERR_MEDIA_HASH_MISMATCH);
    assert(pt == NULL);
    /* The MIME type is authenticated; the ciphertext too. */
    MarmotImetaInfo wrong_mime = imeta;
    wrong_mime.mime_type = (char *)"image/jpeg";
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &wrong_mime, &pt, &pt_len) ==
           MARMOT_ERR_MEDIA_DECRYPT);
    ct[0] ^= 1;
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &imeta, &pt, &pt_len) ==
           MARMOT_ERR_MEDIA_DECRYPT);
    ct[0] ^= 1;
    /* Another epoch's key is not kept: not found. */
    MarmotImetaInfo other = imeta;
    other.epoch = epoch + 1;
    assert(marmot_decrypt_media(m, &gid, ct, ct_len, &other, &pt, &pt_len) ==
           MARMOT_ERR_STORAGE_NOT_FOUND);
    assert(marmot_decrypt_media(m, &gid, ct, 15, &imeta, &pt, &pt_len) == MARMOT_ERR_INVALID_INPUT);
    free(ct);
    free(want);
    marmot_group_id_free(&gid);
    marmot_free(m);
    json_decref(f);
}

/* ── 0x8002 group Blossom image ────────────────────────────────────────── */

static void
test_group_image_vectors(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *gi = json_object_get(root, "group_image");
    uint8_t key[32], nonce[12];
    unhex_fixed(jstr(gi, "image_key"), key, 32);
    unhex_fixed(jstr(gi, "image_nonce"), nonce, 12);
    size_t pt_len = 0;
    uint8_t *pt = unhex(jstr(gi, "plaintext"), &pt_len);
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    assert(marmot_group_image_seal(key, nonce, pt, pt_len, "image/png", &ct, &ct_len) == MARMOT_OK);
    assert_bytes_hex(ct, ct_len, jstr(gi, "ciphertext"));

    MarmotGroupBlossomImage img = { .present = true, .media_type = strdup("image/png") };
    SHA256(ct, ct_len, img.image_hash);
    assert_bytes_hex(img.image_hash, 32, jstr(gi, "image_hash"));
    memcpy(img.image_key, key, 32);
    memcpy(img.image_nonce, nonce, 12);
    unhex_fixed(jstr(gi, "image_upload_key"), img.image_upload_key, 32);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    assert(marmot_group_blossom_image_encode(&img, &enc, &enc_len) == MARMOT_OK);
    assert_bytes_hex(enc, enc_len, jstr(gi, "component_bytes"));
    free(enc);

    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(marmot_group_image_decrypt(&img, ct, ct_len, &out, &out_len) == MARMOT_OK);
    assert(out_len == pt_len && memcmp(out, pt, pt_len) == 0);
    free(out);
    ct[0] ^= 1;
    assert(marmot_group_image_decrypt(&img, ct, ct_len, &out, &out_len) ==
           MARMOT_ERR_MEDIA_CIPHERTEXT_HASH);

    size_t i;
    json_t *c;
    json_array_foreach(json_object_get(gi, "decode"), i, c) {
        size_t len = 0;
        uint8_t *bytes = unhex(jstr(c, "bytes"), &len);
        MarmotGroupBlossomImage d;
        MarmotError err = marmot_group_blossom_image_decode(bytes, len, &d);
        if (json_is_true(json_object_get(c, "valid"))) {
            assert(err == MARMOT_OK);
            uint8_t *re = NULL;
            size_t re_len = 0;
            assert(marmot_group_blossom_image_encode(&d, &re, &re_len) == MARMOT_OK);
            assert(re_len == len && memcmp(re, bytes, len) == 0);
            free(re);
        } else {
            if (err != MARMOT_ERR_MEDIA_INVALID_REFERENCE)
                fprintf(stderr, "\n%s: %d\n", jstr(c, "name"), err);
            assert(err == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
            assert(!d.present && d.media_type == NULL);
        }
        marmot_group_blossom_image_clear(&d);
        free(bytes);
    }
    marmot_group_blossom_image_clear(&img);
    free(ct);
    free(pt);
    json_decref(root);
}

static void
test_group_image_encrypt_fresh(void)
{
    static const uint8_t png[] = "\x89PNG\r\n\x1a\nfresh";
    MarmotGroupBlossomImage a, b;
    uint8_t *ca = NULL, *cb = NULL;
    size_t la = 0, lb = 0;
    assert(marmot_group_image_encrypt(png, sizeof png - 1, " IMAGE/PNG ", &a, &ca, &la) == MARMOT_OK);
    assert(marmot_group_image_encrypt(png, sizeof png - 1, "image/png", &b, &cb, &lb) == MARMOT_OK);
    assert(a.present && strcmp(a.media_type, "image/png") == 0);
    assert(memcmp(a.image_key, b.image_key, 32) != 0);
    assert(memcmp(a.image_nonce, b.image_nonce, 12) != 0);
    assert(memcmp(a.image_upload_key, b.image_upload_key, 32) != 0);
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    assert(marmot_group_image_decrypt(&a, ca, la, &pt, &pt_len) == MARMOT_OK);
    assert(pt_len == sizeof png - 1 && memcmp(pt, png, pt_len) == 0);
    free(pt);
    /* The AAD binds the media type. */
    free(a.media_type);
    a.media_type = strdup("image/jpeg");
    assert(marmot_group_image_decrypt(&a, ca, la, &pt, &pt_len) == MARMOT_ERR_MEDIA_DECRYPT);
    marmot_group_blossom_image_clear(&a);
    marmot_group_blossom_image_clear(&b);
    free(ca);
    free(cb);
    assert(marmot_group_image_encrypt(png, sizeof png - 1, "png", &a, &ca, &la) ==
           MARMOT_ERR_INVALID_INPUT);
}

/* ── 0x8007 group avatar URL ───────────────────────────────────────────── */

/* Decode one stored URL (empty hints); returns the error, *unverified set. */
static MarmotError
decode_stored_url(const char *url, bool *unverified)
{
    size_t n = strlen(url);
    assert(n < 2048);
    uint8_t buf[2 + 2048 + 2];
    size_t at = 0;
    if (n < 64) {
        buf[at++] = (uint8_t)n;
    } else {
        buf[at++] = (uint8_t)(0x40 | (n >> 8));
        buf[at++] = (uint8_t)n;
    }
    memcpy(buf + at, url, n);
    at += n;
    buf[at++] = 0;
    buf[at++] = 0;
    MarmotGroupAvatarUrl d;
    MarmotError err = marmot_group_avatar_url_decode(buf, at, &d);
    if (unverified) *unverified = err == MARMOT_OK && d.url_unverified;
    if (err == MARMOT_OK) assert(d.url && strcmp(d.url, url) == 0);   /* never rewritten */
    marmot_group_avatar_url_clear(&d);
    return err;
}

static void
test_avatar_url_vectors(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *av = json_object_get(root, "avatar_url");
    size_t i, matched = 0;
    json_t *c;
    size_t gaps = 0;
    json_array_foreach(json_object_get(av, "normalize"), i, c) {
        const char *in = jstr(c, "input");
        json_t *want = json_object_get(c, "normalized");
        char *got = NULL;
        MarmotError err = marmot_group_avatar_url_normalize(in, &got);
        if (err == MARMOT_OK) {
            /* What libmarmot produces is exactly what MDK produces. */
            if (json_is_null(want) || strcmp(got, json_string_value(want)) != 0)
                fprintf(stderr, "\n%s -> %s (MDK %s)\n", in, got,
                        json_is_null(want) ? "refuses" : json_string_value(want));
            assert(!json_is_null(want) && strcmp(got, json_string_value(want)) == 0);
            char *again = NULL;   /* a fixed point */
            assert(marmot_group_avatar_url_normalize(got, &again) == MARMOT_OK);
            assert(strcmp(again, got) == 0);
            free(again);
            matched++;
        } else if (!json_is_null(want)) {
            /* Outside the subset: never produced, but MDK's canonical bytes
             * are accepted when decoded (usually unverified), never refused. */
            assert(decode_stored_url(json_string_value(want), NULL) == MARMOT_OK);
            gaps++;
        } else {
            assert(err == MARMOT_ERR_INVALID_INPUT);
            matched++;
        }
        free(got);
    }
    assert(gaps >= 10);
    assert(matched >= 15);

    json_array_foreach(json_object_get(av, "decode"), i, c) {
        size_t len = 0;
        uint8_t *bytes = unhex(jstr(c, "bytes"), &len);
        MarmotGroupAvatarUrl d;
        MarmotError err = marmot_group_avatar_url_decode(bytes, len, &d);
        if (json_is_true(json_object_get(c, "valid"))) {
            assert(err == MARMOT_OK);
            uint8_t *re = NULL;
            size_t re_len = 0;
            assert(marmot_group_avatar_url_encode(&d, &re, &re_len) == MARMOT_OK);
            assert(re_len == len && memcmp(re, bytes, len) == 0);
            free(re);
        } else {
            assert(err == MARMOT_ERR_MEDIA_INVALID_REFERENCE && d.url == NULL);
        }
        marmot_group_avatar_url_clear(&d);
        free(bytes);
    }
    json_decref(root);
}

/* Review M3: validity is consensus, rendering is local.  Against MDK's own
 * decoder verdicts: MDK-valid state is never refused; libmarmot calls a
 * value invalid only when MDK does too, and valid only when MDK does too. */
static void
test_avatar_url_stored_consensus(void)
{
    json_t *root = load_fixture("media-v2-mdk-v0.11.0.json");
    json_t *stored = json_object_get(json_object_get(root, "avatar_url"), "stored");
    size_t i, valid = 0, invalid = 0, unverified_n = 0;
    json_t *c;
    json_array_foreach(stored, i, c) {
        size_t len = 0;
        uint8_t *bytes = unhex(jstr(c, "bytes"), &len);
        bool mdk_valid = json_is_true(json_object_get(c, "mdk_valid"));
        MarmotGroupAvatarUrl d;
        MarmotError err = marmot_group_avatar_url_decode(bytes, len, &d);
        if (err == MARMOT_OK && !d.url_unverified) {
            assert(mdk_valid);
            valid++;
        } else if (err == MARMOT_OK) {
            unverified_n++;
        } else {
            if (mdk_valid) fprintf(stderr, "\nrefuses MDK-valid %s\n", jstr(c, "url"));
            assert(!mdk_valid && err == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
            invalid++;
        }
        marmot_group_avatar_url_clear(&d);
        free(bytes);
    }
    assert(valid >= 2 && invalid >= 15 && unverified_n >= 8);
    json_decref(root);

    /* The review's divergence cases: MDK (url 2.5.8) canonical, outside
     * libmarmot's subset -- accepted, kept byte for byte, never contacted,
     * never produced. */
    static const char *const divergent[] = {
        "https://xn--bcher-kva.example/a.png", "https://xn--r8jz45g.jp/avatar.png",
        "https://my_host.example.com/a.png", "https://example.com/avatar.png?size[]=512",
        "https://example.com/avatar.png?w=512|h=512", "https://example.com/avatar.png?sig=a^b",
        "https://example.com/avatar.png?q=`x`", "https://example.com/a^b.png",
        "https://example.com/a|b.png", "https://example.com/[x].png", NULL };
    for (size_t k = 0; divergent[k]; k++) {
        bool unverified = false;
        assert(decode_stored_url(divergent[k], &unverified) == MARMOT_OK);
        if (!unverified) fprintf(stderr, "\n%s verified\n", divergent[k]);
        assert(unverified);
        MarmotGroupAvatarUrl u = { .url = (char *)divergent[k], .url_unverified = true };
        MarmotGroupBlossomImage img = { .present = true };
        assert(marmot_group_avatar_select(&u, &img) == MARMOT_GROUP_AVATAR_URL_PLACEHOLDER);
        uint8_t *enc = NULL;
        size_t enc_len = 0;
        assert(marmot_group_avatar_url_encode(&u, &enc, &enc_len) ==
               MARMOT_ERR_MEDIA_INVALID_REFERENCE);
        u.url_unverified = false;   /* not normalizable either */
        assert(marmot_group_avatar_url_encode(&u, &enc, &enc_len) ==
               MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    }
    /* Provably non-canonical under every WHATWG version: refused. */
    static const char *const never[] = {
        "https://example.com", "https://example.com?q", "https://Example.com/", "https://e.example:443/",
        "https://e.example:0080/", "https://e.example:/", "https://e.example/a/../b",
        "https://e.example/%2E/b", "https://e.example/a b", "https://e.example/a`b",
        "https://e.example/{x}", "https://e.example/a\\b", "https://e.example/?a b",
        "https://e.example/?a'b", "https://e.example/#f", "https://u@e.example/",
        "https://e%41.example/", "http://e.example/", "https://e.example/\x7f",
        "https://1.2.3/", "https://[2001:DB8::1]/", "https://a<b.example/", "https://a^b.example/",
        "https://a]b.example/",
        /* Outside the subset too (so only the explicit rule decides); MDK's
         * normalizer rewrites each, so none is MDK-valid stored state. */
        "https://My_Host.example/", "https://my_host.example:443/", "https://my_host.example:/",
        "https://my_host.example:0080/", "https://e.example/a^b`c", "https://e.example/a|{x}",
        "https://e.example/?a|b'c", "https://e.example/a^b\"c", "https://e.example/a^b<c", NULL };
    for (size_t k = 0; never[k]; k++) {
        if (decode_stored_url(never[k], NULL) != MARMOT_ERR_MEDIA_INVALID_REFERENCE)
            fprintf(stderr, "\naccepted %s\n", never[k]);
        assert(decode_stored_url(never[k], NULL) == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    }
    /* MDK's canonical forms of those: accepted (unverified). */
    static const char *const mdk_forms[] = {
        "https://my_host.example/", "https://my_host.example:80/", "https://e.example/a^b%60c",
        "https://e.example/a|%7Bx%7D", "https://e.example/?a|b%27c", "https://e.example/a^b%22c",
        "https://e.example/a^b%3Cc", NULL };
    for (size_t k = 0; mdk_forms[k]; k++) {
        bool unverified = false;
        assert(decode_stored_url(mdk_forms[k], &unverified) == MARMOT_OK);
        assert(unverified);
    }
    /* Inside the subset and canonical: valid. */
    static const char *const fine[] = {
        "https://e.example/", "https://e.example:8443/a.png?s=1", "https://1.2.0.3/",
        "https://[2001:db8::1]/a", "https://e.example/%7Euser/a%20b.png", "https://e.example./x", NULL };
    for (size_t k = 0; fine[k]; k++) {
        bool unverified = true;
        assert(decode_stored_url(fine[k], &unverified) == MARMOT_OK);
        assert(!unverified);
    }
}

static void
test_avatar_url_rules_and_precedence(void)
{
    static const char *const ok[][2] = {
        { "https://e.example/a/./b/../c.png", "https://e.example/a/c.png" },
        { "https://e.example/a/.", "https://e.example/a/" },
        { "https://e.example/..", "https://e.example/" },
        { "https:e.example\\a\\b.png", "https://e.example/a/b.png" },
        { "  https://E.example:443\t/x  ", "https://e.example/x" },
        { "https://e.example:80/", "https://e.example:80/" },
        { "https://e.example:/", "https://e.example/" },
        { "https://[::]/", "https://[::]/" },
        { "https://[2001:db8:0:0:1:0:0:1]/", "https://[2001:db8::1:0:0:1]/" },
        { "https://[::ffff:1.2.3.4]/", "https://[::ffff:102:304]/" },
        { "https://1.2.3/", "https://1.2.0.3/" },
        { "https://e.example/q?a='b'", "https://e.example/q?a=%27b%27" },
        { "https://e.example/%41", "https://e.example/%41" },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        char *got = NULL;
        assert(marmot_group_avatar_url_normalize(ok[i][0], &got) == MARMOT_OK);
        if (strcmp(got, ok[i][1]) != 0) fprintf(stderr, "\n%s -> %s\n", ok[i][0], got);
        assert(strcmp(got, ok[i][1]) == 0);
        free(got);
    }
    static const char *const bad[] = {
        "", "https://", "https://a..b/", "https://e.example:65536/",
        "https://e.example:x/", "https://256.1.1.1/", "https://1.2.3.4.5/", "https://09/",
        "https://e_x.example/", "https://e.example/a^b", "https://e.example/a|b",
        "https://[fe80::1%25en0]/", "https://e.example/?a#b", "wss://e.example/",
        "https://h.example/a/./b:/.%2e/c", "https://h.example/a/B:/../c",
        "https://e.example/\xff",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *got = NULL;
        if (marmot_group_avatar_url_normalize(bad[i], &got) != MARMOT_ERR_INVALID_INPUT)
            fprintf(stderr, "\n%s accepted as %s\n", bad[i], got);
        assert(got == NULL);
    }
    char long_url[2100];
    memset(long_url, 'a', sizeof long_url - 1);
    memcpy(long_url, "https://e.example/", 18);
    long_url[sizeof long_url - 1] = 0;
    char *got = NULL;
    assert(marmot_group_avatar_url_normalize(long_url, &got) == MARMOT_ERR_INVALID_INPUT);

    /* Encode normalizes; hints without a URL are invalid. */
    MarmotGroupAvatarUrl in = { .url = (char *)"HTTPS://E.example", .dim = (uint8_t *)"1x1",
                                .dim_len = 3 };
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    assert(marmot_group_avatar_url_encode(&in, &enc, &enc_len) == MARMOT_OK);
    MarmotGroupAvatarUrl d;
    assert(marmot_group_avatar_url_decode(enc, enc_len, &d) == MARMOT_OK);
    assert(strcmp(d.url, "https://e.example/") == 0 && d.dim_len == 3);
    free(enc);
    MarmotGroupAvatarUrl hints_only = { .dim = (uint8_t *)"1x1", .dim_len = 3 };
    assert(marmot_group_avatar_url_encode(&hints_only, &enc, &enc_len) ==
           MARMOT_ERR_MEDIA_INVALID_REFERENCE);

    /* Precedence: the URL avatar wins; clearing it falls back. */
    MarmotGroupBlossomImage img = { .present = true };
    MarmotGroupAvatarUrl none = { 0 };
    assert(marmot_group_avatar_select(&d, &img) == MARMOT_GROUP_AVATAR_URL);
    assert(marmot_group_avatar_select(&d, NULL) == MARMOT_GROUP_AVATAR_URL);
    assert(marmot_group_avatar_select(&none, &img) == MARMOT_GROUP_AVATAR_BLOSSOM);
    assert(marmot_group_avatar_select(NULL, NULL) == MARMOT_GROUP_AVATAR_NONE);
    img.present = false;
    assert(marmot_group_avatar_select(&none, &img) == MARMOT_GROUP_AVATAR_NONE);
    marmot_group_avatar_url_clear(&d);
}

/* ── 0x8006 / 0x800b policies against MDK's own verdicts (nostrc-qp24.5.2,
 *    nostrc-m6tp; policy-verdicts-mdk-v0.11.0.json) ─────────────────────── */

static bool
any_unverified(const MarmotGroupMediaPolicy *p)
{
    for (size_t i = 0; i < p->default_blob_endpoint_count; i++)
        if (p->default_blob_endpoints[i].base_url_unverified) return true;
    return false;
}

/* 0x8006: libmarmot's verdict is MDK's, case for case.  0x800b: the
 * consensus properties of the three-way URL judgement -- never refuse what
 * MDK accepts, call a state valid (verified) only when MDK does, and
 * re-encode every verified state to the same bytes. */
static void
test_policy_verdicts_match_mdk(void)
{
    json_t *root = load_fixture("policy-verdicts-mdk-v0.11.0.json");
    json_t *cases = json_object_get(root, "cases");
    size_t i, agent_n = 0, valid = 0, invalid = 0, unverified = 0;
    json_t *c;
    json_array_foreach(cases, i, c) {
        size_t len = 0;
        const char *hex = jstr(c, "bytes");
        uint8_t *bytes = *hex ? unhex(hex, &len) : NULL;
        bool mdk_ok = json_is_true(json_object_get(c, "mdk_ok"));
        if (strcmp(jstr(c, "id"), "8006") == 0) {
            MarmotAgentTextStreamPolicy p;
            MarmotError err = marmot_agent_text_stream_policy_decode(bytes, len, &p);
            if ((err == MARMOT_OK) != mdk_ok)
                fprintf(stderr, "\n0x8006 %s: libmarmot %d, MDK %d\n", jstr(c, "note"), err, mdk_ok);
            assert((err == MARMOT_OK) == mdk_ok);
            assert(err == MARMOT_OK || err == MARMOT_ERR_EXTENSION_FORMAT);
            if (err == MARMOT_OK) {
                uint8_t again[MARMOT_AGENT_STREAM_STATE_LEN];
                assert(marmot_agent_text_stream_policy_encode(&p, again) == MARMOT_OK);
                assert(len == sizeof again && memcmp(again, bytes, len) == 0);
            }
            agent_n++;
        } else {
            MarmotGroupMediaPolicy p;
            MarmotError err = marmot_group_media_policy_decode(bytes, len, &p);
            if (err == MARMOT_OK && !any_unverified(&p)) {
                if (!mdk_ok) fprintf(stderr, "\n0x800b %s: verified, MDK refuses\n", jstr(c, "note"));
                assert(mdk_ok);
                uint8_t *enc = NULL;
                size_t enc_len = 0;
                assert(marmot_group_media_policy_encode(&p, &enc, &enc_len) == MARMOT_OK);
                assert(enc_len == len && memcmp(enc, bytes, len) == 0);
                free(enc);
                valid++;
            } else if (err == MARMOT_OK) {
                unverified++;
            } else {
                if (mdk_ok) fprintf(stderr, "\n0x800b %s: refuses an MDK-valid state\n", jstr(c, "note"));
                assert(!mdk_ok && err == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
                invalid++;
            }
            marmot_group_media_policy_clear(&p);
        }
        free(bytes);
    }
    assert(agent_n >= 200 && valid >= 20 && invalid >= 40 && unverified >= 5);

    /* What libmarmot produces from a raw endpoint URL is MDK's
     * normalization, or nothing (a URL outside its verifiable subset). */
    json_t *norm = json_object_get(root, "normalize");
    size_t produced = 0;
    json_array_foreach(norm, i, c) {
        MarmotMediaBlobEndpoint ep = { .locator_kind = "blossom-v1", .base_url = (char *)jstr(c, "in") };
        char *kinds[] = { "blossom-v1", NULL };
        MarmotGroupMediaPolicy in = { kinds, 1, &ep, 1 };
        uint8_t *enc = NULL;
        size_t enc_len = 0;
        if (marmot_group_media_policy_encode(&in, &enc, &enc_len) != MARMOT_OK) continue;
        assert(json_is_true(json_object_get(c, "ok")));
        MarmotGroupMediaPolicy out;
        assert(marmot_group_media_policy_decode(enc, enc_len, &out) == MARMOT_OK);
        assert(!any_unverified(&out));
        if (strcmp(out.default_blob_endpoints[0].base_url, jstr(c, "normalized")) != 0)
            fprintf(stderr, "\n%s -> %s, MDK %s\n", jstr(c, "in"),
                    out.default_blob_endpoints[0].base_url, jstr(c, "normalized"));
        assert(strcmp(out.default_blob_endpoints[0].base_url, jstr(c, "normalized")) == 0);
        marmot_group_media_policy_clear(&out);
        free(enc);
        produced++;
    }
    assert(produced >= 15);
    json_decref(root);
}

static void
test_policy_codecs(void)
{
    /* Every White Noise group's 0x8006 state. */
    MarmotAgentTextStreamPolicy d = marmot_agent_text_stream_policy_user_to_agent_default();
    uint8_t state[MARMOT_AGENT_STREAM_STATE_LEN];
    assert(marmot_agent_text_stream_policy_encode(&d, state) == MARMOT_OK);
    assert_bytes_hex(state, sizeof state, "010300001000000000000000");

    /* MDK's blossom_default(["https://blossom.example.com/"]) (the White
     * Noise fixture's 0x800b), decoded and encoded again. */
    static const char *wn =
        "12656e637279707465642d6d656469612d76320b0a626c6f73736f6d2d7631280a626c6f73736f6d2d76"
        "311c68747470733a2f2f626c6f73736f6d2e6578616d706c652e636f6d2f";
    size_t len = 0;
    uint8_t *bytes = unhex(wn, &len);
    MarmotGroupMediaPolicy p;
    assert(marmot_group_media_policy_decode(bytes, len, &p) == MARMOT_OK);
    assert(p.allowed_locator_kind_count == 1 && strcmp(p.allowed_locator_kinds[0], "blossom-v1") == 0 &&
           p.allowed_locator_kinds[1] == NULL);
    assert(p.default_blob_endpoint_count == 1 &&
           strcmp(p.default_blob_endpoints[0].locator_kind, "blossom-v1") == 0 &&
           strcmp(p.default_blob_endpoints[0].base_url, "https://blossom.example.com/") == 0 &&
           !p.default_blob_endpoints[0].base_url_unverified);
    /* allowed_locator_kinds is what marmot_media_imeta_build() takes. */
    MarmotMediaReference ref = { 0 };
    assert(marmot_media_reference_add_locator(&ref, "blossom-v1", "https://blossom.example.com/ab") ==
           MARMOT_OK);
    marmot_media_reference_clear(&ref);
    marmot_group_media_policy_clear(&p);
    free(bytes);

    /* The encoder normalizes as MDK's EncryptedMediaPolicyV2::new() does:
     * kinds trimmed, lowercased, deduplicated; URLs normalized, endpoints
     * deduplicated after normalization. */
    char *kinds[] = { " Blossom-V1 ", "blossom-v1", NULL };
    MarmotMediaBlobEndpoint eps[] = {
        { "blossom-v1", "https://Blossom.Example.com", false },
        { "BLOSSOM-V1", "https://blossom.example.com:443/", false },
    };
    MarmotGroupMediaPolicy in = { kinds, 2, eps, 2 };
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    assert(marmot_group_media_policy_encode(&in, &enc, &enc_len) == MARMOT_OK);
    assert_bytes_hex(enc, enc_len, wn);
    free(enc);
    /* Refused input: no kind, a disallowed endpoint kind, an unverified or
     * unverifiable URL, a query. */
    MarmotGroupMediaPolicy none = { kinds, 0, eps, 1 };
    assert(marmot_group_media_policy_encode(&none, &enc, &enc_len) == MARMOT_ERR_INVALID_INPUT);
    MarmotMediaBlobEndpoint other = { "ipfs-v1", "https://x.example/", false };
    MarmotGroupMediaPolicy wrong = { kinds, 1, &other, 1 };
    assert(marmot_group_media_policy_encode(&wrong, &enc, &enc_len) == MARMOT_ERR_INVALID_INPUT);
    MarmotMediaBlobEndpoint idna = { "blossom-v1", "https://xn--bcher-kva.example/", false };
    MarmotGroupMediaPolicy unv = { kinds, 1, &idna, 1 };
    assert(marmot_group_media_policy_encode(&unv, &enc, &enc_len) == MARMOT_ERR_INVALID_INPUT);
    MarmotMediaBlobEndpoint flagged = { "blossom-v1", "https://x.example/", true };
    MarmotGroupMediaPolicy fl = { kinds, 1, &flagged, 1 };
    assert(marmot_group_media_policy_encode(&fl, &enc, &enc_len) == MARMOT_ERR_INVALID_INPUT);
    MarmotMediaBlobEndpoint query = { "blossom-v1", "https://x.example/?a=1", false };
    MarmotGroupMediaPolicy q = { kinds, 1, &query, 1 };
    assert(marmot_group_media_policy_encode(&q, &enc, &enc_len) == MARMOT_ERR_INVALID_INPUT);
    /* An http endpoint is allowed (avatar URLs are https only). */
    MarmotMediaBlobEndpoint plain = { "blossom-v1", "http://x.example:80", false };
    MarmotGroupMediaPolicy pl = { kinds, 1, &plain, 1 };
    assert(marmot_group_media_policy_encode(&pl, &enc, &enc_len) == MARMOT_OK);
    assert(marmot_group_media_policy_decode(enc, enc_len, &p) == MARMOT_OK);
    assert(strcmp(p.default_blob_endpoints[0].base_url, "http://x.example/") == 0);
    marmot_group_media_policy_clear(&p);
    free(enc);
    char *n = NULL;
    assert(marmot_group_avatar_url_normalize("http://x.example/a.png", &n) == MARMOT_ERR_INVALID_INPUT);

    /* Stored endpoint URLs that no WHATWG serializer produces for a valid
     * endpoint are invalid, never merely unverified (MDK: query, fragment,
     * credentials, scheme, default port, missing path, case). */
    static const char *const never[] = {
        "https://x.example/?", "https://x.example/?q=1", "https://x.example/a?b",
        "https://x.example/#f", "https://u@x.example/", "https://u:p@x.example/",
        "http://x.example:80/", "https://x.example:443/", "https://x.example",
        "https://X.example/", "ftp://x.example/", "wss://x.example/", "https://x.example/a b",
        NULL };
    for (size_t i = 0; never[i]; i++) {
        size_t ul = strlen(never[i]);
        uint8_t st[160];
        size_t at = 0;
        static const char fmt[] = "encrypted-media-v2";
        st[at++] = (uint8_t)strlen(fmt);
        memcpy(st + at, fmt, strlen(fmt));
        at += strlen(fmt);
        st[at++] = 11;
        st[at++] = 10;
        memcpy(st + at, "blossom-v1", 10);
        at += 10;
        st[at++] = (uint8_t)(1 + 10 + 1 + ul);
        st[at++] = 10;
        memcpy(st + at, "blossom-v1", 10);
        at += 10;
        st[at++] = (uint8_t)ul;
        memcpy(st + at, never[i], ul);
        at += ul;
        MarmotGroupMediaPolicy np;
        MarmotError err = marmot_group_media_policy_decode(st, at, &np);
        if (err != MARMOT_ERR_MEDIA_INVALID_REFERENCE) fprintf(stderr, "\naccepts %s\n", never[i]);
        assert(err == MARMOT_ERR_MEDIA_INVALID_REFERENCE);
    }
}

int
main(void)
{
    assert(sodium_init() >= 0);
    printf("libmarmot: encrypted media v2 / group image tests\n");
    TEST(test_mdk_unit_file_key);
    TEST(test_mdk_v2_cases_byte_exact);
    TEST(test_mdk_v2_negatives);
    TEST(test_mdk_media_type_canonicalization);
    TEST(test_mdk_imeta_fixture);
    TEST(test_imeta_rules);
    TEST(test_imeta_build_requires_blossom_locator);
    TEST(test_blossom_fallback_url);
    TEST(test_group_roundtrip_and_retained_epoch);
    TEST(test_group_encrypt_fresh_nonce_and_input_rules);
    TEST(test_legacy_format_is_never_produced);
    TEST(test_legacy_reader);
    TEST(test_exporter_step_vectors);
    TEST(test_check_epoch);
    TEST(test_group_image_vectors);
    TEST(test_group_image_encrypt_fresh);
    TEST(test_avatar_url_vectors);
    TEST(test_avatar_url_stored_consensus);
    TEST(test_avatar_url_rules_and_precedence);
    TEST(test_policy_verdicts_match_mdk);
    TEST(test_policy_codecs);
    printf("All media tests passed.\n");
    return 0;
}
