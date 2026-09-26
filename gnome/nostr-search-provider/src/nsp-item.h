/*
 * nsp-item — a search result: a validated event from the session relay
 * (profile / note / long-form), or a "bare" identifier result for an
 * npub/note/nevent/naddr/NIP-05 the local store does not hold (so the
 * user can still open it through nostr-dispatcher when the store is
 * empty, e.g. a cache-less session relay built without nostrdb).
 *
 * The result id handed to GNOME Shell is the item's canonical NIP-21 URI
 * (nostr:npub1… / nostr:nevent1… with kind + author TLVs / nostr:naddr1…),
 * so ActivateResult needs no state and the id doubles as clipboardText.
 */
#ifndef NSP_ITEM_H
#define NSP_ITEM_H

#include <glib.h>

#include "nd-event.h"
#include "nd-uri.h"

G_BEGIN_DECLS

#define NSP_PICTURE_URL_MAX 2048

typedef struct {
  gint kind;           /* -1 for a bare item */
  char *id_hex;        /* events (NULL for bare profiles) */
  char *pubkey_hex;    /* author / profile key */
  gint64 created_at;
  char *content;       /* raw note content (not for kind 0) */
  char *d_tag;         /* addressable events */
  char *title;         /* kind 30023 "title" tag */
  char *summary;       /* kind 30023 "summary" tag */
  /* kind 0 metadata */
  char *name, *display_name, *nip05, *picture, *about;
  /* bare items: the identifier as typed/resolved */
  gboolean bare;
  char *uri;           /* result id: canonical nostr: URI */
  char *haystack;      /* folded text used for subsearch narrowing */
  guint gen;           /* engine generation that last returned it */
} NspItem;

/* Build from a VALIDATED event (NULL otherwise, or if the JSON lacks
 * fields). Kind 0 content is parsed as NIP-01 metadata. */
NspItem *nsp_item_new_from_event(const NdEvent *ev);

/* Bare results. @target entity PROFILE/EVENT/ADDRESS; relays dropped. */
NspItem *nsp_item_new_bare(const NdTarget *target, const char *nip05);

void nsp_item_free(NspItem *it);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NspItem, nsp_item_free)

gboolean nsp_item_is_profile(const NspItem *it);

/* Preferred human name of a profile item (display_name, name), sanitized;
 * NULL if it has none. */
char *nsp_item_profile_name(const NspItem *it);

/* Result-meta strings (sanitized, never markup-escaped — see nsp-text.h):
 *   profile: name = display name | name | short npub
 *            description = "[✓ ]nip05 · npub1abc…xyz"   (✓ = verified)
 *   note:    name = "<author> · <relative time>"
 *            description = first 80 code points of content
 *            (kind 30023: title, else summary, else content)
 *   bare:    name = "Open Nostr profile|note|article"
 *            description = nip05 · short bech32
 * @author may be NULL. @nip05_verified: the profile's nip05 was
 * resolved to its pubkey this session. */
void nsp_item_meta_text(const NspItem *it, const NspItem *author, gboolean nip05_verified,
                        gint64 now, char **name_out, char **description_out);

G_END_DECLS

#endif /* NSP_ITEM_H */
