#ifndef FIPSD_MDNS_ADVERT_H
#define FIPSD_MDNS_ADVERT_H
/* The FIPS LAN advert: names, TXT keys and the browser's service-level
 * checks, ported from upstream src/mdns/mod.rs (nostrc-9nxwk.5).
 *
 * A node publishes a `_fips._udp.local.` DNS-SD service whose instance and
 * host label is `fips-` and the first 16 characters of its npub, with TXT
 * `npub=<npub>`, `scope=<scope>` (when it has one) and `v=1`. The browser
 * surfaces a resolved advert only when it names an npub other than ours,
 * matches our scope when we have one, names the advertised npub's own host
 * and gives a port; each of its addresses then becomes one discovered peer,
 * an IPv6 link-local address only with the interface scope it arrived on.
 *
 * mDNS adverts are unauthenticated routing hints: identity is proven by the
 * Noise IK handshake the node then initiates. Pure: no I/O, no clock. */
#include "mdns/records.h"
#include <fips/utils/onlink.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_MDNS_SERVICE_TYPE "_fips._udp.local."
#define FIPSD_MDNS_TXT_NPUB "npub"
#define FIPSD_MDNS_TXT_SCOPE "scope"
#define FIPSD_MDNS_TXT_VERSION "v"
/* Kept in sync with the Nostr rendezvous PROTOCOL_VERSION ("1"). */
#define FIPSD_MDNS_TXT_PROTOCOL_VERSION "1"
/* `fips-` and 16 characters, a NUL. */
#define FIPSD_MDNS_LABEL_SIZE 22u
#define FIPSD_MDNS_MAX_ADDRS 16u
#define FIPSD_MDNS_MAX_IFACES 8u
#define FIPSD_MDNS_TEXT_MAX 256u

/* An address record as the browser holds it: an IPv4 one with every
 * interface it was seen on, an IPv6 one with the interface it arrived on as
 * its scope (0 when unknown), as mdns-sd's ScopedIp records them. */
typedef struct fipsd_mdns_scoped_ip {
    fips_ip_addr ip;
    uint32_t scope;
    uint32_t ifaces[FIPSD_MDNS_MAX_IFACES];
    size_t iface_count;
} fipsd_mdns_scoped_ip;
/* A resolved service: its SRV, TXT and the SRV host's addresses. */
typedef struct fipsd_mdns_resolved {
    char fullname[FIPSD_MDNS_TEXT_MAX];
    char host[FIPSD_MDNS_TEXT_MAX];
    uint16_t port;
    uint16_t txt_len;
    uint8_t txt[FIPSD_MDNS_MAX_RDATA];
    size_t addr_count;
    fipsd_mdns_scoped_ip addrs[FIPSD_MDNS_MAX_ADDRS];
} fipsd_mdns_resolved;
/* A socket address: IP, port and (IPv6) scope id. */
typedef struct fipsd_mdns_sockaddr {
    fips_ip_addr ip;
    uint16_t port;
    uint32_t scope_id;
} fipsd_mdns_sockaddr;

/* `fips-` and npub's first 16 bytes into out; false when npub is shorter or
 * those bytes are not ASCII (upstream advert_label_of). */
bool fipsd_mdns_advert_label_of(const char *npub, size_t len, char out[FIPSD_MDNS_LABEL_SIZE]);
/* For this node's own npub; `fips-<npub>` when it has no ASCII prefix. */
void fipsd_mdns_advert_label(const char *npub, char *out, size_t cap);
/* Whether host is the advertiser's: its label under `.local.`/`.local`, or
 * that label with a `-N` conflict suffix, compared without ASCII case. */
bool fipsd_mdns_host_is_advertisers(const char *host, const char *npub, size_t npub_len);

typedef enum fipsd_mdns_skip {
    FIPSD_MDNS_ADVERT_OK,
    FIPSD_MDNS_SKIP_NO_NPUB,      /* no `npub` TXT entry */
    FIPSD_MDNS_SKIP_OWN,          /* our own advert, echoed back */
    FIPSD_MDNS_SKIP_CROSS_SCOPE,  /* a discovery scope other than ours */
    FIPSD_MDNS_SKIP_FOREIGN_HOST, /* the SRV host is not the npub's own */
    FIPSD_MDNS_SKIP_ZERO_PORT     /* no port to dial */
} fipsd_mdns_skip;
/* TXT values as byte strings: a value may hold any byte, NUL included. */
typedef struct fipsd_mdns_advert_fields {
    char npub[FIPSD_MDNS_TEXT_MAX];
    size_t npub_len;
    bool has_scope;
    char scope[FIPSD_MDNS_TEXT_MAX];
    size_t scope_len;
    uint16_t port;
} fipsd_mdns_advert_fields;
/* Upstream check_advert. The last `npub`/`scope` entry wins, as upstream's
 * property loop. *out is filled as far as the checks got, so a skip can be
 * logged with the advert's npub and scope. scope_filter NULL: no filter. */
fipsd_mdns_skip fipsd_mdns_check_advert(const fipsd_mdns_resolved *info, const char *own_npub,
                                        const char *scope_filter, fipsd_mdns_advert_fields *out);
/* The debug line upstream's log_skipped_advert writes for skip, or "" for the
 * silent skips (our own echo, a zero port). Returns out. */
const char *fipsd_mdns_skip_line(const fipsd_mdns_resolved *info, fipsd_mdns_skip skip,
                                 const fipsd_mdns_advert_fields *fields, const char *scope_filter,
                                 char *out, size_t cap);
/* The first 16 bytes of npub cut back to a UTF-8 character boundary. */
size_t fipsd_mdns_short_len(const char *npub, size_t len);
/* Upstream socket_addr_from_scoped_ip: false for a scope-less IPv6
 * link-local address, which cannot be dialled unambiguously. */
bool fipsd_mdns_socket_addr(const fipsd_mdns_scoped_ip *scoped, uint16_t port, fipsd_mdns_sockaddr *out);
/* Upstream arrival_interfaces: the non-zero interfaces an IPv4 record was
 * seen on, or the scope of an IPv6 one. */
size_t fipsd_mdns_arrival_interfaces(const fipsd_mdns_scoped_ip *scoped, uint32_t out[FIPSD_MDNS_MAX_IFACES]);
bool fipsd_mdns_ipv6_is_unicast_link_local(const uint8_t addr[16]);
/* "1.2.3.4:5" or "[fe80::1%2]:5", as Rust's SocketAddr displays. */
size_t fipsd_mdns_sockaddr_text(const fipsd_mdns_sockaddr *a, char *out, size_t cap);
/* The LAN-only discovery scope (upstream lan_rendezvous_scope): a non-empty
 * trimmed `lan_scope`, else the trimmed Nostr `app` with any
 * `fips-overlay-v1:` prefix stripped; false (no scope) when both are empty. */
bool fipsd_mdns_resolve_scope(const char *lan_scope, const char *nostr_app, char *out, size_t cap);
#endif
