#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct BunkerTestRelay BunkerTestRelay;
/* Dispatches on the caller's main context; binds only loopback, port 0. */
BunkerTestRelay *bunker_test_relay_new(void);
const gchar *bunker_test_relay_url(BunkerTestRelay *relay);
void bunker_test_relay_stats(BunkerTestRelay *relay, guint *connections, guint *requests, guint *published);
JsonArray *bunker_test_relay_events(BunkerTestRelay *relay); /* borrowed */
void bunker_test_relay_free(BunkerTestRelay *relay);
