/**
 * gnostr NIP-59 Gift Wrap Utility
 *
 * NIP-59 defines gift-wrapped events (kind 1059) for private communication.
 * This module provides async wrapping and unwrapping functions that integrate
 * with the D-Bus signer interface for NIP-44 encryption.
 *
 * Gift Wrap Structure:
 * - Outer event (kind 1059): Signed with ephemeral key, randomized timestamp
 * - Encrypted content: NIP-44 encrypted seal event
 * - Seal (kind 13): Signed by real sender, contains encrypted rumor
 * - Rumor: Unsigned inner event (the actual message content)
 *
 * NIP-59 is used by:
 * - NIP-17: Private Direct Messages (kind 14 rumors)
 * - Any application requiring metadata-protected event delivery
 */

#ifndef GNOSTR_NIP59_GIFTWRAP_H
#define GNOSTR_NIP59_GIFTWRAP_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-event.h"

G_BEGIN_DECLS

/* Event kinds */
#define NIP59_KIND_SEAL      13
#define NIP59_KIND_GIFT_WRAP 1059

/* Randomization window for gift wrap timestamp (2 days in seconds) */
#define NIP59_TIMESTAMP_WINDOW (2 * 24 * 60 * 60)

/**
 * GnostrGiftWrapResult:
 *
 * Result structure from async gift wrap operations.
 */
typedef struct {
    gboolean success;           /* TRUE if operation succeeded */
    char *gift_wrap_json;       /* Gift wrap event JSON (caller frees) */
    char *error_message;        /* Error message if failed (caller frees) */
} GnostrGiftWrapResult;

/**
 * GnostrUnwrapResult:
 *
 * Result structure from async unwrap operations.
 */
typedef struct {
    gboolean success;           /* TRUE if operation succeeded */
    NostrEvent *rumor;          /* Decrypted rumor event (caller frees) */
    char *sender_pubkey;        /* Real sender pubkey from seal (caller frees) */
    char *error_message;        /* Error message if failed (caller frees) */
} GnostrUnwrapResult;

/**
 * gnostr_gift_wrap_result_free:
 * @result: result to free
 *
 * Frees a gift wrap result and its contents.
 */
void gnostr_gift_wrap_result_free(GnostrGiftWrapResult *result);

/**
 * gnostr_unwrap_result_free:
 * @result: result to free
 *
 * Frees an unwrap result and its contents.
 */
void gnostr_unwrap_result_free(GnostrUnwrapResult *result);

/**
 * GnostrGiftWrapCallback:
 * @result: (transfer full): the operation result
 * @user_data: user data passed to the async function
 *
 * Callback for async gift wrap creation.
 */
typedef void (*GnostrGiftWrapCallback)(GnostrGiftWrapResult *result,
                                        gpointer user_data);

/**
 * GnostrUnwrapCallback:
 * @result: (transfer full): the operation result
 * @user_data: user data passed to the async function
 *
 * Callback for async unwrap operations.
 */
typedef void (*GnostrUnwrapCallback)(GnostrUnwrapResult *result,
                                      gpointer user_data);

/**
 * gnostr_nip59_create_gift_wrap_async:
 * @rumor: (transfer none): unsigned rumor event to wrap
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @sender_pubkey_hex: sender's public key (64 hex chars)
 * @cancellable: (nullable): a GCancellable
 * @callback: callback when complete
 * @user_data: user data for callback
 *
 * Creates a complete NIP-59 gift-wrapped event asynchronously.
 *
 * Flow:
 * 1. Creates a seal (kind 13) containing NIP-44 encrypted rumor JSON
 * 2. Signs the seal via D-Bus signer
 * 3. Generates ephemeral keypair for gift wrap
 * 4. Creates gift wrap (kind 1059) with NIP-44 encrypted seal
 * 5. Signs gift wrap with ephemeral key
 *
 * The rumor should be an unsigned event (kind 14 for DMs, or other kinds).
 * The gift wrap uses a randomized timestamp for metadata protection.
 */
void gnostr_nip59_create_gift_wrap_async(NostrEvent *rumor,
                                          const char *recipient_pubkey_hex,
                                          const char *sender_pubkey_hex,
                                          GCancellable *cancellable,
                                          GnostrGiftWrapCallback callback,
                                          gpointer user_data);

/**
 * gnostr_nip59_unwrap_async:
 * @gift_wrap: (transfer none): gift wrap event (kind 1059)
 * @user_pubkey_hex: current user's public key (64 hex chars)
 * @cancellable: (nullable): a GCancellable
 * @callback: callback when complete
 * @user_data: user data for callback
 *
 * Unwraps a gift wrap event asynchronously using D-Bus NIP-44 decryption.
 *
 * Flow:
 * 1. Validates gift wrap structure and signature
 * 2. Decrypts gift wrap content to get seal (via D-Bus NIP-44)
 * 3. Validates seal signature and structure
 * 4. Decrypts seal content to get rumor (via D-Bus NIP-44)
 * 5. Validates seal pubkey == rumor pubkey (anti-spoofing)
 *
 * On success, result contains the decrypted rumor and real sender pubkey.
 */
void gnostr_nip59_unwrap_async(NostrEvent *gift_wrap,
                                const char *user_pubkey_hex,
                                GCancellable *cancellable,
                                GnostrUnwrapCallback callback,
                                gpointer user_data);

/**
 * gnostr_nip59_create_rumor:
 * @kind: event kind for the rumor (e.g., 14 for DM)
 * @sender_pubkey_hex: sender's public key (64 hex chars)
 * @content: message content (UTF-8)
 * @tags: (transfer none) (nullable): optional tags to include
 *
 * Creates an unsigned rumor event for gift wrapping.
 * The rumor is NOT signed - it will be wrapped in a seal.
 *
 * Returns: (transfer full) (nullable): new rumor event
 */
NostrEvent *gnostr_nip59_create_rumor(int kind,
                                       const char *sender_pubkey_hex,
                                       const char *content,
                                       NostrTags *tags);

/**
 * gnostr_nip59_create_dm_rumor:
 * @sender_pubkey_hex: sender's public key (64 hex chars)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @content: message content (UTF-8)
 *
 * Creates an unsigned kind 14 rumor event for a direct message.
 * Convenience wrapper for NIP-17 private DMs.
 *
 * Returns: (transfer full) (nullable): new rumor event
 */
NostrEvent *gnostr_nip59_create_dm_rumor(const char *sender_pubkey_hex,
                                          const char *recipient_pubkey_hex,
                                          const char *content);

/**
 * gnostr_nip59_validate_gift_wrap:
 * @gift_wrap: (transfer none): gift wrap event
 *
 * Validates gift wrap structure:
 * - Kind is 1059
 * - Signature is valid
 * - Has p-tag for recipient
 * - Has content (encrypted seal)
 *
 * Returns: TRUE if valid
 */
gboolean gnostr_nip59_validate_gift_wrap(NostrEvent *gift_wrap);

/**
 * gnostr_nip59_get_randomized_timestamp:
 *
 * Gets a randomized timestamp for gift wrap creation.
 * Randomizes within NIP59_TIMESTAMP_WINDOW to protect metadata.
 *
 * Returns: Unix timestamp (seconds since epoch)
 */
gint64 gnostr_nip59_get_randomized_timestamp(void);

/**
 * gnostr_nip59_get_recipient_from_gift_wrap:
 * @gift_wrap: (transfer none): gift wrap event
 *
 * Extracts the recipient pubkey from a gift wrap's p-tag.
 *
 * Returns: (transfer full) (nullable): recipient pubkey hex string
 */
char *gnostr_nip59_get_recipient_from_gift_wrap(NostrEvent *gift_wrap);

G_END_DECLS

#endif /* GNOSTR_NIP59_GIFTWRAP_H */
