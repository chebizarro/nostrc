/* ns-fake-signer.h - In-process org.nostr.Signer for nostr-share tests
 *
 * SPDX-License-Identifier: MIT
 *
 * Owns org.nostr.Signer on the bus DBUS_SESSION_BUS_ADDRESS names (a
 * private GTestDBus bus), from its own thread. GetPublicKey answers the
 * npub of a throwaway key; SignEvent signs with it.
 */
#ifndef NS_FAKE_SIGNER_H
#define NS_FAKE_SIGNER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NsFakeSigner NsFakeSigner;

NsFakeSigner *ns_fake_signer_start(void);
void          ns_fake_signer_stop(NsFakeSigner *s);
const gchar  *ns_fake_signer_sk(NsFakeSigner *s);   /* hex */
const gchar  *ns_fake_signer_pk(NsFakeSigner *s);   /* hex */
guint         ns_fake_signer_sign_calls(NsFakeSigner *s);

G_END_DECLS

#endif /* NS_FAKE_SIGNER_H */
