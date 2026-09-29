/*
 * libmarmot - Secret-tree ratchet persistence (nostrc-ai04)
 *
 * Every Marmot operation loads the MLS state, uses it and stores it again.
 * These tests put a save/load (mls_group_serialize/deserialize) between
 * every step, as the API does, and check RFC 9420 section 9:
 *   - a sender never uses a generation twice, across reloads (9.1);
 *   - a receiver never decrypts a generation twice, across reloads;
 *   - skipped keys are kept only inside the out-of-order window, and a
 *     forward jump is bounded (15.3); outside either it fails closed;
 *   - the stored state holds no consumed value (9.2): no encryption_secret,
 *     joiner or welcome secret, used leaf or ratchet secret, or used key;
 *   - a failed decryption consumes nothing;
 *   - format 1 and 2 states load: the own sender moves past everything
 *     they may have used, receivers accept a bounded window;
 *   - malformed states are refused.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls/mls_group.h"
#include "mls/mls_welcome.h"
#include "mls/mls_framing.h"
#include "mls/mls_key_schedule.h"
#include "mls/mls_tree.h"
#include "mls/mls-internal.h"
#include <marmot/marmot-error.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "\n  FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
            abort();                                                        \
        }                                                                   \
    } while (0)

#define RUN(fn) do { printf("  %-56s", #fn); fflush(stdout); fn(); printf("PASS\n"); } while (0)

#define WINDOW MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS

static const uint8_t ALICE_ID[32] = {
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
    0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1,
};
static const uint8_t BOB_ID[32] = {
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
    0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0, 0xB0,
};
static const uint8_t GROUP_ID[] = "ratchet-persist-group";

/* ── Fixture: Alice (leaf 0) and Bob (leaf 1), in memory ──────────────── */

typedef struct {
    MlsGroup alice, bob;
    MlsKeyPackage bob_kp;
    MlsKeyPackagePrivate bob_priv;
} Pair;

static void
pair_init(Pair *p)
{
    memset(p, 0, sizeof(*p));
    uint8_t sk[MLS_SIG_SK_LEN], pk[MLS_SIG_PK_LEN];
    CHECK(mls_crypto_sign_keygen(sk, pk) == 0, "keygen");
    CHECK(mls_group_create(&p->alice, GROUP_ID, sizeof(GROUP_ID), ALICE_ID, 32, sk,
                           NULL, 0) == 0, "create");
    sodium_memzero(sk, sizeof(sk));
    CHECK(mls_key_package_create(&p->bob_kp, &p->bob_priv, BOB_ID, 32, NULL, 0) == 0,
          "key package");
    MlsAddResult add;
    CHECK(mls_group_add_member(&p->alice, &p->bob_kp, &add) == 0, "add");
    CHECK(mls_welcome_process(add.welcome_data, add.welcome_len, &p->bob_kp,
                              &p->bob_priv, NULL, 0, &p->bob) == 0, "welcome");
    mls_add_result_clear(&add);
    CHECK(p->alice.own_leaf_index == 0 && p->bob.own_leaf_index == 1, "leaves");
}

static void
pair_clear(Pair *p)
{
    mls_group_free(&p->alice);
    mls_group_free(&p->bob);
    mls_key_package_clear(&p->bob_kp);
    mls_key_package_private_clear(&p->bob_priv);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void
free_secret(uint8_t *p, size_t len)
{
    if (!p) return;
    sodium_memzero(p, len);
    free(p);
}

static uint8_t *
save(const MlsGroup *g, size_t *len)
{
    uint8_t *blob = NULL;
    CHECK(mls_group_serialize(g, &blob, len) == 0, "serialize");
    return blob;
}

/* Store `g` and load it again in place: what every Marmot operation does. */
static void
reload(MlsGroup *g)
{
    size_t len = 0;
    uint8_t *blob = save(g, &len);
    mls_group_free(g);
    CHECK(mls_group_deserialize(blob, len, g) == 0, "deserialize");
    free_secret(blob, len);
}

static uint8_t *
send_msg(MlsGroup *g, const char *text, size_t *len)
{
    uint8_t *ct = NULL;
    CHECK(mls_group_encrypt(g, (const uint8_t *)text, strlen(text), &ct, len) == 0,
          "encrypt '%s'", text);
    return ct;
}

/* The sender data of `ct` (read with `g`'s epoch secrets). */
static MlsSenderData
sender_of(const MlsGroup *g, const uint8_t *ct, size_t len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, ct, len);
    MlsMLSMessage wire;
    CHECK(mls_message_deserialize(&r, &wire) == 0 &&
          wire.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE, "PrivateMessage");
    const MlsPrivateMessage *pm = &wire.private_message;
    size_t sample = pm->ciphertext_len < MLS_HASH_LEN ? pm->ciphertext_len : MLS_HASH_LEN;
    MlsSenderData sd;
    const MlsSenderDataAAD sd_aad = { pm->group_id, pm->group_id_len, pm->epoch, pm->content_type };
    CHECK(mls_sender_data_decrypt(g->epoch_secrets.sender_data_secret, &sd_aad, pm->ciphertext,
                                  sample, pm->encrypted_sender_data,
                                  pm->encrypted_sender_data_len, &sd) == 0, "sender data");
    mls_message_clear(&wire);
    return sd;
}

/* mls_group_decrypt's result; on success the plaintext must be `expect`. */
static int
recv_msg(MlsGroup *g, const uint8_t *ct, size_t len, const char *expect)
{
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    uint32_t sender = UINT32_MAX;
    int rc = mls_group_decrypt(g, ct, len, &pt, &pt_len, &sender);
    if (rc == 0) {
        CHECK(pt_len == strlen(expect) && memcmp(pt, expect, pt_len) == 0,
              "plaintext of '%s'", expect);
        free(pt);
    }
    return rc;
}

/* A rejected message leaves the stored state byte for byte as it was. */
static void
expect_rejected_unchanged(MlsGroup *g, const uint8_t *ct, size_t len, const char *what)
{
    size_t before_len = 0, after_len = 0;
    uint8_t *before = save(g, &before_len);
    CHECK(recv_msg(g, ct, len, "") != 0, "%s: accepted", what);
    uint8_t *after = save(g, &after_len);
    CHECK(before_len == after_len && memcmp(before, after, before_len) == 0,
          "%s: the rejection changed the state", what);
    free_secret(before, before_len);
    free_secret(after, after_len);
}

static bool
contains(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t n)
{
    for (size_t i = 0; n <= hay_len && i <= hay_len - n; i++)
        if (memcmp(hay + i, needle, n) == 0) return true;
    return false;
}

static size_t
count_of(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; n <= hay_len && i <= hay_len - n; i++)
        if (memcmp(hay + i, needle, n) == 0) c++;
    return c;
}

static uint32_t
cached_keys(const MlsGroup *g, uint32_t leaf)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < WINDOW; i++)
        n += g->secret_tree.senders[leaf].application_skipped[i].valid ? 1 : 0;
    return n;
}

/* The epoch's encryption_secret, recomputed from the joiner secret the
 * group still holds in memory (it is never stored) and the GroupContext:
 * member = Extract(joiner, 0); epoch = ExpandWithLabel(member, "epoch", GC);
 * encryption = DeriveSecret(epoch, "encryption") (RFC 9420 section 8). */
static void
encryption_secret_of(const MlsGroup *g, uint8_t out[MLS_HASH_LEN])
{
    uint8_t zero[MLS_HASH_LEN] = {0}, member[MLS_HASH_LEN], epoch[MLS_HASH_LEN];
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    CHECK(mls_group_context_build(g, &gc, &gc_len) == 0, "context");
    CHECK(mls_crypto_hkdf_extract(member, g->epoch_secrets.joiner_secret, MLS_HASH_LEN,
                                  zero, MLS_HASH_LEN) == 0 &&
          mls_crypto_expand_with_label(epoch, MLS_HASH_LEN, member, "epoch", gc,
                                       gc_len) == 0 &&
          mls_crypto_derive_secret(out, epoch, "encryption") == 0, "key schedule");
    free(gc);
    sodium_memzero(member, sizeof(member));
    sodium_memzero(epoch, sizeof(epoch));
}

/* ── Tests ────────────────────────────────────────────────────────────── */

/* Each send uses the next generation although the state is stored and
 * loaded in between (was: generation 0 every time, nostrc-ai04). */
static void
test_sender_generations_survive_reload(void)
{
    Pair p;
    pair_init(&p);
    enum { N = 6 };
    uint8_t *ct[N];
    size_t len[N];
    for (uint32_t i = 0; i < N; i++) {
        reload(&p.alice);
        char text[16];
        snprintf(text, sizeof(text), "send-%u", i);
        ct[i] = send_msg(&p.alice, text, &len[i]);
        MlsSenderData sd = sender_of(&p.alice, ct[i], len[i]);
        CHECK(sd.leaf_index == 0 && sd.generation == i,
              "send %u used generation %u", i, sd.generation);
    }
    reload(&p.alice);
    CHECK(p.alice.secret_tree.senders[0].application_generation == N, "stored head");
    for (uint32_t i = 0; i < N; i++) {
        reload(&p.bob);
        char text[16];
        snprintf(text, sizeof(text), "send-%u", i);
        CHECK(recv_msg(&p.bob, ct[i], len[i], text) == 0, "Bob reads %u", i);
        free(ct[i]);
    }
    pair_clear(&p);
}

/* A decrypted generation never decrypts again, however often the state is
 * stored and loaded; a skipped one decrypts once, from the stored cache. */
static void
test_replay_rejected_across_reloads(void)
{
    Pair p;
    pair_init(&p);
    size_t l0, l1, l2;
    uint8_t *m0 = send_msg(&p.alice, "m0", &l0);
    uint8_t *m1 = send_msg(&p.alice, "m1", &l1);
    uint8_t *m2 = send_msg(&p.alice, "m2", &l2);

    CHECK(recv_msg(&p.bob, m2, l2, "m2") == 0, "m2");
    reload(&p.bob);
    CHECK(cached_keys(&p.bob, 0) == 2, "generations 0 and 1 kept");
    CHECK(recv_msg(&p.bob, m0, l0, "m0") == 0, "m0 from the stored cache");
    reload(&p.bob);
    expect_rejected_unchanged(&p.bob, m0, l0, "m0 replayed after a reload");
    expect_rejected_unchanged(&p.bob, m2, l2, "m2 replayed after a reload");
    reload(&p.bob);
    CHECK(recv_msg(&p.bob, m1, l1, "m1") == 0, "m1");
    reload(&p.bob);
    expect_rejected_unchanged(&p.bob, m1, l1, "m1 replayed after a reload");
    CHECK(cached_keys(&p.bob, 0) == 0, "nothing left to use");
    free(m0);
    free(m1);
    free(m2);
    pair_clear(&p);
}

/* Out-of-order delivery inside the window works (from the stored cache);
 * older generations and forward jumps past max_forward_distance fail closed
 * and change nothing. */
static void
test_out_of_order_window(void)
{
    Pair p;
    pair_init(&p);
    enum { N = WINDOW + 8 };
    uint8_t *ct[N];
    size_t len[N];
    char text[N][16];
    for (uint32_t i = 0; i < N; i++) {
        snprintf(text[i], sizeof(text[i]), "ooo-%u", i);
        ct[i] = send_msg(&p.alice, text[i], &len[i]);
    }

    /* The last one first: the window is then the WINDOW generations below
     * it, [N - 1 - WINDOW, N - 1). */
    CHECK(recv_msg(&p.bob, ct[N - 1], len[N - 1], text[N - 1]) == 0, "newest");
    reload(&p.bob);
    CHECK(cached_keys(&p.bob, 0) == WINDOW, "cache bounded: %u", cached_keys(&p.bob, 0));
    const uint32_t floor = N - 1 - WINDOW;
    for (uint32_t g = 0; g < floor; g++)
        expect_rejected_unchanged(&p.bob, ct[g], len[g], "older than the window");
    CHECK(recv_msg(&p.bob, ct[floor], len[floor], text[floor]) == 0, "window floor");
    reload(&p.bob);
    CHECK(recv_msg(&p.bob, ct[N - 2], len[N - 2], text[N - 2]) == 0, "just below the head");
    /* The rest in reverse, reloading every few; floor + 1 stays unread. */
    for (uint32_t g = N - 3; g > floor + 1; g--) {
        if (g % 4 == 0) reload(&p.bob);
        CHECK(recv_msg(&p.bob, ct[g], len[g], text[g]) == 0, "in window: %u", g);
    }
    reload(&p.bob);
    for (uint32_t g = floor; g < N; g++)
        if (g != floor + 1)
            expect_rejected_unchanged(&p.bob, ct[g], len[g], "replay in the window");

    /* Forward: exactly max_forward_distance ahead is accepted ... */
    CHECK(p.bob.max_forward_distance == 1000, "default forward distance");
    CHECK(mls_secret_tree_skip(&p.alice.secret_tree, 0, 1000) == 0, "skip");
    size_t far_len = 0;
    uint8_t *far = send_msg(&p.alice, "far", &far_len);
    CHECK(sender_of(&p.alice, far, far_len).generation == N + 1000, "far generation");
    CHECK(recv_msg(&p.bob, far, far_len, "far") == 0, "a jump of max_forward_distance");
    reload(&p.bob);
    /* ... and moved the window: the unread floor + 1 left it. */
    expect_rejected_unchanged(&p.bob, ct[floor + 1], len[floor + 1], "evicted by the jump");
    CHECK(cached_keys(&p.bob, 0) == WINDOW, "cache still bounded");
    /* ... one more is not, and changes nothing. */
    CHECK(mls_secret_tree_skip(&p.alice.secret_tree, 0, 1001) == 0, "skip");
    size_t too_far_len = 0;
    uint8_t *too_far = send_msg(&p.alice, "too far", &too_far_len);
    expect_rejected_unchanged(&p.bob, too_far, too_far_len, "past max_forward_distance");

    free(far);
    free(too_far);
    for (uint32_t i = 0; i < N; i++) free(ct[i]);
    pair_clear(&p);
}

/* A message that fails to authenticate (a forgery by a member holding the
 * sender-data secret, or corruption) consumes nothing: not the generation
 * it names, not the window. */
static void
test_failed_decryption_consumes_nothing(void)
{
    Pair p;
    pair_init(&p);
    size_t l0, l1, l5;
    uint8_t *m0 = send_msg(&p.alice, "m0", &l0);
    uint8_t *m1 = send_msg(&p.alice, "m1", &l1);
    CHECK(mls_secret_tree_skip(&p.alice.secret_tree, 0, 3) == 0, "skip");
    uint8_t *m5 = send_msg(&p.alice, "m5", &l5);
    CHECK(recv_msg(&p.bob, m0, l0, "m0") == 0, "m0");

    uint8_t *bad = malloc(l5);
    CHECK(bad, "alloc");
    memcpy(bad, m5, l5);
    bad[l5 - 1] ^= 0x01;   /* the AEAD tag */
    expect_rejected_unchanged(&p.bob, bad, l5, "tampered generation 5");
    CHECK(recv_msg(&p.bob, m1, l1, "m1") == 0, "m1 still in order");
    CHECK(recv_msg(&p.bob, m5, l5, "m5") == 0, "the genuine generation 5");
    free(bad);
    free(m0);
    free(m1);
    free(m5);
    pair_clear(&p);
}

/* `ct` with the last byte of its content ciphertext (the AEAD tag) flipped,
 * re-serialized.  The ciphertext is longer than the sender-data sample
 * (KDF.Nh bytes), so the sender data still decrypts: the receiver derives
 * the key of the message's generation before the AEAD fails. */
static uint8_t *
tamper_content_tag(const uint8_t *ct, size_t len, size_t *out_len)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, ct, len);
    MlsMLSMessage wire;
    CHECK(mls_message_deserialize(&r, &wire) == 0 &&
          wire.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE, "PrivateMessage");
    MlsPrivateMessage *pm = &wire.private_message;
    CHECK(pm->ciphertext_len > MLS_HASH_LEN, "content longer than the sample");
    pm->ciphertext[pm->ciphertext_len - 1] ^= 0x01;
    MlsTlsBuf buf;
    CHECK(mls_tls_buf_init(&buf, len) == 0 && mls_message_serialize(&wire, &buf) == 0,
          "serialize");
    mls_message_clear(&wire);
    *out_len = buf.len;
    return buf.data;
}

/* Review N2: a message whose sender data is intact but whose content tag
 * is broken makes the receiver derive the keys up to its generation, then
 * fails; the sender's ratchet is put back exactly (nothing cached, nothing
 * skipped), so the genuine message and the ones before it still decrypt.
 * Also for a message that decrypts but whose PrivateMessageContent
 * signature is not the sender leaf's (nostrc-we6g). */
static void
test_failed_message_restores_ratchet(void)
{
    Pair p;
    pair_init(&p);
    size_t l0, l1, l5;
    uint8_t *m0 = send_msg(&p.alice, "restore m0", &l0);
    uint8_t *m1 = send_msg(&p.alice, "restore m1", &l1);
    CHECK(mls_secret_tree_skip(&p.alice.secret_tree, 0, 3) == 0, "skip");
    /* A copy of Alice's state at generation 5 signing with another key. */
    MlsGroup forger;
    {
        size_t n = 0;
        uint8_t *blob = save(&p.alice, &n);
        CHECK(mls_group_deserialize(blob, n, &forger) == 0, "clone");
        free_secret(blob, n);
        uint8_t pk[crypto_sign_PUBLICKEYBYTES];
        crypto_sign_keypair(pk, forger.own_signature_key);
    }
    uint8_t *m5 = send_msg(&p.alice, "restore m5", &l5);
    size_t lf = 0;
    uint8_t *forged5 = send_msg(&forger, "restore m5", &lf);
    CHECK(recv_msg(&p.bob, m0, l0, "restore m0") == 0, "m0");
    reload(&p.bob);

    size_t lb = 0;
    uint8_t *bad = tamper_content_tag(m5, l5, &lb);
    CHECK(sender_of(&p.bob, bad, lb).generation == 5, "the sender data still decrypts");
    const MlsSenderRatchet *r = &p.bob.secret_tree.senders[0];
    expect_rejected_unchanged(&p.bob, bad, lb, "content tag broken");
    CHECK(r->application_generation == 1 && cached_keys(&p.bob, 0) == 0,
          "ratchet as before: next %u, %u cached", r->application_generation,
          cached_keys(&p.bob, 0));

    CHECK(sender_of(&p.bob, forged5, lf).generation == 5, "forgery at generation 5");
    expect_rejected_unchanged(&p.bob, forged5, lf, "signed by another key");
    CHECK(r->application_generation == 1 && cached_keys(&p.bob, 0) == 0,
          "ratchet as before the forgery");

    CHECK(recv_msg(&p.bob, m5, l5, "restore m5") == 0, "the genuine generation 5");
    CHECK(recv_msg(&p.bob, m1, l1, "restore m1") == 0, "generation 1 from the cache");
    free(bad);
    free(m0);
    free(m1);
    free(m5);
    free(forged5);
    mls_group_free(&forger);
    pair_clear(&p);
}

/* RFC 9420 section 9.2: the live stored state holds no consumed value, so
 * it alone cannot decrypt a message already sent or received.  (The
 * retained parent plus the public Commit can derive the epoch again; that
 * exposure is documented, review B1, nostrc-yuj2.) */
static void
test_consumed_secrets_not_stored(void)
{
    Pair p;
    pair_init(&p);
    uint8_t enc[MLS_HASH_LEN];
    encryption_secret_of(&p.alice, enc);
    static const uint8_t zero[MLS_HASH_LEN] = {0};
    CHECK(sodium_memcmp(p.alice.epoch_secrets.encryption_secret, zero, MLS_HASH_LEN) == 0 &&
          sodium_memcmp(p.bob.epoch_secrets.encryption_secret, zero, MLS_HASH_LEN) == 0,
          "the tree's root is deleted in memory");
    /* The committer keeps the joiner and welcome secrets for the Welcome it
     * builds in that operation (never stored); the joiner wipes them. */
    CHECK(sodium_memcmp(p.bob.epoch_secrets.joiner_secret, zero, MLS_HASH_LEN) == 0 &&
          sodium_memcmp(p.bob.epoch_secrets.welcome_secret, zero, MLS_HASH_LEN) == 0,
          "the joiner deleted its joiner and welcome secrets");

    size_t l0, l1, l2;
    uint8_t *m0 = send_msg(&p.alice, "m0", &l0);
    uint8_t *m1 = send_msg(&p.alice, "m1", &l1);
    uint8_t *m2 = send_msg(&p.alice, "m2", &l2);
    CHECK(recv_msg(&p.bob, m2, l2, "m2") == 0 && recv_msg(&p.bob, m0, l0, "m0") == 0,
          "Bob reads 2 then 0");

    /* The values of leaf 0 (Alice) the two states went through. */
    uint8_t leaf0[MLS_HASH_LEN], leaf1[MLS_HASH_LEN];
    static const uint8_t left[] = "left", right[] = "right";
    CHECK(mls_crypto_expand_with_label(leaf0, MLS_HASH_LEN, enc, "tree", left, 4) == 0 &&
          mls_crypto_expand_with_label(leaf1, MLS_HASH_LEN, enc, "tree", right, 5) == 0,
          "leaf secrets");
    uint8_t hs0[MLS_HASH_LEN], app[4][MLS_HASH_LEN];
    uint8_t key[3][MLS_AEAD_KEY_LEN], nonce[3][MLS_AEAD_NONCE_LEN];
    CHECK(mls_crypto_expand_with_label(hs0, MLS_HASH_LEN, leaf0, "handshake", NULL, 0) == 0 &&
          mls_crypto_expand_with_label(app[0], MLS_HASH_LEN, leaf0, "application", NULL, 0) == 0,
          "ratchet roots");
    for (uint32_t g = 0; g < 3; g++) {
        CHECK(mls_crypto_derive_tree_secret(key[g], MLS_AEAD_KEY_LEN, app[g], "key", g) == 0 &&
              mls_crypto_derive_tree_secret(nonce[g], MLS_AEAD_NONCE_LEN, app[g], "nonce", g) == 0 &&
              mls_crypto_derive_tree_secret(app[g + 1], MLS_HASH_LEN, app[g], "secret", g) == 0,
              "generation %u", g);
    }

    size_t la = 0, lb = 0;
    uint8_t *sa = save(&p.alice, &la);
    uint8_t *sb = save(&p.bob, &lb);
    const struct { const uint8_t *v; size_t n; const char *what; } consumed[] = {
        { enc, MLS_HASH_LEN, "encryption_secret" },
        { p.alice.epoch_secrets.joiner_secret, MLS_HASH_LEN, "joiner_secret" },
        { p.alice.epoch_secrets.welcome_secret, MLS_HASH_LEN, "welcome_secret" },
        { leaf0, MLS_HASH_LEN, "leaf secret of a started sender" },
        { app[0], MLS_HASH_LEN, "application ratchet secret 0" },
        { app[1], MLS_HASH_LEN, "application ratchet secret 1" },
        { app[2], MLS_HASH_LEN, "application ratchet secret 2" },
        { key[0], MLS_AEAD_KEY_LEN, "key 0" },
        { key[2], MLS_AEAD_KEY_LEN, "key 2" },
        { nonce[0], MLS_AEAD_NONCE_LEN, "nonce 0" },
        { nonce[2], MLS_AEAD_NONCE_LEN, "nonce 2" },
    };
    for (size_t i = 0; i < sizeof(consumed) / sizeof(consumed[0]); i++) {
        CHECK(!contains(sa, la, consumed[i].v, consumed[i].n), "Alice stores the %s",
              consumed[i].what);
        CHECK(!contains(sb, lb, consumed[i].v, consumed[i].n), "Bob stores the %s",
              consumed[i].what);
    }
    /* Alice used key 1; Bob skipped it, so he keeps it, and only it. */
    CHECK(!contains(sa, la, key[1], MLS_AEAD_KEY_LEN), "Alice stores her used key 1");
    CHECK(contains(sb, lb, key[1], MLS_AEAD_KEY_LEN) &&
          contains(sb, lb, nonce[1], MLS_AEAD_NONCE_LEN), "Bob keeps skipped key 1");
    /* What is still to be used is there: the heads and Bob's unused leaf. */
    CHECK(contains(sa, la, app[3], MLS_HASH_LEN) && contains(sb, lb, app[3], MLS_HASH_LEN),
          "application head");
    CHECK(contains(sa, la, hs0, MLS_HASH_LEN) && contains(sb, lb, hs0, MLS_HASH_LEN),
          "unused handshake chain");
    CHECK(contains(sa, la, leaf1, MLS_HASH_LEN) && contains(sb, lb, leaf1, MLS_HASH_LEN),
          "Bob's leaf, not started");
    free_secret(sb, lb);

    /* Once Bob reads generation 1, its key goes too. */
    CHECK(recv_msg(&p.bob, m1, l1, "m1") == 0, "m1");
    sb = save(&p.bob, &lb);
    CHECK(!contains(sb, lb, key[1], MLS_AEAD_KEY_LEN), "Bob stores his used key 1");

    free_secret(sa, la);
    free_secret(sb, lb);
    sodium_memzero(enc, sizeof(enc));
    sodium_memzero(leaf0, sizeof(leaf0));
    sodium_memzero(app, sizeof(app));
    sodium_memzero(key, sizeof(key));
    free(m0);
    free(m1);
    free(m2);
    pair_clear(&p);
}

/* A chain gives out each generation once, up to MLS_RATCHET_GENERATION_MAX:
 * the counter never wraps to 0; a receiver refuses the generation after. */
static void
test_exhausted_chain_never_wraps(void)
{
    uint8_t enc[MLS_HASH_LEN];
    memset(enc, 0x5c, sizeof(enc));
    MlsSecretTree st;
    CHECK(mls_secret_tree_init(&st, enc, 2) == 0, "init");
    MlsMessageKeys k;
    CHECK(mls_secret_tree_derive_keys(&st, 0, false, &k) == 0 && k.generation == 0, "g0");
    st.senders[0].application_generation = MLS_RATCHET_GENERATION_MAX;
    CHECK(mls_secret_tree_derive_keys(&st, 0, false, &k) == 0 &&
          k.generation == MLS_RATCHET_GENERATION_MAX, "the last generation");
    CHECK(mls_secret_tree_derive_keys(&st, 0, false, &k) != 0, "exhausted");
    CHECK(st.senders[0].application_generation == UINT32_MAX, "no wrap");
    CHECK(mls_secret_tree_get_keys_for_generation(&st, 1, false, UINT32_MAX, UINT32_MAX,
                                                  &k) != 0, "past the last generation");
    mls_secret_tree_free(&st);
    sodium_memzero(&k, sizeof(k));
}

/* ── Format 1/2 states (before nostrc-ai04) ───────────────────────────── */

static void
put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* `g` as libmarmot 0.7 stored it (format `version`, 1 or 2): the same
 * layout with all eleven epoch secrets -- encryption_secret (`enc`), welcome
 * and joiner secret (from the committer `old`, which still holds them; 0.7
 * stored the same values for every member) included -- and no secret tree;
 * format 1 also without the trailing PSK and path-key caches.  Built from
 * the format 3 bytes. */
static uint8_t *
legacy_blob(const MlsGroup *g, const MlsGroup *old, const uint8_t enc[MLS_HASH_LEN],
            uint32_t version, size_t *out_len)
{
    size_t v3_len = 0;
    uint8_t *v3 = save(g, &v3_len);
    MlsTlsBuf tree;
    CHECK(mls_tls_buf_init(&tree, 256) == 0 &&
          mls_secret_tree_serialize(&g->secret_tree, &tree) == 0, "tree bytes");
    CHECK(tree.len < v3_len &&
          memcmp(v3 + v3_len - tree.len, tree.data, tree.len) == 0, "tree at the end");
    size_t body_end = v3_len - tree.len;
    CHECK(count_of(v3, v3_len, g->epoch_secrets.sender_data_secret, MLS_HASH_LEN) == 1,
          "epoch secrets located");
    size_t off = 0;
    while (memcmp(v3 + off, g->epoch_secrets.sender_data_secret, MLS_HASH_LEN) != 0) off++;
    const size_t kept = 8 * MLS_HASH_LEN;   /* sender_data .. init, as in format 3 */
    CHECK(memcmp(v3 + off + 7 * MLS_HASH_LEN, g->epoch_secrets.init_secret,
                 MLS_HASH_LEN) == 0, "format 3 epoch secrets");
    /* Format 1 ends with max_forward_distance (after the two transcript
     * hashes and the extensions): no PSK or path-key cache. */
    size_t tail_end = body_end;
    if (version == 1) {
        size_t start = off + kept + 2 * MLS_HASH_LEN;
        MlsTlsReader r;
        mls_tls_reader_init(&r, v3 + start, body_end - start);
        uint8_t *ext = NULL;
        size_t ext_len = 0;
        uint32_t max_forward = 0;
        CHECK(mls_tls_read_opaque32(&r, &ext, &ext_len) == 0 &&
              mls_tls_read_u32(&r, &max_forward) == 0 &&
              max_forward == g->max_forward_distance, "format 1 layout");
        free(ext);
        tail_end = body_end - mls_tls_reader_remaining(&r);
    }

    size_t len = off + kept + 3 * MLS_HASH_LEN + (tail_end - (off + kept));
    uint8_t *out = malloc(len);
    CHECK(out, "alloc");
    size_t w = 0;
    memcpy(out, v3, off);
    put_u32(out + 4, version);
    w = off;
    memcpy(out + w, v3 + off, MLS_HASH_LEN);                        /* sender_data */
    w += MLS_HASH_LEN;
    memcpy(out + w, enc, MLS_HASH_LEN);                             /* encryption */
    w += MLS_HASH_LEN;
    memcpy(out + w, v3 + off + MLS_HASH_LEN, kept - MLS_HASH_LEN);  /* exporter .. init */
    w += kept - MLS_HASH_LEN;
    memcpy(out + w, old->epoch_secrets.welcome_secret, MLS_HASH_LEN);
    w += MLS_HASH_LEN;
    memcpy(out + w, old->epoch_secrets.joiner_secret, MLS_HASH_LEN);
    w += MLS_HASH_LEN;
    memcpy(out + w, v3 + off + kept, tail_end - (off + kept));
    w += tail_end - (off + kept);
    CHECK(w == len, "legacy length");
    sodium_memzero(tree.data, tree.len);
    mls_tls_buf_free(&tree);
    free_secret(v3, v3_len);
    *out_len = len;
    return out;
}

/* Loading a format 2 (or 1) state: the own sender continues
 * MLS_SECRET_TREE_LEGACY_OWN_STRIDE generations ahead (past the 0 every such
 * state ever sent at); a receiver accepts what it cannot know it read (a
 * window bounded by max_forward_distance) once more, then never again; the
 * next save is format 3 without the consumed secrets. */
static void
test_legacy_state_migration(void)
{
    Pair p;
    pair_init(&p);
    uint8_t enc[MLS_HASH_LEN];
    encryption_secret_of(&p.alice, enc);

    /* 0.7 sent at generation 0 and Bob read it, in one operation each ... */
    size_t l0 = 0;
    uint8_t *m0 = send_msg(&p.alice, "old m0", &l0);
    CHECK(sender_of(&p.alice, m0, l0).generation == 0, "old send");
    CHECK(recv_msg(&p.bob, m0, l0, "old m0") == 0, "old read");
    /* ... and stored no ratchet. */
    size_t la = 0, lb = 0, lb1 = 0;
    uint8_t *va = legacy_blob(&p.alice, &p.alice, enc, 2, &la);
    uint8_t *vb = legacy_blob(&p.bob, &p.alice, enc, 2, &lb);
    uint8_t *vb1 = legacy_blob(&p.bob, &p.alice, enc, 1, &lb1);
    CHECK(contains(va, la, enc, MLS_HASH_LEN) && contains(vb, lb, enc, MLS_HASH_LEN) &&
          contains(vb, lb, p.alice.epoch_secrets.joiner_secret, MLS_HASH_LEN),
          "format 2 stored the encryption and joiner secrets");

    MlsGroup alice, bob, bob1;
    CHECK(mls_group_deserialize(va, la, &alice) == 0, "format 2 (Alice)");
    CHECK(mls_group_deserialize(vb, lb, &bob) == 0, "format 2 (Bob)");
    CHECK(mls_group_deserialize(vb1, lb1, &bob1) == 0, "format 1 (Bob)");
    CHECK(alice.epoch == p.alice.epoch &&
          memcmp(alice.epoch_secrets.exporter_secret, p.alice.epoch_secrets.exporter_secret,
                 MLS_HASH_LEN) == 0, "the same epoch");
    static const uint8_t zero[MLS_HASH_LEN] = {0};
    CHECK(sodium_memcmp(alice.epoch_secrets.encryption_secret, zero, MLS_HASH_LEN) == 0 &&
          sodium_memcmp(alice.epoch_secrets.joiner_secret, zero, MLS_HASH_LEN) == 0 &&
          sodium_memcmp(alice.epoch_secrets.welcome_secret, zero, MLS_HASH_LEN) == 0,
          "consumed secrets dropped on load");
    const MlsSenderRatchet *own = &alice.secret_tree.senders[0];
    CHECK(own->application_generation == MLS_SECRET_TREE_LEGACY_OWN_STRIDE &&
          own->handshake_generation == MLS_SECRET_TREE_LEGACY_OWN_STRIDE,
          "own chains moved to %u/%u", own->application_generation,
          own->handshake_generation);
    CHECK(bob1.secret_tree.senders[1].application_generation ==
          MLS_SECRET_TREE_LEGACY_OWN_STRIDE, "format 1 too");

    /* Bob cannot know he read generation 0: it is accepted once more. */
    CHECK(recv_msg(&bob, m0, l0, "old m0") == 0, "legacy receiver window");
    reload(&bob);   /* format 3 from here on */
    expect_rejected_unchanged(&bob, m0, l0, "and then never again");

    /* Alice's first send after the upgrade is past anything 0.7 used. */
    size_t l1 = 0, l2 = 0, l3 = 0;
    uint8_t *m1 = send_msg(&alice, "new m1", &l1);
    CHECK(sender_of(&alice, m1, l1).generation == MLS_SECRET_TREE_LEGACY_OWN_STRIDE,
          "stride");
    reload(&alice);
    uint8_t *m2 = send_msg(&alice, "new m2", &l2);
    CHECK(sender_of(&alice, m2, l2).generation == MLS_SECRET_TREE_LEGACY_OWN_STRIDE + 1,
          "and on from there, across a reload");
    CHECK(recv_msg(&bob, m1, l1, "new m1") == 0 && recv_msg(&bob, m2, l2, "new m2") == 0,
          "an upgraded receiver follows the jump");
    CHECK(recv_msg(&p.bob, m1, l1, "new m1") == 0, "so does one that never reloaded");
    /* Bob, upgraded too, answers past his own stride. */
    uint8_t *m3 = send_msg(&bob, "bob m3", &l3);
    CHECK(sender_of(&bob, m3, l3).generation == MLS_SECRET_TREE_LEGACY_OWN_STRIDE, "Bob");
    CHECK(recv_msg(&alice, m3, l3, "bob m3") == 0, "Alice reads Bob");

    /* The migrated state is stored as format 3, without the old secrets. */
    size_t ls = 0;
    uint8_t *s = save(&alice, &ls);
    CHECK(s[4] == 0 && s[5] == 0 && s[6] == 0 && s[7] == 3, "format 3");
    CHECK(!contains(s, ls, enc, MLS_HASH_LEN) &&
          !contains(s, ls, p.alice.epoch_secrets.joiner_secret, MLS_HASH_LEN) &&
          !contains(s, ls, p.alice.epoch_secrets.welcome_secret, MLS_HASH_LEN),
          "the consumed secrets are gone");
    free_secret(s, ls);
    s = save(&bob, &ls);
    CHECK(!contains(s, ls, enc, MLS_HASH_LEN) &&
          !contains(s, ls, p.alice.epoch_secrets.joiner_secret, MLS_HASH_LEN),
          "gone from Bob's too");

    free_secret(s, ls);
    free_secret(va, la);
    free_secret(vb, lb);
    free_secret(vb1, lb1);
    free(m0);
    free(m1);
    free(m2);
    free(m3);
    mls_group_free(&alice);
    mls_group_free(&bob);
    mls_group_free(&bob1);
    sodium_memzero(enc, sizeof(enc));
    pair_clear(&p);
}

/* Fail closed: a state that is truncated, extended, of an unknown version,
 * or whose secret tree does not fit the group, does not load. */
static void
test_malformed_states_refused(void)
{
    Pair p;
    pair_init(&p);
    size_t len = 0;
    uint8_t *blob = save(&p.bob, &len);
    MlsTlsBuf tree;
    CHECK(mls_tls_buf_init(&tree, 256) == 0 &&
          mls_secret_tree_serialize(&p.bob.secret_tree, &tree) == 0, "tree");
    size_t tree_off = len - tree.len;
    MlsGroup g;

    uint8_t *copy = malloc(len + 1);
    CHECK(copy, "alloc");
    for (size_t cut = 1; cut < tree.len; cut += 7)
        CHECK(mls_group_deserialize(blob, len - cut, &g) != 0, "truncated by %zu", cut);
    memcpy(copy, blob, len);
    copy[len] = 0;
    CHECK(mls_group_deserialize(copy, len + 1, &g) != 0, "trailing byte");
    memcpy(copy, blob, len);
    put_u32(copy + 4, 4);
    CHECK(mls_group_deserialize(copy, len, &g) != 0, "unknown version");
    memcpy(copy, blob, len);
    put_u32(copy + tree_off, p.bob.tree.n_leaves + 1);
    CHECK(mls_group_deserialize(copy, len, &g) != 0, "leaf count");
    memcpy(copy, blob, len);
    copy[tree_off + 4] = 7;   /* leaf 0's kind */
    CHECK(mls_group_deserialize(copy, len, &g) != 0, "leaf kind");
    memcpy(copy, blob, len);
    put_u32(copy + 4, 2);     /* format 3 bytes claiming format 2 */
    CHECK(mls_group_deserialize(copy, len, &g) != 0, "format 3 read as 2");
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "the original loads");
    mls_group_free(&g);

    /* A skipped key outside its window, twice, or above the head.  After a
     * jump to WINDOW + 10 the window [10, WINDOW + 10) fills the cache;
     * slot 0 (generation 10) is overwritten with each bad entry in turn. */
    uint8_t enc[MLS_HASH_LEN];
    memset(enc, 0x42, sizeof(enc));
    MlsSecretTree st;
    MlsMessageKeys k;
    CHECK(mls_secret_tree_init(&st, enc, 2) == 0 &&
          mls_secret_tree_get_keys_for_generation(&st, 0, false, WINDOW + 10, 1000, &k) == 0,
          "a jump");
    MlsSkippedMessageKey *cache = st.senders[0].application_skipped;
    CHECK(cache[0].valid && cache[0].keys.generation == 10 && cache[1].valid,
          "cache layout");
    const struct { uint32_t gen; const char *what; } bad[] = {
        { 2, "below the window" },
        { WINDOW + 11, "at the next generation" },
        { WINDOW + 20, "above the head" },
    };
    const uint32_t slot = 0;
    for (size_t i = 0; i <= sizeof(bad) / sizeof(bad[0]); i++) {
        MlsSkippedMessageKey saved = cache[slot];
        cache[slot].valid = true;
        cache[slot].keys = cache[1].keys;   /* i == 3: a duplicate */
        if (i < 3) cache[slot].keys.generation = bad[i].gen;
        MlsTlsBuf b;
        CHECK(mls_tls_buf_init(&b, 256) == 0 && mls_secret_tree_serialize(&st, &b) == 0,
              "serialize");
        MlsTlsReader r;
        mls_tls_reader_init(&r, b.data, b.len);
        MlsSecretTree out;
        CHECK(mls_secret_tree_deserialize(&r, 2, &out) != 0, "skipped key %s",
              i < 3 ? bad[i].what : "given twice");
        CHECK(out.senders == NULL && out.tree_secrets == NULL, "nothing left behind");
        sodium_memzero(b.data, b.len);
        mls_tls_buf_free(&b);
        cache[slot] = saved;
    }
    mls_secret_tree_free(&st);

    sodium_memzero(tree.data, tree.len);
    mls_tls_buf_free(&tree);
    free_secret(copy, len + 1);
    free_secret(blob, len);
    pair_clear(&p);
}

int
main(void)
{
    if (sodium_init() < 0) return 1;
    printf("libmarmot: secret-tree ratchet persistence (nostrc-ai04)\n");
    RUN(test_sender_generations_survive_reload);
    RUN(test_replay_rejected_across_reloads);
    RUN(test_out_of_order_window);
    RUN(test_failed_decryption_consumes_nothing);
    RUN(test_failed_message_restores_ratchet);
    RUN(test_consumed_secrets_not_stored);
    RUN(test_exhausted_chain_never_wraps);
    RUN(test_legacy_state_migration);
    RUN(test_malformed_states_refused);
    printf("All ratchet persistence tests passed\n");
    return 0;
}
