/**
 * NIP-61: Nutzaps (Ecash Zaps) Utility
 *
 * Nutzaps allow sending ecash (Cashu) tokens as zaps on Nostr.
 *
 * Event Kinds:
 *   - Kind 10019: Nutzap Preferences (replaceable)
 *     User configuration for receiving nutzaps: accepted mints, relays, p2pk
 *   - Kind 9321: Nutzap Event
 *     The actual nutzap containing Cashu proofs
 *
 * Preferences Event (kind 10019):
 *   - content: empty
 *   - tags:
 *     - ["mint", "<mint-url>", "<unit>", "<optional-pubkey>"]
 *     - ["relay", "<relay-url>"]
 *     - ["p2pk"] - if present, requires tokens locked to user's pubkey
 *
 * Nutzap Event (kind 9321):
 *   - content: empty
 *   - tags:
 *     - ["proofs", "<json-array-of-proofs>"]
 *     - ["u", "<mint-url>"]
 *     - ["e", "<event-id>", "<relay>"] - event being zapped
 *     - ["p", "<pubkey>"] - recipient pubkey
 *     - ["a", "<kind:pubkey:d-tag>"] - optional addressable event ref
 */

#ifndef GNOSTR_NIP61_NUTZAPS_H
#define GNOSTR_NIP61_NUTZAPS_H

#include <glib.h>

G_BEGIN_DECLS

/* Nostr event kinds for NIP-61 */
#define NIP61_KIND_NUTZAP_PREFS 10019
#define NIP61_KIND_NUTZAP       9321

/**
 * GnostrNutzapMint:
 *
 * Represents an accepted mint in nutzap preferences.
 */
typedef struct _GnostrNutzapMint {
  gchar *url;      /* Mint URL */
  gchar *unit;     /* Unit: "sat", "usd", "eur", etc. */
  gchar *pubkey;   /* Optional pubkey for this mint (hex, 64 chars) */
} GnostrNutzapMint;

/**
 * GnostrNutzapPrefs:
 *
 * User's nutzap preferences (kind 10019).
 */
typedef struct _GnostrNutzapPrefs {
  GnostrNutzapMint **mints;  /* Array of accepted mints */
  gsize mint_count;          /* Number of mints */
  gchar **relays;            /* Array of relay URLs for nutzaps */
  gsize relay_count;         /* Number of relays */
  gboolean require_p2pk;     /* If TRUE, tokens must be locked to user's pubkey */
} GnostrNutzapPrefs;

/**
 * GnostrCashuProof:
 *
 * A single Cashu proof (token).
 */
typedef struct _GnostrCashuProof {
  gint64 amount;   /* Amount in the smallest unit */
  gchar *id;       /* Keyset ID */
  gchar *secret;   /* Secret (base64 or hex) */
  gchar *C;        /* Signature point (hex) */
} GnostrCashuProof;

/**
 * GnostrNutzap:
 *
 * A nutzap event (kind 9321) with parsed data.
 */
typedef struct _GnostrNutzap {
  gchar *event_id;           /* The nutzap event ID */
  gchar *sender_pubkey;      /* Nutzap sender pubkey (hex) */
  gchar *proofs_json;        /* Raw JSON array of Cashu proofs */
  GnostrCashuProof **proofs; /* Parsed array of proofs */
  gsize proof_count;         /* Number of proofs */
  gchar *mint_url;           /* Mint URL from "u" tag */
  gchar *zapped_event_id;    /* Event being zapped (from "e" tag) */
  gchar *zapped_event_relay; /* Relay hint for the zapped event */
  gchar *recipient_pubkey;   /* Recipient pubkey (from "p" tag) */
  gchar *addressable_ref;    /* Optional addressable event ref (from "a" tag) */
  gint64 amount_sat;         /* Total amount in satoshis (calculated from proofs) */
  gint64 created_at;         /* Event creation timestamp */
} GnostrNutzap;

/* ============== Nutzap Mint API ============== */

/**
 * gnostr_nutzap_mint_new:
 *
 * Creates a new empty nutzap mint.
 *
 * Returns: (transfer full): A new mint. Free with gnostr_nutzap_mint_free().
 */
GnostrNutzapMint *gnostr_nutzap_mint_new(void);

/**
 * gnostr_nutzap_mint_new_full:
 * @url: Mint URL.
 * @unit: Unit string (e.g., "sat", "usd").
 * @pubkey: (nullable): Optional pubkey for this mint.
 *
 * Creates a new nutzap mint with values.
 *
 * Returns: (transfer full): A new mint. Free with gnostr_nutzap_mint_free().
 */
GnostrNutzapMint *gnostr_nutzap_mint_new_full(const gchar *url,
                                               const gchar *unit,
                                               const gchar *pubkey);

/**
 * gnostr_nutzap_mint_free:
 * @mint: Mint to free.
 *
 * Frees a nutzap mint.
 */
void gnostr_nutzap_mint_free(GnostrNutzapMint *mint);

/* ============== Nutzap Preferences API ============== */

/**
 * gnostr_nutzap_prefs_new:
 *
 * Creates a new empty nutzap preferences structure.
 *
 * Returns: (transfer full): New prefs. Free with gnostr_nutzap_prefs_free().
 */
GnostrNutzapPrefs *gnostr_nutzap_prefs_new(void);

/**
 * gnostr_nutzap_prefs_free:
 * @prefs: Preferences to free.
 *
 * Frees nutzap preferences and all contained data.
 */
void gnostr_nutzap_prefs_free(GnostrNutzapPrefs *prefs);

/**
 * gnostr_nutzap_prefs_parse:
 * @event_json: JSON string of a kind 10019 event.
 *
 * Parses nutzap preferences from a kind 10019 event.
 *
 * Returns: (transfer full) (nullable): Parsed preferences or NULL on error.
 */
GnostrNutzapPrefs *gnostr_nutzap_prefs_parse(const gchar *event_json);

/**
 * gnostr_nutzap_prefs_add_mint:
 * @prefs: Preferences to modify.
 * @mint: (transfer full): Mint to add (ownership transferred).
 *
 * Adds a mint to the preferences.
 */
void gnostr_nutzap_prefs_add_mint(GnostrNutzapPrefs *prefs,
                                   GnostrNutzapMint *mint);

/**
 * gnostr_nutzap_prefs_add_relay:
 * @prefs: Preferences to modify.
 * @relay_url: Relay URL to add.
 *
 * Adds a relay to the preferences.
 */
void gnostr_nutzap_prefs_add_relay(GnostrNutzapPrefs *prefs,
                                    const gchar *relay_url);

/**
 * gnostr_nutzap_prefs_build_tags:
 * @prefs: Preferences to build tags for.
 *
 * Builds a GPtrArray of tags for a kind 10019 event.
 * Each tag is a GPtrArray of gchar* elements.
 *
 * Returns: (transfer full) (element-type GPtrArray): Array of tag arrays.
 */
GPtrArray *gnostr_nutzap_prefs_build_tags(const GnostrNutzapPrefs *prefs);

/**
 * gnostr_nutzap_prefs_build_event_json:
 * @prefs: Preferences to build event for.
 * @pubkey: Author's pubkey (hex, 64 chars).
 *
 * Builds an unsigned kind 10019 event JSON string.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event.
 */
gchar *gnostr_nutzap_prefs_build_event_json(const GnostrNutzapPrefs *prefs,
                                             const gchar *pubkey);

/**
 * gnostr_nutzap_prefs_accepts_mint:
 * @prefs: Preferences to check.
 * @mint_url: Mint URL to check.
 *
 * Checks if the preferences accept a given mint.
 *
 * Returns: TRUE if the mint is accepted.
 */
gboolean gnostr_nutzap_prefs_accepts_mint(const GnostrNutzapPrefs *prefs,
                                           const gchar *mint_url);

/* ============== Cashu Proof API ============== */

/**
 * gnostr_cashu_proof_new:
 *
 * Creates a new empty Cashu proof.
 *
 * Returns: (transfer full): A new proof. Free with gnostr_cashu_proof_free().
 */
GnostrCashuProof *gnostr_cashu_proof_new(void);

/**
 * gnostr_cashu_proof_free:
 * @proof: Proof to free.
 *
 * Frees a Cashu proof.
 */
void gnostr_cashu_proof_free(GnostrCashuProof *proof);

/**
 * gnostr_cashu_proofs_parse:
 * @proofs_json: JSON array string of Cashu proofs.
 * @out_count: (out): Number of parsed proofs.
 *
 * Parses Cashu proofs from a JSON array string.
 *
 * Returns: (transfer full) (array length=out_count) (nullable):
 *          Array of proofs or NULL on error.
 */
GnostrCashuProof **gnostr_cashu_proofs_parse(const gchar *proofs_json,
                                              gsize *out_count);

/**
 * gnostr_cashu_proofs_free:
 * @proofs: Array of proofs to free.
 * @count: Number of proofs.
 *
 * Frees an array of Cashu proofs.
 */
void gnostr_cashu_proofs_free(GnostrCashuProof **proofs, gsize count);

/**
 * gnostr_cashu_proofs_total_amount:
 * @proofs: Array of proofs.
 * @count: Number of proofs.
 *
 * Calculates the total amount from an array of proofs.
 *
 * Returns: Total amount in the smallest unit.
 */
gint64 gnostr_cashu_proofs_total_amount(GnostrCashuProof * const *proofs,
                                         gsize count);

/* ============== Nutzap API ============== */

/**
 * gnostr_nutzap_new:
 *
 * Creates a new empty nutzap.
 *
 * Returns: (transfer full): A new nutzap. Free with gnostr_nutzap_free().
 */
GnostrNutzap *gnostr_nutzap_new(void);

/**
 * gnostr_nutzap_free:
 * @nutzap: Nutzap to free.
 *
 * Frees a nutzap and all its contents.
 */
void gnostr_nutzap_free(GnostrNutzap *nutzap);

/**
 * gnostr_nutzap_parse:
 * @event_json: JSON string of a kind 9321 event.
 *
 * Parses a nutzap from a kind 9321 event.
 *
 * Returns: (transfer full) (nullable): Parsed nutzap or NULL on error.
 */
GnostrNutzap *gnostr_nutzap_parse(const gchar *event_json);

/**
 * gnostr_nutzap_build_tags:
 * @proofs_json: JSON array of Cashu proofs.
 * @mint_url: Mint URL.
 * @event_id: (nullable): Event being zapped.
 * @event_relay: (nullable): Relay hint for the event.
 * @recipient_pubkey: Recipient's pubkey (hex).
 * @addressable_ref: (nullable): Optional addressable event reference.
 *
 * Builds a GPtrArray of tags for a kind 9321 nutzap event.
 * Each tag is a GPtrArray of gchar* elements.
 *
 * Returns: (transfer full) (element-type GPtrArray): Array of tag arrays.
 */
GPtrArray *gnostr_nutzap_build_tags(const gchar *proofs_json,
                                     const gchar *mint_url,
                                     const gchar *event_id,
                                     const gchar *event_relay,
                                     const gchar *recipient_pubkey,
                                     const gchar *addressable_ref);

/**
 * gnostr_nutzap_build_event_json:
 * @proofs_json: JSON array of Cashu proofs.
 * @mint_url: Mint URL.
 * @event_id: (nullable): Event being zapped.
 * @event_relay: (nullable): Relay hint for the event.
 * @recipient_pubkey: Recipient's pubkey (hex).
 * @addressable_ref: (nullable): Optional addressable event reference.
 * @sender_pubkey: Sender's pubkey (hex).
 *
 * Builds an unsigned kind 9321 nutzap event JSON string.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event.
 */
gchar *gnostr_nutzap_build_event_json(const gchar *proofs_json,
                                       const gchar *mint_url,
                                       const gchar *event_id,
                                       const gchar *event_relay,
                                       const gchar *recipient_pubkey,
                                       const gchar *addressable_ref,
                                       const gchar *sender_pubkey);

/* ============== Utility Functions ============== */

/**
 * gnostr_nutzap_format_amount:
 * @amount_sat: Amount in satoshis.
 *
 * Formats a nutzap amount for display (e.g., "21 sats", "1.5K sats").
 *
 * Returns: (transfer full): Formatted string.
 */
gchar *gnostr_nutzap_format_amount(gint64 amount_sat);

/**
 * gnostr_nutzap_is_valid_mint_url:
 * @url: URL to validate.
 *
 * Checks if a URL is a valid mint URL (https scheme, reasonable length).
 *
 * Returns: TRUE if the URL appears to be a valid mint URL.
 */
gboolean gnostr_nutzap_is_valid_mint_url(const gchar *url);

G_END_DECLS

#endif /* GNOSTR_NIP61_NUTZAPS_H */
