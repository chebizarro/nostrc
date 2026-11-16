#ifndef FIPSD_NOSTR_STUN_H
#define FIPSD_NOSTR_STUN_H
#include "transport/udp/udp.h"
#define FIPSD_STUN_REQUEST_SIZE 20u
#define FIPSD_STUN_TRAVERSAL_MS UINT64_C(2000)
#define FIPSD_STUN_ADVERT_MS UINT64_C(5000)
#define FIPSD_STUN_RTO_MS UINT64_C(500)
typedef enum { FIPSD_STUN_WAIT, FIPSD_STUN_SEND, FIPSD_STUN_FAILED, FIPSD_STUN_DONE } fipsd_stun_action;
/* Single actor owns transactions. Source + transaction id both must match.
 * tick emits sends at 0, 500, 1500 (and 3500 for advert discovery) ms.
 * No private socket, clock reads, sleeps or receive-draining side channel. */
typedef struct {
    uint8_t id[12], request[FIPSD_STUN_REQUEST_SIZE];
    fips_transport_addr server, mapped;
    uint64_t deadline, resend_at, rto;
    bool active, complete;
} fipsd_stun;
void fipsd_stun_request(const uint8_t id[12], uint8_t out[FIPSD_STUN_REQUEST_SIZE]);
bool fipsd_stun_packet(const uint8_t *, size_t);
bool fipsd_stun_response(const uint8_t *, size_t, const uint8_t id[12], fips_transport_addr *);
/* Accept upstream stun:host:port, host:port and bracketed IPv6 spellings. */
bool fipsd_stun_server(const char *, fips_transport_addr *);
void fipsd_stun_begin(fipsd_stun *, const fips_transport_addr *, const uint8_t id[12], uint64_t now, uint64_t timeout);
fipsd_stun_action fipsd_stun_tick(fipsd_stun *, uint64_t now);
bool fipsd_stun_receive(fipsd_stun *, const fips_transport_addr *, const uint8_t *, size_t);
uint64_t fipsd_stun_deadline(const fipsd_stun *);
/* SEND effect uses this transport's bound listener, also used by FMP/punch. */
int fipsd_stun_send(const fipsd_stun *, fipsd_transport *);
#endif
