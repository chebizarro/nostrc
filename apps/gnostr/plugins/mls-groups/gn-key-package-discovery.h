/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-discovery.h - Marmot KeyPackage discovery (nostrc-prqu.11)
 *
 * The adopted Marmot spec (transports/nostr.md, "KeyPackage publication")
 * has no dedicated KeyPackage relay list: KeyPackages (kind:30443) are
 * published to, and fetched from, the account's NIP-65 kind:10002
 * write-capable relays ("r" entries marked "write" or unmarked; never
 * read-only ones). The invitee's candidate KeyPackages are gathered from
 * the local store and from those relays, and
 * marmot_gobject_select_key_package_event() picks one (newest per
 * (pubkey, d) slot, validated) instead of trusting whatever is newest.
 *
 * Storage and relay access go through GnKpBackend so the logic is testable
 * without the application; gn_kp_backend_init_for_plugin() binds it to the
 * plugin host.
 */
#ifndef GN_KEY_PACKAGE_DISCOVERY_H
#define GN_KEY_PACKAGE_DISCOVERY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GnostrPluginContext GnostrPluginContext;

typedef struct {
  /* Local store: event JSONs matching @filter_json (element-type utf8,
   * transfer full), or NULL. */
  GPtrArray *(*query_local)(gpointer data, const char *filter_json);
  /* Query @relays (NULL-terminated) with @filter_json; the finish returns
   * event JSONs (element-type utf8, transfer full). */
  void (*query_relays_async)(gpointer data, const char *const *relays,
                             const char *filter_json, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data);
  GPtrArray *(*query_relays_finish)(gpointer data, GAsyncResult *result, GError **error);
  /* The user's configured relays (transfer full, NULL-terminated): where a
   * missing kind:10002 list is looked up. */
  char **(*own_relays)(gpointer data);
  gpointer data;
} GnKpBackend;

void gn_kp_backend_init_for_plugin(GnKpBackend *backend, GnostrPluginContext *context);

/* The write-capable relays of a NIP-65 kind:10002 event (unmarked or
 * "write"; ws:// or wss:// only; de-duplicated). NULL-terminated, never
 * NULL; empty for anything that is not a kind:10002 event. */
char **gn_kp_write_relays_from_relay_list(const char *event_json);

/* The write relays of @pubkey_hex's newest validly signed kind:10002 in
 * the local store; empty if none. Used for publishing our own KeyPackage. */
char **gn_kp_local_write_relays(const GnKpBackend *backend, const char *pubkey_hex);

/* Find @pubkey_hex's KeyPackage: its kind:10002 (local store, else the
 * user's relays), then kind:30443 candidates from the local store and its
 * write relays, chosen with marmot_gobject_select_key_package_event(). */
void gn_kp_discover_async(const GnKpBackend *backend, const char *pubkey_hex,
                          GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data);
/* Returns: (transfer full): the KeyPackage event JSON, or NULL + @error */
char *gn_kp_discover_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* GN_KEY_PACKAGE_DISCOVERY_H */
