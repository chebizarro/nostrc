/**
 * NIP-03 OpenTimestamps Support for gnostr
 *
 * Implements parsing and verification of OpenTimestamps (OTS) proofs
 * attached to Nostr events via the "ots" tag.
 *
 * NIP-03 defines:
 * - "ots" tag contains base64-encoded OpenTimestamps proof
 * - The proof attests to when an event ID existed (anchored to Bitcoin blockchain)
 * - Verification proves the event existed at or before the timestamp
 */

#ifndef GNOSTR_NIP03_OPENTIMESTAMPS_H
#define GNOSTR_NIP03_OPENTIMESTAMPS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* OpenTimestamps verification status */
typedef enum {
  NIP03_OTS_STATUS_UNKNOWN,      /* No OTS tag or not yet checked */
  NIP03_OTS_STATUS_PENDING,      /* OTS proof present but not verified */
  NIP03_OTS_STATUS_VERIFIED,     /* Successfully verified against Bitcoin */
  NIP03_OTS_STATUS_INVALID,      /* Proof is malformed or verification failed */
  NIP03_OTS_STATUS_UPGRADED,     /* Proof was upgraded (had pending attestations) */
} GnostrOtsStatus;

/* Structure representing parsed OTS proof info */
typedef struct {
  char *event_id_hex;           /* Event ID this proof is for */
  char *ots_proof_base64;       /* Raw base64-encoded OTS proof from tag */
  guint8 *ots_proof_binary;     /* Decoded binary OTS proof data */
  gsize ots_proof_len;          /* Length of binary proof */
  GnostrOtsStatus status;       /* Verification status */
  gint64 verified_timestamp;    /* Bitcoin block timestamp when verified (0 if not verified) */
  guint block_height;           /* Bitcoin block height of attestation (0 if not verified) */
  char *block_hash;             /* Bitcoin block hash (NULL if not verified) */
  gboolean is_complete;         /* TRUE if proof contains complete attestation */
} GnostrOtsProof;

/* Cache entry for storing verification results */
typedef struct {
  char *event_id_hex;
  GnostrOtsStatus status;
  gint64 verified_timestamp;
  guint block_height;
  char *block_hash;
  gint64 cache_time;            /* When this entry was cached */
} GnostrOtsCache;

/* Free a GnostrOtsProof structure */
void gnostr_ots_proof_free(gpointer proof);

/* Free a GnostrOtsCache entry */
void gnostr_ots_cache_free(gpointer cache);

/**
 * Parse the "ots" tag from event tags JSON.
 * @tags_json: JSON array string of event tags
 * @event_id_hex: The event ID (for associating the proof)
 * Returns a newly allocated GnostrOtsProof* or NULL if no "ots" tag.
 * Caller must free with gnostr_ots_proof_free().
 */
GnostrOtsProof *gnostr_nip03_parse_ots_tag(const char *tags_json,
                                            const char *event_id_hex);

/**
 * Parse OTS proof from raw base64 string.
 * @ots_base64: Base64-encoded OTS proof
 * @event_id_hex: The event ID this proof is for
 * Returns a newly allocated GnostrOtsProof* or NULL on error.
 * Caller must free with gnostr_ots_proof_free().
 */
GnostrOtsProof *gnostr_nip03_parse_ots_proof(const char *ots_base64,
                                              const char *event_id_hex);

/**
 * Verify an OTS proof against the event ID.
 * This performs local verification of the proof structure.
 * For full Bitcoin verification, external tooling is required.
 * @proof: The OTS proof to verify
 * @event_id_hex: 64-char hex event ID to verify against
 * Returns TRUE if basic verification passes, FALSE otherwise.
 * Updates proof->status based on verification result.
 */
gboolean gnostr_nip03_verify_proof(GnostrOtsProof *proof,
                                    const char *event_id_hex);

/**
 * Check if an OTS proof header is valid.
 * OTS files start with a specific magic header.
 * @proof: The OTS proof to check
 * Returns TRUE if the proof has a valid OTS header.
 */
gboolean gnostr_nip03_is_valid_ots_header(const GnostrOtsProof *proof);

/**
 * Extract attestation info from OTS proof.
 * Parses the proof to find Bitcoin attestation data.
 * @proof: The OTS proof to parse
 * Returns TRUE if attestation info was found and extracted.
 */
gboolean gnostr_nip03_extract_attestation(GnostrOtsProof *proof);

/**
 * Get cached OTS verification result for an event.
 * @event_id_hex: 64-char hex event ID
 * Returns cached GnostrOtsCache* or NULL if not cached.
 * Do NOT free the returned pointer - it's owned by the cache.
 */
const GnostrOtsCache *gnostr_nip03_get_cached(const char *event_id_hex);

/**
 * Store OTS verification result in cache.
 * @proof: The verified proof to cache
 */
void gnostr_nip03_cache_result(const GnostrOtsProof *proof);

/**
 * Clear expired entries from the OTS cache.
 * Entries older than max_age_seconds are removed.
 */
void gnostr_nip03_prune_cache(gint64 max_age_seconds);

/**
 * Initialize the NIP-03 OTS subsystem.
 * Must be called before using other NIP-03 functions.
 */
void gnostr_nip03_init(void);

/**
 * Shutdown the NIP-03 OTS subsystem and free resources.
 */
void gnostr_nip03_shutdown(void);

/**
 * Format verification timestamp for display.
 * @verified_timestamp: Unix timestamp of Bitcoin attestation
 * Returns newly allocated string like "Verified: Jan 15, 2024".
 * Caller must g_free().
 */
char *gnostr_nip03_format_timestamp(gint64 verified_timestamp);

/**
 * Get human-readable status string.
 * @status: OTS verification status
 * Returns static string describing the status.
 */
const char *gnostr_nip03_status_string(GnostrOtsStatus status);

/**
 * Get icon name for OTS status.
 * @status: OTS verification status
 * Returns static icon name for GTK display.
 */
const char *gnostr_nip03_status_icon(GnostrOtsStatus status);

/**
 * Get CSS class for OTS status styling.
 * @status: OTS verification status
 * Returns static CSS class name.
 */
const char *gnostr_nip03_status_css_class(GnostrOtsStatus status);

/* OTS proof format constants */
#define NIP03_OTS_MAGIC_HEADER "\x00\x4f\x70\x65\x6e\x54\x69\x6d\x65\x73\x74\x61\x6d\x70\x73\x00\x00\x50\x72\x6f\x6f\x66\x00\xbf\x89\xe2\xe8\x84\xe8\x92\x94"
#define NIP03_OTS_MAGIC_LEN 31

/* Operation tag bytes in OTS format */
#define NIP03_OTS_OP_SHA256      0x08
#define NIP03_OTS_OP_RIPEMD160   0x09
#define NIP03_OTS_OP_SHA1        0x0a
#define NIP03_OTS_OP_KECCAK256   0x0b
#define NIP03_OTS_OP_APPEND      0xf0
#define NIP03_OTS_OP_PREPEND     0xf1
#define NIP03_OTS_OP_REVERSE     0xf2
#define NIP03_OTS_OP_HEXLIFY     0xf3
#define NIP03_OTS_OP_ATTESTATION 0x00

/* Attestation type tags */
#define NIP03_OTS_ATTESTATION_BITCOIN "\x05\x88\x96\x0d\x73\xd7\x19\x01"
#define NIP03_OTS_ATTESTATION_PENDING "\x83\xdf\xe3\x0d\x2e\xf9\x0c\x8e"
#define NIP03_OTS_ATTESTATION_LITECOIN "\x06\x86\x9a\x0d\x73\xd7\x1b\x45"

G_END_DECLS

#endif /* GNOSTR_NIP03_OPENTIMESTAMPS_H */
