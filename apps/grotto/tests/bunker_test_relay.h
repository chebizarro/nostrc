#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct BunkerTestRelay BunkerTestRelay;
/* Dispatches on the caller's main context; binds only loopback, port 0. */
BunkerTestRelay *bunker_test_relay_new(void);
const gchar *bunker_test_relay_url(BunkerTestRelay *relay);
void bunker_test_relay_stats(BunkerTestRelay *relay, guint *connections, guint *requests, guint *published);
JsonArray *bunker_test_relay_events(BunkerTestRelay *relay); /* borrowed */
JsonArray *bunker_test_relay_deliveries(BunkerTestRelay *relay); /* borrowed; multiplicity preserved */
gint64 bunker_test_relay_last_since(BunkerTestRelay *relay);
void bunker_test_relay_disconnect(BunkerTestRelay *relay);
void bunker_test_relay_disconnect_recipient(BunkerTestRelay *, const gchar *pubkey);
typedef void (*BunkerTestRelayRequestCb)(JsonArray *request, gpointer data);
void bunker_test_relay_set_request_callback(BunkerTestRelay *, BunkerTestRelayRequestCb, gpointer);
typedef void (*BunkerTestRelayEventCb)(const gchar *event_json, gpointer data);
void bunker_test_relay_set_event_callback(BunkerTestRelay *, BunkerTestRelayEventCb, gpointer);
/* Hostile-relay replay: verified signed event delivered regardless of filters. */
void bunker_test_relay_inject(BunkerTestRelay *, const gchar *event_json);
/* Protocol failure fixtures; affect subsequent REQ/EVENT frames. */
void bunker_test_relay_reject_subscriptions(BunkerTestRelay *, gboolean);
void bunker_test_relay_reject_publishes(BunkerTestRelay *, gboolean);
void bunker_test_relay_hold_acks(BunkerTestRelay *, gboolean);
void bunker_test_relay_challenge(BunkerTestRelay *);
void bunker_test_relay_free(BunkerTestRelay *relay);
