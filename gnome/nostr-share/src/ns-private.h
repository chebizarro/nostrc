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

/* The rumor is NIP-44-encrypted into the seal, and the seal's JSON is
 * NIP-44-encrypted into the wrap, whose plaintext limit is 65535 bytes:
 * base64(45000 + NIP-44 overhead ≈ 45100) ≈ 60200, plus the seal's
 * envelope (id, pubkey, sig, created_at, kind, tags ≈ 300 bytes) ≈ 60500,
 * leaving ~5 KB of headroom for future rumor tags. */
#define NS_PRIVATE_MAX_RUMOR_BYTES 45000

/* At most this many inbox relays are taken from someone's kind 10050. */
#define NS_PRIVATE_MAX_INBOX_RELAYS 8

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

/* Whether nostr-share will connect to @url when it comes from someone
 * else's event (a kind-10050 inbox, their NIP-65 list): wss:// only, and
 * never a loopback, private, link-local or `localhost` host — a hostile
 * list must not steer connections at local services (or at relay.sock,
 * which ws://localhost/ maps to), and plaintext ws:// would show every
 * path observer the wrap's recipient. @allow_loopback admits ws:// and
 * wss:// to loopback hosts (tests only). */
gboolean ns_private_remote_relay_ok(const gchar *url, gboolean allow_loopback);

/* Relays of a kind-10050 event (`relay` tags passing
 * ns_private_remote_relay_ok(), in order, deduplicated, at most
 * NS_PRIVATE_MAX_INBOX_RELAYS). Empty (never NULL) when none pass;
 * @out_dropped (optional) counts the entries refused. */
GStrv ns_private_inbox_relays(const gchar *event_json, gboolean allow_loopback,
                              guint *out_dropped);

G_END_DECLS

#endif /* NS_PRIVATE_H */
