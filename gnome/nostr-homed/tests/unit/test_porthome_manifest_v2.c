/*
 * test_porthome_manifest_v2.c — schema v2 round-trip + v1 parse-forward
 * + dot-dot rejection. Bead nostrc-q25o.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nh_porthome_manifest.h"
#include "nh_porthome_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d %s", __FILE__, __LINE__, #cond); } while (0)
#define ASSERT_EQ(a,b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) FAIL("%s:%d %s=%ld != %s=%ld", __FILE__, __LINE__, #a,_a,#b,_b); } while (0)
#define ASSERT_STREQ(a,b) do { const char *_a = (a), *_b = (b); if (!_a || !_b || strcmp(_a,_b)) FAIL("%s:%d '%s' != '%s'", __FILE__, __LINE__, _a?_a:"(null)", _b?_b:"(null)"); } while (0)

/* ── shared helpers ─────────────────────────────────────────────── */

static void make_home_key(uint8_t hk[32]) {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x11 * i);
    if (nh_porthome_key_derive(seed, hk) != NH_PORTHOME_OK) {
        fprintf(stderr, "FATAL: nh_porthome_key_derive failed\n");
        abort();
    }
}

/* Build a v2 fixture with FILE + DIR + SYMLINK entries, encode+decode
 * sealed, deep-compare (including decrypted plaintext names). */
static int test_v2_roundtrip(void) {
    uint8_t hk[32]; make_home_key(hk);

    nh_porthome_manifest m;
    uint8_t root[32]; for (int i = 0; i < 32; i++) root[i] = (uint8_t)(i * 3);
    ASSERT_EQ(nh_porthome_manifest_init_v2(&m, root), NH_PORTHOME_OK);
    ASSERT_EQ(m.version, NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2);

    /* Entry 1: DIR "docs" */
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "docs", &penc), NH_PORTHOME_OK);
        char *name = strdup("docs");
        ASSERT(name);
        ASSERT_EQ(nh_porthome_manifest_add_dir_v2(&m, penc, name,
            0755, 1000, 1000, 1700000000ULL * 1000000000ULL), NH_PORTHOME_OK);
    }
    /* Entry 2: FILE "docs/note.md" with 2 chunks. */
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "docs/note.md", &penc), NH_PORTHOME_OK);
        char *name = strdup("note.md");
        ASSERT(name);
        nh_porthome_chunk chunks[2];
        memset(chunks, 0, sizeof chunks);
        for (int j = 0; j < 32; j++) chunks[0].sha256[j] = (uint8_t)(j + 1);
        chunks[0].size = 4096; chunks[0].chunk_key_id = 0;
        for (int j = 0; j < 32; j++) chunks[1].sha256[j] = (uint8_t)(j + 128);
        chunks[1].size = 128;  chunks[1].chunk_key_id = 0;
        ASSERT_EQ(nh_porthome_manifest_add_file_v2(&m, penc, name,
            0644, 1000, 1000, 1700000001ULL * 1000000000ULL,
            4096 + 128, chunks, 2), NH_PORTHOME_OK);
    }
    /* Entry 3: SYMLINK "link.md" → "docs/note.md" */
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "link.md", &penc), NH_PORTHOME_OK);
        char *name = strdup("link.md");
        char *tgt = strdup("docs/note.md");
        ASSERT(name && tgt);
        ASSERT_EQ(nh_porthome_manifest_add_symlink_v2(&m, penc, name,
            0777, 1000, 1000, 1700000002ULL * 1000000000ULL, tgt), NH_PORTHOME_OK);
    }

    /* Convergence: seal_names then re-seal MUST produce identical bytes. */
    ASSERT_EQ(nh_porthome_manifest_seal_names(&m, hk), NH_PORTHOME_OK);
    uint8_t *first = malloc(m.entries[0].name_sealed_len);
    ASSERT(first);
    size_t first_len = m.entries[0].name_sealed_len;
    memcpy(first, m.entries[0].name_sealed, first_len);
    ASSERT_EQ(nh_porthome_manifest_seal_names(&m, hk), NH_PORTHOME_OK);
    ASSERT_EQ(m.entries[0].name_sealed_len, first_len);
    ASSERT_EQ(memcmp(first, m.entries[0].name_sealed, first_len), 0);
    free(first);

    /* Encode sealed → decode sealed → verify deep-equal. */
    uint8_t *sealed = NULL; size_t sealed_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode_sealed(&m, hk, &sealed, &sealed_len),
              NH_PORTHOME_OK);
    ASSERT(sealed && sealed_len > 0);

    nh_porthome_manifest *dm = NULL;
    ASSERT_EQ(nh_porthome_manifest_decode_sealed(sealed, sealed_len, hk, &dm),
              NH_PORTHOME_OK);
    ASSERT(dm);
    ASSERT_EQ(dm->version, NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V2);
    ASSERT_EQ(dm->entries_len, 3u);

    ASSERT_STREQ(dm->entries[0].name_plain, "docs");
    ASSERT_EQ(dm->entries[0].kind, NH_PORTHOME_KIND_DIR);
    ASSERT_STREQ(dm->entries[1].name_plain, "note.md");
    ASSERT_EQ(dm->entries[1].kind, NH_PORTHOME_KIND_FILE);
    ASSERT_EQ(dm->entries[1].chunks_len, 2u);
    ASSERT_STREQ(dm->entries[2].name_plain, "link.md");
    ASSERT_EQ(dm->entries[2].kind, NH_PORTHOME_KIND_SYMLINK);
    /* symlink_target should mirror the decrypted plaintext target so
     * downstream materialiser code needs no v1/v2 branch. */
    ASSERT_STREQ(dm->entries[2].symlink_target, "docs/note.md");
    ASSERT_STREQ(dm->entries[2].link_target_plain, "docs/note.md");

    /* path_enc round-trip preserved verbatim. */
    ASSERT_STREQ(dm->entries[0].path_enc, m.entries[0].path_enc);
    ASSERT_STREQ(dm->entries[1].path_enc, m.entries[1].path_enc);
    ASSERT_STREQ(dm->entries[2].path_enc, m.entries[2].path_enc);

    nh_porthome_manifest_dispose(dm); free(dm);
    free(sealed);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: v2 roundtrip (3 entries: dir/file/symlink)\n");
    return 0;
}

/* v1 parse-forward: encode a v1 manifest (init default) then decode it.
 * The parser must accept it, name_plain must remain NULL, and the
 * fetch materialiser rename walk (via public API) must be a no-op. */
static int test_v1_parse_forward(void) {
    uint8_t hk[32]; make_home_key(hk);

    nh_porthome_manifest m;
    uint8_t root[32]; for (int i = 0; i < 32; i++) root[i] = (uint8_t)(0x77 + i);
    ASSERT_EQ(nh_porthome_manifest_init(&m, root), NH_PORTHOME_OK);
    ASSERT_EQ(m.version, NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1);

    /* Dir + file + symlink, using v1 add helpers (no name_plain). */
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "legacy", &penc), NH_PORTHOME_OK);
        ASSERT_EQ(nh_porthome_manifest_add_dir(&m, penc,
            0755, 0, 0, 0), NH_PORTHOME_OK);
    }
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "legacy/file.txt", &penc),
                  NH_PORTHOME_OK);
        nh_porthome_chunk c; memset(&c, 0, sizeof c);
        for (int j = 0; j < 32; j++) c.sha256[j] = (uint8_t)j;
        c.size = 42; c.chunk_key_id = 0;
        ASSERT_EQ(nh_porthome_manifest_add_file(&m, penc,
            0644, 0, 0, 0, 42, &c, 1), NH_PORTHOME_OK);
    }
    {
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, "legacy/lnk", &penc),
                  NH_PORTHOME_OK);
        char *tgt = strdup("file.txt");
        ASSERT_EQ(nh_porthome_manifest_add_symlink(&m, penc,
            0777, 0, 0, 0, tgt), NH_PORTHOME_OK);
    }

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode_sealed(&m, hk, &sealed, &sealed_len),
              NH_PORTHOME_OK);

    nh_porthome_manifest *dm = NULL;
    ASSERT_EQ(nh_porthome_manifest_decode_sealed(sealed, sealed_len, hk, &dm),
              NH_PORTHOME_OK);
    ASSERT_EQ(dm->version, NH_PORTHOME_MANIFEST_SCHEMA_VERSION_V1);
    ASSERT_EQ(dm->entries_len, 3u);
    /* No plaintext-name material anywhere. */
    for (size_t i = 0; i < dm->entries_len; i++) {
        ASSERT(dm->entries[i].name_plain == NULL);
        ASSERT(dm->entries[i].name_sealed == NULL);
        ASSERT(dm->entries[i].link_target_sealed == NULL);
        ASSERT(dm->entries[i].link_target_plain == NULL);
    }
    /* v1 symlink still carries its plaintext target. */
    int found_symlink = 0;
    for (size_t i = 0; i < dm->entries_len; i++) {
        if (dm->entries[i].kind == NH_PORTHOME_KIND_SYMLINK) {
            ASSERT_STREQ(dm->entries[i].symlink_target, "file.txt");
            found_symlink = 1;
        }
    }
    ASSERT(found_symlink);

    /* rename_walk on a v1 manifest must succeed with renamed=0 and
     * missed=entries_len — no on-disk state exists so we're testing
     * only the short-circuit branch. */
    size_t renamed = 999, missed = 999;
    ASSERT_EQ(nh_porthome_rename_walk(0 /* staging_fd unused */, dm,
                                      &renamed, &missed), NH_PORTHOME_OK);
    ASSERT_EQ(renamed, 0u);
    ASSERT_EQ(missed, dm->entries_len);

    nh_porthome_manifest_dispose(dm); free(dm);
    free(sealed);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: v1 parse-forward (name_plain stays NULL)\n");
    return 0;
}

/* Dot-dot rejection: hand-craft a v2 manifest whose sealed-name slot
 * decrypts to "../etc/passwd" and prove the decoder refuses. We do
 * this the easy way — build a v2 manifest with a legitimate basename,
 * then patch e->name_plain to "../etc/passwd" and re-seal, then
 * encode. The parser will decrypt the slot, hit the basename gate,
 * and reject the whole manifest. */
static int test_dotdot_reject(void) {
    uint8_t hk[32]; make_home_key(hk);

    /* Build a valid v2 manifest with a single DIR entry. */
    nh_porthome_manifest m;
    uint8_t root[32]; memset(root, 0x22, 32);
    ASSERT_EQ(nh_porthome_manifest_init_v2(&m, root), NH_PORTHOME_OK);
    char *penc = NULL;
    ASSERT_EQ(nh_porthome_encrypt_path(hk, "safe", &penc), NH_PORTHOME_OK);
    char *ok_name = strdup("safe");
    ASSERT_EQ(nh_porthome_manifest_add_dir_v2(&m, penc, ok_name,
        0755, 0, 0, 0), NH_PORTHOME_OK);

    /* Overwrite name_plain with a smuggle attempt WITHOUT going through
     * add_dir_v2's validator. The seal step will encrypt it verbatim
     * because it's just bytes at that layer. */
    free(m.entries[0].name_plain);
    m.entries[0].name_plain = strdup("../etc/passwd");
    free(m.entries[0].name_sealed);
    m.entries[0].name_sealed = NULL;
    m.entries[0].name_sealed_len = 0;

    ASSERT_EQ(nh_porthome_manifest_seal_names(&m, hk), NH_PORTHOME_OK);

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode_sealed(&m, hk, &sealed, &sealed_len),
              NH_PORTHOME_OK);

    /* The parser must refuse this. `decode_sealed` returns
     * NH_PORTHOME_ERR_PATH from open_names. */
    nh_porthome_manifest *dm = NULL;
    int rc = nh_porthome_manifest_decode_sealed(sealed, sealed_len, hk, &dm);
    ASSERT(rc != NH_PORTHOME_OK);
    ASSERT(dm == NULL);
    fprintf(stderr, "ok: dot-dot sealed-name rejected (rc=%d)\n", rc);

    free(sealed);
    nh_porthome_manifest_dispose(&m);
    return 0;
}

/* Slash-in-basename rejection — same shape as dot-dot but the smuggle
 * uses '/' to try to lay down "a/b" so the rename walk would rename
 * INTO a subdirectory. */
static int test_slash_reject(void) {
    uint8_t hk[32]; make_home_key(hk);
    nh_porthome_manifest m;
    uint8_t root[32]; memset(root, 0x33, 32);
    ASSERT_EQ(nh_porthome_manifest_init_v2(&m, root), NH_PORTHOME_OK);
    char *penc = NULL;
    ASSERT_EQ(nh_porthome_encrypt_path(hk, "ok", &penc), NH_PORTHOME_OK);
    char *ok_name = strdup("ok");
    ASSERT_EQ(nh_porthome_manifest_add_dir_v2(&m, penc, ok_name,
        0755, 0, 0, 0), NH_PORTHOME_OK);

    free(m.entries[0].name_plain);
    m.entries[0].name_plain = strdup("a/b");
    free(m.entries[0].name_sealed);
    m.entries[0].name_sealed = NULL;
    m.entries[0].name_sealed_len = 0;

    ASSERT_EQ(nh_porthome_manifest_seal_names(&m, hk), NH_PORTHOME_OK);

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    ASSERT_EQ(nh_porthome_manifest_encode_sealed(&m, hk, &sealed, &sealed_len),
              NH_PORTHOME_OK);
    nh_porthome_manifest *dm = NULL;
    int rc = nh_porthome_manifest_decode_sealed(sealed, sealed_len, hk, &dm);
    ASSERT(rc != NH_PORTHOME_OK);
    ASSERT(dm == NULL);
    fprintf(stderr, "ok: slash-in-basename rejected (rc=%d)\n", rc);
    free(sealed);
    nh_porthome_manifest_dispose(&m);
    return 0;
}

/* v2 encoder MUST refuse to serialize a v2 manifest whose entry lacks
 * name material — catches misuse of add_file (v1) after init_v2. */
static int test_v2_encoder_refuses_naked_entry(void) {
    uint8_t hk[32]; make_home_key(hk);
    nh_porthome_manifest m;
    uint8_t root[32]; memset(root, 0x44, 32);
    ASSERT_EQ(nh_porthome_manifest_init_v2(&m, root), NH_PORTHOME_OK);

    char *penc = NULL;
    ASSERT_EQ(nh_porthome_encrypt_path(hk, "orphan", &penc), NH_PORTHOME_OK);
    ASSERT_EQ(nh_porthome_manifest_add_dir(&m, penc, 0755, 0, 0, 0),
              NH_PORTHOME_OK);
    /* name_plain not set — encoder should fail. */
    uint8_t *out = NULL; size_t out_len = 0;
    int rc = nh_porthome_manifest_encode(&m, &out, &out_len);
    ASSERT(rc != NH_PORTHOME_OK);
    free(out);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: v2 encoder refuses entries without name material\n");
    return 0;
}

int main(void) {
    int failed = 0;
    failed += test_v2_roundtrip();
    failed += test_v1_parse_forward();
    failed += test_dotdot_reject();
    failed += test_slash_reject();
    failed += test_v2_encoder_refuses_naked_entry();
    if (failed) {
        fprintf(stderr, "test_porthome_manifest_v2: %d sub-test(s) FAILED\n", failed);
        return 1;
    }
    fprintf(stderr, "test_porthome_manifest_v2: all pass\n");
    return 0;
}
