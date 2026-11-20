#ifndef FIPSD_TOR_CONTROL_H
#define FIPSD_TOR_CONTROL_H
#include "transport/tor/tor.h"
#include "transport/stats_common.h"
#include <nsync.h>
/* Tor control-port monitor (control-spec AUTHENTICATE, GETINFO, SETEVENTS).
 * One fiber owns one connection: it authenticates (cookie or password),
 * takes the upstream monitoring snapshot once with GETINFO, then subscribes
 * with SETEVENTS STATUS_CLIENT NETWORK_LIVENESS BW and updates the snapshot
 * from Tor's asynchronous 650 events (BOOTSTRAP progress, CIRCUIT_*,
 * liveness, bandwidth totals), re-reading GETINFO on each status event; no
 * periodic polling. With a service set it also follows HS_DESC and asks once
 * per session whether Tor already holds the service's current descriptor. A lost or refused connection counts control_errors and
 * is retried with exponential backoff until stop. */
typedef enum tor_control_auth { TOR_CONTROL_COOKIE, TOR_CONTROL_PASSWORD } tor_control_auth;
/* Onion service publication as the monitor learns it: Tor uploaded the
 * descriptor (650 HS_DESC UPLOADED <service>), already held a current one
 * when the monitor subscribed (GETINFO hs/service/desc/id/<service>), or the
 * monitor could not complete its first session, so cannot confirm either. */
typedef enum { TOR_DESCRIPTOR_UPLOADED, TOR_DESCRIPTOR_CURRENT, TOR_DESCRIPTOR_UNMONITORED } tor_descriptor_signal;
typedef struct tor_control {
    const char *owner;            /* transport name for logs */
    fipsd_transport_stats *stats; /* control_errors */
    char addr[257], cookie_path[1024], password[256];
    tor_control_auth auth;
    uint32_t backoff_ms, stall_ms;
    nsync_mu mu;
    int fd;                       /* mu: published for owner-directed cancellation */
    bool has_info;                /* mu */
    fipsd_tor_monitoring info;    /* mu */
    GoChannel *stop, *done, *updates;
    /* Set before tor_control_start when an onion is served: the v3 address
     * without ".onion" adds HS_DESC to the subscription. Monitor fiber calls. */
    char service[64];
    void (*descriptor)(void *, tor_descriptor_signal);
    void *descriptor_user;
    bool subscribed;              /* fiber: a session's SETEVENTS was accepted */
    fipsd_runtime *rt;
    GoContext *root;
    bool started, warned, stall_warned, refresh, was_dormant;
    uint64_t started_ms;
    uint8_t last_bootstrap;
    char last_liveness[16];
    char buf[4096];               /* line assembly */
    size_t used;
} tor_control;
/* Parses the auth spec; -1 for an unknown form or oversized values. */
int tor_control_init(tor_control *, const char *owner, fipsd_transport_stats *, const char *addr,
    const char *auth, const char *cookie_path, uint32_t backoff_ms, uint32_t stall_ms);
/* On the start fiber: connect, authenticate and snapshot once (control_port
 * mode's fatal start check); the open session is handed to the monitor. */
int tor_control_connect_now(tor_control *, fipsd_runtime *);
int tor_control_start(tor_control *, fipsd_runtime *); /* spawns the monitor */
void tor_control_stop(tor_control *);    /* prompt, idempotent */
void tor_control_close(tor_control *);   /* off-fiber: release a session no monitor took */
void tor_control_destroy(tor_control *); /* after runtime join */
bool tor_control_snapshot(tor_control *, fipsd_tor_monitoring *);
#endif
