#ifndef FIPSD_TCP_INTERNAL_H
#define FIPSD_TCP_INTERNAL_H
#include "transport/tcp/tcp.h"
#include "transport/stream.h"
#include "transport/sockbuf.h"
#include "transport/watcher.h"
#include "libgo/fiber.h"
#include "select.h"
#include <sys/socket.h>
#include <netinet/in.h>
typedef struct tcp_instance tcp_instance;
typedef struct tcp_conn tcp_conn;
typedef struct tcp_endpoint { struct sockaddr_storage address; socklen_t len; } tcp_endpoint;
typedef struct tcp_frame { size_t len; uint8_t bytes[]; } tcp_frame;
typedef struct tcp_stats {
    _Atomic uint64_t accepted, connected, connect_timeouts, connect_refused, send_errors, recv_errors;
    _Atomic uint64_t rejected, source_rejected, mtu_exceeded, deadlines;
} tcp_stats;
struct tcp_conn {
    tcp_instance *owner;
    tcp_conn *next;
    uint64_t id, accepted_ms;
    fips_transport_addr peer;
    tcp_endpoint remote, local;
    fips_link_direction direction;
    int fd; /* owner.mu; published before connect; close only after I/O join */
    uint16_t mss_mtu; /* diagnostic, never a frame bound */
    bool established, closing; /* owner.mu */
    GoChannel *stop, *notify_stop, *close, *ready, *run, *reader_done, *writer_done, *progress, *queue;
    _Atomic uint64_t last_frame_ms;
};
struct tcp_instance {
    fipsd_transport transport;
    fipsd_transport_shared shared;
    fipsd_tcp_config config;
    tcp_stats stats;
    char name[128], bind_addr[257];
    fipsd_runtime *runtime;
    GoContext *root;
    GoChannel *events, *stop, *bound, *accept_done, *changed;
    fipsd_watcher *watcher;
    nsync_mu mu;
    tcp_conn *connections;
    int listener_fd;
    tcp_endpoint local;
    uint64_t generation;
    size_t inbound, outbound;
    bool started, binding;
};
/* pool calls hold mu; no operation under mu may park a fiber. */
tcp_conn *tcp_pool_find(tcp_instance *, const fips_transport_addr *);
int tcp_pool_launch(tcp_instance *, int fd, const fips_transport_addr *,
    const tcp_endpoint *, const tcp_endpoint *, fips_link_direction);
void tcp_pool_remove(tcp_conn *);
void tcp_pool_stop(tcp_instance *);
bool tcp_same_source(const tcp_endpoint *, const tcp_endpoint *);
void tcp_conn_owner(void *);
int tcp_connected_locked(tcp_conn *); /* mu held; never parks */
void tcp_conn_free(tcp_conn *);
int tcp_io_errno(void); /* reacquire TLS after a migrating fiber resumes */
int tcp_socket_configure(tcp_instance *, int);
uint16_t tcp_socket_mss(int, uint16_t);
void tcp_fd_close(int); /* joins precede this call */
bool tcp_endpoint_addr(const tcp_endpoint *, fips_transport_addr *);
bool tcp_resolve(const fips_transport_addr *, bool numeric, tcp_endpoint **, size_t *);
fipsd_bind_result tcp_bind(void *, fipsd_runtime *, GoChannel *, const fipsd_presence_facts *, uint64_t);
void tcp_unbind(void *, uint64_t);
void tcp_stats_init(tcp_stats *);
void tcp_stats_error(tcp_instance *, bool send);
#endif
