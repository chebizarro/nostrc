#include "node/handlers/internal.h"
#include "fakes/host.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    handlers *h = calloc(1, sizeof *h); assert(h);
    fipsd_dataplane plane = {0}; h->dataplane = &plane;
    fips_test_clock clock = {0}; h->clock = fips_test_clock_adapter(&clock);
    h->me = (fips_node_addr){{1}};
    h->tree = calloc(1, sizeof *h->tree); assert(h->tree);
    fips_tree_state_new(h->tree, h->me, 0, fips_test_instant(0), NULL, 0);
    enum { SLOTS = FIPS_COORD_CACHE_SLOTS(8), WORDS = FIPS_COORD_CACHE_INDEX_WORDS(SLOTS) };
    fips_coord_slot *slots = calloc(SLOTS, sizeof *slots); uint32_t *index = calloc(WORDS, sizeof *index);
    assert(slots && index);
    fips_fixed_entropy entropy = {0x5a};
    assert(fips_coord_cache_init(&h->cache, slots, SLOTS, index, WORDS, 8, 300000,
        fips_test_instant(0), fips_fixed_entropy_adapter(&entropy)) == FIPS_OK);
    peer_rec peer = {.addr = {{2}}};
    fips_node_addr remote = {{3}}, path[] = {remote, peer.addr, h->me};
    uint8_t bytes[256]; fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
    fips_fsp_session_setup setup = {.src_coords = {path[0].bytes, 3}, .dest_coords = {h->me.bytes, 1}};
    assert(fips_fsp_session_setup_encode(&setup, &w, NULL) == FIPS_OK);
    fips_session_datagram dg; fips_session_datagram_init(&dg, remote, h->me, bytes, fips_writer_len(&w));
    fipsd_dp_warm(h, &peer, &dg);
    const fips_cache_entry *entry = fips_coord_cache_get_entry(&h->cache, remote.bytes);
    assert(entry && entry->coords.len == 3 && entry->verified_until == 0);
    assert(!fips_coord_cache_get_entry(&h->cache, h->me.bytes));
    assert(plane.needs_publish && fipsd_peer_tokens_available(&peer.coord_hints, fips_test_instant(0)) == 0);
    /* Decoder storage is borrowed, the cached coordinate must not be. */
    memset(bytes, 0xa5, sizeof bytes);
    assert(!memcmp(entry->coords.entries[0].node_addr, remote.bytes, 16));
    assert(!memcmp(entry->coords.entries[2].node_addr, h->me.bytes, 16));
    /* Malformed and foreign-root hints do not overwrite a good route. */
    plane.needs_publish = false; dg.payload_len = 3; fipsd_dp_warm(h, &peer, &dg); assert(!plane.needs_publish);
    path[2] = (fips_node_addr){{9}};
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fips_fsp_session_setup_encode(&setup, &w, NULL) == FIPS_OK);
    dg.payload_len = fips_writer_len(&w); fipsd_dp_warm(h, &peer, &dg);
    entry = fips_coord_cache_get_entry(&h->cache, remote.bytes);
    assert(entry && !memcmp(entry->coords.entries[2].node_addr, h->me.bytes, 16));
    /* A cleartext established CP header is never trusted at its endpoint. */
    uint8_t header[FIPS_FRAMING_FSP_HEADER_SIZE];
    fips_fsp_build_header(0, FIPS_FSP_FLAG_CP, 16, header);
    memcpy(bytes, header, sizeof header); memset(bytes + sizeof header, 0, 16);
    dg.payload_len = sizeof header + 16; plane.needs_publish = false;
    fipsd_dp_warm(h, &peer, &dg); assert(!plane.needs_publish);
    /* A flood of unfunded peer-chosen keys stays in bounded probation and
     * cannot displace a live verified entry. Quiet time funds later writes. */
    fips_node_addr protected = {{8}};
    uint8_t protected_path[2][16]; memcpy(protected_path[0], protected.bytes, 16); memcpy(protected_path[1], h->me.bytes, 16);
    fips_cached_coord verified;
    assert(fips_cached_coord_from_addrs(&verified, (const uint8_t (*)[16])protected_path, 2) == FIPS_OK);
    assert(fips_coord_cache_insert_verified(&h->cache, protected.bytes, &verified, fips_test_instant(0)) == FIPS_OK);
    path[2] = h->me;
    /* The shared setup/transit hint path must retain master's contract:
     * even an otherwise valid same-root hint cannot replace a verified entry. */
    fips_node_addr stated_peer = path[0];
    path[0] = protected;
    fipsd_dp_hint(h, &peer, &protected, setup.src_coords);
    const fips_cache_entry *keep_verified = fips_coord_cache_get_entry(&h->cache, protected.bytes);
    assert(keep_verified && keep_verified->verified_until > 0 && keep_verified->coords.len == 2);
    assert(!memcmp(keep_verified->coords.entries[1].node_addr, h->me.bytes, 16));
    path[0] = stated_peer;
    fipsd_dp_stated(h, &stated_peer, setup.src_coords);
    for (uint8_t i = 10; i < 50; ++i) {
        path[0] = (fips_node_addr){{i}}; dg.src_addr = path[0];
        fips_writer_init(&w, bytes, sizeof bytes);
        assert(fips_fsp_session_setup_encode(&setup, &w, NULL) == FIPS_OK);
        dg.payload_len = fips_writer_len(&w); fipsd_dp_warm(h, &peer, &dg);
        assert(h->cache.probation_len <= FIPS_COORD_CACHE_PROBATION_BOUND(8));
        assert(h->cache.len <= SLOTS);
        assert(fips_coord_cache_get_entry(&h->cache, stated_peer.bytes));
        const fips_cache_entry *keep = fips_coord_cache_get_entry(&h->cache, protected.bytes);
        assert(keep && keep->verified_until > 0 && !memcmp(keep->coords.entries[0].node_addr, protected.bytes, 16));
    }
    clock.millis = 1000;
    uint64_t available = fipsd_peer_tokens_available(&peer.coord_hints, fips_test_instant(1000));
    assert(available == 64 * FIPS_ADDRS_PER_ENTRY);
    path[0] = (fips_node_addr){{50}}; dg.src_addr = path[0];
    fips_writer_init(&w, bytes, sizeof bytes);
    assert(fips_fsp_session_setup_encode(&setup, &w, NULL) == FIPS_OK);
    dg.payload_len = fips_writer_len(&w); fipsd_dp_warm(h, &peer, &dg);
    assert(fipsd_peer_tokens_available(&peer.coord_hints, fips_test_instant(1000)) == available - FIPS_ADDRS_PER_ENTRY);
    assert(fips_coord_cache_get_entry(&h->cache, dg.src_addr.bytes));
    assert(fips_coord_cache_get_entry(&h->cache, protected.bytes));
    free(index); free(slots); free(h->tree); free(h);
    puts("coordinate warming: bounded hints, foreign-root/self refusal, endpoint CP guard: ok");
    return 0;
}
