/* nwa-service.h - org.nostr.Wallet1 on the session bus
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NWA_SERVICE_H
#define NWA_SERVICE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NWA_BUS_NAME    "org.nostr.Wallet1"
#define NWA_OBJECT_PATH "/org/nostr/Wallet1"
#define NWA_INTERFACE   "org.nostr.Wallet1"

typedef struct _NwaService NwaService;

/* Exports the interface on @bus (the name is owned by the caller). */
NwaService *nwa_service_new(GDBusConnection *bus, GError **error);
void        nwa_service_free(NwaService *self);

/* Scheme-handler entry point (lightning:, bitcoin:, nostr+walletconnect:). */
void        nwa_service_open_uri(NwaService *self, const gchar *uri);

G_END_DECLS

#endif /* NWA_SERVICE_H */
