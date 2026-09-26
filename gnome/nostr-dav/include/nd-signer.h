/* nd-signer.h - nostr-dav names for the libnostr-publish signer
 *
 * SPDX-License-Identifier: MIT
 *
 * The signer abstraction over `org.nostr.Signer.SignEvent` moved to
 * libnostr-publish (nostr-publish-signer.h, bead nostrc-tmsc) so
 * nostr-share and nostr-wallet-agent can share it. nostr-dav keeps its
 * historic NdSigner spelling through these aliases; the only nostr-dav
 * policy here is the D-Bus `app_id`, which stays "nostr-dav" so existing
 * signer ACL entries and approval prompts are unchanged.
 */
#ifndef ND_SIGNER_H
#define ND_SIGNER_H

#include <nostr-publish/nostr-publish-signer.h>

G_BEGIN_DECLS

#define ND_SIGNER_APP_ID "nostr-dav"

typedef NostrPublishSigner       NdSigner;
typedef NostrPublishSignerVTable NdSignerVTable;
typedef NostrPublishSignerError  NdSignerError;

#define ND_SIGNER_ERROR            NOSTR_PUBLISH_SIGNER_ERROR
#define ND_SIGNER_ERROR_DENIED     NOSTR_PUBLISH_SIGNER_ERROR_DENIED
#define ND_SIGNER_ERROR_TRANSIENT  NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT
#define ND_SIGNER_ERROR_MALFORMED  NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED

#define nd_signer_error_quark      nostr_publish_signer_error_quark
#define nd_signer_new_from_vtable  nostr_publish_signer_new_from_vtable
#define nd_signer_ref              nostr_publish_signer_ref
#define nd_signer_unref            nostr_publish_signer_unref
#define nd_signer_sign_event_json  nostr_publish_signer_sign_event_json

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdSigner, nostr_publish_signer_unref)

/* Signer proxy on the session bus, identifying itself as "nostr-dav". */
static inline NdSigner *
nd_signer_new_dbus(GDBusConnection *connection, GError **error)
{
  return nostr_publish_signer_new_dbus(connection, ND_SIGNER_APP_ID, error);
}

G_END_DECLS
#endif /* ND_SIGNER_H */
