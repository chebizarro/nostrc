/* nseal-signer.h — org.nostr.Signer client lane for nostr-seal.
 * SPDX-License-Identifier: MIT
 *
 * The secret key stays in the signer daemon. Decrypt asks it to open the
 * stanza's standard NIP-44 v2 payload with NIP44DecryptB64ForApp. Only the
 * per-file key is returned; no conversation key is exported. */
#ifndef NSEAL_SIGNER_H
#define NSEAL_SIGNER_H

#include "nostr-seal.h"

G_BEGIN_DECLS

#define NSEAL_SIGNER_APP_ID "org.nostr.Seal"

typedef struct _NsealSigner NsealSigner;

/* Connect to org.nostr.Signer on the session bus. identity is the signer's
 * selector (npub1… or ""/NULL for the active identity). */
NsealSigner *nseal_signer_new(const char *identity, GError **error);
void         nseal_signer_free(NsealSigner *s);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsealSigner, nseal_signer_free)

/* Public key of the selected identity: the selector itself when it is an
 * npub, else the signer's GetPublicKey. */
gboolean nseal_signer_public_key(NsealSigner *s, uint8_t out[NSEAL_PUBKEY_LEN], GError **error);

/* NsealUnwrapFunc; user_data = NsealSigner*. */
gboolean nseal_signer_unwrap(gpointer signer,
                             const uint8_t ephemeral_pk[NSEAL_PUBKEY_LEN],
                             const char *payload,
                             uint8_t out_file_key[NSEAL_FILE_KEY_LEN],
                             GError **error);

G_END_DECLS

#endif
