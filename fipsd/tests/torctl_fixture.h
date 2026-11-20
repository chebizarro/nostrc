#ifndef FIPSD_TORCTL_FIXTURE_H
#define FIPSD_TORCTL_FIXTURE_H
#include <channel.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
/* A Tor control port on its own thread (control-spec), the C counterpart of
 * upstream tor/mock_control.rs, extended with asynchronous events:
 * AUTHENTICATE (a password or a 32-byte cookie in hex, or refuse), GETINFO
 * for the monitoring keys (batched or one key at a time, optionally refusing
 * the batch with 552 the way a daemon lacking one key does), the onion
 * service's current descriptor (hs/service/desc/id/, 552 unless set), SETEVENTS, and
 * 510 for anything else (ADD_ONION included). One client at a time; every
 * command line is reported on `commands`. torctl_fixture_event writes a 650
 * line to the client and updates the values GETINFO reports to match it. */
typedef struct torctl_fixture {
    int fd, stop[2];
    pthread_t thread;
    char addr[128];          /* control_addr: "127.0.0.1:port" or the socket path */
    char path[128];
    pthread_mutex_t mu;      /* everything below; serialises writes to client */
    int client;
    unsigned connections, authenticated;
    bool use_cookie, reject_auth, refuse_batch;
    bool descriptor;         /* GETINFO hs/service/desc/id/<x>: a current descriptor (else 552) */
    char password[64];
    uint8_t cookie[32];
    unsigned bootstrap;
    bool circuit, dormant;
    uint64_t traffic_read, traffic_written;
    char liveness[8], version[32];
    GoChannel *commands;     /* char *, owned by the receiver */
} torctl_fixture;
void torctl_fixture_start(torctl_fixture *, const char *unix_path /* NULL: TCP loopback */);
void torctl_fixture_stop(torctl_fixture *);
void torctl_fixture_event(torctl_fixture *, const char *event); /* without the "650 " */
void torctl_fixture_drop(torctl_fixture *);   /* end the current connection */
#endif
