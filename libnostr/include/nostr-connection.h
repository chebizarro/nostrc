#ifndef __NOSTR_CONNECTION_H__
#define __NOSTR_CONNECTION_H__

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
NostrConnection *nostr_connection_new(const char *url);
void nostr_connection_close(NostrConnection *conn);
void nostr_connection_write_message(NostrConnection *conn, GoContext *ctx, char *message, Error **err);
void nostr_connection_read_message(NostrConnection *conn, GoContext *ctx, char *buffer, size_t buffer_size, Error **err);

/* Accessors (GLib-friendly) */
/**
 * nostr_connection_get_send_channel:
 * @conn: (nullable): connection
 *
 * Returns: (transfer none) (nullable): internal send channel owned by connection
 */
GoChannel *nostr_connection_get_send_channel(const NostrConnection *conn);

/**
 * nostr_connection_get_recv_channel:
 * @conn: (nullable): connection
 *
 * Returns: (transfer none) (nullable): internal receive channel owned by connection
 */
GoChannel *nostr_connection_get_recv_channel(const NostrConnection *conn);

/**
 * nostr_connection_is_running:
 * @conn: (nullable): connection
 *
 * Returns: whether the background service thread is running (false in test mode)
 */
bool nostr_connection_is_running(const NostrConnection *conn);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_CONNECTION_H__ */
