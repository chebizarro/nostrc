/* Wire properties use SeededEntropy, bounded Reader/Writer adapters and the
 * pinned differential sidecars. No production entropy or relay I/O. */
#include "fakes/host.h"
#include <fips/proto/stp/mod.h>
#include <fips/proto/bloom/mod.h>
#include <fips/proto/lookup/wire.h>
#include <fips/proto/routing/wire.h>
#include <fips/proto/fmp/wire.h>
#include <fips/proto/fsp/wire.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #x, current); exit(1); } } while (0)
static const char *current = "setup";
static size_t random_count, vector_count, derived_count, truncation_count, mutation_count, capacity_count, derived_count;
/* Structured tests deliberately use short paths, not a reduced production cap. */
#define BUFFER_SIZE 8192u
static uint8_t wire[BUFFER_SIZE], again[BUFFER_SIZE], mutated[BUFFER_SIZE];
typedef enum codec {
    ENTRY, COORDS, OPTIONAL, DECLARATION, TREE, BLOOM, FILTER, LOOKUP_REQ,
    LOOKUP_RESP, DATAGRAM, DISCONNECT, PORTS, SETUP, ACK, MSG3,
    STRICT_SETUP, STRICT_ACK, STRICT_MSG3, REQUIRED, BROKEN, MTU,
    FMP1, FMP2, FMP_ENCRYPTED, FSP_ENCRYPTED, FMP_INNER, FSP_INNER, CODEC_COUNT
} codec;
static const char *const names[] = {
    "entry", "coords", "optional-coords", "declaration", "tree", "bloom", "filter",
    "lookup-request", "lookup-response", "datagram", "disconnect", "ports", "setup",
    "ack", "msg3", "strict-setup", "strict-ack", "strict-msg3", "coords-required",
    "path-broken", "mtu-exceeded", "fmp1", "fmp2", "fmp-encrypted", "fsp-encrypted",
    "fmp-inner", "fsp-inner"
};
typedef struct value {
    union {
        fips_coord_entry entry;
        fips_tree_coordinate coords;
        fips_parent_declaration declaration;
        fips_tree_announce tree;
        fips_bloom_filter bloom;
        fips_filter_announce filter;
        fips_lookup_request req;
        fips_lookup_response resp;
        fips_session_datagram datagram;
        fips_fmp_disconnect disconnect;
        fips_fsp_port_header ports;
        fips_fsp_session_setup setup;
        fips_fsp_session_ack ack;
        fips_fsp_session_msg3 msg3;
        fips_coords_required required;
        fips_path_broken broken;
        fips_mtu_exceeded mtu;
    } as;
    fips_tree_coordinate storage;
    bool present;
    fips_fmp_msg1_header m1;
    fips_fmp_msg2_header m2;
    fips_fmp_encrypted_header fmph;
    fips_fsp_encrypted_header fsph;
    fips_fsp_inner_header inner;
    fips_fsp_bytes bytes;
} value;
/* Large coordinate values must not live on the stack. */
static value a, b, c;
static fips_entropy entropy;
static uint8_t payload[128], addrs[64];
static uint64_t random_u64(void) {
    uint8_t x[8]; uint64_t result = 0;
    entropy.fill(entropy.ctx, x, sizeof(x));
    for (unsigned i = 0; i < 8; ++i) result |= (uint64_t)x[i] << (8 * i);
    return result;
}
static void random_bytes(void *p, size_t n) { entropy.fill(entropy.ctx, p, n); }
static void random_coords(fips_tree_coordinate *p, bool metadata) {
    p->len = (uint16_t)(1 + random_u64() % 4);
    for (size_t i = 0; i < p->len; ++i) {
        random_bytes(p->entries[i].node_addr.bytes, 16);
        p->entries[i].sequence = metadata ? random_u64() : 0;
        p->entries[i].timestamp = metadata ? random_u64() : 0;
    }
}
static size_t offset(codec k) {
    if (k == TREE || k == FILTER || k == LOOKUP_REQ || k == LOOKUP_RESP || k == DATAGRAM || k == DISCONNECT) return 1;
    if (k >= SETUP && k <= STRICT_MSG3) return 4;
    if (k >= REQUIRED && k <= MTU) return 5;
    return 0;
}
static void generate(codec k, value *v) {
    memset(v, 0, sizeof(*v));
    random_bytes(payload, sizeof(payload)); random_bytes(addrs, sizeof(addrs));
    size_t n = (size_t)(random_u64() % 65);
    switch (k) {
    case ENTRY:
        random_bytes(v->as.entry.node_addr.bytes, 16);
        v->as.entry.sequence = random_u64(); v->as.entry.timestamp = random_u64(); break;
    case COORDS: random_coords(&v->as.coords, false); break;
    case OPTIONAL:
        v->present = (random_u64() & 1) != 0;
        if (v->present) random_coords(&v->as.coords, false);
        break;
    case DECLARATION: case TREE: {
        fips_parent_declaration *d = k == TREE ? &v->as.tree.declaration : &v->as.declaration;
        random_bytes(d->node_addr.bytes, 16); random_bytes(d->parent_id.bytes, 16);
        d->sequence = random_u64(); d->timestamp = random_u64();
        if (k == TREE) {
            d->is_signed = true; random_bytes(d->signature, 64);
            random_coords(&v->as.tree.ancestry, true);
            d->node_addr = v->as.tree.ancestry.entries[0].node_addr;
        }
        break;
    }
    case BLOOM: case FILTER: {
        fips_bloom_filter *f = k == BLOOM ? &v->as.bloom : &v->as.filter.filter;
        fips_bloom_init(f); random_bytes(f->bits, f->num_bits / 8);
        if (k == FILTER) { v->as.filter.sequence = random_u64(); v->as.filter.hash_count = f->hash_count; v->as.filter.size_class = 1; }
        break;
    }
    case LOOKUP_REQ:
        v->as.req.request_id = random_u64(); random_bytes(v->as.req.target.bytes, 16);
        random_bytes(v->as.req.origin.bytes, 16); random_coords(&v->as.req.origin_coords, false);
        v->as.req.ttl = (uint8_t)random_u64(); v->as.req.min_mtu = (uint16_t)random_u64(); break;
    case LOOKUP_RESP:
        v->as.resp.request_id = random_u64(); random_bytes(v->as.resp.target.bytes, 16);
        v->as.resp.path_mtu = (uint16_t)random_u64(); random_coords(&v->as.resp.target_coords, false);
        random_bytes(v->as.resp.proof, 64); break;
    case DATAGRAM:
        random_bytes(v->as.datagram.src_addr.bytes, 16); random_bytes(v->as.datagram.dest_addr.bytes, 16);
        v->as.datagram.ttl = (uint8_t)random_u64(); v->as.datagram.path_mtu = (uint16_t)random_u64();
        v->as.datagram.payload = payload; v->as.datagram.payload_len = n; break;
    case DISCONNECT: {
        unsigned reason = (unsigned)(random_u64() % 9);
        v->as.disconnect.reason = (fips_fmp_disconnect_reason)(reason == 8 ? 255 : reason); break;
    }
    case PORTS: v->as.ports = (fips_fsp_port_header){(uint16_t)random_u64(), (uint16_t)random_u64()}; break;
    case SETUP: case STRICT_SETUP:
        v->as.setup.src_coords = (fips_fsp_coords){addrs, 1 + random_u64() % 4};
        v->as.setup.dest_coords = (fips_fsp_coords){addrs, 1 + random_u64() % 4};
        v->as.setup.flags.bits = k == STRICT_SETUP ? 0 : (uint8_t)random_u64();
        v->as.setup.handshake_payload = (fips_fsp_bytes){payload, n}; break;
    case ACK: case STRICT_ACK:
        v->as.ack.src_coords = (fips_fsp_coords){addrs, 1 + random_u64() % 4};
        v->as.ack.dest_coords = (fips_fsp_coords){addrs, 1 + random_u64() % 4};
        v->as.ack.flags = k == STRICT_ACK ? 0 : (uint8_t)random_u64();
        v->as.ack.handshake_payload = (fips_fsp_bytes){payload, n}; break;
    case MSG3: case STRICT_MSG3:
        v->as.msg3.flags = k == STRICT_MSG3 ? 0 : (uint8_t)random_u64();
        v->as.msg3.handshake_payload = (fips_fsp_bytes){payload, n}; break;
    case REQUIRED:
        random_bytes(v->as.required.dest_addr.bytes, 16); random_bytes(v->as.required.reporter.bytes, 16); break;
    case BROKEN:
        random_bytes(v->as.broken.dest_addr.bytes, 16); random_bytes(v->as.broken.reporter.bytes, 16);
        if (random_u64() & 1) { random_coords(&v->storage, false); v->as.broken.last_known_coords = &v->storage; }
        break;
    case MTU:
        random_bytes(v->as.mtu.dest_addr.bytes, 16); random_bytes(v->as.mtu.reporter.bytes, 16);
        v->as.mtu.mtu = (uint16_t)random_u64(); break;
    case FMP1: case FMP2:
        v->m1.sender_idx.value = (uint32_t)random_u64();
        v->m2.sender_idx = v->m1.sender_idx; v->m2.receiver_idx.value = (uint32_t)random_u64();
        v->bytes = (fips_fsp_bytes){payload, k == FMP1 ? FIPS_FRAMING_NOISE_HANDSHAKE_MSG1_SIZE : FIPS_FRAMING_NOISE_HANDSHAKE_MSG2_SIZE}; break;
    case FMP_ENCRYPTED:
        v->fmph.receiver_idx.value = (uint32_t)random_u64(); v->fmph.counter = random_u64();
        v->fmph.flags = (uint8_t)(random_u64() & 7); v->bytes = (fips_fsp_bytes){payload, n + 16};
        v->fmph.payload_len = (uint16_t)n;
        fips_fmp_build_established_header(v->fmph.receiver_idx, v->fmph.counter, v->fmph.flags, v->fmph.payload_len, v->fmph.header_bytes); break;
    case FSP_ENCRYPTED:
        v->fsph.counter = random_u64(); v->fsph.flags = (uint8_t)(random_u64() & 3);
        v->bytes = (fips_fsp_bytes){payload, n + 16}; v->fsph.payload_len = (uint16_t)n;
        fips_fsp_build_header(v->fsph.counter, v->fsph.flags, v->fsph.payload_len, v->fsph.header_bytes); break;
    case FMP_INNER: case FSP_INNER:
        v->inner = (fips_fsp_inner_header){(uint32_t)random_u64(), (uint8_t)random_u64(), (uint8_t)random_u64()};
        v->bytes = (fips_fsp_bytes){payload, n}; break;
    case CODEC_COUNT: abort();
    }
}
static fips_status encode(codec k, const value *v, fips_writer *w, fips_proto_error *e) {
    switch (k) {
    case ENTRY: return fips_coord_entry_encode(&v->as.entry, w, e);
    case COORDS: return fips_encode_coords(&v->as.coords, w, e);
    case OPTIONAL: return v->present ? fips_encode_coords(&v->as.coords, w, e) : fips_encode_empty_coords(w, e);
    case DECLARATION: return fips_parent_declaration_signing_bytes(&v->as.declaration, w, e);
    case TREE: return fips_tree_announce_encode(&v->as.tree, w, e);
    case BLOOM: return fips_bloom_encode(&v->as.bloom, w, e);
    case FILTER: return fips_filter_announce_encode(&v->as.filter, w, e);
    case LOOKUP_REQ: return fips_lookup_request_encode(&v->as.req, w, e);
    case LOOKUP_RESP: return fips_lookup_response_encode(&v->as.resp, w, e);
    case DATAGRAM: return fips_session_datagram_encode(&v->as.datagram, w, e);
    case DISCONNECT: return fips_fmp_disconnect_encode(&v->as.disconnect, w, e);
    case PORTS: return fips_fsp_port_header_encode(v->as.ports, w, e);
    case SETUP: case STRICT_SETUP: return fips_fsp_session_setup_encode(&v->as.setup, w, e);
    case ACK: case STRICT_ACK: return fips_fsp_session_ack_encode(&v->as.ack, w, e);
    case MSG3: case STRICT_MSG3: return fips_fsp_session_msg3_encode(&v->as.msg3, w, e);
    case REQUIRED: return fips_coords_required_encode(&v->as.required, w, e);
    case BROKEN: return fips_path_broken_encode(&v->as.broken, w, e);
    case MTU: return fips_mtu_exceeded_encode(&v->as.mtu, w, e);
    case FMP1: return fips_fmp_build_msg1(v->m1.sender_idx, v->bytes.data, v->bytes.len, w, e);
    case FMP2: return fips_fmp_build_msg2(v->m2.sender_idx, v->m2.receiver_idx, v->bytes.data, v->bytes.len, w, e);
    case FMP_ENCRYPTED: return fips_fmp_build_encrypted(v->fmph.header_bytes, v->bytes.data, v->bytes.len, w, e);
    case FSP_ENCRYPTED: return fips_fsp_build_encrypted(v->fsph.header_bytes, v->bytes.data, v->bytes.len, w, e);
    case FMP_INNER: return fips_fmp_prepend_inner_header(v->inner.timestamp_ms, v->bytes.data, v->bytes.len, w, e);
    case FSP_INNER: return fips_fsp_prepend_inner_header(v->inner.timestamp_ms, v->inner.msg_type, v->inner.inner_flags, v->bytes.data, v->bytes.len, w, e);
    case CODEC_COUNT: break;
    }
    abort();
}
static fips_status decode(codec k, fips_reader *r, value *v, fips_proto_error *e) {
    switch (k) {
    case ENTRY: return fips_coord_entry_decode(r, &v->as.entry, e);
    case COORDS: return fips_decode_coords(r, &v->as.coords, e);
    case OPTIONAL: return fips_decode_optional_coords(r, &v->as.coords, &v->present, e);
    case DECLARATION: return fips_parent_declaration_decode(r, &v->as.declaration, e);
    case TREE: return fips_tree_announce_decode(r, &v->as.tree, e);
    case BLOOM: return fips_bloom_decode(r, 5, &v->as.bloom, e);
    case FILTER: return fips_filter_announce_decode(r, &v->as.filter, e);
    case LOOKUP_REQ: return fips_lookup_request_decode(r, &v->as.req, e);
    case LOOKUP_RESP: return fips_lookup_response_decode(r, &v->as.resp, e);
    case DATAGRAM: return fips_session_datagram_decode(r, &v->as.datagram, e);
    case DISCONNECT: return fips_fmp_disconnect_decode(r, &v->as.disconnect, e);
    case PORTS: return fips_fsp_port_header_decode(r, &v->as.ports, e);
    case SETUP: return fips_fsp_session_setup_decode(r, &v->as.setup, e);
    case ACK: return fips_fsp_session_ack_decode(r, &v->as.ack, e);
    case MSG3: return fips_fsp_session_msg3_decode(r, &v->as.msg3, e);
    case STRICT_SETUP: return fips_fsp_session_setup_decode_strict(r, &v->as.setup, e);
    case STRICT_ACK: return fips_fsp_session_ack_decode_strict(r, &v->as.ack, e);
    case STRICT_MSG3: return fips_fsp_session_msg3_decode_strict(r, &v->as.msg3, e);
    case REQUIRED: return fips_coords_required_decode(r, &v->as.required, e);
    case BROKEN: return fips_path_broken_decode(r, &v->as.broken, &v->storage, e);
    case MTU: return fips_mtu_exceeded_decode(r, &v->as.mtu, e);
    default: break;
    }
    const uint8_t *p = r->buf + r->pos; size_t n = fips_reader_remaining(r), header = 0;
    if (k == FMP1 || k == FMP2 || k == FMP_ENCRYPTED) {
        if (fips_fmp_validate_frame(p, n, e) != FIPS_OK) return FIPS_ERR_RANGE;
        if (k == FMP1) { if (!fips_fmp_msg1_header_parse(p, n, &v->m1)) return FIPS_ERR_RANGE; header = v->m1.noise_msg1_offset; }
        else if (k == FMP2) { if (!fips_fmp_msg2_header_parse(p, n, &v->m2)) return FIPS_ERR_RANGE; header = v->m2.noise_msg2_offset; }
        else { if (!fips_fmp_encrypted_header_parse(p, n, &v->fmph)) return FIPS_ERR_RANGE; header = 16; }
        v->bytes = (fips_fsp_bytes){p + header, n - header};
    } else if (k == FSP_ENCRYPTED) {
        if (!fips_fsp_encrypted_header_parse(p, n, &v->fsph)) return FIPS_ERR_RANGE;
        v->bytes = (fips_fsp_bytes){p + 12, n - 12};
    } else if (k == FMP_INNER) {
        if (!fips_fmp_strip_inner_header(p, n, &v->inner.timestamp_ms, &v->bytes.data, &v->bytes.len)) return FIPS_ERR_RANGE;
    } else if (k == FSP_INNER) {
        if (!fips_fsp_strip_inner_header(p, n, &v->inner, &v->bytes)) return FIPS_ERR_RANGE;
    } else abort();
    r->pos = r->len; return FIPS_OK;
}
/* Equality checks fields, not padding or pointer addresses. For the compound
 * records compare their scalar/byte fields plus meaningful coordinate entries. */
static bool bytes_equal(fips_fsp_bytes x, fips_fsp_bytes y) { return x.len == y.len && !memcmp(x.data, y.data, x.len); }
static bool coords_equal(fips_fsp_coords x, fips_fsp_coords y) { return x.count == y.count && !memcmp(x.addrs, y.addrs, x.count * 16); }
static bool addr_equal(fips_node_addr x, fips_node_addr y) { return !memcmp(x.bytes, y.bytes, 16); }
static bool equal(codec k, const value *x, const value *y) {
    switch (k) {
    case ENTRY: return fips_coord_entry_equal(&x->as.entry, &y->as.entry);
    case COORDS: return fips_tree_coordinate_equal(&x->as.coords, &y->as.coords);
    case OPTIONAL: return x->present == y->present && (!x->present || fips_tree_coordinate_equal(&x->as.coords, &y->as.coords));
    case DECLARATION: return fips_parent_declaration_equal(&x->as.declaration, &y->as.declaration);
    case TREE:
        if (!fips_parent_declaration_equal(&x->as.tree.declaration, &y->as.tree.declaration) || x->as.tree.ancestry.len != y->as.tree.ancestry.len) return false;
        for (size_t i = 0; i < x->as.tree.ancestry.len; ++i)
            if (!fips_coord_entry_equal(&x->as.tree.ancestry.entries[i], &y->as.tree.ancestry.entries[i])) return false;
        return true;
    case BLOOM: return fips_bloom_equal(&x->as.bloom, &y->as.bloom);
    case FILTER: return x->as.filter.sequence == y->as.filter.sequence && x->as.filter.hash_count == y->as.filter.hash_count && x->as.filter.size_class == y->as.filter.size_class && fips_bloom_equal(&x->as.filter.filter, &y->as.filter.filter);
    case LOOKUP_REQ: return x->as.req.request_id == y->as.req.request_id && addr_equal(x->as.req.target, y->as.req.target) && addr_equal(x->as.req.origin, y->as.req.origin) && x->as.req.ttl == y->as.req.ttl && x->as.req.min_mtu == y->as.req.min_mtu && fips_tree_coordinate_equal(&x->as.req.origin_coords, &y->as.req.origin_coords);
    case LOOKUP_RESP: return x->as.resp.request_id == y->as.resp.request_id && addr_equal(x->as.resp.target, y->as.resp.target) && x->as.resp.path_mtu == y->as.resp.path_mtu && !memcmp(x->as.resp.proof, y->as.resp.proof, 64) && fips_tree_coordinate_equal(&x->as.resp.target_coords, &y->as.resp.target_coords);
    case DATAGRAM: return addr_equal(x->as.datagram.src_addr, y->as.datagram.src_addr) && addr_equal(x->as.datagram.dest_addr, y->as.datagram.dest_addr) && x->as.datagram.ttl == y->as.datagram.ttl && x->as.datagram.path_mtu == y->as.datagram.path_mtu && bytes_equal((fips_fsp_bytes){x->as.datagram.payload, x->as.datagram.payload_len}, (fips_fsp_bytes){y->as.datagram.payload, y->as.datagram.payload_len});
    case DISCONNECT: return x->as.disconnect.reason == y->as.disconnect.reason;
    case PORTS: return x->as.ports.src_port == y->as.ports.src_port && x->as.ports.dst_port == y->as.ports.dst_port;
    case SETUP: case STRICT_SETUP: return x->as.setup.flags.bits == y->as.setup.flags.bits && coords_equal(x->as.setup.src_coords, y->as.setup.src_coords) && coords_equal(x->as.setup.dest_coords, y->as.setup.dest_coords) && bytes_equal(x->as.setup.handshake_payload, y->as.setup.handshake_payload);
    case ACK: case STRICT_ACK: return x->as.ack.flags == y->as.ack.flags && coords_equal(x->as.ack.src_coords, y->as.ack.src_coords) && coords_equal(x->as.ack.dest_coords, y->as.ack.dest_coords) && bytes_equal(x->as.ack.handshake_payload, y->as.ack.handshake_payload);
    case MSG3: case STRICT_MSG3: return x->as.msg3.flags == y->as.msg3.flags && bytes_equal(x->as.msg3.handshake_payload, y->as.msg3.handshake_payload);
    case REQUIRED: return addr_equal(x->as.required.dest_addr, y->as.required.dest_addr) && addr_equal(x->as.required.reporter, y->as.required.reporter);
    case BROKEN: return addr_equal(x->as.broken.dest_addr, y->as.broken.dest_addr) && addr_equal(x->as.broken.reporter, y->as.broken.reporter) && ((x->as.broken.last_known_coords == NULL && y->as.broken.last_known_coords == NULL) || (x->as.broken.last_known_coords && y->as.broken.last_known_coords && fips_tree_coordinate_equal(x->as.broken.last_known_coords, y->as.broken.last_known_coords)));
    case MTU: return addr_equal(x->as.mtu.dest_addr, y->as.mtu.dest_addr) && addr_equal(x->as.mtu.reporter, y->as.mtu.reporter) && x->as.mtu.mtu == y->as.mtu.mtu;
    case FMP1: return x->m1.sender_idx.value == y->m1.sender_idx.value && bytes_equal(x->bytes, y->bytes);
    case FMP2: return x->m2.sender_idx.value == y->m2.sender_idx.value && x->m2.receiver_idx.value == y->m2.receiver_idx.value && bytes_equal(x->bytes, y->bytes);
    case FMP_ENCRYPTED: return x->fmph.receiver_idx.value == y->fmph.receiver_idx.value && x->fmph.counter == y->fmph.counter && x->fmph.flags == y->fmph.flags && x->fmph.payload_len == y->fmph.payload_len && bytes_equal(x->bytes, y->bytes);
    case FSP_ENCRYPTED: return x->fsph.counter == y->fsph.counter && x->fsph.flags == y->fsph.flags && x->fsph.payload_len == y->fsph.payload_len && bytes_equal(x->bytes, y->bytes);
    case FMP_INNER: case FSP_INNER: return x->inner.timestamp_ms == y->inner.timestamp_ms && (k == FMP_INNER || (x->inner.msg_type == y->inner.msg_type && x->inner.inner_flags == y->inner.inner_flags)) && bytes_equal(x->bytes, y->bytes);
    case CODEC_COUNT: break;
    }
    return false;
}
static void capacity(codec k, const value *v, size_t n) {
    /* Nonzero writer position catches accidental capacity-vs-remaining checks. */
    fips_writer w; fips_proto_error e = {.variant = FIPS_PROTO_InvalidSignature};
    memset(again, 0xa5, sizeof(again)); fips_writer_init(&w, again, n + 3); w.pos = 3;
    CHECK(encode(k, v, &w, &e) == FIPS_OK && w.pos == n + 3);
    CHECK(e.variant == FIPS_PROTO_InvalidSignature && !memcmp(again + 3, wire, n));
    memset(again, 0xa5, sizeof(again)); fips_writer_init(&w, again, n + 2); w.pos = 3;
    CHECK(encode(k, v, &w, &e) == FIPS_ERR_RANGE && w.pos == 3);
    CHECK(e.variant == FIPS_PROTO_MessageTooLong);
    for (size_t i = 0; i < sizeof(again); ++i) CHECK(again[i] == 0xa5);
    capacity_count += 2;
}
static void exercise(codec k, size_t n, bool exhaustive) {
    size_t skip = offset(k); CHECK(n >= skip);
    fips_reader r; fips_writer w; fips_proto_error e;
    fips_reader_init(&r, wire + skip, n - skip);
    CHECK(decode(k, &r, &b, &e) == FIPS_OK && r.pos == n - skip);
    fips_writer_init(&w, again, sizeof(again));
    CHECK(encode(k, &b, &w, &e) == FIPS_OK && w.pos == n && !memcmp(wire, again, n));
    capacity(k, &b, n);
    if (!exhaustive) return;
    for (size_t len = 0; len < n - skip; ++len) {
        /* Exact allocation puts the ASan redzone at each truncation boundary. */
        uint8_t *short_input = malloc(len ? len : 1); CHECK(short_input);
        memcpy(short_input, wire + skip, len); fips_reader_init(&r, short_input, len);
        e.variant = FIPS_PROTO_InvalidSignature;
        fips_status status = decode(k, &r, &c, &e);
        /* Raw bloom and opaque tails have no encoded length. Shorter payloads
         * remain valid; Option parsers only require their fixed header. */
        size_t minimum = k == DATAGRAM ? 35 : k == FSP_ENCRYPTED ? 28 : k == FMP_INNER ? 5 : k == FSP_INNER ? 6 : 0;
        bool allowed = (k == BLOOM && len != 0) || (minimum && len >= minimum);
        CHECK((status == FIPS_OK) == allowed);
        if (status != FIPS_OK) {
            CHECK(r.pos == 0);
            if (k == BLOOM) CHECK(e.variant == FIPS_PROTO_BadBloom);
            else if (k == FSP_ENCRYPTED || k == FMP_INNER || k == FSP_INNER) CHECK(e.variant == FIPS_PROTO_InvalidSignature); /* Option::None, no error API */
            else if (k == FMP_ENCRYPTED && len >= FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE) CHECK(e.variant == FIPS_PROTO_Malformed && !strcmp(e.as.malformed, "payload_len"));
            else CHECK(e.variant == FIPS_PROTO_MessageTooShort);
            if (e.variant == FIPS_PROTO_MessageTooShort) {
                CHECK(e.as.message_too_short.got == (k == BROKEN && len >= 35 ? len - 33 : len));
                CHECK(e.as.message_too_short.expected > e.as.message_too_short.got);
            }
        }
        free(short_input); ++truncation_count;
    }
    for (size_t i = 0; i < n - skip; ++i) {
        memcpy(mutated, wire + skip, n - skip); mutated[i] ^= 0xff;
        fips_reader_init(&r, mutated, n - skip);
        if (decode(k, &r, &c, &e) == FIPS_OK) {
            size_t consumed = r.pos;
            fips_writer_init(&w, again, sizeof(again)); CHECK(encode(k, &c, &w, &e) == FIPS_OK);
            /* Disconnect unknown reasons normalize to Other; all other body
             * codecs must retain exactly their consumed wire bytes. */
            if (k == DISCONNECT && mutated[0] > 7 && mutated[0] != 255) CHECK(again[1] == 255);
            else CHECK(w.pos == consumed + skip && !memcmp(again + skip, mutated, consumed));
            fips_reader_init(&r, again + skip, w.pos - skip);
            CHECK(decode(k, &r, &b, &e) == FIPS_OK && equal(k, &b, &c));
        } else CHECK(r.pos == 0);
        ++mutation_count;
    }
}
static void vectors(void) {
    char path[4096]; snprintf(path, sizeof(path), "%s/proto.json", FIPS_DIFF_VECTORS);
    /* Only names/hex and small lengths are read; wide JSON u64 fields are
     * not numeric oracles here (they exceed Jansson json_int_t). */
    json_error_t error; json_t *root = json_load_file(path, JSON_REJECT_DUPLICATES | JSON_DECODE_INT_AS_REAL, &error);
    if (!root) fprintf(stderr, "%s: %s\n", path, error.text);
    CHECK(root);
    json_t *cases = json_object_get(json_object_get(root, "cases"), "wire_cases"); CHECK(json_is_array(cases));
    static const struct { const char *name; codec kind; } map[] = {
        {"coords", COORDS}, {"coords-empty", OPTIONAL}, {"bloom", BLOOM}, {"filter-announce", FILTER},
        {"parent-declaration", DECLARATION}, {"tree-announce", TREE}, {"lookup-request", LOOKUP_REQ},
        {"lookup-response", LOOKUP_RESP}, {"disconnect", DISCONNECT}, {"fmp-msg-1", FMP1},
        {"fmp-msg-2", FMP2}, {"fsp-setup", SETUP}, {"fsp-ack", ACK}, {"fsp-msg3", MSG3},
        {"fsp-encrypted", FSP_ENCRYPTED}, {"session-datagram", DATAGRAM}, {"fmp-encrypted", FMP_ENCRYPTED}
    };
    size_t index; json_t *item;
    json_array_foreach(cases, index, item) {
        const char *name = json_string_value(json_object_get(item, "name")); CHECK(name);
        for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); ++m) {
            if (strcmp(name, map[m].name)) continue;
            current = name;
            json_t *blob = json_object_get(item, map[m].kind == DECLARATION ? "signing_bytes" : "bytes"); CHECK(blob);
            const char *file = json_string_value(json_object_get(blob, "file")); CHECK(file);
            snprintf(path, sizeof(path), "%s/%s", FIPS_DIFF_VECTORS, file);
            FILE *f = fopen(path, "rb"); CHECK(f);
            size_t n = fread(wire, 1, sizeof(wire), f); CHECK(!ferror(f) && feof(f)); CHECK(fclose(f) == 0);
            CHECK(n == (size_t)json_number_value(json_object_get(blob, "length")));
            const char *hex = json_string_value(json_object_get(blob, "hex")); CHECK(hex && strlen(hex) == n * 2);
            for (size_t j = 0; j < n; ++j) { char pair[3]; snprintf(pair, sizeof(pair), "%02x", wire[j]); CHECK(!memcmp(pair, hex + j * 2, 2)); }
            exercise(map[m].kind, n, true); ++vector_count;
            if (map[m].kind == TREE) {
                /* TreeAnnounce's first ancestry entry uses the standalone
                 * CoordEntry codec, with its sequence/timestamp metadata. */
                memmove(wire, wire + 36, FIPS_COORD_ENTRY_WIRE_SIZE);
                exercise(ENTRY, FIPS_COORD_ENTRY_WIRE_SIZE, true); ++derived_count;
            }
            if (map[m].kind >= SETUP && map[m].kind <= MSG3) { exercise((codec)(map[m].kind + STRICT_SETUP - SETUP), n, true); ++vector_count; }
        }
    }
    CHECK(vector_count == 20 && derived_count == 1); json_decref(root);
}
static void catalogs(void) {
    current = "catalogs/flags";
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t byte = (uint8_t)i; fips_proto_error e;
        fips_link_message_type link; fips_session_message_type session;
        bool known = i <= 2 || i == 16 || i == 32 || i == 48 || i == 49 || (i >= 80 && i <= 84);
        CHECK((fips_link_message_type_from_u8(byte, &link, &e) == FIPS_OK) == known);
        if (known) CHECK((unsigned)link == i); else CHECK(e.variant == FIPS_PROTO_InvalidMessageType && e.as.invalid_message_type == byte);
        known = i >= 16 && i <= 20;
        CHECK((fips_session_message_type_from_u8(byte, &session, &e) == FIPS_OK) == known);
        if (known) CHECK((unsigned)session == i); else CHECK(e.variant == FIPS_PROTO_InvalidMessageType && e.as.invalid_message_type == byte);
        fips_routing_signal_type signal;
        known = i >= 32 && i <= 34;
        CHECK(fips_routing_signal_type_from_byte(byte, &signal) == known);
        if (known) CHECK(fips_routing_signal_type_to_byte(signal) == byte);
        fips_fmp_handshake_message_type phase;
        CHECK(fips_fmp_handshake_message_type_from_byte(byte, &phase) == (i == 1 || i == 2));
        fips_fmp_disconnect_reason reason;
        CHECK(fips_fmp_disconnect_reason_from_byte(byte, &reason) == (i <= 7 || i == 255));
        CHECK(fips_fsp_session_flags_to_byte(fips_fsp_session_flags_from_byte(byte)) == byte);
        CHECK(fips_fsp_flags_to_byte(fips_fsp_flags_from_byte(byte)) == (byte & 7));
        CHECK(fips_fsp_inner_flags_to_byte(fips_fsp_inner_flags_from_byte(byte)) == (byte & 1));
    }
}
/* Primitive Reader/Writer pairs and the prefix-only Option parsers are not
 * message-body codecs, so exercise their own fixed-width contracts separately. */
static void primitives_and_prefixes(void) {
    current = "Reader/Writer and prefixes";
    for (unsigned sample = 0; sample < 64; ++sample) {
        uint8_t u8 = (uint8_t)random_u64(), r8 = 0;
        uint16_t u16 = (uint16_t)random_u64(), r16 = 0;
        uint32_t u32 = (uint32_t)random_u64(), r32 = 0;
        uint64_t u64 = random_u64(), r64 = 0;
        uint8_t array[13]; random_bytes(array, sizeof(array));
        for (unsigned op = 0; op < 7; ++op) {
            size_t n = op < 4 ? (size_t)1 << op : sizeof(array);
            fips_writer w; fips_proto_error e;
            memset(wire, 0xa5, sizeof(wire));
            for (unsigned short_cap = 0; short_cap < 2; ++short_cap) {
                fips_writer_init(&w, wire, n - short_cap);
                fips_status status;
                if (op == 0) status = fips_writer_write_u8(&w, u8, &e);
                else if (op == 1) status = fips_writer_write_u16_le(&w, u16, &e);
                else if (op == 2) status = fips_writer_write_u32_le(&w, u32, &e);
                else if (op == 3) status = fips_writer_write_u64_le(&w, u64, &e);
                else status = fips_writer_write_bytes(&w, array, n, &e);
                CHECK((status == FIPS_OK) == !short_cap);
                if (short_cap) {
                    CHECK(w.pos == 0 && e.variant == FIPS_PROTO_MessageTooLong);
                    CHECK(!memcmp(wire, again, sizeof(wire)));
                } else { CHECK(w.pos == n); memcpy(again, wire, sizeof(wire)); }
                ++capacity_count;
            }
            for (size_t len = 0; len <= n; ++len) {
                uint8_t *input = malloc(len ? len : 1); CHECK(input); memcpy(input, wire, len);
                fips_reader r; fips_reader_init(&r, input, len);
                fips_status status; const uint8_t *borrow; uint8_t out[13];
                if (op == 0) status = fips_reader_read_u8(&r, &r8, &e);
                else if (op == 1) status = fips_reader_read_u16_le(&r, &r16, &e);
                else if (op == 2) status = fips_reader_read_u32_le(&r, &r32, &e);
                else if (op == 3) status = fips_reader_read_u64_le(&r, &r64, &e);
                else if (op == 4) status = fips_reader_read_array(&r, out, n, &e);
                else if (op == 5) status = fips_reader_read_bytes(&r, n, &borrow, &e);
                else status = fips_reader_advance(&r, n, &e);
                CHECK((status == FIPS_OK) == (len == n));
                if (len < n) {
                    CHECK(r.pos == 0 && e.variant == FIPS_PROTO_MessageTooShort);
                    CHECK(e.as.message_too_short.expected == n && e.as.message_too_short.got == len);
                    ++truncation_count;
                } else {
                    CHECK(r.pos == n);
                    if (op == 0) CHECK(r8 == u8);
                    if (op == 1) CHECK(r16 == u16);
                    if (op == 2) CHECK(r32 == u32);
                    if (op == 3) CHECK(r64 == u64);
                    if (op == 4) CHECK(!memcmp(out, array, n));
                    if (op == 5) CHECK(!memcmp(borrow, array, n));
                }
                free(input);
            }
        }
        uint8_t header[4]; uint16_t len = (uint16_t)random_u64();
        fips_fsp_common_prefix fsp; fips_fmp_common_prefix fmp;
        fips_fsp_build_handshake_prefix(u8, len, header);
        CHECK(fips_fsp_common_prefix_parse(header, 4, &fsp));
        CHECK(fsp.version == 0 && fsp.phase == (u8 & 15) && fsp.flags == 0 && fsp.payload_len == len);
        CHECK(fips_fmp_common_prefix_parse(header, 4, &fmp));
        CHECK(fmp.version == fsp.version && fmp.phase == fsp.phase && fmp.flags == fsp.flags && fmp.payload_len == len);
        fips_fsp_build_error_prefix(len, header);
        CHECK(fips_fsp_common_prefix_parse(header, 4, &fsp) && fsp.phase == 0 && fsp.flags == FIPS_FSP_FLAG_U && fsp.payload_len == len);
        for (size_t n = 0; n < 4; ++n) {
            uint8_t *input = malloc(n ? n : 1); CHECK(input); memcpy(input, header, n);
            CHECK(!fips_fsp_common_prefix_parse(input, n, &fsp));
            CHECK(!fips_fmp_common_prefix_parse(input, n, &fmp));
            free(input); truncation_count += 2;
        }
        /* Optional coordinate-pair view used after an encrypted FSP header. */
        random_coords(&a.as.coords, false);
        fips_writer w; fips_writer_init(&w, wire, sizeof(wire));
        CHECK(fips_encode_coords(&a.as.coords, &w, NULL) == FIPS_OK);
        CHECK(fips_encode_empty_coords(&w, NULL) == FIPS_OK);
        for (size_t n = 0; n <= w.pos; ++n) {
            uint8_t *input = malloc(n ? n : 1); CHECK(input); memcpy(input, wire, n);
            fips_reader r; fips_reader_init(&r, input, n); fips_fsp_coords src, dest; fips_proto_error e;
            fips_status status = fips_fsp_parse_encrypted_coords(&r, &src, &dest, &e);
            CHECK((status == FIPS_OK) == (n == w.pos));
            if (status == FIPS_OK) {
                CHECK(src.count == a.as.coords.len && dest.count == 0 && dest.addrs == NULL && r.pos == w.pos);
                for (size_t i = 0; i < src.count; ++i) CHECK(!memcmp(src.addrs + i * 16, a.as.coords.entries[i].node_addr.bytes, 16));
            } else {
                CHECK(r.pos == 0 && e.variant == FIPS_PROTO_MessageTooShort);
                CHECK(e.as.message_too_short.got == n); ++truncation_count;
            }
            free(input);
        }
    }
}
int main(void) {
    fips_seeded_entropy seeded = {UINT64_C(0x82a39)}; entropy = fips_seeded_entropy_adapter(&seeded);
    for (codec k = ENTRY; k < CODEC_COUNT; k = (codec)(k + 1)) {
        current = names[k];
        for (unsigned i = 0; i < 64; ++i) {
            generate(k, &a); fips_writer w; fips_reader r; fips_proto_error e;
            fips_writer_init(&w, wire, sizeof(wire)); CHECK(encode(k, &a, &w, &e) == FIPS_OK);
            size_t skip = offset(k); fips_reader_init(&r, wire + skip, w.pos - skip);
            CHECK(decode(k, &r, &b, &e) == FIPS_OK && equal(k, &a, &b));
            exercise(k, w.pos, true); ++random_count;
        }
    }
    vectors(); catalogs(); primitives_and_prefixes();
    printf("proto properties: %u codec adapters, %zu structured round trips, %zu vector replays + %zu derived-entry replay, %zu truncations, %zu corruptions, %zu capacity checks, 256 catalog/flag bytes, 64 primitive/prefix/coordinate-view rounds\n", (unsigned)CODEC_COUNT, random_count, vector_count, derived_count, truncation_count, mutation_count, capacity_count);
    return 0;
}
