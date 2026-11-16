/* Plaintext FSP coordinate hints: the upstream forwarding intake, before
 * route resolution. They are hints, never signed/verified cache entries. */
#include "node/handlers/internal.h"

void fipsd_dp_hint(handlers *h, peer_rec *from, const fips_node_addr *key, fips_fsp_coords coords)
{
    if (!coords.count || coords.count > FIPS_CACHED_COORD_MAX_ENTRIES) return;
    const fips_node_addr *root = fips_tree_state_root(h->tree);
    if (memcmp(coords.addrs + 16 * (coords.count - 1), root->bytes, 16)) {
        METRIC(h, forwarding, coord_warm_foreign_root); return;
    }
    if (memcmp(key->bytes, coords.addrs, 16)) METRIC(h, forwarding, coord_warm_key_mismatch);
    if (!memcmp(key->bytes, h->me.bytes, 16)) return;
    fips_cached_coord c;
    if (fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])coords.addrs, coords.count) != FIPS_OK) return;
    fips_instant now = hs_now(h);
    if (!from->coord_hints.refill.ops)
        fipsd_peer_tokens_init(&from->coord_hints, 64 * FIPS_ADDRS_PER_ENTRY, 256 * FIPS_ADDRS_PER_ENTRY, 0, now);
    fips_funding funding = FIPS_FUNDING_UPTO(fipsd_peer_tokens_available(&from->coord_hints, now));
    fips_placed placed;
    /* An unfunded hint still routes from bounded probation; it cannot evict
     * funded/verified main entries. Charge only newly occupied cache weight. */
    if (fips_coord_cache_insert_funded(&h->cache, key->bytes, &c, now, funding, &placed) != FIPS_OK) return;
    if (placed.added_weight) (void)fipsd_peer_tokens_spend(&from->coord_hints, placed.added_weight, now);
    if (placed.unfunded) METRIC(h, forwarding, coord_hint_unfunded);
    h->dataplane->needs_publish = true;
}
/* A session-authenticated peer's own statement is bounded by session
 * admission, not charged to the carrier. Unlike transit hints it must not
 * remain in one-entry probation and disappear behind unrelated hints. */
void fipsd_dp_stated(handlers *h, const fips_node_addr *peer, fips_fsp_coords coords)
{
    if (!coords.count || coords.count > FIPS_CACHED_COORD_MAX_ENTRIES ||
        !memcmp(peer->bytes, h->me.bytes, 16)) return;
    if (memcmp(coords.addrs + 16 * (coords.count - 1), fips_tree_state_root(h->tree)->bytes, 16)) {
        METRIC(h, forwarding, coord_warm_foreign_root); return;
    }
    fips_cached_coord c; fips_placed placed;
    if (fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])coords.addrs, coords.count) != FIPS_OK) return;
    if (fips_coord_cache_insert_stated(&h->cache, peer->bytes, &c, hs_now(h), FIPS_FUNDING_UNCHARGED, &placed) != FIPS_OK) return;
    h->dataplane->needs_publish = true;
}
void fipsd_dp_warm(handlers *h, peer_rec *from, const fips_session_datagram *dg)
{
    fips_fsp_common_prefix prefix;
    if (!fips_fsp_common_prefix_parse(dg->payload, dg->payload_len, &prefix)) return;
    fips_reader reader;
    fips_reader_init(&reader, dg->payload + FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE,
        dg->payload_len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE);
    fips_fsp_coords src = {0}, dest = {0};
    if (prefix.phase == FIPS_FSP_PHASE_MSG1) {
        fips_fsp_session_setup m;
        if (fips_fsp_session_setup_decode(&reader, &m, NULL) != FIPS_OK) return;
        src = m.src_coords; dest = m.dest_coords;
    } else if (prefix.phase == FIPS_FSP_PHASE_MSG2) {
        fips_fsp_session_ack m;
        if (fips_fsp_session_ack_decode(&reader, &m, NULL) != FIPS_OK) return;
        src = m.src_coords; dest = m.dest_coords;
    } else if (prefix.phase == FIPS_FSP_PHASE_ESTABLISHED && fips_fsp_common_prefix_has_coords(&prefix)) {
        /* Endpoints leave established-message coordinates to authenticated
         * session intake; an unauthenticated CP header must not warm there. */
        if (!memcmp(dg->dest_addr.bytes, h->me.bytes, 16)) return;
        fips_fsp_encrypted_header header;
        if (!fips_fsp_encrypted_header_parse(dg->payload, dg->payload_len, &header)) return;
        fips_reader_init(&reader, dg->payload + FIPS_FRAMING_FSP_HEADER_SIZE,
            dg->payload_len - FIPS_FRAMING_FSP_HEADER_SIZE);
        if (fips_fsp_parse_encrypted_coords(&reader, &src, &dest, NULL) != FIPS_OK) return;
    } else return;
    fipsd_dp_hint(h, from, &dg->src_addr, src);
    fipsd_dp_hint(h, from, &dg->dest_addr, dest);
}
