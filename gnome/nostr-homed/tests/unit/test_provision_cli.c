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

int main(void) {
    test_account_round_trip();
    test_account_rejects_unknown_keys();
    test_account_rejects_schema();
    test_split_csv();
    test_push_dry_run();
    test_render_fetch_ctl();
    fprintf(stderr, "all tests OK\n");
    return 0;
}
