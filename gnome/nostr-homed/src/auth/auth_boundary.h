#ifndef NH_AUTH_BOUNDARY_H
#define NH_AUTH_BOUNDARY_H
#include "auth_transaction.h"

#define NH_AUTH_BOUNDARY_CONNECTIONS 8u
#define NH_AUTH_BOUNDARY_RATE_BUCKETS 32u
#define NH_AUTH_BOUNDARY_ATTEMPTS 8u
#define NH_AUTH_BOUNDARY_WINDOW_MS 60000u

typedef struct nh_auth_boundary nh_auth_boundary;
typedef struct nh_auth_connection nh_auth_connection;
/* Single broker event-loop owner. Callback must cancel the attached transaction
 * before initiating worker cleanup. It fires once on close, including shutdown.
 */
typedef void (*nh_auth_disconnect_fn)(void *context);
nh_auth_boundary *nh_auth_boundary_new(void);
void nh_auth_boundary_free(nh_auth_boundary *boundary);
/* Consumes fd on both success and failure. Only connected AF_UNIX SEQPACKET
 * descriptors from broker-owned listeners are accepted. now is CLOCK_MONOTONIC.
 */
nh_auth_connection *nh_auth_boundary_accept(nh_auth_boundary *boundary, int fd,
                                            nh_auth_endpoint endpoint,
                                            uint64_t now);
const nh_auth_peer_snapshot *
nh_auth_connection_peer(nh_auth_connection *connection);
int nh_auth_connection_pidfd(nh_auth_connection *connection);
/* Attach only a broker-created transaction after authoritative admission.
 * At most one transaction per connection; client IDs cannot establish
 * ownership. */
int nh_auth_connection_bind(nh_auth_connection *connection,
                            const char *transaction_id,
                            nh_auth_disconnect_fn disconnect, void *context);
/* Nonblocking: 1 authorized message; 0 EAGAIN; -1 closed/rejected. Must also be
 * called when pidfd is readable or socket hangs up. Caller clears output. Does
 * not execute operations or turn provider/client messages into proof success.
 */
int nh_auth_connection_receive(nh_auth_connection *connection,
                               nh_auth_message *out);
/* Invalidates connection pointer. */
void nh_auth_connection_close(nh_auth_connection *connection);
#endif
