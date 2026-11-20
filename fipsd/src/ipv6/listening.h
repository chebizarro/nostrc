#ifndef FIPSD_IPV6_LISTENING_H
#define FIPSD_IPV6_LISTENING_H
/* Listening-socket enumeration for the "Listening on fips0" panel (upstream
 * listening.rs): /proc/net/tcp6 and /proc/net/udp6 rows paired with the owning
 * PID/process by walking /proc/<pid>/fd/, filtered to sockets reachable from
 * the TUN (bound to :: or to the node's own fd00::/8 address). IPv4 listeners
 * are not enumerated; the TUN is IPv6-only. Linux-only; other hosts get none. */
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum fipsd_ipv6_proto { FIPSD_IPV6_PROTO_TCP, FIPSD_IPV6_PROTO_UDP } fipsd_ipv6_proto;
const char *fipsd_ipv6_proto_str(fipsd_ipv6_proto proto);
#define FIPSD_IPV6_PROCESS_NAME 32u
#define FIPSD_IPV6_TCP_LISTEN 0x0au
typedef struct fipsd_ipv6_listening_socket {
    fipsd_ipv6_proto proto;
    uint8_t local_addr[16];
    uint16_t port;
    bool has_pid;
    uint32_t pid;
    char process[FIPSD_IPV6_PROCESS_NAME];
    bool wildcard_bind;  /* bound to :: rather than the TUN address */
} fipsd_ipv6_listening_socket;
typedef struct fipsd_ipv6_inode_owner { uint64_t inode; uint32_t pid; char comm[FIPSD_IPV6_PROCESS_NAME]; } fipsd_ipv6_inode_owner;
typedef struct fipsd_ipv6_inode_map { fipsd_ipv6_inode_owner *entries; size_t count, capacity; } fipsd_ipv6_inode_map;
bool fipsd_ipv6_inode_map_get(const fipsd_ipv6_inode_map *map, uint64_t inode, uint32_t *pid, char comm[FIPSD_IPV6_PROCESS_NAME]);
/* One /proc/net/{tcp6,udp6} row: local/remote address and port (kernel word
 * order decoded on this host), state and inode. False for the header row or
 * a malformed one. */
bool fipsd_ipv6_listening_parse_row(const char *row, uint8_t local[16], uint16_t *port, uint8_t remote[16],
                                    uint16_t *remote_port, unsigned *state, uint64_t *inode);
/* Decide whether a socket is reachable from the TUN and build its row: a v4
 * (or v4-mapped) address, ::1 or any other specific address is not. */
bool fipsd_ipv6_listening_build_entry(fipsd_ipv6_proto proto, bool is_v6, const uint8_t local[16], uint16_t port,
                                      uint64_t inode, const fipsd_ipv6_inode_map *map, const uint8_t fips0[16],
                                      fipsd_ipv6_listening_socket *out);
/* Stable order: proto, then port, then PID (unknown first). */
void fipsd_ipv6_listening_sort(fipsd_ipv6_listening_socket *sockets, size_t count);
/* Linux: the sorted sockets reachable from the TUN, at most cap; elsewhere 0. */
size_t fipsd_ipv6_listening_enumerate(const uint8_t fips0[16], fipsd_ipv6_listening_socket *out, size_t cap);
/* The panel's reply (upstream status.rs render): each socket classified
 * against the loaded firewall when it polices tun, else as unprotected. */
struct fipsd_ipv6_filter_classifier;
json_t *fipsd_ipv6_listening_render(const struct fipsd_ipv6_filter_classifier *classifier, const char *tun,
                                    const uint8_t fips0[16], const fipsd_ipv6_listening_socket *sockets, size_t count);
#endif
