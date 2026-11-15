#ifndef FIPSD_NODE_NETMON_H
#define FIPSD_NODE_NETMON_H
#include "runtime.h"
#include <swap.h>
#include <fips/utils/onlink.h>

#define FIPSD_NETMON_MAX_INTERFACES 256u
#define FIPSD_NETMON_MAX_ADDRESSES 1024u
#define FIPSD_INTERFACE_NAME_SIZE 64u
typedef struct fipsd_interface {
    char name[FIPSD_INTERFACE_NAME_SIZE];
    uint32_t index;
    bool up, loopback;
} fipsd_interface;
typedef struct fipsd_interface_address {
    uint32_t index;
    fips_ip_addr ip;
    uint8_t prefix;
} fipsd_interface_address;
/* Includes down and loopback interfaces for presence reporting; onlink excludes
 * both, as upstream read_system does. Core canonicalizes masks and /0 discards.
 * A failed/truncated read MUST NOT look like an authoritative empty read. */
typedef struct fipsd_network_snapshot {
    uint64_t generation;
    size_t interface_count, address_count;
    fipsd_interface interfaces[FIPSD_NETMON_MAX_INTERFACES];
    fipsd_interface_address addresses[FIPSD_NETMON_MAX_ADDRESSES];
    fips_onlink_prefix storage[FIPSD_NETMON_MAX_ADDRESSES];
    fips_onlink_prefixes onlink;
    bool failed;
} fipsd_network_snapshot;
/* Heap result; caller frees. Called through blocking executor in fibers. */
fipsd_network_snapshot *fipsd_onlink_read_system(void);
/* Rebuild onlink from an injected/raw snapshot; no I/O. */
bool fipsd_network_snapshot_prepare(fipsd_network_snapshot *);
typedef enum fipsd_netmon_event_kind {
    FIPSD_INTERFACE_UP, FIPSD_INTERFACE_DOWN, FIPSD_INTERFACE_ADDR_CHANGE,
    FIPSD_NETWORK_REFRESH, FIPSD_NETWORK_READ_FAILED
} fipsd_netmon_event_kind;
typedef struct fipsd_netmon_event {
    fipsd_netmon_event_kind kind;
    uint32_t index;
    uint64_t generation;
    bool backstop;
} fipsd_netmon_event;
typedef struct fipsd_netmon_source {
    /* Optional injected wake channel and reader. Reader runs off scheduler,
     * returns a separately owned snapshot. User/source outlive runtime join. */
    GoChannel *wake;
    fipsd_network_snapshot *(*read)(void *);
    void *user;
    bool injected; /* disables kernel backend; retains the 1 s backstop */
    GoChannel *ticks; /* injected tests only: manual backstop channel instead of Ticker */
} fipsd_netmon_source;
typedef struct fipsd_netmon fipsd_netmon;
fipsd_netmon *fipsd_netmon_create(const fipsd_netmon_source *);
int fipsd_netmon_start(fipsd_netmon *, fipsd_runtime *);
/* Prompt stop; destroys nothing. Runtime joins tracked fibers under its grace. */
void fipsd_netmon_stop(fipsd_netmon *);
void fipsd_netmon_destroy(fipsd_netmon *); /* after join */
GoChannel *fipsd_netmon_events(fipsd_netmon *); /* owned non-NULL malloc'd events */
GoRefPtr fipsd_netmon_snapshot(fipsd_netmon *);
/* Native backend diagnostic, -1 when timer-only. Valid until destroy. */
int fipsd_netmon_fd(const fipsd_netmon *);
#endif
