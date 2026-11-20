#ifndef FIPSD_NOSTR_ADVERT_H
#define FIPSD_NOSTR_ADVERT_H
#include "nostr/events.h"
typedef enum { FIPSD_NOSTR_UDP, FIPSD_NOSTR_TCP, FIPSD_NOSTR_TOR } fipsd_nostr_transport;
typedef struct { fipsd_nostr_transport transport; char addr[FIPSD_NOSTR_ADDR_MAX + 1]; } fipsd_nostr_endpoint;
typedef struct {
    size_t endpoint_count, relay_count, stun_count;
    fipsd_nostr_endpoint endpoints[FIPSD_NOSTR_ENDPOINTS_MAX];
    bool has_relays, has_stun;
    char relays[FIPSD_NOSTR_RELAYS_MAX][FIPSD_NOSTR_URL_MAX + 1];
    char stun[FIPSD_NOSTR_STUN_MAX][FIPSD_NOSTR_URL_MAX + 1];
} fipsd_nostr_advert;
/* One operational transport's advert inputs (upstream lifecycle
 * advert_transport_snapshot). `external` is the resolved external_addr
 * (fipsd_nostr_advert_external), `local` the bound socket address, both as
 * "ip:port"; `reflexive` is the STUN-observed public IP of a public UDP
 * socket, advertised with the bound port; `onion` the advertisable Tor
 * hostname, published as "<onion>:<advertised_port>". */
typedef struct {
    fipsd_nostr_transport transport;
    bool advertise, is_public; /* is_public: UDP only */
    const char *external, *local, *reflexive, *onion;
    uint16_t advertised_port;
} fipsd_nostr_advert_source;
/* Upstream driver build_overlay_advert, one transport: UDP public=false is
 * "nat"; public UDP and TCP prefer external_addr, then a non-wildcard bound
 * address, then (UDP only) the STUN-observed IP; Tor is its onion. False when
 * the transport contributes nothing; *why names an operator-visible omission
 * (wildcard bind without an address to publish), NULL for a silent one. */
bool fipsd_nostr_advert_source_endpoint(const fipsd_nostr_advert_source *, fipsd_nostr_endpoint *, const char **why);
/* Upstream external_advert_addr: `raw` as a socket address, else as a bare
 * IP with the port of `bind_addr` (itself a socket address). False when raw
 * or the needed port is absent or unparsable. Canonical Rust SocketAddr text. */
bool fipsd_nostr_advert_external(const char *raw, const char *bind_addr, char out[FIPSD_NOSTR_ADDR_MAX + 1]);
/* Upstream plan_publish: keep publicly usable endpoints, keep signalRelays and
 * stunServers only beside udp:nat (which requires both non-empty). False when
 * nothing publishable remains. */
bool fipsd_nostr_advert_publishable(fipsd_nostr_advert *);
char *fipsd_nostr_advert_content(const fipsd_nostr_advert *advert);
fipsd_nostr_status fipsd_nostr_advert_build(const fips_identity *identity,
    const fipsd_nostr_advert *advert, const char *app, uint64_t created_at_secs,
    uint64_t ttl_secs, NostrEvent **out);
/* Mirrors upstream endpoint filtering, NAT dependency, version and app checks;
 * future created_at is CLAMPED, not rejected. Explicit now avoids wall clocks. */
fipsd_nostr_status fipsd_nostr_advert_parse(const NostrEvent *event, const char *app,
    uint64_t now_ms, uint64_t max_age_ms, fipsd_nostr_advert *out, uint64_t *valid_until_ms);
#endif
