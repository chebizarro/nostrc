/* All 49 named tests from pinned proto/routing/tests/{core,wire,limits}.rs.
 * Mock reach sets become real bloom filters; time is synthetic throughout. */
#include <fips/proto/routing/mod.h>
#include "fakes/host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == FIPS_OK)
#define EQ(a,b) CHECK(memcmp((a).bytes, (b).bytes, 16) == 0)
static fips_tree_coordinate coords[8], decoded_coords;
static fips_bloom_filter filters[8];
static fips_routing_peer peers[8];
static fips_router router;
static fips_routing_error_rate_limiter limiter;
static fips_node_addr mine, dest, source;
static fips_routing_view view;
static size_t cache_reads, congestion_reads;
static bool cached, congested;
static uint8_t bytes[512];
static fips_writer writer;
static fips_node_addr addr(uint8_t n) { fips_node_addr a = {{0}}; a.bytes[0] = n; return a; }
static const fips_tree_coordinate *cache(void *ctx, const fips_node_addr *target, fips_instant now)
{
    (void)ctx; CHECK(now.ops == fips_test_instant(0).ops); ++cache_reads;
    return cached && !memcmp(target->bytes, dest.bytes, 16) ? &coords[0] : NULL;
}
static bool congestion(void *ctx, const fips_node_addr *target)
{ (void)ctx; (void)target; ++congestion_reads; return congested; }
static void reset(void)
{
    mine = addr(0x10); dest = addr(0x20); source = addr(0x21);
    cached = congested = false; cache_reads = congestion_reads = 0;
    view = (fips_routing_view){.ctx = NULL, .cached_coords = cache, .is_congested = congestion};
    fips_router_init(&router, fips_test_instant(0));
    fips_routing_error_rate_limiter_init(&limiter, fips_test_instant(0));
    fips_writer_init(&writer, bytes, sizeof(bytes));
}
static void path(size_t slot, const uint8_t *ids, size_t n)
{
    fips_node_addr addrs[16]; CHECK(n <= 16);
    for (size_t i = 0; i < n; ++i) addrs[i] = addr(ids[i]);
    OK(fips_tree_coordinate_from_addrs(&coords[slot], addrs, n, NULL));
}
#define PATH(slot,...) do { const uint8_t ids[] = {__VA_ARGS__}; path(slot,ids,sizeof(ids)); } while (0)
static void peer(size_t i, uint8_t id, bool reach, double cost, const fips_tree_coordinate *c)
{
    fips_bloom_init(&filters[i]); if (reach) fips_bloom_insert(&filters[i], &dest);
    peers[i] = (fips_routing_peer){addr(id), c, &filters[i], cost};
    view.peers = peers; view.peer_count = i + 1;
}
static fips_session_datagram datagram(uint8_t ttl, bool local)
{
    fips_session_datagram dg; fips_session_datagram_init(&dg, addr(1), local ? mine : dest, NULL, 0);
    dg.ttl = ttl; return dg;
}
static fips_session_datagram decode_forward(void)
{
    fips_reader r; fips_session_datagram dg;
    CHECK(bytes[0] == FIPS_LINK_SessionDatagram);
    fips_reader_init(&r, bytes + 1, writer.pos - 1);
    OK(fips_session_datagram_decode(&r, &dg, NULL)); return dg;
}
static fips_route_outcome route(uint8_t ttl, bool local, bool hop, bool ce, uint16_t mtu)
{
    fips_session_datagram dg = datagram(ttl, local); dg.path_mtu = mtu;
    fips_next_hop nh = {addr(0x30), 1400}; fips_route_outcome out;
    OK(fips_router_route(&router, &dg, &mine, ce, hop ? &nh : NULL, &view, &writer, &out, NULL));
    return out;
}
static void ttl_zero_drops_as_exhausted(void)
{ reset(); fips_route_outcome o = route(0,false,true,false,UINT16_MAX); CHECK(o.kind == FIPS_ROUTE_Drop && o.reason == FIPS_DROP_TtlExhausted); CHECK(writer.pos == 0); }
static void ttl_zero_to_self_delivers_local(void)
{ reset(); CHECK(route(0,true,false,false,UINT16_MAX).kind == FIPS_ROUTE_DeliverLocal); CHECK(writer.pos == 0); }
static void ttl_one_transit_drops_before_forwarding(void)
{ reset(); CHECK(route(1,false,true,false,UINT16_MAX).kind == FIPS_ROUTE_Drop); CHECK(writer.pos == 0); }
static void ttl_two_transit_forwards_at_one(void)
{ reset(); CHECK(route(2,false,true,false,UINT16_MAX).kind == FIPS_ROUTE_Forward); CHECK(decode_forward().ttl == 1); }
static void destination_is_self_delivers_local(void)
{ reset(); CHECK(route(5,true,false,false,UINT16_MAX).kind == FIPS_ROUTE_DeliverLocal); }
static void transit_without_next_hop_is_no_route(void)
{ reset(); CHECK(route(5,false,false,false,UINT16_MAX).kind == FIPS_ROUTE_NoRoute); CHECK(writer.pos == 0); }
static void forward_decrements_ttl_and_folds_link_mtu(void)
{
    reset(); fips_route_outcome o = route(5,false,true,false,UINT16_MAX);
    CHECK(o.kind == FIPS_ROUTE_Forward && !o.outgoing_ce); EQ(o.next_hop,addr(0x30));
    fips_session_datagram dg = decode_forward(); CHECK(dg.ttl == 4 && dg.path_mtu == 1400);
}
static void forward_keeps_smaller_datagram_path_mtu(void)
{ reset(); CHECK(route(5,false,true,false,900).kind == FIPS_ROUTE_Forward); CHECK(decode_forward().path_mtu == 900); }
static void outgoing_ce_set_by_incoming_ce(void)
{ reset(); CHECK(route(5,false,true,true,UINT16_MAX).outgoing_ce); CHECK(congestion_reads == 0); }
static void outgoing_ce_set_by_local_congestion(void)
{ reset(); congested = true; CHECK(route(5,false,true,false,UINT16_MAX).outgoing_ce); CHECK(congestion_reads == 1); }
static void outgoing_ce_clear_when_neither_signal(void)
{ reset(); CHECK(!route(5,false,true,false,UINT16_MAX).outgoing_ce); }
static void cached_coords_reads_the_view_table(void)
{ reset(); cached = true; CHECK(view.cached_coords(NULL,&dest,fips_test_instant(0))); fips_node_addr other = addr(0x41); CHECK(!view.cached_coords(NULL,&other,fips_test_instant(0))); }
static bool choose(fips_node_addr *out)
{ return fips_select_best_candidate(&view,&dest,&coords[0],&coords[1],out); }
static void selection_setup(void)
{ reset(); dest = addr(0x50); PATH(0,0x50,0); PATH(1,0x10,0); PATH(2,0); }
static void candidate_selection_is_independent_of_peer_enumeration_order(void)
{
    selection_setup(); peer(0,0x20,true,1,&coords[2]); peer(1,0x30,true,1,&coords[2]);
    fips_node_addr out; CHECK(choose(&out)); EQ(out,addr(0x20));
    fips_routing_peer tmp = peers[0]; peers[0] = peers[1]; peers[1] = tmp;
    CHECK(choose(&out)); EQ(out,addr(0x20));
}
static void candidate_selection_filters_bloom_and_missing_coords(void)
{
    selection_setup(); peer(0,2,true,0,NULL); peer(1,3,false,0,&coords[0]); peer(2,0x60,true,10,&coords[2]);
    fips_node_addr out; CHECK(choose(&out)); EQ(out,addr(0x60));
}
static void candidate_must_be_strictly_closer_than_self(void)
{
    selection_setup(); PATH(3,0x20,0); PATH(4,0x21,0x20,0);
    peer(0,0x20,true,1,&coords[3]); peer(1,0x21,true,.5,&coords[4]);
    fips_node_addr out = addr(99); CHECK(!choose(&out)); EQ(out,addr(99));
}
static void candidate_ordering_is_cost_then_distance_then_address(void)
{
    selection_setup(); PATH(1,0x10,0x11,0);
    peer(0,1,true,1,&coords[2]); peer(1,2,true,1,&coords[0]);
    peer(2,4,true,.5,&coords[2]); peer(3,3,true,.5,&coords[2]);
    fips_node_addr out; CHECK(choose(&out)); EQ(out,addr(3));
    view.peer_count = 2; CHECK(choose(&out)); EQ(out,addr(2)); /* distance beats address */
}
static fips_error_synth synth(bool mtu, uint64_t now)
{
    fips_error_synth out;
    if (mtu) OK(fips_router_synth_mtu_exceeded(&router,&dest,&source,&mine,1280,fips_test_instant(now),64,&writer,&out,NULL));
    else OK(fips_router_synth_routing_error(&router,&dest,&source,&mine,&view,fips_test_instant(now),64,&writer,&out,NULL));
    return out;
}
static void check_error(fips_error_synth s, uint8_t type)
{
    CHECK(s.has_action && s.verdict == FIPS_LIMIT_Admit && s.action.kind == FIPS_ROUTE_SendError);
    EQ(s.action.toward,source); fips_session_datagram dg = decode_forward();
    EQ(dg.src_addr,mine); EQ(dg.dest_addr,source); CHECK(dg.ttl == 64 && dg.path_mtu == UINT16_MAX);
    CHECK(dg.payload[0] == 0 && dg.payload[1] == 4 && dg.payload[4] == type);
}
static void synth_uses_pathbroken_when_coords_cached(void)
{
    reset(); cached = true; PATH(0,0x20,0); check_error(synth(false,0),0x21);
    fips_session_datagram dg = decode_forward(); CHECK(dg.payload_len == 40 && dg.payload[38] == 0 && dg.payload[39] == 0);
}
static void synth_uses_coords_required_when_not_cached(void)
{ reset(); check_error(synth(false,0),0x20); CHECK(decode_forward().payload_len == 38); }
static void synth_gate(bool mtu)
{
    reset(); CHECK(synth(mtu,0).has_action); size_t n = writer.pos, reads = cache_reads;
    fips_error_synth s = synth(mtu,0); CHECK(!s.has_action && s.verdict == FIPS_LIMIT_Suppress);
    CHECK(writer.pos == n && cache_reads == reads);
    dest = addr(0x22); CHECK(synth(mtu,0).has_action);
}
static void synth_rate_limit_gate_suppresses_second_call(void) { synth_gate(false); }
static void synth_mtu_exceeded_carries_bottleneck_and_targets_source(void)
{ reset(); check_error(synth(true,0),0x22); fips_session_datagram dg = decode_forward(); CHECK(dg.payload[38] == 0 && dg.payload[39] == 5 && cache_reads == 0); }
static void synth_mtu_exceeded_rate_limit_gate_suppresses_second_call(void) { synth_gate(true); }
static void route_drops_for_hop_limit_exactly_when_can_forward_is_false(void)
{
    for (unsigned ttl = 0; ttl <= UINT8_MAX; ++ttl) {
        reset(); fips_session_datagram dg = datagram((uint8_t)ttl,false);
        fips_route_outcome o = route((uint8_t)ttl,false,true,false,UINT16_MAX);
        CHECK((o.kind == FIPS_ROUTE_Forward) == fips_session_datagram_can_forward(&dg));
        if (o.kind == FIPS_ROUTE_Forward) { uint8_t next; CHECK(fips_ttl_after_hop((uint8_t)ttl,&next)); CHECK(decode_forward().ttl == next); }
        else CHECK(o.kind == FIPS_ROUTE_Drop && o.reason == FIPS_DROP_TtlExhausted);
        writer.pos = 0; CHECK(route((uint8_t)ttl,true,true,false,UINT16_MAX).kind == FIPS_ROUTE_DeliverLocal);
    }
}
static bool send(uint8_t id, uint64_t now)
{ fips_node_addr a = addr(id); return fips_routing_error_rate_limiter_should_send(&limiter,&a,fips_test_instant(now)); }
static fips_node_addr minted(uint32_t n)
{
    fips_node_addr a = {{0}}; for (unsigned i = 0; i < 4; ++i) a.bytes[i] = (uint8_t)(n >> (8*i));
    a.bytes[15] = 0xff; return a;
}
static fips_limit_verdict mint_check(uint32_t n, uint64_t now)
{ fips_node_addr a = minted(n); return fips_routing_error_rate_limiter_check(&limiter,&a,fips_test_instant(now)); }
static void test_first_send_allowed(void) { reset(); CHECK(send(1,0)); }
static void test_rapid_sends_rate_limited(void) { reset(); CHECK(send(1,0)); CHECK(!send(1,0)); CHECK(!send(1,50)); }
static void test_different_destinations_independent(void)
{ reset(); CHECK(send(1,0)); CHECK(send(2,0)); CHECK(!send(1,0)); CHECK(!send(2,0)); }
static void test_send_allowed_after_interval(void) { reset(); CHECK(send(1,0)); CHECK(send(1,110)); }
static void test_cleanup_removes_old_entries(void)
{ reset(); CHECK(send(1,0)); CHECK(send(2,0)); CHECK(limiter.inner.len == 2); fips_routing_error_rate_limiter_cleanup(&limiter,fips_test_instant(11000)); CHECK(limiter.inner.len == 0); }
static void test_cleanup_preserves_recent_entries(void)
{ reset(); CHECK(send(1,0)); fips_routing_error_rate_limiter_cleanup(&limiter,fips_test_instant(0)); CHECK(limiter.inner.len == 1); }
static void test_with_interval_custom_rate(void)
{ reset(); fips_routing_error_rate_limiter_with_interval_ms(&limiter,500,fips_test_instant(0)); CHECK(send(1,0)); CHECK(!send(1,0)); CHECK(!send(1,200)); CHECK(send(1,500)); }
static void the_map_stays_bounded_when_a_sender_mints_distinct_destination_keys(void)
{ reset(); for (uint32_t i = 0; i < 100000; ++i) (void)mint_check(i,0); CHECK(limiter.inner.len == FIPS_PER_ADDR_MAX_ENTRIES); }
static void an_admission_at_capacity_still_sends_rather_than_going_silent(void)
{ reset(); for (uint32_t i = 0; i < FIPS_PER_ADDR_MAX_ENTRIES; ++i) CHECK(mint_check(i,0) == FIPS_LIMIT_Admit); CHECK(mint_check(FIPS_PER_ADDR_MAX_ENTRIES,0) == FIPS_LIMIT_AdmitAtCapacity); }
static void the_map_scan_does_not_run_once_per_admitted_destination(void)
{ reset(); (void)mint_check(0,1); uint64_t before = limiter.inner.sweeps; for (uint32_t i = 1; i < 1000; ++i) (void)mint_check(i,1); CHECK(limiter.inner.sweeps == before); }
static void the_map_scan_still_runs_once_a_sweep_interval_has_passed(void)
{ reset(); (void)mint_check(0,1); uint64_t before = limiter.inner.sweeps; (void)mint_check(1,11000); CHECK(limiter.inner.sweeps == before + 1 && limiter.inner.len == 1); }
/* Wire helper variants: 0 CoordsRequired, 1 PathBroken(None),
 * 2 PathBroken(Some), 3 MtuExceeded. */
static void encode_signal(unsigned which, uint16_t mtu)
{
    fips_coords_required cr = fips_coords_required_new(dest,mine);
    fips_path_broken pb = fips_path_broken_new(dest,mine);
    fips_mtu_exceeded me = fips_mtu_exceeded_new(dest,mine,mtu);
    PATH(0,0xcc,0xdd,0xee);
    if (which == 2) fips_path_broken_with_last_coords(&pb,&coords[0]);
    if (which == 0) OK(fips_coords_required_encode(&cr,&writer,NULL));
    else if (which == 3) OK(fips_mtu_exceeded_encode(&me,&writer,NULL));
    else OK(fips_path_broken_encode(&pb,&writer,NULL));
}
static fips_status decode_signal(unsigned which, fips_reader *r, fips_proto_error *e)
{
    fips_coords_required cr = {addr(99),addr(98)};
    fips_mtu_exceeded me = {addr(99),addr(98),77};
    fips_path_broken pb = {addr(99),addr(98),NULL};
    fips_status s;
    if (which == 0) s = fips_coords_required_decode(r,&cr,e);
    else if (which == 3) s = fips_mtu_exceeded_decode(r,&me,e);
    else s = fips_path_broken_decode(r,&pb,&decoded_coords,e);
    if (s == FIPS_OK) {
        if (which == 0) { EQ(cr.dest_addr,dest); EQ(cr.reporter,mine); }
        else if (which == 3) { EQ(me.dest_addr,dest); EQ(me.reporter,mine); }
        else { EQ(pb.dest_addr,dest); EQ(pb.reporter,mine); CHECK((pb.last_known_coords != NULL) == (which == 2)); if (which == 2) CHECK(fips_tree_coordinate_equal(pb.last_known_coords,&coords[0])); }
    } else {
        EQ(cr.dest_addr,addr(99)); EQ(cr.reporter,addr(98));
        EQ(me.dest_addr,addr(99)); EQ(me.reporter,addr(98)); CHECK(me.mtu == 77);
        EQ(pb.dest_addr,addr(99)); EQ(pb.reporter,addr(98)); CHECK(!pb.last_known_coords);
    }
    return s;
}
static void roundtrip(unsigned which)
{
    reset(); encode_signal(which,1400);
    size_t n = which == 0 ? 38 : which == 2 ? 88 : 40;
    CHECK(writer.pos == n && bytes[0] == 0 && bytes[1] == 4 && bytes[2] == n-4 && bytes[3] == 0);
    CHECK(bytes[4] == (which == 0 ? 0x20 : which == 3 ? 0x22 : 0x21));
    fips_reader r; fips_reader_init(&r,bytes+5,n-5); OK(decode_signal(which,&r,NULL)); CHECK(r.pos == n-5);
}
static void test_routing_signal_type_roundtrip(void)
{ for (uint8_t i = 0x20; i <= 0x22; ++i) { fips_routing_signal_type t; CHECK(fips_routing_signal_type_from_byte(i,&t)); CHECK(fips_routing_signal_type_to_byte(t) == i); } }
static void test_routing_signal_type_values_and_invalid(void)
{
    CHECK(FIPS_ROUTING_CoordsRequired == 0x20 && FIPS_ROUTING_PathBroken == 0x21 && FIPS_ROUTING_MtuExceeded == 0x22);
    for (unsigned i = 0; i <= 255; ++i) { fips_routing_signal_type t = FIPS_ROUTING_PathBroken; bool ok = fips_routing_signal_type_from_byte((uint8_t)i,&t); CHECK(ok == (i >= 0x20 && i <= 0x22)); if (!ok) CHECK(t == FIPS_ROUTING_PathBroken); }
}
static void test_routing_signal_type_display(void)
{ CHECK(!strcmp(fips_routing_signal_type_name(FIPS_ROUTING_CoordsRequired),"CoordsRequired")); CHECK(!strcmp(fips_routing_signal_type_name(FIPS_ROUTING_PathBroken),"PathBroken")); CHECK(!strcmp(fips_routing_signal_type_name(FIPS_ROUTING_MtuExceeded),"MtuExceeded")); }
static void test_coords_required(void)
{ fips_coords_required p = fips_coords_required_new(addr(1),addr(2)); EQ(p.dest_addr,addr(1)); EQ(p.reporter,addr(2)); }
static void test_path_broken(void)
{ PATH(0,2,0); fips_path_broken p = fips_path_broken_new(addr(2),addr(3)); CHECK(!p.last_known_coords); fips_path_broken_with_last_coords(&p,&coords[0]); EQ(p.dest_addr,addr(2)); EQ(p.reporter,addr(3)); CHECK(p.last_known_coords == &coords[0]); }
static void test_coords_required_encode_decode(void) { roundtrip(0); }
static void test_path_broken_encode_decode_no_coords(void) { roundtrip(1); }
static void test_path_broken_encode_decode_with_coords(void) { roundtrip(2); }
static void too_short(unsigned which)
{
    reset(); encode_signal(which,1400); size_t n = writer.pos;
    memset(&decoded_coords,0xa5,sizeof(decoded_coords));
    for (size_t i = 0; i < n-5; ++i) {
        fips_reader r; fips_reader_init(&r,bytes+5,i); fips_proto_error e;
        CHECK(decode_signal(which,&r,&e) == FIPS_ERR_RANGE); CHECK(r.pos == 0 && e.variant == FIPS_PROTO_MessageTooShort);
        size_t expected = which == 0 ? 33 : 35, got = i;
        if (which == 2 && i >= 35) { expected = 50; got = i-33; }
        CHECK(e.as.message_too_short.expected == expected && e.as.message_too_short.got == got);
    }
    const uint8_t *p = (const uint8_t *)&decoded_coords;
    for (size_t i = 0; i < sizeof(decoded_coords); ++i) CHECK(p[i] == 0xa5);
}
static void test_coords_required_decode_too_short(void) { too_short(0); }
static void test_path_broken_decode_too_short(void) { too_short(1); too_short(2); }
static void test_mtu_exceeded_encode_size(void) { roundtrip(3); CHECK(writer.pos == 4 + FIPS_MTU_EXCEEDED_SIZE); }
static void test_mtu_exceeded_encode_decode(void)
{ roundtrip(3); fips_reader r; fips_mtu_exceeded p; fips_reader_init(&r,bytes+5,35); OK(fips_mtu_exceeded_decode(&r,&p,NULL)); CHECK(p.mtu == 1400); }
static void test_mtu_exceeded_decode_too_short(void) { too_short(3); }
static void test_mtu_exceeded_boundary_mtu_values(void)
{
    const uint16_t values[] = {0,1280,1500,UINT16_MAX};
    for (size_t i = 0; i < 4; ++i) { reset(); encode_signal(3,values[i]); fips_reader r; fips_mtu_exceeded p; fips_reader_init(&r,bytes+5,35); OK(fips_mtu_exceeded_decode(&r,&p,NULL)); CHECK(p.mtu == values[i]); }
}
static void routing_signals_with_a_reserved_sig_flags_bit_set_are_refused(void)
{
    for (unsigned which = 0; which < 4; ++which) {
        reset(); encode_signal(which,1280); size_t n = writer.pos;
        for (unsigned bit = 0; bit < 8; ++bit) {
            bytes[5] = (uint8_t)(1u << bit); fips_reader r; fips_proto_error e;
            fips_reader_init(&r,bytes+5,n-5); CHECK(decode_signal(which,&r,&e) == FIPS_ERR_RANGE);
            CHECK(r.pos == 0 && e.variant == FIPS_PROTO_Malformed && !strcmp(e.as.malformed,"routing signal sig_flags set"));
        }
    }
}
static void routing_signals_with_bytes_after_the_body_are_refused(void)
{
    for (unsigned which = 0; which < 4; ++which) {
        reset(); encode_signal(which,1280); size_t n = writer.pos; bytes[n] = 0;
        fips_reader r; fips_proto_error e; fips_reader_init(&r,bytes+5,n-4);
        CHECK(decode_signal(which,&r,&e) == FIPS_ERR_RANGE);
        CHECK(r.pos == 0 && e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.max == n-5 && e.as.message_too_long.got == n-4);
    }
}
/* C boundary tests beyond the upstream named ports. */
static void buffer_refusals_are_atomic(void)
{
    for (unsigned which = 0; which < 4; ++which) {
        reset(); encode_signal(which,1280); size_t n = writer.pos;
        for (size_t cap = 0; cap < n; ++cap) {
            uint8_t buf[128]; memset(buf,0xa5,sizeof(buf)); fips_writer w; fips_writer_init(&w,buf,cap);
            fips_coords_required cr = fips_coords_required_new(dest,mine);
            fips_path_broken pb = fips_path_broken_new(dest,mine); if (which == 2) pb.last_known_coords = &coords[0];
            fips_mtu_exceeded me = fips_mtu_exceeded_new(dest,mine,1280);
            fips_proto_error e; fips_status s;
            if (which == 0) s = fips_coords_required_encode(&cr,&w,&e);
            else if (which == 3) s = fips_mtu_exceeded_encode(&me,&w,&e);
            else s = fips_path_broken_encode(&pb,&w,&e);
            CHECK(s == FIPS_ERR_RANGE && w.pos == 0 && e.variant == FIPS_PROTO_MessageTooLong);
            for (size_t i = 0; i < sizeof(buf); ++i) CHECK(buf[i] == 0xa5);
        }
    }
    reset(); fips_writer_init(&writer,bytes,0); fips_error_synth s;
    CHECK(fips_router_synth_routing_error(&router,&dest,&source,&mine,&view,fips_test_instant(0),64,&writer,&s,NULL) == FIPS_ERR_RANGE);
    CHECK(router.error_limiter.inner.len == 0 && writer.pos == 0);
    fips_writer_init(&writer,bytes,sizeof(bytes)); CHECK(synth(false,0).has_action);
    fips_writer_init(&writer,NULL,0); CHECK(!synth(true,0).has_action); /* shared gate, no buffer needed */
    fips_session_datagram dg = datagram(5,false); fips_next_hop nh = {addr(3),1400};
    fips_route_outcome out = {.kind = FIPS_ROUTE_NoRoute};
    CHECK(fips_router_route(&router,&dg,&mine,false,&nh,&view,&writer,&out,NULL) == FIPS_ERR_RANGE);
    CHECK(out.kind == FIPS_ROUTE_NoRoute && writer.pos == 0 && congestion_reads == 0);
}
static void bloom_miss_falls_back_to_shell_resolved_hop(void)
{
    selection_setup(); peer(0,0x30,false,1,&coords[2]); fips_node_addr out = addr(99);
    CHECK(!choose(&out)); EQ(out,addr(99));
    CHECK(route(5,false,false,false,1500).kind == FIPS_ROUTE_NoRoute);
    CHECK(route(5,false,true,false,1500).kind == FIPS_ROUTE_Forward);
}
static void classify_all_six_routes(void)
{
    reset(); mine = addr(0x10); dest = addr(0x50); fips_node_addr chosen = addr(0x20);
    PATH(0,0x10,0x20,0); PATH(1,0x50,0x20,0); PATH(2,0x20,0x10,0);
    CHECK(fips_classify_forward(&dest,&dest,&mine,&coords[0],NULL,NULL) == FIPS_ROUTE_DirectPeer);
    CHECK(fips_classify_forward(&dest,&chosen,&mine,&coords[0],NULL,NULL) == FIPS_ROUTE_TreeUp);
    PATH(0,0x10,0);
    CHECK(fips_classify_forward(&dest,&chosen,&mine,&coords[0],&coords[1],&coords[2]) == FIPS_ROUTE_TreeDown);
    CHECK(fips_classify_forward(&dest,&chosen,&mine,&coords[0],NULL,&coords[2]) == FIPS_ROUTE_TreeDownCross);
    CHECK(fips_classify_forward(&dest,&chosen,&mine,&coords[0],&coords[1],NULL) == FIPS_ROUTE_CrosslinkDescend);
    CHECK(fips_classify_forward(&dest,&chosen,&mine,&coords[0],NULL,NULL) == FIPS_ROUTE_CrosslinkAscend);
    const char *names[] = {"tree_up","tree_down","tree_down_cross","crosslink_descend","crosslink_ascend","direct_peer"};
    for (unsigned i = 0; i < 6; ++i) CHECK(!strcmp(fips_route_class_name((fips_route_class)i),names[i]));
}
static void malformed_and_mutated_fields_by_offset(void)
{
    for (unsigned which = 0; which < 4; ++which) {
        reset(); encode_signal(which,1280); size_t n = writer.pos;
        /* Truncation at every offset is checked by too_short above. Mutating
         * every body byte also distinguishes opaque addresses/MTU/coordinates
         * (valid) from reserved flags and a corrupt coordinate count. */
        for (size_t offset = 5; offset < n; ++offset) {
            uint8_t saved = bytes[offset]; bytes[offset] ^= 0x80;
            fips_reader r; fips_reader_init(&r,bytes+5,n-5);
            fips_coords_required cr; fips_mtu_exceeded me; fips_path_broken pb;
            fips_proto_error e; fips_status s;
            if (which == 0) s = fips_coords_required_decode(&r,&cr,&e);
            else if (which == 3) s = fips_mtu_exceeded_decode(&r,&me,&e);
            else s = fips_path_broken_decode(&r,&pb,&decoded_coords,&e);
            bool bad_count = (which == 1 || which == 2) && (offset == 38 || offset == 39);
            CHECK((s != FIPS_OK) == (offset == 5 || bad_count));
            if (s != FIPS_OK) CHECK(r.pos == 0);
            bytes[offset] = saved;
        }
        /* Existing cursor offset, successful error preservation and append. */
        fips_reader r; fips_reader_init(&r,bytes,n); r.pos = 5;
        fips_proto_error e = {.variant = FIPS_PROTO_TtlExpired};
        OK(decode_signal(which,&r,&e)); CHECK(r.pos == n && e.variant == FIPS_PROTO_TtlExpired);
    }
    reset(); encode_signal(2,1280); bytes[5] = 1; bytes[39] = 0xff;
    fips_reader r; fips_path_broken pb; fips_proto_error e;
    fips_reader_init(&r,bytes+5,writer.pos-5);
    CHECK(fips_path_broken_decode(&r,&pb,&decoded_coords,&e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooShort); /* coords precedes flags in Rust */
}
static void maximum_coordinate_wire_domain(void)
{
    static uint8_t wire[FIPS_ROUTING_MAX_WIRE_SIZE];
    reset();
    for (size_t i = 0; i < FIPS_COORD_MAX_ENTRIES; ++i)
        coords[0].entries[i] = fips_coord_entry_addr_only(minted((uint32_t)i));
    coords[0].len = FIPS_COORD_MAX_ENTRIES;
    fips_path_broken pb = fips_path_broken_new(dest,mine);
    fips_path_broken_with_last_coords(&pb,&coords[0]);
    fips_writer w; fips_writer_init(&w,wire,sizeof(wire));
    fips_proto_error e;
    wire[0] = 0xa5;
    CHECK(fips_path_broken_encode(&pb,&w,&e) == FIPS_ERR_RANGE);
    CHECK(e.variant == FIPS_PROTO_MessageTooLong && w.pos == 0 && wire[0] == 0xa5);
    coords[0].len = (UINT16_MAX - 36u) / 16u + 1u;
    CHECK(fips_path_broken_encode(&pb,&w,&e) == FIPS_ERR_RANGE && w.pos == 0);
    --coords[0].len;
    OK(fips_path_broken_encode(&pb,&w,NULL)); CHECK(w.pos - 4 <= UINT16_MAX);
    CHECK(w.pos-4 == (size_t)(wire[2] | ((unsigned)wire[3]<<8)));
    fips_reader r; fips_reader_init(&r,wire+5,w.pos-5);
    OK(fips_path_broken_decode(&r,&pb,&decoded_coords,NULL));
    CHECK(pb.last_known_coords == &decoded_coords && fips_tree_coordinate_equal(&coords[0],&decoded_coords));
    fips_reader_init(&r,wire+5,w.pos-6); decoded_coords.len = 7;
    CHECK(fips_path_broken_decode(&r,&pb,&decoded_coords,NULL) == FIPS_ERR_RANGE);
    CHECK(decoded_coords.len == 7 && r.pos == 0);
}
static void ieee_costs_and_capacity_synthesis(void)
{
    selection_setup(); peer(0,0x30,true,INFINITY,&coords[2]); peer(1,0x20,true,INFINITY,&coords[2]);
    fips_node_addr out; CHECK(choose(&out)); EQ(out,addr(0x20));
    peers[0].link_cost = NAN; CHECK(choose(&out)); EQ(out,addr(0x30)); /* Rust: first NaN sticks */
    reset();
    for (uint32_t i = 0; i < FIPS_PER_ADDR_MAX_ENTRIES; ++i) {
        fips_node_addr a = minted(i);
        CHECK(fips_routing_error_rate_limiter_check(&router.error_limiter,&a,fips_test_instant(0)) == FIPS_LIMIT_Admit);
    }
    CHECK(synth(true,0).verdict == FIPS_LIMIT_AdmitAtCapacity);
    writer.pos = 0; CHECK(synth(false,0).verdict == FIPS_LIMIT_AdmitAtCapacity);
    CHECK(router.error_limiter.inner.len == FIPS_PER_ADDR_MAX_ENTRIES);
}
int main(void)
{
    /* Generated registration below retains the upstream test names. */
    ttl_zero_drops_as_exhausted();
    ttl_zero_to_self_delivers_local();
    ttl_one_transit_drops_before_forwarding();
    ttl_two_transit_forwards_at_one();
    destination_is_self_delivers_local();
    transit_without_next_hop_is_no_route();
    forward_decrements_ttl_and_folds_link_mtu();
    forward_keeps_smaller_datagram_path_mtu();
    outgoing_ce_set_by_incoming_ce();
    outgoing_ce_set_by_local_congestion();
    outgoing_ce_clear_when_neither_signal();
    cached_coords_reads_the_view_table();
    candidate_selection_is_independent_of_peer_enumeration_order();
    candidate_selection_filters_bloom_and_missing_coords();
    candidate_must_be_strictly_closer_than_self();
    candidate_ordering_is_cost_then_distance_then_address();
    synth_uses_pathbroken_when_coords_cached();
    synth_uses_coords_required_when_not_cached();
    synth_rate_limit_gate_suppresses_second_call();
    synth_mtu_exceeded_carries_bottleneck_and_targets_source();
    synth_mtu_exceeded_rate_limit_gate_suppresses_second_call();
    route_drops_for_hop_limit_exactly_when_can_forward_is_false();
    test_first_send_allowed();
    test_rapid_sends_rate_limited();
    test_different_destinations_independent();
    test_send_allowed_after_interval();
    test_cleanup_removes_old_entries();
    test_cleanup_preserves_recent_entries();
    test_with_interval_custom_rate();
    the_map_stays_bounded_when_a_sender_mints_distinct_destination_keys();
    an_admission_at_capacity_still_sends_rather_than_going_silent();
    the_map_scan_does_not_run_once_per_admitted_destination();
    the_map_scan_still_runs_once_a_sweep_interval_has_passed();
    test_routing_signal_type_roundtrip();
    test_routing_signal_type_values_and_invalid();
    test_routing_signal_type_display();
    test_coords_required();
    test_path_broken();
    test_coords_required_encode_decode();
    test_path_broken_encode_decode_no_coords();
    test_path_broken_encode_decode_with_coords();
    test_coords_required_decode_too_short();
    test_path_broken_decode_too_short();
    test_mtu_exceeded_encode_size();
    test_mtu_exceeded_encode_decode();
    test_mtu_exceeded_decode_too_short();
    test_mtu_exceeded_boundary_mtu_values();
    routing_signals_with_a_reserved_sig_flags_bit_set_are_refused();
    routing_signals_with_bytes_after_the_body_are_refused();
    buffer_refusals_are_atomic(); bloom_miss_falls_back_to_shell_resolved_hop(); classify_all_six_routes();
    malformed_and_mutated_fields_by_offset(); maximum_coordinate_wire_domain(); ieee_costs_and_capacity_synthesis();
    puts("routing: 49 upstream tests + 6 C boundary groups passed");
    return 0;
}
