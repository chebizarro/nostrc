#include <fips/noise/handshake.h>
#include <fips/noise/ecdh.h>
#include <fips/proto/framing.h>
#include "diff/diff.h"
#include "fakes/host.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

typedef fips_noise_status (*writer)(fips_handshake_state *, uint8_t *, size_t, size_t *);
typedef fips_noise_status (*reader)(fips_handshake_state *, const uint8_t *, size_t);
typedef fips_noise_status (*fork_reader)(const fips_handshake_state *, const uint8_t *, size_t, fips_handshake_state *);
typedef struct fixture {
    fips_fixed_entropy fixed;
    fips_seeded_entropy stream;
    fips_secp *secp;
    fips_identity *key[2];
    uint8_t pub[2][33];
    diff_bytes epoch, msg[3];
    bool xk;
    fips_handshake_state i, r;
} fixture;

static int filled(const void *data, size_t len, uint8_t byte)
{
    const uint8_t *p = data;
    for (size_t j = 0; j < len; ++j) if (p[j] != byte) return 0;
    return 1;
}
static int same(const fips_handshake_state *a, const fips_handshake_state *b)
{ return memcmp(a, b, sizeof(*a)) == 0; }
static int init(fixture *f, diff_value *p)
{
    memset(f, 0, sizeof(*f));
    f->fixed.byte = 0x5a;
    fips_entropy blind = fips_fixed_entropy_adapter(&f->fixed);
    CHECK(fips_identity_init_secp(blind, &f->secp) == FIPS_IDENTITY_OK);
    diff_value *seeds = FIELD(p, "keypair_seeds");
    for (size_t k = 0; k < 2; ++k) {
        fips_seeded_entropy state = {ITEM(seeds, k)->as.number ^ UINT64_C(0x6b65797061697273)};
        fips_entropy seeded = fips_seeded_entropy_adapter(&state);
        uint8_t secret[32];
        seeded.fill(seeded.ctx, secret, 32);
        fips_identity_status rc = fips_identity_from_secret_bytes(f->secp, blind, secret, &f->key[k]);
        sodium_memzero(secret, sizeof(secret));
        CHECK(rc == FIPS_IDENTITY_OK);
        fips_identity_pubkey_full(f->key[k], f->pub[k]);
    }
    f->stream.state = UINT(p, "entropy_seed_hex");
    fips_entropy entropy = fips_seeded_entropy_adapter(&f->stream);
    f->epoch = BYTES(p, "startup_epoch_hex");
    f->xk = !strcmp(TEXT(p, "pattern"), "XK");
    CHECK(f->epoch.len == 8);
    for (size_t k = 0; k < (f->xk ? 3u : 2u); ++k)
        f->msg[k] = BYTES(ITEM(FIELD(p, "messages"), k), "bytes");
    CHECK((f->xk ? fips_handshake_new_xk_initiator(&f->i, f->secp, f->key[0], f->pub[1], f->epoch.data, entropy)
                 : fips_handshake_new_initiator(&f->i, f->secp, f->key[0], f->pub[1], f->epoch.data, entropy)) == FIPS_NOISE_OK);
    CHECK((f->xk ? fips_handshake_new_xk_responder(&f->r, f->secp, f->key[1], f->epoch.data, entropy)
                 : fips_handshake_new_responder(&f->r, f->secp, f->key[1], f->epoch.data, entropy)) == FIPS_NOISE_OK);
    CHECK(f->stream.state == UINT(p, "entropy_seed_hex")); /* Constructors draw nothing. */
    return 0;
}
static void destroy(fixture *f)
{
    fips_handshake_destroy(&f->i); fips_handshake_destroy(&f->r);
    fips_identity_destroy(f->key[0]); fips_identity_destroy(f->key[1]);
    fips_identity_destroy_secp(f->secp);
    sodium_memzero(f, sizeof(*f));
}

/* Every API is rejected on every inapplicable role/pattern/progress combination.
 * Applicable calls are tested with every short size/capacity plus overlong and
 * SIZE_MAX lengths. No work, entropy, DH, output or state change is permitted. */
static int misuse(fips_handshake_state *s)
{
    const writer writes[] = {fips_handshake_write_message_1, fips_handshake_write_message_2,
        fips_handshake_write_xk_message_1, fips_handshake_write_xk_message_2, fips_handshake_write_xk_message_3};
    const reader reads[] = {fips_handshake_read_message_1, fips_handshake_read_message_2,
        fips_handshake_read_xk_message_1, fips_handshake_read_xk_message_2, fips_handshake_read_xk_message_3};
    const size_t sizes[] = {106, 57, 33, 57, 73};
    const fips_handshake_progress progress[] = {FIPS_HANDSHAKE_INITIAL, FIPS_HANDSHAKE_MESSAGE1_DONE,
        FIPS_HANDSHAKE_INITIAL, FIPS_HANDSHAKE_MESSAGE1_DONE, FIPS_HANDSHAKE_MESSAGE2_DONE};
    const fips_handshake_role roles[] = {FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_RESPONDER,
        FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_RESPONDER, FIPS_HANDSHAKE_INITIATOR};
    fips_handshake_state before;
    fips_handshake_fork(s, &before);
    uint64_t dh = fips_noise_dh_ops();
    uint8_t out[107]; memset(out, 0xa5, sizeof(out));
    for (size_t k = 0; k < 5; ++k) {
        bool applicable = (k < 2 ? FIPS_NOISE_IK : FIPS_NOISE_XK) == s->pattern && progress[k] == s->progress;
        size_t n = SIZE_MAX;
        if (applicable && s->role == roles[k]) {
            for (size_t cap = 0; cap < sizes[k]; ++cap)
                CHECK(writes[k](s, out, cap, &n) == FIPS_NOISE_MessageTooLarge);
        } else CHECK(writes[k](s, out, sizeof(out), &n) == FIPS_NOISE_WrongState);
        CHECK(n == SIZE_MAX && filled(out, sizeof(out), 0xa5) && same(s, &before));
        if (applicable && s->role != roles[k]) {
            for (size_t len = 0; len < sizes[k]; ++len)
                CHECK(reads[k](s, len ? out : NULL, len) == FIPS_NOISE_MessageTooShort);
            CHECK(reads[k](s, out, sizes[k] + 1) == FIPS_NOISE_MessageTooShort);
            CHECK(reads[k](s, out, SIZE_MAX) == FIPS_NOISE_MessageTooShort);
        } else CHECK(reads[k](s, out, sizes[k]) == FIPS_NOISE_WrongState);
        CHECK(same(s, &before));
    }
    const fork_reader forks[] = {fips_handshake_fork_msg2, fips_handshake_fork_xk2, fips_handshake_fork_xk3};
    for (size_t k = 0; k < 3; ++k) {
        fips_handshake_state dst, sentinel;
        memset(&dst, 0xa5, sizeof(dst)); memcpy(&sentinel, &dst, sizeof(dst));
        CHECK(forks[k](s, NULL, 0, &dst) != FIPS_NOISE_OK);
        CHECK(same(&dst, &sentinel) && same(s, &before));
        CHECK(forks[k](s, out, sizeof(out), s) == FIPS_NOISE_WrongState);
        CHECK(same(s, &before));
        fips_handshake_destroy(&dst); fips_handshake_destroy(&sentinel);
    }
    fips_aead_key send, recv;
    uint8_t h[32], pub[33], epoch[8];
    memset(&send, 0xa5, sizeof(send)); memset(&recv, 0xa5, sizeof(recv));
    memset(h, 0xa5, sizeof(h)); memset(pub, 0xa5, sizeof(pub)); memset(epoch, 0xa5, sizeof(epoch));
    CHECK(fips_handshake_finish(s, &send, s->progress == FIPS_HANDSHAKE_COMPLETE ? &send : &recv, h, pub, epoch) ==
        (s->progress == FIPS_HANDSHAKE_COMPLETE ? FIPS_NOISE_WrongState : FIPS_NOISE_HandshakeNotComplete));
    CHECK(same(s, &before) && filled(&send, sizeof(send), 0xa5) && filled(&recv, sizeof(recv), 0xa5));
    CHECK(filled(h, 32, 0xa5) && filled(pub, 33, 0xa5) && filled(epoch, 8, 0xa5));
    CHECK(fips_noise_dh_ops() == dh);
    fips_aead_key_destroy(&send); fips_aead_key_destroy(&recv);
    fips_handshake_destroy(&before);
    return 0;
}

/* Independently pinned by Euler's criterion on y^2 = x^3+7 mod (2^256-2^32-977)
 * for each noise.json XK msg1 byte XOR 1. These and ONLY these offsets encode
 * valid points; msg1 has no AEAD. Offset 0 is 02 <-> 03 (same x). */
static const uint8_t xk1_accepted[] = {0, 1, 2, 3, 4, 5, 8, 9, 11, 12, 13, 15, 19, 20, 22, 23, 27, 29, 31, 32};
static bool accepts_xk1(size_t offset)
{
    for (size_t j = 0; j < sizeof(xk1_accepted); ++j) if (offset == xk1_accepted[j]) return true;
    return false;
}
static int tamper(fixture *f, size_t number, fips_handshake_state *original,
                  reader raw, fork_reader fork_read)
{
    diff_bytes msg = f->msg[number - 1];
    fips_handshake_state before, copy, dst;
    fips_handshake_fork(original, &before);
    size_t accepted = 0, failed = 0, auth_dh_one = 0;
    for (size_t offset = 0; offset < msg.len; ++offset) {
        uint8_t bad[106]; memcpy(bad, msg.data, msg.len); bad[offset] ^= 1;
        bool allow = f->xk && number == 1 ? accepts_xk1(offset) : number < 3 && offset == 0;
        memset(&dst, 0xa5, sizeof(dst));
        fips_handshake_fork(original, &copy);
        uint64_t dh = fips_noise_dh_ops();
        fips_noise_status rc = fork_read ? fork_read(original, bad, msg.len, &dst) : raw(&copy, bad, msg.len);
        uint64_t cost = fips_noise_dh_ops() - dh;
        CHECK(same(original, &before)); /* Entire struct, not only h. */
        CHECK((rc == FIPS_NOISE_OK) == allow);
        if (!f->xk && number == 2)
            CHECK(cost == (rc == FIPS_NOISE_InvalidPublicKey ? 0u : 1u));
        if (allow) {
            ++accepted;
            const fips_handshake_state *advanced = fork_read ? &dst : &copy;
            CHECK(memcmp(fips_handshake_hash(advanced), fips_handshake_hash(original), 32));
            if (number == 1 && f->xk) {
                /* Valid but changed x cannot authenticate msg2 at the initiator.
                 * Parity-only mutation CAN finish; h is NOT AEAD-bound in v1. */
                fips_handshake_state reply, read = {0};
                fips_handshake_fork(advanced, &reply);
                uint8_t wire[57]; size_t n;
                uint64_t entropy_before = f->stream.state;
                CHECK(fips_handshake_write_xk_message_2(&reply, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
                CHECK(fips_handshake_fork_xk2(&f->i, wire, n, &read) ==
                    (offset == 0 ? FIPS_NOISE_OK : FIPS_NOISE_DecryptionFailed));
                f->stream.state = entropy_before;
                if (offset == 0) {
                    CHECK(!memcmp(read.symmetric.ck, reply.symmetric.ck, 32));
                    CHECK(memcmp(fips_handshake_hash(&read), fips_handshake_hash(&reply), 32));
                    uint8_t third[73]; size_t third_len;
                    fips_handshake_state complete;
                    CHECK(fips_handshake_write_xk_message_3(&read, third, sizeof(third), &third_len) == FIPS_NOISE_OK);
                    CHECK(fips_handshake_fork_xk3(&reply, third, third_len, &complete) == FIPS_NOISE_OK);
                    CHECK(read.progress == FIPS_HANDSHAKE_COMPLETE && complete.progress == FIPS_HANDSHAKE_COMPLETE);
                    CHECK(!memcmp(read.symmetric.ck, complete.symmetric.ck, 32));
                    CHECK(memcmp(fips_handshake_hash(&read), fips_handshake_hash(&complete), 32));
                    fips_handshake_destroy(&complete);
                }
                fips_handshake_destroy(&reply); fips_handshake_destroy(&read);
            }
        } else {
            ++failed;
            CHECK(rc == FIPS_NOISE_InvalidPublicKey || rc == FIPS_NOISE_DecryptionFailed);
            if (fork_read) CHECK(filled(&dst, sizeof(dst), 0xa5));
            if (!f->xk && number == 2 && rc == FIPS_NOISE_DecryptionFailed) {
                CHECK(cost == 1); ++auth_dh_one;
            }
            /* Every encrypted payload/tag byte must fail authentication. */
            if ((number == 1 && offset >= 33) || (number == 2 && offset >= 33) || number == 3)
                CHECK(rc == FIPS_NOISE_DecryptionFailed);
        }
        fips_handshake_destroy(&copy); fips_handshake_destroy(&dst);
        CHECK(filled(&copy, sizeof(copy), 0) && filled(&dst, sizeof(dst), 0));
    }
    CHECK(accepted == (f->xk && number == 1 ? sizeof(xk1_accepted) : number < 3 ? 1u : 0u));
    if (!f->xk && number == 2) CHECK(auth_dh_one >= 24);
    printf("%s msg%zu XOR 1 at all %zu offsets: %zu rejected, %zu accepted%s\n",
        f->xk ? "XK" : "IK", number, msg.len, failed, accepted,
        !f->xk && number == 2 ? "; every authentication failure dh_ops == 1" : "");
    fips_handshake_destroy(&before);
    return 0;
}

static int transcript(diff_value *p, bool remote_parity)
{
    fixture f;
    CHECK(init(&f, p) == 0);
    if (remote_parity) {
        uint8_t remote[33]; memcpy(remote, f.pub[1], 33); remote[0] ^= 1;
        fips_handshake_destroy(&f.i);
        fips_entropy entropy = fips_seeded_entropy_adapter(&f.stream);
        CHECK((f.xk ? fips_handshake_new_xk_initiator(&f.i, f.secp, f.key[0], remote, f.epoch.data, entropy)
                    : fips_handshake_new_initiator(&f.i, f.secp, f.key[0], remote, f.epoch.data, entropy)) == FIPS_NOISE_OK);
    }
    CHECK(misuse(&f.i) == 0 && misuse(&f.r) == 0);
    uint8_t wire[106]; size_t n;
    CHECK((f.xk ? fips_handshake_write_xk_message_1(&f.i, wire, sizeof(wire), &n)
                : fips_handshake_write_message_1(&f.i, wire, sizeof(wire), &n)) == FIPS_NOISE_OK);
    CHECK(n == f.msg[0].len && !memcmp(wire, f.msg[0].data, n));
    CHECK(f.i.has_ephemeral && f.i.has_es == !f.xk);
    CHECK(misuse(&f.i) == 0);
    if (!remote_parity) CHECK(tamper(&f, 1, &f.r,
        f.xk ? fips_handshake_read_xk_message_1 : fips_handshake_read_message_1, NULL) == 0);
    CHECK((f.xk ? fips_handshake_read_xk_message_1(&f.r, wire, n)
                : fips_handshake_read_message_1(&f.r, wire, n)) == FIPS_NOISE_OK);
    CHECK(f.r.has_remote_static == !f.xk && f.r.has_remote_epoch == !f.xk);
    CHECK(misuse(&f.r) == 0);
    CHECK((f.xk ? fips_handshake_write_xk_message_2(&f.r, wire, sizeof(wire), &n)
                : fips_handshake_write_message_2(&f.r, wire, sizeof(wire), &n)) == FIPS_NOISE_OK);
    CHECK(n == f.msg[1].len && !memcmp(wire, f.msg[1].data, n));
    CHECK(misuse(&f.r) == 0);
    if (!remote_parity) CHECK(tamper(&f, 2, &f.i, NULL,
        f.xk ? fips_handshake_fork_xk2 : fips_handshake_fork_msg2) == 0);
    fips_handshake_state copy, before;
    fips_handshake_fork(&f.i, &before);
    CHECK((f.xk ? fips_handshake_fork_xk2(&f.i, wire, n, &copy)
                : fips_handshake_fork_msg2(&f.i, wire, n, &copy)) == FIPS_NOISE_OK);
    CHECK(same(&f.i, &before));
    fips_handshake_destroy(&before); fips_handshake_destroy(&f.i);
    fips_handshake_fork(&copy, &f.i); fips_handshake_destroy(&copy);
    CHECK(misuse(&f.i) == 0);
    if (f.xk) {
        CHECK(fips_handshake_write_xk_message_3(&f.i, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
        CHECK(n == f.msg[2].len && !memcmp(wire, f.msg[2].data, n));
        if (!remote_parity) CHECK(tamper(&f, 3, &f.r, NULL, fips_handshake_fork_xk3) == 0);
        fips_handshake_fork(&f.r, &before);
        CHECK(fips_handshake_fork_xk3(&f.r, wire, n, &copy) == FIPS_NOISE_OK);
        CHECK(same(&f.r, &before));
        fips_handshake_destroy(&before); fips_handshake_destroy(&f.r);
        fips_handshake_fork(&copy, &f.r); fips_handshake_destroy(&copy);
    }
    CHECK(misuse(&f.i) == 0 && misuse(&f.r) == 0);
    CHECK(f.i.progress == FIPS_HANDSHAKE_COMPLETE && f.r.progress == FIPS_HANDSHAKE_COMPLETE);
    CHECK(!memcmp(fips_handshake_hash(&f.i), fips_handshake_hash(&f.r), 32));
    CHECK(!memcmp(f.i.remote_epoch, f.epoch.data, 8) && !memcmp(f.r.remote_epoch, f.epoch.data, 8));
    fips_handshake_fork(&f.i, &copy);
    fips_handshake_fork(&copy, &copy); /* Generic self-fork is explicitly a no-op. */
    CHECK(same(&f.i, &copy));
    fips_handshake_destroy(&copy);
    CHECK(filled(&copy, sizeof(copy), 0));
    fips_handshake_destroy(&copy);
    fips_aead_key is, ir, rs, rr;
    uint8_t ih[32], rh[32], ip[33], rp[33], ie[8], re[8];
    CHECK(fips_handshake_finish(&f.i, &is, &ir, ih, ip, ie) == FIPS_NOISE_OK);
    CHECK(fips_handshake_finish(&f.r, &rs, &rr, rh, rp, re) == FIPS_NOISE_OK);
    CHECK(filled(&f.i, sizeof(f.i), 0) && filled(&f.r, sizeof(f.r), 0));
    CHECK(!memcmp(is.bytes, rr.bytes, 32) && !memcmp(ir.bytes, rs.bytes, 32) && memcmp(is.bytes, ir.bytes, 32));
    CHECK(!memcmp(ih, rh, 32) && !memcmp(ie, re, 8));
    CHECK(!memcmp(ip + 1, f.pub[1] + 1, 32) && !memcmp(rp, f.pub[0], 33));
    CHECK(fips_handshake_finish(&f.i, &is, &ir, ih, ip, ie) == FIPS_NOISE_HandshakeNotComplete);
    fips_aead_key_destroy(&is); fips_aead_key_destroy(&ir);
    fips_aead_key_destroy(&rs); fips_aead_key_destroy(&rr);
    destroy(&f);
    CHECK(filled(&f, sizeof(f), 0));
    return 0;
}
static void zero_fill(void *ctx, uint8_t *out, size_t len)
{ (void)ctx; memset(out, 0, len); }
static int wrong_keys(diff_value *p)
{
    fixture f;
    CHECK(init(&f, p) == 0);
    fips_entropy entropy = fips_seeded_entropy_adapter(&f.stream);
    fips_handshake_state copy, sentinel;
    memset(&copy, 0xa5, sizeof(copy)); memcpy(&sentinel, &copy, sizeof(copy));
    uint8_t invalid[33] = {0};
    CHECK((f.xk ? fips_handshake_new_xk_initiator(&copy, f.secp, f.key[0], invalid, f.epoch.data, entropy)
                : fips_handshake_new_initiator(&copy, f.secp, f.key[0], invalid, f.epoch.data, entropy)) == FIPS_NOISE_InvalidPublicKey);
    CHECK(same(&copy, &sentinel));
    fips_secp *other = NULL;
    CHECK(fips_identity_init_secp(fips_fixed_entropy_adapter(&f.fixed), &other) == FIPS_IDENTITY_OK);
    CHECK(fips_handshake_new_responder(&copy, other, f.key[1], f.epoch.data, entropy) == FIPS_NOISE_WrongState);
    CHECK(same(&copy, &sentinel));
    fips_identity_destroy_secp(other);
    fips_handshake_destroy(&copy); fips_handshake_destroy(&sentinel);
    /* Valid but WRONG remote static. XK accepts msg1, but msg2 cannot open. */
    fips_handshake_destroy(&f.i);
    CHECK((f.xk ? fips_handshake_new_xk_initiator(&f.i, f.secp, f.key[0], f.pub[0], f.epoch.data, entropy)
                : fips_handshake_new_initiator(&f.i, f.secp, f.key[0], f.pub[0], f.epoch.data, entropy)) == FIPS_NOISE_OK);
    uint8_t wire[106]; size_t n;
    CHECK((f.xk ? fips_handshake_write_xk_message_1(&f.i, wire, sizeof(wire), &n)
                : fips_handshake_write_message_1(&f.i, wire, sizeof(wire), &n)) == FIPS_NOISE_OK);
    if (f.xk) {
        CHECK(fips_handshake_read_xk_message_1(&f.r, wire, n) == FIPS_NOISE_OK);
        CHECK(fips_handshake_write_xk_message_2(&f.r, wire, sizeof(wire), &n) == FIPS_NOISE_OK);
        CHECK(fips_handshake_fork_xk2(&f.i, wire, n, &copy) == FIPS_NOISE_DecryptionFailed);
    } else CHECK(fips_handshake_read_message_1(&f.r, wire, n) == FIPS_NOISE_DecryptionFailed);
    destroy(&f);
    /* Invalid host scalars fail, never retry/abort, and never publish output. */
    CHECK(init(&f, p) == 0);
    f.i.entropy = (fips_entropy){zero_fill, NULL};
    memset(wire, 0xa5, sizeof(wire)); n = SIZE_MAX;
    CHECK((f.xk ? fips_handshake_write_xk_message_1(&f.i, wire, sizeof(wire), &n)
                : fips_handshake_write_message_1(&f.i, wire, sizeof(wire), &n)) == FIPS_NOISE_Secp256k1);
    CHECK(n == SIZE_MAX && filled(wire, sizeof(wire), 0xa5));
    CHECK(filled(f.i.ephemeral_secret, 32, 0) && !f.i.has_ephemeral);
    CHECK((f.xk ? fips_handshake_read_xk_message_1(&f.r, f.msg[0].data, f.msg[0].len)
                : fips_handshake_read_message_1(&f.r, f.msg[0].data, f.msg[0].len)) == FIPS_NOISE_OK);
    f.r.entropy = (fips_entropy){zero_fill, NULL};
    CHECK((f.xk ? fips_handshake_write_xk_message_2(&f.r, wire, sizeof(wire), &n)
                : fips_handshake_write_message_2(&f.r, wire, sizeof(wire), &n)) == FIPS_NOISE_Secp256k1);
    CHECK(n == SIZE_MAX && filled(wire, sizeof(wire), 0xa5) && filled(f.r.ephemeral_secret, 32, 0));
    destroy(&f);
    return 0;
}
int main(void)
{
    CHECK(sodium_init() >= 0);
    CHECK(FIPS_NOISE_HANDSHAKE_MSG1_SIZE == FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE);
    CHECK(FIPS_NOISE_HANDSHAKE_MSG2_SIZE == FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE);
    CHECK(FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE == 33 && FIPS_NOISE_XK_HANDSHAKE_MSG2_SIZE == 57 && FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE == 73);
    diff_context ctx = {.dir = FIPS_DIFF_VECTORS, .pin = FIPS_DIFF_PIN};
    diff_value *cases = diff_load(&ctx, "noise");
    if (!cases) { fprintf(stderr, "%s\n", ctx.error); return 1; }
    diff_value *patterns = FIELD(cases, "patterns");
    CHECK(COUNT(patterns) == 2);
    for (size_t k = 0; k < COUNT(patterns); ++k) {
        diff_value *p = ITEM(patterns, k);
        CHECK(transcript(p, false) == 0);
        CHECK(transcript(p, true) == 0);
        CHECK(wrong_keys(p) == 0);
    }
    diff_free(cases);
    puts("Handshake: fixture bytes, all-offset tamper, exact accepted offsets, immutable Fork, directional split, misuse, invalid entropy and zeroization passed");
    return 0;
}
