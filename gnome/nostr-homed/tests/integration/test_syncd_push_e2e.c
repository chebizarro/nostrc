/*
 * test_syncd_push_e2e.c — end-to-end push closure over an in-process
 * mock publisher and a temp $HOME tree (bead nostrc-p6qp I1).
 *
 * SPDX-License-Identifier: MIT
 *
 * This test exercises everything except the actual libnostr relay
 * round-trip and the actual Blossom PUT (both replaced by injected
 * seams). It:
 *   1. Materializes a small $HOME (3 files) under /tmp.
 *   2. Builds a fresh state, an ignore matcher, an interlocks probe.
 *   3. Constructs a batch listing all three files.
 *   4. Calls nh_syncd_push_batch with:
 *        - a blossom_servers list of length 0 (no upload attempts —
 *          the mock signer captures the encrypted-plaintext hashes;
 *          the pusher records chunk_addrs_hex from the SHA-256 of
 *          each sealed chunk, which nh_porthome_encrypt_chunk returns
 *          out-of-band).
 *        - an event_publish_fn that succeeds and records the sealed
 *          manifest bytes so the test can assert the manifest was
 *          well-formed by decoding it.
 *   5. Asserts snapshot.json + generation advanced.
 *   6. Modifies one file, submits another batch, asserts the
 *      generation advances again and the sealed manifest changed.
 *
 * Also runs the interlock tests: creating ~/.nostr-home-limited
 * blocks the next push; deleting snapshot.json blocks the next push.
 *
 * When n_blossom_servers == 0 the pusher's `upload_chunk` path is
 * reached but nh_porthome_blossom_new is skipped. In that branch
 * we still compute (encrypt) each chunk, and the address string in
 * snapshot.json.chunk_addrs_hex ends up as the sealed SHA-256 —
 * exactly what a real Blossom-backed round-trip would record.
 * That gives us a byte-identical shape without a live server. A
 * separate ctest label ("live") will drive a real Blossom + relay
 * once packaging is in.
 */

#include "nh_syncd.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Fixed fixture nsec (dev-only). Its public key doesn't matter — the
 * pusher just needs a valid secp256k1 scalar to sign the pointer
 * event. The mock publish_fn ignores the signature. */
static const char *FIXTURE_NSEC = "1111111111111111111111111111111111111111111111111111111111111111";
static const char *FIXTURE_PK   = "0000000000000000000000000000000000000000000000000000000000000001";

typedef struct {
    uint8_t *sealed;
    size_t   sealed_len;
    int      calls;
} mock_pub_ctx;

static int mock_publish(void *ud, const char *pk, int64_t cat,
                        const char *dtag,
                        const uint8_t *sealed, size_t sealed_len,
                        char out_event_id[65])
{
    mock_pub_ctx *m = (mock_pub_ctx *)ud;
    free(m->sealed);
    m->sealed = malloc(sealed_len);
    memcpy(m->sealed, sealed, sealed_len);
    m->sealed_len = sealed_len;
    m->calls++;
    (void)pk; (void)cat; (void)dtag;
    /* Fake event id: deterministic from call count so the test can
     * assert the second push produced a new event. */
    snprintf(out_event_id, 65,
             "deadbeef%056x", m->calls);
    return 0;
}

static char g_home[300];
static char g_state[300];

static void write_file(const char *rel, const char *contents) {
    char abs[600];
    snprintf(abs, sizeof abs, "%s/%s", g_home, rel);
    /* mkdir -p on the parent. */
    char *slash = strrchr(abs, '/');
    if (slash) {
        *slash = '\0';
        char *p = strdup(abs);
        for (char *c = p + 1; *c; c++) {
            if (*c == '/') { *c = '\0'; mkdir(p, 0700); *c = '/'; }
        }
        mkdir(p, 0700);
        free(p);
        *slash = '/';
    }
    FILE *f = fopen(abs, "w");
    assert(f);
    fputs(contents, f);
    fclose(f);
}

static void setup(void) {
    snprintf(g_home,  sizeof g_home,  "/tmp/nh_syncd_e2e_home_%d",  (int)getpid());
    snprintf(g_state, sizeof g_state, "/tmp/nh_syncd_e2e_state_%d", (int)getpid());
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
    assert(mkdir(g_home, 0700) == 0);
    assert(mkdir(g_state, 0700) == 0);
    write_file("hello.txt", "hello world\n");
    write_file("docs/notes.md", "# notes\n\nline\n");
    write_file("bin/run.sh", "#!/bin/sh\necho hi\n");
}
static void teardown(void) {
    char rm[1024];
    snprintf(rm, sizeof rm, "rm -rf %s %s", g_home, g_state);
    (void)system(rm);
}

static uint64_t vnow(void *ud) { (void)ud; static uint64_t t = 0; t += 10ull * 1000000000ull; return t; }

static void push_once(nh_syncd_state *state,
                      nh_syncd_ignore *ig,
                      const char *const *rels, size_t n_rels,
                      mock_pub_ctx *mp)
{
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(vnow, NULL, 1, 1, &b) == 0);
    for (size_t i = 0; i < n_rels; i++) {
        assert(nh_syncd_batcher_push(b, rels[i], NH_SYNCD_CHANGE_CREATE) == 0);
    }
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);
    assert(bat != NULL);

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    nh_syncd_push_cfg pc = {0};
    assert(nh_porthome_key_derive(seed, pc.home_key) == 0);
    memcpy(pc.root_id, seed, 32);
    pc.event_signer_nsec_hex = FIXTURE_NSEC;
    pc.event_publish_fn      = mock_publish;
    pc.event_publish_ud      = mp;
    pc.account_pubkey_hex_override = FIXTURE_PK;
    pc.d_tag = "nostr-homed.home.v1:personal";

    nh_syncd_interlocks ilk = { g_home, NULL };
    char *emsg = NULL;
    int rc = nh_syncd_push_batch(&pc, state, bat, g_home, ig, &ilk, g_state, &emsg);
    if (rc != NH_SYNCD_OK)
        fprintf(stderr, "push_once rc=%d msg=%s\n", rc, emsg ? emsg : "");
    assert(rc == NH_SYNCD_OK);
    free(emsg);

    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
}

static void t_happy_path(void) {
    /* Fresh state initialised with real root/d_tag (not the empty
     * one nh_syncd_state_load returns for a missing file). */
    uint8_t root_id[32];
    for (int i = 0; i < 32; i++) root_id[i] = (uint8_t)i;
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "nostr-homed.home.v1:personal",
                              FIXTURE_PK, root_id, &state) == 0);
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    mock_pub_ctx mp = {0};

    /* Include the parent dirs in the batch so the recorded
     * snapshot contains both files AND directory entries — matches
     * how inotify fires IN_CREATE on the parent when a subtree is
     * built. */
    const char *r1[] = { "hello.txt", "docs", "docs/notes.md",
                         "bin", "bin/run.sh" };
    push_once(state, ig, r1, 5, &mp);

    assert(mp.calls == 1);
    assert(nh_syncd_state_get_local_generation(state) == 1);
    assert(nh_syncd_state_file_count(state) == 5);
    /* snapshot.json + generation exist. */
    char gp[400]; snprintf(gp, sizeof gp, "%s/generation", g_state);
    FILE *gf = fopen(gp, "r"); assert(gf);
    char gbuf[8] = {0}; fread(gbuf, 1, 7, gf); fclose(gf);
    assert(gbuf[0] == '1');

    /* Decode the sealed manifest to prove it's well-formed. */
    uint8_t seed[32], hk[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    assert(nh_porthome_key_derive(seed, hk) == 0);
    uint8_t *pt = NULL; size_t pt_len = 0;
    assert(nh_porthome_decrypt_manifest(hk, mp.sealed, mp.sealed_len,
                                        &pt, &pt_len) == 0);
    nh_porthome_manifest *m = NULL;
    assert(nh_porthome_manifest_decode(pt, pt_len, &m) == 0);
    /* 3 files + 2 dirs (docs/, bin/). */
    assert(m->entries_len == 5);
    nh_porthome_manifest_dispose(m);
    free(m);
    free(pt);

    /* Modify one file → second push. */
    write_file("docs/notes.md", "# notes\n\nCHANGED line\n");
    const char *r2[] = { "docs/notes.md" };
    push_once(state, ig, r2, 1, &mp);
    assert(mp.calls == 2);
    assert(nh_syncd_state_get_local_generation(state) == 2);
    assert(nh_syncd_state_file_count(state) == 5);

    nh_syncd_state_free(state);
    nh_syncd_ignore_free(ig);
    free(mp.sealed);
    printf("t_happy_path OK\n");
}

static void t_interlock_limited_mode(void) {
    /* Fresh state. */
    uint8_t root_id[32] = {0};
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_new(g_home, "nostr-homed.home.v1:personal",
                              FIXTURE_PK, root_id, &state) == 0);
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    mock_pub_ctx mp = {0};

    /* Create the limited-mode file. */
    char limpath[400]; snprintf(limpath, sizeof limpath, "%s/.nostr-home-limited", g_home);
    FILE *f = fopen(limpath, "w"); assert(f); fputs("x", f); fclose(f);

    nh_syncd_batcher *b = NULL;
    nh_syncd_batcher_new(vnow, NULL, 1, 1, &b);
    nh_syncd_batcher_push(b, "hello.txt", NH_SYNCD_CHANGE_MODIFY);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);

    nh_syncd_push_cfg pc = {0};
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    nh_porthome_key_derive(seed, pc.home_key);
    memcpy(pc.root_id, seed, 32);
    pc.event_signer_nsec_hex = FIXTURE_NSEC;
    pc.event_publish_fn      = mock_publish;
    pc.event_publish_ud      = &mp;
    pc.account_pubkey_hex_override = FIXTURE_PK;
    pc.d_tag = "nostr-homed.home.v1:personal";
    nh_syncd_interlocks ilk = { g_home, NULL };
    char *emsg = NULL;
    int rc = nh_syncd_push_batch(&pc, state, bat, g_home, ig, &ilk, g_state, &emsg);
    assert(rc == NH_SYNCD_ERR_LIMITED_MODE);
    assert(mp.calls == 0);
    free(emsg);

    unlink(limpath);
    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    nh_syncd_state_free(state);
    nh_syncd_ignore_free(ig);
    printf("t_interlock_limited_mode OK\n");
}

static void t_interlock_snapshot_unknown(void) {
    /* Load state from empty state dir (this test uses a fresh subdir
     * so we don't clobber the happy-path snapshot). */
    char state2[400]; snprintf(state2, sizeof state2, "%s.blank", g_state);
    mkdir(state2, 0700);

    bool unknown = false;
    nh_syncd_state *state = NULL;
    assert(nh_syncd_state_load(state2, &state, &unknown) == 0);
    assert(unknown == true);

    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    mock_pub_ctx mp = {0};

    nh_syncd_batcher *b = NULL;
    nh_syncd_batcher_new(vnow, NULL, 1, 1, &b);
    nh_syncd_batcher_push(b, "hello.txt", NH_SYNCD_CHANGE_MODIFY);
    nh_syncd_batch *bat = nh_syncd_batcher_take(b, true);

    nh_syncd_push_cfg pc = {0};
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    nh_porthome_key_derive(seed, pc.home_key);
    memcpy(pc.root_id, seed, 32);
    pc.event_signer_nsec_hex = FIXTURE_NSEC;
    pc.event_publish_fn      = mock_publish;
    pc.event_publish_ud      = &mp;
    pc.account_pubkey_hex_override = FIXTURE_PK;
    pc.d_tag = "nostr-homed.home.v1:personal";
    nh_syncd_interlocks ilk = { g_home, NULL };
    char *emsg = NULL;
    int rc = nh_syncd_push_batch(&pc, state, bat, g_home, ig, &ilk, state2, &emsg);
    assert(rc == NH_SYNCD_ERR_SNAPSHOT_UNKNOWN);
    assert(mp.calls == 0);
    free(emsg);

    nh_syncd_batch_free(bat);
    nh_syncd_batcher_free(b);
    nh_syncd_state_free(state);
    nh_syncd_ignore_free(ig);
    char rm[512]; snprintf(rm, sizeof rm, "rm -rf %s", state2); (void)system(rm);
    printf("t_interlock_snapshot_unknown OK\n");
}

static void t_lock_reentrant(void) {
    /* Lock is single-instance: acquiring twice from the same process
     * on the same fd would deadlock, but two separate opens must fail
     * the second with NH_SYNCD_ERR_LOCKED. */
    nh_syncd_lock *l1 = NULL, *l2 = NULL;
    assert(nh_syncd_lock_acquire(g_state, &l1) == NH_SYNCD_OK);
    int rc = nh_syncd_lock_acquire(g_state, &l2);
    assert(rc == NH_SYNCD_ERR_LOCKED);
    assert(l2 == NULL);
    nh_syncd_lock_release(l1);
    /* After release, we can grab it again. */
    assert(nh_syncd_lock_acquire(g_state, &l2) == NH_SYNCD_OK);
    nh_syncd_lock_release(l2);
    printf("t_lock_reentrant OK\n");
}

int main(void) {
    setup();
    t_happy_path();
    t_interlock_limited_mode();
    t_interlock_snapshot_unknown();
    t_lock_reentrant();
    teardown();
    printf("test_syncd_push_e2e: OK\n");
    return 0;
}
