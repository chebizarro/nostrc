#ifndef __NOSTR_CONNECTION_H__
#define __NOSTR_CONNECTION_H__

#include "nostr-export.h"

/* Public header exposing GI-friendly names for Connection. */

#include <stdbool.h>
#include "channel.h"      /* GoChannel */
#include "go.h"           /* GoContext */
#include "error.h"        /* Error */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GI-style naming */
typedef struct _NostrConnectionPrivate NostrConnectionPrivate;

typedef struct _NostrConnection {
    NostrConnectionPrivate *priv;
    GoChannel *send_channel;
    GoChannel *recv_channel;
} NostrConnection;

/* Canonical API */
NOSTR_API NostrConnection *nostr_connection_new(const char *url);
/* Cancellation interrupts queued request waits. A dependency call already in
 * progress retains its inputs until the service thread acknowledges it. */
NOSTR_API NostrConnection *nostr_connection_new_with_context(const char *url, GoContext *cancel);
/* Nonblocking: wakes handshake and channel waiters without freeing conn. */
NOSTR_API void nostr_connection_interrupt(NostrConnection *conn);
/* Initialize exit ordering without opening a socket. Terminal shutdown must
 * follow relay-owner drainage; it joins LWS before OpenSSL's atexit cleanup. */
NOSTR_API void nostr_connection_runtime_init(void);
NOSTR_API void nostr_connection_shutdown(void);
NOSTR_API void nostr_connection_close(NostrConnection *conn);
NOSTR_API void nostr_connection_write_message(NostrConnection *conn, GoContext *ctx, char *message, Error **err);
NOSTR_API void nostr_connection_read_message(NostrConnection *conn, GoContext *ctx, char *buffer, size_t buffer_size, Error **err);

/* Accessors (GLib-friendly) */
/**
 * nostr_connection_get_send_channel:
 * @conn: (nullable): connection
 *
 * Returns: (transfer none) (nullable): internal send channel owned by connection
 */
NOSTR_API GoChannel *nostr_connection_get_send_channel(const NostrConnection *conn);

/**
 * nostr_connection_get_recv_channel:
 * @conn: (nullable): connection
 *
 * Returns: (transfer none) (nullable): internal receive channel owned by connection
 */
NOSTR_API GoChannel *nostr_connection_get_recv_channel(const NostrConnection *conn);

/**
 * nostr_connection_is_running:
 * @conn: (nullable): connection
 *
 * Returns: whether the background service thread is running (false in test mode)
 */
NOSTR_API bool nostr_connection_is_running(const NostrConnection *conn);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_CONNECTION_H__ */
