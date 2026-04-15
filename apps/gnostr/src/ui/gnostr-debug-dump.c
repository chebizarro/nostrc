/*
 * gnostr-debug-dump.c — nostrc-8mb8.2: Protocol diagnostics dump
 *
 * Dumps active relay health and NDB subscription state to g_debug logs.
 * Triggered by Ctrl+Shift+D or programmatic call. Safe in production:
 * output only visible when G_MESSAGES_DEBUG=gnostr-debug is set.
 */

#define G_LOG_DOMAIN "gnostr-debug"

#include "gnostr-main-window-private.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_relay.h>
#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/storage_ndb.h>

/* nostrc-8mb8.2: Human-readable relay state nick (duplicated from nostr_relay.c
 * to avoid exporting internal helper; the enum is small and stable). */
static const char *
debug_relay_state_nick(GNostrRelayState state)
{
    switch (state) {
    case GNOSTR_RELAY_STATE_DISCONNECTED: return "disconnected";
    case GNOSTR_RELAY_STATE_CONNECTING:   return "connecting";
    case GNOSTR_RELAY_STATE_CONNECTED:    return "connected";
    case GNOSTR_RELAY_STATE_ERROR:        return "error";
    default:                              return "unknown";
    }
}

void
gnostr_debug_dump_protocol_state(GnostrMainWindow *self)
{
    g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

    g_debug("═══════════ PROTOCOL DIAGNOSTIC DUMP ═══════════");

    /* ── 1. Relay health ── */
    GNostrPool *pool = self->pool;
    if (pool) {
        GListStore *relays = gnostr_pool_get_relays(pool);
        guint n = g_list_model_get_n_items(G_LIST_MODEL(relays));
        g_debug("[RELAYS] pool has %u relay(s)", n);

        for (guint i = 0; i < n; i++) {
            g_autoptr(GNostrRelay) relay =
                GNOSTR_RELAY(g_list_model_get_item(G_LIST_MODEL(relays), i));
            if (!relay) continue;

            const char *url = gnostr_relay_get_url(relay);
            GNostrRelayState state = gnostr_relay_get_state(relay);
            gboolean authed = gnostr_relay_get_authenticated(relay);

            g_debug("[RELAY] url=%s state=%s authenticated=%s",
                    url ? url : "?",
                    debug_relay_state_nick(state),
                    authed ? "yes" : "no");
        }
    } else {
        g_debug("[RELAYS] pool=NULL (not initialized)");
    }

    /* ── 2. NDB storage stats ── */
    {
        guint64 count = storage_ndb_get_ingest_count();
        guint64 bytes = storage_ndb_get_ingest_bytes();
        g_debug("[NDB] ingested_events=%" G_GUINT64_FORMAT " ingested_bytes=%" G_GUINT64_FORMAT,
                count, bytes);
    }

    /* ── 3. Active NDB subscriptions ── */
    {
        guint active = gn_ndb_get_active_subscription_count();
        g_debug("[NDB_SUBS] active_subscriptions=%u", active);
    }

    g_debug("═══════════ END DIAGNOSTIC DUMP ═══════════");
}
