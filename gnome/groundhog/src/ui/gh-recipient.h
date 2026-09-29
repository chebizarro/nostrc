#ifndef GH_RECIPIENT_H
#define GH_RECIPIENT_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * What someone typed or pasted into New Message (privacy charter §7.9, G18),
 * classified offline: nothing here touches the network, the signer or any
 * store. GTK-free, so it is tested without a display.
 *
 *  - PUBKEY: an npub, an nprofile or a nostr: URI of either (a leading "@"
 *    is ignored), checksum-verified. An nprofile's relay hints are dropped:
 *    Groundhog never contacts a relay because a pasted identifier named it
 *    (charter P1); the person's own signed 10050 decides where messages go.
 *  - NIP05: name@domain, normalized to lower case. It names a person only
 *    after the user chooses to look it up (the consent row), which contacts
 *    domain over HTTPS (gh-nip05.h).
 *  - SECRET: an nsec, ncryptsec or bunker:// secret. Refused, never kept:
 *    New Message needs a public identifier only.
 *  - OTHER_ENTITY: a NIP-19 note, nevent, naddr or nrelay (not a person).
 *  - INVALID: looks like an npub/nprofile (or a nostr: URI) but does not
 *    decode (typo, truncated paste).
 *  - TEXT: anything else, used as a search of the local contacts only.
 */

typedef enum {
  GH_RECIPIENT_INPUT_EMPTY,
  GH_RECIPIENT_INPUT_PUBKEY,
  GH_RECIPIENT_INPUT_NIP05,
  GH_RECIPIENT_INPUT_SECRET,
  GH_RECIPIENT_INPUT_OTHER_ENTITY,
  GH_RECIPIENT_INPUT_INVALID,
  GH_RECIPIENT_INPUT_TEXT
} GhRecipientInputKind;

typedef struct {
  GhRecipientInputKind kind;
  gchar *text;         /* the trimmed input */
  gchar *pubkey;       /* PUBKEY: lowercase hex */
  gchar *nip05;        /* NIP05: "local@domain", lower case */
  gchar *nip05_local;
  gchar *nip05_domain;
} GhRecipientInput;

GhRecipientInput *gh_recipient_input_parse(const gchar *text);
void gh_recipient_input_free(GhRecipientInput *input);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhRecipientInput, gh_recipient_input_free)

/* NIP-05 address syntax (NIP-05: local part a-z0-9-_. in any case; a host
 * name of ASCII letters, digits and hyphens with at least two labels and a
 * non-numeric top-level label, so no IP literal, port or path). The parts
 * come back in lower case; FALSE (nothing set) otherwise. */
gboolean gh_recipient_parse_nip05(const gchar *address, gchar **out_local,
                                  gchar **out_domain);

/* Whether value is 64 hex characters. */
gboolean gh_recipient_is_pubkey(const gchar *value);
/* The npub of a hex pubkey; NULL if it is not one. */
gchar *gh_recipient_npub(const gchar *pubkey_hex);
/* "npub1abcde…wxyz" (list titles); the hex itself if it is not one. */
gchar *gh_recipient_npub_short(const gchar *pubkey_hex);
/* The npub in space-separated groups of four characters after "npub1", for
 * reading and comparing it aloud (charter §7.9 confirm page). */
gchar *gh_recipient_npub_grouped(const gchar *pubkey_hex);

G_END_DECLS
#endif
