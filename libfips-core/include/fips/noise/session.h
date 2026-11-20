#ifndef FIPS_NOISE_SESSION_H
#define FIPS_NOISE_SESSION_H

#include <fips/noise/cipher.h>
#include <fips/noise/replay.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No allocation. Fields are read-only outside this module. Destroy every owned
 * copy, including worker clones. No internal locks or threads.
 * THREADING CONTRACT: clones are per-worker independent cipher states; they do
 * not share counters. The session's window is single-writer. The dispatcher
 * serializes send-counter reservation across ALL clones of a key, and workers
 * seal with those explicit counters, NEVER independently auto-sealing from the
 * same snapshot. A decrypt worker authenticates, then the single window owner
 * rechecks and marks only on acceptance. Concurrent lifecycle/mutation or reads
 * racing with mutation require host synchronization. An owned window snapshot
 * may be handed off only by transferring sole replay authority to that worker.
 * Buffer, output and failure conventions are cipher.h/host.h's. */
typedef struct fips_noise_session {
    fips_cipher_state send_cipher, recv_cipher;
    fips_replay_window replay_window;
    fips_handshake_role role;
    uint8_t handshake_hash[32], remote_static[33], remote_epoch[8];
} fips_noise_session;

/* Consumes exactly the directionally split outputs of handshake_finish (no
 * second split or key swap). Copies metadata; wipes send/recv keys on success.
 * Invalid role or send == recv returns WrongState without changing any input
 * or output. All other overlap is forbidden. out must be uninitialized/destroyed.
 * The transcript hash is metadata, NOT an authenticated channel binding. */
FIPS_CORE_API fips_noise_status fips_noise_session_from_handshake(fips_noise_session *out,
    fips_handshake_role role, fips_aead_key *send, fips_aead_key *recv,
    const uint8_t final_h[32], const uint8_t remote_static[33], const uint8_t remote_epoch[8]);
FIPS_CORE_API fips_noise_status fips_noise_session_seal(fips_noise_session *session,
    const uint8_t *ad, size_t ad_len, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t cap, size_t *out_len);
/* Check -> authenticate -> mark. Failure never changes the replay window. */
FIPS_CORE_API fips_noise_status fips_noise_session_open(fips_noise_session *session,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len);
/* Authenticate ONLY: neither checks nor marks replay, even for a seen counter.
 * Rekey callers check first, trial-open, then recheck/mark ONLY on acceptance. */
FIPS_CORE_API fips_noise_status fips_noise_session_open_unmarked(const fips_noise_session *session,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len);
FIPS_CORE_API fips_noise_status fips_noise_session_check_replay(const fips_noise_session *session,
    uint64_t counter);
FIPS_CORE_API fips_noise_status fips_noise_session_accept_replay(fips_noise_session *session,
    uint64_t counter);
FIPS_CORE_API void fips_noise_session_reset_replay_window(fips_noise_session *session);
FIPS_CORE_API uint64_t fips_noise_session_highest_received_counter(const fips_noise_session *session);
FIPS_CORE_API uint64_t fips_noise_session_current_send_counter(const fips_noise_session *session);
/* Explicit-counter opens leave the receive cipher's local nonce unchanged. */
FIPS_CORE_API uint64_t fips_noise_session_recv_nonce(const fips_noise_session *session);
FIPS_CORE_API fips_noise_status fips_noise_session_take_send_counter(fips_noise_session *session,
    uint64_t *out);
/* Copies the initialized key ONLY, as upstream's bare AeadKey hand-out: the
 * clone's local counter is pinned at the refused ceiling, so auto-seal and
 * take_counter on a clone return NonceOverflow and can never reissue a counter
 * this session hands out. Workers seal with explicit dispatcher-reserved
 * counters and open with explicit wire counters. WrongState on a cleared
 * cipher; destination is unchanged on failure. */
FIPS_CORE_API fips_noise_status fips_noise_session_send_cipher_clone(const fips_noise_session *session,
    fips_cipher_state *out);
FIPS_CORE_API fips_noise_status fips_noise_session_recv_cipher_clone(const fips_noise_session *session,
    fips_cipher_state *out);
FIPS_CORE_API fips_noise_status fips_noise_session_take_cleared(fips_noise_session *slot,
    fips_noise_session *out);
FIPS_CORE_API void fips_noise_session_clear_slot(fips_noise_session *slot);
FIPS_CORE_API void fips_noise_session_destroy(fips_noise_session *session);

#ifdef __cplusplus
}
#endif
#endif
