/* Noise negative space (nostrc-pwo3h.8). The differential harness is the
 * positive oracle; this file is the adversarial one: exact error variants and
 * DH cost for every single-byte mutation of every handshake message and
 * transport frame, untouched originals and fork destinations, an EXPLICIT
 * allow-list for every accepted mutation, every length from 0 to MAX+1 for
 * every message type, replay-window edges against an independent set oracle,
 * nonce overflow at every layer, wrong static keys, API misuse after destroy,
 * and zeroization of every secret-bearing range. Handshake, ECDH and cipher are
 * compiled in with FIPS_TEST_HOOKS (DH counter, counter injection). */
#include <fips/noise/session.h>
#include <fips/noise/ecdh.h>
#include "fakes/host.h"
#include <secp256k1.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

enum { IK1 = 0, IK2, XK1, XK2, XK3, MESSAGES };
static const size_t sizes[MESSAGES] = {FIPS_NOISE_HANDSHAKE_MSG1_SIZE, FIPS_NOISE_HANDSHAKE_MSG2_SIZE,
    FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE, FIPS_NOISE_XK_HANDSHAKE_MSG2_SIZE, FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE};
/* DH operations a genuine read costs: IK1 es+ss, IK2 ee (msg1's es is cached),
 * XK1 es, XK2 ee, XK3 se. */
static const unsigned genuine_dh[MESSAGES] = {2, 1, 1, 1, 1};
static const char *const names[MESSAGES] = {"IK msg1", "IK msg2", "XK msg1", "XK msg2", "XK msg3"};

typedef fips_noise_status (*writer)(fips_handshake_state *, uint8_t *, size_t, size_t *);
typedef fips_noise_status (*reader)(fips_handshake_state *, const uint8_t *, size_t);
typedef fips_noise_status (*forker)(const fips_handshake_state *, const uint8_t *, size_t, fips_handshake_state *);
static const writer writers[MESSAGES] = {fips_handshake_write_message_1, fips_handshake_write_message_2,
    fips_handshake_write_xk_message_1, fips_handshake_write_xk_message_2, fips_handshake_write_xk_message_3};
static const reader readers[MESSAGES] = {fips_handshake_read_message_1, fips_handshake_read_message_2,
    fips_handshake_read_xk_message_1, fips_handshake_read_xk_message_2, fips_handshake_read_xk_message_3};
/* msg1 has no fork reader in either port: a responder gets a fresh state per msg1. */
static const forker forkers[MESSAGES] = {NULL, fips_handshake_fork_msg2, NULL, fips_handshake_fork_xk2,
    fips_handshake_fork_xk3};

typedef enum mutation { M_XOR1, M_XOR80, M_ZERO, M_FF, MUTATIONS } mutation;
static uint8_t mutate(uint8_t byte, mutation m)
{
    switch (m) {
    case M_XOR1: return (uint8_t)(byte ^ 0x01);
    case M_XOR80: return (uint8_t)(byte ^ 0x80);
    case M_ZERO: return 0x00;
    default: return 0xff;
    }
}

typedef struct snapshot { fips_handshake_state state; uint64_t entropy; } snapshot;
typedef struct fixture {
    fips_fixed_entropy fixed;
    fips_seeded_entropy stream;
    fips_secp *secp;
    secp256k1_context *oracle; /* Independent point-validity oracle, never the port's context. */
    fips_identity *key[2];
    uint8_t pub[2][33], epoch[8];
    bool xk;
    unsigned first, last;
    fips_handshake_state i, r; /* Complete. */
    snapshot before_write[MESSAGES], before_read[MESSAGES];
    uint8_t msg[MESSAGES][FIPS_NOISE_HANDSHAKE_MSG1_SIZE];
} fixture;

static bool filled(const void *data, size_t len, uint8_t byte)
{
    const uint8_t *p = data;
    for (size_t k = 0; k < len; ++k) if (p[k] != byte) return false;
    return true;
}
static bool same(const fips_handshake_state *a, const fips_handshake_state *b)
{ return memcmp(a, b, sizeof(*a)) == 0; }
static bool valid_point(const fixture *f, const uint8_t bytes[33])
{
    secp256k1_pubkey parsed;
    return secp256k1_ec_pubkey_parse(f->oracle, &parsed, bytes, 33) == 1;
}
static void snap(snapshot *s, const fips_handshake_state *state, uint64_t entropy)
{
    fips_handshake_fork(state, &s->state);
    s->entropy = entropy;
}
static bool to_responder(unsigned m) { return m == IK1 || m == XK1 || m == XK3; }

static int init(fixture *f, bool xk, uint64_t seed)
{
    memset(f, 0, sizeof(*f));
    f->fixed.byte = 0x5a;
    fips_entropy blind = fips_fixed_entropy_adapter(&f->fixed);
    CHECK(fips_identity_init_secp(blind, &f->secp) == FIPS_IDENTITY_OK);
    f->oracle = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    CHECK(f->oracle != NULL);
    for (size_t k = 0; k < 2; ++k) {
        fips_seeded_entropy keyed = {seed ^ ((uint64_t)(k + 1) * UINT64_C(0x6b65797061697273))};
        fips_entropy e = fips_seeded_entropy_adapter(&keyed);
        uint8_t secret[32];
        e.fill(e.ctx, secret, sizeof(secret));
        fips_identity_status rc = fips_identity_from_secret_bytes(f->secp, blind, secret, &f->key[k]);
        sodium_memzero(secret, sizeof(secret));
        CHECK(rc == FIPS_IDENTITY_OK);
        fips_identity_pubkey_full(f->key[k], f->pub[k]);
    }
    f->stream.state = seed;
    fips_entropy entropy = fips_seeded_entropy_adapter(&f->stream);
    for (unsigned k = 0; k < 8; ++k) f->epoch[k] = (uint8_t)(0xe0 + k);
    f->xk = xk;
    f->first = xk ? XK1 : IK1;
    f->last = xk ? XK3 : IK2;
    CHECK((xk ? fips_handshake_new_xk_initiator(&f->i, f->secp, f->key[0], f->pub[1], f->epoch, entropy)
              : fips_handshake_new_initiator(&f->i, f->secp, f->key[0], f->pub[1], f->epoch, entropy)) == FIPS_NOISE_OK);
    CHECK((xk ? fips_handshake_new_xk_responder(&f->r, f->secp, f->key[1], f->epoch, entropy)
              : fips_handshake_new_responder(&f->r, f->secp, f->key[1], f->epoch, entropy)) == FIPS_NOISE_OK);
    /* Run the transcript, snapshotting writer and reader right before each message. */
    for (unsigned m = f->first; m <= f->last; ++m) {
        fips_handshake_state *w = to_responder(m) ? &f->i : &f->r, *rd = to_responder(m) ? &f->r : &f->i;
        size_t n = 0;
        snap(&f->before_write[m], w, f->stream.state);
        CHECK(writers[m](w, f->msg[m], sizes[m], &n) == FIPS_NOISE_OK && n == sizes[m]);
        snap(&f->before_read[m], rd, f->stream.state);
        if (forkers[m]) {
            fips_handshake_state next;
            CHECK(forkers[m](rd, f->msg[m], n, &next) == FIPS_NOISE_OK);
            fips_handshake_destroy(rd);
            fips_handshake_fork(&next, rd);
            fips_handshake_destroy(&next);
        } else CHECK(readers[m](rd, f->msg[m], n) == FIPS_NOISE_OK);
    }
    CHECK(f->i.progress == FIPS_HANDSHAKE_COMPLETE && f->r.progress == FIPS_HANDSHAKE_COMPLETE);
    CHECK(!memcmp(fips_handshake_hash(&f->i), fips_handshake_hash(&f->r), 32));
    return 0;
}
static void destroy(fixture *f)
{
    fips_handshake_destroy(&f->i);
    fips_handshake_destroy(&f->r);
    for (unsigned m = 0; m < MESSAGES; ++m) {
        fips_handshake_destroy(&f->before_write[m].state);
        fips_handshake_destroy(&f->before_read[m].state);
    }
    fips_identity_destroy(f->key[0]);
    fips_identity_destroy(f->key[1]);
    fips_identity_destroy_secp(f->secp);
    secp256k1_context_destroy(f->oracle);
    sodium_memzero(f, sizeof(*f));
}
/* Sessions from forks of the completed states; the fixture stays usable. */
static int make_sessions(fixture *f, fips_noise_session *si, fips_noise_session *sr)
{
    fips_handshake_state i, r;
    fips_aead_key is, ir, rs, rr;
    uint8_t ih[32], rh[32], ip[33], rp[33], ie[8], re[8];
    fips_handshake_fork(&f->i, &i);
    fips_handshake_fork(&f->r, &r);
    CHECK(fips_handshake_finish(&i, &is, &ir, ih, ip, ie) == FIPS_NOISE_OK);
    CHECK(fips_handshake_finish(&r, &rs, &rr, rh, rp, re) == FIPS_NOISE_OK);
    CHECK(filled(&i, sizeof(i), 0) && filled(&r, sizeof(r), 0));
    CHECK(!filled(is.bytes, 32, 0) && !filled(ir.bytes, 32, 0));
    CHECK(fips_noise_session_from_handshake(si, FIPS_HANDSHAKE_INITIATOR, &is, &ir, ih, ip, ie) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_from_handshake(sr, FIPS_HANDSHAKE_RESPONDER, &rs, &rr, rh, rp, re) == FIPS_NOISE_OK);
    /* Consumed keys are wiped by the constructor. */
    CHECK(filled(&is, sizeof(is), 0) && filled(&ir, sizeof(ir), 0) && filled(&rs, sizeof(rs), 0) && filled(&rr, sizeof(rr), 0));
    return 0;
}

/* Read message m from `in` on a fresh copy of the reader's snapshot: the
 * snapshot is untouched, no entropy is drawn, the DH cost is reported and a
 * fork reader's destination is untouched on failure. On success *out holds the
 * advanced copy (caller destroys it). */
static int attempt(fixture *f, unsigned m, const uint8_t *in, size_t len, fips_noise_status *rc,
                   unsigned *cost, fips_handshake_state *out)
{
    const fips_handshake_state *src = &f->before_read[m].state;
    fips_handshake_state before, dst;
    fips_handshake_fork(src, &before);
    memset(&dst, 0xa5, sizeof(dst));
    uint64_t entropy = f->stream.state, dh = fips_noise_dh_ops();
    if (forkers[m]) *rc = forkers[m](src, in, len, &dst);
    else {
        fips_handshake_fork(src, &dst);
        *rc = readers[m](&dst, in, len);
    }
    *cost = (unsigned)(fips_noise_dh_ops() - dh);
    CHECK(same(src, &before) && f->stream.state == entropy);
    if (*rc == FIPS_NOISE_OK) memcpy(out, &dst, sizeof(dst));
    else if (forkers[m]) CHECK(filled(&dst, sizeof(dst), 0xa5));
    fips_handshake_destroy(&dst);
    fips_handshake_destroy(&before);
    return 0;
}

/* The explicit allow-list and cost model. Every accepted mutation is one of:
 *   identity  - the mutation left the byte unchanged (0x00 stays 0x00, 0xff stays 0xff);
 *   parity    - ephemeral prefix 02<->03 on an AEAD-carrying message: x-only DH
 *               gives the same key, and pinned FIPS v1 does not bind h as AAD;
 *   xk1       - XK msg1 carries no authentication at all, so any 33 bytes that
 *               the independent oracle parses as a point are read (1 DH).
 * Everything else must fail with the exact variant and DH count below. */
typedef enum rule { R_IDENTITY, R_PARITY, R_XK1, R_INVALID_POINT, R_WRONG_X, R_AEAD, RULES } rule;
static const char *const rule_names[RULES] = {"identity", "parity", "xk1-unauthenticated",
    "invalid-point", "valid-point-wrong-x", "aead"};
typedef struct verdict { fips_noise_status rc; unsigned dh; rule rule; } verdict;
static verdict expect(const fixture *f, unsigned m, size_t off, const uint8_t *bad, const uint8_t *good)
{
    verdict v = {FIPS_NOISE_DecryptionFailed, 1, R_AEAD};
    if (!memcmp(bad, good, sizes[m])) {
        v.rc = FIPS_NOISE_OK; v.dh = genuine_dh[m]; v.rule = R_IDENTITY;
        return v;
    }
    if (m != XK3 && off < FIPS_NOISE_PUBKEY_SIZE) {
        if (!valid_point(f, bad)) { v.rc = FIPS_NOISE_InvalidPublicKey; v.dh = 0; v.rule = R_INVALID_POINT; }
        else if (m == XK1) { v.rc = FIPS_NOISE_OK; v.dh = 1; v.rule = R_XK1; }
        else if (off == 0) { v.rc = FIPS_NOISE_OK; v.dh = genuine_dh[m]; v.rule = R_PARITY; }
        else { v.rc = FIPS_NOISE_DecryptionFailed; v.dh = 1; v.rule = R_WRONG_X; }
        return v;
    }
    /* AEAD regions: the DH work done before the failing open. */
    if (m == IK1) v.dh = off < FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_TAG_SIZE ? 1 : 2;
    else if (m == XK3) v.dh = off < FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_TAG_SIZE ? 0 : 1;
    return v;
}

/* An XK msg1 the oracle accepted is unauthenticated, so the responder proceeds;
 * msg2 then authenticates at the real initiator only if x was unchanged. */
static int xk1_follow_up(fixture *f, fips_handshake_state *tampered, bool same_x)
{
    fips_seeded_entropy scratch = {UINT64_C(0x5c7a7c6d)};
    fips_handshake_state reply, before, read;
    uint8_t wire[FIPS_NOISE_XK_HANDSHAKE_MSG2_SIZE];
    size_t n = 0;
    fips_handshake_fork(tampered, &reply);
    reply.entropy = fips_seeded_entropy_adapter(&scratch);
    CHECK(fips_handshake_write_xk_message_2(&reply, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
    const fips_handshake_state *initiator = &f->before_read[XK2].state;
    fips_handshake_fork(initiator, &before);
    memset(&read, 0xa5, sizeof(read));
    uint64_t dh = fips_noise_dh_ops();
    CHECK(fips_handshake_fork_xk2(initiator, wire, n, &read) == (same_x ? FIPS_NOISE_OK : FIPS_NOISE_DecryptionFailed));
    CHECK(fips_noise_dh_ops() - dh == 1 && same(initiator, &before));
    if (!same_x) CHECK(filled(&read, sizeof(read), 0xa5));
    fips_handshake_destroy(&reply);
    fips_handshake_destroy(&before);
    fips_handshake_destroy(&read);
    return 0;
}

/* Pinned allow-list sizes for the fixture seeds (IK seed 0x494b, XK seed 0x584b):
 * identity mutations are counted from the message bytes; parity is exactly one
 * mutation (offset 0 XOR 1) on each AEAD-carrying ephemeral message; XK msg1
 * accepts exactly the oracle's valid points, pinned here so a change in either
 * the fixture or the parser is visible. */
static const size_t xk1_accepted_pinned = 71; /* Cross-checked by Euler's criterion in Python. */
static int tamper_matrix(fixture *f, unsigned m)
{
    size_t n = sizes[m], counts[RULES] = {0}, identity_expected = 0;
    fips_handshake_state genuine, got;
    unsigned cost;
    fips_noise_status rc;
    CHECK(attempt(f, m, f->msg[m], n, &rc, &cost, &genuine) == 0);
    CHECK(rc == FIPS_NOISE_OK && cost == genuine_dh[m]);
    for (size_t off = 0; off < n; ++off)
        identity_expected += (f->msg[m][off] == 0x00) + (f->msg[m][off] == 0xff);
    for (size_t off = 0; off < n; ++off) {
        for (unsigned mu = 0; mu < MUTATIONS; ++mu) {
            uint8_t bad[FIPS_NOISE_HANDSHAKE_MSG1_SIZE];
            memcpy(bad, f->msg[m], n);
            bad[off] = mutate(bad[off], (mutation)mu);
            verdict v = expect(f, m, off, bad, f->msg[m]);
            CHECK(attempt(f, m, bad, n, &rc, &cost, &got) == 0);
            if (rc != v.rc || cost != v.dh)
                fprintf(stderr, "%s offset %zu mutation %u: rc %d (expected %d) dh %u (expected %u) rule %s\n",
                        names[m], off, mu, (int)rc, (int)v.rc, cost, v.dh, rule_names[v.rule]);
            CHECK(rc == v.rc && cost == v.dh);
            ++counts[v.rule];
            if (rc != FIPS_NOISE_OK) continue;
            if (v.rule == R_IDENTITY) CHECK(same(&got, &genuine));
            else {
                /* Accepted but different bytes: a different transcript hash. */
                CHECK(memcmp(fips_handshake_hash(&got), fips_handshake_hash(&genuine), 32) != 0);
                CHECK(!memcmp(got.remote_ephemeral, bad, 33) && got.has_remote_ephemeral);
                if (v.rule == R_PARITY) CHECK(!memcmp(got.symmetric.ck, genuine.symmetric.ck, 32));
                if (v.rule == R_XK1) CHECK(xk1_follow_up(f, &got, off == 0) == 0);
            }
            fips_handshake_destroy(&got);
        }
    }
    fips_handshake_destroy(&genuine);
    printf("%s bytes:", names[m]);
    for (size_t off = 0; off < n; ++off) printf("%02x", f->msg[m][off]);
    printf("\n%s: %zu mutations; identity %zu, parity %zu, xk1 %zu, invalid-point %zu, wrong-x %zu, aead %zu\n",
           names[m], n * MUTATIONS, counts[R_IDENTITY], counts[R_PARITY], counts[R_XK1],
           counts[R_INVALID_POINT], counts[R_WRONG_X], counts[R_AEAD]);
    CHECK(counts[R_IDENTITY] == identity_expected);
    CHECK(counts[R_PARITY] == (m == IK1 || m == IK2 || m == XK2 ? 1u : 0u));
    CHECK(counts[R_XK1] == (m == XK1 ? xk1_accepted_pinned : 0u));
    if (m != XK3) CHECK(counts[R_INVALID_POINT] >= 3 * MUTATIONS); /* Offset 0 -> 0x82/0x83, 0x00, 0xff and more. */
    if (m != XK1 && m != XK3) CHECK(counts[R_WRONG_X] > 16 && counts[R_WRONG_X] < 32 * MUTATIONS - 16);
    if (m != XK1) CHECK(counts[R_AEAD] + counts[R_IDENTITY] >= (n - (m == XK3 ? 0 : 33)) * MUTATIONS - identity_expected);
    return 0;
}

/* Every length from 0 to MAX+1 except the exact one is MessageTooShort, before
 * any work: no DH, no entropy, raw state byte-identical, fork destination
 * untouched. SIZE_MAX too. */
static int length_matrix(fixture *f, unsigned m)
{
    static uint8_t buf[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    size_t n = sizes[m];
    const fips_handshake_state *src = &f->before_read[m].state;
    fips_handshake_state before, dst;
    memset(buf, 0x3c, sizeof(buf));
    memcpy(buf, f->msg[m], n);
    fips_handshake_fork(src, &before);
    uint64_t dh = fips_noise_dh_ops(), entropy = f->stream.state;
    for (size_t len = 0; len <= FIPS_NOISE_MAX_MESSAGE_SIZE + 1; ++len) {
        if (len == n) continue;
        const uint8_t *in = len ? buf : NULL;
        fips_handshake_fork(src, &dst);
        CHECK(readers[m](&dst, in, len) == FIPS_NOISE_MessageTooShort);
        CHECK(same(&dst, &before));
        if (!forkers[m]) continue;
        memset(&dst, 0xa5, sizeof(dst));
        CHECK(forkers[m](src, in, len, &dst) == FIPS_NOISE_MessageTooShort);
        CHECK(filled(&dst, sizeof(dst), 0xa5));
    }
    fips_handshake_fork(src, &dst);
    CHECK(readers[m](&dst, buf, SIZE_MAX) == FIPS_NOISE_MessageTooShort && same(&dst, &before));
    CHECK(same(src, &before) && dh == fips_noise_dh_ops() && entropy == f->stream.state);
    fips_handshake_destroy(&dst);
    fips_handshake_destroy(&before);
    return 0;
}

/* Every capacity below the message size is MessageTooLarge with nothing drawn,
 * computed or written; the exact capacity and larger ones reproduce the bytes. */
static int capacity_matrix(fixture *f, unsigned m)
{
    size_t n = sizes[m], len;
    const snapshot *s = &f->before_write[m];
    fips_handshake_state copy, before;
    uint8_t out[FIPS_NOISE_HANDSHAKE_MSG1_SIZE + 1];
    fips_handshake_fork(&s->state, &before);
    uint64_t dh = fips_noise_dh_ops();
    for (size_t cap = 0; cap < n; ++cap) {
        fips_handshake_fork(&s->state, &copy);
        f->stream.state = s->entropy;
        memset(out, 0xa5, sizeof(out));
        len = SIZE_MAX;
        CHECK(writers[m](&copy, out, cap, &len) == FIPS_NOISE_MessageTooLarge);
        CHECK(len == SIZE_MAX && filled(out, sizeof(out), 0xa5) && same(&copy, &before));
        CHECK(f->stream.state == s->entropy && dh == fips_noise_dh_ops());
    }
    static const size_t generous[] = {0, 1, SIZE_MAX};
    for (size_t k = 0; k < 3; ++k) {
        fips_handshake_fork(&s->state, &copy);
        f->stream.state = s->entropy;
        memset(out, 0xa5, sizeof(out));
        len = SIZE_MAX;
        CHECK(writers[m](&copy, out, generous[k] == SIZE_MAX ? SIZE_MAX : n + generous[k], &len) == FIPS_NOISE_OK);
        CHECK(len == n && !memcmp(out, f->msg[m], n) && out[n] == 0xa5);
        fips_handshake_destroy(&copy);
    }
    CHECK(same(&s->state, &before));
    fips_handshake_destroy(&before);
    return 0;
}

/* Transport frames: every byte of ciphertext||tag and of the AD, with every
 * mutation, and the counter, must fail with DecryptionFailed (ReplayDetected
 * for the refused ceiling), zero exactly the plaintext range, leave out_len,
 * the replay window and the counter's acceptability untouched. */
static int frame_matrix(fixture *f)
{
    fips_noise_session si, sr;
    static const size_t plain_lens[] = {0, 1, 15, 16, 17, 64, 1000};
    uint8_t ad[3] = {0, 1, 0xa5};
    size_t frames = 0, rejected = 0;
    CHECK(make_sessions(f, &si, &sr) == 0);
    for (size_t p = 0; p < sizeof(plain_lens) / sizeof(plain_lens[0]); ++p) {
        for (size_t ad_len = 0; ad_len <= sizeof(ad); ad_len += sizeof(ad)) {
            uint8_t plain[1000], wire[1000 + FIPS_NOISE_TAG_SIZE], bad[sizeof(wire)], out[sizeof(wire)];
            size_t plain_len = plain_lens[p], clen = 0, got;
            for (size_t k = 0; k < plain_len; ++k) plain[k] = (uint8_t)(k * 13 + p);
            uint64_t counter = fips_noise_session_current_send_counter(&si);
            CHECK(fips_noise_session_seal(&si, ad, ad_len, plain, plain_len, wire, sizeof(wire), &clen) == FIPS_NOISE_OK);
            CHECK(clen == plain_len + FIPS_NOISE_TAG_SIZE);
            fips_replay_window window = sr.replay_window;
            ++frames;
            for (size_t off = 0; off < clen; ++off) {
                for (unsigned mu = 0; mu < MUTATIONS; ++mu) {
                    memcpy(bad, wire, clen);
                    bad[off] = mutate(bad[off], (mutation)mu);
                    got = SIZE_MAX;
                    memset(out, 0xa5, sizeof(out));
                    if (!memcmp(bad, wire, clen)) { /* Identity mutation: still the genuine frame. */
                        CHECK(fips_noise_session_open_unmarked(&sr, counter, ad, ad_len, bad, clen, out, sizeof(out), &got) == FIPS_NOISE_OK);
                        CHECK(got == plain_len && !memcmp(out, plain, plain_len));
                        CHECK(!memcmp(&sr.replay_window, &window, sizeof(window)));
                        continue;
                    }
                    CHECK(fips_noise_session_open(&sr, counter, ad, ad_len, bad, clen, out, sizeof(out), &got) == FIPS_NOISE_DecryptionFailed);
                    CHECK(got == SIZE_MAX && filled(out, plain_len, 0) && filled(out + plain_len, sizeof(out) - plain_len, 0xa5));
                    CHECK(!memcmp(&sr.replay_window, &window, sizeof(window)));
                    CHECK(fips_noise_session_check_replay(&sr, counter) == FIPS_NOISE_OK);
                    ++rejected;
                }
            }
            for (size_t off = 0; off < ad_len; ++off) {
                for (unsigned mu = 0; mu < MUTATIONS; ++mu) {
                    uint8_t bad_ad[sizeof(ad)];
                    memcpy(bad_ad, ad, ad_len);
                    bad_ad[off] = mutate(bad_ad[off], (mutation)mu);
                    if (!memcmp(bad_ad, ad, ad_len)) continue;
                    got = SIZE_MAX;
                    memset(out, 0xa5, sizeof(out));
                    CHECK(fips_noise_session_open(&sr, counter, bad_ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_DecryptionFailed);
                    CHECK(got == SIZE_MAX && filled(out, plain_len, 0) && !memcmp(&sr.replay_window, &window, sizeof(window)));
                    ++rejected;
                }
            }
            /* Wrong counter (acceptable to the window) fails authentication; the
             * refused ceiling is caught by the window before any AEAD work. */
            static const uint64_t deltas[] = {1, 2, 8191, 8192, UINT64_C(1) << 40};
            for (size_t k = 0; k < sizeof(deltas) / sizeof(deltas[0]); ++k) {
                got = SIZE_MAX;
                CHECK(fips_noise_session_open(&sr, counter + deltas[k], ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_DecryptionFailed);
                CHECK(got == SIZE_MAX && !memcmp(&sr.replay_window, &window, sizeof(window)));
                ++rejected;
            }
            memset(out, 0xa5, sizeof(out));
            CHECK(fips_noise_session_open(&sr, UINT64_MAX, ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_ReplayDetected);
            CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5) && !memcmp(&sr.replay_window, &window, sizeof(window)));
            /* Wrong direction: the sender's own receive key must not open its frame. */
            CHECK(fips_noise_session_open(&si, counter, ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_DecryptionFailed);
            CHECK(got == SIZE_MAX);
            /* The genuine frame opens once, then is a replay; unmarked trial opens stay possible. */
            CHECK(fips_noise_session_open(&sr, counter, ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_OK);
            CHECK(got == plain_len && !memcmp(out, plain, plain_len));
            CHECK(fips_noise_session_check_replay(&sr, counter) == FIPS_NOISE_ReplayDetected);
            got = SIZE_MAX;
            CHECK(fips_noise_session_open(&sr, counter, ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_ReplayDetected);
            CHECK(got == SIZE_MAX);
            CHECK(fips_noise_session_open_unmarked(&sr, counter, ad, ad_len, wire, clen, out, sizeof(out), &got) == FIPS_NOISE_OK);
        }
    }
    printf("frames: %zu sealed, %zu tampered opens rejected\n", frames, rejected);
    fips_noise_session_destroy(&si);
    fips_noise_session_destroy(&sr);
    CHECK(filled(&si, sizeof(si), 0) && filled(&sr, sizeof(sr), 0));
    return 0;
}

/* Every transport length 0..MAX+1 against a maximum-size genuine frame: below a
 * tag MessageTooShort, above MAX MessageTooLarge, everything else fails
 * authentication; the window and out_len never move. Seal: every plaintext
 * length up to MAX-16 seals, MAX-15 and above are MessageTooLarge. */
static int transport_length_matrix(fixture *f)
{
    static uint8_t plain[FIPS_NOISE_MAX_MESSAGE_SIZE], wire[FIPS_NOISE_MAX_MESSAGE_SIZE + 1], out[FIPS_NOISE_MAX_MESSAGE_SIZE];
    fips_noise_session si, sr;
    size_t clen = 0, got;
    CHECK(make_sessions(f, &si, &sr) == 0);
    for (size_t k = 0; k < sizeof(plain); ++k) plain[k] = (uint8_t)(k ^ (k >> 8));
    size_t max_plain = FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE;
    uint64_t counter = fips_noise_session_current_send_counter(&si);
    CHECK(fips_noise_session_seal(&si, NULL, 0, plain, max_plain, wire, sizeof(wire), &clen) == FIPS_NOISE_OK);
    CHECK(clen == FIPS_NOISE_MAX_MESSAGE_SIZE);
    wire[FIPS_NOISE_MAX_MESSAGE_SIZE] = 0x5c;
    fips_replay_window window = sr.replay_window;
    for (size_t len = 0; len <= FIPS_NOISE_MAX_MESSAGE_SIZE + 1; ++len) {
        if (len == clen) continue;
        fips_noise_status expected = len < FIPS_NOISE_TAG_SIZE ? FIPS_NOISE_MessageTooShort
            : len > FIPS_NOISE_MAX_MESSAGE_SIZE ? FIPS_NOISE_MessageTooLarge : FIPS_NOISE_DecryptionFailed;
        bool probe_zeroing = len % 4099 == 0 || len < 20 || len > FIPS_NOISE_MAX_MESSAGE_SIZE - 3;
        got = SIZE_MAX;
        if (probe_zeroing) memset(out, 0xa5, sizeof(out));
        CHECK(fips_noise_session_open(&sr, counter, NULL, 0, len ? wire : NULL, len, out, sizeof(out), &got) == expected);
        CHECK(got == SIZE_MAX && !memcmp(&sr.replay_window, &window, sizeof(window)));
        if (probe_zeroing) {
            size_t zeroed = expected == FIPS_NOISE_DecryptionFailed ? len - FIPS_NOISE_TAG_SIZE : 0;
            CHECK(filled(out, zeroed, 0) && filled(out + zeroed, sizeof(out) - zeroed, 0xa5));
        }
    }
    CHECK(fips_noise_session_open(&sr, counter, NULL, 0, wire, SIZE_MAX, out, sizeof(out), &got) == FIPS_NOISE_MessageTooLarge);
    /* Output capacity one short of the plaintext is a capacity error before any AEAD work. */
    CHECK(fips_noise_session_open(&sr, counter, NULL, 0, wire, clen, out, max_plain - 1, &got) == FIPS_NOISE_MessageTooLarge);
    CHECK(got == SIZE_MAX && !memcmp(&sr.replay_window, &window, sizeof(window)));
    CHECK(fips_noise_session_open(&sr, counter, NULL, 0, wire, clen, out, max_plain, &got) == FIPS_NOISE_OK && got == max_plain);
    CHECK(!memcmp(out, plain, max_plain));
    /* Seal lengths: every length up to 4096, every 997th above, and the boundary. */
    for (size_t len = 0; len <= FIPS_NOISE_MAX_MESSAGE_SIZE + 1; len += len < 4096 || len > max_plain - 4 ? 1 : 997) {
        uint64_t before = fips_noise_session_current_send_counter(&si);
        got = SIZE_MAX;
        if (len > max_plain) {
            wire[0] = 0xa5;
            CHECK(fips_noise_session_seal(&si, NULL, 0, plain, len, wire, sizeof(wire), &got) == FIPS_NOISE_MessageTooLarge);
            CHECK(got == SIZE_MAX && wire[0] == 0xa5 && fips_noise_session_current_send_counter(&si) == before);
            /* Capacity one short of ciphertext||tag is also MessageTooLarge, counter unreserved. */
            CHECK(fips_noise_session_seal(&si, NULL, 0, plain, max_plain, wire, FIPS_NOISE_MAX_MESSAGE_SIZE - 1, &got) == FIPS_NOISE_MessageTooLarge);
            CHECK(got == SIZE_MAX && fips_noise_session_current_send_counter(&si) == before);
            continue;
        }
        CHECK(fips_noise_session_seal(&si, NULL, 0, plain, len, wire, sizeof(wire), &got) == FIPS_NOISE_OK);
        CHECK(got == len + FIPS_NOISE_TAG_SIZE && fips_noise_session_current_send_counter(&si) == before + 1);
        if (len % 4099 == 0 || len > max_plain - 4) {
            size_t opened = SIZE_MAX;
            CHECK(fips_noise_session_open(&sr, before, NULL, 0, wire, got, out, sizeof(out), &opened) == FIPS_NOISE_OK);
            CHECK(opened == len && !memcmp(out, plain, len));
        }
    }
    CHECK(fips_noise_session_seal(&si, NULL, 0, plain, SIZE_MAX, wire, sizeof(wire), &got) == FIPS_NOISE_MessageTooLarge);
    fips_noise_session_destroy(&si);
    fips_noise_session_destroy(&sr);
    return 0;
}

/* Replay edges against an independent set-of-counters oracle: the refused
 * ceiling, the exclusive edge at age 8192, the inclusive edge at 8191, counter
 * 0 (unseen at start, seen once), far-future jumps that reset the bitmap and
 * exact-size shifts, with failed marks leaving every byte unchanged. */
typedef struct seen { uint64_t v[64]; size_t n; uint64_t high; } seen;
static bool oracle_accepts(const seen *s, uint64_t c)
{
    if (c == UINT64_MAX) return false;
    if (c <= s->high && s->high - c >= FIPS_REPLAY_WINDOW_SIZE) return false;
    for (size_t k = 0; k < s->n; ++k) if (s->v[k] == c) return false;
    return true;
}
static int mark_checked(fips_replay_window *w, seen *s, uint64_t c)
{
    fips_replay_window before = *w;
    bool expected = oracle_accepts(s, c);
    CHECK(fips_replay_window_check(w, c) == expected);
    CHECK(!memcmp(w, &before, sizeof(before)));
    CHECK(fips_replay_window_mark(w, c) == expected);
    if (!expected) CHECK(!memcmp(w, &before, sizeof(before)));
    else {
        CHECK(s->n < sizeof(s->v) / sizeof(s->v[0]));
        s->v[s->n++] = c;
        if (c > s->high) s->high = c;
        CHECK(!fips_replay_window_check(w, c));
    }
    CHECK(fips_replay_window_highest(w) == s->high);
    return 0;
}
static int probe_all(const fips_replay_window *w, const seen *s, const uint64_t *probes, size_t count)
{
    for (size_t k = 0; k < count; ++k) CHECK(fips_replay_window_check(w, probes[k]) == oracle_accepts(s, probes[k]));
    return 0;
}
static int replay_matrix(void)
{
    static const uint64_t highs[] = {0, 1, 2, 63, 64, 65, 127, 128, 8190, 8191, 8192, 8193, 16383, 16384,
        UINT64_C(1) << 32, UINT64_C(1) << 63, UINT64_MAX - 2, UINT64_MAX - 1};
    static const uint64_t below[] = {1, 2, 63, 64, 65, 8190, 8191, 8192, 8193};
    static const uint64_t above[] = {1, 2, 63, 64, 65, 8190, 8191, 8192, 8193, UINT64_C(1) << 40};
    size_t scenarios = 0;
    for (size_t h = 0; h < sizeof(highs) / sizeof(highs[0]); ++h) {
        uint64_t H = highs[h], probes[40];
        size_t count = 0;
        probes[count++] = 0; probes[count++] = 1; probes[count++] = H;
        probes[count++] = UINT64_MAX; probes[count++] = UINT64_MAX - 1;
        for (size_t k = 0; k < sizeof(below) / sizeof(below[0]); ++k) if (H >= below[k]) probes[count++] = H - below[k];
        for (size_t k = 0; k < sizeof(above) / sizeof(above[0]); ++k) if (UINT64_MAX - H > above[k]) probes[count++] = H + above[k];
        fips_replay_window w;
        seen s = {{0}, 0, 0};
        fips_replay_window_init(&w);
        CHECK(fips_replay_window_check(&w, 0) && !fips_replay_window_check(&w, UINT64_MAX));
        CHECK(mark_checked(&w, &s, H) == 0);
        CHECK(probe_all(&w, &s, probes, count) == 0);
        /* Mark every probe in turn, including rejected ones, re-verifying all. */
        for (size_t k = 0; k < count; ++k) {
            CHECK(mark_checked(&w, &s, probes[k]) == 0);
            CHECK(probe_all(&w, &s, probes, count) == 0);
        }
        /* Far-future jump: a full reset; only the new counter is seen. */
        if (UINT64_MAX - s.high > (UINT64_C(1) << 40) + FIPS_REPLAY_WINDOW_SIZE) {
            uint64_t J = s.high + (UINT64_C(1) << 40);
            CHECK(mark_checked(&w, &s, J) == 0);
            CHECK(probe_all(&w, &s, probes, count) == 0);
            CHECK(fips_replay_window_check(&w, J - (FIPS_REPLAY_WINDOW_SIZE - 1)));
            CHECK(!fips_replay_window_check(&w, J - FIPS_REPLAY_WINDOW_SIZE));
            CHECK(mark_checked(&w, &s, J - (FIPS_REPLAY_WINDOW_SIZE - 1)) == 0);
            /* Shift by exactly the window: everything up to J falls out, J+1 is still open. */
            CHECK(mark_checked(&w, &s, J + FIPS_REPLAY_WINDOW_SIZE) == 0);
            CHECK(!fips_replay_window_check(&w, J) && fips_replay_window_check(&w, J + 1));
            CHECK(mark_checked(&w, &s, J + 1) == 0);
            CHECK(!fips_replay_window_check(&w, J + 1));
        }
        fips_replay_window_reset(&w);
        CHECK(filled(&w, sizeof(w), 0) && fips_replay_window_check(&w, 0));
        ++scenarios;
    }
    /* Counter 0 is accepted exactly once from a fresh window, and ages out at 8192. */
    fips_replay_window w;
    fips_replay_window_init(&w);
    CHECK(fips_replay_window_mark(&w, 0) && !fips_replay_window_check(&w, 0) && !fips_replay_window_mark(&w, 0));
    fips_replay_window_init(&w);
    CHECK(fips_replay_window_mark(&w, FIPS_REPLAY_WINDOW_SIZE - 1) && fips_replay_window_check(&w, 0));
    CHECK(fips_replay_window_mark(&w, FIPS_REPLAY_WINDOW_SIZE) && !fips_replay_window_check(&w, 0) && !fips_replay_window_mark(&w, 0));
    printf("replay: %zu highest-counter scenarios against the set oracle\n", scenarios);
    return 0;
}

/* Nonce overflow at every layer: refused before any sealing, outputs and state
 * untouched, never a wrapped or reused counter. */
static int nonce_overflow(fixture *f)
{
    uint8_t bytes[32] = {0x11}, plain[4] = {1, 2, 3, 4}, wire[20], out[4];
    size_t len, got;
    fips_aead_key key;
    fips_cipher_state c;
    CHECK(f->xk);
    CHECK(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK);
    fips_cipher_state_init(&c, &key);
    fips_cipher_state_test_set_counter(&c, UINT64_MAX - 1);
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, sizeof(plain), wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    CHECK(len == sizeof(wire) && fips_cipher_state_counter(&c) == UINT64_MAX);
    uint8_t scratch[sizeof(wire)];
    memset(scratch, 0xa5, sizeof(scratch)); len = SIZE_MAX;
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, sizeof(plain), scratch, sizeof(scratch), &len) == FIPS_NOISE_NonceOverflow);
    CHECK(fips_cipher_state_seal_with_counter(&c, UINT64_MAX, NULL, 0, plain, sizeof(plain), scratch, sizeof(scratch), &len) == FIPS_NOISE_NonceOverflow);
    uint64_t reserved = 5;
    CHECK(fips_cipher_state_take_counter(&c, &reserved) == FIPS_NOISE_NonceOverflow && reserved == 5);
    CHECK(len == SIZE_MAX && filled(scratch, sizeof(scratch), 0xa5) && fips_cipher_state_counter(&c) == UINT64_MAX);
    /* Capacity is checked before the ceiling: both refuse before any crypto. */
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, sizeof(plain), out, sizeof(out), &len) == FIPS_NOISE_MessageTooLarge);
    CHECK(len == SIZE_MAX && fips_cipher_state_counter(&c) == UINT64_MAX);
    /* The explicit-counter open does not refuse the ceiling (upstream parity):
     * the replay window is the layer that does. */
    CHECK(fips_cipher_state_open(&c, UINT64_MAX - 1, NULL, 0, wire, sizeof(wire), out, sizeof(out), &got) == FIPS_NOISE_OK && got == 4);
    fips_cipher_state_destroy(&c);
    fips_aead_key_destroy(&key);
    /* Session: the send counter at the ceiling refuses seal and reservation. */
    fips_noise_session si, sr;
    CHECK(make_sessions(f, &si, &sr) == 0);
    fips_cipher_state_test_set_counter(&si.send_cipher, UINT64_MAX);
    len = SIZE_MAX; memset(wire, 0xa5, sizeof(wire));
    CHECK(fips_noise_session_seal(&si, NULL, 0, plain, sizeof(plain), wire, sizeof(wire), &len) == FIPS_NOISE_NonceOverflow);
    CHECK(fips_noise_session_take_send_counter(&si, &reserved) == FIPS_NOISE_NonceOverflow && reserved == 5);
    CHECK(len == SIZE_MAX && filled(wire, sizeof(wire), 0xa5) && fips_noise_session_current_send_counter(&si) == UINT64_MAX);
    fips_noise_session_destroy(&si);
    fips_noise_session_destroy(&sr);
    /* SymmetricState: a keyed handshake cipher at the ceiling refuses both directions unchanged. */
    fips_symmetric_state sym, sym_before;
    fips_symmetric_copy(&sym, &f->before_write[XK3].state.symmetric);
    CHECK(sym.cipher.has_key);
    sym.cipher.nonce = UINT64_MAX;
    fips_symmetric_copy(&sym_before, &sym);
    uint8_t big[64];
    memset(big, 0xa5, sizeof(big)); len = SIZE_MAX;
    CHECK(fips_symmetric_encrypt_and_hash(&sym, plain, sizeof(plain), big, sizeof(big), &len) == FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW);
    CHECK(fips_symmetric_decrypt_and_hash(&sym, wire, sizeof(wire), big, sizeof(big), &len) == FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW);
    CHECK(len == SIZE_MAX && filled(big, sizeof(big), 0xa5) && !memcmp(&sym, &sym_before, sizeof(sym)));
    fips_symmetric_destroy(&sym);
    fips_symmetric_destroy(&sym_before);
    /* Handshake: XK msg3 is the only step whose cipher is keyed at entry, so an
     * injected ceiling maps through to NonceOverflow with nothing changed. */
    fips_handshake_state w, w_before, r, r_dst;
    uint8_t msg3[FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE];
    fips_handshake_fork(&f->before_write[XK3].state, &w);
    w.symmetric.cipher.nonce = UINT64_MAX;
    fips_handshake_fork(&w, &w_before);
    memset(msg3, 0xa5, sizeof(msg3)); len = SIZE_MAX;
    uint64_t dh = fips_noise_dh_ops(), entropy = f->stream.state;
    CHECK(fips_handshake_write_xk_message_3(&w, msg3, sizeof(msg3), &len) == FIPS_NOISE_NonceOverflow);
    CHECK(len == SIZE_MAX && filled(msg3, sizeof(msg3), 0xa5) && same(&w, &w_before));
    fips_handshake_fork(&f->before_read[XK3].state, &r);
    r.symmetric.cipher.nonce = UINT64_MAX;
    fips_handshake_fork(&r, &w_before);
    memset(&r_dst, 0xa5, sizeof(r_dst));
    CHECK(fips_handshake_fork_xk3(&r, f->msg[XK3], sizes[XK3], &r_dst) == FIPS_NOISE_NonceOverflow);
    CHECK(same(&r, &w_before) && filled(&r_dst, sizeof(r_dst), 0xa5));
    CHECK(fips_noise_dh_ops() == dh && f->stream.state == entropy);
    fips_handshake_destroy(&w); fips_handshake_destroy(&w_before);
    fips_handshake_destroy(&r); fips_handshake_destroy(&r_dst);
    return 0;
}

/* A valid but wrong remote static: IK fails at msg1 after one DH; XK accepts
 * the unauthenticated msg1 and fails at msg2 after one DH. A responder holding
 * a different identity than the one the initiator trusts fails the same way. */
static int wrong_static(fixture *f)
{
    fips_entropy entropy = fips_seeded_entropy_adapter(&f->stream);
    fips_handshake_state i, r, r_before, dst;
    uint8_t wire[FIPS_NOISE_HANDSHAKE_MSG1_SIZE];
    size_t n = 0;
    /* The initiator trusts key 0 (its own) instead of the responder's key 1. */
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&i, f->secp, f->key[0], f->pub[0], f->epoch, entropy)
                 : fips_handshake_new_initiator(&i, f->secp, f->key[0], f->pub[0], f->epoch, entropy)) == FIPS_NOISE_OK);
    CHECK(writers[f->first](&i, wire, sizeof(wire), &n) == FIPS_NOISE_OK && n == sizes[f->first]);
    fips_handshake_fork(&f->before_read[f->first].state, &r);
    fips_handshake_fork(&r, &r_before);
    uint64_t dh = fips_noise_dh_ops();
    if (!f->xk) {
        CHECK(fips_handshake_read_message_1(&r, wire, n) == FIPS_NOISE_DecryptionFailed);
        CHECK(fips_noise_dh_ops() - dh == 1 && !r.has_remote_static && !r.has_remote_epoch);
    } else {
        CHECK(fips_handshake_read_xk_message_1(&r, wire, n) == FIPS_NOISE_OK && fips_noise_dh_ops() - dh == 1);
        CHECK(fips_handshake_write_xk_message_2(&r, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
        fips_handshake_state i_before;
        fips_handshake_fork(&i, &i_before);
        memset(&dst, 0xa5, sizeof(dst));
        dh = fips_noise_dh_ops();
        CHECK(fips_handshake_fork_xk2(&i, wire, n, &dst) == FIPS_NOISE_DecryptionFailed);
        CHECK(fips_noise_dh_ops() - dh == 1 && same(&i, &i_before) && filled(&dst, sizeof(dst), 0xa5));
        fips_handshake_destroy(&i_before);
    }
    fips_handshake_destroy(&i); fips_handshake_destroy(&r); fips_handshake_destroy(&r_before);
    /* The genuine msg1 reaches a responder that holds key 0, not the trusted key 1. */
    CHECK((f->xk ? fips_handshake_new_xk_responder(&r, f->secp, f->key[0], f->epoch, entropy)
                 : fips_handshake_new_responder(&r, f->secp, f->key[0], f->epoch, entropy)) == FIPS_NOISE_OK);
    dh = fips_noise_dh_ops();
    if (!f->xk) {
        CHECK(fips_handshake_read_message_1(&r, f->msg[IK1], sizes[IK1]) == FIPS_NOISE_DecryptionFailed);
        CHECK(fips_noise_dh_ops() - dh == 1);
    } else {
        CHECK(fips_handshake_read_xk_message_1(&r, f->msg[XK1], sizes[XK1]) == FIPS_NOISE_OK);
        CHECK(fips_handshake_write_xk_message_2(&r, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
        memset(&dst, 0xa5, sizeof(dst));
        CHECK(fips_handshake_fork_xk2(&f->before_read[XK2].state, wire, n, &dst) == FIPS_NOISE_DecryptionFailed);
        CHECK(filled(&dst, sizeof(dst), 0xa5));
    }
    fips_handshake_destroy(&r);
    /* Invalid remote statics are refused at construction with the output untouched. */
    static const uint8_t prefixes[] = {0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x82, 0xff};
    for (size_t k = 0; k < sizeof(prefixes); ++k) {
        uint8_t bad[33];
        memcpy(bad, f->pub[1], 33);
        bad[0] = prefixes[k];
        memset(&i, 0xa5, sizeof(i));
        CHECK((f->xk ? fips_handshake_new_xk_initiator(&i, f->secp, f->key[0], bad, f->epoch, entropy)
                     : fips_handshake_new_initiator(&i, f->secp, f->key[0], bad, f->epoch, entropy)) == FIPS_NOISE_InvalidPublicKey);
        CHECK(filled(&i, sizeof(i), 0xa5));
    }
    uint8_t zeros[33] = {0x02}, ffs[33];
    memset(ffs, 0xff, sizeof(ffs));
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&i, f->secp, f->key[0], zeros, f->epoch, entropy)
                 : fips_handshake_new_initiator(&i, f->secp, f->key[0], zeros, f->epoch, entropy)) == FIPS_NOISE_InvalidPublicKey);
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&i, f->secp, f->key[0], ffs, f->epoch, entropy)
                 : fips_handshake_new_initiator(&i, f->secp, f->key[0], ffs, f->epoch, entropy)) == FIPS_NOISE_InvalidPublicKey);
    /* Either parity of the genuine responder static is accepted; the pre-message is normalized. */
    uint8_t odd[33];
    memcpy(odd, f->pub[1], 33);
    odd[0] ^= 1;
    fips_handshake_state even;
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&i, f->secp, f->key[0], odd, f->epoch, entropy)
                 : fips_handshake_new_initiator(&i, f->secp, f->key[0], odd, f->epoch, entropy)) == FIPS_NOISE_OK);
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&even, f->secp, f->key[0], f->pub[1], f->epoch, entropy)
                 : fips_handshake_new_initiator(&even, f->secp, f->key[0], f->pub[1], f->epoch, entropy)) == FIPS_NOISE_OK);
    CHECK(!memcmp(fips_handshake_hash(&i), fips_handshake_hash(&even), 32) && i.remote_static[0] == odd[0]);
    fips_handshake_destroy(&i);
    fips_handshake_destroy(&even);
    return 0;
}

/* A DH that fails inside a step (a corrupted ephemeral scalar) surfaces as
 * Secp256k1 and is counted; fork originals and destinations are untouched. */
static int dh_failure(fixture *f)
{
    unsigned m = f->xk ? XK3 : IK2;
    fips_handshake_state src, before, dst;
    fips_handshake_fork(&f->before_read[m].state, &src);
    CHECK(src.has_ephemeral && !filled(src.ephemeral_secret, 32, 0));
    memset(src.ephemeral_secret, 0, 32);
    fips_handshake_fork(&src, &before);
    memset(&dst, 0xa5, sizeof(dst));
    uint64_t dh = fips_noise_dh_ops();
    CHECK(forkers[m](&src, f->msg[m], sizes[m], &dst) == FIPS_NOISE_Secp256k1);
    CHECK(fips_noise_dh_ops() - dh == 1 && same(&src, &before) && filled(&dst, sizeof(dst), 0xa5));
    /* The ECDH primitive itself refuses a zero scalar and an invalid point with the output untouched. */
    uint8_t out[32], invalid[33] = {0x04};
    memset(out, 0xa5, sizeof(out));
    CHECK(fips_noise_ecdh(f->secp, src.ephemeral_secret, f->pub[1], out) == FIPS_IDENTITY_InvalidSecretKey);
    CHECK(fips_noise_ecdh(f->secp, f->before_read[m].state.ephemeral_secret, invalid, out) == FIPS_IDENTITY_InvalidSecretKey);
    CHECK(filled(out, sizeof(out), 0xa5) && fips_noise_dh_ops() - dh == 3);
    fips_handshake_destroy(&src); fips_handshake_destroy(&before); fips_handshake_destroy(&dst);
    return 0;
}

/* After destroy, every entry point refuses before any work; finish twice is
 * HandshakeNotComplete; an unknown progress value names itself "Invalid". */
static int after_destroy(fixture *f)
{
    fips_handshake_state d, dst;
    fips_aead_key send, recv;
    uint8_t out[FIPS_NOISE_HANDSHAKE_MSG1_SIZE], h[32], pub[33], epoch[8];
    size_t len = SIZE_MAX;
    fips_handshake_fork(&f->i, &d);
    fips_handshake_destroy(&d);
    CHECK(filled(&d, sizeof(d), 0));
    uint64_t dh = fips_noise_dh_ops(), entropy = f->stream.state;
    memset(out, 0xa5, sizeof(out));
    for (unsigned m = 0; m < MESSAGES; ++m) {
        CHECK(writers[m](&d, out, sizeof(out), &len) == FIPS_NOISE_WrongState);
        CHECK(readers[m](&d, f->msg[f->first], sizes[m]) == FIPS_NOISE_WrongState);
        if (forkers[m]) {
            memset(&dst, 0xa5, sizeof(dst));
            CHECK(forkers[m](&d, f->msg[f->first], sizes[m], &dst) == FIPS_NOISE_WrongState);
            CHECK(forkers[m](&d, f->msg[f->first], sizes[m], &d) == FIPS_NOISE_WrongState);
            CHECK(filled(&dst, sizeof(dst), 0xa5));
        }
    }
    memset(&send, 0xa5, sizeof(send)); memset(&recv, 0xa5, sizeof(recv));
    memset(h, 0xa5, 32); memset(pub, 0xa5, 33); memset(epoch, 0xa5, 8);
    CHECK(fips_handshake_finish(&d, &send, &recv, h, pub, epoch) == FIPS_NOISE_HandshakeNotComplete);
    CHECK(len == SIZE_MAX && filled(out, sizeof(out), 0xa5) && filled(&d, sizeof(d), 0));
    CHECK(filled(&send, sizeof(send), 0xa5) && filled(&recv, sizeof(recv), 0xa5) && filled(h, 32, 0xa5));
    CHECK(filled(fips_handshake_hash(&d), 32, 0));
    CHECK(fips_noise_dh_ops() == dh && f->stream.state == entropy);
    CHECK(!strcmp(fips_handshake_progress_name((fips_handshake_progress)99), "Invalid"));
    CHECK(!strcmp(fips_handshake_progress_name(FIPS_HANDSHAKE_COMPLETE), "Complete"));
    /* Double finish and finish with aliased key outputs. */
    fips_handshake_fork(&f->i, &d);
    CHECK(fips_handshake_finish(&d, &send, &send, h, pub, epoch) == FIPS_NOISE_WrongState && same(&d, &f->i));
    CHECK(fips_handshake_finish(&d, &send, &recv, h, pub, epoch) == FIPS_NOISE_OK && filled(&d, sizeof(d), 0));
    CHECK(memcmp(send.bytes, recv.bytes, 32) != 0);
    fips_aead_key_destroy(&send); fips_aead_key_destroy(&recv);
    CHECK(filled(&send, sizeof(send), 0) && filled(&recv, sizeof(recv), 0));
    CHECK(fips_handshake_finish(&d, &send, &recv, h, pub, epoch) == FIPS_NOISE_HandshakeNotComplete);
    CHECK(filled(&send, sizeof(send), 0) && filled(&recv, sizeof(recv), 0));
    /* Cipher and session after destroy. */
    fips_noise_session si, sr, moved;
    fips_cipher_state c;
    uint8_t plain[2] = {1, 2}, wire[18];
    size_t got = SIZE_MAX;
    CHECK(make_sessions(f, &si, &sr) == 0);
    CHECK(fips_noise_session_send_cipher_clone(&si, &c) == FIPS_NOISE_OK);
    fips_cipher_state_destroy(&c);
    CHECK(filled(&c, sizeof(c), 0));
    len = SIZE_MAX;
    CHECK(fips_cipher_state_seal(&c, NULL, 0, plain, 2, wire, sizeof(wire), &len) == FIPS_NOISE_WrongState);
    CHECK(fips_cipher_state_seal_with_counter(&c, 0, NULL, 0, plain, 2, wire, sizeof(wire), &len) == FIPS_NOISE_WrongState);
    CHECK(fips_cipher_state_open(&c, 0, NULL, 0, wire, sizeof(wire), plain, sizeof(plain), &got) == FIPS_NOISE_WrongState);
    uint64_t reserved = 9;
    CHECK(fips_cipher_state_take_counter(&c, &reserved) == FIPS_NOISE_WrongState && reserved == 9);
    CHECK(len == SIZE_MAX && got == SIZE_MAX);
    CHECK(fips_noise_session_seal(&si, NULL, 0, plain, 2, wire, sizeof(wire), &len) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_take_cleared(&sr, &moved) == FIPS_NOISE_OK && filled(&sr, sizeof(sr), 0));
    fips_replay_window window = sr.replay_window;
    CHECK(fips_noise_session_open(&sr, 0, NULL, 0, wire, len, plain, sizeof(plain), &got) == FIPS_NOISE_WrongState);
    CHECK(fips_noise_session_open_unmarked(&sr, 0, NULL, 0, wire, len, plain, sizeof(plain), &got) == FIPS_NOISE_WrongState);
    CHECK(got == SIZE_MAX && !memcmp(&sr.replay_window, &window, sizeof(window)));
    CHECK(fips_noise_session_seal(&sr, NULL, 0, plain, 2, wire, sizeof(wire), &len) == FIPS_NOISE_WrongState);
    CHECK(fips_noise_session_take_send_counter(&sr, &reserved) == FIPS_NOISE_WrongState && reserved == 9);
    memset(&c, 0xa5, sizeof(c));
    CHECK(fips_noise_session_send_cipher_clone(&sr, &c) == FIPS_NOISE_WrongState);
    CHECK(fips_noise_session_recv_cipher_clone(&sr, &c) == FIPS_NOISE_WrongState);
    CHECK(filled(&c, sizeof(c), 0xa5));
    /* The moved session still opens the frame. */
    CHECK(fips_noise_session_open(&moved, 0, NULL, 0, wire, len, plain, sizeof(plain), &got) == FIPS_NOISE_OK && got == 2);
    fips_noise_session_destroy(&si); fips_noise_session_destroy(&moved);
    CHECK(filled(&si, sizeof(si), 0) && filled(&moved, sizeof(moved), 0));
    /* SymmetricState after destroy is the upstream empty CipherState: it passes
     * bytes through unencrypted (finding N-7, upstream parity). The handshake
     * never reaches it because ready() refuses a destroyed state first. */
    fips_symmetric_state sym;
    fips_symmetric_copy(&sym, &f->i.symmetric);
    fips_symmetric_destroy(&sym);
    CHECK(filled(&sym, sizeof(sym), 0));
    memset(wire, 0xa5, sizeof(wire));
    CHECK(fips_symmetric_encrypt_and_hash(&sym, plain, 2, wire, sizeof(wire), &len) == FIPS_SYMMETRIC_OK);
    CHECK(len == 2 && !memcmp(wire, plain, 2) && !filled(sym.h, 32, 0));
    fips_symmetric_destroy(&sym);
    return 0;
}

/* Secret-bearing ranges are nonzero while live and zero after destroy, for a
 * mid-handshake fork, a finished handshake, consumed keys, slots and clones. */
static int zeroization(fixture *f)
{
    unsigned m = f->xk ? XK2 : IK2; /* Initiator mid-handshake: ephemeral, es (IK), ck, keyed cipher. */
    fips_handshake_state s;
    fips_handshake_fork(&f->before_read[m].state, &s);
    CHECK(s.has_ephemeral && !filled(s.ephemeral_secret, 32, 0) && !filled(s.ephemeral_public, 33, 0));
    CHECK(!filled(s.symmetric.ck, 32, 0) && !filled(s.symmetric.h, 32, 0));
    CHECK(s.symmetric.cipher.has_key && !filled(s.symmetric.cipher.key.bytes, 32, 0));
    CHECK(s.has_es == !f->xk && (f->xk || !filled(s.es, 32, 0)));
    CHECK(s.secp != NULL && s.static_keypair != NULL && s.entropy.fill != NULL);
    fips_handshake_destroy(&s);
    CHECK(filled(s.ephemeral_secret, 32, 0) && filled(s.es, 32, 0) && filled(s.symmetric.ck, 32, 0));
    CHECK(filled(s.symmetric.h, 32, 0) && filled(s.symmetric.cipher.key.bytes, 32, 0));
    CHECK(filled(s.remote_static, 33, 0) && filled(s.remote_ephemeral, 33, 0) && filled(s.local_epoch, 8, 0));
    CHECK(s.secp == NULL && s.static_keypair == NULL && s.entropy.fill == NULL && s.entropy.ctx == NULL);
    CHECK(!s.has_ephemeral && !s.has_es && !s.has_remote_static && !s.has_remote_ephemeral && !s.has_remote_epoch);
    CHECK(filled(&s, sizeof(s), 0));
    /* A discarded fork clears everything a fork copied, including the padding. */
    fips_handshake_state fork;
    fips_handshake_fork(&f->r, &fork);
    CHECK(same(&fork, &f->r));
    fips_handshake_destroy(&fork);
    CHECK(filled(&fork, sizeof(fork), 0));
    /* finish wipes the state and the two keys are the caller's; the session
     * constructor wipes those, take_cleared/clear_slot wipe the slot. */
    fips_noise_session si, sr, slot;
    fips_cipher_state clone, moved;
    CHECK(make_sessions(f, &si, &sr) == 0);
    CHECK(!filled(si.send_cipher.key.bytes, 32, 0) && !filled(si.recv_cipher.key.bytes, 32, 0));
    CHECK(fips_noise_session_recv_cipher_clone(&sr, &clone) == FIPS_NOISE_OK);
    CHECK(!memcmp(clone.key.bytes, si.send_cipher.key.bytes, 32));
    CHECK(fips_cipher_state_take_cleared(&clone, &moved) == FIPS_NOISE_OK);
    CHECK(filled(&clone, sizeof(clone), 0) && !memcmp(moved.key.bytes, si.send_cipher.key.bytes, 32));
    fips_cipher_state_clear_slot(&moved);
    CHECK(filled(&moved, sizeof(moved), 0));
    CHECK(fips_noise_session_take_cleared(&si, &slot) == FIPS_NOISE_OK && filled(&si, sizeof(si), 0));
    fips_noise_session_clear_slot(&slot);
    fips_noise_session_destroy(&sr);
    CHECK(filled(&slot, sizeof(slot), 0) && filled(&sr, sizeof(sr), 0));
    /* AEAD key and SymmetricState destroy. */
    fips_aead_key key;
    uint8_t bytes[32];
    memset(bytes, 0x42, 32);
    CHECK(fips_aead_key_init(&key, bytes) == FIPS_AEAD_OK && !filled(&key, sizeof(key), 0));
    fips_aead_key_destroy(&key);
    CHECK(filled(&key, sizeof(key), 0));
    fips_symmetric_state sym;
    fips_symmetric_copy(&sym, &f->i.symmetric);
    CHECK(!filled(sym.ck, 32, 0) && !filled(sym.cipher.key.bytes, 32, 0));
    fips_symmetric_destroy(&sym);
    CHECK(filled(&sym, sizeof(sym), 0));
    return 0;
}

/* Worker clones are bare keys (finding N-1): they seal only with explicit,
 * dispatcher-reserved counters and can never auto-issue one. */
static int clone_counters(fixture *f)
{
    fips_noise_session si, sr;
    fips_cipher_state a, b, recv;
    uint8_t plain[3] = {7, 8, 9}, wa[19], wb[19], out[3];
    size_t len = SIZE_MAX, got;
    uint64_t ca, cb, reserved = 77;
    CHECK(make_sessions(f, &si, &sr) == 0);
    uint8_t si_before[sizeof(si)], sr_before[sizeof(sr)];
    memcpy(si_before, &si, sizeof(si));
    memcpy(sr_before, &sr, sizeof(sr));
    CHECK(fips_noise_session_send_cipher_clone(&si, &a) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_send_cipher_clone(&si, &b) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_recv_cipher_clone(&sr, &recv) == FIPS_NOISE_OK);
    /* Cloning reads the session only. */
    CHECK(!memcmp(si_before, &si, sizeof(si)) && !memcmp(sr_before, &sr, sizeof(sr)));
    sodium_memzero(si_before, sizeof(si_before));
    sodium_memzero(sr_before, sizeof(sr_before));
    CHECK(a.has_key && !memcmp(a.key.bytes, si.send_cipher.key.bytes, 32) && !memcmp(a.key.bytes, recv.key.bytes, 32));
    CHECK(fips_cipher_state_counter(&a) == UINT64_MAX && fips_cipher_state_counter(&b) == UINT64_MAX);
    CHECK(fips_cipher_state_counter(&recv) == UINT64_MAX);
    memset(wa, 0xa5, sizeof(wa));
    CHECK(fips_cipher_state_seal(&a, NULL, 0, plain, sizeof(plain), wa, sizeof(wa), &len) == FIPS_NOISE_NonceOverflow);
    CHECK(fips_cipher_state_take_counter(&a, &reserved) == FIPS_NOISE_NonceOverflow && reserved == 77);
    CHECK(len == SIZE_MAX && filled(wa, sizeof(wa), 0xa5) && fips_noise_session_current_send_counter(&si) == 0);
    CHECK(fips_noise_session_take_send_counter(&si, &ca) == FIPS_NOISE_OK && ca == 0);
    CHECK(fips_noise_session_take_send_counter(&si, &cb) == FIPS_NOISE_OK && cb == 1);
    CHECK(fips_cipher_state_seal_with_counter(&a, ca, NULL, 0, plain, sizeof(plain), wa, sizeof(wa), &len) == FIPS_NOISE_OK);
    CHECK(fips_cipher_state_seal_with_counter(&b, cb, NULL, 0, plain, sizeof(plain), wb, sizeof(wb), &len) == FIPS_NOISE_OK);
    CHECK(memcmp(wa, wb, sizeof(wa)) != 0 && fips_cipher_state_counter(&a) == UINT64_MAX);
    CHECK(fips_cipher_state_open(&recv, ca, NULL, 0, wa, sizeof(wa), out, sizeof(out), &got) == FIPS_NOISE_OK && got == 3);
    CHECK(fips_noise_session_open(&sr, cb, NULL, 0, wb, sizeof(wb), out, sizeof(out), &got) == FIPS_NOISE_OK && got == 3);
    CHECK(fips_noise_session_accept_replay(&sr, ca) == FIPS_NOISE_OK && fips_noise_session_accept_replay(&sr, ca) == FIPS_NOISE_ReplayDetected);
    /* The session's own auto-seal continues from the reserved counters. */
    CHECK(fips_noise_session_seal(&si, NULL, 0, plain, sizeof(plain), wa, sizeof(wa), &len) == FIPS_NOISE_OK);
    CHECK(fips_noise_session_open(&sr, 2, NULL, 0, wa, sizeof(wa), out, sizeof(out), &got) == FIPS_NOISE_OK);
    fips_cipher_state_destroy(&a); fips_cipher_state_destroy(&b); fips_cipher_state_destroy(&recv);
    fips_noise_session_destroy(&si); fips_noise_session_destroy(&sr);
    return 0;
}

static int pattern(bool xk, uint64_t seed)
{
    fixture f;
    CHECK(init(&f, xk, seed) == 0);
    for (unsigned m = f.first; m <= f.last; ++m) {
        CHECK(tamper_matrix(&f, m) == 0);
        CHECK(length_matrix(&f, m) == 0);
        CHECK(capacity_matrix(&f, m) == 0);
    }
    CHECK(frame_matrix(&f) == 0);
    if (!xk) CHECK(transport_length_matrix(&f) == 0);
    if (xk) CHECK(nonce_overflow(&f) == 0);
    CHECK(wrong_static(&f) == 0);
    CHECK(dh_failure(&f) == 0);
    CHECK(after_destroy(&f) == 0);
    CHECK(zeroization(&f) == 0);
    CHECK(clone_counters(&f) == 0);
    destroy(&f);
    CHECK(filled(&f, sizeof(f), 0));
    return 0;
}

int main(void)
{
    CHECK(sodium_init() >= 0);
    CHECK(pattern(false, UINT64_C(0x494b)) == 0);
    CHECK(pattern(true, UINT64_C(0x584b)) == 0);
    CHECK(replay_matrix() == 0);
    puts("noise negative: tamper/length/capacity/frame/replay/nonce/wrong-key/misuse/zeroization matrices passed");
    return 0;
}
