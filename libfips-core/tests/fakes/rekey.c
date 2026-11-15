#include "rekey.h"
#include <sodium.h>
#include <string.h>
/* Stub transcript, not an IK/XK implementation. Public test identities and a
 * transcript hash let tests independently corrupt the authenticated static,
 * startup epoch and attempt id. Real Noise transport keys/replay test the slot
 * lifecycle; production handshake profiles are deliberately not linked. */
#define WIRE_LEN 121u
static _Thread_local uint64_t completions;
uint64_t rk_test_completions(void) { return completions; }
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void mac(uint8_t *wire) {
    uint8_t body[81];
    body[0] = wire[0];
    memcpy(body + 1, wire + 9, 80);
    crypto_hash_sha256(wire + 89, body, sizeof body);
}
static bool valid_auth(const uint8_t *wire, size_t len, uint8_t type) {
    if (len != WIRE_LEN || wire[0] != type)
        return false;
    uint8_t copy[WIRE_LEN];
    memcpy(copy, wire, WIRE_LEN);
    mac(copy);
    return sodium_memcmp(copy + 89, wire + 89, 32) == 0;
}
void rk_test_auth(rk_test_packet *p) {
    assert(p->len == WIRE_LEN);
    mac(p->bytes);
}
static void make_wire(fips_rekey_wire *out, uint8_t type, uint32_t ours, uint32_t theirs,
                      uint64_t id, const fips_peer_key *local, const fips_peer_key *peer,
                      const uint8_t epoch[8]) {
    memset(out, 0, sizeof *out);
    out->len = WIRE_LEN;
    out->bytes[0] = type;
    put32(out->bytes + 1, ours);
    put32(out->bytes + 5, theirs);
    put64(out->bytes + 9, id);
    memcpy(out->bytes + 17, local->bytes, 32);
    memcpy(out->bytes + 49, peer->bytes, 32);
    memcpy(out->bytes + 81, epoch, 8);
    mac(out->bytes);
}
static void save_hs(fips_rekey_hs *hs, const fips_rekey_wire *msg) {
    memcpy(hs->extra, msg->bytes, msg->len);
}
static void completed(fips_rekey_completed *d, uint64_t id, fips_handshake_role role,
                      fips_peer_key remote, const uint8_t epoch[8], fips_rekey_index theirs) {
    if (completions < UINT64_MAX)
        ++completions;
    uint8_t seed[9], h[32], keybytes[32], pub[33] = {2};
    put64(seed, id);
    fips_aead_key send, recv;
    seed[8] = role == FIPS_HANDSHAKE_INITIATOR ? 1 : 2;
    crypto_hash_sha256(keybytes, seed, sizeof seed);
    fips_aead_key_init(&send, keybytes);
    seed[8] = role == FIPS_HANDSHAKE_INITIATOR ? 2 : 1;
    crypto_hash_sha256(keybytes, seed, sizeof seed);
    fips_aead_key_init(&recv, keybytes);
    crypto_hash_sha256(h, seed, 8);
    memcpy(pub + 1, remote.bytes, 32);
    assert(fips_noise_session_from_handshake(&d->session, role, &send, &recv, h, pub, epoch) ==
           FIPS_NOISE_OK);
    d->remote = remote;
    memcpy(d->epoch, epoch, 8);
    d->theirs = theirs;
    sodium_memzero(keybytes, sizeof keybytes);
}
static bool initiate(void *ctx, const fips_peer_key *peer, fips_rekey_index ours, fips_rekey_hs *hs,
                     fips_rekey_wire *out) {
    rk_test_local *l = ctx;
    if (l->fail)
        return false;
    make_wire(out, 1, ours.value, 0, l->next++, &l->key, peer, l->epoch);
    save_hs(hs, out);
    return true;
}
static bool valid(const uint8_t *p, size_t len) {
    return len == WIRE_LEN && p[0] == 1 && get64(p + 9) != 0;
}
static bool answer(void *ctx, const uint8_t *p, size_t len, fips_rekey_hs *hs,
                   fips_rekey_wire *out) {
    rk_test_local *l = ctx;
    if (l->fail || !valid(p, len))
        return false;
    fips_peer_key peer;
    memcpy(peer.bytes, p + 17, 32);
    make_wire(out, 2, 0, get32(p + 1), get64(p + 9), &l->key, &peer, l->epoch);
    save_hs(hs, out);
    return true;
}
static fips_rekey_index index_reply(const uint8_t *p, size_t len) {
    return len >= 9 ? (fips_rekey_index){get32(p + 5), true} : (fips_rekey_index){0};
}
static bool reply(const fips_rekey_hs *hs, void *ctx, const uint8_t *p, size_t len,
                  fips_rekey_read *r) {
    rk_test_local *l = ctx;
    if (l->fail || !valid_auth(p, len, 2) || get64(p + 9) != get64(hs->extra + 9))
        return false;
    fips_peer_key remote;
    memcpy(remote.bytes, p + 17, 32);
    completed(&r->done, get64(p + 9), FIPS_HANDSHAKE_INITIATOR, remote, p + 81,
              (fips_rekey_index){get32(p + 1), l->indexed});
    r->has_fin = l->messages == 3;
    if (r->has_fin)
        make_wire(&r->fin, 3, get32(hs->extra + 1), get32(p + 1), get64(p + 9), &l->key, &remote,
                  l->epoch);
    return true;
}
static bool complete(const fips_rekey_hs *hs, const uint8_t *p, size_t len,
                     fips_rekey_completed *out) {
    if (!valid_auth(p, len, 3) || get64(p + 9) != get64(hs->extra + 9))
        return false;
    fips_peer_key remote;
    memcpy(remote.bytes, p + 17, 32);
    completed(out, get64(p + 9), FIPS_HANDSHAKE_RESPONDER, remote, p + 81, (fips_rekey_index){0});
    return true;
}
static bool msg1_answer(const void *token, void *ctx, fips_rekey_index ours,
                        fips_rekey_completed *out, fips_rekey_wire *msg) {
    const rk_test_packet *packet = token;
    rk_test_local *l = ctx;
    if (l->fail || !valid_auth(packet->bytes, packet->len, 1))
        return false;
    fips_peer_key remote;
    memcpy(remote.bytes, packet->bytes + 17, 32);
    uint64_t id = get64(packet->bytes + 9);
    completed(out, id, FIPS_HANDSHAKE_RESPONDER, remote, packet->bytes + 81,
              (fips_rekey_index){get32(packet->bytes + 1), true});
    make_wire(msg, 2, ours.value, get32(packet->bytes + 1), id, &l->key, &remote, l->epoch);
    return true;
}
static bool prompt(fips_duration elapsed, fips_rekey_wire *out) {
    out->len = 5;
    out->bytes[0] = 0x51;
    put32(out->bytes + 1, (uint32_t)(elapsed.secs * 1000 + elapsed.nanos / 1000000));
    return true;
}
static bool is_probe(const uint8_t *p, size_t len) { return len >= 5 && p[0] == 0x51; }
#define COMMON                                                                                     \
    .floor = 5, .aad = 13, .keys = &fips_rekey_noise_keys, .initiate = initiate, .valid = valid,   \
    .answer = answer, .index = index_reply, .reply = reply, .complete = complete,                  \
    .prompt = prompt, .is_probe = is_probe, .msg1_answer = msg1_answer
const fips_rekey_profile rk_test_ik = {.layer = FIPS_REKEY_LAYER_Fmp,
                                       .messages = 2,
                                       .commit = FIPS_REKEY_COMMIT_AtAnswer,
                                       .initiator_cutover = FIPS_REKEY_OnProof,
                                       .responder_cutover = FIPS_REKEY_OnFrame,
                                       .dampen_at = FIPS_REKEY_DAMPEN_Answer,
                                       .select = FIPS_REKEY_ByIndex,
                                       .indexed = true,
                                       COMMON};
const fips_rekey_profile rk_test_xk = {.layer = FIPS_REKEY_LAYER_Fsp,
                                       .messages = 3,
                                       .commit = FIPS_REKEY_COMMIT_AtFinal,
                                       .initiator_cutover = FIPS_REKEY_OnEvidence,
                                       .responder_cutover = FIPS_REKEY_AtProof,
                                       .candidates = 4,
                                       .per_carrier = 2,
                                       .dampen_at = FIPS_REKEY_DAMPEN_Completion,
                                       .select = FIPS_REKEY_Trial,
                                       .silence = true,
                                       .setup_rtt = true,
                                       COMMON};
const fips_rekey_profile rk_test_xk_frame = {.layer = FIPS_REKEY_LAYER_Fsp,
                                             .messages = 3,
                                             .commit = FIPS_REKEY_COMMIT_AtFinal,
                                             .initiator_cutover = FIPS_REKEY_OnEvidence,
                                             .responder_cutover = FIPS_REKEY_OnFrame,
                                             .candidates = 4,
                                             .per_carrier = 2,
                                             .dampen_at = FIPS_REKEY_DAMPEN_Completion,
                                             .select = FIPS_REKEY_Trial,
                                             .silence = true,
                                             .setup_rtt = true,
                                             COMMON};
const fips_rekey_profile rk_test_xx = {.layer = FIPS_REKEY_LAYER_Fmp,
                                       .messages = 3,
                                       .commit = FIPS_REKEY_COMMIT_AtFinal,
                                       .initiator_cutover = FIPS_REKEY_AfterDelay,
                                       .responder_cutover = FIPS_REKEY_OnFrame,
                                       .dampen_at = FIPS_REKEY_DAMPEN_Completion,
                                       .select = FIPS_REKEY_ByIndex,
                                       .indexed = true,
                                       COMMON};
#undef COMMON
void rk_test_local_init(rk_test_local *l, uint8_t seed, unsigned messages) {
    memset(l, 0, sizeof *l);
    memset(l->key.bytes, seed, 32);
    memset(l->epoch, 1, 8);
    l->next = (uint64_t)seed * 100000 + 1;
    l->messages = messages;
    l->indexed = messages == 2;
}
fips_node_addr rk_test_addr(fips_peer_key key) {
    fips_xonly_pubkey p;
    memcpy(p.bytes, key.bytes, 32);
    return fips_node_addr_from_pubkey(&p);
}
static bool draw(void *ctx, uint32_t *out) {
    rk_test_node *n = ctx;
    if (n->next_index >= 1024)
        return false;
    *out = n->next_index++;
    assert(!n->indices[*out]);
    n->indices[*out] = true;
    ++n->allocated;
    return true;
}
static uint16_t jitter(void *ctx, uint16_t bound) {
    rk_test_node *n = ctx;
    return n->custom_jitter ? (n->jitter <= bound ? n->jitter : bound) : (uint16_t)(bound / 2);
}
void rk_test_node_init(rk_test_node *n, uint8_t seed, const fips_rekey_profile *p) {
    memset(n, 0, sizeof *n);
    n->profile = *p;
    rk_test_local_init(&n->local, seed, p->messages);
    n->local.indexed = p->indexed;
    n->next_index = 100;
    n->env = (fips_rekey_env){.indices = {draw, n},
                              .transport = 1,
                              .tags = &n->tags,
                              .jitter_draw = jitter,
                              .jitter_ctx = n,
                              .local = &n->local,
                              .scratch = n->scratch,
                              .scratch_capacity = sizeof n->scratch};
}
void rk_test_establish(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                       fips_rekey_setup_facts *facts) {
    fips_node_addr mine = rk_test_addr(n->local.key), other = rk_test_addr(peer->key);
    fips_rekey_completed first = {0};
    completed(&first, 42,
              memcmp(&mine, &other, sizeof mine) < 0 ? FIPS_HANDSHAKE_INITIATOR
                                                     : FIPS_HANDSHAKE_RESPONDER,
              peer->key, peer->epoch, (fips_rekey_index){100, n->profile.indexed});
    fips_rekey_ids ids = {0};
    ids.transport = n->env.transport;
    if (n->profile.indexed) {
        assert(fips_rekey_allocate(n->env.indices, &ids.lease));
        ids.theirs = (fips_rekey_index){100, true};
    }
    fips_rekey_outputs_clear(&n->out);
    assert(fips_rekey_established(&n->machine, &n->profile, params,
                                  (fips_rekey_bond){.kind = FIPS_REKEY_BOND_Key, .key = peer->key},
                                  mine, &first, &ids, facts, fips_test_instant(0), &n->env,
                                  &n->out) == FIPS_OK);
    rk_test_execute(n, 0);
}
void rk_test_pair(rk_test_node *a, rk_test_node *b, const fips_rekey_profile *p,
                  fips_rekey_config config) {
    fips_peer_key key1, key2;
    memset(key1.bytes, 1, 32);
    memset(key2.bytes, 2, 32);
    fips_node_addr addr1 = rk_test_addr(key1), addr2 = rk_test_addr(key2);
    bool ordered = memcmp(&addr1, &addr2, sizeof addr1) < 0;
    rk_test_node_init(a, ordered ? 1 : 2, p);
    rk_test_node_init(b, ordered ? 2 : 1, p);
    fips_rekey_params params;
    assert(fips_rekey_params_new(config, &params) == FIPS_REKEY_PARAMS_OK);
    fips_rekey_setup_facts facts = {.has_peer_epoch = true,
                                    .has_peer_k = false,
                                    .link_since = fips_test_instant(0),
                                    .started = fips_test_instant(0)};
    memset(facts.peer_epoch, 1, 8);
    rk_test_establish(a, &b->local, params, &facts);
    facts = (fips_rekey_setup_facts){.has_peer_epoch = true,
                                     .link_since = fips_test_instant(0),
                                     .started = fips_test_instant(0)};
    memset(facts.peer_epoch, 1, 8);
    rk_test_establish(b, &a->local, params, &facts);
}
void rk_test_open(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                  uint64_t now) {
    fips_rekey_outputs_clear(&n->out);
    assert(fips_rekey_opening_outbound(&n->machine, &n->profile, params, peer->key,
                                       rk_test_addr(n->local.key), fips_test_instant(now), &n->env,
                                       &n->out) == FIPS_OK);
    rk_test_execute(n, now);
}
void rk_test_inbound(rk_test_node *n, const rk_test_local *peer, fips_rekey_params params,
                     uint64_t now, const rk_test_packet *setup) {
    fips_rekey_outputs_clear(&n->out);
    fips_rekey_input in = {.kind = FIPS_REKEY_IN_Setup,
                           .bytes = setup->bytes,
                           .len = setup->len,
                           .carrier = rk_test_addr(peer->key),
                           .route = 7};
    assert(fips_rekey_opening_inbound(&n->machine, &n->profile, params, rk_test_addr(peer->key),
                                      rk_test_addr(n->local.key), fips_test_instant(now), in,
                                      &n->env, &n->out) == FIPS_OK);
    rk_test_execute(n, now);
}
static rk_test_packet seal_packet(fips_rekey_sealed *s, const uint8_t *p, size_t len) {
    rk_test_packet packet = {0};
    packet.bytes[0] = s->k;
    put32(packet.bytes + 1, s->theirs.value);
    put64(packet.bytes + 5, s->counter);
    size_t n = 0;
    assert(fips_cipher_state_seal_with_counter(&s->key, s->counter, packet.bytes, 13, p, len,
                                               packet.bytes + 13, sizeof packet.bytes - 13,
                                               &n) == FIPS_NOISE_OK);
    packet.len = n + 13;
    fips_rekey_sealed_clear(s);
    return packet;
}
void rk_test_execute(rk_test_node *n, uint64_t now) {
    for (size_t i = 0; i < n->out.count; ++i) {
        fips_rekey_output *o = &n->out.items[i];
        if (o->kind == FIPS_REKEY_OUT_Release) {
            uint32_t index;
            assert(fips_rekey_index_lease_release(&o->lease, &index));
            assert(index < 1024 && n->indices[index]);
            n->indices[index] = false;
            --n->allocated;
        } else if (o->kind == FIPS_REKEY_OUT_Send) {
            assert(n->wires < RK_TEST_WIRE);
            rk_test_packet *p = &n->wire[n->wires++];
            memset(p, 0, sizeof *p);
            p->handshake = true;
            p->kind = o->message_kind;
            p->len = o->data.msg.len;
            memcpy(p->bytes, o->data.msg.bytes, p->len);
        } else if (o->kind == FIPS_REKEY_OUT_Prompt) {
            fips_rekey_sealed_prompt prompt = {0};
            fips_rekey_seal_refusal refusal =
                fips_rekey_seal_prompt(&n->machine, fips_test_instant(now), o->tag, &prompt);
            if (refusal == FIPS_REKEY_SEAL_Exhausted) {
                /* A host may drop an exhausted prompt. The low-limit fuzzer
                 * reaches this upstream-defined refusal on a pending epoch. */
                const fips_noise_session *session =
                    fips_rekey_keys(&n->machine, o->tag == n->machine.cur.tag ? FIPS_REKEY_Current
                                                                              : FIPS_REKEY_Pending);
                assert(session && fips_noise_session_current_send_counter(session) >=
                                      n->machine.params.cfg.hard);
                assert(sodium_is_zero((const uint8_t *)&prompt, sizeof prompt));
                continue;
            }
            assert(refusal == FIPS_REKEY_SEAL_OK);
            assert(n->wires < RK_TEST_WIRE);
            n->wire[n->wires++] =
                seal_packet(&prompt.sealed, prompt.plaintext.bytes, prompt.plaintext.len);
            fips_rekey_sealed_prompt_clear(&prompt);
        }
    }
    rk_test_conserved(n);
}
void rk_test_step(rk_test_node *n, uint64_t now, fips_rekey_input in) {
    fips_rekey_outputs_clear(&n->out);
    assert(fips_rekey_step(&n->machine, fips_test_instant(now), in, &n->env, &n->out) == FIPS_OK);
    rk_test_execute(n, now);
}
void rk_test_poll(rk_test_node *n, uint64_t now) {
    rk_test_step(n, now, (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll});
}
void rk_test_start(rk_test_node *n, uint64_t now) {
    fips_rekey_outputs_clear(&n->out);
#ifdef FIPS_TEST_HOOKS
    fips_rekey_start_now(&n->machine, fips_test_instant(now), &n->env, &n->out);
#else
    /* Fuzz builds exercise the production trigger, not a private transition. */
    assert(fips_rekey_step(&n->machine, fips_test_instant(now),
                           (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &n->env,
                           &n->out) == FIPS_OK);
#endif
    rk_test_execute(n, now);
}
bool rk_test_token(const rk_test_packet *p, fips_rekey_msg1_read *tok) {
    if (!valid_auth(p->bytes, p->len, 1))
        return false;
    memset(tok, 0, sizeof *tok);
    memcpy(tok->remote.bytes, p->bytes + 17, 32);
    memcpy(tok->epoch, p->bytes + 81, 8);
    crypto_hash_sha256(tok->digest, p->bytes, p->len);
    crypto_hash_sha256(tok->ephemeral, p->bytes + 9, 8);
    tok->token = p;
    return true;
}
void rk_test_recv_on(rk_test_node *n, uint64_t now, const rk_test_packet *packet, bool on_link,
                     bool reachable, uint8_t carrier) {
    rk_test_packet p = *packet;
    fips_rekey_msg1_read token = {0};
    fips_rekey_input in = {.bytes = p.bytes,
                           .buf = p.bytes,
                           .len = p.len,
                           .route = 7,
                           .on_link = on_link,
                           .reachable = reachable};
    memset(in.carrier.bytes, carrier, sizeof in.carrier.bytes);
    if (!p.handshake) {
        in.kind = FIPS_REKEY_IN_Frame;
        if (p.len >= 13)
            in.hint = (fips_rekey_frame_hint){get64(p.bytes + 5), p.bytes[0] != 0,
                                              get32(p.bytes + 1), 13};
        else
            in.hint.body = 13;
    } else
        switch (p.kind) {
        case FIPS_REKEY_MSG_Msg1:
            if (!rk_test_token(&p, &token)) {
                fips_rekey_outputs_clear(&n->out);
                return;
            }
            in.kind = FIPS_REKEY_IN_PeerMsg1;
            in.tok = &token;
            break;
        case FIPS_REKEY_MSG_Msg2:
        case FIPS_REKEY_MSG_Ack:
            in.kind = FIPS_REKEY_IN_Reply;
            break;
        case FIPS_REKEY_MSG_Setup:
            in.kind = FIPS_REKEY_IN_Setup;
            break;
        case FIPS_REKEY_MSG_Msg3:
            in.kind = FIPS_REKEY_IN_Final;
            break;
        }
    rk_test_step(n, now, in);
}
void rk_test_recv(rk_test_node *n, uint64_t now, const rk_test_packet *p) {
    rk_test_recv_on(n, now, p, true, true, 1);
}
bool rk_test_has_packet(const rk_test_node *n, bool hs, fips_rekey_msg_kind kind) {
    for (size_t i = 0; i < n->wires; ++i)
        if (n->wire[i].handshake == hs && (!hs || n->wire[i].kind == kind))
            return true;
    return false;
}
rk_test_packet rk_test_pop(rk_test_node *n, bool hs, fips_rekey_msg_kind kind) {
    for (size_t i = 0; i < n->wires; ++i)
        if (n->wire[i].handshake == hs && (!hs || n->wire[i].kind == kind)) {
            rk_test_packet p = n->wire[i];
            memmove(n->wire + i, n->wire + i + 1, (n->wires - i - 1) * sizeof p);
            --n->wires;
            return p;
        }
    assert(!"expected packet missing");
    return (rk_test_packet){0};
}
rk_test_packet rk_test_data(rk_test_node *n, uint64_t now, const uint8_t *plain, size_t len) {
    fips_rekey_sealed s = {0};
    assert(fips_rekey_seal(&n->machine, fips_test_instant(now), &s) == FIPS_REKEY_SEAL_OK);
    return seal_packet(&s, plain, len);
}
rk_test_packet rk_test_slot_frame(rk_test_node *n, fips_rekey_slot slot, bool k,
                                  const uint8_t *plain, size_t len) {
    fips_rekey_sealed s = {0};
    s.k = k;
    s.theirs = fips_rekey_theirs(&n->machine, slot);
    const fips_noise_session *session = fips_rekey_keys(&n->machine, slot);
    assert(session);
#ifdef FIPS_TEST_HOOKS
    assert(fips_rekey_reserve_slot(&n->machine, slot, &s.counter, &s.key));
#else
    (void)session;
    assert(!"slot forgeries are test hooks only");
#endif
    return seal_packet(&s, plain, len);
}
rk_test_packet rk_test_fresh(rk_test_local *from, const rk_test_local *to, uint32_t idx) {
    fips_rekey_wire wire = {0};
    fips_rekey_hs hs = {0};
    assert(initiate(from, &to->key, (fips_rekey_index){idx, from->indexed}, &hs, &wire));
    rk_test_packet p = {.handshake = true,
                        .kind = from->messages == 2 ? FIPS_REKEY_MSG_Msg1 : FIPS_REKEY_MSG_Setup,
                        .len = wire.len};
    memcpy(p.bytes, wire.bytes, wire.len);
    fips_rekey_hs_clear(&hs);
    return p;
}
rk_test_packet rk_test_answer(rk_test_local *l, const rk_test_packet *msg1, uint32_t idx) {
    fips_rekey_completed done = {0};
    fips_rekey_wire msg = {0};
    assert(msg1_answer(msg1, l, (fips_rekey_index){idx, true}, &done, &msg));
    rk_test_packet p = {.handshake = true,
                        .kind = l->messages == 2 ? FIPS_REKEY_MSG_Msg2 : FIPS_REKEY_MSG_Ack,
                        .len = msg.len};
    memcpy(p.bytes, msg.bytes, msg.len);
    fips_rekey_completed_clear(&done);
    return p;
}
rk_test_packet rk_test_forge3(const rk_test_packet *p, fips_peer_key other,
                              const uint8_t epoch[8]) {
    rk_test_packet out = *p;
    memcpy(out.bytes + 17, other.bytes, 32);
    memcpy(out.bytes + 81, epoch, 8);
    mac(out.bytes);
    return out;
}
void rk_test_complete_leg(rk_test_local *l, const rk_test_packet *setup, uint32_t index,
                          fips_rekey_leg *leg) {
    fips_rekey_wire reply_wire = {0};
    assert(msg1_answer(setup, l, (fips_rekey_index){index, true}, &leg->done, &reply_wire));
}
void rk_test_conserved(const rk_test_node *n) {
    fips_rekey_binding all[3];
    size_t count = fips_rekey_bindings(&n->machine, all);
    assert(count + n->external_leases == n->allocated);
    for (size_t i = 0; i < count; ++i) {
        assert(all[i].index < 1024 && n->indices[all[i].index]);
        for (size_t j = 0; j < i; ++j)
            assert(all[j].index != all[i].index);
    }
    assert(n->machine.cands.count <= n->profile.candidates);
#ifdef FIPS_TEST_HOOKS
    assert(fips_rekey_cleared_slots_are_zero(&n->machine));
#endif
}
void rk_test_close(rk_test_node *n) {
    if (!n->machine.live) {
        fips_rekey_outputs_clear(&n->out);
        return;
    }
    fips_rekey_outputs_clear(&n->out);
    fips_rekey_carry carry = {0};
    (void)fips_rekey_close_machine(&n->machine, FIPS_REKEY_CLOSE_Shutdown, &n->out, &carry);
    rk_test_execute(n, 0);
    assert(!n->allocated);
    assert(sodium_is_zero((const uint8_t *)&n->machine, sizeof n->machine));
    fips_rekey_outputs_clear(&n->out);
}
size_t rk_test_count(const rk_test_node *n, fips_rekey_output_kind kind) {
    size_t count = 0;
    for (size_t i = 0; i < n->out.count; ++i)
        if (n->out.items[i].kind == kind)
            ++count;
    return count;
}
bool rk_test_rejected(const rk_test_node *n, fips_rekey_reason reason) {
    for (size_t i = 0; i < n->out.count; ++i)
        if (n->out.items[i].kind == FIPS_REKEY_OUT_Reject && n->out.items[i].reason == reason)
            return true;
    return false;
}
bool rk_test_abandoned(const rk_test_node *n, fips_rekey_abandon why) {
    for (size_t i = 0; i < n->out.count; ++i)
        if (n->out.items[i].kind == FIPS_REKEY_OUT_Abandoned && n->out.items[i].abandon == why)
            return true;
    return false;
}
