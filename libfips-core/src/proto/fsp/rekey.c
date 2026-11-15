#include <fips/proto/fsp/rekey.h>
#include "../rekey/noise_profile.h"
#include <secp256k1.h>

static bool initiate(void *ctx, const fips_peer_key *peer, fips_rekey_index ours,
                     fips_rekey_hs *hs, fips_rekey_wire *out) {
    const fips_rekey_node_keys *local = ctx;
    uint8_t remote[33];
    (void)ours;
    fips_rekey_pinned(peer, remote);
    return fips_handshake_new_xk_initiator(&hs->noise, local->secp, local->keypair,
               remote, local->epoch, local->entropy) == FIPS_NOISE_OK &&
           fips_handshake_write_xk_message_1(&hs->noise, out->bytes, sizeof out->bytes,
                                              &out->len) == FIPS_NOISE_OK;
}
static bool valid(const uint8_t *bytes, size_t len) {
    /* Use the same point parser as the handshake, without drawing entropy. */
    secp256k1_pubkey point;
    return len == FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE &&
           secp256k1_ec_pubkey_parse(secp256k1_context_static, &point, bytes, len) != 0;
}
static bool answer(void *ctx, const uint8_t *bytes, size_t len,
                   fips_rekey_hs *hs, fips_rekey_wire *out) {
    const fips_rekey_node_keys *local = ctx;
    return fips_handshake_new_xk_responder(&hs->noise, local->secp, local->keypair,
               local->epoch, local->entropy) == FIPS_NOISE_OK &&
           fips_handshake_read_xk_message_1(&hs->noise, bytes, len) == FIPS_NOISE_OK &&
           fips_handshake_write_xk_message_2(&hs->noise, out->bytes, sizeof out->bytes,
                                              &out->len) == FIPS_NOISE_OK;
}
static fips_rekey_index index_reply(const uint8_t *bytes, size_t len) {
    (void)bytes; (void)len;
    return (fips_rekey_index){0};
}
static bool reply(const fips_rekey_hs *hs, void *ctx, const uint8_t *bytes, size_t len,
                  fips_rekey_read *out) {
    (void)ctx;
    fips_handshake_state fork = {0};
    bool ok = fips_handshake_fork_xk2(&hs->noise, bytes, len, &fork) == FIPS_NOISE_OK &&
              fips_handshake_write_xk_message_3(&fork, out->fin.bytes, sizeof out->fin.bytes,
                                                  &out->fin.len) == FIPS_NOISE_OK &&
              noise_completed(&fork, &out->done);
    out->has_fin = ok;
    fips_handshake_destroy(&fork);
    return ok;
}
static bool complete(const fips_rekey_hs *hs, const uint8_t *bytes, size_t len,
                     fips_rekey_completed *out) {
    fips_handshake_state fork = {0};
    bool ok = fips_handshake_fork_xk3(&hs->noise, bytes, len, &fork) == FIPS_NOISE_OK &&
              noise_completed(&fork, out);
    fips_handshake_destroy(&fork);
    return ok;
}
static bool prompt(fips_duration elapsed, fips_rekey_wire *out) {
    fips_writer w;
    fips_writer_init(&w, out->bytes, sizeof out->bytes);
    if (fips_fsp_prepend_inner_header(elapsed_ms(elapsed), FIPS_SESSION_CoordsWarmup, 0,
                                     NULL, 0, &w, NULL) != FIPS_OK) return false;
    out->len = fips_writer_len(&w);
    return true;
}
static bool is_probe(const uint8_t *p, size_t len) {
    fips_fsp_inner_header h;
    fips_fsp_bytes payload;
    return fips_fsp_strip_inner_header(p, len, &h, &payload) &&
           h.msg_type == FIPS_SESSION_CoordsWarmup;
}
const fips_rekey_profile fips_fsp_rekey_profile = {
    .layer = FIPS_REKEY_LAYER_Fsp, .messages = 3, .commit = FIPS_REKEY_COMMIT_AtFinal,
    .initiator_cutover = FIPS_REKEY_OnEvidence, .responder_cutover = FIPS_REKEY_AtProof,
    .candidates = 4, .per_carrier = 2, .dampen_at = FIPS_REKEY_DAMPEN_Completion,
    .select = FIPS_REKEY_Trial, .silence = true, .setup_rtt = true, .indexed = false,
    .floor = FIPS_FRAMING_FSP_INNER_HEADER_SIZE, .aad = FIPS_FRAMING_FSP_HEADER_SIZE,
    .keys = &fips_rekey_noise_keys, .initiate = initiate, .valid = valid, .answer = answer,
    .index = index_reply, .reply = reply, .complete = complete, .prompt = prompt,
    .is_probe = is_probe
};

fips_status fips_fsp_rekey_receive(fips_rekey *m, fips_instant now,
    fips_node_addr carrier, fips_rekey_route_handle route, uint8_t *frame, size_t len,
    fips_rekey_env *env, fips_rekey_outputs *out) {
    if (m->profile != &fips_fsp_rekey_profile) return FIPS_ERR_RANGE;
    fips_fsp_action action;
    fips_fsp_action_list list = {&action, 0, 1};
    fips_status status = fips_fsp_receive(fips_rekey_bond_addr(&m->bond), frame, len, &list);
    if (status != FIPS_OK) return status;
    if (action.kind == FIPS_FSP_DropFrame || action.kind == FIPS_FSP_ReceiveRoutingSignal)
        return FIPS_ERR_RANGE;
    fips_rekey_input in = {.bytes = frame, .buf = frame, .len = len,
                           .carrier = carrier, .route = route};
    fips_reader r;
    fips_reader_init(&r, frame + FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE,
                         len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE);
    if (action.kind == FIPS_FSP_ReceiveSetup) {
        fips_fsp_session_setup msg;
        if (fips_fsp_session_setup_decode_strict(&r, &msg, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
        in.kind = FIPS_REKEY_IN_Setup; in.bytes = msg.handshake_payload.data; in.len = msg.handshake_payload.len;
    } else if (action.kind == FIPS_FSP_ReceiveAck) {
        fips_fsp_session_ack msg;
        if (fips_fsp_session_ack_decode_strict(&r, &msg, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
        in.kind = FIPS_REKEY_IN_Reply; in.bytes = msg.handshake_payload.data; in.len = msg.handshake_payload.len;
    } else if (action.kind == FIPS_FSP_ReceiveMsg3) {
        fips_fsp_session_msg3 msg;
        if (fips_fsp_session_msg3_decode_strict(&r, &msg, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
        in.kind = FIPS_REKEY_IN_Final; in.bytes = msg.handshake_payload.data; in.len = msg.handshake_payload.len;
    } else {
        fips_fsp_encrypted_header h;
        if (!fips_fsp_encrypted_header_parse(frame, len, &h)) return FIPS_ERR_RANGE;
        size_t body = FIPS_FRAMING_FSP_HEADER_SIZE;
        if (fips_fsp_encrypted_header_has_coords(&h)) {
            fips_fsp_coords src, dest;
            fips_reader_init(&r, frame + body, len - body);
            if (fips_fsp_parse_encrypted_coords(&r, &src, &dest, NULL) != FIPS_OK) return FIPS_ERR_RANGE;
            body += fips_reader_position(&r);
        }
        in.kind = FIPS_REKEY_IN_Frame;
        in.hint = (fips_rekey_frame_hint){h.counter, (h.flags & FIPS_FSP_FLAG_K) != 0, 0, body};
    }
    return fips_rekey_step(m, now, in, env, out);
}
