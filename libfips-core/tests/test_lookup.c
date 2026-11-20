#include <fips/proto/lookup/mod.h>
#include "fakes/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned tests;
#define RUN(f) do { f(); tests++; puts("PASS " #f); } while (0)
static fips_node_addr addr(uint32_t n) {
    fips_node_addr a = {{0}};
    a.bytes[0] = (uint8_t)(n >> 24); a.bytes[1] = (uint8_t)(n >> 16);
    a.bytes[2] = (uint8_t)(n >> 8); a.bytes[3] = (uint8_t)n;
    return a;
}
static bool same(fips_node_addr a, fips_node_addr b) { return !memcmp(a.bytes, b.bytes, 16); }
static fips_lookup l;
static fips_lookup_request *req;
static fips_lookup_response *resp;
static fips_lookup_action items[FIPS_LOOKUP_MAX_FORWARD_PEERS];
static uint8_t bytes[4096], scratch[4096];
static fips_lookup_action_list actions;
static fips_lookup_poll_entry retries[4096], timeouts[4096];
static fips_lookup_poll_outcome poll_out;
static const uint64_t ladder[] = {1,2,4,8};
static void reset(size_t recent_cap, size_t pending_cap, uint64_t base, uint64_t cap) {
    free(l.recent_requests); free(l.pending_lookups); free(l.backoff.entries); free(l.forward_limiter.entries);
    fips_lookup_backoff b;
    fips_lookup_forward_rate_limiter f;
    fips_lookup_backoff_entry *be = calloc(FIPS_MAX_BACKOFF_ENTRIES, sizeof(*be));
    fips_lookup_forward_entry *fe = calloc(FIPS_MAX_FORWARD_ENTRIES, sizeof(*fe));
    fips_recent_request *re = calloc(recent_cap, sizeof(*re));
    fips_pending_lookup *pe = calloc(pending_cap, sizeof(*pe));
    CHECK(be && fe && re && pe);
    CHECK(fips_lookup_backoff_init(&b, be, FIPS_MAX_BACKOFF_ENTRIES, base, cap) == FIPS_OK);
    CHECK(fips_lookup_forward_rate_limiter_init(&f, fe, FIPS_MAX_FORWARD_ENTRIES, 2000) == FIPS_OK);
    CHECK(fips_lookup_init(&l, re, recent_cap, pe, pending_cap, b, f) == FIPS_OK);
    actions = (fips_lookup_action_list){.items = items, .capacity = FIPS_LOOKUP_MAX_FORWARD_PEERS,
        .bytes = bytes, .bytes_capacity = sizeof(bytes)};
    poll_out = (fips_lookup_poll_outcome){.retries = retries, .timeouts = timeouts,
        .retries_capacity = 4096, .timeouts_capacity = 4096};
}
static void request(uint64_t id, fips_node_addr target, uint8_t ttl) {
    req->request_id = id; req->target = target; req->origin = addr(0xbb); req->ttl = ttl; req->min_mtu = 0;
    fips_tree_coordinate_root(&req->origin_coords, req->origin);
}
static fips_initiate_decision gate(fips_node_addr target, uint64_t now, bool reachable) {
    fips_initiate_decision out;
    uint32_t failures;
    CHECK(fips_lookup_initiate_gate(&l, &target, now, reachable, &out, &failures) == FIPS_OK);
    if (out == FIPS_INITIATE_Suppressed) CHECK(failures == fips_lookup_backoff_failure_count(&l.backoff, &target));
    return out;
}
static fips_lookup_classification deliver(uint64_t id, fips_node_addr target, fips_node_addr from,
    fips_node_addr me, uint64_t now, uint64_t expiry, size_t cap, size_t peers) {
    request(id, target, 5);
    fips_lookup_classification c;
    CHECK(fips_lookup_classify_request(&l, req, &from, &me, now, expiry, cap, peers, &c) == FIPS_OK);
    return c;
}
typedef struct mock_peer { fips_node_addr addr; bool tree, reaches; } mock_peer;
typedef struct mock_view { mock_peer peers[300]; size_t len; } mock_view;
static fips_node_addr peer_at(void *ctx, size_t i) { return ((mock_view *)ctx)->peers[i].addr; }
static bool is_tree(void *ctx, const fips_node_addr *a) {
    mock_view *v = ctx;
    for (size_t i = 0; i < v->len; i++) if (same(v->peers[i].addr, *a)) return v->peers[i].tree;
    return false;
}
static bool reaches(void *ctx, const fips_node_addr *a, const fips_node_addr *target) {
    (void)target;
    mock_view *v = ctx;
    for (size_t i = 0; i < v->len; i++) if (same(v->peers[i].addr, *a)) return v->peers[i].reaches;
    return false;
}
static fips_lookup_routing_view view(mock_view *v) { return (fips_lookup_routing_view){v, v->len, peer_at, is_tree, reaches}; }
static fips_forward_result transit(uint64_t id, fips_node_addr target, fips_node_addr from, uint64_t now, mock_view *v) {
    CHECK(deliver(id, target, from, addr(0x99), now, 60000, l.recent_capacity, 1).outcome == FIPS_REQUEST_Forward);
    fips_forward_result out;
    fips_lookup_routing_view rv = view(v);
    CHECK(fips_lookup_plan_transit_forward(&l, req, &from, &rv, now, &actions, &out) == FIPS_OK);
    return out;
}
static fips_response_route route(uint64_t id, fips_node_addr target, fips_node_addr from, uint8_t n) {
    uint8_t digest[8]; memset(digest, n, 8);
    return fips_lookup_classify_response(&l, id, &target, &from, digest);
}
static bool forward_holds(fips_node_addr from, fips_node_addr target) {
    for (size_t i = 0; i < l.forward_limiter.len; i++)
        if (same(l.forward_limiter.entries[i].from, from) && same(l.forward_limiter.entries[i].target, target)) return true;
    return false;
}
static size_t forward_live(fips_node_addr from) {
    size_t n = 0;
    for (size_t i = 0; i < l.forward_limiter.len; i++) if (same(l.forward_limiter.entries[i].from, from)) n++;
    return n;
}
static fips_secp *secp;
static fips_identity *identity;
static fips_peer_identity peer;
static fips_fixed_entropy fixed = {0x5a};
static void crypto_init(void) {
    fips_entropy e = fips_fixed_entropy_adapter(&fixed);
    uint8_t secret[32] = {0}; secret[31] = 1;
    CHECK(fips_identity_init_secp(e, &secp) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_from_secret_bytes(secp, e, secret, &identity) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey key = fips_identity_pubkey(identity);
    CHECK(fips_peer_identity_from_pubkey(secp, &key, &peer) == FIPS_IDENTITY_OK);
}
static void response(uint64_t id) {
    resp->request_id = id; resp->target = peer.node_addr;
    fips_node_addr coords[3] = {peer.node_addr, addr(1), addr(0)};
    CHECK(fips_tree_coordinate_from_addrs(&resp->target_coords, coords, 3, NULL) == FIPS_OK);
    fips_sign_entry entries[4];
    fips_lookup_sign_limiter limiter;
    fips_lookup_sign_init(&limiter, entries, 4, 256, 32);
    bool signed_out = false;
    fips_node_addr from = addr(10);
    CHECK(fips_lookup_response_sign(resp, identity, &limiter, &from, fips_test_instant(0), scratch, sizeof(scratch), &signed_out, NULL) == FIPS_OK && signed_out);
}
static void test_lookup_request_forward(void) { request(123, addr(1), 5); CHECK(fips_lookup_request_can_forward(req)); CHECK(fips_lookup_request_forward(req)); CHECK(req->ttl == 4); }
static void test_lookup_request_ttl_exhausted(void) { request(123, addr(1), 1); CHECK(fips_lookup_request_forward(req)); CHECK(!fips_lookup_request_can_forward(req)); CHECK(!fips_lookup_request_forward(req)); }
static void test_lookup_request_generate(void) {
    fips_seeded_entropy seeded = {3}; fips_entropy e = fips_seeded_entropy_adapter(&seeded);
    uint8_t a[8], b[8]; e.fill(e.ctx, a, 8); e.fill(e.ctx, b, 8); CHECK(memcmp(a,b,8));
}
static void test_lookup_response_proof_bytes(void) {
    response(12345);
    fips_writer w; fips_writer_init(&w, bytes, sizeof(bytes));
    CHECK(fips_lookup_response_proof_bytes(resp->request_id, &resp->target, &resp->target_coords, &w, NULL) == FIPS_OK);
    CHECK(w.pos == 74 && bytes[0] == 0x39 && bytes[1] == 0x30 && bytes[24] == 3 && !bytes[25]);
    CHECK(!memcmp(bytes+8, resp->target.bytes, 16));
}
static void test_lookup_request_encode_decode_roundtrip(void) {
    request(12345, addr(10), 8); req->min_mtu = 1386;
    fips_node_addr coords[2] = {addr(20),addr(0)};
    CHECK(fips_tree_coordinate_from_addrs(&req->origin_coords, coords, 2, NULL) == FIPS_OK);
    CHECK(fips_lookup_request_forward(req));
    fips_writer w; fips_writer_init(&w, bytes, sizeof(bytes));
    CHECK(fips_lookup_request_encode(req, &w, NULL) == FIPS_OK && bytes[0] == 0x30);
    fips_reader r; fips_reader_init(&r, bytes+1, w.pos-1);
    CHECK(fips_lookup_request_decode(&r, req, NULL) == FIPS_OK && r.pos == w.pos-1);
    CHECK(req->request_id == 12345 && same(req->target,addr(10)) && req->ttl == 7 && req->min_mtu == 1386);
    CHECK(req->origin_coords.len == 2 && same(req->origin_coords.entries[1].node_addr,addr(0)));
}
static void test_lookup_request_decode_too_short(void) {
    fips_reader r; fips_proto_error e;
    for (size_t n = 0; n <= 42; n += 42) {
        fips_reader_init(&r, bytes, n);
        CHECK(fips_lookup_request_decode(&r, req, &e) == FIPS_ERR_RANGE && !r.pos);
        CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.expected == 45 && e.as.message_too_short.got == n);
    }
}
static void test_lookup_request_min_mtu_boundary_values(void) {
    const uint16_t mtus[] = {0,1386,UINT16_MAX};
    for (size_t i=0;i<3;i++) {
        request(100,addr(10),5); req->min_mtu = mtus[i];
        fips_writer w; fips_writer_init(&w,bytes,sizeof(bytes));
        CHECK(fips_lookup_request_encode(req,&w,NULL)==FIPS_OK);
        fips_reader r; fips_reader_init(&r,bytes+1,w.pos-1);
        CHECK(fips_lookup_request_decode(&r,req,NULL)==FIPS_OK && req->min_mtu==mtus[i]);
    }
}
static void test_lookup_response_encode_decode_roundtrip(void) {
    response(999); CHECK(resp->path_mtu==UINT16_MAX);
    uint8_t proof[64]; memcpy(proof,resp->proof,64);
    fips_writer w; fips_writer_init(&w,bytes,sizeof(bytes));
    CHECK(fips_lookup_response_encode(resp,&w,NULL)==FIPS_OK && bytes[0]==0x31);
    fips_reader r; fips_reader_init(&r,bytes+1,w.pos-1);
    CHECK(fips_lookup_response_decode(&r,resp,NULL)==FIPS_OK && r.pos==w.pos-1);
    CHECK(resp->request_id==999 && same(resp->target,peer.node_addr) && resp->path_mtu==UINT16_MAX && !memcmp(proof,resp->proof,64));
}
static void test_lookup_response_path_mtu_roundtrip(void) {
    response(999); const uint16_t mtus[]={0,1280,1386,9000,UINT16_MAX};
    for(size_t i=0;i<5;i++) {
        resp->path_mtu=mtus[i]; fips_writer w; fips_writer_init(&w,bytes,sizeof(bytes));
        CHECK(fips_lookup_response_encode(resp,&w,NULL)==FIPS_OK);
        fips_reader r; fips_reader_init(&r,bytes+1,w.pos-1);
        CHECK(fips_lookup_response_decode(&r,resp,NULL)==FIPS_OK && resp->path_mtu==mtus[i]);
        CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),NULL)==FIPS_OK);
    }
}
static void test_lookup_response_path_mtu_not_in_proof_bytes(void) {
    test_lookup_response_proof_bytes();
    CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),NULL)==FIPS_OK);
    resp->path_mtu=1;
    CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),NULL)==FIPS_OK);
}
static void test_lookup_response_decode_too_short(void) {
    fips_reader r; fips_proto_error e;
    for(size_t n=0;n<=50;n+=50) {
        fips_reader_init(&r,bytes,n);
        CHECK(fips_lookup_response_decode(&r,resp,&e)==FIPS_ERR_RANGE && !r.pos);
        CHECK(e.variant==FIPS_PROTO_MessageTooShort && e.as.message_too_short.expected==92 && e.as.message_too_short.got==n);
    }
}
/* Each table row ports the named upstream selection/sender test. */
static void planners(void) {
    typedef struct row { const char *name; mock_view v; uint32_t from; uint8_t ttl; bool initiate; fips_forward_outcome outcome; bool fallback; size_t n; uint32_t peers[3]; } row;
    const row rows[]={
        {"picks_only_tree_peers_when_a_tree_peer_matches", {{{addr(1),true,true},{addr(2),false,true}},2}, 0xcc,3,false,FIPS_FORWARD_Forward,false,1,{1}},
        {"falls_back_to_non_tree_peers_when_no_tree_peer_matches", {{{addr(5),true,false},{addr(3),false,true},{addr(4),false,true}},3}, 0xcc,3,false,FIPS_FORWARD_Forward,true,2,{3,4}},
        {"returns_no_peers_when_nothing_reaches_target", {{{addr(6),true,false},{addr(7),false,false}},2}, 0xcc,3,false,FIPS_FORWARD_NoPeers,false,0,{0}},
        {"returns_ttl_exhausted_when_ttl_is_zero", {{{addr(8),true,true}},1}, 0xcc,0,false,FIPS_FORWARD_TtlExhausted,false,0,{0}},
        {"initiate_picks_only_tree_peers_and_never_falls_back", {{{addr(1),true,true},{addr(2),false,true}},2}, 0,3,true,FIPS_FORWARD_Forward,false,1,{1}},
        {"initiate_returns_empty_when_only_non_tree_peers_reach", {{{addr(5),true,false},{addr(3),false,true},{addr(4),false,true}},3}, 0,3,true,FIPS_FORWARD_NoPeers,false,0,{0}},
        {"initiate_returns_empty_when_nothing_reaches_target", {{{addr(6),true,false},{addr(7),false,false}},2}, 0,3,true,FIPS_FORWARD_NoPeers,false,0,{0}},
        {"forward_never_sends_the_request_back_to_the_tree_peer_it_came_from", {{{addr(1),true,true},{addr(2),true,true},{addr(3),false,true}},3}, 1,3,false,FIPS_FORWARD_Forward,false,1,{2}},
        {"forward_returns_no_peers_when_the_sender_is_the_only_matching_tree_peer_even_with_non_tree_matches", {{{addr(1),true,true},{addr(2),false,true},{addr(3),true,false}},3}, 1,3,false,FIPS_FORWARD_NoPeers,false,0,{0}},
        {"fallback_never_sends_the_request_back_to_the_non_tree_peer_it_came_from", {{{addr(1),false,true},{addr(2),false,true},{addr(3),true,false}},3}, 1,3,false,FIPS_FORWARD_Forward,true,1,{2}},
        {"fallback_returns_no_peers_when_the_non_tree_sender_is_the_only_match", {{{addr(1),false,true}},1}, 1,3,false,FIPS_FORWARD_NoPeers,false,0,{0}},
        {"forward_from_the_parent_still_reaches_the_child_whose_filter_matches", {{{addr(1),true,true},{addr(2),true,true}},2}, 1,3,false,FIPS_FORWARD_Forward,false,1,{2}},
        {"forward_from_a_non_tree_sender_still_uses_every_matching_tree_peer", {{{addr(1),true,true},{addr(2),true,true},{addr(3),false,true}},3}, 3,3,false,FIPS_FORWARD_Forward,false,2,{1,2}},
        {"forward_from_a_tree_peer_still_falls_back_when_no_tree_peer_matches", {{{addr(1),true,false},{addr(2),false,true}},2}, 1,3,false,FIPS_FORWARD_Forward,true,1,{2}},
        {"fallback_from_a_non_matching_non_tree_sender_still_reaches_the_other_match", {{{addr(1),false,false},{addr(2),false,true}},2}, 1,3,false,FIPS_FORWARD_Forward,true,1,{2}}
    };
    for(size_t i=0;i<sizeof(rows)/sizeof(*rows);i++) {
        mock_view v=rows[i].v; fips_lookup_routing_view rv=view(&v); fips_node_addr from=addr(rows[i].from);
        request(1,addr(0xaa),rows[i].ttl);
        fips_forward_result out;
        if(rows[i].initiate) CHECK(fips_lookup_plan_initiate(req,&rv,&actions)==FIPS_OK);
        else { CHECK(fips_lookup_plan_forward(req,&from,&rv,&actions,&out)==FIPS_OK); CHECK(out.outcome==rows[i].outcome); if(rows[i].n) CHECK(out.used_fallback==rows[i].fallback); }
        CHECK(actions.len==rows[i].n);
        for(size_t j=0;j<actions.len;j++) {
            CHECK(items[j].kind==FIPS_LOOKUP_SendLink && same(items[j].peer,addr(rows[i].peers[j])));
            CHECK(items[j].bytes==bytes && bytes[0]==0x30 && bytes[41]==(rows[i].initiate ? rows[i].ttl : rows[i].ttl-1));
        }
        tests++; printf("PASS %s\n",rows[i].name);
    }
}
static void forwarded(uint64_t id) {
    reset(128,16,0,0);
    mock_view v={{{addr(1),true,true},{addr(0xe0),true,true}},2};
    CHECK(transit(id,addr(0xaa),addr(1),1000,&v).outcome==FIPS_FORWARD_Forward);
    CHECK(actions.len==1 && same(items[0].peer,addr(0xe0)));
}
static void classify_response_transit_on_fresh_forwarded_request(void) {
    forwarded(42); fips_response_route r=route(42,addr(0xaa),addr(0xe0),0x5a);
    CHECK(r.kind==FIPS_RESPONSE_Transit && same(r.from_peer,addr(1)) && fips_lookup_recent(&l,42)->relayed_len==1);
}
static void classify_response_already_forwarded_on_second_call(void) {
    forwarded(7); CHECK(route(7,addr(0xaa),addr(0xe0),0x5a).kind==FIPS_RESPONSE_Transit);
    CHECK(route(7,addr(0xaa),addr(0xe0),0x5a).kind==FIPS_RESPONSE_AlreadyForwarded);
}
static void classify_response_originator_wins_over_a_transit_dedup_record(void) {
    forwarded(4242); CHECK(gate(addr(0xaa),1000,true)==FIPS_INITIATE_Proceed);
    fips_pending_lookup_record(fips_lookup_pending(&l,&req->target),4242);
    CHECK(route(4242,addr(0xaa),addr(0xe0),0x5a).kind==FIPS_RESPONSE_Originator);
    CHECK(fips_lookup_recent(&l,4242)->relayed_len==0);
}
static void classify_response_originator_when_a_pending_lookup_issued_the_id(void) {
    reset(128,16,0,0); fips_node_addr target=addr(0x51);
    CHECK(gate(target,1000,true)==FIPS_INITIATE_Proceed);
    fips_pending_lookup_record(fips_lookup_pending(&l,&target),999);
    CHECK(route(999,target,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Originator);
}
static void classify_response_unsolicited_when_nothing_correlates_the_id(void) {
    reset(128,16,0,0); fips_node_addr target=addr(0x52);
    CHECK(route(999,target,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Unsolicited);
    CHECK(gate(target,1000,true)==FIPS_INITIATE_Proceed);
    fips_pending_lookup_record(fips_lookup_pending(&l,&target),1);
    CHECK(route(999,target,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Unsolicited);
    CHECK(route(1,addr(0x53),addr(0xe0),0x5a).kind==FIPS_RESPONSE_Unsolicited);
    CHECK(route(1,target,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Originator);
}
static void on_response_accepted_clears_state_and_emits_effects(void) {
    reset(128,16,30,300); fips_node_addr target=addr(0x5a);
    CHECK(gate(target,1000,true)==FIPS_INITIATE_Proceed);
    fips_lookup_backoff_record_failure(&l.backoff,&target,1000);
    CHECK(l.backoff.len==1);
    fips_tree_coordinate_root(&resp->target_coords,target);
    CHECK(fips_lookup_on_response_accepted(&l,7,&target,&resp->target_coords,12345,1400,&actions)==FIPS_OK);
    CHECK(!l.pending_len && !l.backoff.len && actions.len==4);
    for(size_t i=0;i<4;i++) { CHECK(items[i].kind==(fips_lookup_action_kind)(i+1)); CHECK(same(items[i].target,target)); }
    CHECK(items[0].now_ms==12345 && items[1].now_ms==12345 && items[0].path_mtu==1400 && items[1].path_mtu==1400);
    CHECK(items[0].coords==&resp->target_coords);
}
static void cache_coords_action_carries_the_request_id_the_response_arrived_with(void) {
    reset(128,16,0,0); fips_node_addr target=addr(0x5a);
    fips_tree_coordinate_root(&resp->target_coords,target);
    CHECK(fips_lookup_on_response_accepted(&l,UINT64_C(0xdeadbeef00000001),&target,&resp->target_coords,12345,1400,&actions)==FIPS_OK);
    CHECK(items[0].request_id==UINT64_C(0xdeadbeef00000001));
}
static void poll_at(uint64_t now, const uint64_t *steps, size_t n) { CHECK(fips_lookup_poll_pending(&l,now,steps,n,&poll_out)==FIPS_OK); }
static void start(fips_node_addr target, uint64_t now, uint64_t base, uint64_t cap) {
    reset(128,16,base,cap); CHECK(gate(target,now,true)==FIPS_INITIATE_Proceed);
}
static void poll_pending_no_action_before_first_deadline(void) {
    fips_node_addr target=addr(0x30); start(target,10000,0,0); poll_at(10999,ladder,4);
    CHECK(!poll_out.retries_len && !poll_out.timeouts_len && l.pending_lookups[0].attempt==1 && l.pending_lookups[0].last_sent_ms==10000);
}
static void poll_pending_retries_at_first_deadline(void) {
    start(addr(0x31),10000,0,0); poll_at(11000,ladder,4);
    CHECK(poll_out.retries_len==1 && !poll_out.timeouts_len && retries[0].attempt==2);
    CHECK(l.pending_lookups[0].last_sent_ms==11000 && l.pending_lookups[0].attempt==2);
}
static void poll_pending_final_timeout_at_max_attempt(void) {
    start(addr(0x32),50000,0,0); l.pending_lookups[0].attempt=4; poll_at(58000,ladder,4);
    CHECK(!poll_out.retries_len && poll_out.timeouts_len==1 && !timeouts[0].has_failures && !l.pending_len && !l.backoff.len);
}
static void poll_pending_final_timeout_at_max_attempt_records_a_failure_with_backoff_enabled(void) {
    start(addr(0x32),50000,30,300); l.pending_lookups[0].attempt=4; poll_at(58000,ladder,4);
    CHECK(poll_out.timeouts_len==1 && timeouts[0].has_failures && timeouts[0].failures==1 && !l.pending_len && l.backoff.len==1);
}
static void with_backoff_enabled_an_entry_is_forgotten_once_its_window_and_one_cap_interval_have_passed_on_the_lookup_tick(void) {
    reset(128,16,30,300); CHECK(gate(addr(0x34),1000,false)==FIPS_INITIATE_BloomMiss);
    poll_at(330999,ladder,4); CHECK(l.backoff.len==1);
    poll_at(331000,ladder,4); CHECK(!l.backoff.len);
}
static void a_bloom_miss_under_the_default_disabled_backoff_records_nothing_so_ten_thousand_distinct_misses_leave_the_failure_table_empty(void) {
    reset(128,16,0,0);
    for(uint32_t i=0;i<10000;i++) CHECK(gate(addr(i),1000+i,false)==FIPS_INITIATE_BloomMiss);
    CHECK(!l.pending_len && !l.backoff.len);
}
static void with_backoff_enabled_a_failed_target_is_suppressed_inside_its_window_allowed_after_it_and_escalates_on_the_next_failure(void) {
    reset(128,16,30,300); fips_node_addr t=addr(0x35);
    CHECK(gate(t,1000,false)==FIPS_INITIATE_BloomMiss && gate(t,30999,true)==FIPS_INITIATE_Suppressed);
    poll_at(31000,ladder,4); CHECK(fips_lookup_backoff_failure_count(&l.backoff,&t)==1);
    CHECK(gate(t,31000,false)==FIPS_INITIATE_BloomMiss && fips_lookup_backoff_failure_count(&l.backoff,&t)==2);
    CHECK(fips_lookup_backoff_is_suppressed(&l.backoff,&t,90999) && !fips_lookup_backoff_is_suppressed(&l.backoff,&t,91000));
}
static void classify_request_forwards_fresh_and_records_it(void) {
    reset(128,16,0,0);
    CHECK(deliver(1,addr(0xaa),addr(1),addr(0x99),1000,5000,4096,1).outcome==FIPS_REQUEST_Forward);
    CHECK(same(fips_lookup_recent(&l,1)->from_peer,addr(1)));
}
static void classify_request_duplicate_on_second_call(void) {
    classify_request_forwards_fresh_and_records_it();
    fips_lookup_classification c=deliver(1,addr(0xaa),addr(2),addr(0x99),1000,5000,4096,1);
    CHECK(c.outcome==FIPS_REQUEST_Duplicate && !c.has_eviction && l.recent_len==1 && fips_lookup_peer_entries(&l,&req->origin)==0);
}
static void classify_request_drops_our_own_request_looped_back_to_us(void) {
    fips_node_addr t=addr(0xaa), from=addr(1), me=addr(0x99);
    start(t,1000,0,0); fips_pending_lookup_record(fips_lookup_pending(&l,&t),77);
    request(77,t,3); req->origin=me;
    fips_lookup_classification c;
    CHECK(fips_lookup_classify_request(&l,req,&from,&me,1000,5000,4096,1,&c)==FIPS_OK);
    CHECK(c.outcome==FIPS_REQUEST_OwnRequestLooped && !c.has_eviction && !l.recent_len);
    CHECK(route(77,t,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Originator);
    CHECK(fips_lookup_on_response_accepted(&l,77,&t,&req->origin_coords,2000,1280,&actions)==FIPS_OK);
    CHECK(route(77,t,addr(0xe0),0x5a).kind==FIPS_RESPONSE_Unsolicited);
}
static void classify_request_records_our_own_id_as_transit_once_its_lookup_has_timed_out(void) {
    fips_node_addr t=addr(0xaa); start(t,1000,0,0); fips_pending_lookup_record(fips_lookup_pending(&l,&t),77);
    poll_at(2000,ladder,1); CHECK(poll_out.timeouts_len==1);
    CHECK(deliver(77,t,addr(1),addr(0x99),2000,5000,4096,1).outcome==FIPS_REQUEST_Forward);
    CHECK(deliver(77,t,addr(1),addr(0x99),2000,5000,4096,1).outcome==FIPS_REQUEST_Duplicate);
}
static void classify_request_records_an_own_id_older_than_the_recorded_window_as_transit(void) {
    fips_node_addr t=addr(0xaa); start(t,1000,0,0);
    for(uint64_t id=1;id<=9;id++) fips_pending_lookup_record(fips_lookup_pending(&l,&t),id);
    fips_pending_lookup_record(fips_lookup_pending(&l,&t),9); CHECK(l.pending_lookups[0].ids_len==8);
    CHECK(deliver(1,t,addr(1),addr(0x99),1000,5000,4096,1).outcome==FIPS_REQUEST_Forward);
    CHECK(deliver(9,t,addr(1),addr(0x99),1000,5000,4096,1).outcome==FIPS_REQUEST_OwnRequestLooped);
}
static void classify_request_still_transits_a_foreign_id_for_a_target_we_are_looking_up(void) {
    fips_node_addr t=addr(0xaa); start(t,1000,0,0); fips_pending_lookup_record(fips_lookup_pending(&l,&t),77);
    CHECK(deliver(88,t,addr(1),addr(0x99),1000,5000,4096,1).outcome==FIPS_REQUEST_Forward);
}
static void classify_request_evicts_rather_than_refusing_a_full_dedup_cache(void) {
    reset(3,16,0,0);
    for(uint64_t id=100;id<103;id++) CHECK(fips_lookup_record_recent(&l,id,addr(1),addr(0xf0),1000)==FIPS_OK);
    fips_lookup_classification c=deliver(1,addr(0xaa),addr(1),addr(0x99),1000,5000,3,1);
    CHECK(c.outcome==FIPS_REQUEST_Forward && c.has_eviction && c.evicted.request_id==100 && same(c.evicted.peer,addr(1)));
    CHECK(l.recent_len==3 && !fips_lookup_recent(&l,100) && fips_lookup_recent(&l,1));
}
static void classify_request_respond_as_target(void) {
    reset(128,16,0,0); CHECK(deliver(1,addr(0xaa),addr(1),addr(0xaa),1000,5000,4096,1).outcome==FIPS_REQUEST_RespondAsTarget); CHECK(l.recent_len==1);
}
static void classify_request_ttl_exhausted_for_non_target(void) {
    reset(128,16,0,0); request(1,addr(0xaa),0); fips_lookup_classification c; fips_node_addr from=addr(1),me=addr(0x99);
    CHECK(fips_lookup_classify_request(&l,req,&from,&me,1000,5000,4096,1,&c)==FIPS_OK && c.outcome==FIPS_REQUEST_TtlExhausted);
}
static void plan_transit_forward_rate_limited(void) {
    reset(128,16,0,0); fips_node_addr from=addr(1), t=addr(0xaa);
    CHECK(fips_lookup_forward_should_forward(&l.forward_limiter,&from,&t,1000));
    mock_view v={{{addr(2),true,true}},1};
    CHECK(transit(1,t,from,1000,&v).outcome==FIPS_FORWARD_RateLimited && !fips_lookup_recent(&l,1)->forwarded_len);
}
static void classify_request_purges_expired_entries(void) {
    reset(128,16,0,0); CHECK(fips_lookup_record_recent(&l,55,addr(1),addr(0xaa),1000)==FIPS_OK);
    CHECK(deliver(1,addr(0xaa),addr(1),addr(0x99),10000,5000,4096,1).outcome==FIPS_REQUEST_Forward);
    CHECK(!fips_lookup_recent(&l,55) && l.recent_len==1);
}
static void poll_pending_full_ladder_end_to_end(void) {
    start(addr(0x33),0,0,0); fips_test_clock clock={0};
    const uint64_t waits[]={1000,2000,4000,8000};
    const uint64_t deadlines[]={1000,3000,7000,15000};
    for(size_t i=0;i<4;i++) {
        CHECK(fips_test_clock_advance(&clock,waits[i]-1)==FIPS_OK); poll_at(clock.millis,ladder,4);
        CHECK(!poll_out.retries_len && !poll_out.timeouts_len);
        CHECK(fips_test_clock_advance(&clock,1)==FIPS_OK && clock.millis==deadlines[i]); poll_at(clock.millis,ladder,4);
        if(i<3) CHECK(poll_out.retries_len==1 && retries[0].attempt==i+2 && !poll_out.timeouts_len);
        else CHECK(!poll_out.retries_len && poll_out.timeouts_len==1 && !timeouts[0].has_failures && !l.pending_len && !l.backoff.len);
    }
}
static void initiate_gate_deduplicated_when_pending(void) { start(addr(0x40),500,0,0); CHECK(gate(addr(0x40),1000,true)==FIPS_INITIATE_Deduplicated); }
static void initiate_gate_suppressed_by_backoff(void) {
    reset(128,16,30,300); fips_node_addr t=addr(0x41); fips_lookup_backoff_record_failure(&l.backoff,&t,1000);
    CHECK(gate(t,1000,true)==FIPS_INITIATE_Suppressed && !l.pending_len);
}
static void initiate_gate_bloom_miss_records_failure(void) {
    fips_node_addr t=addr(0x42); reset(128,16,0,0); CHECK(gate(t,1000,false)==FIPS_INITIATE_BloomMiss && !l.backoff.len && !l.pending_len);
    reset(128,16,30,300); CHECK(gate(t,1000,false)==FIPS_INITIATE_BloomMiss && l.backoff.len==1 && !l.pending_len);
}
static void initiate_gate_proceed_inserts_pending(void) { start(addr(0x43),7777,0,0); CHECK(l.pending_lookups[0].last_sent_ms==7777 && l.pending_lookups[0].initiated_ms==7777 && l.pending_lookups[0].attempt==1); }
static void initiate_failed_drops_pending_and_records_failure(void) {
    fips_node_addr t=addr(0x44); start(t,1000,0,0); fips_lookup_initiate_failed(&l,&t,1000); CHECK(!l.pending_len && !l.backoff.len);
    start(t,1000,30,300); fips_lookup_initiate_failed(&l,&t,1000); CHECK(!l.pending_len && fips_lookup_backoff_failure_count(&l.backoff,&t)==1);
}
static void a_response_from_the_requests_upstream_peer_is_ineligible(void) {
    forwarded(7); fips_response_route r=route(7,addr(0xaa),addr(1),1);
    CHECK(r.kind==FIPS_RESPONSE_Ineligible && r.reason==FIPS_INELIGIBLE_NotSentTo && !fips_lookup_recent(&l,7)->relayed_len);
}
static void a_response_for_another_target_is_ineligible(void) {
    forwarded(7); fips_response_route r=route(7,addr(0xab),addr(0xe0),1);
    CHECK(r.kind==FIPS_RESPONSE_Ineligible && r.reason==FIPS_INELIGIBLE_OtherTarget);
    CHECK(route(7,addr(0xaa),addr(0xe0),1).kind==FIPS_RESPONSE_Transit);
}
static void a_response_from_a_peer_the_request_was_not_forwarded_to_is_ineligible(void) {
    forwarded(7); fips_response_route r=route(7,addr(0xaa),addr(0xf),1);
    CHECK(r.kind==FIPS_RESPONSE_Ineligible && r.reason==FIPS_INELIGIBLE_NotSentTo);
    CHECK(route(7,addr(0xaa),addr(0xe0),2).kind==FIPS_RESPONSE_Transit);
}
static void a_response_to_a_request_this_node_did_not_forward_is_ineligible(void) {
    reset(128,16,0,0); mock_view v={{{addr(0xe0),true,false}},1};
    CHECK(transit(7,addr(0xaa),addr(1),1000,&v).outcome==FIPS_FORWARD_NoPeers);
    fips_response_route r=route(7,addr(0xaa),addr(0xe0),1);
    CHECK(r.kind==FIPS_RESPONSE_Ineligible && r.reason==FIPS_INELIGIBLE_NotSentTo);
}
static void a_response_from_each_peer_the_request_was_forwarded_to_is_eligible(void) {
    reset(128,16,0,0); mock_view v={{{addr(1),true,true},{addr(2),true,true},{addr(3),true,true},{addr(4),true,true}},4};
    CHECK(transit(7,addr(0xaa),addr(1),1000,&v).outcome==FIPS_FORWARD_Forward && actions.len==3);
    for(uint8_t i=2;i<=4;i++) { fips_response_route r=route(7,addr(0xaa),addr(i),i); CHECK(r.kind==FIPS_RESPONSE_Transit && same(r.from_peer,addr(1))); }
}
static void an_identical_response_is_relayed_once(void) { classify_response_already_forwarded_on_second_call(); }
static void a_reverse_path_cycle_stops_after_one_circuit(void) {
    fips_lookup nodes[3]; memset(nodes,0,sizeof(nodes));
    for(uint32_t i=0;i<3;i++) {
        reset(128,16,0,0); mock_view v={{{addr((i+1)%3+10),true,true}},1};
        CHECK(transit(7,addr(0xaa),addr((i+2)%3+10),1000,&v).outcome==FIPS_FORWARD_Forward);
        nodes[i]=l; memset(&l,0,sizeof(l));
    }
    size_t at=0, relays=0; fips_node_addr sender=addr(11);
    for(size_t i=0;i<30;i++) {
        l=nodes[at]; fips_response_route r=route(7,addr(0xaa),sender,1); nodes[at]=l;
        if(r.kind==FIPS_RESPONSE_AlreadyForwarded) break;
        CHECK(r.kind==FIPS_RESPONSE_Transit); relays++; sender=addr((uint32_t)at+10);
        at=(at+2)%3; CHECK(same(r.from_peer,addr((uint32_t)at+10)));
    }
    CHECK(relays==3);
    memset(&l,0,sizeof(l));
    for(size_t i=0;i<3;i++) { free(nodes[i].recent_requests); free(nodes[i].pending_lookups); free(nodes[i].backoff.entries); free(nodes[i].forward_limiter.entries); }
}
static void the_duplicate_memory_holds_at_most_eight_digests(void) {
    forwarded(7);
    for(uint8_t n=0;n<20;n++) { CHECK(route(7,addr(0xaa),addr(0xe0),n).kind==FIPS_RESPONSE_Transit); CHECK(fips_lookup_recent(&l,7)->relayed_len<=8); }
    fips_recent_request *r=fips_lookup_recent(&l,7); CHECK(r->relayed_len==8);
    for(size_t i=0;i<8;i++) for(size_t j=0;j<8;j++) CHECK(r->relayed[i][j]==i+12);
    CHECK(route(7,addr(0xaa),addr(0xe0),0).kind==FIPS_RESPONSE_Transit);
}
static void a_ninth_distinct_response_is_still_relayed(void) {
    forwarded(7); for(uint8_t n=0;n<8;n++) CHECK(route(7,addr(0xaa),addr(0xe0),n).kind==FIPS_RESPONSE_Transit);
    CHECK(route(7,addr(0xaa),addr(0xe0),0xff).kind==FIPS_RESPONSE_Transit);
}
static void one_peer_refreshing_a_target_does_not_suppress_another_peers_request_for_it(void) {
    reset(128,16,0,0); mock_view v={{{addr(0x77),true,true}},1};
    CHECK(transit(1,addr(0xaa),addr(1),1000,&v).outcome==FIPS_FORWARD_Forward);
    CHECK(transit(2,addr(0xaa),addr(2),1500,&v).outcome==FIPS_FORWARD_Forward);
}
static void a_peer_repeating_a_target_within_the_interval_is_still_suppressed(void) {
    reset(128,16,0,0); mock_view v={{{addr(0x77),true,true}},1};
    CHECK(transit(1,addr(0xaa),addr(1),1000,&v).outcome==FIPS_FORWARD_Forward);
    CHECK(transit(2,addr(0xaa),addr(1),1500,&v).outcome==FIPS_FORWARD_RateLimited);
}
static void a_target_no_peer_reaches_takes_no_limiter_entry(void) {
    reset(128,16,0,0); mock_view v={{{addr(5),true,false}},1};
    CHECK(transit(1,addr(0xab),addr(1),1000,&v).outcome==FIPS_FORWARD_NoPeers && !l.forward_limiter.len);
    v.peers[0].reaches=true;
    CHECK(transit(2,addr(0xab),addr(1),1100,&v).outcome==FIPS_FORWARD_Forward && l.forward_limiter.len==1);
    CHECK(transit(3,addr(0xab),addr(1),1200,&v).outcome==FIPS_FORWARD_RateLimited);
}
static void a_full_limiter_still_records_the_newcomer(void) {
    reset(4096,16,0,0); mock_view v={{{addr(0x77),true,true}},1};
    for(uint32_t i=0;i<4096;i++) CHECK(transit(10000+i,addr(0x1000+i),addr(1),1000,&v).outcome==FIPS_FORWARD_Forward);
    CHECK(l.forward_limiter.len==4096);
    CHECK(transit(1,addr(0xaa),addr(2),1000,&v).outcome==FIPS_FORWARD_Forward);
    CHECK(transit(2,addr(0xaa),addr(2),1100,&v).outcome==FIPS_FORWARD_RateLimited);
}
static void poll_pending_retries_rather_than_times_out_under_a_ladder_of_256_entries(void) {
    uint64_t steps[256]; for(size_t i=0;i<256;i++) steps[i]=1;
    start(addr(0x33),10000,0,0); poll_at(11000,steps,256);
    CHECK(poll_out.retries_len==1 && retries[0].attempt==2 && !poll_out.timeouts_len);
}
static void poll_pending_neither_overflows_nor_times_out_on_a_huge_ladder_entry(void) {
    uint64_t huge=UINT64_MAX; start(addr(0x34),10000,0,0); poll_at(3610000,&huge,1);
    CHECK(!poll_out.retries_len && !poll_out.timeouts_len && l.pending_len==1);
}
static void a_peer_over_its_share_evicts_its_own_oldest_and_not_a_light_peers(void) {
    reset(128,16,0,0); fips_node_addr me=addr(0x99),t=addr(0xbb),heavy=addr(1),light=addr(2);
    deliver(7,t,light,me,1000,60000,128,64);
    for(uint64_t i=0;i<=64;i++) deliver(1000+i,t,heavy,me,1000,60000,128,64);
    CHECK(fips_lookup_recent(&l,7) && !fips_lookup_recent(&l,1000) && fips_lookup_recent(&l,1064));
    CHECK(fips_lookup_peer_entries(&l,&heavy)==64 && fips_lookup_peer_entries(&l,&light)==1);
}
static void the_per_peer_share_never_falls_below_the_floor_however_many_peers(void) {
    CHECK(fips_lookup_peer_share(128,128)==64 && fips_lookup_peer_share(128,SIZE_MAX)==64);
    CHECK(fips_lookup_peer_share(4096,8)==512 && fips_lookup_peer_share(128,0)==128);
    reset(128,16,0,0);
    for(uint64_t i=0;i<64;i++) CHECK(!deliver(i,addr(0xbb),addr(1),addr(0x99),1000,60000,128,SIZE_MAX).has_eviction);
    fips_lookup_classification c=deliver(999,addr(0xbb),addr(1),addr(0x99),1000,60000,128,SIZE_MAX);
    CHECK(c.has_eviction && c.evicted.request_id==0 && c.evicted.share==64 && same(c.evicted.peer,addr(1)));
}
static void a_node_whose_dedup_cache_is_saturated_still_answers_a_lookup_for_itself(void) {
    reset(128,16,0,0);
    for(uint64_t i=0;i<128;i++) deliver(i,addr(0xbb),addr(1),addr(0x99),1000,60000,128,1);
    fips_lookup_classification c=deliver(UINT64_MAX,addr(0x99),addr(2),addr(0x99),1000,60000,128,1);
    CHECK(c.outcome==FIPS_REQUEST_RespondAsTarget && c.has_eviction && c.evicted.request_id==0 && same(c.evicted.peer,addr(1)));
    CHECK(l.recent_len==128 && fips_lookup_recent(&l,UINT64_MAX));
}
static void a_duplicate_is_not_indexed_twice_and_evicts_nothing(void) { classify_request_duplicate_on_second_call(); }
static void purging_expired_entries_leaves_the_index_level_with_the_cache(void) {
    reset(128,16,0,0); fips_node_addr a=addr(1),b=addr(2);
    for(uint64_t i=0;i<5;i++) deliver(i,addr(0xbb),a,addr(0x99),1000,60000,128,2);
    for(uint64_t i=100;i<103;i++) deliver(i,addr(0xbb),b,addr(0x99),1000,60000,128,2);
    CHECK(fips_lookup_peer_entries(&l,&a)+fips_lookup_peer_entries(&l,&b)==l.recent_len);
    fips_lookup_purge_recent(&l,61000,60000); CHECK(l.recent_len==8);
    fips_lookup_purge_recent(&l,61001,60000); CHECK(!l.recent_len && !fips_lookup_peer_entries(&l,&a) && !fips_lookup_peer_entries(&l,&b));
}
static void an_eviction_carries_the_evicted_entrys_age_and_whether_it_was_answered(void) {
    reset(128,16,0,0);
    deliver(0,addr(0xbb),addr(1),addr(0x99),1000,600000,128,SIZE_MAX);
    for(uint64_t i=1;i<64;i++) deliver(i,addr(0xbb),addr(1),addr(0x99),2000,600000,128,SIZE_MAX);
    uint8_t digest[8]={0}; CHECK(fips_recent_request_note_relayed(fips_lookup_recent(&l,1),digest));
    fips_lookup_classification c=deliver(500,addr(0xbb),addr(1),addr(0x99),5500,600000,128,SIZE_MAX);
    CHECK(c.has_eviction && c.evicted.request_id==0 && c.evicted.age_ms==4500 && !c.evicted.answered);
    c=deliver(501,addr(0xbb),addr(1),addr(0x99),7000,600000,128,SIZE_MAX);
    CHECK(c.has_eviction && c.evicted.request_id==1 && c.evicted.age_ms==5000 && c.evicted.answered);
}
static void evicted_ids_are_remembered_up_to_the_ring_size_and_the_oldest_forgotten(void) {
    reset(128,16,0,0);
    for(uint64_t i=0;i<64+1024+1;i++) deliver(i,addr(0xbb),addr(1),addr(0x99),1000,600000,128,SIZE_MAX);
    CHECK(!fips_lookup_was_evicted(&l,0) && fips_lookup_was_evicted(&l,1) && fips_lookup_was_evicted(&l,1024) && !fips_lookup_was_evicted(&l,1025));
}
static void an_expiry_purge_is_not_recorded_as_an_eviction(void) {
    reset(128,16,0,0); deliver(7,addr(0xbb),addr(1),addr(0x99),1000,600000,128,SIZE_MAX);
    fips_lookup_purge_recent(&l,601001,600000); CHECK(!l.recent_len && !fips_lookup_was_evicted(&l,7));
}
static void test_backoff_not_suppressed_initially(void) { reset(128,16,0,0); fips_node_addr t=addr(1); CHECK(!fips_lookup_backoff_is_suppressed(&l.backoff,&t,0)); }
static void test_backoff_suppressed_after_failure(void) {
    reset(128,16,30,300); fips_node_addr a=addr(1),b=addr(2);
    fips_lookup_backoff_record_failure(&l.backoff,&a,1000);
    CHECK(fips_lookup_backoff_is_suppressed(&l.backoff,&a,1000) && !fips_lookup_backoff_is_suppressed(&l.backoff,&b,1000));
}
static void test_backoff_cleared_on_success(void) {
    test_backoff_suppressed_after_failure(); fips_node_addr a=addr(1);
    fips_lookup_backoff_record_success(&l.backoff,&a); CHECK(!l.backoff.len && !fips_lookup_backoff_is_suppressed(&l.backoff,&a,1000));
}
static void test_backoff_reset_all(void) {
    test_backoff_suppressed_after_failure(); fips_node_addr b=addr(2);
    fips_lookup_backoff_record_failure(&l.backoff,&b,1000); CHECK(l.backoff.len==2);
    CHECK(fips_lookup_reset_backoff(&l)==2 && !l.backoff.len && !fips_lookup_reset_backoff(&l));
}
static void test_backoff_exponential(void) {
    reset(128,16,1,300); fips_node_addr a=addr(1);
    const uint64_t waits[]={1000,2000,4000};
    for(uint32_t i=0;i<3;i++) {
        fips_lookup_backoff_record_failure(&l.backoff,&a,1000);
        CHECK(fips_lookup_backoff_failure_count(&l.backoff,&a)==i+1);
        CHECK(fips_lookup_backoff_is_suppressed(&l.backoff,&a,1000+waits[i]-1) && !fips_lookup_backoff_is_suppressed(&l.backoff,&a,1000+waits[i]));
    }
}
static void test_backoff_expires(void) {
    reset(128,16,0,0); fips_node_addr a=addr(1); fips_lookup_backoff_record_failure(&l.backoff,&a,1000);
    CHECK(!l.backoff.len && !fips_lookup_backoff_is_suppressed(&l.backoff,&a,1000));
}
static void test_backoff_capped(void) {
    reset(128,16,1,10); fips_node_addr a=addr(1);
    for(size_t i=0;i<20;i++) fips_lookup_backoff_record_failure(&l.backoff,&a,1000);
    CHECK(l.backoff.entries[0].suppress_until_ms==11000);
}
static void with_backoff_enabled_five_thousand_distinct_failures_never_hold_more_than_max_backoff_entries(void) {
    reset(128,16,30,300);
    for(uint32_t i=0;i<5000;i++) { fips_node_addr a=addr(i); fips_lookup_backoff_record_failure(&l.backoff,&a,1000+i); CHECK(l.backoff.len<=4096); }
    CHECK(l.backoff.len==4096);
}
static void fill_backoff(void) {
    reset(128,16,30,300);
    for(uint32_t i=0;i<4096;i++) { fips_node_addr a=addr(i); fips_lookup_backoff_record_failure(&l.backoff,&a,1000+i); }
    CHECK(l.backoff.len==4096 && !fips_lookup_backoff_take_evicted(&l.backoff));
}
static void at_the_failure_table_bound_the_entry_nearest_expiry_is_evicted_and_the_newest_is_kept(void) {
    fill_backoff(); fips_node_addr a=addr(4096),first=addr(0),next=addr(1);
    fips_lookup_backoff_record_failure(&l.backoff,&a,5096);
    CHECK(l.backoff.len==4096 && !fips_lookup_backoff_failure_count(&l.backoff,&first) && fips_lookup_backoff_failure_count(&l.backoff,&next));
    CHECK(fips_lookup_backoff_failure_count(&l.backoff,&a) && fips_lookup_backoff_take_evicted(&l.backoff)==1 && !fips_lookup_backoff_take_evicted(&l.backoff));
}
static void at_the_failure_table_bound_eviction_follows_windows_moved_by_a_repeat_failure_and_skips_targets_cleared_by_success(void) {
    fill_backoff(); fips_node_addr a=addr(0),b=addr(1),c=addr(2),d=addr(3),e=addr(4096),f=addr(4097);
    fips_lookup_backoff_record_failure(&l.backoff,&a,5096); fips_lookup_backoff_record_success(&l.backoff,&b);
    CHECK(l.backoff.len==4095); fips_lookup_backoff_record_failure(&l.backoff,&e,5096); CHECK(!fips_lookup_backoff_take_evicted(&l.backoff));
    fips_lookup_backoff_record_failure(&l.backoff,&f,5096);
    CHECK(l.backoff.len==4096 && fips_lookup_backoff_take_evicted(&l.backoff)==1 && fips_lookup_backoff_failure_count(&l.backoff,&a)==2);
    CHECK(!fips_lookup_backoff_failure_count(&l.backoff,&c) && fips_lookup_backoff_failure_count(&l.backoff,&d) && fips_lookup_backoff_failure_count(&l.backoff,&e) && fips_lookup_backoff_failure_count(&l.backoff,&f));
}
static void with_a_zero_backoff_cap_and_a_nonzero_base_one_hundred_distinct_failures_record_nothing(void) {
    reset(128,16,30,300);
    for(uint32_t i=0;i<100;i++) { fips_node_addr a=addr(i); fips_lookup_backoff_record_failure(&l.backoff,&a,1000); }
    CHECK(l.backoff.len==100); l.backoff.max_ms=0; fips_lookup_backoff_reset_all(&l.backoff);
    for(uint32_t i=0;i<100;i++) { fips_node_addr a=addr(i); fips_lookup_backoff_record_failure(&l.backoff,&a,1000); }
    CHECK(!l.backoff.len && !fips_lookup_backoff_is_enabled(&l.backoff));
}
static bool allow(uint32_t from, uint32_t target, uint64_t now) {
    fips_node_addr a=addr(from),b=addr(target); return fips_lookup_forward_should_forward(&l.forward_limiter,&a,&b,now);
}
static void test_forward_first_allowed(void) { reset(128,16,0,0); CHECK(allow(0xf0,1,0)); }
static void test_forward_rapid_rate_limited(void) { reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && !allow(0xf0,1,1000) && !allow(0xf0,1,1000)); }
static void test_forward_different_targets_independent(void) { reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && allow(0xf0,2,1000) && !allow(0xf0,1,1000) && !allow(0xf0,2,1000)); }
static void test_forward_allowed_after_interval(void) { reset(128,16,0,0); l.forward_limiter.min_interval_ms=100; CHECK(allow(0xf0,1,1000) && allow(0xf0,1,1110)); }
static void test_forward_cleanup_removes_old(void) {
    reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && allow(0xf0,2,1000) && l.forward_limiter.len==2);
    fips_lookup_forward_prune(&l.forward_limiter,62000); CHECK(!l.forward_limiter.len);
}
static void test_forward_cleanup_preserves_recent(void) {
    reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && allow(0xf1,2,7000) && l.forward_limiter.len==2);
    fips_lookup_forward_prune(&l.forward_limiter,8500);
    CHECK(!forward_holds(addr(0xf0),addr(1)) && forward_holds(addr(0xf1),addr(2)));
}
static void test_forward_different_peers_independent(void) { reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && allow(0xf1,1,1000) && !allow(0xf0,1,1000) && !allow(0xf1,1,1000)); }
static void an_entry_is_forgotten_once_its_interval_has_passed(void) {
    reset(128,16,0,0); CHECK(allow(0xf0,1,1000) && !allow(0xf0,1,2999) && allow(0xf0,1,3000) && l.forward_limiter.len==1);
    fips_lookup_forward_prune(&l.forward_limiter,63000); CHECK(!l.forward_limiter.len && !forward_live(addr(0xf0)));
}
static void a_full_limiter_charges_the_heaviest_peer_and_still_records_the_newcomer(void) {
    reset(128,16,0,0); for(uint32_t i=0;i<4096;i++) CHECK(allow(1,0x1000+i,1000));
    CHECK(l.forward_limiter.len==4096 && !fips_lookup_forward_take_evicted(&l.forward_limiter));
    CHECK(allow(2,0xaa,1000) && l.forward_limiter.len==4096 && fips_lookup_forward_take_evicted(&l.forward_limiter)==1);
    CHECK(forward_holds(addr(2),addr(0xaa)) && !forward_holds(addr(1),addr(0x1000)) && forward_holds(addr(1),addr(0x1001)) && !allow(2,0xaa,1100));
    CHECK(!fips_lookup_forward_take_evicted(&l.forward_limiter));
}
static void equal_stamps_are_evicted_in_insertion_order_not_target_order(void) {
    reset(128,16,0,0); for(uint32_t i=0;i<4096;i++) CHECK(allow(1,4096-i,1000));
    CHECK(allow(2,0xaa,1000) && !forward_holds(addr(1),addr(4096)) && forward_holds(addr(1),addr(1)));
}
static void one_target_refreshed_for_an_hour_leaves_one_entry_and_one_queue_element(void) {
    reset(128,16,0,0);
    for(uint64_t now=1000;now<3601000;now+=2001) CHECK(allow(1,0xaa,now) && l.forward_limiter.len==1 && forward_live(addr(1))==1);
}
static void the_heaviest_peer_is_chosen_by_live_entries_not_queue_length(void) {
    reset(128,16,0,0); CHECK(allow(1,0xa1,10000));
    const uint64_t stamps[]={1000,3000,5000,7000};
    for(size_t i=0;i<4;i++) CHECK(allow(1,0xa2,stamps[i]));
    /* C has no stale queue: its insertion-order table is the index. Preserve
     * the upstream regression's live-count eviction and backward-clock setup. */
    CHECK(forward_live(addr(1))==2);
    for(uint32_t t=0xb1;t<=0xb3;t++) CHECK(allow(2,t,7000));
    uint32_t i=0;
    while(l.forward_limiter.len+2<=4096) { CHECK(allow(0x70000000+i,1,7000) && allow(0x70000000+i,2,7000)); i++; }
    while(l.forward_limiter.len<4096) CHECK(allow(0x7f000000,1,7000));
    CHECK(!fips_lookup_forward_take_evicted(&l.forward_limiter) && allow(3,0xcc,7000));
    CHECK(fips_lookup_forward_take_evicted(&l.forward_limiter)==1 && forward_live(addr(2))==2 && forward_live(addr(1))==2);
}
static void after_a_backward_clock_step_a_peers_queue_stays_within_one_sweep_of_its_entries(void) {
    reset(128,16,0,0); CHECK(allow(1,0xa0,10000000)); fips_lookup_forward_prune(&l.forward_limiter,10000000);
    uint64_t now=6400000;
    for(size_t i=0;i<900;i++) {
        for(uint32_t t=0;t<100;t++) CHECK(allow(1,0x1000+t,now));
        fips_lookup_forward_prune(&l.forward_limiter,now); now+=2001;
        CHECK(l.forward_limiter.len<=101); /* no unbounded stale index in C */
    }
    CHECK(forward_live(addr(1))==101);
}
static void c_signature_failures_and_sign_limiter(void) {
    response(99); fips_proto_error error;
    CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_OK);
    resp->request_id++; CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_ERR_RANGE && error.variant==FIPS_PROTO_InvalidSignature); resp->request_id--;
    resp->target_coords.entries[1].node_addr.bytes[0]^=1;
    CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_ERR_RANGE); resp->target_coords.entries[1].node_addr.bytes[0]^=1;
    resp->proof[0]^=1; CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_ERR_RANGE); resp->proof[0]^=1;
    resp->target.bytes[0]^=1; CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_ERR_RANGE); resp->target.bytes[0]^=1;
    CHECK(fips_lookup_response_verify(resp,secp,&peer,scratch,0,&error)==FIPS_ERR_RANGE && error.variant==FIPS_PROTO_MessageTooLong);
    fips_sign_entry entries[2]; fips_lookup_sign_limiter limiter; fips_lookup_sign_init(&limiter,entries,2,1,1);
    fips_node_addr from=addr(1),other=addr(2),third=addr(3);
    fips_test_clock clock={0}; fips_clock adapter=fips_test_clock_adapter(&clock);
    bool signed_out=false;
    CHECK(fips_lookup_response_sign(resp,identity,&limiter,&from,adapter.now(adapter.ctx),scratch,sizeof(scratch),&signed_out,&error)==FIPS_OK && signed_out);
    uint8_t saved[64]; memcpy(saved,resp->proof,64); resp->path_mtu=1234;
    CHECK(fips_lookup_response_sign(resp,identity,&limiter,&from,adapter.now(adapter.ctx),scratch,sizeof(scratch),&signed_out,&error)==FIPS_OK && !signed_out);
    CHECK(!memcmp(saved,resp->proof,64) && resp->path_mtu==1234);
    CHECK(fips_lookup_response_sign(resp,identity,&limiter,&other,adapter.now(adapter.ctx),scratch,sizeof(scratch),&signed_out,&error)==FIPS_OK && signed_out);
    CHECK(fips_lookup_response_sign(resp,identity,&limiter,&third,adapter.now(adapter.ctx),scratch,sizeof(scratch),&signed_out,&error)==FIPS_ERR_RANGE);
    CHECK(fips_test_clock_advance(&clock,1000)==FIPS_OK);
    CHECK(fips_lookup_response_sign(resp,identity,&limiter,&from,adapter.now(adapter.ctx),scratch,sizeof(scratch),&signed_out,&error)==FIPS_OK && signed_out);
    CHECK(resp->path_mtu==UINT16_MAX);
}
static void c_handle_response_verifies_before_accepting_and_rejects_stale(void) {
    response(42); start(resp->target,1000,0,0);
    fips_pending_lookup_record(fips_lookup_pending(&l,&resp->target),42);
    fips_node_addr from=addr(1); fips_response_route r={.kind=FIPS_RESPONSE_Transit}; fips_proto_error error;
    resp->proof[0]^=1;
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,2000,secp,&peer,scratch,sizeof(scratch),&actions,&r,&error)==FIPS_ERR_RANGE);
    CHECK(l.pending_len==1 && actions.len==0 && r.kind==FIPS_RESPONSE_Transit && error.variant==FIPS_PROTO_InvalidSignature);
    resp->proof[0]^=1; resp->path_mtu=1400;
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,2000,secp,&peer,scratch,sizeof(scratch),&actions,&r,&error)==FIPS_OK);
    CHECK(r.kind==FIPS_RESPONSE_Originator && actions.len==4 && !l.pending_len && items[0].path_mtu==1400 && items[0].request_id==42);
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,3000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_OK);
    CHECK(r.kind==FIPS_RESPONSE_Unsolicited && !actions.len);
    CHECK(gate(resp->target,4000,true)==FIPS_INITIATE_Proceed);
    fips_pending_lookup_record(fips_lookup_pending(&l,&resp->target),43);
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,4000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_OK && r.kind==FIPS_RESPONSE_Unsolicited);
    fips_pending_lookup_record(fips_lookup_pending(&l,&resp->target),42);
    actions.capacity=3;
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,4000,secp,&peer,scratch,sizeof(scratch),&actions,&r,&error)==FIPS_ERR_RANGE && l.pending_len==1);
    actions.capacity=256;
    poll_at(5000,ladder,1);
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,5000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_OK && r.kind==FIPS_RESPONSE_Unsolicited);
}
static void c_transit_response_mtu_and_atomic_capacity(void) {
    response(7); reset(128,16,0,0); mock_view v={{{addr(2),true,true}},1};
    CHECK(transit(7,resp->target,addr(1),1000,&v).outcome==FIPS_FORWARD_Forward);
    fips_node_addr from=addr(2); fips_response_route r; fips_proto_error error;
    actions.bytes_capacity=1;
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,1000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_ERR_RANGE);
    CHECK(!fips_lookup_recent(&l,7)->relayed_len);
    actions.bytes_capacity=sizeof(bytes);
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,1000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_OK);
    CHECK(r.kind==FIPS_RESPONSE_Transit && actions.len==1 && same(items[0].peer,addr(1)) && resp->path_mtu==UINT16_MAX);
    fips_lookup_response *decoded=malloc(sizeof(*decoded)); CHECK(decoded);
    fips_reader reader; fips_reader_init(&reader,bytes+1,items[0].bytes_len-1);
    CHECK(fips_lookup_response_decode(&reader,decoded,&error)==FIPS_OK && decoded->path_mtu==1280);
    CHECK(fips_lookup_response_verify(decoded,secp,&peer,scratch,sizeof(scratch),&error)==FIPS_OK);
    CHECK(fips_lookup_handle_response(&l,resp,&from,1280,1000,NULL,NULL,NULL,0,&actions,&r,&error)==FIPS_OK && r.kind==FIPS_RESPONSE_AlreadyForwarded && !actions.len);
    free(decoded);
}
static void c_wire_bounds_failure_atomicity_and_full_u16_domain(void) {
    response(7); fips_writer w; fips_reader reader; fips_proto_error e;
    fips_writer_init(&w,bytes,sizeof(bytes)); CHECK(fips_lookup_response_encode(resp,&w,NULL)==FIPS_OK);
    size_t n=w.pos; uint64_t sentinel=resp->request_id;
    for(size_t i=0;i<n-1;i++) {
        fips_reader_init(&reader,bytes+1,i); CHECK(fips_lookup_response_decode(&reader,resp,&e)==FIPS_ERR_RANGE && !reader.pos && resp->request_id==sentinel);
    }
    bytes[n]=0x55; fips_reader_init(&reader,bytes+1,n);
    CHECK(fips_lookup_response_decode(&reader,resp,NULL)==FIPS_OK && reader.pos==n-1 && fips_reader_remaining(&reader)==1);
    fips_writer_init(&w,bytes,1); bytes[0]=0xaa;
    CHECK(fips_lookup_response_encode(resp,&w,&e)==FIPS_ERR_RANGE && !w.pos && bytes[0]==0xaa && e.variant==FIPS_PROTO_MessageTooLong);
    request(4,addr(1),5); fips_writer_init(&w,bytes,sizeof(bytes)); CHECK(fips_lookup_request_encode(req,&w,NULL)==FIPS_OK); n=w.pos;
    for(size_t i=0;i<n-1;i++) {
        fips_reader_init(&reader,bytes+1,i); CHECK(fips_lookup_request_decode(&reader,req,&e)==FIPS_ERR_RANGE && !reader.pos && req->request_id==4);
    }
    bytes[44]=bytes[45]=0; fips_reader_init(&reader,bytes+1,n-1);
    CHECK(fips_lookup_request_decode(&reader,req,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_Malformed && !strcmp(e.as.malformed,"coordinate with zero entries"));
    bytes[44]=bytes[45]=0xff; fips_reader_init(&reader,bytes+1,n-1);
    CHECK(fips_lookup_request_decode(&reader,req,&e)==FIPS_ERR_RANGE && e.variant==FIPS_PROTO_MessageTooShort);
    uint8_t *large=malloc(FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE); CHECK(large);
    resp->target_coords.len=UINT16_MAX;
    for(size_t i=0;i<UINT16_MAX;i++) resp->target_coords.entries[i]=fips_coord_entry_addr_only(addr((uint32_t)i));
    fips_writer_init(&w,large,FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
    CHECK(fips_lookup_response_encode(resp,&w,NULL)==FIPS_OK && w.pos==FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
    fips_reader_init(&reader,large+1,w.pos-1);
    CHECK(fips_lookup_response_decode(&reader,resp,NULL)==FIPS_OK && resp->target_coords.len==UINT16_MAX && same(resp->target_coords.entries[UINT16_MAX-1].node_addr,addr(UINT16_MAX-1)));
    req->origin_coords.len=UINT16_MAX;
    for(size_t i=0;i<UINT16_MAX;i++) req->origin_coords.entries[i]=fips_coord_entry_addr_only(addr((uint32_t)i));
    fips_writer_init(&w,large,FIPS_LOOKUP_RESPONSE_MAX_WIRE_SIZE);
    CHECK(fips_lookup_request_encode(req,&w,NULL)==FIPS_OK && w.pos==FIPS_LOOKUP_REQUEST_MAX_WIRE_SIZE);
    fips_reader_init(&reader,large+1,w.pos-1);
    CHECK(fips_lookup_request_decode(&reader,req,NULL)==FIPS_OK && req->origin_coords.len==UINT16_MAX);
    free(large);
}
static void c_state_capacity_ladder_and_action_bounds(void) {
    reset(128,1,0,0); CHECK(gate(addr(1),0,true)==FIPS_INITIATE_Proceed);
    fips_initiate_decision decision=FIPS_INITIATE_Suppressed; uint32_t failures=42; fips_node_addr b=addr(2);
    CHECK(fips_lookup_initiate_gate(&l,&b,0,true,&decision,&failures)==FIPS_ERR_RANGE && decision==FIPS_INITIATE_Suppressed && failures==42 && l.pending_len==1);
    poll_out.retries_capacity=0;
    CHECK(fips_lookup_poll_pending(&l,1000,ladder,4,&poll_out)==FIPS_ERR_RANGE && l.pending_lookups[0].attempt==1);
    poll_out.retries_capacity=4096;
    uint64_t zeros[256]={0};
    for(size_t i=0;i<254;i++) { poll_at(0,zeros,256); CHECK(poll_out.retries_len==1 && l.pending_lookups[0].attempt==i+2); }
    poll_at(0,zeros,256); CHECK(poll_out.timeouts_len==1 && !l.pending_len);
    CHECK(gate(addr(1),1000,true)==FIPS_INITIATE_Proceed); poll_at(0,ladder,4); CHECK(!poll_out.retries_len && !poll_out.timeouts_len);
    poll_at(0,NULL,0); CHECK(poll_out.timeouts_len==1);
    mock_view v={0}; for(uint32_t i=0;i<257;i++) v.peers[v.len++]=(mock_peer){addr(i),true,true};
    fips_lookup_routing_view rv=view(&v); fips_node_addr from=addr(999); request(1,addr(0xaa),3); fips_forward_result result={.outcome=FIPS_FORWARD_RateLimited};
    CHECK(fips_lookup_plan_forward(req,&from,&rv,&actions,&result)==FIPS_ERR_RANGE && req->ttl==3 && result.outcome==FIPS_FORWARD_RateLimited);
    v.len=1; rv=view(&v); actions.capacity=0;
    CHECK(fips_lookup_plan_forward(req,&from,&rv,&actions,&result)==FIPS_ERR_RANGE && req->ttl==3);
    actions.capacity=256; actions.bytes_capacity=0;
    CHECK(fips_lookup_plan_forward(req,&from,&rv,&actions,&result)==FIPS_ERR_RANGE && req->ttl==3);
    actions.bytes_capacity=sizeof(bytes);
    CHECK(fips_lookup_plan_forward(req,&from,&rv,&actions,&result)==FIPS_OK && result.outcome==FIPS_FORWARD_Forward && req->ttl==2);
}

static void c_borrowed_pending_target_survives_removal(void) {
    fips_node_addr a=addr(1), b=addr(2);
    reset(128,2,1000,8000);
    CHECK(gate(a,0,true)==FIPS_INITIATE_Proceed);
    CHECK(gate(b,0,true)==FIPS_INITIATE_Proceed);
    request(42,a,3);
    const fips_node_addr *borrowed=&fips_lookup_pending(&l,&a)->target;
    CHECK(fips_lookup_on_response_accepted(&l,42,borrowed,&req->origin_coords,100,1200,&actions)==FIPS_OK);
    CHECK(actions.len==4 && l.pending_len==1 && same(l.pending_lookups[0].target,b));
    for(size_t i=0;i<actions.len;i++) CHECK(same(actions.items[i].target,a));
    CHECK(gate(a,200,true)==FIPS_INITIATE_Proceed);
    borrowed=&fips_lookup_pending(&l,&a)->target;
    fips_lookup_initiate_failed(&l,borrowed,300);
    CHECK(l.pending_len==1 && same(l.pending_lookups[0].target,b));
    CHECK(fips_lookup_backoff_failure_count(&l.backoff,&a)==1);
    CHECK(fips_lookup_backoff_failure_count(&l.backoff,&b)==0);
}

int main(void) {
    req=malloc(sizeof(*req)); resp=malloc(sizeof(*resp)); CHECK(req && resp);
    crypto_init(); reset(128,16,0,0);
    planners();
    RUN(test_lookup_request_forward);
    RUN(test_lookup_request_ttl_exhausted);
    RUN(test_lookup_request_generate);
    RUN(test_lookup_response_proof_bytes);
    RUN(test_lookup_request_encode_decode_roundtrip);
    RUN(test_lookup_request_decode_too_short);
    RUN(test_lookup_request_min_mtu_boundary_values);
    RUN(test_lookup_response_encode_decode_roundtrip);
    RUN(test_lookup_response_path_mtu_roundtrip);
    RUN(test_lookup_response_path_mtu_not_in_proof_bytes);
    RUN(test_lookup_response_decode_too_short);
    RUN(classify_response_transit_on_fresh_forwarded_request);
    RUN(classify_response_already_forwarded_on_second_call);
    RUN(classify_response_originator_wins_over_a_transit_dedup_record);
    RUN(classify_response_originator_when_a_pending_lookup_issued_the_id);
    RUN(classify_response_unsolicited_when_nothing_correlates_the_id);
    RUN(on_response_accepted_clears_state_and_emits_effects);
    RUN(cache_coords_action_carries_the_request_id_the_response_arrived_with);
    RUN(poll_pending_no_action_before_first_deadline);
    RUN(poll_pending_retries_at_first_deadline);
    RUN(poll_pending_final_timeout_at_max_attempt);
    RUN(poll_pending_final_timeout_at_max_attempt_records_a_failure_with_backoff_enabled);
    RUN(with_backoff_enabled_an_entry_is_forgotten_once_its_window_and_one_cap_interval_have_passed_on_the_lookup_tick);
    RUN(a_bloom_miss_under_the_default_disabled_backoff_records_nothing_so_ten_thousand_distinct_misses_leave_the_failure_table_empty);
    RUN(with_backoff_enabled_a_failed_target_is_suppressed_inside_its_window_allowed_after_it_and_escalates_on_the_next_failure);
    RUN(classify_request_forwards_fresh_and_records_it);
    RUN(classify_request_duplicate_on_second_call);
    RUN(classify_request_drops_our_own_request_looped_back_to_us);
    RUN(classify_request_records_our_own_id_as_transit_once_its_lookup_has_timed_out);
    RUN(classify_request_records_an_own_id_older_than_the_recorded_window_as_transit);
    RUN(classify_request_still_transits_a_foreign_id_for_a_target_we_are_looking_up);
    RUN(classify_request_evicts_rather_than_refusing_a_full_dedup_cache);
    RUN(classify_request_respond_as_target);
    RUN(classify_request_ttl_exhausted_for_non_target);
    RUN(plan_transit_forward_rate_limited);
    RUN(classify_request_purges_expired_entries);
    RUN(poll_pending_full_ladder_end_to_end);
    RUN(initiate_gate_deduplicated_when_pending);
    RUN(initiate_gate_suppressed_by_backoff);
    RUN(initiate_gate_bloom_miss_records_failure);
    RUN(initiate_gate_proceed_inserts_pending);
    RUN(initiate_failed_drops_pending_and_records_failure);
    RUN(a_response_from_the_requests_upstream_peer_is_ineligible);
    RUN(a_response_for_another_target_is_ineligible);
    RUN(a_response_from_a_peer_the_request_was_not_forwarded_to_is_ineligible);
    RUN(a_response_to_a_request_this_node_did_not_forward_is_ineligible);
    RUN(a_response_from_each_peer_the_request_was_forwarded_to_is_eligible);
    RUN(an_identical_response_is_relayed_once);
    RUN(a_reverse_path_cycle_stops_after_one_circuit);
    RUN(the_duplicate_memory_holds_at_most_eight_digests);
    RUN(a_ninth_distinct_response_is_still_relayed);
    RUN(one_peer_refreshing_a_target_does_not_suppress_another_peers_request_for_it);
    RUN(a_peer_repeating_a_target_within_the_interval_is_still_suppressed);
    RUN(a_target_no_peer_reaches_takes_no_limiter_entry);
    RUN(a_full_limiter_still_records_the_newcomer);
    RUN(poll_pending_retries_rather_than_times_out_under_a_ladder_of_256_entries);
    RUN(poll_pending_neither_overflows_nor_times_out_on_a_huge_ladder_entry);
    RUN(a_peer_over_its_share_evicts_its_own_oldest_and_not_a_light_peers);
    RUN(the_per_peer_share_never_falls_below_the_floor_however_many_peers);
    RUN(a_node_whose_dedup_cache_is_saturated_still_answers_a_lookup_for_itself);
    RUN(a_duplicate_is_not_indexed_twice_and_evicts_nothing);
    RUN(purging_expired_entries_leaves_the_index_level_with_the_cache);
    RUN(an_eviction_carries_the_evicted_entrys_age_and_whether_it_was_answered);
    RUN(evicted_ids_are_remembered_up_to_the_ring_size_and_the_oldest_forgotten);
    RUN(an_expiry_purge_is_not_recorded_as_an_eviction);
    RUN(test_backoff_not_suppressed_initially);
    RUN(test_backoff_suppressed_after_failure);
    RUN(test_backoff_cleared_on_success);
    RUN(test_backoff_reset_all);
    RUN(test_backoff_exponential);
    RUN(test_backoff_expires);
    RUN(test_backoff_capped);
    RUN(with_backoff_enabled_five_thousand_distinct_failures_never_hold_more_than_max_backoff_entries);
    RUN(at_the_failure_table_bound_the_entry_nearest_expiry_is_evicted_and_the_newest_is_kept);
    RUN(at_the_failure_table_bound_eviction_follows_windows_moved_by_a_repeat_failure_and_skips_targets_cleared_by_success);
    RUN(with_a_zero_backoff_cap_and_a_nonzero_base_one_hundred_distinct_failures_record_nothing);
    RUN(test_forward_first_allowed);
    RUN(test_forward_rapid_rate_limited);
    RUN(test_forward_different_targets_independent);
    RUN(test_forward_allowed_after_interval);
    RUN(test_forward_cleanup_removes_old);
    RUN(test_forward_cleanup_preserves_recent);
    RUN(test_forward_different_peers_independent);
    RUN(an_entry_is_forgotten_once_its_interval_has_passed);
    RUN(a_full_limiter_charges_the_heaviest_peer_and_still_records_the_newcomer);
    RUN(equal_stamps_are_evicted_in_insertion_order_not_target_order);
    RUN(one_target_refreshed_for_an_hour_leaves_one_entry_and_one_queue_element);
    RUN(the_heaviest_peer_is_chosen_by_live_entries_not_queue_length);
    RUN(after_a_backward_clock_step_a_peers_queue_stays_within_one_sweep_of_its_entries);
    RUN(c_signature_failures_and_sign_limiter);
    RUN(c_handle_response_verifies_before_accepting_and_rejects_stale);
    RUN(c_transit_response_mtu_and_atomic_capacity);
    RUN(c_wire_bounds_failure_atomicity_and_full_u16_domain);
    RUN(c_state_capacity_ladder_and_action_bounds);
    RUN(c_borrowed_pending_target_survives_removal);
    free(l.recent_requests); free(l.pending_lookups); free(l.backoff.entries); free(l.forward_limiter.entries);
    free(req); free(resp); fips_identity_destroy(identity); fips_identity_destroy_secp(secp);
    printf("Lookup: %u tests passed\n",tests); return 0;
}
