/* nseal-publish.h — publish a sealed file (nostrc-hby8).
 * SPDX-License-Identifier: MIT
 *
 * Uploads the .nsealed blob to the user's Blossom servers and publishes a
 * NIP-94 kind-1063 file-metadata event p-tagging every NIP-44 recipient in
 * the file's own header (so the tags cannot drift from who can open it).
 * Reuses nostr-share's BUD-03 discovery, GnBlossomClient for transport,
 * BUD-02 for kind-24242 auth through org.nostr.Signer, and
 * libnostr-publish (NostrPublisher + transport factory); signing goes
 * through org.nostr.Signer as org.nostr.Seal. Target relays come from
 * nostr_publish_policy_select_targets() with seal.conf's upstream_mode.
 *
 * Discovery relays and the Blossom fallback list are nostr-share's
 * (~/.config/nostr-share/nostr-share.conf: home_relays, blossom_servers).
 */
#ifndef NSEAL_PUBLISH_H
#define NSEAL_PUBLISH_H

#include "nostr-seal.h"
#ifdef NSEAL_HAVE_PUBLISH
#include <nostr-publish/nostr-publish.h>
#endif

G_BEGIN_DECLS

typedef struct {
  gboolean dry_run;         /* print the kind-1063 event; upload/publish nothing */
  const char *identity;     /* signer identity (npub) or NULL for the active one */
} NsealPublishOptions;

/* TRUE when built with nostr-share (ENABLE_NOSTR_SHARE). */
gboolean nseal_publish_available(void);

/* Kind-1063 tags for a sealed blob (pure; exposed for tests): url, m, x,
 * ox, size, alt, then one p per recipient (64-hex). Returns compact
 * unsigned event JSON. */
char *nseal_publish_event_json(const char *pubkey_hex, gint64 created_at, const char *url,
                               const char *sha256_hex, guint64 size,
                               const char *const *recipients_hex, gboolean pretty);

gboolean nseal_publish_file(const char *path, const NsealPublishOptions *opts, GError **error);

#ifdef NSEAL_HAVE_PUBLISH
/* Private upload engine. Production passes FALSE for loopback_http; TRUE is
 * only for an isolated loopback fixture, never a user-configurable policy. */
gboolean nseal_publish_upload(const char *const *servers, NostrPublishSigner *signer,
                              const char *pubkey, GBytes *bytes, const char *hash,
                              gboolean loopback_http, char **out_url, GError **error);
#endif

G_END_DECLS

#endif
