/* H1 (nostrc-f66sr.7): a captured earlier-epoch IK msg1 must not evict the live
 * link. Production node, dispatcher, workers and loopback transport; the wire
 * peer uses production IK/AEAD but controls restart, replay and confirmation. */
#include "handshake_fixture.h"
#include "test_tmpdir.h"
#include <stdio.h>

static bool same_recv_key(const hf_state *a, const hf_state *b)
{ return !memcmp(a->keys.recv_cipher.key.bytes, b->keys.recv_cipher.key.bytes, sizeof a->keys.recv_cipher.key.bytes); }
/* Live-link invariants that no held, refused or forged restart may touch. */
static void incumbent_intact(const hf_state *live, const hf_state *s)
{
    assert(s->peers == 1 && s->peer == live->peer && s->promoted == live->promoted);
    assert(s->bindings == live->bindings && s->legs == 0 && same_recv_key(live, s));
    assert(fips_transport_addr_equal(&s->info.addr, &live->info.addr) && s->info.link.value == live->info.link.value);
}
static void candidate_index(handshake_fixture *f, void *arg)
{
    peer_rec *r = hs_rec_find(f->h, &f->remote.node_addr);
    assert(r && r->restart.used && r->restart.lease.live);
    *(uint32_t *)arg = r->restart.lease.index;
}
/* Hold deadlines are poll deadlines, not sleeps: drive the poll past them. */
static void expire(handshake_fixture *f, void *arg)
{
    (void)arg;
    fips_instant later = fips_deadline(hs_now(f->h), fips_duration_from_millis(2 * f->h->cfg.timing.handshake_timeout_ms));
    (void)hs_legs_poll(f->h, later);
}
/* Every expired-candidate tombstone lapses now: the same msg1 may be challenged again. */
static void cooldown_lapsed(handshake_fixture *f, void *arg)
{
    (void)arg; handlers *h = f->h;
    for (size_t i = 0; i < FIPSD_RESTART_HISTORY; ++i)
        if (h->retired[i].used && !h->retired[i].has_epoch) h->retired[i].until = hs_now(h);
}
/* A real AEAD link message under the wire peer's current keys, addressed to an explicit index. */
static size_t seal_link(handshake_fixture *f, uint32_t index, const uint8_t *message, size_t mlen, uint8_t *wire, size_t capacity)
{
    uint8_t plain[32];
    fips_writer w; fips_writer_init(&w, plain, sizeof plain);
    assert(fips_fmp_prepend_inner_header(0, message, mlen, &w, NULL) == FIPS_OK);
    size_t plen = fips_writer_len(&w), len = 0, aad = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
    uint64_t counter = fips_noise_session_current_send_counter(&f->live.session);
    fips_fmp_build_established_header((fips_session_index){index}, counter, 0, (uint16_t)plen, wire);
    assert(fips_noise_session_seal(&f->live.session, wire, aad, plain, plen, wire + aad, capacity - aad, &len) == FIPS_NOISE_OK);
    return aad + len;
}
static size_t seal_heartbeat(handshake_fixture *f, uint32_t index, uint8_t *wire, size_t capacity)
{ uint8_t hb = FIPS_LINK_Heartbeat; return seal_link(f, index, &hb, 1, wire, capacity); }
static void frame_at(handshake_fixture *f, fipsd_transport *from, uint32_t index)
{
    uint8_t wire[128];
    size_t len = seal_heartbeat(f, index, wire, sizeof wire);
    hf_open(f, from, wire, len);
}
/* Traffic continues on the live link: one more authenticated frame delivered. */
static void traffic(handshake_fixture *f, hf_state *live)
{
    hf_heartbeat(f);
    hf_state now; hf_snapshot(f, &now);
    assert(now.frames_in == live->frames_in + 1);
    incumbent_intact(live, &now);
    *live = now;
}
/* A stale msg1 the memory already refuses: byte-identical handler state. */
static void replay_refused(handshake_fixture *f, const hf_state *live, const fips_rekey_wire *stale)
{
    hf_send(f, f->client, stale->bytes, stale->len);
    hf_unchanged(&f->before, &f->after);
    incumbent_intact(live, &f->after);
    assert(f->after.restart_pending == f->before.restart_pending);
}
/* A restart msg1 that is held: msg2 answered, nothing of the incumbent moves. */
static void held(handshake_fixture *f, const hf_state *live, const fips_rekey_wire *msg1)
{
    hf_send(f, f->client, msg1->bytes, msg1->len);
    incumbent_intact(live, &f->after);
    assert(!memcmp(&f->after.keys, &f->before.keys, sizeof f->before.keys)); /* replay window included */
    assert(f->before.restart_pending == 0 && f->after.restart_pending == 1);
    assert(f->after.indices == f->before.indices + 1 && f->after.bindings == f->before.bindings);
    assert(f->after.sent == f->before.sent + 1 && f->after.next_link == f->before.next_link + 1);
}
/* Legitimate restart, first half: hold, identical msg2 for exact retransmits
 * from the same and from another endpoint, msg2 processed by the wire peer. */
static void restart_answered(handshake_fixture *f, const hf_state *live, uint8_t epoch)
{
    hf_start(f, epoch);
    fips_rekey_wire msg1 = f->msg1;
    held(f, live, &msg1);
    fipsd_transport *sources[] = {f->client, f->alternate};
    for (size_t i = 0; i < 2; ++i) {
        hf_send(f, sources[i], msg1.bytes, msg1.len); /* exact retransmit */
        incumbent_intact(live, &f->after);
        assert(f->after.restart_pending == 1 && f->after.indices == f->before.indices);
        assert(f->after.sent == f->before.sent + 1 && f->after.next_link == f->before.next_link);
    }
    fipsd_transport_event *first = hf_wire(f, 2);
    for (size_t i = 0; i < 2; ++i) {
        fipsd_transport_event *again = hf_wire(f, 2);
        assert(first->size == again->size && !memcmp(first->bytes, again->bytes, first->size));
        free(again);
    }
    fips_rekey_read read = {0};
    assert(fips_fmp_rekey_profile.reply(&f->hs, &f->local, first->bytes, first->size, &read));
    fips_rekey_completed_clear(&f->live); f->live = read.done;
    sodium_memzero(&read, sizeof read); free(first);
}
/* Second half: promotion happened exactly once, the old index was released and
 * every authenticated frame was delivered to the new link. */
static void confirmed(handshake_fixture *f, hf_state *live, uint64_t delivered)
{
    hf_state now; hf_snapshot(f, &now);
    assert(now.peers == 1 && now.promoted == live->promoted + 1 && now.restarts == live->restarts + 1);
    assert(now.restart_pending == 0 && now.indices == live->indices && now.bindings == live->bindings);
    assert(!same_recv_key(live, &now) && now.frames_in == delivered && now.legs == 0);
    assert(now.info.link.value != live->info.link.value);
    *live = now;
}
static void restart_confirmed(handshake_fixture *f, hf_state *live, uint8_t epoch)
{
    restart_answered(f, live, epoch);
    fips_transport_addr source = live->info.addr;
    hf_heartbeat(f); /* the first authenticated frame is the eviction gate */
    confirmed(f, live, 1);
    assert(fips_transport_addr_equal(&live->info.addr, &source));
}
/* Two frames in flight at the candidate index. The dispatcher's own state when
 * the second frame arrived fixes how many OPEN completions follow: a candidate
 * still held means both frames were opened under the candidate keys, whichever
 * result lands first confirms and the sibling overtaken by the promotion
 * re-enters the ordinary path (a third OPEN); a link already promoted means the
 * second frame was looked up as the new link's index directly. Both orderings
 * must deliver both frames and promote exactly once. Wait for exactly that many
 * completions: stopping once frames_in reads 2 could leave the trailing
 * completion's token behind to satisfy a later barrier before its own frame's
 * result, which is what made the following traffic() check flaky. */
static void restart_confirmed_two_in_flight(handshake_fixture *f, hf_state *live, uint8_t epoch)
{
    restart_answered(f, live, epoch);
    uint8_t a[128], b[128];
    size_t alen = seal_heartbeat(f, f->live.theirs.value, a, sizeof a);
    size_t blen = seal_heartbeat(f, f->live.theirs.value, b, sizeof b);
    hf_send(f, f->client, a, alen); hf_send(f, f->client, b, blen);
    int completions = f->after.restart_pending ? 3 : 2; /* the hold was still pending when b arrived */
    for (int i = 0; i < completions; ++i) hf_wait(f->completed);
    confirmed(f, live, 2);
}
/* The node's own dial to the wire peer, left unanswered. Its handshake
 * timeout is the deployed one, so the fixture's 1 s window cannot end the leg
 * while the checks below run (hs_dial copies the timing into the leg). */
static void dial_remote(handshake_fixture *f, void *arg)
{
    (void)arg; handlers *h = f->h;
    uint64_t timeout = h->cfg.timing.handshake_timeout_ms;
    h->cfg.timing.handshake_timeout_ms = fipsd_peer_timing_default().handshake_timeout_ms;
    assert(hs_dial(h, &f->remote, (fips_transport_id){1}, fipsd_loopback_transport_address(f->client), false));
    h->cfg.timing.handshake_timeout_ms = timeout;
}
int main(void)
{
    handshake_fixture f; hf_init(&f);
    hf_state live;
    fips_rekey_wire stale_a, stale_b;

    /* A crossing (nostrc-8rtmi): the node's own dial to the peer is pending
     * when the peer's msg1 promotes the link inbound. That msg1 is IK-authentic
     * but replayable, so neither the promotion nor a replay of it ends the leg,
     * and nor does the link's first AEAD frame (only the leg's own msg2 settles
     * the tie-break, as upstream). A forged Disconnect ends nothing; the peer's
     * authenticated Disconnect ends the link and the leg with it. */
    hf_call(&f, dial_remote, NULL);
    free(hf_wire(&f, 1)); /* the node's msg1, never answered */
    hf_start(&f, 0xa1);
    fips_rekey_wire crossing = f.msg1;
    hf_send(&f, f.client, crossing.bytes, crossing.len);
    assert(f.before.legs == 1 && f.after.peers == 1 && f.after.legs == 1);
    hf_send(&f, f.client, crossing.bytes, crossing.len); /* replayed: answered from the held msg2 */
    assert(f.after.peers == 1 && f.after.legs == 1 && f.after.promoted == f.before.promoted);
    hf_finish(&f); free(hf_wire(&f, 2));
    hf_heartbeat(&f);
    hf_state crossed; hf_snapshot(&f, &crossed);
    assert(crossed.peers == 1 && crossed.frames_in == 1 && crossed.legs == 1);
    uint8_t bye[8], wire[128];
    fips_writer bw; fips_writer_init(&bw, bye, sizeof bye);
    assert(fips_fmp_disconnect_encode(&(fips_fmp_disconnect){FIPS_FMP_Shutdown}, &bw, NULL) == FIPS_OK);
    size_t wlen = seal_link(&f, f.live.theirs.value, bye, fips_writer_len(&bw), wire, sizeof wire);
    wire[wlen - 1] ^= 1; /* the tag no longer authenticates */
    hf_open(&f, f.client, wire, wlen);
    hf_state ended; hf_snapshot(&f, &ended);
    assert(ended.peers == 1 && ended.legs == 1 && ended.frames_in == crossed.frames_in);
    wlen = seal_link(&f, f.live.theirs.value, bye, fips_writer_len(&bw), wire, sizeof wire);
    hf_open(&f, f.client, wire, wlen);
    hf_snapshot(&f, &ended);
    assert(ended.peers == 0 && ended.legs == 0 && ended.indices == 0);
    puts("crossing: a pending dial survives an inbound promotion, its msg1 replayed, an AEAD frame and a forged Disconnect; "
         "the authenticated Disconnect ends link and leg");

    /* Epoch A established and carrying traffic. */
    hf_start(&f, 0xa1); stale_a = f.msg1;
    hf_send(&f, f.client, f.msg1.bytes, f.msg1.len); hf_finish(&f); hf_heartbeat(&f);
    hf_snapshot(&f, &live);
    assert(live.peers == 1 && live.promoted == crossed.promoted + 1 && live.frames_in == 1 && live.restart_pending == 0);

    /* Restart into B: eviction waits for B's first authenticated frame. */
    restart_confirmed(&f, &live, 0xb2);
    puts("H1 restart A->B: incumbent untouched until a frame authenticates under msg2 keys; no duplicate promotion");

    /* The captured epoch-A msg1 replayed against B. */
    replay_refused(&f, &live, &stale_a);
    traffic(&f, &live);
    puts("H1 replayed epoch-A msg1: B's peer/index/binding/endpoint/key/replay/send state byte-identical; traffic continues");

    /* Restart into C with two frames racing at the candidate index, then
     * alternate the two retired recordings. */
    hf_start(&f, 0xb2); stale_b = f.msg1; /* a second recording of epoch B, never answered */
    restart_confirmed_two_in_flight(&f, &live, 0xc3);
    for (int i = 0; i < 3; ++i) { replay_refused(&f, &live, &stale_a); replay_refused(&f, &live, &stale_b); }
    traffic(&f, &live);
    puts("H1 alternating A/B replays against C: refused without a DH, index or send");

    /* Saturate the memory with real expired candidates: 64 distinct stale msg1
     * each held once (msg2 answered, never confirmed) and released by deadline. */
    for (unsigned i = 0; i < FIPSD_RESTART_HISTORY; ++i) {
        hf_start(&f, (uint8_t)(0x10 + (i % 0x70))); /* fresh ephemeral and a non-live epoch each time */
        held(&f, &live, &f.msg1);
        free(hf_wire(&f, 2));
        hf_call(&f, expire, NULL);
        hf_state s; hf_snapshot(&f, &s);
        assert(s.restart_pending == 0 && s.indices == live.indices);
        incumbent_intact(&live, &s);
    }
    traffic(&f, &live);
    /* Forgotten by memory, A is held again but can never confirm. */
    size_t bound = live.indices; /* the live link's own index count, without a candidate */
    held(&f, &live, &stale_a);
    free(hf_wire(&f, 2));
    traffic(&f, &live);
    replay_refused(&f, &live, &stale_b); /* one candidate per peer while held */
    uint32_t index = 0; hf_call(&f, candidate_index, &index);
    frame_at(&f, f.client, index); /* forged frame at the candidate index: wrong keys */
    hf_state s; hf_snapshot(&f, &s);
    assert(s.restart_pending == 1 && s.promoted == live.promoted && s.restarts == live.restarts);
    incumbent_intact(&live, &s);
    traffic(&f, &live);
    hf_call(&f, expire, NULL); hf_snapshot(&f, &s);
    assert(s.restart_pending == 0 && s.indices == bound);
    incumbent_intact(&live, &s);
    live = s;
    replay_refused(&f, &live, &stale_a); /* expired candidate remembered */
    puts("H1 after history saturation: A held once, forged frame refused, second msg1 waits, hold expires, A refused again; C intact throughout");

    /* The tombstone is a bounded cooldown: afterwards A is challenged afresh,
     * still never promoted, and remembered again when that hold lapses. */
    hf_call(&f, cooldown_lapsed, NULL);
    held(&f, &live, &stale_a);
    free(hf_wire(&f, 2));
    hf_call(&f, expire, NULL); hf_snapshot(&f, &s);
    assert(s.restart_pending == 0 && s.indices == bound);
    incumbent_intact(&live, &s);
    live = s;
    replay_refused(&f, &live, &stale_a);
    puts("H1 expired-candidate memory is a cooldown: re-challenged once it lapses, never promoted");

    /* Legitimate restart still promotes after all of the above, here with the
     * confirming frame arriving from another endpoint: authenticated first, the
     * address then judged by the ordinary link policy rather than refused. */
    restart_answered(&f, &live, 0xd4);
    frame_at(&f, f.alternate, f.live.theirs.value);
    confirmed(&f, &live, 1);
    assert(fips_transport_addr_equal(&live.info.addr, fipsd_loopback_transport_address(f.client)) ||
        fips_transport_addr_equal(&live.info.addr, fipsd_loopback_transport_address(f.alternate)));
    hf_heartbeat(&f); hf_snapshot(&f, &live); /* settle the endpoint under the ordinary policy */
    assert(live.peers == 1 && live.frames_in == 2);
    puts("H1 legitimate restart C->D promotes on a confirmation from another endpoint");

    /* A1 x H1: allow at msg1, deny before the confirming frame. */
    const char *tmp = fipsd_test_tmpdir(NULL);
    char path[4096]; snprintf(path, sizeof path, "%s/restart.deny", tmp);
    hf_policy(&f, false, path);
    fips_rekey_completed keep = f.live; /* D's keys */
    hf_start(&f, 0xe5);
    held(&f, &live, &f.msg1);
    hf_policy(&f, true, path);
    hf_finish(&f); hf_heartbeat(&f); /* E's confirmation under a now-denied identity */
    hf_snapshot(&f, &s);
    assert(s.restart_pending == 0 && s.indices == live.indices);
    incumbent_intact(&live, &s);
    hf_policy(&f, false, path);
    f.live = keep;
    traffic(&f, &live);
    puts("H1 deny reload between msg1 and confirmation: candidate dropped, D intact, traffic continues");

    hf_destroy(&f);
    return 0;
}
