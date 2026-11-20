#include <fips/proto/fsp/mod.h>
#include <fips/identity/address.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
static fips_node_addr addr(uint8_t v) { fips_node_addr a = {{0}}; a.bytes[0] = v; return a; }
static int core(void)
{
    fips_fsp_action storage[4];
    fips_fsp_action_list a = {storage, 0, 4};
    fips_node_addr dest = addr(5);
    /* plan_coords_required_lookup_gated_on_identity */
    CHECK(fips_fsp_plan_coords_required_lookup(dest, false, &a) == FIPS_OK && !a.len);
    CHECK(fips_fsp_plan_coords_required_lookup(dest, true, &a) == FIPS_OK && a.len == 1);
    CHECK(a.items[0].kind == FIPS_FSP_InitiateLookup && !memcmp(a.items[0].dest.bytes, dest.bytes, 16));
    /* plan_path_broken_removes_an_unverified_entry_then_looks_it_up;
     * plan_path_broken_keeps_a_verified_entry_below_quorum_and_looks_it_up;
     * plan_path_broken_demotes_a_verified_entry_at_quorum_then_looks_it_up */
    for (unsigned verified = 0; verified < 2; ++verified) for (unsigned reached = 0; reached < 2; ++reached) {
        a.len = 0;
        CHECK(fips_fsp_plan_path_broken(dest, verified != 0, (fips_fsp_quorum_verdict){reached ? FIPS_FSP_Reached : FIPS_FSP_Below, reached ? 2 : 1}, &a) == FIPS_OK);
        bool change = !verified || reached;
        CHECK(a.len == (change ? 2u : 1u));
        if (change) CHECK(a.items[0].kind == (verified ? FIPS_FSP_DemoteCoords : FIPS_FSP_InvalidateCoords));
        CHECK(a.items[a.len - 1].kind == FIPS_FSP_InitiateLookup);
        for (size_t i = 0; i < a.len; ++i) CHECK(!memcmp(a.items[i].dest.bytes, dest.bytes, 16));
    }
    /* plan_path_mtu_tighten_emits_only_when_tighter; should_apply_path_mtu_keeps_tighter */
    const uint16_t existing[] = {0, 1500, 1300, 1400};
    fips_ipv6_address fa = fips_ipv6_address_from_node_addr(&dest);
    for (size_t i = 0; i < 4; ++i) {
        a.len = 0;
        CHECK(fips_fsp_should_apply_path_mtu(i != 0, existing[i], 1400) == (i < 2));
        CHECK(fips_fsp_plan_path_mtu_tighten(fa, i != 0, existing[i], 1400, &a) == FIPS_OK && a.len == (i < 2 ? 1u : 0u));
        if (a.len) CHECK(a.items[0].kind == FIPS_FSP_TightenPathMtuLookup && a.items[0].mtu == 1400 && !memcmp(a.items[0].fips_addr.bytes, fa.bytes, 16));
    }
    /* C append/capacity atomicity: a two-action plan cannot partially append. */
    a.len = 3; memset(&storage[3], 0xcc, sizeof(storage[3]));
    fips_fsp_action saved = storage[3];
    CHECK(fips_fsp_plan_path_broken(dest, false, (fips_fsp_quorum_verdict){FIPS_FSP_Reached, 2}, &a) == FIPS_ERR_RANGE);
    CHECK(a.len == 3 && !memcmp(&saved, &storage[3], sizeof(saved)));
    fips_fsp_action_list empty = {NULL, 0, 0};
    CHECK(fips_fsp_plan_coords_required_lookup(dest, false, &empty) == FIPS_OK);
    CHECK(fips_fsp_plan_coords_required_lookup(dest, true, &empty) == FIPS_ERR_RANGE);
    /* push_bounded_pending_drops_oldest_at_capacity */
    fips_fsp_bytes packets[2];
    fips_fsp_pending_queue q = {packets, 0, 2};
    const uint8_t data[] = {1, 2, 3};
    for (size_t i = 0; i < 3; ++i) CHECK(fips_fsp_push_bounded_pending(&q, (fips_fsp_bytes){data + i, 1}) == FIPS_OK);
    CHECK(q.len == 2 && q.items[0].data[0] == 2 && q.items[1].data[0] == 3);
    q = (fips_fsp_pending_queue){NULL, 0, 0};
    CHECK(fips_fsp_push_bounded_pending(&q, (fips_fsp_bytes){data, 1}) == FIPS_ERR_RANGE && !q.len);
    return 0;
}
static int quorum(void)
{
    fips_fsp_quorum_entry storage[100];
    fips_fsp_link_quorum q;
    fips_fsp_quorum_verdict v;
    const uint64_t t = 1000000, window = FIPS_FSP_QUORUM_WINDOW_MS;
    /* the_quorum_needs_two_links; one_report_stays_below_the_quorum */
    CHECK(FIPS_FSP_QUORUM_LINKS == 2 && window == 15000);
    fips_fsp_link_quorum_init(&q, storage, 100);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t, &v) == FIPS_OK && v.kind == FIPS_FSP_Below && v.distinct == 1);
    /* two_distinct_links_inside_the_window_reach_the_quorum */
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + window, &v) == FIPS_OK && v.kind == FIPS_FSP_Reached && !q.len);
    /* the_same_link_repeated_never_reaches_the_quorum */
    fips_fsp_link_quorum_init(&q, storage, 100);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t + i, &v) == FIPS_OK && v.kind == FIPS_FSP_Below && v.distinct == 1);
    CHECK(storage[0].first_seen_ms == t);
    /* links_further_apart_than_the_window_do_not_combine */
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + window + 1, &v) == FIPS_OK && v.kind == FIPS_FSP_Below);
    /* a_repeat_does_not_refresh_its_links_first_seen_time */
    fips_fsp_link_quorum_init(&q, storage, 100);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t, &v) == FIPS_OK);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t + window, &v) == FIPS_OK);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + window + 1, &v) == FIPS_OK && v.kind == FIPS_FSP_Below);
    /* reaching_the_quorum_clears_the_destination */
    fips_fsp_link_quorum_init(&q, storage, 100);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t, &v) == FIPS_OK);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + 1, &v) == FIPS_OK && v.kind == FIPS_FSP_Reached && q.len == 0);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t + 2, &v) == FIPS_OK && v.kind == FIPS_FSP_Below);
    /* clear_forgets_the_reports_about_a_destination */
    fips_fsp_link_quorum_clear(&q, &(fips_node_addr){{1}});
    CHECK(!q.len);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + 3, &v) == FIPS_OK && v.kind == FIPS_FSP_Below);
    /* destinations_are_counted_separately */
    CHECK(fips_fsp_link_quorum_record(&q, addr(2), addr(10), t + 3, &v) == FIPS_OK && v.kind == FIPS_FSP_Below && q.len == 2);
    /* the_sweep_drops_destinations_with_no_live_link */
    fips_fsp_link_quorum_init(&q, storage, 100);
    for (unsigned i = 0; i < 100; ++i) CHECK(fips_fsp_link_quorum_record(&q, addr((uint8_t)i), addr(10), t, &v) == FIPS_OK);
    CHECK(q.len == 100);
    CHECK(fips_fsp_link_quorum_record(&q, addr(200), addr(10), t + window + 1, &v) == FIPS_OK && q.len == 1);
    /* C capacity refusal is fail-closed and transactional, including timestamp. */
    fips_fsp_link_quorum_init(&q, storage, 1);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t, &v) == FIPS_OK);
    fips_fsp_quorum_entry saved = storage[0];
    fips_fsp_link_quorum before = q;
    v = (fips_fsp_quorum_verdict){FIPS_FSP_Reached, 77};
    CHECK(fips_fsp_link_quorum_record(&q, addr(2), addr(11), t + window, &v) == FIPS_ERR_RANGE);
    CHECK(!memcmp(&before, &q, sizeof(q)) && !memcmp(&saved, storage, sizeof(saved)) && v.distinct == 77);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), t + window, &v) == FIPS_OK && v.kind == FIPS_FSP_Reached);
    /* Backward/near-overflow clocks use saturating subtraction, no addition. */
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), UINT64_MAX, &v) == FIPS_OK);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(11), 0, &v) == FIPS_OK && v.kind == FIPS_FSP_Reached);
    fips_fsp_link_quorum_init(&q, NULL, 0);
    CHECK(fips_fsp_link_quorum_record(&q, addr(1), addr(10), t, &v) == FIPS_ERR_RANGE);
    return 0;
}
static int catalog_flags(void)
{
    const char *names[] = {"DataPacket", "SenderReport", "ReceiverReport", "PathMtuNotification", "CoordsWarmup"};
    /* test_session_message_type_{roundtrip,invalid,new_values,display};
     * session_flags_name_no_bits_and_keep_the_byte_as_it_arrived;
     * test_fsp_flags_{default,roundtrip,individual_bits,ignores_reserved_bits};
     * test_fsp_inner_flags_{default,roundtrip,ignores_reserved}; test_flag_bits_distinct */
    for (unsigned b = 0; b < 256; ++b) {
        fips_session_message_type type;
        bool known = b >= 0x10 && b <= 0x14;
        CHECK((fips_session_message_type_from_u8((uint8_t)b, &type, NULL) == FIPS_OK) == known);
        if (known) CHECK((unsigned)type == b && !strcmp(fips_session_message_type_name(type), names[b - 0x10]));
        fips_fsp_session_flags sf = fips_fsp_session_flags_from_byte((uint8_t)b); CHECK(fips_fsp_session_flags_to_byte(sf) == b);
        fips_fsp_flags f = fips_fsp_flags_from_byte((uint8_t)b);
        CHECK(f.coords_present == ((b & 1) != 0) && f.key_epoch == ((b & 2) != 0) && f.unencrypted == ((b & 4) != 0));
        CHECK(fips_fsp_flags_to_byte(f) == (b & 7));
        fips_fsp_inner_flags inner = fips_fsp_inner_flags_from_byte((uint8_t)b);
        CHECK(inner.spin_bit == ((b & 1) != 0) && fips_fsp_inner_flags_to_byte(inner) == (b & 1));
        CHECK((fips_fsp_check_inner((uint8_t)b, NULL) == FIPS_OK) == (b <= 1));
    }
    CHECK(!(FIPS_FSP_FLAG_CP & FIPS_FSP_FLAG_K) && !(FIPS_FSP_FLAG_CP & FIPS_FSP_FLAG_U) && !(FIPS_FSP_FLAG_K & FIPS_FSP_FLAG_U));
    CHECK(fips_fsp_session_flags_to_byte(fips_fsp_session_flags_new()) == 0);
    CHECK(fips_fsp_flags_to_byte((fips_fsp_flags){0}) == 0 && fips_fsp_inner_flags_to_byte((fips_fsp_inner_flags){0}) == 0);
    return 0;
}
/* One adapter for the three different upstream message types. Test outputs are
 * normalized only after success; sentinel outputs exercise failure atomicity. */
typedef struct decoded { uint8_t flags; fips_fsp_coords src, dest; fips_fsp_bytes hs; } decoded;
static fips_status decode(unsigned phase, fips_reader *r, bool strict, decoded *out, fips_proto_error *e)
{
    decoded d = {0};
    if (phase == 1) {
        fips_fsp_session_setup m;
        fips_status s = strict ? fips_fsp_session_setup_decode_strict(r, &m, e) : fips_fsp_session_setup_decode(r, &m, e);
        if (s != FIPS_OK) return s;
        d = (decoded){m.flags.bits, m.src_coords, m.dest_coords, m.handshake_payload};
    } else if (phase == 2) {
        fips_fsp_session_ack m;
        fips_status s = strict ? fips_fsp_session_ack_decode_strict(r, &m, e) : fips_fsp_session_ack_decode(r, &m, e);
        if (s != FIPS_OK) return s;
        d = (decoded){m.flags, m.src_coords, m.dest_coords, m.handshake_payload};
    } else {
        fips_fsp_session_msg3 m;
        fips_status s = strict ? fips_fsp_session_msg3_decode_strict(r, &m, e) : fips_fsp_session_msg3_decode(r, &m, e);
        if (s != FIPS_OK) return s;
        d.flags = m.flags; d.hs = m.handshake_payload;
    }
    *out = d; return FIPS_OK;
}
static fips_status encode(unsigned phase, const decoded *d, fips_writer *w, fips_proto_error *e)
{
    if (phase == 1) { fips_fsp_session_setup m = {d->src, d->dest, {d->flags}, d->hs}; return fips_fsp_session_setup_encode(&m, w, e); }
    if (phase == 2) { fips_fsp_session_ack m = {d->src, d->dest, d->flags, d->hs}; return fips_fsp_session_ack_encode(&m, w, e); }
    fips_fsp_session_msg3 m = {d->flags, d->hs}; return fips_fsp_session_msg3_encode(&m, w, e);
}
static int handshakes(void)
{
    uint8_t src[11 * 16] = {0}, dest[5 * 16] = {0}, hs[82], buf[512], other[512];
    for (size_t i = 0; i < 11; ++i) src[16 * i] = (uint8_t)i;
    for (size_t i = 0; i < 5; ++i) dest[16 * i] = (uint8_t)(20 + i);
    /* test_session_setup; test_session_{setup,ack,msg3}_encode_decode;
     * test_session_setup_no_handshake; test_session_setup_deep_coords;
     * test_session_msg3_empty_handshake; test_session_{setup,ack,msg3}_decode_too_short;
     * strict_handshake_decoders_accept_every_conforming_message */
    for (unsigned phase = 1; phase <= 3; ++phase) for (unsigned empty = 0; empty < 2; ++empty) {
        size_t hs_len = empty ? 0 : phase == 1 ? 82 : phase == 2 ? 33 : 73;
        memset(hs, phase == 1 ? 0xaa : phase == 2 ? 0xbb : 0xcc, sizeof(hs));
        decoded input = {0, {src, phase == 1 ? 11 : 3}, {dest, 5}, {empty ? NULL : hs, hs_len}}, d;
        fips_writer w;
        fips_writer_init(&w, buf, sizeof(buf));
        CHECK(encode(phase, &input, &w, NULL) == FIPS_OK);
        size_t total = w.pos, body = total - 4;
        CHECK(buf[0] == phase && !buf[1] && !buf[4]);
        fips_fsp_common_prefix p;
        CHECK(fips_fsp_common_prefix_parse(buf, total, &p) && !p.version && p.phase == phase && p.payload_len == body);
        CHECK(fips_fsp_check_prefix(&p, body, NULL) == FIPS_OK);
        fips_fsp_action emitted;
        fips_fsp_action_list actions = {&emitted, 0, 1};
        CHECK(fips_fsp_receive(addr(1), buf, total, &actions) == FIPS_OK && actions.len == 1);
        CHECK(emitted.kind == (phase == 1 ? FIPS_FSP_ReceiveSetup : phase == 2 ? FIPS_FSP_ReceiveAck : FIPS_FSP_ReceiveMsg3));
        for (size_t n = 0; n < total; ++n) {
            actions.len = 0;
            CHECK(fips_fsp_receive(addr(1), n ? buf : NULL, n, &actions) == FIPS_OK && emitted.kind == FIPS_FSP_DropFrame);
        }
        fips_reader r;
        fips_reader_init(&r, buf + 4, body);
        fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
        CHECK(decode(phase, &r, true, &d, &e) == FIPS_OK && r.pos == body && !d.flags && d.hs.len == hs_len);
        CHECK(e.variant == FIPS_PROTO_InvalidSignature && (!hs_len || !memcmp(d.hs.data, hs, hs_len)));
        if (phase != 3) {
            CHECK(d.src.count == input.src.count && d.dest.count == input.dest.count);
            CHECK(!memcmp(d.src.addrs, src, d.src.count * 16) && !memcmp(d.dest.addrs, dest, d.dest.count * 16));
        }
        fips_writer_init(&w, other, sizeof(other)); CHECK(encode(phase, &d, &w, NULL) == FIPS_OK && w.pos == total && !memcmp(buf, other, total));
        /* Every short body byte offset: all decoders preserve output/cursor. */
        for (size_t n = 0; n < body; ++n) {
            fips_reader_init(&r, n ? buf + 4 : NULL, n);
            memset(&d, 0xcc, sizeof(d)); decoded saved = d;
            CHECK(decode(phase, &r, false, &d, &e) == FIPS_ERR_RANGE && !r.pos && !memcmp(&d, &saved, sizeof(d)));
            CHECK(e.variant == FIPS_PROTO_MessageTooShort && e.as.message_too_short.got == n && e.as.message_too_short.expected > n);
            CHECK(decode(phase, &r, true, &d, NULL) == FIPS_ERR_RANGE && !r.pos);
        }
        /* Every insufficient writer capacity, including cumulative offsets. */
        for (size_t n = 0; n < total; ++n) {
            memset(other, 0xcc, sizeof(other)); fips_writer_init(&w, n ? other : NULL, n);
            CHECK(encode(phase, &input, &w, &e) == FIPS_ERR_RANGE && !w.pos && other[0] == 0xcc);
            CHECK(e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.got == total && e.as.message_too_long.max == n);
        }
        fips_writer_init(&w, other, total); CHECK(fips_writer_write_u8(&w, 7, NULL) == FIPS_OK);
        CHECK(encode(phase, &input, &w, &e) == FIPS_ERR_RANGE && w.pos == 1 && other[0] == 7 && e.as.message_too_long.got == total + 1);
        /* strict_handshake_decoders_refuse_any_body_flag_bit;
         * check_prefix_refuses_any_handshake_prefix_flag_bit */
        for (unsigned bit = 0; bit < 8; ++bit) {
            buf[4] = (uint8_t)(1u << bit); fips_reader_init(&r, buf + 4, body);
            CHECK(decode(phase, &r, true, &d, &e) == FIPS_ERR_RANGE && !r.pos && e.variant == FIPS_PROTO_Malformed);
            CHECK(decode(phase, &r, false, &d, NULL) == FIPS_OK && d.flags == buf[4]);
            buf[4] = 0; buf[1] = (uint8_t)(1u << bit);
            CHECK(fips_fsp_common_prefix_parse(buf, total, &p) && fips_fsp_check_prefix(&p, body, NULL) == FIPS_ERR_RANGE);
            buf[1] = 0;
        }
        /* strict_handshake_decoders_refuse_bytes_after_the_noise_message;
         * the_shared_handshake_decoders_still_read_coordinates_past_reserved_flags_and_trailing_bytes */
        buf[total] = 0; fips_reader_init(&r, buf + 4, body + 1);
        CHECK(decode(phase, &r, true, &d, &e) == FIPS_ERR_RANGE && !r.pos && e.variant == FIPS_PROTO_MessageTooLong && e.as.message_too_long.max == body);
        buf[4] = 255; CHECK(decode(phase, &r, false, &d, NULL) == FIPS_OK && r.pos == body && d.flags == 255);
        if (phase != 3) CHECK(d.src.count == input.src.count && d.dest.count == input.dest.count);
        buf[4] = 0;
        /* check_prefix_refuses_a_handshake_or_signal_payload_len_that_is_not_the_rest */
        CHECK(fips_fsp_common_prefix_parse(buf, total, &p));
        CHECK(fips_fsp_check_prefix(&p, body - 1, NULL) == FIPS_ERR_RANGE && fips_fsp_check_prefix(&p, body + 1, NULL) == FIPS_ERR_RANGE && fips_fsp_check_prefix(&p, 0, NULL) == FIPS_ERR_RANGE);
    }
    /* Bad counts/handshake lengths; no guessed Noise size imposed. */
    decoded input = {0, {src, 1}, {dest, 1}, {hs, 1}}, d;
    fips_writer w;
    fips_writer_init(&w, buf, sizeof(buf)); CHECK(encode(1, &input, &w, NULL) == FIPS_OK);
    buf[5] = 0; buf[6] = 0;
    fips_reader r; fips_reader_init(&r, buf + 4, w.pos - 4);
    fips_proto_error e;
    CHECK(decode(1, &r, false, &d, &e) == FIPS_ERR_RANGE && e.variant == FIPS_PROTO_Malformed && !r.pos);
    input.src.count = UINT16_MAX + (size_t)1; fips_writer_init(&w, buf, sizeof(buf));
    CHECK(encode(1, &input, &w, NULL) == FIPS_ERR_RANGE && !w.pos);
    input.src.count = 1; input.hs.len = UINT16_MAX; CHECK(encode(1, &input, &w, NULL) == FIPS_ERR_RANGE && !w.pos);
    input.hs.len = SIZE_MAX; CHECK(encode(3, &input, &w, NULL) == FIPS_ERR_RANGE && !w.pos);
    return 0;
}
static int headers(void)
{
    uint8_t header[FIPS_FRAMING_FSP_HEADER_SIZE], packet[1300], inner[16];
    fips_fsp_common_prefix p;
    fips_fsp_encrypted_header h;
    fips_writer w;
    /* test_wire_sizes; test_common_prefix_parse_{established,handshake,error_signal};
     * test_common_prefix_too_short; test_all_message_types_through_prefix;
     * test_build_fsp_handshake_prefix_msg{1,2,3}; test_build_fsp_error_prefix */
    CHECK(FIPS_FRAMING_FSP_HEADER_SIZE == 12 && FIPS_FRAMING_FSP_INNER_HEADER_SIZE == 6 && FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE == 28 && FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE == 4);
    for (unsigned phase = 0; phase < 4; ++phase) {
        uint8_t prefix[4]; fips_fsp_build_handshake_prefix((uint8_t)phase, phase == 1 ? 100 : phase == 2 ? 50 : 73, prefix);
        CHECK(prefix[0] == phase && !prefix[1] && fips_fsp_common_prefix_parse(prefix, 4, &p) && p.phase == phase && p.version == 0 && !p.flags);
        CHECK(p.payload_len == (phase == 1 ? 100 : phase == 2 ? 50 : 73));
        CHECK(!fips_fsp_common_prefix_is_unencrypted(&p));
    }
    const uint8_t established[] = {0, FIPS_FSP_FLAG_CP, 64, 0};
    CHECK(fips_fsp_common_prefix_parse(established, 4, &p) && p.payload_len == 64 && fips_fsp_common_prefix_has_coords(&p));
    for (size_t n = 0; n < 4; ++n) CHECK(!fips_fsp_common_prefix_parse(n ? established : NULL, n, &p));
    uint8_t prefix[4]; fips_fsp_build_error_prefix(34, prefix);
    CHECK(fips_fsp_common_prefix_parse(prefix, 4, &p) && p.phase == 0 && p.payload_len == 34 && fips_fsp_common_prefix_is_unencrypted(&p));
    CHECK(fips_fsp_check_prefix(&p, 34, NULL) == FIPS_OK);
    CHECK(fips_fsp_check_prefix(&p, 33, NULL) == FIPS_ERR_RANGE && fips_fsp_check_prefix(&p, 35, NULL) == FIPS_ERR_RANGE && fips_fsp_check_prefix(&p, 0, NULL) == FIPS_ERR_RANGE);
    /* check_prefix_refuses_a_routing_signal_with_a_reserved_prefix_bit */
    for (unsigned bit = 3; bit < 8; ++bit) { p.flags = (uint8_t)(FIPS_FSP_FLAG_U | (1u << bit)); CHECK(fips_fsp_check_prefix(&p, 34, NULL) == FIPS_ERR_RANGE); }
    p.flags = 7; CHECK(fips_fsp_check_prefix(&p, 34, NULL) == FIPS_OK);
    /* test_build_fsp_header; test_encrypted_header_parse; test_build_fsp_encrypted;
     * test_header_roundtrip; the_encrypted_header_parser_still_reads_a_header_with_reserved_prefix_bits_set;
     * check_header_refuses_each_reserved_prefix_bit_and_passes_cp_and_k;
     * check_prefix_leaves_an_established_message_to_the_header_checks */
    const uint64_t counters[] = {0, 42, 1000, UINT64_C(0xdeadbeef12345678), UINT64_MAX};
    for (size_t i = 0; i < 5; ++i) for (unsigned flags = 0; flags < 256; ++flags) {
        fips_fsp_build_header(counters[i], (uint8_t)flags, 1234, header);
        CHECK(header[0] == 0 && header[1] == flags && header[2] == 0xd2 && header[3] == 4);
        fips_writer_init(&w, packet, sizeof(packet));
        memset(packet + 12, 0xcc, 1250); /* opaque payload assembled separately below */
        uint8_t sealed[1250]; memset(sealed, 0xcc, sizeof(sealed));
        CHECK(fips_fsp_build_encrypted(header, sealed, sizeof(sealed), &w, NULL) == FIPS_OK && w.pos == 1262);
        CHECK(!memcmp(packet, header, 12) && !memcmp(packet + 12, sealed, sizeof(sealed)));
        if (flags & FIPS_FSP_FLAG_U) { CHECK(!fips_fsp_encrypted_header_parse(packet, w.pos, &h)); continue; }
        CHECK(fips_fsp_encrypted_header_parse(packet, w.pos, &h) && h.counter == counters[i] && h.flags == flags && h.payload_len == 1234 && !memcmp(h.header_bytes, header, 12));
        CHECK(fips_fsp_encrypted_header_has_coords(&h) == ((flags & FIPS_FSP_FLAG_CP) != 0) && fips_fsp_encrypted_header_data_offset(&h) == 12);
        CHECK((fips_fsp_check_header(&h, NULL) == FIPS_OK) == !(flags & FIPS_FSP_FLAGS_RESERVED));
        CHECK(fips_fsp_common_prefix_parse(header, 12, &p) && fips_fsp_check_prefix(&p, 0, NULL) == FIPS_OK);
        /* check_sealed_compares_payload_len_with_the_bytes_less_the_tag */
        CHECK(fips_fsp_check_sealed(&h, 1250, NULL) == FIPS_OK);
        const size_t bad[] = {1249, 1251, 16, 15, 0};
        for (size_t j = 0; j < 5; ++j) CHECK(fips_fsp_check_sealed(&h, bad[j], NULL) == FIPS_ERR_RANGE);
    }
    /* test_encrypted_header_{too_short,wrong_phase,wrong_version,u_flag_rejected} */
    memset(packet, 0, 28);
    for (size_t n = 0; n < 28; ++n) CHECK(!fips_fsp_encrypted_header_parse(n ? packet : NULL, n, &h));
    packet[0] = 1; CHECK(!fips_fsp_encrypted_header_parse(packet, 28, &h));
    packet[0] = 0x10; CHECK(!fips_fsp_encrypted_header_parse(packet, 28, &h));
    packet[0] = 0; packet[1] = FIPS_FSP_FLAG_U; CHECK(!fips_fsp_encrypted_header_parse(packet, 28, &h));
    /* test_inner_header_{prepend_strip,empty_payload,too_short};
     * check_inner_refuses_each_reserved_inner_flag_and_passes_the_spin_bit */
    const uint8_t payload[] = {0xaa, 0xbb, 0xcc};
    for (unsigned empty = 0; empty < 2; ++empty) {
        fips_writer_init(&w, inner, sizeof(inner));
        CHECK(fips_fsp_prepend_inner_header(empty ? 0 : 12345, empty ? 0x13 : 0x10, empty ? 0 : 1, empty ? NULL : payload, empty ? 0 : 3, &w, NULL) == FIPS_OK);
        fips_fsp_inner_header ih; fips_fsp_bytes rest;
        CHECK(fips_fsp_strip_inner_header(inner, w.pos, &ih, &rest) && ih.timestamp_ms == (empty ? 0u : 12345u) && ih.msg_type == (empty ? 0x13 : 0x10) && ih.inner_flags == (empty ? 0 : 1));
        CHECK(rest.len == (empty ? 0u : 3u) && (!rest.len || !memcmp(rest.data, payload, 3)));
        for (size_t n = 0; n < 6; ++n) CHECK(!fips_fsp_strip_inner_header(n ? inner : NULL, n, &ih, &rest));
    }
    /* C writer overflow/capacity and no partial output. */
    for (size_t n = 0; n < 28; ++n) {
        memset(packet, 0xcc, sizeof(packet)); fips_writer_init(&w, n ? packet : NULL, n);
        CHECK(fips_fsp_build_encrypted(header, inner, 16, &w, NULL) == FIPS_ERR_RANGE && !w.pos && packet[0] == 0xcc);
    }
    fips_writer_init(&w, packet, sizeof(packet));
    CHECK(fips_fsp_build_encrypted(header, inner, SIZE_MAX, &w, NULL) == FIPS_ERR_RANGE && !w.pos);
    CHECK(fips_fsp_prepend_inner_header(0, 0, 0, inner, SIZE_MAX, &w, NULL) == FIPS_ERR_RANGE && !w.pos);
    return 0;
}
static int coords_ports(void)
{
    uint8_t data[80] = {0};
    fips_writer w; fips_reader r;
    /* Port 256 is LE 00 01, dispatch is by destination, rest is service bytes. */
    const fips_fsp_port_header inputs[] = {{0, 0}, {1, 256}, {65535, 65535}, {256, 1}};
    for (size_t i = 0; i < 4; ++i) {
        fips_writer_init(&w, data, sizeof(data));
        CHECK(fips_fsp_port_header_encode(inputs[i], &w, NULL) == FIPS_OK && w.pos == 4);
        CHECK(data[0] == (uint8_t)inputs[i].src_port && data[1] == (uint8_t)(inputs[i].src_port >> 8) && data[2] == (uint8_t)inputs[i].dst_port && data[3] == (uint8_t)(inputs[i].dst_port >> 8));
        data[4] = 0xaa; fips_reader_init(&r, data, 5); fips_fsp_port_header p;
        CHECK(fips_fsp_port_header_decode(&r, &p, NULL) == FIPS_OK && r.pos == 4 && p.src_port == inputs[i].src_port && p.dst_port == inputs[i].dst_port && *fips_reader_rest(&r) == 0xaa);
        for (size_t n = 0; n < 4; ++n) {
            p = (fips_fsp_port_header){7, 9}; fips_reader_init(&r, n ? data : NULL, n);
            CHECK(fips_fsp_port_header_decode(&r, &p, NULL) == FIPS_ERR_RANGE && !r.pos && p.src_port == 7 && p.dst_port == 9);
            fips_writer_init(&w, n ? data : NULL, n); CHECK(fips_fsp_port_header_encode(inputs[i], &w, NULL) == FIPS_ERR_RANGE && !w.pos);
        }
    }
    CHECK(FIPS_FSP_PORT_IPV6_SHIM == 256 && FIPS_FRAMING_FSP_PORT_HEADER_SIZE == 4);
    /* parse_encrypted_coords: both absent, one absent, both present; every
     * truncated byte, empty optional path, full u16 coordinate domain. */
    for (unsigned src = 0; src < 2; ++src) for (unsigned dest = 0; dest < 2; ++dest) {
        size_t total = 4 + 16 * (src + dest);
        memset(data, 0, sizeof(data)); data[0] = (uint8_t)src; data[2 + 16 * src] = (uint8_t)dest;
        for (size_t n = 0; n <= total; ++n) {
            fips_reader_init(&r, n ? data : NULL, n);
            fips_fsp_coords s = {NULL, 99}, d = {NULL, 88};
            fips_status status = fips_fsp_parse_encrypted_coords(&r, &s, &d, NULL);
            if (n < total) CHECK(status == FIPS_ERR_RANGE && !r.pos && s.count == 99 && d.count == 88);
            else CHECK(status == FIPS_OK && r.pos == total && s.count == src && d.count == dest && (src || !s.addrs) && (dest || !d.addrs));
        }
    }
    const size_t total = 4 + 16 * (size_t)UINT16_MAX;
    uint8_t *large = calloc(1, total); CHECK(large);
    large[0] = 255; large[1] = 255;
    fips_reader_init(&r, large, total); fips_fsp_coords s, d;
    CHECK(fips_fsp_parse_encrypted_coords(&r, &s, &d, NULL) == FIPS_OK && s.count == UINT16_MAX && !d.count && r.pos == total);
    free(large);
    return 0;
}
static int ingress(void)
{
    uint8_t packet[128], hs[3] = {1, 2, 3};
    fips_fsp_action output[2] = {0}; fips_fsp_action_list a = {output, 0, 2};
    fips_fsp_session_msg3 msg = {0, {hs, 3}};
    fips_writer w; fips_writer_init(&w, packet, sizeof(packet));
    CHECK(fips_fsp_session_msg3_encode(&msg, &w, NULL) == FIPS_OK);
    size_t total = w.pos;
    for (size_t n = 0; n < total; ++n) {
        a.len = 0; CHECK(fips_fsp_receive(addr(1), n ? packet : NULL, n, &a) == FIPS_OK && a.len == 1 && output[0].kind == FIPS_FSP_DropFrame);
    }
    a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, total, &a) == FIPS_OK && output[0].kind == FIPS_FSP_ReceiveMsg3 && output[0].bytes.data == packet);
    packet[4] = 1; a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, total, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    memset(packet, 0, sizeof(packet)); fips_fsp_build_header(7, FIPS_FSP_FLAG_CP | FIPS_FSP_FLAG_K, 6, packet);
    /* CP block of two optional absent paths: not part of payload_len or tag. */
    total = 12 + 4 + 6 + 16;
    a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, total, &a) == FIPS_OK && output[0].kind == FIPS_FSP_ReceiveEncrypted);
    for (size_t n = 0; n < total; ++n) {
        a.len = 0; CHECK(fips_fsp_receive(addr(1), n ? packet : NULL, n, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    }
    packet[1] |= 8; a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, total, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    fips_fsp_build_error_prefix(1, packet); packet[4] = 0x20;
    a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, 5, &a) == FIPS_OK && output[0].kind == FIPS_FSP_ReceiveRoutingSignal);
    packet[0] = 0x10; a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, 5, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame && output[0].error.variant == FIPS_PROTO_UnsupportedVersion);
    packet[0] = 15; a.len = 0; CHECK(fips_fsp_receive(addr(1), packet, 5, &a) == FIPS_OK && output[0].error.variant == FIPS_PROTO_InvalidMessageType);
    for (size_t n = 0; n < 6; ++n) {
        a.len = 0; CHECK(fips_fsp_receive_plaintext(addr(1), n ? packet : NULL, n, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    }
    fips_writer_init(&w, packet, sizeof(packet)); CHECK(fips_fsp_prepend_inner_header(123, FIPS_SESSION_DataPacket, 1, hs, 3, &w, NULL) == FIPS_OK);
    a.len = 0; CHECK(fips_fsp_receive_plaintext(addr(1), packet, w.pos, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DeliverSessionMessage && output[0].inner.timestamp_ms == 123 && output[0].bytes.len == 3);
    packet[5] = 2; a.len = 0; CHECK(fips_fsp_receive_plaintext(addr(1), packet, w.pos, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    packet[5] = 0; packet[4] = 0x20; a.len = 0; CHECK(fips_fsp_receive_plaintext(addr(1), packet, w.pos, &a) == FIPS_OK && output[0].kind == FIPS_FSP_DropFrame);
    a.len = a.capacity; fips_fsp_action saved = output[1];
    CHECK(fips_fsp_receive(addr(1), packet, 1, &a) == FIPS_ERR_RANGE && a.len == 2 && !memcmp(&saved, output + 1, sizeof(saved)));
    CHECK(fips_fsp_receive_plaintext(addr(1), packet, w.pos, &a) == FIPS_ERR_RANGE && a.len == 2);
    return 0;
}
int main(void)
{
    int failed = core() + quorum() + catalog_flags() + handshakes() + headers() + coords_ports() + ingress();
    if (failed) return 1;
    puts("FSP core/quorum/wire upstream ports and C bounds/ingress tests passed"); return 0;
}
