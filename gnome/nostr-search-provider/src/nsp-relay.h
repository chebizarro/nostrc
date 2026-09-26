/*
 * nsp-relay — one bounded NIP-01 REQ against the per-user session relay
 * ($XDG_RUNTIME_DIR/nostr/relay.sock: WebSocket over AF_UNIX, the same
 * transport nostr-dispatcher's nd-fetch uses).
 *
 * The query ALWAYS completes by @deadline_us (monotonic): on EOSE, on
 * CLOSED, on connection failure, on cancellation, or at the deadline with
 * whatever arrived so far (partial results). This matters because a
 * cache-less session relay (built without nostrdb) answers a plain REQ
 * with nothing at all — no EVENT, no EOSE — so the deadline is the only
 * terminator there.
 *
 * Only id- and signature-validated events are returned (nd_event_parse),
 * de-duplicated by id and capped at @max_events. The session relay
 * allows one open subscription per connection (max_subs = 1), so every
 * query uses its own connection.
 */
#ifndef NSP_RELAY_H
#define NSP_RELAY_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
  NSP_RELAY_EOSE,
  NSP_RELAY_CLOSED,
  NSP_RELAY_TIMEOUT,
  NSP_RELAY_UNAVAILABLE,
  NSP_RELAY_CANCELLED,
} NspRelayStatus;

typedef struct {
  NspRelayStatus status;
  char *closed_reason; /* CLOSED message, may be NULL */
  GPtrArray *events;   /* NdEvent*, validated, owned */
  guint rejected;      /* invalid / unverifiable events dropped */
} NspRelayReply;

void nsp_relay_reply_free(NspRelayReply *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NspRelayReply, nsp_relay_reply_free)

const char *nsp_relay_status_name(NspRelayStatus s);

char *nsp_relay_default_socket_path(void);

/* @filters: JSON array of filter objects (not consumed). */
void nsp_relay_query_async(const char *socket_path, JsonNode *filters, gint64 deadline_us,
                           guint max_events, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data);
/* Never NULL. */
NspRelayReply *nsp_relay_query_finish(GAsyncResult *res);

G_END_DECLS

#endif /* NSP_RELAY_H */
