#ifndef GNOSTR_EVENT_H
#define GNOSTR_EVENT_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

/* Define GNostrEvent GObject wrapper */
/* Prefixed with G to avoid clashing with core NostrEvent struct */
#define GNOSTR_TYPE_EVENT (gnostr_event_get_type())
G_DECLARE_FINAL_TYPE(GNostrEvent, gnostr_event, GNOSTR, EVENT, GObject)

/**
 * GNostrEvent:
 *
 * A GObject wrapper for Nostr events implementing NIP-01.
 * Provides property notifications and signals for signing/verification.
 */

/* Signal indices */
enum {
    GNOSTR_EVENT_SIGNAL_SIGNED,
    GNOSTR_EVENT_SIGNAL_VERIFIED,
    GNOSTR_EVENT_SIGNALS_COUNT
};

/**
 * gnostr_event_new:
 *
 * Creates a new empty GNostrEvent.
 *
 * Returns: (transfer full): a new #GNostrEvent
 */
GNostrEvent *gnostr_event_new(void);

/**
 * gnostr_event_new_from_json:
 * @json: a JSON string representing a Nostr event
 * @error: (nullable): return location for a #GError
 *
 * Creates a new GNostrEvent from a JSON string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrEvent, or %NULL on error
 */
GNostrEvent *gnostr_event_new_from_json(const gchar *json, GError **error);

/**
 * gnostr_event_to_json:
 * @self: a #GNostrEvent
 *
 * Serializes the event to a JSON string.
 *
 * Returns: (transfer full): a newly allocated JSON string
 */
gchar *gnostr_event_to_json(GNostrEvent *self);

/**
 * gnostr_event_sign:
 * @self: a #GNostrEvent
 * @privkey: hex-encoded private key (64 characters)
 * @error: (nullable): return location for a #GError
 *
 * Signs the event with the provided private key.
 * Sets the id, pubkey, and sig fields.
 * Emits the "signed" signal on success.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_event_sign(GNostrEvent *self, const gchar *privkey, GError **error);

/**
 * gnostr_event_verify:
 * @self: a #GNostrEvent
 * @error: (nullable): return location for a #GError
 *
 * Verifies the event signature.
 * Emits the "verified" signal on success.
 *
 * Returns: %TRUE if signature is valid, %FALSE otherwise
 */
gboolean gnostr_event_verify(GNostrEvent *self, GError **error);

/* Property accessors */

/**
 * gnostr_event_get_id:
 * @self: a #GNostrEvent
 *
 * Gets the event ID (read-only after signing).
 *
 * Returns: (transfer none) (nullable): the event ID
 */
const gchar *gnostr_event_get_id(GNostrEvent *self);

/**
 * gnostr_event_get_pubkey:
 * @self: a #GNostrEvent
 *
 * Gets the public key (read-only).
 *
 * Returns: (transfer none) (nullable): the public key
 */
const gchar *gnostr_event_get_pubkey(GNostrEvent *self);

/**
 * gnostr_event_get_created_at:
 * @self: a #GNostrEvent
 *
 * Gets the creation timestamp.
 *
 * Returns: Unix timestamp
 */
gint64 gnostr_event_get_created_at(GNostrEvent *self);

/**
 * gnostr_event_set_created_at:
 * @self: a #GNostrEvent
 * @created_at: Unix timestamp
 *
 * Sets the creation timestamp.
 */
void gnostr_event_set_created_at(GNostrEvent *self, gint64 created_at);

/**
 * gnostr_event_get_kind:
 * @self: a #GNostrEvent
 *
 * Gets the event kind.
 *
 * Returns: the event kind
 */
guint gnostr_event_get_kind(GNostrEvent *self);

/**
 * gnostr_event_set_kind:
 * @self: a #GNostrEvent
 * @kind: the event kind
 *
 * Sets the event kind.
 */
void gnostr_event_set_kind(GNostrEvent *self, guint kind);

/**
 * gnostr_event_get_content:
 * @self: a #GNostrEvent
 *
 * Gets the event content.
 *
 * Returns: (transfer none) (nullable): the content string
 */
const gchar *gnostr_event_get_content(GNostrEvent *self);

/**
 * gnostr_event_set_content:
 * @self: a #GNostrEvent
 * @content: (nullable): the content string
 *
 * Sets the event content.
 */
void gnostr_event_set_content(GNostrEvent *self, const gchar *content);

/**
 * gnostr_event_get_sig:
 * @self: a #GNostrEvent
 *
 * Gets the signature (read-only after signing).
 *
 * Returns: (transfer none) (nullable): the signature
 */
const gchar *gnostr_event_get_sig(GNostrEvent *self);

/**
 * gnostr_event_get_tags:
 * @self: a #GNostrEvent
 *
 * Gets the event tags as a pointer to the internal NostrTags.
 * Use with core libnostr tag manipulation functions.
 *
 * Returns: (transfer none) (nullable) (type gpointer): internal tags pointer
 */
gpointer gnostr_event_get_tags(GNostrEvent *self);

/**
 * gnostr_event_set_tags:
 * @self: a #GNostrEvent
 * @tags: (transfer full) (nullable) (type gpointer): new tags; previous freed if different
 *
 * Sets the event tags. Takes ownership of the provided tags.
 */
void gnostr_event_set_tags(GNostrEvent *self, gpointer tags);

/* Note: G_DECLARE_FINAL_TYPE already defines autoptr cleanup for GObject subclasses */

G_END_DECLS

#endif /* GNOSTR_EVENT_H */
