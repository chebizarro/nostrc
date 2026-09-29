#ifndef GH_PRIVACY_SUMMARY_H
#define GH_PRIVACY_SUMMARY_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * What a conversation protects and what it doesn't, in plain language
 * (privacy charter §1.4, §2.2, §3.7, D7; item G19). GTK-free: the one strings
 * table behind the conversation header's subtitle (§2.2 surface 1), the
 * Privacy group of Conversation Info (surface 2) and, later, the group
 * dialogs (G20). Every sentence is a claim the protocol and Groundhog keep;
 * none promises more (P4): a relay group is never called encrypted,
 * disappearing messages are a request, not a guarantee, and nothing hides
 * that you use Nostr or which relays you use (Groundhog has no proxy or Tor
 * support yet, §4.1). Strings are translatable (English source).
 *
 * The safety code below belongs to the same honesty rules: it lets two
 * people compare their keys out of band; Groundhog never checks anything
 * with anyone and only remembers that you marked a key verified.
 */

/* Values equal GhConversationBackend and the store's conversations.backend
 * (charter §3.3). */
typedef enum {
  GH_PRIVACY_BACKEND_NIP17 = 1, /* private conversation: NIP-17 gift wraps */
  GH_PRIVACY_BACKEND_NIP29 = 2, /* relay group: plain text on its relay */
  GH_PRIVACY_BACKEND_MLS = 3    /* encrypted group: Marmot (MLS) */
} GhPrivacyBackend;

/* Where Groundhog itself keeps this account's messages (the Stored on This
 * Device line). The zero value is the normal encrypted store. */
typedef enum {
  GH_PRIVACY_STORAGE_SAVED = 0, /* the encrypted store on disk */
  GH_PRIVACY_STORAGE_MEMORY,    /* "Continue Without Saving": memory only */
  GH_PRIVACY_STORAGE_NONE       /* no store is open: nothing is kept */
} GhPrivacyStorage;

typedef struct {
  GhPrivacyBackend backend;
  /* NIP-17 with exactly one other person: the name the UI shows for them
   * (petname, cached display name or short npub). NULL: "the other person". */
  const gchar *peer_name;
  /* NIP-17: the people other than you (0: a note to self). MLS: the members,
   * you included (0: unknown). Unused for NIP-29. */
  guint n_people;
  /* NIP-29: the group relay's host name. NULL: "the group's relay". */
  const gchar *relay_host;
  /* NIP-17 only, subtitle only: a message request, with the subject its
   * sender chose (nullable), which is shown as secondary text (charter §7.9). */
  gboolean is_request;
  const gchar *subject;
  /* The storage line only. */
  GhPrivacyStorage storage;
} GhPrivacyContext;

typedef struct {
  GhPrivacyBackend backend;
  gboolean end_to_end;     /* only the participants can read the messages */
  const gchar *icon_name;  /* symbolic icon for the heading */
  gchar *subtitle;         /* the conversation header's subtitle */
  gchar *heading;          /* "End-to-end encrypted" / "Not end-to-end encrypted" */
  gchar *encrypted;        /* who can read the messages, and what relays can't see */
  GStrv visible;           /* what relays and other people can see */
  GStrv unprotected;       /* what Groundhog can't protect */
  gchar *storage;          /* where Groundhog keeps the messages (D7) */
} GhPrivacySummary;

/* NULL for an unknown backend. */
GhPrivacySummary *gh_privacy_summary_new(const GhPrivacyContext *context);
void gh_privacy_summary_free(GhPrivacySummary *summary);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhPrivacySummary, gh_privacy_summary_free)

/* The header subtitle alone (charter §2.2 surface 1), e.g. "Private ·
 * end-to-end encrypted", "Relay group · not end-to-end encrypted" or
 * "Encrypted group · 4 members"; NULL for an unknown backend. */
gchar *gh_privacy_summary_dup_subtitle(const GhPrivacyContext *context);

/* Every field of summary as stable plain text, one item per line (snapshot
 * tests; also a complete description for a screen reader). */
gchar *gh_privacy_summary_to_text(const GhPrivacySummary *summary);

/* ---- Safety code ----------------------------------------------------------------
 * A short code two people can compare in person or on a call they trust. It
 * is derived from both public keys only, so each side computes the same
 * code: SHA-256("groundhog-safety-code-v1" || lower key || higher key), its
 * first 60 bits read as GH_PRIVACY_SAFETY_CODE_LENGTH 6-bit indices into a
 * table of 64 symbols (an emoji and a name each). Matching codes mean both
 * sides hold each other's real keys; only an app that computes the same code
 * (Groundhog) shows it, so the full npub stays the universal check. 60 bits
 * keep grinding a look-alike key (one secp256k1 key per try) out of reach of
 * a casual attacker; comparing the full npub is the complete check. */
#define GH_PRIVACY_SAFETY_CODE_LENGTH 10
#define GH_PRIVACY_SAFETY_SYMBOLS 64

/* Fills code with symbol indices (< GH_PRIVACY_SAFETY_SYMBOLS). FALSE when
 * either key is not 64 hex characters or both are the same key. */
gboolean gh_privacy_safety_code(const gchar *pubkey_a, const gchar *pubkey_b,
                                guint8 code[GH_PRIVACY_SAFETY_CODE_LENGTH]);
/* The emoji of symbol index, and its translated name ("Dog", "Rocket"). */
const gchar *gh_privacy_safety_symbol_emoji(guint index);
const gchar *gh_privacy_safety_symbol_name(guint index);
/* The code as text: "Dog Rocket Key …" (names, space-separated). */
gchar *gh_privacy_safety_code_to_text(const guint8 code[GH_PRIVACY_SAFETY_CODE_LENGTH]);

/* A bech32 key for reading aloud: the "npub1" prefix, then groups of four
 * characters separated by spaces ("npub1 abcd efgh … xy"). Other text is
 * grouped in fours from the start. */
gchar *gh_privacy_format_key(const gchar *key);

G_END_DECLS
#endif
