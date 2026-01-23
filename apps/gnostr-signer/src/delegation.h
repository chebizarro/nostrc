/* delegation.h - NIP-26 delegation token management for gnostr-signer
 *
 * Implements NIP-26 delegation allowing one key (delegator) to grant
 * signing authority to another key (delegatee) with optional restrictions:
 * - Event kind restrictions (e.g., only kind 1 notes)
 * - Time-bound validity (valid_from, valid_until timestamps)
 *
 * Delegation format per NIP-26:
 * - Delegation tag: ["delegation", delegator_pubkey, conditions, sig]
 * - Conditions string: "kind=1&created_at>1234&created_at<5678"
 * - Signature: schnorr_sign(sha256(sha256(delegatee_pubkey || conditions)))
 */
#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Error domain for GError integration */
#define GN_DELEGATION_ERROR (gn_delegation_error_quark())
GQuark gn_delegation_error_quark(void);

/* Result codes */
typedef enum {
  GN_DELEGATION_OK = 0,
  GN_DELEGATION_ERR_INVALID_PUBKEY,
  GN_DELEGATION_ERR_INVALID_CONDITIONS,
  GN_DELEGATION_ERR_SIGN_FAILED,
  GN_DELEGATION_ERR_NOT_FOUND,
  GN_DELEGATION_ERR_EXPIRED,
  GN_DELEGATION_ERR_REVOKED,
  GN_DELEGATION_ERR_IO,
  GN_DELEGATION_ERR_PARSE
} GnDelegationResult;

/**
 * GnDelegation:
 * @id: Unique identifier for this delegation (hex of first 8 bytes of signature)
 * @delegator_npub: Public key of the delegator (the identity granting authority)
 * @delegatee_pubkey_hex: Hex public key of delegatee (who receives authority)
 * @allowed_kinds: GArray of guint16 event kinds allowed, or NULL for all kinds
 * @valid_from: Unix timestamp when delegation becomes valid (0 = immediate)
 * @valid_until: Unix timestamp when delegation expires (0 = no expiry)
 * @conditions: The conditions string (e.g., "kind=1&created_at>1234&created_at<5678")
 * @signature: Hex schnorr signature of the delegation
 * @created_at: When this delegation was created
 * @revoked: Whether this delegation has been revoked
 * @revoked_at: When this delegation was revoked (if revoked)
 * @label: Optional user-defined label for this delegation
 *
 * Represents a NIP-26 delegation token.
 */
typedef struct {
  gchar *id;
  gchar *delegator_npub;
  gchar *delegatee_pubkey_hex;
  GArray *allowed_kinds;        /* GArray of guint16, NULL = all kinds */
  gint64 valid_from;            /* Unix timestamp, 0 = immediate */
  gint64 valid_until;           /* Unix timestamp, 0 = no expiry */
  gchar *conditions;
  gchar *signature;
  gint64 created_at;
  gboolean revoked;
  gint64 revoked_at;
  gchar *label;
} GnDelegation;

/**
 * gn_delegation_new:
 *
 * Create a new empty delegation struct.
 *
 * Returns: (transfer full): New delegation, free with gn_delegation_free()
 */
GnDelegation *gn_delegation_new(void);

/**
 * gn_delegation_free:
 * @delegation: Delegation to free
 *
 * Free a delegation and all its fields.
 */
void gn_delegation_free(GnDelegation *delegation);

/**
 * gn_delegation_copy:
 * @delegation: Delegation to copy
 *
 * Create a deep copy of a delegation.
 *
 * Returns: (transfer full): Copy of delegation
 */
GnDelegation *gn_delegation_copy(const GnDelegation *delegation);

/**
 * gn_delegation_create:
 * @delegator_npub: npub of the delegator (must have private key access)
 * @delegatee_pubkey_hex: Hex public key of the delegatee
 * @allowed_kinds: (nullable): GArray of guint16 kinds to allow, or NULL for all
 * @valid_from: Start timestamp (0 for immediate)
 * @valid_until: End timestamp (0 for no expiry)
 * @label: (nullable): Optional user label
 * @out_delegation: (out): Created delegation on success
 *
 * Create and sign a new NIP-26 delegation token.
 *
 * Returns: Result code
 */
GnDelegationResult gn_delegation_create(const gchar *delegator_npub,
                                         const gchar *delegatee_pubkey_hex,
                                         GArray *allowed_kinds,
                                         gint64 valid_from,
                                         gint64 valid_until,
                                         const gchar *label,
                                         GnDelegation **out_delegation);

/**
 * gn_delegation_revoke:
 * @delegator_npub: npub of the delegator who owns this delegation
 * @delegation_id: ID of the delegation to revoke
 *
 * Revoke an active delegation. This marks the delegation as revoked
 * in storage. Revocation is local-only (NIP-26 has no on-chain revocation).
 *
 * Returns: Result code
 */
GnDelegationResult gn_delegation_revoke(const gchar *delegator_npub,
                                         const gchar *delegation_id);

/**
 * gn_delegation_is_valid:
 * @delegation: Delegation to check
 * @event_kind: Event kind to check against (0 to skip kind check)
 * @timestamp: Timestamp to check against (0 for current time)
 *
 * Check if a delegation is currently valid (not revoked, not expired,
 * kind allowed).
 *
 * Returns: TRUE if delegation is valid for the given parameters
 */
gboolean gn_delegation_is_valid(const GnDelegation *delegation,
                                 guint16 event_kind,
                                 gint64 timestamp);

/**
 * gn_delegation_build_conditions:
 * @allowed_kinds: (nullable): Array of allowed kinds
 * @valid_from: Start timestamp (0 to omit)
 * @valid_until: End timestamp (0 to omit)
 *
 * Build the NIP-26 conditions string from parameters.
 * Example: "kind=1&kind=7&created_at>1700000000&created_at<1800000000"
 *
 * Returns: (transfer full): Conditions string
 */
gchar *gn_delegation_build_conditions(GArray *allowed_kinds,
                                       gint64 valid_from,
                                       gint64 valid_until);

/**
 * gn_delegation_build_tag:
 * @delegation: The delegation
 *
 * Build the NIP-26 delegation tag array for inclusion in events.
 * Returns: ["delegation", delegator_pubkey_hex, conditions, sig]
 *
 * Returns: (transfer full): JSON array string
 */
gchar *gn_delegation_build_tag(const GnDelegation *delegation);

/* ======== Persistence ======== */

/**
 * gn_delegation_list:
 * @delegator_npub: npub of the delegator
 * @include_revoked: Whether to include revoked delegations
 *
 * List all delegations for a given delegator identity.
 *
 * Returns: (transfer full): GPtrArray of GnDelegation*, caller owns
 */
GPtrArray *gn_delegation_list(const gchar *delegator_npub,
                               gboolean include_revoked);

/**
 * gn_delegation_get:
 * @delegator_npub: npub of the delegator
 * @delegation_id: ID of the delegation
 * @out_delegation: (out): The delegation if found
 *
 * Get a specific delegation by ID.
 *
 * Returns: Result code
 */
GnDelegationResult gn_delegation_get(const gchar *delegator_npub,
                                      const gchar *delegation_id,
                                      GnDelegation **out_delegation);

/**
 * gn_delegation_save:
 * @delegator_npub: npub of the delegator
 * @delegation: Delegation to save
 *
 * Save a delegation to persistent storage.
 *
 * Returns: Result code
 */
GnDelegationResult gn_delegation_save(const gchar *delegator_npub,
                                       const GnDelegation *delegation);

/**
 * gn_delegation_delete:
 * @delegator_npub: npub of the delegator
 * @delegation_id: ID of the delegation to delete
 *
 * Permanently delete a delegation from storage.
 *
 * Returns: Result code
 */
GnDelegationResult gn_delegation_delete(const gchar *delegator_npub,
                                         const gchar *delegation_id);

/**
 * gn_delegation_load_all:
 * @delegator_npub: npub of the delegator
 *
 * Load all delegations from storage for a delegator.
 * This is called internally by gn_delegation_list().
 *
 * Returns: (transfer full): GPtrArray of GnDelegation*
 */
GPtrArray *gn_delegation_load_all(const gchar *delegator_npub);

/* ======== Utilities ======== */

/**
 * gn_delegation_result_to_string:
 * @result: Result code
 *
 * Get human-readable string for result code.
 *
 * Returns: Static string
 */
const gchar *gn_delegation_result_to_string(GnDelegationResult result);

/**
 * gn_delegation_get_storage_path:
 * @delegator_npub: npub of the delegator
 *
 * Get the path to the delegations JSON file for a delegator.
 *
 * Returns: (transfer full): File path
 */
gchar *gn_delegation_get_storage_path(const gchar *delegator_npub);

/**
 * gn_delegation_kind_name:
 * @kind: Nostr event kind number
 *
 * Get a human-readable name for common event kinds.
 *
 * Returns: Static string name
 */
const gchar *gn_delegation_kind_name(guint16 kind);

G_END_DECLS
