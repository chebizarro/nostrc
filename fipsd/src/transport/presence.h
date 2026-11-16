#ifndef FIPSD_PRESENCE_H
#define FIPSD_PRESENCE_H
#include "transport/medium.h"
#include "node/netmon.h"
#include <ifaddrs.h>
#include <nsync.h>
typedef enum fipsd_presence_phase { FIPSD_ABSENT, FIPSD_BINDING, FIPSD_PRESENT } fipsd_presence_phase;
typedef struct fipsd_presence_facts {
    bool valid, exists, up, carrier, has_mac;
    uint32_t index;
    uint8_t mac[6];
    fips_path_medium medium;
} fipsd_presence_facts;
typedef struct fipsd_presence_view {
    fipsd_presence_phase phase;
    fipsd_presence_facts interface;
    uint64_t since_ms, binds;
    uint32_t attempts;
    bool optional;
} fipsd_presence_view;
typedef struct fipsd_presence { nsync_mu mu; fipsd_presence_view view; } fipsd_presence;
void fipsd_presence_init(fipsd_presence *, bool optional, uint64_t now);
fipsd_presence_view fipsd_presence_snapshot(fipsd_presence *);
void fipsd_presence_transition(fipsd_presence *, fipsd_presence_phase, uint64_t now);
void fipsd_presence_observe(fipsd_presence *, fipsd_presence_facts);
void fipsd_presence_bound(fipsd_presence *, fipsd_presence_facts, uint64_t now);
uint32_t fipsd_presence_attempt(fipsd_presence *);
uint64_t fipsd_bind_backoff_ms(uint32_t);
const char *fipsd_presence_name(fipsd_presence_phase);
fipsd_presence_facts fipsd_presence_from_snapshot(const fipsd_network_snapshot *, const char *);
/* No I/O; supports injected ifaddrs and index resolver. */
fipsd_presence_facts fipsd_presence_from_ifaddrs(const struct ifaddrs *, const char *,
                                               unsigned (*index_of)(const char *));
fipsd_presence_facts fipsd_presence_read(const char *); /* blocking executor */
uint64_t fipsd_transport_now_ms(void);
#endif
