/*
 * gnostr-signer-bridge-install.h
 *
 * nostrc-ecrx: registers the app-layer signer implementation with the
 * lower-layer nostr-gobject signer bridge. Call once at app startup,
 * before any code path that may need signing.
 */
#ifndef GNOSTR_SIGNER_BRIDGE_INSTALL_H
#define GNOSTR_SIGNER_BRIDGE_INSTALL_H

#include <glib.h>

G_BEGIN_DECLS

void gnostr_signer_bridge_install_default(void);

G_END_DECLS

#endif
