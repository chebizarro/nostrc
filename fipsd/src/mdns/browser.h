#ifndef FIPSD_MDNS_BROWSER_H
#define FIPSD_MDNS_BROWSER_H
/* DNS-SD browser for one service type: the mdns-sd `browse` half of
 * upstream src/mdns/mod.rs, as a sans-IO record cache (nostrc-9nxwk.5).
 *
 * Responses received on any enabled interface are cached when they concern
 * the service: PTR records naming its instances, those instances' SRV and
 * TXT records, and the address records of the hosts the SRV records name.
 * An IPv4 address record remembers every interface it arrived on; an IPv6
 * one is keyed by the interface it arrived on, its scope. Probe authority
 * records and a query's known answers are never cached.
 *
 * Lifetimes follow RFC 6762: a record lives for its TTL; a goodbye (TTL 0,
 * Section 10.1) removes it from resolution at once and from the cache one
 * second later; a record with the cache-flush bit (Section 10.2) expires,
 * one second later, every cached record of its name and type that was not
 * received in the last second. Querying is continuous (Section 5.2): the
 * first query 20-120 ms after start, then at intervals doubling from one
 * second to an hour, carrying known answers whose remaining TTL is over half
 * (Section 7.1); records still wanted are re-queried at 80, 85, 90 and 95%
 * of their TTL plus up to 2% jitter; an instance missing its SRV, TXT or
 * addresses is asked for them at once, then one and three seconds later.
 *
 * An instance is resolved when it has an SRV, a TXT and at least one
 * address. Each change of a resolved instance's SRV, TXT or address set is
 * reported once (mdns-sd's ServiceResolved), so repeated identical
 * announcements are not; an instance that stops resolving is reported as
 * removed, and reported again in full should it return.
 *
 * Time (monotonic milliseconds) and randomness are inputs. Single owner. */
#include "mdns/advert.h"
#include "mdns/records.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_MDNS_CACHE_MAX 256u
#define FIPSD_MDNS_INSTANCES_MAX 64u
#define FIPSD_MDNS_QUERY_MAX_INTERVAL_MS 3600000u

/* bound > 0; returns a value in [0, bound). */
typedef struct fipsd_mdns_rng { uint32_t (*uniform)(void *user, uint32_t bound); void *user; } fipsd_mdns_rng;

typedef struct fipsd_mdns_browser fipsd_mdns_browser;
fipsd_mdns_browser *fipsd_mdns_browser_create(const char *service_type, fipsd_mdns_rng rng, uint64_t now_ms);
void fipsd_mdns_browser_destroy(fipsd_mdns_browser *b);
/* One received message from interface ifindex (0 unknown). */
void fipsd_mdns_browser_ingest(fipsd_mdns_browser *b, const fipsd_mdns_message *msg, uint32_t ifindex, uint64_t now_ms);
/* Drop records whose lifetime has passed. */
void fipsd_mdns_browser_expire(fipsd_mdns_browser *b, uint64_t now_ms);
/* Interface ifindex went away: forget what was learnt only on it. */
void fipsd_mdns_browser_forget_interface(fipsd_mdns_browser *b, uint32_t ifindex);
/* An interface appeared: query again 20-120 ms from now, the backoff reset. */
void fipsd_mdns_browser_restart(fipsd_mdns_browser *b, uint64_t now_ms);
/* The next resolved instance whose records changed since it was last
 * reported; false when none. */
bool fipsd_mdns_browser_next_resolved(fipsd_mdns_browser *b, fipsd_mdns_resolved *out);
/* The next instance that stopped resolving; false when none. */
bool fipsd_mdns_browser_next_removed(fipsd_mdns_browser *b, char *fullname, size_t cap);
/* The query due at now_ms, or 0 when none is (or it does not fit cap). */
size_t fipsd_mdns_browser_query(fipsd_mdns_browser *b, uint64_t now_ms, uint8_t *buf, size_t cap);
/* The earliest instant anything above is due; UINT64_MAX when none. */
uint64_t fipsd_mdns_browser_deadline(const fipsd_mdns_browser *b);
/* Cached record count, for tests. */
size_t fipsd_mdns_browser_cached(const fipsd_mdns_browser *b);
#endif
