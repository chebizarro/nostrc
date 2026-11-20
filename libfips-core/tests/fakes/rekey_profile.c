#include "rekey_profile.h"
#include <fips/identity/key.h>
#include <assert.h>
#include <sodium.h>
#include <string.h>
static bool allocate(void *ctx, uint32_t *out) {
    rp_node *n = ctx;
    for (size_t i = 0; i < 256; ++i) {
        uint32_t index = n->next_index++ % 256;
        if (!n->indices[index]) {
            n->indices[index] = true; ++n->allocated; *out = index; return true;
        }
    }
    return false;
}
static uint16_t zero_jitter(void *ctx, uint16_t bound) { (void)ctx; (void)bound; return 0; }
bool rp_init(rp_node *n, uint64_t seed, fips_entropy entropy, const uint8_t epoch[8]) {
    memset(n, 0, sizeof *n);
    uint8_t secret[32];
    fips_seeded_entropy keygen = {seed ^ UINT64_C(0x6b65797061697273)};
    fips_entropy kg = fips_seeded_entropy_adapter(&keygen);
    kg.fill(kg.ctx, secret, sizeof secret);
    fips_fixed_entropy blind = {0x42};
    fips_entropy blinding = fips_fixed_entropy_adapter(&blind);
    bool ok = fips_identity_init_secp(blinding, &n->secp) == FIPS_IDENTITY_OK &&
        fips_identity_from_secret_bytes(n->secp, entropy, secret, &n->identity) == FIPS_IDENTITY_OK;
    sodium_memzero(secret, sizeof secret);
    if (!ok) return false;
    n->local = (fips_rekey_node_keys){.secp = n->secp, .keypair = n->identity, .entropy = entropy};
    memcpy(n->local.epoch, epoch, 8);
    fips_xonly_pubkey key = fips_identity_pubkey(n->identity);
    n->peer = fips_peer_key_from_xonly(&key);
    n->me = fips_identity_node_addr(n->identity);
    n->env = (fips_rekey_env){.indices = {allocate, n}, .transport = 7, .tags = &n->tags,
        .local = &n->local, .jitter_draw = zero_jitter, .scratch = n->scratch,
        .scratch_capacity = sizeof n->scratch};
    return true;
}
bool rp_handshake(rp_node *a, rp_node *b, const fips_rekey_profile *p, fips_rekey_completed done[2]) {
    fips_rekey_hs i = {0}, r = {0};
    fips_rekey_wire msg1 = {0}, msg2 = {0};
    fips_rekey_read read = {0};
    fips_fmp_ik_msg1 token = {0};
    a->remote = b->me; b->remote = a->me;
    bool ok = p->initiate(&a->local, &b->peer, (fips_rekey_index){10, p->indexed}, &i, &msg1);
    if (ok && p->indexed)
        ok = fips_fmp_rekey_read_msg1(&b->local, msg1.bytes, msg1.len, &token) &&
            p->msg1_answer(&token, &b->local, (fips_rekey_index){20, true}, &done[1], &msg2);
    else if (ok) ok = p->answer(&b->local, msg1.bytes, msg1.len, &r, &msg2);
    if (ok) ok = p->reply(&i, &a->local, msg2.bytes, msg2.len, &read);
    if (ok && !p->indexed) ok = p->complete(&r, read.fin.bytes, read.fin.len, &done[1]);
    if (ok) {
        ok = fips_noise_session_take_cleared(&read.done.session, &done[0].session) == FIPS_NOISE_OK;
        done[0].remote = read.done.remote;
        memcpy(done[0].epoch, read.done.epoch, 8);
        done[0].theirs = read.done.theirs;
    }
    fips_rekey_completed_clear(&read.done);
    fips_rekey_hs_clear(&i); fips_rekey_hs_clear(&r); fips_fmp_rekey_msg1_clear(&token);
    sodium_memzero(&read, sizeof read);
    return ok;
}
bool rp_pair(rp_node *a, rp_node *b, const fips_rekey_profile *p) {
    fips_rekey_completed done[2] = {0};
    bool ok = rp_handshake(a, b, p, done);
    rp_node *nodes[2] = {a, b};
    for (size_t k = 0; ok && k < 2; ++k) {
        rp_node *n = nodes[k];
        fips_rekey_ids ids = {.transport = 7, .theirs = done[k].theirs};
        if (p->indexed) {
            uint32_t index = k ? 20 : 10;
            n->next_index = index;
            ok = fips_rekey_allocate(n->env.indices, &ids.lease);
        }
        fips_rekey_setup_facts facts = {.has_peer_epoch = true, .has_peer_k = true,
            .started = fips_test_instant(0), .link_since = fips_test_instant(0), .last_rx = fips_test_instant(0)};
        memcpy(facts.peer_epoch, done[k].epoch, 8);
        if (ok) ok = fips_rekey_established(&n->machine, p,
            p->indexed ? fips_rekey_params_fmp() : fips_rekey_params_fsp(),
            (fips_rekey_bond){.kind = FIPS_REKEY_BOND_Key, .key = done[k].remote}, n->me,
            &done[k], &ids, &facts, fips_test_instant(0), &n->env, &n->out) == FIPS_OK;
        if (!ok && ids.lease.live) {
            uint32_t index;
            if (fips_rekey_index_lease_release(&ids.lease, &index)) { n->indices[index] = false; --n->allocated; }
        }
        if (ok) ok = rp_execute(n, 0, NULL, 0);
    }
    for (size_t k = 0; k < 2; ++k) fips_rekey_completed_clear(&done[k]);
    return ok;
}
static bool coords(void *ctx, fips_rekey_route route, fips_rekey_route_handle handle,
                   fips_fsp_coords *src, fips_fsp_coords *dest) {
    rp_node *n = ctx;
    (void)route; (void)handle;
    if (!n->with_coords) return false;
    *src = (fips_fsp_coords){n->me.bytes, 1}; *dest = (fips_fsp_coords){n->remote.bytes, 1};
    return true;
}
static void record(rp_node *n, fips_rekey_output_kind kind) {
    assert(n->order_count < FIPS_REKEY_OUTPUTS_MAX);
    n->order[n->order_count++] = kind;
}
static fips_status send_packet(void *ctx, const fips_rekey_output *o, const uint8_t *p, size_t len) {
    rp_node *n = ctx;
    record(n, o->kind);
    if (n->fail_send) return FIPS_ERR_RANGE;
    assert(n->queued < RP_QUEUE_MAX && len <= RP_PACKET_MAX);
    rp_packet *packet = &n->queue[n->queued++];
    packet->len = len; memcpy(packet->bytes, p, len);
    return FIPS_OK;
}
static void effect(void *ctx, const fips_rekey_output *o, const uint8_t *p, size_t len) {
    rp_node *n = ctx;
    (void)p; (void)len;
    record(n, o->kind);
    if (o->kind == FIPS_REKEY_OUT_Deliver) ++n->deliveries;
    if (o->kind == FIPS_REKEY_OUT_Installed) ++n->installed;
    if (o->kind == FIPS_REKEY_OUT_Retired) ++n->retired;
}
static void release_index(void *ctx, uint32_t index) {
    rp_node *n = ctx;
    record(n, FIPS_REKEY_OUT_Release);
    assert(index < 256 && n->indices[index] && n->allocated);
    n->indices[index] = false; --n->allocated;
}
bool rp_execute_machine(rp_node *n, fips_rekey *machine, uint64_t now, const uint8_t *input, size_t len) {
    n->order_count = 0;
    fips_session_executor x = {.ctx = n, .coords = coords, .send = send_packet,
        .effect = effect, .release = release_index, .wire = n->wire, .capacity = sizeof n->wire};
    return fips_session_execute(machine, fips_test_instant(now), &n->out, input, len, &x) == FIPS_OK;
}
bool rp_execute(rp_node *n, uint64_t now, const uint8_t *input, size_t len) {
    return rp_execute_machine(n, &n->machine, now, input, len);
}
bool rp_receive_machine(rp_node *n, fips_rekey *machine, uint64_t now, rp_packet *packet) {
    fips_rekey_outputs_clear(&n->out);
    fips_status status;
    if (machine->profile == &fips_fsp_rekey_profile)
        status = fips_fsp_rekey_receive(machine, fips_test_instant(now), n->remote, 1,
            packet->bytes, packet->len, &n->env, &n->out);
    else {
        fips_fmp_msg1_header header;
        if (fips_fmp_msg1_header_parse(packet->bytes, packet->len, &header)) {
            fips_fmp_ik_msg1 token = {0};
            if (!fips_fmp_rekey_read_msg1(&n->local, packet->bytes, packet->len, &token)) return false;
            fips_fmp_inbound_decision d = {.kind = FIPS_FMP_IN_Rekey, .peer = n->remote};
            status = fips_fmp_rekey_inbound(machine, fips_test_instant(now), &d, &token,
                true, true, &n->env, &n->out);
            bool ok = status == FIPS_OK && rp_execute_machine(n, machine, now, packet->bytes, packet->len);
            fips_fmp_rekey_msg1_clear(&token);
            return ok;
        }
        status = fips_fmp_rekey_receive(machine, fips_test_instant(now),
            packet->bytes, packet->len, &n->env, &n->out);
    }
    return status == FIPS_OK && rp_execute_machine(n, machine, now, packet->bytes, packet->len);
}
bool rp_receive(rp_node *n, uint64_t now, rp_packet *packet) {
    return rp_receive_machine(n, &n->machine, now, packet);
}
bool rp_poll(rp_node *n, uint64_t now) {
    fips_rekey_outputs_clear(&n->out);
    return fips_rekey_step(&n->machine, fips_test_instant(now),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &n->env, &n->out) == FIPS_OK && rp_execute(n, now, NULL, 0);
}
bool rp_data(rp_node *n, uint64_t now, rp_packet *packet) {
    uint8_t plain[11];
    fips_rekey_sealed s = {0};
    if (fips_rekey_seal(&n->machine, fips_test_instant(now), &s) != FIPS_REKEY_SEAL_OK) return false;
    fips_writer w;
    fips_writer_init(&w, plain, sizeof plain);
    const uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t ms = (uint32_t)s.elapsed.secs * 1000 + s.elapsed.nanos / 1000000;
    bool ok;
    if (n->machine.profile == &fips_fsp_rekey_profile)
        ok = fips_fsp_prepend_inner_header(ms, FIPS_SESSION_DataPacket, 0, data, sizeof data, &w, NULL) == FIPS_OK;
    else {
        const uint8_t heartbeat[] = {FIPS_LINK_Heartbeat};
        ok = fips_fmp_prepend_inner_header(ms, heartbeat, sizeof heartbeat, &w, NULL) == FIPS_OK;
    }
    if (ok) ok = fips_session_encode_sealed(n->machine.profile, &s, plain, fips_writer_len(&w),
        packet->bytes, sizeof packet->bytes, &packet->len) == FIPS_OK;
    fips_rekey_sealed_clear(&s);
    fips_rekey_outputs_clear(&n->out);
    if (ok) ok = fips_rekey_step(&n->machine, fips_test_instant(now),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, &n->env, &n->out) == FIPS_OK;
    return ok;
}
void rp_close(rp_node *n) {
    fips_rekey_outputs_clear(&n->out);
    if (n->machine.live) {
        fips_rekey_carry carry = {0};
        (void)fips_rekey_close_machine(&n->machine, FIPS_REKEY_CLOSE_Shutdown, &n->out, &carry);
        assert(rp_execute(n, 0, NULL, 0));
    }
    assert(!n->allocated);
}
void rp_destroy(rp_node *n) {
    rp_close(n);
    fips_identity_destroy(n->identity); fips_identity_destroy_secp(n->secp);
    sodium_memzero(n, sizeof *n);
}
