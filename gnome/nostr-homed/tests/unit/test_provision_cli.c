/*
 * test_provision_cli.c — unit tests for the nostr-homed-provision helpers.
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-5fdu (follow-up of nostrc-89rj).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nh_provision_cli.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Tiny helper: cheap "contains" scan on a NUL-terminated string. */
static int contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

/* ── 1. Account init + JSON round-trip (redacted vs raw) ─────────── */
static void test_account_round_trip(void) {
    nh_prov_account a;
    nh_prov_account_init(&a);

    /* d_tag default check */
    assert(strcmp(a.d_tag, "nostr-homed.home.v1:personal") == 0);
    assert(a.n_home_relays == 0);

    /* Fill in a canonical fake identity. */
    memset(a.account_pubkey_hex, 'a', 64); a.account_pubkey_hex[64] = 0;
    memset(a.account_nsec_hex,   'b', 64); a.account_nsec_hex[64] = 0;
    memset(a.wrap_seed_hex,      'c', 64); a.wrap_seed_hex[64] = 0;
    memset(a.home_key_hex,       'd', 64); a.home_key_hex[64] = 0;
    memset(a.root_id_hex,        'c', 64); a.root_id_hex[64] = 0;
    snprintf(a.home_relays[0], sizeof a.home_relays[0], "%s",
             "wss://relay.sharegap.net");
    a.n_home_relays = 1;
    snprintf(a.blossom_servers[0], sizeof a.blossom_servers[0], "%s",
             "https://blossom.sharegap.net");
    a.n_blossom_servers = 1;
    a.generation = 3;

    char *raw = nh_prov_account_to_json(&a, 0);
    char *red = nh_prov_account_to_json(&a, 1);
    assert(raw && red);
    /* Raw carries secrets; redacted carries "REDACTED". */
    assert(contains(raw, "bbbbb"));
    assert(!contains(red, "bbbbb"));
    assert(contains(red, "REDACTED"));
    /* Non-secret fields visible in both. */
    assert(contains(raw, "wss://relay.sharegap.net"));
    assert(contains(red, "wss://relay.sharegap.net"));
    assert(contains(raw, "\"generation\":         3"));

    /* Round-trip. */
    nh_prov_account b;
    int r = nh_prov_account_from_json(raw, strlen(raw), &b);
    assert(r == 0);
    assert(strcmp(b.account_pubkey_hex, a.account_pubkey_hex) == 0);
    assert(strcmp(b.account_nsec_hex,   a.account_nsec_hex)   == 0);
    assert(strcmp(b.wrap_seed_hex,      a.wrap_seed_hex)      == 0);
    assert(strcmp(b.home_key_hex,       a.home_key_hex)       == 0);
    assert(strcmp(b.root_id_hex,        a.root_id_hex)        == 0);
    assert(strcmp(b.d_tag,              a.d_tag)              == 0);
    assert(b.n_home_relays == 1);
    assert(strcmp(b.home_relays[0], a.home_relays[0]) == 0);
    assert(b.n_blossom_servers == 1);
    assert(b.generation == 3);

    free(raw); free(red);
    fprintf(stderr, "OK  account_round_trip\n");
}

/* ── 2. Account JSON rejects unknown keys ────────────────────────── */
static void test_account_rejects_unknown_keys(void) {
    const char *hostile =
        "{\n"
        "  \"schema\": 1,\n"
        "  \"account_pubkey_hex\": \"deadbeef\",\n"
        "  \"hostile_key\": 42\n"
        "}\n";
    nh_prov_account a;
    int r = nh_prov_account_from_json(hostile, strlen(hostile), &a);
    assert(r != 0);
    fprintf(stderr, "OK  account_rejects_unknown_keys\n");
}

/* ── 3. Account JSON rejects bad schema ──────────────────────────── */
static void test_account_rejects_schema(void) {
    const char *bad = "{\"schema\": 2}";
    nh_prov_account a;
    int r = nh_prov_account_from_json(bad, strlen(bad), &a);
    assert(r != 0);
    fprintf(stderr, "OK  account_rejects_schema\n");
}

/* ── 4. CSV splitter ─────────────────────────────────────────────── */
static void test_split_csv(void) {
    size_t n = 0;
    char **a = nh_prov_split_csv("wss://a.example, wss://b.example ,wss://c.example", &n);
    assert(a && n == 3);
    assert(strcmp(a[0], "wss://a.example") == 0);
    assert(strcmp(a[1], "wss://b.example") == 0);
    assert(strcmp(a[2], "wss://c.example") == 0);
    nh_prov_free_csv(a, n);

    /* Empty input yields NULL. */
    size_t m = 42;
    char **b = nh_prov_split_csv("", &m);
    assert(!b && m == 0);
    fprintf(stderr, "OK  split_csv\n");
}

/* ── 5. Push dry-run walks a fabricated tree ─────────────────────── */
static int mkfile(const char *path, const char *body, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, body, len);
    close(fd);
    return w == (ssize_t)len ? 0 : -1;
}
static void test_push_dry_run(void) {
    char tmpl[] = "/tmp/nhp-tst-XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    char sub[512];
    snprintf(sub, sizeof sub, "%s/subdir", dir);
    mkdir(sub, 0700);

    char fp[512];
    snprintf(fp, sizeof fp, "%s/hello.txt", dir);
    assert(mkfile(fp, "hello world\n", 12) == 0);
    snprintf(fp, sizeof fp, "%s/subdir/inner.bin", dir);
    /* 5 MiB test — crosses one 4 MiB chunk boundary. */
    size_t big = 5u * 1024u * 1024u;
    char *buf = malloc(big);
    assert(buf);
    memset(buf, 0xa5, big);
    assert(mkfile(fp, buf, big) == 0);
    free(buf);

    char *out = NULL;
    size_t out_cap = 0;
    FILE *mem = open_memstream(&out, &out_cap);
    assert(mem);

    uint64_t tb = 0, tf = 0, tc = 0;
    int r = nh_prov_push_dry_run(dir, 4u * 1024u * 1024u, mem, &tb, &tf, &tc);
    fclose(mem);
    assert(r == 0);
    assert(tf == 2);                    /* hello.txt + inner.bin */
    assert(tb == 12 + big);
    assert(tc == 1 + 2);                /* 1 + 2 chunks */

    /* Emitted lines mention the paths. */
    assert(contains(out, "hello.txt"));
    assert(contains(out, "subdir/inner.bin"));
    assert(contains(out, "\"kind\":\"dir\""));

    free(out);

    /* Cleanup. */
    char rm[600];
    snprintf(rm, sizeof rm, "rm -rf -- %s", dir);
    (void)system(rm);
    fprintf(stderr, "OK  push_dry_run\n");
}

/* ── 6. fetch_ctl renderer roundtrip through nh_porthome_fetch_ctl_parse ── */
#include "porthome_fetch_ctl.h"
static void test_render_fetch_ctl(void) {
    nh_prov_account a;
    nh_prov_account_init(&a);
    memset(a.account_pubkey_hex, '0', 64); a.account_pubkey_hex[64] = 0;
    memset(a.root_id_hex,        '1', 64); a.root_id_hex[64] = 0;
    memset(a.home_key_hex,       '2', 64); a.home_key_hex[64] = 0;
    snprintf(a.home_relays[0], sizeof a.home_relays[0], "wss://example.org");
    a.n_home_relays = 1;
    snprintf(a.blossom_servers[0], sizeof a.blossom_servers[0],
             "https://example.org");
    a.n_blossom_servers = 1;

    size_t len = 0;
    char *ctl = nh_prov_render_fetch_ctl(&a, 5000, 0, 0, 0, 0, &len);
    assert(ctl);
    assert(len > 0);
    nh_porthome_fetch_ctl parsed = {0};
    nh_porthome_fetch_ctl_status ps =
        nh_porthome_fetch_ctl_parse(ctl, len, &parsed);
    if (ps != NH_PORTHOME_FETCH_CTL_OK) {
        fprintf(stderr, "parse err: %s\n", nh_porthome_fetch_ctl_strerror(ps));
        fprintf(stderr, "payload:\n%s\n", ctl);
    }
    assert(ps == NH_PORTHOME_FETCH_CTL_OK);
    assert(parsed.relays_count == 1);
    assert(strcmp(parsed.relays[0], "wss://example.org") == 0);
    assert(parsed.blossom_servers_count == 1);
    assert(parsed.relay_timeout_ms == 5000);
    free(ctl);
    fprintf(stderr, "OK  render_fetch_ctl\n");
}



/* ── 7. push_real_dry_shape: walk produces the expected manifest plan ── */
static void test_push_real_dry_shape(void) {
    char tmpl[] = "/tmp/nhp-real-XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    /* Tree: 3 files (small, medium, boundary), 1 subdir, 1 symlink. */
    char sub[512];
    snprintf(sub, sizeof sub, "%s/subdir", dir);
    mkdir(sub, 0700);

    char fp[512];
    snprintf(fp, sizeof fp, "%s/hello.txt", dir);
    assert(mkfile(fp, "hello world\n", 12) == 0);

    snprintf(fp, sizeof fp, "%s/subdir/inner.bin", dir);
    size_t big = 5u * 1024u * 1024u;
    char *buf = malloc(big);
    assert(buf);
    memset(buf, 0xa5, big);
    assert(mkfile(fp, buf, big) == 0);
    free(buf);

    /* Symlink relative — pointing at hello.txt. */
    char link[512]; snprintf(link, sizeof link, "%s/hello.link", dir);
    assert(symlink("hello.txt", link) == 0);

    nh_prov_walk_entry *entries = NULL;
    size_t n = 0, skipped = 0;
    int r = nh_prov_walk_home(dir, NULL, &entries, &n, &skipped);
    assert(r == 0);
    /* Expect at least: subdir, hello.txt, inner.bin, hello.link (5 entries) */
    assert(n >= 4);
    int seen_dir = 0, seen_symlink = 0, seen_file_big = 0;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].kind == NH_PROV_WALK_KIND_DIR &&
            strcmp(entries[i].rel_path, "subdir") == 0) seen_dir = 1;
        if (entries[i].kind == NH_PROV_WALK_KIND_SYMLINK &&
            strcmp(entries[i].rel_path, "hello.link") == 0) {
            assert(entries[i].symlink_target &&
                   strcmp(entries[i].symlink_target, "hello.txt") == 0);
            seen_symlink = 1;
        }
        if (entries[i].kind == NH_PROV_WALK_KIND_FILE &&
            strcmp(entries[i].rel_path, "subdir/inner.bin") == 0 &&
            entries[i].size == big) seen_file_big = 1;
    }
    assert(seen_dir && seen_symlink && seen_file_big);

    /* Chunk-size normalise: default 4 MiB, clamp low/high. */
    assert(nh_prov_normalize_chunk_size(0)       == 4u * 1024u * 1024u);
    assert(nh_prov_normalize_chunk_size(1)       == 64u * 1024u);
    assert(nh_prov_normalize_chunk_size(999)     == 64u * 1024u);
    assert(nh_prov_normalize_chunk_size(1u<<30)  == 8u * 1024u * 1024u);
    assert(nh_prov_normalize_chunk_size(1u<<20)  == 1u * 1024u * 1024u);

    /* Chunk-count math (5 MiB @ 4 MiB → 2). */
    uint64_t cs = nh_prov_normalize_chunk_size(0);
    uint64_t nc = (big + cs - 1) / cs;
    assert(nc == 2);

    nh_prov_free_walk(entries, n);
    char rm[600]; snprintf(rm, sizeof rm, "rm -rf -- %s", dir);
    (void)system(rm);
    fprintf(stderr, "OK  push_real_dry_shape\n");
}

/* ── 8. push_convergent_encryption — D4: same (home_key, pt) → same ct ── */
#include "nh_porthome_crypto.h"
static void test_push_convergent_encryption(void) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i ^ 0xA5);
    uint8_t home_key[32];
    assert(nh_porthome_key_derive(seed, home_key) == 0);

    /* Encrypt the same 1 MiB plaintext twice under the same home_key. */
    size_t n = 1u * 1024u * 1024u;
    uint8_t *pt = malloc(n);
    assert(pt);
    for (size_t i = 0; i < n; i++) pt[i] = (uint8_t)(i * 31u);

    uint8_t *ct1 = NULL, *ct2 = NULL;
    size_t   c1 = 0, c2 = 0;
    uint8_t  addr1[32], addr2[32];
    assert(nh_porthome_encrypt_chunk(home_key, pt, n, &ct1, &c1, addr1) == 0);
    assert(nh_porthome_encrypt_chunk(home_key, pt, n, &ct2, &c2, addr2) == 0);
    assert(c1 == c2);
    /* D4: byte-identical ciphertext + address (convergent). */
    assert(memcmp(ct1, ct2, c1) == 0);
    assert(memcmp(addr1, addr2, 32) == 0);
    /* Wire layout: version=0x01, then nonce(12), then ct, then tag(16). */
    assert(ct1[0] == 0x01);
    assert(c1 == n + 1 + 12 + 16);

    /* Different home_key → different ciphertext + address. */
    seed[0] ^= 0xff;
    uint8_t home_key_b[32];
    assert(nh_porthome_key_derive(seed, home_key_b) == 0);
    uint8_t *ct3 = NULL; size_t c3 = 0; uint8_t addr3[32];
    assert(nh_porthome_encrypt_chunk(home_key_b, pt, n, &ct3, &c3, addr3) == 0);
    assert(c3 == c1);
    assert(memcmp(ct1, ct3, c1) != 0);
    assert(memcmp(addr1, addr3, 32) != 0);

    free(ct1); free(ct2); free(ct3); free(pt);
    fprintf(stderr, "OK  push_convergent_encryption\n");
}

/* ── 9. push_min_replication_gate — synthetic per-server accounting ── */
static void test_push_min_replication_gate(void) {
    /* 3 servers, 10 chunks. Server 0 accepted all 10, servers 1 and 2
     * dropped some. Full-replica count = 1 → below default min=2. */
    uint32_t up[3]  = {10, 7, 0};
    uint32_t fail[3] = {0, 3, 10};
    size_t r = nh_prov_count_full_replicas(up, fail, 3, 10);
    assert(r == 1);

    /* Two full replicas → OK for min=2. */
    uint32_t up2[3]  = {10, 10, 4};
    uint32_t fail2[3] = {0, 0, 6};
    assert(nh_prov_count_full_replicas(up2, fail2, 3, 10) == 2);

    /* Every server holds a full replica. */
    uint32_t up3[3]  = {10, 10, 10};
    uint32_t fail3[3] = {0, 0, 0};
    assert(nh_prov_count_full_replicas(up3, fail3, 3, 10) == 3);

    /* n_chunks==0 (empty push) — vacuously OK if any server observed. */
    uint32_t up4[1] = {0};
    uint32_t fail4[1] = {0};
    assert(nh_prov_count_full_replicas(up4, fail4, 1, 0) == 1);

    fprintf(stderr, "OK  push_min_replication_gate\n");
}

int main(void) {
    test_account_round_trip();
    test_account_rejects_unknown_keys();
    test_account_rejects_schema();
    test_split_csv();
    test_push_dry_run();
    test_render_fetch_ctl();
    test_push_real_dry_shape();
    test_push_convergent_encryption();
    test_push_min_replication_gate();
    fprintf(stderr, "all tests OK\n");
    return 0;
}
