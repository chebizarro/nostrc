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
char *fipsd_nostr_advert_content(const fipsd_nostr_advert *advert);
fipsd_nostr_status fipsd_nostr_advert_build(const fips_identity *identity,
    const fipsd_nostr_advert *advert, const char *app, uint64_t created_at_secs,
    uint64_t ttl_secs, NostrEvent **out);
/* Mirrors upstream endpoint filtering, NAT dependency, version and app checks;
 * future created_at is CLAMPED, not rejected. Explicit now avoids wall clocks. */
fipsd_nostr_status fipsd_nostr_advert_parse(const NostrEvent *event, const char *app,
    uint64_t now_ms, uint64_t max_age_ms, fipsd_nostr_advert *out, uint64_t *valid_until_ms);
#endif
