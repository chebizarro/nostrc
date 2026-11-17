#ifndef FIPSD_MDNS_RESPONDER_H
#define FIPSD_MDNS_RESPONDER_H
/* DNS-SD responder for this node's one service instance: the mdns-sd
 * `register` half of upstream src/mdns/mod.rs, sans-IO (nostrc-9nxwk.5).
 *
 * It owns four names' records: the shared PTR `<service> -> <instance>`
 * (TTL 4500 s), the instance's SRV (TTL 120 s, naming the host and the UDP
 * port) and TXT (TTL 4500 s), and the host's A and AAAA records (TTL 120 s);
 * the TTLs are mdns-sd's defaults. On each interface it announces only that
 * interface's own addresses (RFC 6762 Section 6.2), IPv6 link-local ones
 * included, never loopback interfaces: upstream seeds `127.0.0.1`, but
 * mdns-sd sends on an interface only the addresses in that interface's
 * subnets and runs on no loopback interface, so the seed is never sent.
 *
 * Per interface it probes three times 250 ms apart after a random 0-250 ms
 * delay, then announces twice one second apart (Section 8). A conflicting
 * response while probing renames the instance and host (`<label>-2`,
 * `-3`, ...: the `-N` suffix the browser's host check accepts) and probes
 * again; after fifteen conflicts in ten seconds it waits five seconds
 * (Section 8.1). One after the names were won resets that interface to
 * probing (Section 9). A simultaneous probe for one of our names is settled
 * by the Section 8.2 comparison: the loser waits a second and probes again.
 *
 * Queries are answered on the interface they arrived on once it has won its
 * names. Answers the query already lists with at least half their TTL are
 * left out (Section 7.1). A legacy query (source port not 5353) gets a
 * unicast reply echoing its ID and questions, TTLs capped at 10 s and no
 * cache-flush bits (Section 6.7). A QU question gets a unicast reply when
 * the record was multicast on that interface within a quarter of its TTL,
 * a multicast one otherwise (Section 5.4). Multicast replies carrying only
 * unique records go at once, ones carrying the shared PTR after 20-120 ms
 * so answers aggregate (Section 6); no record is multicast on an interface
 * more than once a second, except 250 ms apart in defence of a probe.
 * A PTR answer carries the SRV, TXT and addresses as additional records, an
 * SRV answer the addresses (RFC 6763 Section 12).
 *
 * An address change on an announced interface re-announces it, carrying
 * goodbyes (TTL 0) for the addresses it lost; a port change re-announces
 * every interface; shutdown sends goodbyes for every record (Section 10.1).
 *
 * Time and randomness are inputs; packets due are returned, never sent. */
#include "mdns/advert.h"
#include "mdns/browser.h"
#include "mdns/records.h"

#define FIPSD_MDNS_HOST_TTL 120u
#define FIPSD_MDNS_OTHER_TTL 4500u
#define FIPSD_MDNS_PROBE_INTERVAL_MS 250u
#define FIPSD_MDNS_PROBES 3u
#define FIPSD_MDNS_ANNOUNCE_INTERVAL_MS 1000u
#define FIPSD_MDNS_ANNOUNCEMENTS 2u
#define FIPSD_MDNS_IFACE_ADDRS 8u
#define FIPSD_MDNS_MAX_INTERFACES 16u
#define FIPSD_MDNS_LEGACY_TTL 10u

/* One enabled interface and its own addresses. */
typedef struct fipsd_mdns_iface_addrs {
    uint32_t index;
    size_t v4_count, v6_count;
    uint8_t v4[FIPSD_MDNS_IFACE_ADDRS][4];
    uint8_t v6[FIPSD_MDNS_IFACE_ADDRS][16];
} fipsd_mdns_iface_addrs;
/* A packet to send: multicast on every family the interface has an address
 * of, or unicast to dst. */
typedef struct fipsd_mdns_out {
    uint32_t ifindex;
    bool unicast;
    fipsd_mdns_sockaddr dst;
    size_t len;
    uint8_t bytes[FIPSD_MDNS_MAX_PACKET];
} fipsd_mdns_out;
typedef struct fipsd_mdns_responder_config {
    const char *service_type;
    const char *label;  /* fipsd_mdns_advert_label of the node's npub */
    const fipsd_mdns_txt_entry *txt;
    size_t txt_count;
    fipsd_mdns_rng rng;
} fipsd_mdns_responder_config;
typedef struct fipsd_mdns_responder fipsd_mdns_responder;
fipsd_mdns_responder *fipsd_mdns_responder_create(const fipsd_mdns_responder_config *cfg);
void fipsd_mdns_responder_destroy(fipsd_mdns_responder *r);
/* The advertised UDP port; 0 holds every interface idle until one is set. */
void fipsd_mdns_responder_set_port(fipsd_mdns_responder *r, uint16_t port, uint64_t now_ms);
/* The current enabled interfaces, replacing the previous set. */
void fipsd_mdns_responder_set_interfaces(fipsd_mdns_responder *r, const fipsd_mdns_iface_addrs *ifaces, size_t n,
                                         uint64_t now_ms);
/* A message received on ifindex from src (src_port its UDP source port).
 * own: the engine recognised it as one this node sent, looped back. */
void fipsd_mdns_responder_on_message(fipsd_mdns_responder *r, const fipsd_mdns_message *msg, uint32_t ifindex,
                                     const fipsd_mdns_sockaddr *src, bool own, uint64_t now_ms);
/* Packets due at now_ms, at most cap. */
size_t fipsd_mdns_responder_poll(fipsd_mdns_responder *r, uint64_t now_ms, fipsd_mdns_out *out, size_t cap);
/* Goodbye packets for every interface that announced. */
size_t fipsd_mdns_responder_goodbye(fipsd_mdns_responder *r, fipsd_mdns_out *out, size_t cap);
/* The next instant poll has work; UINT64_MAX when none. */
uint64_t fipsd_mdns_responder_deadline(const fipsd_mdns_responder *r);
/* The label in use (the configured one, or it with a conflict suffix). */
const char *fipsd_mdns_responder_label(const fipsd_mdns_responder *r);
/* Whether ifindex has won its names and announced. */
bool fipsd_mdns_responder_announced(const fipsd_mdns_responder *r, uint32_t ifindex);
#endif
