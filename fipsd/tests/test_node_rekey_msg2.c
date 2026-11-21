/* nostrc-tyukg: a msg2 that matches no pending handshake is counted as upstream
 * counts it (node/handlers/handshake.rs msg2_body and msg2_to_link), and
 * changes nothing else. A copy of a reply that already completed, a reply with
 * no attempt to answer and a reply under an index nothing holds are the
 * handshake `unknown_connection` counter; a reply that fails its read is
 * `bad_state`, and the attempt stays so the genuine reply can still finish it.
 * Production node, dispatcher, workers and loopback transport; the wire peer
 * uses production IK and AEAD, answers the node's own msg1 and controls which
 * msg2 arrives when, in both of the node's initiator roles: the first dial and
 * a link rekey. */
#include "handshake_fixture.h"
#include "control/protocol.h"
#include <stdio.h>

/* Every handshake reject counter, so that "exactly one moved by exactly one"
 * is checked against all of them, not only the two this change touches. */
static const fipsd_metric counters[] = {
    FIPSD_METRIC_handshake_bad_state, FIPSD_METRIC_handshake_unknown_connection,
    FIPSD_METRIC_handshake_silent_backoff, FIPSD_METRIC_handshake_max_peers,
    FIPSD_METRIC_handshake_rekey_pending, FIPSD_METRIC_handshake_rekey_tiebreak,
    FIPSD_METRIC_handshake_answered_before, FIPSD_METRIC_handshake_off_link,
};
#define NCOUNTERS (sizeof counters / sizeof counters[0])
typedef struct tally { uint64_t v[NCOUNTERS]; } tally;
static tally read_counters(handshake_fixture *f)
{
    tally t;
    for (size_t i = 0; i < NCOUNTERS; ++i) t.v[i] = fipsd_metrics_get(&f->h->metrics, counters[i]);
    return t;
}
/* Only `which` moved since `before`, and by exactly one; `which` of
 * FIPSD_METRIC_COUNT means that none moved. */
static void only_moved(handshake_fixture *f, const tally *before, fipsd_metric which)
{
    tally now = read_counters(f);
    for (size_t i = 0; i < NCOUNTERS; ++i) {
        uint64_t want = before->v[i] + (counters[i] == which);
        if (now.v[i] != want) fprintf(stderr, "handshake counter %zu: %llu -> %llu, wanted %llu\n", i,
            (unsigned long long)before->v[i], (unsigned long long)now.v[i], (unsigned long long)want);
        assert(now.v[i] == want);
    }
}
/* The msg2 changed no node state at all: no link, leg, index, binding, key or
 * datagram moved (hf_state is read on the dispatcher around the packet). */
static void untouched(handshake_fixture *f, const tally *before, fipsd_metric which)
{
    hf_unchanged(&f->before, &f->after);
    only_moved(f, before, which);
}
static void refused(handshake_fixture *f, const uint8_t *msg2, size_t len, fipsd_metric which)
{
    tally before = read_counters(f);
    hf_send(f, f->client, msg2, len);
    untouched(f, &before, which);
}
/* A msg2 whose ephemeral is intact and whose sealed epoch no longer opens. */
static size_t forged(const fips_rekey_wire *msg2, uint8_t *out)
{
    memcpy(out, msg2->bytes, msg2->len);
    out[msg2->len - 1] ^= 0x01;
    return msg2->len;
}
/* The same msg2 addressed to an index nothing holds. */
static size_t strayed(const fips_rekey_wire *msg2, uint8_t *out)
{
    const uint32_t stray = 0x5eedf00du;
    fips_fmp_msg2_header header;
    assert(fips_fmp_msg2_header_parse(msg2->bytes, msg2->len, &header) && header.receiver_idx.value != stray);
    memcpy(out, msg2->bytes, msg2->len);
    for (unsigned i = 0; i < 4; ++i) out[8 + i] = (uint8_t)(stray >> (8 * i)); /* receiver_idx, little endian */
    assert(fips_fmp_msg2_header_parse(out, msg2->len, &header) && header.receiver_idx.value == stray);
    return msg2->len;
}
/* The wire peer answers the node's msg1 as a responder would: the msg2, and
 * the keys the node's own session will carry. */
static fips_rekey_wire answer(handshake_fixture *f, const fipsd_transport_event *msg1, uint32_t index, fips_rekey_completed *done)
{
    fips_fmp_ik_msg1 token = {0};
    assert(fips_fmp_rekey_read_msg1(&f->local, msg1->bytes, msg1->size, &token));
    fips_rekey_wire msg2 = {0};
    assert(fips_fmp_rekey_profile.msg1_answer(&token, &f->local, (fips_rekey_index){index, true}, done, &msg2));
    return msg2;
}
/* The control output carries upstream's field names and the registry's counts:
 * show_metrics reads the published snapshot, so publish one first. */
static void publish(handshake_fixture *f, void *arg) { (void)arg; hs_publish(f->h, hs_now(f->h)); }
static void control_reports(handshake_fixture *f, uint64_t bad_state, uint64_t unknown_connection)
{
    hf_call(f, publish, NULL);
    fipsd_control_provider p = fipsd_node_handlers_provider(f->h);
    GoRefPtr ref = p.acquire(p.user);
    assert(ref.ptr);
    json_t *out = fipsd_control_query(ref.ptr, "show_metrics", NULL);
    go_refptr_release(&ref);
    assert(out);
    json_t *hs = json_object_get(json_object_get(out, "data"), "handshake");
    assert(json_integer_value(json_object_get(hs, "bad_state")) == (json_int_t)bad_state);
    assert(json_integer_value(json_object_get(hs, "unknown_connection")) == (json_int_t)unknown_connection);
    json_decref(out);
}

/* The node's own dial to the wire peer. */
static void dial_remote(handshake_fixture *f, void *arg)
{
    (void)arg;
    assert(hs_dial(f->h, &f->remote, (fips_transport_id){1}, fipsd_loopback_transport_address(f->client), false));
}
static void first_dial(void)
{
    handshake_fixture f; hf_init(&f);
    memset(f.local.epoch, 0xd1, sizeof f.local.epoch);
    hf_call(&f, dial_remote, NULL);
    fipsd_transport_event *msg1 = hf_wire(&f, 1);
    fips_rekey_completed done = {0};
    fips_rekey_wire msg2 = answer(&f, msg1, 41, &done);
    free(msg1);
    hf_state dialing; hf_snapshot(&f, &dialing);
    assert(dialing.legs == 1 && dialing.peers == 0);

    /* A reply that does not authenticate is refused where it is read, and the
     * dial stays: the index it names travelled in cleartext in the msg1. */
    uint8_t bad[FIPS_REKEY_WIRE_MAX];
    size_t bad_len = forged(&msg2, bad);
    for (int i = 0; i < 3; ++i) {
        refused(&f, bad, bad_len, FIPSD_METRIC_handshake_bad_state);
        assert(f.after.legs == 1 && f.after.peers == 0);
    }
    /* A reply under an index no dial holds finds nothing to read. */
    uint8_t stray[FIPS_REKEY_WIRE_MAX];
    size_t stray_len = strayed(&msg2, stray);
    refused(&f, stray, stray_len, FIPSD_METRIC_handshake_unknown_connection);
    assert(f.after.legs == 1 && f.after.peers == 0);

    /* The genuine reply still completes the dial, and counts nothing. */
    tally before = read_counters(&f);
    hf_send(&f, f.client, msg2.bytes, msg2.len);
    only_moved(&f, &before, FIPSD_METRIC_COUNT);
    hf_state up; hf_snapshot(&f, &up);
    assert(up.peers == 1 && up.legs == 0 && up.promoted == dialing.promoted + 1);

    /* A second copy of it, the dial being over, is a late reply. */
    refused(&f, msg2.bytes, msg2.len, FIPSD_METRIC_handshake_unknown_connection);
    assert(f.after.peers == 1 && f.after.legs == 0 && f.after.promoted == up.promoted);
    refused(&f, msg2.bytes, msg2.len, FIPSD_METRIC_handshake_unknown_connection);
    refused(&f, stray, stray_len, FIPSD_METRIC_handshake_unknown_connection);
    hf_state end; hf_snapshot(&f, &end);
    assert(end.peers == 1 && end.legs == 0 && end.promoted == up.promoted && end.info.link.value == up.info.link.value);
    assert(end.indices == up.indices && end.bindings == up.bindings);
    control_reports(&f, 3, 4);
    fips_rekey_completed_clear(&done);
    hf_destroy(&f);
    puts("first dial: unauthenticated x3 = bad_state 3; stray, late x3 = unknown_connection 4; nothing else moved");
}

/* A link rekey in a few seconds: the wire peer never starts one, so the node
 * is the initiator. */
static void fast_rekey(fipsd_node_handlers_config *c, const fipsd_config *cfg)
{
    fipsd_config_core_params(cfg, &c->core);
    c->has_core = true;
    c->core.link_rekey.after = (fips_duration){1, 0}; c->core.link_rekey.jitter = 0;
    c->core.link_rekey.link_age = c->core.link_rekey.session_age = (fips_duration){0, 0};
    c->core.link_rekey.floor_link = c->core.link_rekey.floor_session = (fips_duration){0, 0};
}
/* One authenticated link frame under `session`, to the node's index `index`. */
static size_t seal_under(fips_noise_session *session, uint32_t index, uint8_t flags, uint8_t *wire, size_t capacity)
{
    uint8_t plain[32], hb = FIPS_LINK_Heartbeat;
    fips_writer w; fips_writer_init(&w, plain, sizeof plain);
    assert(fips_fmp_prepend_inner_header(0, &hb, 1, &w, NULL) == FIPS_OK);
    size_t plen = fips_writer_len(&w), len = 0, aad = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
    uint64_t counter = fips_noise_session_current_send_counter(session);
    fips_fmp_build_established_header((fips_session_index){index}, counter, flags, (uint16_t)plen, wire);
    assert(fips_noise_session_seal(session, wire, aad, plain, plen, wire + aad, capacity - aad, &len) == FIPS_NOISE_OK);
    return aad + len;
}
static bool same_recv_key(const hf_state *a, const hf_state *b)
{ return !memcmp(a->keys.recv_cipher.key.bytes, b->keys.recv_cipher.key.bytes, sizeof a->keys.recv_cipher.key.bytes); }
static void link_rekey(void)
{
    handshake_fixture f; hf_init_with(&f, fast_rekey);
    /* The wire peer dials the node, and the link is up. */
    hf_start(&f, 0xc2);
    hf_send(&f, f.client, f.msg1.bytes, f.msg1.len);
    hf_finish(&f);
    hf_heartbeat(&f);
    hf_state live; hf_snapshot(&f, &live);
    assert(live.peers == 1 && live.legs == 0);

    /* The node's own rekey msg1 goes out once the trigger is due, and the wire
     * peer answers it under the epoch it already holds, as a rekey. */
    fipsd_transport_event *msg1 = hf_wire(&f, 1);
    fips_rekey_completed done = {0};
    fips_rekey_wire msg2 = answer(&f, msg1, f.index + 40, &done);
    free(msg1);

    /* A reply that does not authenticate is refused where it is read, and the
     * cycle stays. */
    uint8_t bad[FIPS_REKEY_WIRE_MAX];
    size_t bad_len = forged(&msg2, bad);
    for (int i = 0; i < 8; ++i) refused(&f, bad, bad_len, FIPSD_METRIC_handshake_bad_state);
    /* A different address can name the cleartext index, but after eight
     * refused reads it is counted off_link without reading the msg2. */
    tally budget = read_counters(&f);
    hf_send(&f, f.alternate, bad, bad_len);
    untouched(&f, &budget, FIPSD_METRIC_handshake_off_link);
    /* Even a genuine reply from that address is unread; the peer's established
     * link remains admissible and the genuine msg2 below still completes. */
    budget = read_counters(&f);
    hf_send(&f, f.alternate, msg2.bytes, msg2.len);
    untouched(&f, &budget, FIPSD_METRIC_handshake_off_link);
    /* A reply under an index nothing holds. */
    uint8_t stray[FIPS_REKEY_WIRE_MAX];
    size_t stray_len = strayed(&msg2, stray);
    refused(&f, stray, stray_len, FIPSD_METRIC_handshake_unknown_connection);

    /* The genuine reply completes the attempt, still counting nothing. */
    hf_state before; hf_snapshot(&f, &before);
    tally none = read_counters(&f);
    hf_send(&f, f.client, msg2.bytes, msg2.len);
    only_moved(&f, &none, FIPSD_METRIC_COUNT);
    assert(f.after.peers == 1 && f.after.legs == 0 && f.after.promoted == before.promoted);
    assert(f.after.info.link.value == before.info.link.value && same_recv_key(&before, &f.after));

    /* The rekey is complete and its cutover pending: a second copy is a late
     * reply, and changes nothing. */
    refused(&f, msg2.bytes, msg2.len, FIPSD_METRIC_handshake_unknown_connection);
    refused(&f, msg2.bytes, msg2.len, FIPSD_METRIC_handshake_unknown_connection);

    /* The cutover: the first frame under the new key, with the K bit flipped,
     * to the index the node chose for it. */
    uint8_t wire[128];
    size_t wire_len = seal_under(&done.session, done.theirs.value, FIPS_FMP_FLAG_KEY_EPOCH, wire, sizeof wire);
    hf_open(&f, f.client, wire, wire_len);
    hf_state cut; hf_snapshot(&f, &cut);
    assert(!same_recv_key(&before, &cut) && cut.peers == 1 && cut.info.link.value == before.info.link.value);
    /* After it the index is the link's current one, and a late copy is still
     * only that. */
    refused(&f, msg2.bytes, msg2.len, FIPSD_METRIC_handshake_unknown_connection);
    refused(&f, stray, stray_len, FIPSD_METRIC_handshake_unknown_connection);
    hf_state end; hf_snapshot(&f, &end);
    assert(end.peers == 1 && end.legs == 0 && end.promoted == live.promoted && same_recv_key(&cut, &end));
    assert(end.info.link.value == live.info.link.value && end.indices == cut.indices && end.bindings == cut.bindings);
    control_reports(&f, 8, 5);
    fips_rekey_completed_clear(&done);
    hf_destroy(&f);
    puts("link rekey: unauthenticated x8 = bad_state 8; off-link x2 unread; stray, duplicate x2 before cutover, late x2 after = unknown_connection 5; nothing else moved");
}
int main(void)
{
    first_dial();
    link_rekey();
    return 0;
}
