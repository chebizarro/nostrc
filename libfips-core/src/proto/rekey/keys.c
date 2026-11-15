#include <fips/proto/rekey/keys.h>
#include <string.h>
static bool open_frame(const fips_noise_session *s, size_t aad, fips_rekey_frame_hint h,
                       const uint8_t *frame, size_t len, uint8_t *out, size_t cap, size_t *n) {
    if (aad > len || h.body > len)
        return false;
    return fips_noise_session_open_unmarked(s, h.counter, frame, aad, frame + h.body, len - h.body,
                                            out, cap, n) == FIPS_NOISE_OK;
}
static bool fresh(const fips_noise_session *s, uint64_t c) {
    return fips_noise_session_check_replay(s, c) == FIPS_NOISE_OK;
}
static void mark(fips_noise_session *s, uint64_t c) {
    (void)fips_noise_session_accept_replay(s, c);
}
static bool reserve(fips_noise_session *s, uint64_t *c, fips_cipher_state *key) {
    fips_cipher_state k = {0};
    if (fips_noise_session_send_cipher_clone(s, &k) != FIPS_NOISE_OK)
        return false;
    if (fips_noise_session_take_send_counter(s, c) != FIPS_NOISE_OK) {
        fips_cipher_state_destroy(&k);
        return false;
    }
    *key = k;
    fips_cipher_state_destroy(&k);
    return true;
}
static void copy(const fips_noise_session *s, bool has_floor, uint64_t floor,
                 fips_rekey_recv_copy *out) {
    *out = (fips_rekey_recv_copy){0};
    (void)fips_noise_session_recv_cipher_clone(s, &out->key);
    out->window = s->replay_window;
    out->has_floor = has_floor;
    out->floor = floor;
}
const fips_rekey_session_keys fips_rekey_noise_keys = {
    open_frame, fresh, mark, fips_noise_session_current_send_counter, reserve, copy};
void fips_rekey_pinned(const fips_peer_key *p, uint8_t out[33]) {
    out[0] = 2;
    memcpy(out + 1, p->bytes, 32);
}
