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
 * The safety codes below belong to the same honesty rules: they let two
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

/* ---- Safety codes (per-person fingerprints) ------------------------------------
 * Each person's safety code comes from their own public key only, as
 * Signal's safety numbers do: Verify Key shows "their code" (from the key
 * this device holds for them) and "your code" (from the account's key), and
 * the two people compare both, in person or on a call they trust: their
 * Groundhog shows the same two codes the other way round.
 *
 * Why per person (W15 review B2). A code over both keys, as G19 first had,
 * lets a man in the middle who shows each side a key of their own (K1 as
 * Bob to Alice, K2 as Alice to Bob) search for a pair with code(A, K1) ==
 * code(K2, B): a collision between two sets they choose, about 2^(n/2)
 * work. Per person, the attacker must instead find K1 whose code equals
 * Bob's and K2 whose code equals Alice's: a second preimage of a fixed code
 * each, about 2^106 tries (times the iterations) for
 * GH_PRIVACY_FINGERPRINT_DIGITS decimal digits.
 *
 * The code: SHA-512("groundhog-fingerprint-v1" || key), then
 * GH_PRIVACY_FINGERPRINT_ITERATIONS - 1 more times SHA-512(digest || key)
 * (key: the 32 raw bytes); group i of GH_PRIVACY_FINGERPRINT_GROUP digits is
 * bytes 5i..5i+4 of the final digest, big-endian, mod 10000, zero-padded.
 * Groups are separated by single spaces, for reading aloud.
 *
 * What matching proves: that the key each device holds for the other is the
 * key the other device uses, provided the comparison itself reached the
 * real person. It says nothing about who they are beyond that, and only
 * Groundhog shows these codes: with another app, the full npub is the check.
 * A "verified" mark (gh-store-contacts.h) records a key and a time, never a
 * code, so it does not depend on this format. */
#define GH_PRIVACY_FINGERPRINT_DIGITS 32
#define GH_PRIVACY_FINGERPRINT_GROUP 4
#define GH_PRIVACY_FINGERPRINT_ITERATIONS 5200

/* The safety code of @pubkey (64 hex characters, any case): e.g. "0123 4567
 * …" (GH_PRIVACY_FINGERPRINT_DIGITS digits in groups). NULL for anything
 * that is not a 64-character hex key. */
gchar *gh_privacy_fingerprint(const gchar *pubkey);
/* The code as a screen reader should say it: one digit at a time, a pause
 * (",") between groups: "0 1 2 3, 4 5 6 7, …". */
gchar *gh_privacy_fingerprint_spoken(const gchar *fingerprint);

/* A bech32 key for reading aloud: the "npub1" prefix, then groups of four
 * characters separated by spaces ("npub1 abcd efgh … xy"). Other text is
 * grouped in fours from the start. */
gchar *gh_privacy_format_key(const gchar *key);

G_END_DECLS
#endif
