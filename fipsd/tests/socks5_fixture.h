#ifndef FIPSD_SOCKS5_FIXTURE_H
#define FIPSD_SOCKS5_FIXTURE_H
#include <channel.h>
#include <fips/transport/types.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
/* Loopback SOCKS5 server (RFC 1928 / RFC 1929) on its own threads: the C
 * port of upstream transport/socks5/mock.rs. Negotiates no-auth or
 * username/password (preferring the latter when offered, as the mock does),
 * accepts CONNECT to IPv4, IPv6 and domain targets, then relays bytes: a
 * numeric target is dialled as requested, a domain (".onion" included)
 * through the route the test registered for it. Every CONNECT request is
 * reported on `requests`. The behaviour switch drives the client's failure
 * paths; a connection that closes before its greeting (the Nym readiness
 * probe) is ignored, as upstream's per-connection task ignores it. */
typedef enum s5_behaviour {
    S5_RELAY,     /* success reply, then relay */
    S5_REPLY,     /* reply with `rep` (0x05 refused, 0x01 general failure) and close */
    S5_AUTH_FAIL, /* RFC 1929 status 0x01 */
    S5_NO_METHOD, /* method reply 0xff */
    S5_STALL      /* read the greeting and never answer it */
} s5_behaviour;
typedef struct s5_request {
    uint8_t greeting[8];
    size_t greeting_len;
    bool userpass;
    char username[256], password[256];
    uint8_t atyp;
    char host[256]; /* domain, or the numeric address text */
    uint16_t port;
} s5_request;
#define S5_ROUTES 8
#define S5_CONNS 128
typedef struct s5_route { char host[256]; struct sockaddr_storage to; socklen_t len; } s5_route;
typedef struct socks5_fixture {
    int fd, stop[2];
    pthread_t thread;
    pthread_mutex_t mu; /* everything below */
    pthread_t conns[S5_CONNS];
    size_t conn_count;
    s5_behaviour behaviour;
    uint8_t rep;
    s5_route routes[S5_ROUTES];
    size_t route_count;
    char addr[64];            /* "127.0.0.1:port", the socks5_addr to configure */
    fips_transport_addr address;
    GoChannel *requests;      /* s5_request *, owned by the receiver */
    GoChannel *stalled;       /* one token per connection parked by S5_STALL */
} socks5_fixture;
void socks5_fixture_start(socks5_fixture *);
void socks5_fixture_stop(socks5_fixture *); /* joins every connection thread */
void socks5_fixture_behave(socks5_fixture *, s5_behaviour, uint8_t rep);
/* Relay a CONNECT for `host` (a domain) to the numeric `to`. */
void socks5_fixture_route(socks5_fixture *, const char *host, const fips_transport_addr *to);
#endif
