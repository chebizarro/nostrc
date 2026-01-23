/* key_rotation.h - Nostr key rotation and migration for gnostr-signer
 *
 * Implements key rotation functionality per NIP-41 (proposed) patterns:
 * - Generate new keypair while keeping old one accessible
 * - Create migration announcement event (kind 1776)
 * - Sign migration event with both old and new keys
 * - Update stored identity with new key
 * - Publish migration to relays
 *
 * Migration Event Structure (kind 1776):
 * {
 *   "kind": 1776,
 *   "pubkey": "<old_pubkey>",
 *   "created_at": <timestamp>,
 *   "tags": [
 *     ["p", "<new_pubkey>"],
 *     ["alt", "Key migration announcement"],
 *     ["new_sig", "<signature_from_new_key>"]  // optional but recommended
 *   ],
 *   "content": "Migrating to new key: <new_pubkey>",
 *   "sig": "<signature_from_old_key>"
 * }
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Result codes for key rotation operations */
typedef enum {
  KEY_ROTATION_OK = 0,
  KEY_ROTATION_ERR_NO_SOURCE_KEY,     /* Source key not found or inaccessible */
  KEY_ROTATION_ERR_GENERATE_FAILED,   /* Failed to generate new keypair */
  KEY_ROTATION_ERR_SIGN_FAILED,       /* Failed to sign migration event */
  KEY_ROTATION_ERR_STORE_FAILED,      /* Failed to store new key */
  KEY_ROTATION_ERR_PUBLISH_FAILED,    /* Failed to publish to relays */
  KEY_ROTATION_ERR_INVALID_PARAMS,    /* Invalid parameters provided */
  KEY_ROTATION_ERR_CANCELLED          /* Operation was cancelled by user */
} KeyRotationResult;

/* State of a rotation operation */
typedef enum {
  KEY_ROTATION_STATE_IDLE = 0,
  KEY_ROTATION_STATE_GENERATING,      /* Generating new keypair */
  KEY_ROTATION_STATE_CREATING_EVENT,  /* Creating migration event */
  KEY_ROTATION_STATE_SIGNING_OLD,     /* Signing with old key */
  KEY_ROTATION_STATE_SIGNING_NEW,     /* Creating new key attestation */
  KEY_ROTATION_STATE_STORING,         /* Storing new key in secure storage */
  KEY_ROTATION_STATE_PUBLISHING,      /* Publishing to relays */
  KEY_ROTATION_STATE_COMPLETE,        /* Rotation complete */
  KEY_ROTATION_STATE_ERROR            /* Error occurred */
} KeyRotationState;

/* Migration event kind per NIP-41 draft */
#define KEY_MIGRATION_EVENT_KIND 1776

/* Key rotation context - opaque handle */
typedef struct _KeyRotation KeyRotation;

/* Progress callback type */
typedef void (*KeyRotationProgressCb)(KeyRotation *kr,
                                       KeyRotationState state,
                                       const gchar *message,
                                       gpointer user_data);

/* Completion callback type */
typedef void (*KeyRotationCompleteCb)(KeyRotation *kr,
                                       KeyRotationResult result,
                                       const gchar *new_npub,
                                       const gchar *error_message,
                                       gpointer user_data);

/**
 * key_rotation_new:
 * @old_npub: The npub of the key being rotated from
 *
 * Create a new key rotation context.
 *
 * Returns: (transfer full): A new KeyRotation context, or NULL on error
 */
KeyRotation *key_rotation_new(const gchar *old_npub);

/**
 * key_rotation_free:
 * @kr: The key rotation context to free
 *
 * Free a key rotation context and all associated resources.
 * Safe to call with NULL.
 */
void key_rotation_free(KeyRotation *kr);

/**
 * key_rotation_set_new_label:
 * @kr: The key rotation context
 * @label: Label for the new key (can be NULL)
 *
 * Set the label for the newly generated key.
 * If not set, defaults to original key's label with " (rotated)" suffix.
 */
void key_rotation_set_new_label(KeyRotation *kr, const gchar *label);

/**
 * key_rotation_set_publish:
 * @kr: The key rotation context
 * @publish: Whether to publish migration event to relays
 *
 * Set whether to publish the migration event.
 * Default is TRUE.
 */
void key_rotation_set_publish(KeyRotation *kr, gboolean publish);

/**
 * key_rotation_set_keep_old:
 * @kr: The key rotation context
 * @keep: Whether to keep the old key accessible
 *
 * Set whether to keep the old key in secure storage.
 * Default is TRUE (recommended for recovery).
 */
void key_rotation_set_keep_old(KeyRotation *kr, gboolean keep);

/**
 * key_rotation_set_progress_callback:
 * @kr: The key rotation context
 * @callback: Progress callback function
 * @user_data: Data to pass to callback
 *
 * Set callback for progress updates during rotation.
 */
void key_rotation_set_progress_callback(KeyRotation *kr,
                                         KeyRotationProgressCb callback,
                                         gpointer user_data);

/**
 * key_rotation_set_complete_callback:
 * @kr: The key rotation context
 * @callback: Completion callback function
 * @user_data: Data to pass to callback
 *
 * Set callback for when rotation completes (success or failure).
 */
void key_rotation_set_complete_callback(KeyRotation *kr,
                                         KeyRotationCompleteCb callback,
                                         gpointer user_data);

/**
 * key_rotation_execute:
 * @kr: The key rotation context
 *
 * Start the key rotation process.
 * This is asynchronous - use callbacks to track progress and completion.
 *
 * Returns: TRUE if rotation started, FALSE on immediate error
 */
gboolean key_rotation_execute(KeyRotation *kr);

/**
 * key_rotation_cancel:
 * @kr: The key rotation context
 *
 * Cancel an in-progress rotation.
 * Any generated key will not be persisted.
 */
void key_rotation_cancel(KeyRotation *kr);

/**
 * key_rotation_get_state:
 * @kr: The key rotation context
 *
 * Get the current state of the rotation operation.
 *
 * Returns: Current state
 */
KeyRotationState key_rotation_get_state(KeyRotation *kr);

/**
 * key_rotation_get_old_npub:
 * @kr: The key rotation context
 *
 * Get the old (source) npub being rotated from.
 *
 * Returns: The old npub (owned by context, do not free)
 */
const gchar *key_rotation_get_old_npub(KeyRotation *kr);

/**
 * key_rotation_get_new_npub:
 * @kr: The key rotation context
 *
 * Get the new npub after rotation completes.
 *
 * Returns: The new npub, or NULL if rotation not complete
 */
const gchar *key_rotation_get_new_npub(KeyRotation *kr);

/**
 * key_rotation_get_migration_event:
 * @kr: The key rotation context
 *
 * Get the signed migration event JSON.
 *
 * Returns: Migration event JSON, or NULL if not yet created
 */
const gchar *key_rotation_get_migration_event(KeyRotation *kr);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/**
 * key_rotation_build_migration_event:
 * @old_pubkey_hex: Old public key in hex format
 * @new_pubkey_hex: New public key in hex format
 * @created_at: Event timestamp (or 0 for current time)
 * @content: Optional content message (NULL for default)
 *
 * Build an unsigned migration event JSON.
 * The event needs to be signed with the old key before publishing.
 *
 * Returns: (transfer full): Unsigned event JSON, caller frees
 */
gchar *key_rotation_build_migration_event(const gchar *old_pubkey_hex,
                                           const gchar *new_pubkey_hex,
                                           gint64 created_at,
                                           const gchar *content);

/**
 * key_rotation_verify_migration:
 * @event_json: Signed migration event JSON
 * @out_old_pubkey: Output old pubkey (caller frees)
 * @out_new_pubkey: Output new pubkey (caller frees)
 *
 * Verify a migration event is properly signed.
 *
 * Returns: TRUE if event is valid and properly signed
 */
gboolean key_rotation_verify_migration(const gchar *event_json,
                                        gchar **out_old_pubkey,
                                        gchar **out_new_pubkey);

/**
 * key_rotation_result_to_string:
 * @result: The result code
 *
 * Get a human-readable string for a result code.
 *
 * Returns: Static string describing the result
 */
const gchar *key_rotation_result_to_string(KeyRotationResult result);

/**
 * key_rotation_state_to_string:
 * @state: The state
 *
 * Get a human-readable string for a state.
 *
 * Returns: Static string describing the state
 */
const gchar *key_rotation_state_to_string(KeyRotationState state);

G_END_DECLS
