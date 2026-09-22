/*
 * gnostr-signer-bridge.h — registration seam for the higher-layer signer.
 *
 * Some helpers inside nostr-gobject (gnostr-relays.c, gnostr-mute-list.c)
 * need to invoke signing / D-Bus signer proxy operations owned by
 * apps/gnostr (GnostrSignerService and the shared NIP-55L proxy). Rather
 * than link the app layer INTO the library — which broke shared builds
 * with undefined symbols — the library exposes this bridge and the app
 * installs its implementation at startup.
 *
 * If no bridge is installed, all operations report "not initialized" so
 * the library remains self-contained.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GNOSTR_SIGNER_BRIDGE_H
#define GNOSTR_SIGNER_BRIDGE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Forward declaration of the gdbus-codegen NIP-55L proxy type.
 * Only the pointer is trafficked across the bridge; callers that need to
 * dispatch real methods on it include the generated <signer_proxy.h>
 * themselves. */
typedef struct _NostrOrgNostrSigner NostrOrgNostrSigner;

/**
 * GnostrSignerBridge:
 * @is_available: TRUE when a signing method (NIP-55L or NIP-46) is usable.
 * @sign_event_async: begin an asynchronous sign_event operation on the
 *   default signer service. The callback semantics match GAsyncResult.
 * @sign_event_finish: retrieve the signed event JSON from a GAsyncResult
 *   returned via @sign_event_async.
 * @proxy_get: return the shared NIP-55L D-Bus proxy, or NULL on failure.
 *
 * All members are optional; a NULL entry means the corresponding operation
 * is unavailable and returns a G_IO_ERROR_NOT_INITIALIZED error.
 */
typedef struct {
  gboolean (*is_available)(void);
  void (*sign_event_async)(const char *event_json,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);
  gboolean (*sign_event_finish)(GAsyncResult *res,
                                 char **out_signed_event,
                                 GError **error);
  NostrOrgNostrSigner *(*proxy_get)(GError **error);
} GnostrSignerBridge;

/**
 * gnostr_signer_bridge_install:
 * @vtable: (nullable) (transfer none): the vtable to install. Pass %NULL
 *          to detach at shutdown.
 *
 * Registers the higher-layer signer implementation with nostr-gobject.
 * The vtable is copied; the caller can free it after this returns.
 */
void gnostr_signer_bridge_install(const GnostrSignerBridge *vtable);

/* Convenience accessors used by nostr-gobject internals. */
gboolean gnostr_signer_bridge_is_available(void);
void gnostr_signer_bridge_sign_event_async(const char *event_json,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
gboolean gnostr_signer_bridge_sign_event_finish(GAsyncResult *res,
                                                 char **out_signed_event,
                                                 GError **error);
NostrOrgNostrSigner *gnostr_signer_bridge_proxy_get(GError **error);

G_END_DECLS

#endif /* GNOSTR_SIGNER_BRIDGE_H */
