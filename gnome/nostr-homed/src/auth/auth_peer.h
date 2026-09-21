#ifndef NH_AUTH_PEER_H
#define NH_AUTH_PEER_H

#include "auth_transaction.h" /* nh_auth_peer_snapshot, NH_AUTH_CONNECTION_ID_LEN */
#include "nostr_auth_protocol.h"

/* Fills a peer snapshot for a connected SOCK_SEQPACKET fd from SO_PEERCRED plus
 * the peer's process start time (best effort) and a fresh random connection id.
 * Returns 0 on success, -1 on failure. */
int nh_auth_peer_from_fd(int fd, nh_auth_endpoint endpoint,
                         nh_auth_peer_snapshot *out);

#endif /* NH_AUTH_PEER_H */
