#ifndef NH_AUTH_BROKER_H
#define NH_AUTH_BROKER_H

#include "nostr_identity.h"

typedef struct nh_auth_broker nh_auth_broker;

/* Forward declaration: the SMB authority is optional and lives in D5's
 * smb_credential module.  Broker consumers that don't touch SMB avoid the
 * transitive SQLite include by not attaching an authority. */
typedef struct nh_smb_authority nh_smb_authority;

/* Creates a broker bound to an open identity store (authority). The store is
 * borrowed and must outlive the broker. */
nh_auth_broker *nh_auth_broker_new(nh_identity_store *store);
void nh_auth_broker_free(nh_auth_broker *broker);

/* Attaches an SMB credential authority to the broker.  Borrowed; must outlive
 * the broker.  When set, connections on the USER endpoint may run
 * BEGIN_SMB_PROOF and, on a verified proof, receive a freshly minted volatile
 * password from the authority.  Leaving this unset makes SMB proof unavailable
 * (the broker responds PROVIDER_UNAVAILABLE).  Pass NULL to detach. */
void nh_auth_broker_set_smb_authority(nh_auth_broker *broker,
                                      nh_smb_authority *authority);

/* Reads one request from a connected AUTH-endpoint SOCK_SEQPACKET fd (auth.sock,
 * uid 0 only for login), enforces the SO_PEERCRED/endpoint ACL, dispatches it,
 * and writes one response.  Returns 0 if a response was sent, -1 on transport
 * failure. */
int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd);

/* Same as nh_auth_broker_handle_connection but for a USER-endpoint connection
 * (user.sock, any uid, own-account only).  The peer snapshot is taken with
 * NH_AUTH_ENDPOINT_USER; the ACL then permits BEGIN_SMB_PROOF and, on a
 * verified proof, the broker mints a short-lived SMB password from the
 * attached authority and returns it once. */
int nh_auth_broker_handle_user_connection(nh_auth_broker *broker, int fd);

#endif /* NH_AUTH_BROKER_H */
