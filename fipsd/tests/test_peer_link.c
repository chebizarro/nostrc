#include "peer_test.h"
#include "peer/link.h"
#include "peer/table.h"
#include <fips/proto/rekey/keys.h>
#include <fips/noise/aead.h>

/* Refusing test handshake profile, NOT the concurrently written IK profile.
 * Real Noise session keys, reserve, AEAD and replay paths below are production
 * code. This fixture has no fake successful handshake or session-slot access. */
static bool initiate(void *local, const fips_peer_key *peer, fips_rekey_index index,
    fips_rekey_hs *hs, fips_rekey_wire *wire)
{ (void)local; (void)peer; (void)index; (void)hs; (void)wire; return false; }
static bool reply(const fips_rekey_hs *hs, void *local, const uint8_t *bytes, size_t len, fips_rekey_read *out)
{ (void)hs; (void)local; (void)bytes; (void)len; (void)out; return false; }
static bool answer(const void *token, void *local, fips_rekey_index index,
    fips_rekey_completed *done, fips_rekey_wire *wire)
{ (void)token; (void)local; (void)index; (void)done; (void)wire; return false; }
static bool prompt(fips_duration elapsed, fips_rekey_wire *wire)
{ (void)elapsed; wire->len = 1; wire->bytes[0] = 0x51; return true; }
static bool is_probe(const uint8_t *bytes, size_t len) { return len && bytes[0] == 0x51; }
static fips_rekey_index index_of(const uint8_t *bytes, size_t len)
{ (void)bytes; (void)len; return (fips_rekey_index){0}; }
static uint16_t jitter(void *ctx, uint16_t bound) { (void)ctx; return (uint16_t)(bound / 2); }
static bool draw(void *ctx, uint32_t *index)
{
    bool *used = ctx;
    if (*used) return false;
    *used = true; *index = 42; return true;
}
static const fips_rekey_profile test_profile = {
    .layer = FIPS_REKEY_LAYER_Fmp, .messages = 2, .commit = FIPS_REKEY_COMMIT_AtAnswer,
    .initiator_cutover = FIPS_REKEY_AfterDelay, .responder_cutover = FIPS_REKEY_OnFrame,
    .select = FIPS_REKEY_ByIndex, .indexed = true, .keys = &fips_rekey_noise_keys,
    .initiate = initiate, .reply = reply, .msg1_answer = answer,
    .prompt = prompt, .is_probe = is_probe, .index = index_of
};
int peer_link_tests(void)
{
    size_t leaks_before = fips_rekey_leaked();
    fipsd_active_peer *p = new_peer(1, 1, 1); CHECK(p);
    fips_rekey_outputs out = {0};
    fips_rekey_tag_source tags = {0};
    bool used = false;
    uint8_t scratch[256];
    fips_rekey_env env = {.indices = {draw, &used}, .transport = 1, .tags = &tags,
        .scratch = scratch, .scratch_capacity = sizeof scratch, .jitter_draw = jitter};
    fips_rekey_sealed job = {0};
    CHECK(fipsd_active_peer_link_seal(p, fips_test_instant(0), &job) == FIPS_REKEY_SEAL_NotEstablished);
    CHECK(fipsd_active_peer_link_step(p, fips_test_instant(0),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_Poll}, &env, &out) == FIPS_ERR_RANGE);
    uint8_t send_bytes[32], recv_bytes[32], hash[32] = {0}, remote[33] = {2};
    memset(send_bytes, 1, sizeof send_bytes); memset(recv_bytes, 2, sizeof recv_bytes);
    remote[1] = 1;
    fips_aead_key send, recv;
    CHECK(fips_aead_key_init(&send, send_bytes) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&recv, recv_bytes) == FIPS_AEAD_OK);
    fips_rekey_completed first = {0};
    first.remote.bytes[0] = 1;
    first.epoch[0] = 1;
    first.theirs = (fips_rekey_index){7, true};
    CHECK(fips_noise_session_from_handshake(&first.session, FIPS_HANDSHAKE_INITIATOR,
        &send, &recv, hash, remote, first.epoch) == FIPS_NOISE_OK);
    fips_rekey_ids ids = {.transport = 1, .theirs = {7, true}};
    CHECK(fips_rekey_allocate(env.indices, &ids.lease));
    fips_rekey_setup_facts facts = {.has_peer_epoch = true, .peer_epoch = {1},
        .link_since = fips_test_instant(0), .started = fips_test_instant(0), .last_rx = fips_test_instant(0)};
    fips_node_addr me = peer_id(2).node_addr;
    CHECK(fipsd_active_peer_link_established(p, NULL, fips_rekey_params_fmp(), me,
        &first, &ids, &facts, fips_test_instant(0), &env, &out) == FIPS_ERR_RANGE);
    CHECK(ids.lease.live && first.session.send_cipher.has_key);
    CHECK(fipsd_active_peer_link_established(p, &test_profile, fips_rekey_params_fmp(), me,
        &first, &ids, &facts, fips_test_instant(0), &env, &out) == FIPS_OK);
    CHECK(!ids.lease.live && !first.session.send_cipher.has_key);
    CHECK(fipsd_active_peer_link_view(p).proven && !fipsd_active_peer_destroy(p));
    fips_rekey_outputs_clear(&out);
    /* Per-job independent bare clones; counters are reserved by this owner. */
    uint8_t cipher[64]; size_t len = 0;
    for (uint64_t i = 0; i < 3; ++i) {
        CHECK(fipsd_active_peer_link_seal(p, fips_test_instant(10), &job) == FIPS_REKEY_SEAL_OK);
        CHECK(job.counter == i);
        CHECK(fips_cipher_state_seal(&job.key, NULL, 0, (const uint8_t *)"data", 4,
            cipher, sizeof cipher, &len) == FIPS_NOISE_NonceOverflow);
        CHECK(fips_cipher_state_seal_with_counter(&job.key, job.counter, NULL, 0,
            (const uint8_t *)"data", 4, cipher, sizeof cipher, &len) == FIPS_NOISE_OK);
        fips_rekey_sealed_clear(&job);
    }
    /* Real AEAD decrypt -> dispatcher replay acceptance -> duplicate refusal. */
    CHECK(fips_aead_key_init(&recv, recv_bytes) == FIPS_AEAD_OK);
    CHECK(fips_aead_seal(&recv, 0, NULL, 0, (const uint8_t *)"data", 4, cipher, sizeof cipher, &len) == FIPS_AEAD_OK);
    uint8_t saved[64]; memcpy(saved, cipher, len);
    fips_rekey_input frame = {.kind = FIPS_REKEY_IN_Frame, .buf = cipher, .len = len,
        .hint = {.counter = 0, .index = 42, .body = 0}};
    CHECK(fipsd_active_peer_link_step(p, fips_test_instant(20), frame, &env, &out) == FIPS_OK);
    bool delivered = false;
    for (size_t i = 0; i < out.count; ++i) if (out.items[i].kind == FIPS_REKEY_OUT_Deliver) delivered = true;
    CHECK(delivered);
    fips_rekey_outputs_clear(&out); memcpy(cipher, saved, len);
    CHECK(fipsd_active_peer_link_step(p, fips_test_instant(21), frame, &env, &out) == FIPS_OK);
    CHECK(out.count == 1 && out.items[0].kind == FIPS_REKEY_OUT_Reject && out.items[0].reason == FIPS_REKEY_REASON_Replay);
    fips_rekey_outputs_clear(&out); fips_aead_key_destroy(&recv);
    fipsd_transport_caps limit = {.transport = {2}, .caps = {.peers = 1, .links = 1}};
    fipsd_peer_table *table = fipsd_peer_table_create(2, (fipsd_peer_caps){0}, &limit, 1); CHECK(table);
    CHECK(fipsd_peer_table_insert(table, p) == FIPSD_ADMIT_OK);
    CHECK(!fipsd_peer_table_destroy(table));
    fips_node_addr node = peer_id(1).node_addr;
    CHECK(fipsd_peer_table_remove(table, &node) == FIPSD_ADMIT_BUSY);
    /* Dispatcher may move an attached link, but cannot steal another peer's
     * pair or overdraw its transport. Refusals leave endpoint AND ticket intact. */
    fipsd_active_peer *other = new_peer(3, 2, 9); CHECK(other);
    CHECK(fipsd_peer_table_insert(table, other) == FIPSD_ADMIT_OK);
    fips_transport_addr taken = peer_addr(9), fresh = peer_addr(8);
    CHECK(fipsd_peer_table_rebind(table, &node, (fips_transport_id){2}, &taken) == FIPSD_ADMIT_DUPLICATE);
    CHECK(fipsd_peer_table_rebind(table, &node, (fips_transport_id){2}, &fresh) == FIPSD_ADMIT_TRANSPORT);
    CHECK(fipsd_active_peer_info(p)->transport.value == 1 && fipsd_active_peer_info(p)->addr.bytes[0] == 1);
    fips_node_addr other_node = peer_id(3).node_addr;
    CHECK(fipsd_peer_table_remove(table, &other_node) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_publish(table));
    GoRefPtr before = fipsd_peer_table_snapshot_load(table);
    fips_rekey *machine = fipsd_active_peer_link_machine(p);
    CHECK(fipsd_peer_table_rebind(table, &node, (fips_transport_id){2}, &fresh) == FIPSD_ADMIT_OK);
    CHECK(fipsd_peer_table_endpoint(table, (fips_transport_id){2}, &fresh) == p);
    CHECK(fipsd_active_peer_link_machine(p) == machine && fipsd_active_peer_link_attached(p));
    CHECK(fipsd_active_peer_link_seal(p, fips_test_instant(21), &job) == FIPS_REKEY_SEAL_OK && job.counter == 3);
    fips_rekey_sealed_clear(&job);
    env.transport = 2;
    CHECK(fipsd_active_peer_link_step(p, fips_test_instant(21),
        (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, &env, &out) == FIPS_OK);
    fips_rekey_outputs_clear(&out);
    CHECK(fipsd_peer_table_publish(table));
    GoRefPtr after = fipsd_peer_table_snapshot_load(table);
    CHECK(((fipsd_peer_table_snapshot *)before.ptr)->peers[0].transport.value == 1);
    CHECK(((fipsd_peer_table_snapshot *)after.ptr)->peers[0].transport.value == 2);
    go_refptr_release(&before); go_refptr_release(&after);
    /* Same-transport address changes use exactly the same ownership rule. */
    CHECK(fipsd_peer_table_rebind(table, &node, (fips_transport_id){2}, &taken) == FIPSD_ADMIT_OK);
    CHECK(!fipsd_peer_table_endpoint(table, (fips_transport_id){2}, &fresh));
    fips_rekey_carry carry;
    fipsd_active_peer_link_close(p, FIPS_REKEY_CLOSE_Shutdown, &out, &carry);
    size_t released = 0;
    for (size_t i = 0; i < out.count; ++i) if (out.items[i].kind == FIPS_REKEY_OUT_Release) {
        uint32_t index;
        CHECK(fips_rekey_index_lease_release(&out.items[i].lease, &index) && index == 42 && used);
        used = false; ++released;
    }
    CHECK(released == 1 && !used && !fipsd_active_peer_link_attached(p));
    fips_rekey_outputs_clear(&out);
    CHECK(fipsd_active_peer_link_seal(p, fips_test_instant(22), &job) == FIPS_REKEY_SEAL_NotEstablished);
    CHECK(fipsd_peer_table_destroy(table));
    CHECK(fips_rekey_leaked() == leaks_before);
    return 0;
}
