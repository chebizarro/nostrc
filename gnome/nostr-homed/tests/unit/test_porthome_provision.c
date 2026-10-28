/*
 * test_porthome_provision.c — exercises nh_porthome_materialize_into_fd
 * and nh_porthome_materialize_sealed_into_fd end-to-end using an
 * in-memory blob store. Also asserts the LIMITED_MODE no-push interlock
 * on a hostile manifest (garbage bytes -> LIMITED, no writes into the
 * staging dir). Bead nostrc-89rj.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "nh_porthome_crypto.h"
#include "nh_porthome_manifest.h"
#include "nh_porthome_provision.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void die(const char *msg) { fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno); exit(1); }
static void pass(const char *msg) { fprintf(stdout, "PASS: %s\n", msg); }

/* ────────────────────── in-memory blob store ────────────────────── */

typedef struct blob {
    char hex[65];
    uint8_t *ct;
    size_t ct_len;
    struct blob *next;
} blob;

static blob *g_blobs;

static void store_put(const char *hex, const uint8_t *ct, size_t ct_len) {
    blob *b = calloc(1, sizeof *b);
    memcpy(b->hex, hex, 65);
    b->ct = malloc(ct_len);
    memcpy(b->ct, ct, ct_len);
    b->ct_len = ct_len;
    b->next = g_blobs;
    g_blobs = b;
}

static int store_fetch(void *ctx, const char *sha256_hex,
                       uint8_t **out_ct, size_t *out_ct_len) {
    (void)ctx;
    for (blob *b = g_blobs; b; b = b->next) {
        if (strncasecmp(b->hex, sha256_hex, 64) == 0) {
            uint8_t *copy = malloc(b->ct_len);
            memcpy(copy, b->ct, b->ct_len);
            *out_ct = copy;
            *out_ct_len = b->ct_len;
            return 0;
        }
    }
    return -1;
}

/* A fetcher that always fails — used for the LIMITED test. */
static int store_fetch_fail(void *ctx, const char *hex,
                            uint8_t **out_ct, size_t *out_ct_len) {
    (void)ctx; (void)hex; (void)out_ct; (void)out_ct_len;
    return -1;
}

/* ────────────────────── helpers ────────────────────── */

static int make_tempdir(char out_path[512], const char *label) {
    snprintf(out_path, 512, "/tmp/porthome_provision_%s_XXXXXX", label);
    if (!mkdtemp(out_path)) return -1;
    return 0;
}

static int count_dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

/* Build a small manifest with two files under the same encrypted-path
 * prefix, upload their ciphertexts to the in-memory store, and return
 * the sealed manifest bytes. */
static uint8_t *build_fixture(const uint8_t home_key[32],
                              size_t *out_len) {
    nh_porthome_manifest m;
    uint8_t root[32]; memset(root, 0x11, 32);
    if (nh_porthome_manifest_init(&m, root) != 0) die("manifest_init");

    /* Add a dir + a small file inside it. */
    char *dpath = strdup("a");   /* small path components */
    if (nh_porthome_manifest_add_dir(&m, dpath, 0755, 1000, 1000,
                                     1600000000ULL * 1000000000ULL) != 0)
        die("add_dir");

    /* File contents. */
    const char *content = "hello portable home\n";
    size_t clen = strlen(content);

    uint8_t *ct = NULL; size_t ct_len = 0; uint8_t sha[32];
    if (nh_porthome_encrypt_chunk(home_key, (const uint8_t *)content, clen,
                                  &ct, &ct_len, sha) != 0)
        die("encrypt_chunk");
    char hex[65]; nh_porthome_hex64(sha, hex);
    store_put(hex, ct, ct_len);
    free(ct);

    nh_porthome_chunk ch;
    memcpy(ch.sha256, sha, 32);
    ch.size = (uint32_t)ct_len;
    ch.chunk_key_id = 0;

    char *fpath = strdup("a/greeting.txt");
    if (nh_porthome_manifest_add_file(&m, fpath, 0644, 1000, 1000,
                                      1600000000ULL * 1000000000ULL,
                                      (uint64_t)clen, &ch, 1) != 0)
        die("add_file");

    uint8_t *sealed = NULL; size_t sealed_len = 0;
    if (nh_porthome_manifest_encode_sealed(&m, home_key, &sealed, &sealed_len) != 0)
        die("encode_sealed");
    nh_porthome_manifest_dispose(&m);
    *out_len = sealed_len;
    return sealed;
}

/* ────────────────────── tests ────────────────────── */

static void test_e2e(void) {
    uint8_t seed[32]; memset(seed, 0x42, 32);
    uint8_t home_key[32];
    if (nh_porthome_key_derive(seed, home_key) != 0) die("key_derive");

    size_t sealed_len = 0;
    uint8_t *sealed = build_fixture(home_key, &sealed_len);

    char stage[512];
    if (make_tempdir(stage, "e2e") != 0) die("mkdtemp");
    int fd = open(stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) die("open stage");

    nh_porthome_prov_opts opts = {0};
    opts.local_uid = (uid_t)getuid();
    opts.local_gid = (gid_t)getgid();

    nh_porthome_prov_status r = nh_porthome_materialize_sealed_into_fd(
        fd, sealed, sealed_len, home_key, store_fetch, NULL, &opts, "tx1");
    if (r != NH_PORTHOME_PROV_OK) {
        fprintf(stderr, "materialize rc=%d\n", (int)r);
        die("materialize e2e");
    }

    /* Verify file exists and matches. */
    char fpath[600];
    snprintf(fpath, sizeof fpath, "%s/a/greeting.txt", stage);
    FILE *f = fopen(fpath, "rb");
    if (!f) die("fopen greeting");
    char buf[64]; size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n != strlen("hello portable home\n") ||
        memcmp(buf, "hello portable home\n", n) != 0)
        die("greeting content mismatch");
    pass("e2e materialize: greeting.txt content matches");

    /* Verify mode. */
    struct stat st;
    if (stat(fpath, &st) != 0) die("stat greeting");
    if ((st.st_mode & 0777) != 0644) die("greeting mode");
    pass("e2e materialize: mode 0644");

    close(fd);
    free(sealed);
}

static void test_limited_mode_hostile_manifest(void) {
    uint8_t seed[32]; memset(seed, 0x43, 32);
    uint8_t home_key[32];
    nh_porthome_key_derive(seed, home_key);

    /* Empty existing staging dir seeded with a "user file" that MUST
     * survive: the interlock says a manifest fetch failure never
     * touches the existing home. */
    char stage[512];
    if (make_tempdir(stage, "limited") != 0) die("mkdtemp");
    char guard[600]; snprintf(guard, sizeof guard, "%s/USER_FILE", stage);
    FILE *g = fopen(guard, "w");
    fputs("do not touch", g);
    fclose(g);

    int fd = open(stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) die("open stage");

    /* Feed a garbage sealed manifest — decrypt will fail. */
    uint8_t garbage[64];
    memset(garbage, 0x99, sizeof garbage);
    nh_porthome_prov_opts opts = {0};
    opts.local_uid = (uid_t)getuid();
    opts.local_gid = (gid_t)getgid();
    nh_porthome_prov_status r = nh_porthome_materialize_sealed_into_fd(
        fd, garbage, sizeof garbage, home_key, store_fetch_fail, NULL,
        &opts, "tx-limited");
    if (r != NH_PORTHOME_PROV_LIMITED) {
        fprintf(stderr, "expected LIMITED, got %d\n", (int)r);
        die("limited path did not return LIMITED");
    }
    pass("hostile manifest -> LIMITED");

    /* Guard file must still be present + untouched. */
    struct stat st;
    if (stat(guard, &st) != 0) die("guard vanished!");
    FILE *rf = fopen(guard, "rb");
    char buf[64]; size_t n = fread(buf, 1, sizeof buf, rf);
    fclose(rf);
    if (n != strlen("do not touch") || memcmp(buf, "do not touch", n) != 0)
        die("guard content mutated");
    pass("LIMITED interlock: existing home preserved");

    /* Directory should still have exactly one entry. */
    int cnt = count_dir_entries(stage);
    if (cnt != 1) { fprintf(stderr, "entries=%d\n", cnt); die("dir entry count"); }
    pass("LIMITED interlock: no new entries created");

    close(fd);
}

static void test_limited_mode_chunk_fetch_fail(void) {
    uint8_t seed[32]; memset(seed, 0x44, 32);
    uint8_t home_key[32];
    nh_porthome_key_derive(seed, home_key);

    size_t sealed_len = 0;
    uint8_t *sealed = build_fixture(home_key, &sealed_len);

    char stage[512];
    if (make_tempdir(stage, "chunkfail") != 0) die("mkdtemp");
    int fd = open(stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) die("open stage");

    /* Use the fail-fetcher: manifest decrypts fine, but no chunk
     * is fetchable — status must be LIMITED. */
    nh_porthome_prov_opts opts = {0};
    opts.local_uid = (uid_t)getuid();
    opts.local_gid = (gid_t)getgid();
    nh_porthome_prov_status r = nh_porthome_materialize_sealed_into_fd(
        fd, sealed, sealed_len, home_key, store_fetch_fail, NULL, &opts, "tx-cf");
    if (r != NH_PORTHOME_PROV_LIMITED) {
        fprintf(stderr, "expected LIMITED, got %d\n", (int)r);
        die("chunk-fetch-fail path");
    }
    pass("chunk fetch fail -> LIMITED");
    close(fd);
    free(sealed);
}

static int lazy_fetch_count;
static int counting_fetch(void *ctx, const char *addr, uint8_t **ct, size_t *len) {
    lazy_fetch_count++;
    return store_fetch(ctx, addr, ct, len);
}

static void test_fresh_lazy_archive(void) {
    uint8_t seed[32], key[32], root[32];
    memset(seed, 0x52, sizeof seed); memset(root, 0x35, sizeof root);
    if (nh_porthome_key_derive(seed, key) != 0) die("lazy key");
    nh_porthome_manifest m;
    if (nh_porthome_manifest_init_v2(&m, root) != 0) die("lazy init");
    if (nh_porthome_manifest_add_dir_v2(&m, strdup("encarchive"),
            strdup("Archive"), 0755, 0, 0, 1) != 0) die("lazy dir");
    m.entries[m.entries_len - 1].lazy = true;
    if (nh_porthome_manifest_add_dir_v2(&m, strdup("encarchive/encyear"),
            strdup("2024"), 0755, 0, 0, 1) != 0) die("lazy child dir");
    const char *cold = "cold bytes", *eager = "eager bytes";
    nh_porthome_chunk ch = {0};
    uint8_t *ct = NULL; size_t ct_len = 0;
    if (nh_porthome_encrypt_chunk(key, (const uint8_t *)cold, strlen(cold),
                                  &ct, &ct_len, ch.sha256) != 0) die("cold seal");
    ch.size = (uint32_t)ct_len;
    char hex[65]; nh_porthome_hex64(ch.sha256, hex);
    store_put(hex, ct, ct_len); free(ct);
    if (nh_porthome_manifest_add_file_v2(&m,
            strdup("encarchive/encyear/enccold"), strdup("cold.txt"),
            0644, 0, 0, 1, strlen(cold), &ch, 1) != 0) die("cold entry");
    if (nh_porthome_encrypt_chunk(key, (const uint8_t *)eager, strlen(eager),
                                  &ct, &ct_len, ch.sha256) != 0) die("eager seal");
    ch.size = (uint32_t)ct_len;
    nh_porthome_hex64(ch.sha256, hex); store_put(hex, ct, ct_len); free(ct);
    if (nh_porthome_manifest_add_file_v2(&m, strdup("enceager"),
            strdup("eager.txt"), 0644, 0, 0, 1, strlen(eager), &ch, 1) != 0)
        die("eager entry");
    uint8_t *wire = NULL; size_t wire_len = 0;
    if (nh_porthome_manifest_encode_sealed(&m, key, &wire, &wire_len) != 0)
        die("lazy wire encode");
    nh_porthome_manifest *decoded = NULL;
    if (nh_porthome_manifest_decode_sealed(wire, wire_len, key, &decoded) != 0)
        die("lazy wire decode");
    char stage[512];
    if (make_tempdir(stage, "lazy") != 0) die("lazy temp");
    int fd = open(stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) die("lazy fd");
    nh_porthome_prov_opts opts = {.local_uid = getuid(), .local_gid = getgid()};
    lazy_fetch_count = 0;
    if (nh_porthome_materialize_into_fd(fd, decoded, key, counting_fetch,
                                        NULL, &opts, "tx-lazy") != NH_PORTHOME_PROV_OK)
        die("lazy materialize");
    size_t renamed = 0, missed = 0;
    if (nh_porthome_rename_walk(fd, decoded, &renamed, &missed) != 0)
        die("lazy rename");
    char path[600];
    snprintf(path, sizeof path, "%s/Archive", stage);
    if (count_dir_entries(path) != 0) die("Archive not empty");
    snprintf(path, sizeof path, "%s/eager.txt", stage);
    FILE *f = fopen(path, "rb");
    if (!f) die("eager missing");
    char buf[32]; size_t got = fread(buf, 1, sizeof buf, f); fclose(f);
    if (got != strlen(eager) || memcmp(buf, eager, got)) die("eager content");
    if (lazy_fetch_count != 1) die("cold chunk was fetched");
    pass("fresh marked Archive stays empty; eager sibling fetched once");
    close(fd); free(wire); nh_porthome_manifest_dispose(decoded); free(decoded);
    nh_porthome_manifest_dispose(&m);
}

int main(void) {
    test_fresh_lazy_archive();
    test_e2e();
    test_limited_mode_hostile_manifest();
    test_limited_mode_chunk_fetch_fail();
    printf("OK test_porthome_provision\n");
    return 0;
}
