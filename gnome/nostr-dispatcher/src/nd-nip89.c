/*
 * nd-nip89.c — NIP-89 handler discovery hook (stub).
 *
 * Called by the registry after handlers.list and X-Nostr-Kinds produced no
 * match for a known kind, and before the `*` generic-viewer fallback.
 *
 * TODO(nostrc-prqu.1): query kind-31989 recommendations from the user's
 * follows plus kind-31990 handler-information events for @kind (session
 * relay first, then home relays), and offer the recommended handler —
 * web handlers via their `web` URL template, never auto-launched without
 * user confirmation. Must honour `[Dispatcher] fetch-relay-hints=false`.
 * Network discovery is deliberately not implemented yet.
 */
#include "nd-nip89.h"

char *nd_nip89_discover(guint32 kind) {
  g_debug("nostr-dispatcher: NIP-89 discovery for kind %u not implemented "
          "(nostrc-prqu.1)", kind);
  return NULL;
}
