#include <fips/noise/handshake.h>
#include "../tests/fakes/host.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

typedef fips_noise_status (*reader)(fips_handshake_state *, const uint8_t *, size_t);
typedef fips_noise_status (*fork_reader)(const fips_handshake_state *, const uint8_t *, size_t, fips_handshake_state *);
static fips_secp *ctx;
static fips_identity *keys[2];
static fips_fixed_entropy fixed = {0x5a};
static fips_seeded_entropy streams[2];
static fips_handshake_state responder1[2], initiator2[2], responder3;
static void require(bool ok) { if (!ok) abort(); }
static void cleanup(void)
{
    for (size_t k = 0; k < 2; ++k) {
        fips_handshake_destroy(&responder1[k]); fips_handshake_destroy(&initiator2[k]);
        fips_identity_destroy(keys[k]);
    }
    fips_handshake_destroy(&responder3);
    fips_identity_destroy_secp(ctx);
}
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    fips_entropy blind = fips_fixed_entropy_adapter(&fixed);
    require(fips_identity_init_secp(blind, &ctx) == FIPS_IDENTITY_OK);
    require(atexit(cleanup) == 0);
    for (size_t k = 0; k < 2; ++k) {
        fips_seeded_entropy state = {(uint64_t)k ^ UINT64_C(0x6b65797061697273)};
        fips_entropy keyed = fips_seeded_entropy_adapter(&state);
        uint8_t secret[32]; keyed.fill(keyed.ctx, secret, sizeof(secret));
        fips_identity_status rc = fips_identity_from_secret_bytes(ctx, blind, secret, &keys[k]);
        sodium_memzero(secret, sizeof(secret));
        require(rc == FIPS_IDENTITY_OK);
    }
    uint8_t remote[33]; fips_identity_pubkey_full(keys[1], remote);
    const uint8_t epoch[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    for (size_t xk = 0; xk < 2; ++xk) {
        streams[xk].state = xk ? 0x584b : 0x494b;
        fips_entropy entropy = fips_seeded_entropy_adapter(&streams[xk]);
        fips_handshake_state i, r;
        require((xk ? fips_handshake_new_xk_initiator(&i, ctx, keys[0], remote, epoch, entropy)
                    : fips_handshake_new_initiator(&i, ctx, keys[0], remote, epoch, entropy)) == FIPS_NOISE_OK);
        require((xk ? fips_handshake_new_xk_responder(&r, ctx, keys[1], epoch, entropy)
                    : fips_handshake_new_responder(&r, ctx, keys[1], epoch, entropy)) == FIPS_NOISE_OK);
        fips_handshake_fork(&r, &responder1[xk]);
        uint8_t wire[106]; size_t n;
        require((xk ? fips_handshake_write_xk_message_1(&i, wire, sizeof(wire), &n)
                    : fips_handshake_write_message_1(&i, wire, sizeof(wire), &n)) == FIPS_NOISE_OK);
        fips_handshake_fork(&i, &initiator2[xk]);
        require((xk ? fips_handshake_read_xk_message_1(&r, wire, n)
                    : fips_handshake_read_message_1(&r, wire, n)) == FIPS_NOISE_OK);
        require((xk ? fips_handshake_write_xk_message_2(&r, wire, sizeof(wire), &n)
                    : fips_handshake_write_message_2(&r, wire, sizeof(wire), &n)) == FIPS_NOISE_OK);
        if (xk) fips_handshake_fork(&r, &responder3);
        fips_handshake_destroy(&i); fips_handshake_destroy(&r);
    }
    return 0;
}
static void read_on_fork(const fips_handshake_state *s, const uint8_t *data, size_t n,
                         reader raw, fork_reader forked)
{
    fips_handshake_state before, direct, out, sentinel;
    fips_handshake_fork(s, &before); fips_handshake_fork(s, &direct);
    memset(&out, 0xa5, sizeof(out)); memcpy(&sentinel, &out, sizeof(out));
    fips_noise_status a = raw(&direct, data, n), b = forked(s, data, n, &out);
    require(a == b && !memcmp(s, &before, sizeof(before)));
    require(a == FIPS_NOISE_OK ? !memcmp(&out, &direct, sizeof(out)) : !memcmp(&out, &sentinel, sizeof(out)));
    fips_handshake_destroy(&before); fips_handshake_destroy(&direct);
    fips_handshake_destroy(&out); fips_handshake_destroy(&sentinel);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    for (size_t xk = 0; xk < 2; ++xk) {
        fips_handshake_state copy, before;
        fips_handshake_fork(&responder1[xk], &copy);
        fips_handshake_fork(&responder1[xk], &before);
        fips_noise_status rc = xk ? fips_handshake_read_xk_message_1(&copy, data, size)
                                 : fips_handshake_read_message_1(&copy, data, size);
        require(!memcmp(&responder1[xk], &before, sizeof(before)));
        if (rc == FIPS_NOISE_OK) require(copy.progress == FIPS_HANDSHAKE_MESSAGE1_DONE);
        fips_handshake_destroy(&copy); fips_handshake_destroy(&before);
        read_on_fork(&initiator2[xk], data, size,
            xk ? fips_handshake_read_xk_message_2 : fips_handshake_read_message_2,
            xk ? fips_handshake_fork_xk2 : fips_handshake_fork_msg2);
    }
    read_on_fork(&responder3, data, size, fips_handshake_read_xk_message_3, fips_handshake_fork_xk3);
    return 0;
}
