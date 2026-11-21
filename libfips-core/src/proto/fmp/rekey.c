#include <fips/proto/fmp/rekey.h>
#include <fips/identity/key.h>
#include "proto/rekey/noise_profile.h"

static bool initiate(void *ctx, const fips_peer_key *peer, fips_rekey_index ours,
                     fips_rekey_hs *hs, fips_rekey_wire *out) {
    const fips_rekey_node_keys *local = ctx;
    uint8_t remote[33], noise[FIPS_NOISE_HANDSHAKE_MSG1_SIZE];
    size_t len = 0;
    if (!ours.some) return false;
    fips_rekey_pinned(peer, remote);
    bool ok = fips_handshake_new_initiator(&hs->noise, local->secp, local->keypair,
                  remote, local->epoch, local->entropy) == FIPS_NOISE_OK &&
              fips_handshake_write_message_1(&hs->noise, noise, sizeof noise, &len) == FIPS_NOISE_OK;
    fips_writer w;
    fips_writer_init(&w, out->bytes, sizeof out->bytes);
    if (ok) ok = fips_fmp_build_msg1((fips_session_index){ours.value}, noise, len, &w, NULL) == FIPS_OK;
    if (ok) out->len = fips_writer_len(&w);
    sodium_memzero(noise, sizeof noise);
    return ok;
}
bool fips_fmp_rekey_read_msg1(const fips_rekey_node_keys *local, const uint8_t *bytes,
                             size_t len, fips_fmp_ik_msg1 *out) {
    fips_fmp_msg1_header h;
    if (fips_fmp_validate_frame(bytes, len, NULL) != FIPS_OK ||
        !fips_fmp_msg1_header_parse(bytes, len, &h)) return false;
    fips_fmp_ik_msg1 t = {0};
    bool ok = fips_handshake_new_responder(&t.hs.noise, local->secp, local->keypair,
                  local->epoch, local->entropy) == FIPS_NOISE_OK &&
              fips_handshake_read_message_1(&t.hs.noise, bytes + h.noise_msg1_offset,
                                               len - h.noise_msg1_offset) == FIPS_NOISE_OK;
    if (ok && t.hs.noise.has_remote_static && t.hs.noise.has_remote_epoch) {
        t.theirs = h.sender_idx.value;
        memcpy(t.proof.remote.bytes, t.hs.noise.remote_static + 1, 32);
        memcpy(t.proof.epoch, t.hs.noise.remote_epoch, 8);
        crypto_hash_sha256(t.proof.digest, bytes, len);
        crypto_hash_sha256(t.proof.ephemeral, bytes + h.noise_msg1_offset, FIPS_NOISE_PUBKEY_SIZE);
        *out = t;
        out->proof.token = out;
    } else ok = false;
    fips_fmp_rekey_msg1_clear(&t);
    return ok;
}
bool fips_fmp_rekey_setup_record(const uint8_t *msg1, size_t len,
                                const fips_rekey_wire *msg2, fips_rekey_record *out) {
    fips_fmp_msg1_header h;
    if (fips_fmp_validate_frame(msg1, len, NULL) != FIPS_OK ||
        !fips_fmp_msg1_header_parse(msg1, len, &h) ||
        len - h.noise_msg1_offset < FIPS_NOISE_PUBKEY_SIZE ||
        (msg2 && msg2->len > FIPS_REKEY_WIRE_MAX)) return false;
    fips_rekey_digest whole, ephemeral;
    crypto_hash_sha256(whole.bytes, msg1, len);
    crypto_hash_sha256(ephemeral.bytes, msg1 + h.noise_msg1_offset, FIPS_NOISE_PUBKEY_SIZE);
    fips_rekey_record_setup(out, whole, ephemeral, msg2);
    return true;
}
void fips_fmp_rekey_msg1_clear(fips_fmp_ik_msg1 *token) {
    fips_rekey_hs_clear(&token->hs);
    sodium_memzero(token, sizeof *token);
}
static bool msg1_answer(const void *token, void *ctx, fips_rekey_index ours,
                        fips_rekey_completed *out, fips_rekey_wire *msg) {
    const fips_fmp_ik_msg1 *t = token;
    (void)ctx;
    if (!ours.some || !t || t->proof.token != t) return false;
    fips_handshake_state fork = {0};
    fips_handshake_fork(&t->hs.noise, &fork);
    uint8_t noise[FIPS_NOISE_HANDSHAKE_MSG2_SIZE];
    size_t len = 0;
    bool ok = fips_handshake_write_message_2(&fork, noise, sizeof noise, &len) == FIPS_NOISE_OK &&
              noise_completed(&fork, out);
    fips_writer w;
    fips_writer_init(&w, msg->bytes, sizeof msg->bytes);
    if (ok) ok = fips_fmp_build_msg2((fips_session_index){ours.value},
        (fips_session_index){t->theirs}, noise, len, &w, NULL) == FIPS_OK;
    if (ok) {
        out->theirs = (fips_rekey_index){t->theirs, true};
        msg->len = fips_writer_len(&w);
    }
    fips_handshake_destroy(&fork);
    sodium_memzero(noise, sizeof noise);
    return ok;
}
static fips_rekey_index index_reply(const uint8_t *bytes, size_t len) {
    fips_fmp_msg2_header h;
    return fips_fmp_msg2_header_parse(bytes, len, &h)
        ? (fips_rekey_index){h.receiver_idx.value, true} : (fips_rekey_index){0};
}
static bool reply(const fips_rekey_hs *hs, void *ctx, const uint8_t *bytes, size_t len,
                  fips_rekey_read *out) {
    (void)ctx;
    fips_fmp_msg2_header h;
    if (fips_fmp_validate_frame(bytes, len, NULL) != FIPS_OK ||
        !fips_fmp_msg2_header_parse(bytes, len, &h)) return false;
    fips_handshake_state fork = {0};
    bool ok = fips_handshake_fork_msg2(&hs->noise, bytes + h.noise_msg2_offset,
                  len - h.noise_msg2_offset, &fork) == FIPS_NOISE_OK &&
              noise_completed(&fork, &out->done);
    if (ok) out->done.theirs = (fips_rekey_index){h.sender_idx.value, true};
    fips_handshake_destroy(&fork);
    return ok;
}
static bool valid(const uint8_t *p, size_t len) { (void)p; (void)len; return false; }
static bool answer(void *ctx, const uint8_t *p, size_t len, fips_rekey_hs *hs, fips_rekey_wire *out) {
    (void)ctx; (void)p; (void)len; (void)hs; (void)out; return false;
}
static bool complete(const fips_rekey_hs *hs, const uint8_t *p, size_t len, fips_rekey_completed *out) {
    (void)hs; (void)p; (void)len; (void)out; return false;
}
static bool prompt(fips_duration elapsed, fips_rekey_wire *out) {
    uint8_t type = FIPS_LINK_Heartbeat;
    fips_writer w;
    fips_writer_init(&w, out->bytes, sizeof out->bytes);
    if (fips_fmp_prepend_inner_header(elapsed_ms(elapsed), &type, 1, &w, NULL) != FIPS_OK) return false;
    out->len = fips_writer_len(&w);
    return true;
}
static bool is_probe(const uint8_t *p, size_t len) { (void)p; (void)len; return false; }
const fips_rekey_profile fips_fmp_rekey_profile = {
    .layer = FIPS_REKEY_LAYER_Fmp, .messages = 2, .commit = FIPS_REKEY_COMMIT_AtAnswer,
    .initiator_cutover = FIPS_REKEY_OnProof, .responder_cutover = FIPS_REKEY_OnFrame,
    .dampen_at = FIPS_REKEY_DAMPEN_Answer, .select = FIPS_REKEY_ByIndex, .indexed = true, .cap_replies = true,
    .floor = FIPS_FRAMING_FMP_INNER_HEADER_SIZE, .aad = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE,
    .keys = &fips_rekey_noise_keys, .initiate = initiate, .valid = valid, .answer = answer,
    .index = index_reply, .reply = reply, .complete = complete, .prompt = prompt,
    .is_probe = is_probe, .msg1_answer = msg1_answer
};
fips_status fips_fmp_rekey_inbound(fips_rekey *m, fips_instant now,
    const fips_fmp_inbound_decision *decision, const fips_fmp_ik_msg1 *token,
    bool on_link, bool reachable, fips_rekey_env *env, fips_rekey_outputs *out) {
    fips_node_addr peer = fips_rekey_bond_addr(&m->bond);
    fips_node_addr remote = fips_peer_key_addr(&token->proof.remote);
    if (m->profile != &fips_fmp_rekey_profile || decision->kind != FIPS_FMP_IN_Rekey ||
        token->proof.token != token || memcmp(peer.bytes, decision->peer.bytes, 16) ||
        memcmp(peer.bytes, remote.bytes, 16)) return FIPS_ERR_RANGE;
    return fips_rekey_step(m, now, (fips_rekey_input){.kind = FIPS_REKEY_IN_PeerMsg1,
        .tok = &token->proof, .on_link = on_link, .reachable = reachable}, env, out);
}
fips_status fips_fmp_rekey_receive(fips_rekey *m, fips_instant now, uint8_t *frame,
    size_t len, fips_rekey_env *env, fips_rekey_outputs *out) {
    if (m->profile != &fips_fmp_rekey_profile ||
        fips_fmp_validate_frame(frame, len, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_fmp_msg2_header reply_header;
    fips_rekey_input in = {.bytes = frame, .buf = frame, .len = len};
    if (fips_fmp_msg2_header_parse(frame, len, &reply_header)) in.kind = FIPS_REKEY_IN_Reply;
    else {
        fips_fmp_encrypted_header h;
        if (!fips_fmp_encrypted_header_parse(frame, len, &h)) return FIPS_ERR_RANGE;
        in.kind = FIPS_REKEY_IN_Frame;
        in.hint = (fips_rekey_frame_hint){h.counter, (h.flags & FIPS_FMP_FLAG_KEY_EPOCH) != 0,
                                         h.receiver_idx.value, FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE};
    }
    return fips_rekey_step(m, now, in, env, out);
}
