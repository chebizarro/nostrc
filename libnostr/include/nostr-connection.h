#ifndef __NOSTR_CONNECTION_H__
#define __NOSTR_CONNECTION_H__

/* Transitional header exposing GLib-friendly names for Connection. */

#include <stdbool.h>
#include "connection.h"   /* legacy Connection and APIs */
#include "channel.h"      /* GoChannel */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GLib-style naming */
typedef Connection NostrConnection;

/* New API names mapped to legacy implementations */
#define nostr_connection_new             new_connection
#define nostr_connection_close           connection_close
#define nostr_connection_write_message   connection_write_message
#define nostr_connection_read_message    connection_read_message

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
