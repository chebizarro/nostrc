/* ns-private.h - Private shares to one npub: NIP-17 over NIP-59 (nostrc-k95e)
 *
 * SPDX-License-Identifier: MIT
 *
 *   rumor   kind 14 (text, links) or kind 15 (a file: its AES-256-GCM
 *           ciphertext on Blossom, key and nonce inside the rumor);
 *           unsigned, with its id; pubkey = the sender
 *   seal    kind 13: NIP-44 of the rumor to the receiver, no tags,
 *           created_at up to two days in the past; encrypted and signed
 *           by org.nostr.Signer (the nsec never enters this process)
 *   wrap    kind 1059: NIP-44 of the seal from a throwaway key made for
 *           this one wrap, only a `p` tag, created_at randomised the same
 *           way; published only to the receiver's kind-10050 relays
 *
 * One wrap per receiver: the recipient, and a copy to the sender's own
 * inbox when the sender has a kind 10050.
 */
#ifndef NS_PRIVATE_H
#define NS_PRIVATE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <nostr-publish/nostr-publish.h>

#include "ns-event.h"

G_BEGIN_DECLS

#define NS_KIND_SEAL            13
#define NS_KIND_PRIVATE_MESSAGE 14
#define NS_KIND_PRIVATE_FILE    15
#define NS_KIND_GIFT_WRAP       1059
#define NS_KIND_DM_RELAYS       10050

/* A rumor's JSON must fit NIP-44 (65535 bytes) once more inside the seal
 * (base64 + event envelope). */
#define NS_PRIVATE_MAX_RUMOR_BYTES 45000

typedef struct {
  guint8 key[32];
  guint8 nonce[12];
} NsFileKey;

/* AES-256-GCM with a fresh random key and nonce (NIP-17 kind 15,
 * `encryption-algorithm aes-gcm`): returns ciphertext || 16-byte tag. */
GBytes *ns_private_encrypt_file(GBytes *plain, NsFileKey *out_key, GError **error);
GBytes *ns_private_decrypt_file(GBytes *sealed, const NsFileKey *key, GError **error);
void    ns_file_key_clear(NsFileKey *key);   /* wipes */

/* NIP-17 kind-15 tags after the `p` tag: file-type, encryption-algorithm,
 * decryption-key, decryption-nonce (lower-case hex), x (of the
 * ciphertext), ox (of the plaintext), size (ciphertext), dim?. */
void ns_tags_add_private_file(JsonArray       *tags,
                              const NsBlobMeta *sealed,
                              const gchar     *plain_mime,
                              const gchar     *plain_sha256,
                              guint            width,
                              guint            height,
                              const NsFileKey *key);

/* An unsigned rumor: {"id","pubkey","created_at","kind","tags","content"}
 * with the NIP-01 id. Without @sender_hex (draft, no signer yet) there is
 * no pubkey and no id. */
gchar *ns_private_rumor_json(gint         kind,
                             gint64       created_at,
                             const gchar *sender_hex,
                             JsonArray   *tags,
                             const gchar *content,
                             gboolean     pretty);

/* Seal @rumor_json for @receiver_hex through @signer (NIP44Encrypt +
 * SignEvent), check the seal is the sender's, then gift-wrap it with a new
 * throwaway key. Returns the signed kind-1059 JSON. */
gchar *ns_private_gift_wrap(NostrPublishSigner *signer,
                            const gchar        *sender_hex,
                            const gchar        *rumor_json,
                            const gchar        *receiver_hex,
                            GError            **error);

/* Relays of a kind-10050 event (`relay` tags; ws:// and wss:// only, in
 * order, deduplicated). Empty (never NULL) when there are none. */
GStrv ns_private_inbox_relays(const gchar *event_json);

G_END_DECLS

#endif /* NS_PRIVATE_H */
