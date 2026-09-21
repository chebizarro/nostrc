#ifndef NH_AUTH_BROKER_H
#define NH_AUTH_BROKER_H

#include "nostr_identity.h"

typedef struct nh_auth_broker nh_auth_broker;

/* Creates a broker bound to an open identity store (authority). The store is
 * borrowed and must outlive the broker. */
nh_auth_broker *nh_auth_broker_new(nh_identity_store *store);
void nh_auth_broker_free(nh_auth_broker *broker);

/* Reads one request from a connected AUTH-endpoint SOCK_SEQPACKET fd, enforces
 * the SO_PEERCRED/endpoint ACL, dispatches it, and writes one response.
 * Returns 0 if a response was sent, -1 on transport failure. */
int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd);

#endif /* NH_AUTH_BROKER_H */
