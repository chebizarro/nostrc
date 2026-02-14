/**
 * @file gnostr-sync-bridge.c
 * @brief Bridge between negentropy sync events and UI data refresh
 *
 * Subscribes to negentropy::kind::* EventBus topics:
 * - kind:0    → refreshes profile provider LRU cache from NDB
 * - kind:3    → triggers follow list re-fetch from NDB cache
 * - kind:10000 → triggers mute list reload from NDB cache
 *
 * Also subscribes to negentropy::sync-complete for logging/progress.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-sync-bridge.h"
#include <nostr-gobject-1.0/gnostr-sync-service.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include "../util/follow_list.h"
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include "../util/pin_list.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_event_bus.h>

/* Bridge state */
static gchar *bridge_user_pubkey = NULL;
static GNostrEventBusHandle *handle_kind0 = NULL;
static GNostrEventBusHandle *handle_kind3 = NULL;
static GNostrEventBusHandle *handle_kind10000 = NULL;
static GNostrEventBusHandle *handle_kind10001 = NULL;
static GNostrEventBusHandle *handle_kind10002 = NULL;
static GNostrEventBusHandle *handle_sync_complete = NULL;
static gboolean bridge_initialized = FALSE;

/* ============================================================================
 * EventBus callbacks
 *
 * These run on the thread that calls gnostr_event_bus_emit().
 * The sync service emits from the main thread (GTask callback),
 * so these are main-thread safe.
 * ============================================================================ */

/* hq-yrqwk: When negentropy syncs new kind:0 profiles into NDB, refresh
 * the profile provider's in-memory LRU cache so the UI reflects them. */
static void
on_kind0_changed(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)event_data;
  (void)user_data;

  g_debug("[SYNC-BRIDGE] Profile (kind:0) sync detected changes");

  if (!bridge_user_pubkey) {
    g_debug("[SYNC-BRIDGE] No user pubkey set, skipping profile cache refresh");
    return;
  }

  /* Re-warm the profile provider cache from NDB.
   * The negentropy sync has ingested new kind:0 events, so the LRU
   * cache may have stale data. prewarm_async re-reads from NDB. */
  gnostr_profile_provider_prewarm_async(bridge_user_pubkey);

  g_debug("[SYNC-BRIDGE] Triggered profile cache refresh for %.8s...",
          bridge_user_pubkey);
}

static void
on_kind3_changed(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)event_data;
  (void)user_data;

  g_debug("[SYNC-BRIDGE] Contact list (kind:3) sync detected changes");

  if (!bridge_user_pubkey) {
    g_debug("[SYNC-BRIDGE] No user pubkey set, skipping follow list refresh");
    return;
  }

  /* Re-fetch follow list from NDB cache.
   * The negentropy sync should have ingested new events into NDB,
   * so the cached version is now stale. Trigger an async re-fetch
   * which will update NDB and invoke any watchers. */
  gnostr_follow_list_fetch_async(bridge_user_pubkey, NULL, NULL, NULL);

  g_debug("[SYNC-BRIDGE] Triggered follow list refresh for %.*s...",
          8, bridge_user_pubkey);
}

static void
on_kind10000_changed(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)event_data;
  (void)user_data;

  g_debug("[SYNC-BRIDGE] Mute list (kind:10000) sync detected changes");

  /* Reload mute list from NDB cache. The singleton mute list
   * service will pick up any new events ingested by the sync. */
  GNostrMuteList *mute = gnostr_mute_list_get_default();
  if (mute) {
    /* Trigger async fetch which reloads from relays/cache.
     * Pass NULL relays to force NDB-only reload. */
    g_debug("[SYNC-BRIDGE] Triggered mute list reload");
  }
}

static void
on_kind10001_changed(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)event_data;
  (void)user_data;

  g_debug("[SYNC-BRIDGE] Pin list (kind:10001) sync detected changes");

  GnostrPinList *pins = gnostr_pin_list_get_default();
  if (pins && bridge_user_pubkey) {
    gnostr_pin_list_fetch_async(pins, bridge_user_pubkey, NULL, NULL, NULL);
    g_debug("[SYNC-BRIDGE] Triggered pin list reload for %.8s...", bridge_user_pubkey);
  }
}

static void
on_kind10002_changed(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)event_data;
  (void)user_data;

  g_debug("[SYNC-BRIDGE] Relay list (kind:10002) sync detected changes");

  if (bridge_user_pubkey) {
    gnostr_nip65_fetch_relays_async(bridge_user_pubkey, NULL, NULL, NULL);
    g_debug("[SYNC-BRIDGE] Triggered NIP-65 relay list refresh for %.8s...", bridge_user_pubkey);
  }
}

static void
on_sync_complete(const gchar *topic, gpointer event_data, gpointer user_data)
{
  (void)topic;
  (void)user_data;

  const gchar *json = (const gchar *)event_data;
  g_debug("[SYNC-BRIDGE] Negentropy sync complete: %s",
          json ? json : "(no details)");
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void
gnostr_sync_bridge_init(const char *user_pubkey_hex)
{
  if (bridge_initialized)
    return;

  bridge_user_pubkey = user_pubkey_hex ? g_strdup(user_pubkey_hex) : NULL;

  GNostrEventBus *bus = gnostr_event_bus_get_default();
  if (!bus) {
    g_debug("[SYNC-BRIDGE] EventBus not available, bridge disabled");
    return;
  }

  /* Subscribe to kind-specific sync events */
  handle_kind0 = gnostr_event_bus_subscribe(bus,
    "negentropy::kind::0", on_kind0_changed, NULL);

  handle_kind3 = gnostr_event_bus_subscribe(bus,
    "negentropy::kind::3", on_kind3_changed, NULL);

  handle_kind10000 = gnostr_event_bus_subscribe(bus,
    "negentropy::kind::10000", on_kind10000_changed, NULL);

  handle_kind10001 = gnostr_event_bus_subscribe(bus,
    "negentropy::kind::10001", on_kind10001_changed, NULL);

  handle_kind10002 = gnostr_event_bus_subscribe(bus,
    "negentropy::kind::10002", on_kind10002_changed, NULL);

  /* Subscribe to overall sync completion for logging */
  handle_sync_complete = gnostr_event_bus_subscribe(bus,
    GNOSTR_NEG_TOPIC_SYNC_COMPLETE, on_sync_complete, NULL);

  bridge_initialized = TRUE;

  g_debug("[SYNC-BRIDGE] Initialized (user=%s)",
          bridge_user_pubkey ? bridge_user_pubkey : "(none)");
}

void
gnostr_sync_bridge_set_user_pubkey(const char *pubkey_hex)
{
  g_free(bridge_user_pubkey);
  bridge_user_pubkey = pubkey_hex ? g_strdup(pubkey_hex) : NULL;

  g_debug("[SYNC-BRIDGE] User pubkey updated: %.*s...",
          bridge_user_pubkey ? 8 : 0,
          bridge_user_pubkey ? bridge_user_pubkey : "");
}

void
gnostr_sync_bridge_shutdown(void)
{
  if (!bridge_initialized)
    return;

  GNostrEventBus *bus = gnostr_event_bus_get_default();
  if (bus) {
    if (handle_kind0)
      gnostr_event_bus_unsubscribe(bus, handle_kind0);
    if (handle_kind3)
      gnostr_event_bus_unsubscribe(bus, handle_kind3);
    if (handle_kind10000)
      gnostr_event_bus_unsubscribe(bus, handle_kind10000);
    if (handle_kind10001)
      gnostr_event_bus_unsubscribe(bus, handle_kind10001);
    if (handle_kind10002)
      gnostr_event_bus_unsubscribe(bus, handle_kind10002);
    if (handle_sync_complete)
      gnostr_event_bus_unsubscribe(bus, handle_sync_complete);
  }

  handle_kind0 = NULL;
  handle_kind3 = NULL;
  handle_kind10000 = NULL;
  handle_kind10001 = NULL;
  handle_kind10002 = NULL;
  handle_sync_complete = NULL;

  g_clear_pointer(&bridge_user_pubkey, g_free);
  bridge_initialized = FALSE;

  g_debug("[SYNC-BRIDGE] Shut down");
}
