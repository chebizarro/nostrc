#ifndef FIPS_REKEY_NOISE_PROFILE_INTERNAL_H
#define FIPS_REKEY_NOISE_PROFILE_INTERNAL_H
#include <fips/proto/rekey/keys.h>
#include <sodium.h>
#include <string.h>
/* Consumes the fork; finish and every failed intermediate are erased. */
static bool noise_completed(fips_handshake_state *hs, fips_rekey_completed *done) {
    fips_aead_key send = {0}, recv = {0};
    uint8_t hash[32] = {0}, remote[33] = {0}, epoch[8] = {0};
    fips_handshake_role role = hs->role;
    bool ok = fips_handshake_finish(hs, &send, &recv, hash, remote, epoch) == FIPS_NOISE_OK;
    if (ok) {
        ok = fips_noise_session_from_handshake(&done->session, role, &send, &recv,
                                               hash, remote, epoch) == FIPS_NOISE_OK;
        if (ok) {
            memcpy(done->remote.bytes, remote + 1, 32);
            memcpy(done->epoch, epoch, 8);
        }
    }
    fips_aead_key_destroy(&send);
    fips_aead_key_destroy(&recv);
    fips_handshake_destroy(hs);
    sodium_memzero(hash, sizeof hash);
    sodium_memzero(remote, sizeof remote);
    sodium_memzero(epoch, sizeof epoch);
    return ok;
}
static uint32_t elapsed_ms(fips_duration d) {
    /* Deployed sender truncates to 32 bits (including for enormous durations). */
    return (uint32_t)d.secs * UINT32_C(1000) + d.nanos / UINT32_C(1000000);
}
#endif
