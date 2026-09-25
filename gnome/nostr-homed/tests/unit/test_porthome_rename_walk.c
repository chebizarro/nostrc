/*
 * test_porthome_rename_walk.c — depth-first rename walk over a
 * fabricated destination tree. Bead nostrc-bms6.
 *
 * SPDX-License-Identifier: MIT
 *
 * Scenario: fabricate a small tree under a mktemp destination whose
 * path components are 48-hex path_enc names, then run
 * nh_porthome_rename_walk with a matching v2 manifest and assert
 * the final tree has the expected plaintext layout AND that a
 * 3-level nested subtree renames children-before-parents correctly.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nh_porthome_manifest.h"
#include "nh_porthome_crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d %s", __FILE__, __LINE__, #cond); } while (0)
#define ASSERT_EQ(a,b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) FAIL("%s:%d %s=%ld != %s=%ld", __FILE__, __LINE__, #a,_a,#b,_b); } while (0)

static int mkdirp_at(int base, const char *rel) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", rel);
    char *p = buf;
    while (*p) {
        while (*p == '/') p++;
        char *slash = strchr(p, '/');
        if (slash) *slash = '\0';
        if (*p) {
            if (mkdirat(base, buf, 0755) != 0 && errno != EEXIST) return -1;
        }
        if (slash) { *slash = '/'; p = slash; } else break;
    }
    return 0;
}

static int touch_at(int base, const char *rel, const char *body) {
    /* Create every intermediate directory. */
    char buf[4096]; snprintf(buf, sizeof buf, "%s", rel);
    char *last = strrchr(buf, '/');
    if (last) {
        *last = '\0';
        if (mkdirp_at(base, buf) != 0) return -1;
        *last = '/';
    }
    int fd = openat(base, rel, O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (body) {
        size_t n = strlen(body);
        if (write(fd, body, n) != (ssize_t)n) { close(fd); return -1; }
    }
    close(fd);
    return 0;
}

static int path_exists(int base, const char *rel) {
    struct stat st;
    return fstatat(base, rel, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

static int read_file_at(int base, const char *rel, char *out, size_t cap) {
    int fd = openat(base, rel, O_RDONLY|O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return (int)n;
}

/* Helper: build a v2 manifest for a set of plaintext rel_paths, using
 * the given home_key to compute path_enc and to seal name material. */
typedef struct { const char *rel; int kind; const char *content; } spec_t;

static int build_manifest_from_specs(nh_porthome_manifest *m,
                                     const uint8_t hk[32],
                                     const spec_t *specs, size_t nspecs) {
    uint8_t root[32]; memset(root, 0x55, 32);
    if (nh_porthome_manifest_init_v2(m, root) != 0) return -1;
    for (size_t i = 0; i < nspecs; i++) {
        char *penc = NULL;
        if (nh_porthome_encrypt_path(hk, specs[i].rel, &penc) != 0) return -1;
        const char *slash = strrchr(specs[i].rel, '/');
        char *name = strdup(slash ? slash + 1 : specs[i].rel);
        if (!name) { free(penc); return -1; }
        if (specs[i].kind == NH_PORTHOME_KIND_DIR) {
            if (nh_porthome_manifest_add_dir_v2(m, penc, name,
                    0755, 0, 0, 0) != 0) return -1;
        } else if (specs[i].kind == NH_PORTHOME_KIND_FILE) {
            /* Dummy 1-chunk manifest — the rename walk doesn't care
             * about content, only about path_enc / name_plain. */
            nh_porthome_chunk c; memset(&c, 0, sizeof c);
            for (int j = 0; j < 32; j++) c.sha256[j] = (uint8_t)(i * 7 + j);
            c.size = 42; c.chunk_key_id = 0;
            uint64_t sz = specs[i].content ? strlen(specs[i].content) : 42;
            if (nh_porthome_manifest_add_file_v2(m, penc, name,
                    0644, 0, 0, 0, sz, &c, 1) != 0) return -1;
        } else if (specs[i].kind == NH_PORTHOME_KIND_SYMLINK) {
            char *tgt = strdup(specs[i].content ? specs[i].content : "x");
            if (nh_porthome_manifest_add_symlink_v2(m, penc, name,
                    0777, 0, 0, 0, tgt) != 0) return -1;
        }
    }
    /* Seal so name_sealed is populated (round-trip semantics). */
    if (nh_porthome_manifest_seal_names(m, hk) != 0) return -1;
    return 0;
}

static int test_flat_rename(void) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    uint8_t hk[32]; ASSERT_EQ(nh_porthome_key_derive(seed, hk), NH_PORTHOME_OK);

    char tmpl[] = "/tmp/nhp_rw_flat_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    ASSERT(tmp);
    int base = open(tmp, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ASSERT(base >= 0);

    /* Two flat files under root. */
    spec_t specs[] = {
        { "alpha.txt",  NH_PORTHOME_KIND_FILE, "aaa" },
        { "beta.log",   NH_PORTHOME_KIND_FILE, "bbb" },
    };
    size_t n = sizeof specs / sizeof specs[0];

    nh_porthome_manifest m;
    ASSERT_EQ(build_manifest_from_specs(&m, hk, specs, n), 0);

    /* Simulate materialize: write the path_enc-named files. */
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(touch_at(base, m.entries[i].path_enc, specs[i].content), 0);
    }

    size_t renamed = 0, missed = 0;
    ASSERT_EQ(nh_porthome_rename_walk(base, &m, &renamed, &missed),
              NH_PORTHOME_OK);
    ASSERT_EQ(renamed, n);
    ASSERT_EQ(missed, 0u);

    /* Plaintext basenames now on disk; path_enc names gone. */
    ASSERT(path_exists(base, "alpha.txt"));
    ASSERT(path_exists(base, "beta.log"));
    ASSERT(!path_exists(base, m.entries[0].path_enc));
    ASSERT(!path_exists(base, m.entries[1].path_enc));

    /* Content preserved. */
    char buf[16];
    ASSERT(read_file_at(base, "alpha.txt", buf, sizeof buf) > 0);
    ASSERT_EQ(strcmp(buf, "aaa"), 0);

    close(base);
    /* Best-effort cleanup. */
    char rm[256]; snprintf(rm, sizeof rm, "rm -rf %s", tmp);
    (void)system(rm);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: flat rename walk (2 files)\n");
    return 0;
}

static int test_nested_rename(void) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 7);
    uint8_t hk[32]; ASSERT_EQ(nh_porthome_key_derive(seed, hk), NH_PORTHOME_OK);

    char tmpl[] = "/tmp/nhp_rw_nested_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    ASSERT(tmp);
    int base = open(tmp, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ASSERT(base >= 0);

    /* 3-deep nesting: a/b/c/leaf.txt. Order in the manifest doesn't
     * matter because rename_walk sorts by depth descending. */
    spec_t specs[] = {
        { "a",              NH_PORTHOME_KIND_DIR,  NULL },
        { "a/b",            NH_PORTHOME_KIND_DIR,  NULL },
        { "a/b/c",          NH_PORTHOME_KIND_DIR,  NULL },
        { "a/b/c/leaf.txt", NH_PORTHOME_KIND_FILE, "deep!" },
        { "top.md",         NH_PORTHOME_KIND_FILE, "top" },
    };
    size_t n = sizeof specs / sizeof specs[0];

    nh_porthome_manifest m;
    ASSERT_EQ(build_manifest_from_specs(&m, hk, specs, n), 0);

    /* Materialise path_enc-shaped tree. */
    for (size_t i = 0; i < n; i++) {
        if (specs[i].kind == NH_PORTHOME_KIND_DIR) {
            ASSERT_EQ(mkdirp_at(base, m.entries[i].path_enc), 0);
        } else {
            ASSERT_EQ(touch_at(base, m.entries[i].path_enc, specs[i].content), 0);
        }
    }

    size_t renamed = 0, missed = 0;
    ASSERT_EQ(nh_porthome_rename_walk(base, &m, &renamed, &missed),
              NH_PORTHOME_OK);
    ASSERT_EQ(renamed, n);
    ASSERT_EQ(missed, 0u);

    /* Every plaintext path now visible from root. */
    ASSERT(path_exists(base, "a"));
    ASSERT(path_exists(base, "a/b"));
    ASSERT(path_exists(base, "a/b/c"));
    ASSERT(path_exists(base, "a/b/c/leaf.txt"));
    ASSERT(path_exists(base, "top.md"));

    /* And no path_enc leaks under root. */
    ASSERT(!path_exists(base, m.entries[0].path_enc));

    /* Content preserved through nested rename. */
    char buf[16];
    ASSERT(read_file_at(base, "a/b/c/leaf.txt", buf, sizeof buf) > 0);
    ASSERT_EQ(strcmp(buf, "deep!"), 0);

    close(base);
    char rm[256]; snprintf(rm, sizeof rm, "rm -rf %s", tmp);
    (void)system(rm);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: nested rename walk (3-deep)\n");
    return 0;
}

/* Collision: manifest lists two entries whose plaintext basenames
 * collide under the same parent (hostile pointer). rename_walk
 * refuses with NH_PORTHOME_ERR_PATH. */
static int test_collision_refused(void) {
    uint8_t seed[32]; memset(seed, 0xAA, 32);
    uint8_t hk[32]; ASSERT_EQ(nh_porthome_key_derive(seed, hk), NH_PORTHOME_OK);

    char tmpl[] = "/tmp/nhp_rw_coll_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    ASSERT(tmp);
    int base = open(tmp, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ASSERT(base >= 0);

    nh_porthome_manifest m;
    uint8_t root[32]; memset(root, 0xBB, 32);
    ASSERT_EQ(nh_porthome_manifest_init_v2(&m, root), NH_PORTHOME_OK);

    /* Two entries with different path_enc but the same name_plain
     * "dup.txt" under root. */
    for (int i = 0; i < 2; i++) {
        char rel[32]; snprintf(rel, sizeof rel, "orig_%d", i);
        char *penc = NULL;
        ASSERT_EQ(nh_porthome_encrypt_path(hk, rel, &penc), NH_PORTHOME_OK);
        nh_porthome_chunk c; memset(&c, 0, sizeof c);
        c.size = 8; c.chunk_key_id = 0;
        char *name = strdup("dup.txt");
        ASSERT_EQ(nh_porthome_manifest_add_file_v2(&m, penc, name,
                    0644, 0, 0, 0, 3, &c, 1), NH_PORTHOME_OK);
        ASSERT_EQ(touch_at(base, m.entries[i].path_enc, "xyz"), 0);
    }
    ASSERT_EQ(nh_porthome_manifest_seal_names(&m, hk), NH_PORTHOME_OK);

    size_t renamed = 0, missed = 0;
    int rc = nh_porthome_rename_walk(base, &m, &renamed, &missed);
    /* First rename succeeds; second finds the plaintext leaf already
     * present and refuses. */
    ASSERT(rc == NH_PORTHOME_ERR_PATH);

    close(base);
    char rmc[256]; snprintf(rmc, sizeof rmc, "rm -rf %s", tmp);
    (void)system(rmc);
    nh_porthome_manifest_dispose(&m);
    fprintf(stderr, "ok: collision refused (rc=%d)\n", rc);
    return 0;
}

int main(void) {
    int failed = 0;
    failed += test_flat_rename();
    failed += test_nested_rename();
    failed += test_collision_refused();
    if (failed) {
        fprintf(stderr, "test_porthome_rename_walk: %d sub-test(s) FAILED\n", failed);
        return 1;
    }
    fprintf(stderr, "test_porthome_rename_walk: all pass\n");
    return 0;
}
