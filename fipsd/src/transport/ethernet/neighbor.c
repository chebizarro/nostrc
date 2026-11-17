#include "transport/ethernet/internal.h"
#include <string.h>
bool fipsd_eth_group(const uint8_t mac[6]) { return (mac[0] & 1) != 0; }
size_t fipsd_eth_data_encode(uint8_t *out, size_t cap, const uint8_t *p, size_t n)
{
    if (n > UINT16_MAX || cap < n + 3 || (!p && n)) return 0;
    out[0] = 0; out[1] = (uint8_t)n; out[2] = (uint8_t)(n >> 8);
    if (n) memcpy(out + 3, p, n);
    return n + 3;
}
bool fipsd_eth_data_decode(const uint8_t *p, size_t n, const uint8_t **out, size_t *len)
{
    if (!p || n < 3 || p[0]) return false;
    size_t size = (size_t)p[1] | ((size_t)p[2] << 8);
    if (size > n - 3) return false;
    *out = p + 3; *len = size; return true;
}
void fipsd_eth_beacon_encode(uint8_t out[34], const fips_xonly_pubkey *key)
{ out[0] = 1; out[1] = 1; memcpy(out + 2, key->bytes, 32); }
bool fipsd_eth_beacon_decode(const fips_secp *secp, const uint8_t *p, size_t n, fips_xonly_pubkey *key)
{
    if (!secp || !p || n < 34 || p[0] != 1 || p[1] != 1) return false;
    fips_xonly_pubkey k; fips_peer_identity peer; memcpy(k.bytes, p + 2, 32);
    if (fips_peer_identity_from_pubkey(secp, &k, &peer) != FIPS_IDENTITY_OK) return false;
    *key = k; return true;
}
void fipsd_eth_neighbors_init(fipsd_eth_neighbors *b)
{ memset(b, 0, sizeof *b); nsync_mu_init(&b->mu); }
bool fipsd_eth_neighbors_add(fipsd_eth_neighbors *b, const uint8_t mac[6], const fips_xonly_pubkey *key)
{
    nsync_mu_lock(&b->mu);
    size_t i;
    for (i = 0; i < b->count; ++i) if (!memcmp(b->peers[i].mac, mac, 6)) break;
    if (i == b->count && b->count == FIPSD_ETH_NEIGHBORS) {
        ++b->dropped; nsync_mu_unlock(&b->mu); return false;
    }
    if (i < b->count) { memmove(&b->peers[i], &b->peers[i + 1], (b->count - i - 1) * sizeof b->peers[0]); --b->count; }
    fipsd_eth_neighbor *p = &b->peers[b->count++]; memcpy(p->mac, mac, 6); p->key = *key;
    nsync_mu_unlock(&b->mu); return true;
}
size_t fipsd_eth_neighbors_take(fipsd_eth_neighbors *b, fipsd_eth_neighbor *out, size_t cap)
{
    nsync_mu_lock(&b->mu); size_t n = b->count < cap ? b->count : cap;
    if (n) memcpy(out, b->peers, n * sizeof *out);
    memmove(b->peers, b->peers + n, (b->count - n) * sizeof *out); b->count -= n;
    nsync_mu_unlock(&b->mu); return n;
}
void fipsd_eth_neighbors_clear(fipsd_eth_neighbors *b)
{ nsync_mu_lock(&b->mu); b->count = 0; nsync_mu_unlock(&b->mu); }
/* Invalid/truncated captures are discarded, never passed to AEAD. */
bool fipsd_eth_bpf_next(const uint8_t *buf, size_t total, size_t *at, uint16_t type,
                      const uint8_t **out, size_t *len, uint8_t mac[6])
{
    while (*at < total) {
        size_t pos = *at; fipsd_eth_bpf_header h;
        if (total - pos < sizeof h) break;
        memcpy(&h, buf + pos, sizeof h);
        if (h.hdrlen < sizeof h || h.hdrlen > total - pos || h.caplen > total - pos - h.hdrlen) break;
        size_t end = pos + h.hdrlen + h.caplen;
        *at = (end + 3) & ~(size_t)3;
        if (h.caplen < 14 || h.caplen != h.datalen) continue;
        const uint8_t *eth = buf + pos + h.hdrlen;
        if (eth[12] != (uint8_t)(type >> 8) || eth[13] != (uint8_t)type) continue;
        memcpy(mac, eth + 6, 6); *out = eth + 14; *len = h.caplen - 14; return true;
    }
    *at = total; return false;
}
