#include <fips/noise/session.h>
#include <sodium.h>
#include <string.h>

fips_noise_status fips_noise_session_from_handshake(fips_noise_session *out,
    fips_handshake_role role, fips_aead_key *send, fips_aead_key *recv,
    const uint8_t final_h[32], const uint8_t remote_static[33], const uint8_t remote_epoch[8])
{
    if ((role != FIPS_HANDSHAKE_INITIATOR && role != FIPS_HANDSHAKE_RESPONDER) || send == recv)
        return FIPS_NOISE_WrongState;
    memset(out, 0, sizeof(*out));
    fips_cipher_state_init(&out->send_cipher, send);
    fips_cipher_state_init(&out->recv_cipher, recv);
    out->role = role;
    memcpy(out->handshake_hash, final_h, sizeof(out->handshake_hash));
    memcpy(out->remote_static, remote_static, sizeof(out->remote_static));
    memcpy(out->remote_epoch, remote_epoch, sizeof(out->remote_epoch));
    fips_aead_key_destroy(send);
    fips_aead_key_destroy(recv);
    return FIPS_NOISE_OK;
}

fips_noise_status fips_noise_session_seal(fips_noise_session *session,
    const uint8_t *ad, size_t ad_len, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t cap, size_t *out_len)
{
    return fips_cipher_state_seal(&session->send_cipher, ad, ad_len, plaintext,
                                  len, out, cap, out_len);
}

fips_noise_status fips_noise_session_check_replay(const fips_noise_session *session, uint64_t counter)
{
    return fips_replay_window_check(&session->replay_window, counter)
        ? FIPS_NOISE_OK : FIPS_NOISE_ReplayDetected;
}

fips_noise_status fips_noise_session_accept_replay(fips_noise_session *session, uint64_t counter)
{
    return fips_replay_window_mark(&session->replay_window, counter)
        ? FIPS_NOISE_OK : FIPS_NOISE_ReplayDetected;
}

fips_noise_status fips_noise_session_open(fips_noise_session *session,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    fips_noise_status rc = fips_noise_session_check_replay(session, counter);
    if (rc != FIPS_NOISE_OK) return rc;
    rc = fips_noise_session_open_unmarked(session, counter, ad, ad_len, ciphertext,
                                          len, out, cap, out_len);
    if (rc != FIPS_NOISE_OK) return rc;
    /* Single-writer contract: no mutation can intervene between check and mark. */
    (void)fips_replay_window_mark(&session->replay_window, counter);
    return FIPS_NOISE_OK;
}

fips_noise_status fips_noise_session_open_unmarked(const fips_noise_session *session,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    return fips_cipher_state_open(&session->recv_cipher, counter, ad, ad_len,
                                  ciphertext, len, out, cap, out_len);
}

void fips_noise_session_reset_replay_window(fips_noise_session *session)
{ fips_replay_window_reset(&session->replay_window); }

uint64_t fips_noise_session_highest_received_counter(const fips_noise_session *session)
{ return fips_replay_window_highest(&session->replay_window); }

uint64_t fips_noise_session_current_send_counter(const fips_noise_session *session)
{ return fips_cipher_state_counter(&session->send_cipher); }

uint64_t fips_noise_session_recv_nonce(const fips_noise_session *session)
{ return fips_cipher_state_counter(&session->recv_cipher); }

fips_noise_status fips_noise_session_take_send_counter(fips_noise_session *session, uint64_t *out)
{ return fips_cipher_state_take_counter(&session->send_cipher, out); }

/* Upstream cipher_clone hands a worker a bare AeadKey: the key and NO counter.
 * A byte copy with the counter pinned at the refused ceiling keeps that shape
 * in C (and tolerates out == src): auto-seal and take_counter on a clone fail
 * with NonceOverflow instead of silently reissuing counters the session already
 * handed out under the same key (nostrc-pwo3h.8 finding N-1). */
static void clone_cipher(const fips_cipher_state *src, fips_cipher_state *out)
{
    memmove(out, src, sizeof(*out));
    out->counter = UINT64_MAX;
}

fips_noise_status fips_noise_session_send_cipher_clone(const fips_noise_session *session,
    fips_cipher_state *out)
{
    if (!session->send_cipher.has_key) return FIPS_NOISE_WrongState;
    clone_cipher(&session->send_cipher, out);
    return FIPS_NOISE_OK;
}

fips_noise_status fips_noise_session_recv_cipher_clone(const fips_noise_session *session,
    fips_cipher_state *out)
{
    if (!session->recv_cipher.has_key) return FIPS_NOISE_WrongState;
    clone_cipher(&session->recv_cipher, out);
    return FIPS_NOISE_OK;
}

fips_noise_status fips_noise_session_take_cleared(fips_noise_session *slot, fips_noise_session *out)
{
    if (slot == out) return FIPS_NOISE_WrongState;
    memcpy(out, slot, sizeof(*out));
    fips_noise_session_clear_slot(slot);
    return FIPS_NOISE_OK;
}

void fips_noise_session_clear_slot(fips_noise_session *slot)
{ sodium_memzero(slot, sizeof(*slot)); }

void fips_noise_session_destroy(fips_noise_session *session)
{ fips_noise_session_clear_slot(session); }
