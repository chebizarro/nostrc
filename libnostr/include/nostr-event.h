#ifndef __NOSTR_EVENT_H__
#define __NOSTR_EVENT_H__

/* Transitional header: exposes GLib-friendly libnostr naming for events.
 * Avoids including internal headers; wrapper source bridges to internals. */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-tag.h"
#include "secure_buf.h"

/* Opaque to GI; implemented in src wrappers */
/**
 * NostrEvent:
 *
 * Opaque event record; registered as a GBoxed type via `nostr_event_get_type()`.
 */
// Define the NostrEvent structure
/* Event objects are mutable and are not thread-safe. Callers must synchronize
 * both mutation and lifetime; the declared id field is untrusted until validated. */
typedef struct _NostrEvent {
    char *id;
    char *pubkey;
    int64_t created_at;
    int kind;
    NostrTags *tags;
    char *content;
    char *sig;
    void *extra; // Extra fields
} NostrEvent;

typedef enum {
    NOSTR_EVENT_VALIDATION_OK = 0,
    NOSTR_EVENT_VALIDATION_NULL,
    NOSTR_EVENT_VALIDATION_MISSING_FIELD,
    NOSTR_EVENT_VALIDATION_BAD_ID,
    NOSTR_EVENT_VALIDATION_BAD_PUBKEY,
    NOSTR_EVENT_VALIDATION_BAD_SIGNATURE_FORMAT,
    NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH,
    NOSTR_EVENT_VALIDATION_SIGNATURE_INVALID,
    NOSTR_EVENT_VALIDATION_LIMIT,
    NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR,
    NOSTR_EVENT_VALIDATION_CRYPTO_ERROR
} NostrEventValidationStatus;

#ifdef __cplusplus
extern "C" {
#endif

/* Types: opaque NostrEvent handle */

/* Constructors and core ops */
/**
 * nostr_event_new:
 *
 * Returns: (transfer full) (nullable): new event
 */
NostrEvent *nostr_event_new(void);

/**
 * nostr_event_free:
 * @event: (transfer full) (nullable): event to free
 */
void nostr_event_free(NostrEvent *event);

/**
 * nostr_event_copy:
 * @event: (nullable): event to copy
 *
 * Returns: (transfer full) (nullable): deep copy of @event
 */
NostrEvent *nostr_event_copy(const NostrEvent *event);

#ifdef __GI_SCANNER__
#include <glib-object.h>
GType nostr_event_get_type(void);
#define NOSTR_TYPE_EVENT (nostr_event_get_type())
#endif

/**
 * nostr_event_compute_id:
 * @event: (nullable): event
 * @canonical_id_out: (out) (array fixed-size=65): lowercase canonical event id
 *
 * Computes the NIP-01 SHA-256 id without consulting @event->id.
 */
NostrEventValidationStatus nostr_event_compute_id(const NostrEvent *event,
                                                   char canonical_id_out[65]);

/**
 * nostr_event_validate_id:
 * @event: (nullable): event
 * @canonical_id_out: (out) (array fixed-size=65): lowercase canonical event id
 *
 * Computes the canonical id and requires the declared id to encode the same hash.
 */
NostrEventValidationStatus nostr_event_validate_id(const NostrEvent *event,
                                                    char canonical_id_out[65]);

/**
 * nostr_event_validate:
 * @event: (nullable): event
 * @canonical_id_out: (out) (array fixed-size=65) (nullable): canonical event id
 *
 * Strictly validates required signed fields, binds the declared id to the
 * canonical NIP-01 hash, and verifies the Schnorr signature against that hash.
 */
NostrEventValidationStatus nostr_event_validate(const NostrEvent *event,
                                                 char canonical_id_out[65]);

const char *nostr_event_validation_status_string(NostrEventValidationStatus status);

/**
 * nostr_event_get_id:
 * @event: (nullable): event
 *
 * Recomputes the canonical NIP-01 id. It never trusts or caches @event->id.
 * Returns: (transfer full) (nullable): newly allocated lowercase hex id
 */
char *nostr_event_get_id(NostrEvent *event);

/**
 * nostr_event_check_signature:
 * @event: (nullable): event
 *
 * Returns: whether the declared id is canonical and the signature verifies
 */
bool nostr_event_check_signature(NostrEvent *event);

/**
 * nostr_event_sign:
 * @event: (nullable): event
 * @private_key: (nullable): hex private key
 *
 * Returns: 0 on success
 */
int nostr_event_sign(NostrEvent *event, const char *private_key);

/**
 * nostr_event_sign_secure:
 * @event: (nullable): event
 * @sk: (not nullable): pointer to a 32-byte private key stored in nostr_secure_buf
 *
 * Signs the event using the provided secret key stored in secure memory.
 * Returns: 0 on success
 */
int nostr_event_sign_secure(NostrEvent *event, const nostr_secure_buf *sk);

/**
 * nostr_event_is_regular:
 * @event: (nullable): event
 *
 * Returns: whether event is regular
 */
bool nostr_event_is_regular(NostrEvent *event);

/* Accessors for public struct members (for GObject properties later) */
/**
 * nostr_event_get_pubkey:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable): internal pubkey string
 */
const char *nostr_event_get_pubkey(const NostrEvent *event);
/**
 * nostr_event_set_pubkey:
 * @event: (nullable): event (no-op if NULL)
 * @pubkey: (nullable): hex pubkey; duplicated internally
 */
void nostr_event_set_pubkey(NostrEvent *event, const char *pubkey);

/**
 * nostr_event_get_created_at:
 * @event: (nullable): event
 *
 * Returns: created_at timestamp
 */
int64_t nostr_event_get_created_at(const NostrEvent *event);
/**
 * nostr_event_set_created_at:
 * @event: (nullable): event
 * @created_at: timestamp
 */
void nostr_event_set_created_at(NostrEvent *event, int64_t created_at);

/**
 * nostr_event_get_kind:
 * @event: (nullable): event
 *
 * Returns: kind integer
 */
int nostr_event_get_kind(const NostrEvent *event);
/**
 * nostr_event_set_kind:
 * @event: (nullable): event
 * @kind: kind integer
 */
void nostr_event_set_kind(NostrEvent *event, int kind);

/**
 * nostr_event_get_tags:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable) (type gpointer): owned tags pointer
 */
void *nostr_event_get_tags(const NostrEvent *event);
/**
 * nostr_event_set_tags:
 * @event: (nullable): event (no-op if NULL)
 * @tags: (transfer full) (nullable) (type gpointer): new tags; previous freed if different
 *
 * Ownership: takes full ownership of @tags.
 */
void nostr_event_set_tags(NostrEvent *event, void *tags);

/**
 * nostr_event_get_content:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable)
 */
const char *nostr_event_get_content(const NostrEvent *event);
/**
 * nostr_event_set_content:
 * @event: (nullable): event (no-op if NULL)
 * @content: (nullable): utf8; duplicated internally
 */
void nostr_event_set_content(NostrEvent *event, const char *content);

/**
 * nostr_event_get_sig:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable)
 */
const char *nostr_event_get_sig(const NostrEvent *event);
/**
 * nostr_event_set_sig:
 * @event: (nullable): event (no-op if NULL)
 * @sig: (nullable): hex signature; duplicated internally
 */
void nostr_event_set_sig(NostrEvent *event, const char *sig);

/* No further remapping here to prevent recursive macro definitions. */

/* Fast-path JSON serialization for hot paths (avoids backend/jansson).
 * Returns a newly-allocated compact JSON object string representing the event.
 * Only includes fields that are set (id, pubkey, created_at, kind, tags, content, sig).
 */
char *nostr_event_serialize_compact(const NostrEvent *event);

/* Fast-path JSON deserialization from a compact object string.
 * Returns 1 on success, 0 on parse error. Populates provided @event.
 * If @err_out is non-NULL, receives structured error info on failure.
 */
typedef struct NostrJsonErrorInfo NostrJsonErrorInfo;
int nostr_event_deserialize_compact(NostrEvent *event, const char *json,
                                     NostrJsonErrorInfo *err_out);

/* Strict structural parser for signed event JSON. Unlike the permissive
 * compact parser above, this requires id, pubkey, created_at, kind, tags,
 * content, and sig to be present. Cryptographic validation remains a separate
 * nostr_event_validate() step so envelope parsing and admission stay distinct. */
NostrEventValidationStatus nostr_event_deserialize_signed(
    NostrEvent *event, const char *json, NostrJsonErrorInfo *err_out);

/* Strict structural parser for unsigned NIP-01-shaped events such as NIP-17
 * rumors. Requires pubkey, created_at, kind, tags, and content; permits an id
 * but rejects sig. If id is present, validate it separately with
 * nostr_event_validate_id(). */
NostrEventValidationStatus nostr_event_deserialize_unsigned(
    NostrEvent *event, const char *json, NostrJsonErrorInfo *err_out);

/* ========================================================================
 * Event Priority Classification (nostrc-7u2)
 * ======================================================================== */

/**
 * NostrEventPriority:
 * Priority levels for backpressure decisions.
 */
typedef enum {
    NOSTR_EVENT_PRIORITY_CRITICAL = 0,  /**< DMs, zaps, mentions - never dropped */
    NOSTR_EVENT_PRIORITY_HIGH     = 1,  /**< Replies to own posts */
    NOSTR_EVENT_PRIORITY_NORMAL   = 2,  /**< Timeline events */
    NOSTR_EVENT_PRIORITY_LOW      = 3   /**< Reactions, reposts - dropped first */
} NostrEventPriority;

/**
 * nostr_event_get_priority:
 * @event: event to classify
 * @user_pubkey: (nullable): current user's pubkey for mention detection
 *
 * Classifies an event's priority for backpressure decisions.
 * Classification rules:
 * - CRITICAL: DMs (kind 4, 1059), zaps (kind 9735), mentions of user
 * - HIGH: Replies (kind 1 with "e" tag)
 * - LOW: Reactions (kind 7), reposts (kind 6)
 * - NORMAL: Everything else
 *
 * Returns: priority level
 */
NostrEventPriority nostr_event_get_priority(const NostrEvent *event, const char *user_pubkey);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_H__ */
