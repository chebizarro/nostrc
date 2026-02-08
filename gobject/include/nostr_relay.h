#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-enums.h"
#include "nostr-error.h"

G_BEGIN_DECLS

/* Forward declarations - these types are defined in core libnostr headers.
 * We only forward-declare them here to avoid pulling in all of libnostr's
 * headers which can cause type conflicts. The actual struct definitions
 * come from nostr-relay.h, nostr-event.h, nostr-filter.h in libnostr. */

/* Core NostrRelay type (from libnostr/include/nostr-relay.h) */
#ifndef NOSTR_RELAY_FORWARD_DECLARED
#define NOSTR_RELAY_FORWARD_DECLARED
struct NostrRelay;
typedef struct NostrRelay NostrRelay;
#endif

/* Core NostrEvent type (from libnostr/include/nostr-event.h) */
#ifndef NOSTR_EVENT_FORWARD_DECLARED
#define NOSTR_EVENT_FORWARD_DECLARED
/* Note: Core uses struct _NostrEvent internally */
typedef struct _NostrEvent NostrEvent;
#endif

/* Core NostrFilter type (from libnostr/include/nostr-filter.h) */
#ifndef NOSTR_FILTER_FORWARD_DECLARED
#define NOSTR_FILTER_FORWARD_DECLARED
typedef struct NostrFilter NostrFilter;
#endif

/* Define GNostrRelay GObject */
/* GLib wrapper type is prefixed with G to avoid clashing with core NostrRelay */
#define GNOSTR_TYPE_RELAY (gnostr_relay_get_type())
G_DECLARE_FINAL_TYPE(GNostrRelay, gnostr_relay, GNOSTR, RELAY, GObject)

/**
 * GNostrRelay:
 *
 * A GObject wrapper for Nostr relay connections implementing NIP-01.
 * Provides property notifications and signals for connection state,
 * events, notices, and relay protocol messages.
 */

/* Signal indices */
enum {
    GNOSTR_RELAY_SIGNAL_STATE_CHANGED,
    GNOSTR_RELAY_SIGNAL_EVENT_RECEIVED,
    GNOSTR_RELAY_SIGNAL_NOTICE,
    GNOSTR_RELAY_SIGNAL_OK,
    GNOSTR_RELAY_SIGNAL_EOSE,
    GNOSTR_RELAY_SIGNAL_CLOSED,
    GNOSTR_RELAY_SIGNAL_ERROR,
    GNOSTR_RELAY_SIGNAL_NIP11_INFO,
    GNOSTR_RELAY_SIGNALS_COUNT
};

/* Legacy signal indices for backward compatibility */
enum {
    SIGNAL_CONNECTED = 0,
    SIGNAL_DISCONNECTED,
    SIGNAL_EVENT_RECEIVED,
    SIGNAL_ERROR,
    NOSTR_RELAY_SIGNALS_COUNT
};

/* GObject convenience API (prefixed with gnostr_ to avoid clashes with core
 * libnostr C API which uses nostr_relay_*). */

/**
 * gnostr_relay_new:
 * @url: the relay URL (e.g., "wss://relay.damus.io")
 *
 * Creates a new GNostrRelay with the given URL.
 * The URL is a construct-only property.
 *
 * Returns: (transfer full): a new #GNostrRelay
 */
GNostrRelay *gnostr_relay_new(const gchar *url);

/**
 * gnostr_relay_connect:
 * @self: a #GNostrRelay
 * @error: (nullable): return location for a #GError
 *
 * Synchronously connects to the relay.
 * Emits "state-changed" signal on state transitions.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_relay_connect(GNostrRelay *self, GError **error);

/**
 * gnostr_relay_connect_async:
 * @self: a #GNostrRelay
 * @cancellable: (nullable): optional #GCancellable object
 * @callback: (scope async): callback to call when operation completes
 * @user_data: (closure): user data for @callback
 *
 * Asynchronously connects to the relay.
 * Emits "state-changed" signal on state transitions.
 */
void gnostr_relay_connect_async(GNostrRelay *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

/**
 * gnostr_relay_connect_finish:
 * @self: a #GNostrRelay
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Finishes an asynchronous connection operation.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_relay_connect_finish(GNostrRelay *self,
                                      GAsyncResult *result,
                                      GError **error);

/**
 * gnostr_relay_disconnect:
 * @self: a #GNostrRelay
 *
 * Disconnects from the relay.
 * Emits "state-changed" signal.
 */
void gnostr_relay_disconnect(GNostrRelay *self);

/**
 * gnostr_relay_publish:
 * @self: a #GNostrRelay
 * @event: a #NostrEvent to publish
 * @error: (nullable): return location for a #GError
 *
 * Publishes an event to the relay.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_relay_publish(GNostrRelay *self, NostrEvent *event, GError **error);

/**
 * gnostr_relay_query_sync:
 * @self: a #GNostrRelay
 * @filter: a #NostrFilter
 * @error: (nullable): return location for a #GError
 *
 * Synchronously queries events from the relay.
 * Deprecated: Use subscription-based API instead.
 *
 * Returns: (element-type NostrEvent) (transfer full) (nullable): array of events
 */
GPtrArray *gnostr_relay_query_sync(GNostrRelay *self, NostrFilter *filter, GError **error);

/* Property accessors */

/**
 * gnostr_relay_get_url:
 * @self: a #GNostrRelay
 *
 * Gets the relay URL.
 *
 * Returns: (transfer none) (nullable): the relay URL
 */
const gchar *gnostr_relay_get_url(GNostrRelay *self);

/**
 * gnostr_relay_get_state:
 * @self: a #GNostrRelay
 *
 * Gets the current connection state.
 *
 * Returns: the current #GNostrRelayState
 */
GNostrRelayState gnostr_relay_get_state(GNostrRelay *self);

/**
 * gnostr_relay_get_connected:
 * @self: a #GNostrRelay
 *
 * Gets whether the relay is currently connected.
 * This is a derived property (state == NOSTR_RELAY_STATE_CONNECTED).
 *
 * Returns: %TRUE if connected, %FALSE otherwise
 */
gboolean gnostr_relay_get_connected(GNostrRelay *self);

/**
 * gnostr_relay_get_core_relay:
 * @self: a #GNostrRelay
 *
 * Gets the underlying core NostrRelay pointer.
 * For advanced use cases requiring direct libnostr API access.
 *
 * Returns: (transfer none) (nullable): the core NostrRelay pointer
 */
NostrRelay *gnostr_relay_get_core_relay(GNostrRelay *self);

/* ---- NIP-11 Relay Information (nostrc-20) ---- */

/* Opaque NIP-11 info type - actually RelayInformationDocument* from nip11.h */
typedef struct RelayInformationDocument GNostrRelayNip11Info;

/**
 * gnostr_relay_get_nip11_info:
 * @self: a #GNostrRelay
 *
 * Gets the cached NIP-11 relay information document, if available.
 * The info is fetched automatically when the relay connects.
 *
 * Returns: (transfer none) (nullable): the NIP-11 info, or %NULL if not yet fetched
 */
const GNostrRelayNip11Info *gnostr_relay_get_nip11_info(GNostrRelay *self);

/**
 * gnostr_relay_supports_nip:
 * @self: a #GNostrRelay
 * @nip: the NIP number to check (e.g. 11, 42, 50)
 *
 * Checks if the relay advertises support for a given NIP.
 * Returns %FALSE if NIP-11 info has not been fetched yet.
 *
 * Returns: %TRUE if the relay supports the given NIP
 */
gboolean gnostr_relay_supports_nip(GNostrRelay *self, gint nip);

/**
 * gnostr_relay_fetch_nip11_async:
 * @self: a #GNostrRelay
 *
 * Manually triggers a NIP-11 info fetch. Normally this happens
 * automatically on connect, but can be called to refresh.
 * Emits "nip11-info-fetched" signal when complete.
 */
void gnostr_relay_fetch_nip11_async(GNostrRelay *self);

G_END_DECLS

#endif /* NOSTR_RELAY_H */
