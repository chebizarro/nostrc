/**
 * GnostrDrafts - NIP-37 Draft Events Manager
 *
 * Manages draft events (kind 31234) for saving work-in-progress notes.
 * Drafts are stored locally and optionally published to relays.
 *
 * NIP-37 defines:
 * - kind 31234: Parameterized replaceable event (uses "d" tag)
 * - "k" tag: specifies target kind (e.g., "1" for notes)
 * - Content: NIP-44 encrypted draft event JSON
 * - Expiration: recommended 90 days
 */

#ifndef GNOSTR_DRAFTS_H
#define GNOSTR_DRAFTS_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_DRAFTS (gnostr_drafts_get_type())

G_DECLARE_FINAL_TYPE(GnostrDrafts, gnostr_drafts, GNOSTR, DRAFTS, GObject)

/**
 * Draft event kind per NIP-37
 */
#define GNOSTR_DRAFT_KIND 31234

/**
 * Default expiration: 90 days in seconds
 */
#define GNOSTR_DRAFT_DEFAULT_EXPIRATION_SECS (90 * 24 * 60 * 60)

/**
 * A single draft entry
 */
typedef struct _GnostrDraft {
  char *d_tag;           /* Unique identifier (d-tag) */
  int target_kind;       /* Kind of the draft (e.g., 1 for text note) */
  char *content;         /* Draft content (text) */
  char *subject;         /* Optional subject (NIP-14) */
  char *reply_to_id;     /* Reply context: event ID being replied to */
  char *root_id;         /* Reply context: thread root event ID */
  char *reply_to_pubkey; /* Reply context: pubkey of author being replied to */
  char *quote_id;        /* Quote context: event ID being quoted */
  char *quote_pubkey;    /* Quote context: pubkey of author being quoted */
  char *quote_nostr_uri; /* Quote context: nostr:note1... URI */
  gint64 created_at;     /* Unix timestamp when draft was created */
  gint64 updated_at;     /* Unix timestamp when draft was last updated */
  gboolean is_sensitive; /* NIP-36: content warning flag */
} GnostrDraft;

/**
 * Callback for draft operations
 */
typedef void (*GnostrDraftsCallback)(GnostrDrafts *drafts,
                                      gboolean success,
                                      const char *error_message,
                                      gpointer user_data);

/**
 * Callback for loading drafts
 */
typedef void (*GnostrDraftsLoadCallback)(GnostrDrafts *drafts,
                                          GPtrArray *draft_list, /* GnostrDraft* */
                                          GError *error,
                                          gpointer user_data);

/**
 * gnostr_drafts_new:
 *
 * Creates a new drafts manager instance.
 *
 * Returns: (transfer full): A new #GnostrDrafts
 */
GnostrDrafts *gnostr_drafts_new(void);

/**
 * gnostr_drafts_get_default:
 *
 * Gets the default (global) drafts manager instance.
 * Creates one if it doesn't exist.
 *
 * Returns: (transfer none): The default #GnostrDrafts
 */
GnostrDrafts *gnostr_drafts_get_default(void);

/**
 * gnostr_drafts_set_user_pubkey:
 * @self: The drafts manager
 * @pubkey_hex: The user's public key in hex format
 *
 * Sets the current user's public key for draft encryption.
 */
void gnostr_drafts_set_user_pubkey(GnostrDrafts *self, const char *pubkey_hex);

/**
 * gnostr_drafts_save_async:
 * @self: The drafts manager
 * @draft: The draft to save
 * @callback: Callback when save completes
 * @user_data: User data for callback
 *
 * Saves a draft locally and optionally publishes to relays.
 * Creates a kind 31234 event with NIP-44 encrypted content.
 */
void gnostr_drafts_save_async(GnostrDrafts *self,
                               GnostrDraft *draft,
                               GnostrDraftsCallback callback,
                               gpointer user_data);

/**
 * gnostr_drafts_load_local:
 * @self: The drafts manager
 *
 * Loads all drafts from local storage synchronously.
 *
 * Returns: (transfer full) (element-type GnostrDraft): Array of drafts
 */
GPtrArray *gnostr_drafts_load_local(GnostrDrafts *self);

/**
 * gnostr_drafts_load_from_relays_async:
 * @self: The drafts manager
 * @callback: Callback when load completes
 * @user_data: User data for callback
 *
 * Fetches drafts from relays (kind 31234 events for current user).
 */
void gnostr_drafts_load_from_relays_async(GnostrDrafts *self,
                                           GnostrDraftsLoadCallback callback,
                                           gpointer user_data);

/**
 * gnostr_drafts_delete_async:
 * @self: The drafts manager
 * @d_tag: The draft identifier to delete
 * @callback: Callback when delete completes
 * @user_data: User data for callback
 *
 * Deletes a draft by publishing a blanked event (empty content).
 */
void gnostr_drafts_delete_async(GnostrDrafts *self,
                                 const char *d_tag,
                                 GnostrDraftsCallback callback,
                                 gpointer user_data);

/**
 * gnostr_drafts_delete_local:
 * @self: The drafts manager
 * @d_tag: The draft identifier to delete
 *
 * Deletes a draft from local storage only.
 *
 * Returns: TRUE if deleted, FALSE if not found
 */
gboolean gnostr_drafts_delete_local(GnostrDrafts *self, const char *d_tag);

/**
 * gnostr_draft_new:
 *
 * Creates a new empty draft.
 *
 * Returns: (transfer full): A new #GnostrDraft
 */
GnostrDraft *gnostr_draft_new(void);

/**
 * gnostr_draft_free:
 * @draft: The draft to free
 *
 * Frees a draft and all its resources.
 */
void gnostr_draft_free(GnostrDraft *draft);

/**
 * gnostr_draft_copy:
 * @draft: The draft to copy
 *
 * Creates a deep copy of a draft.
 *
 * Returns: (transfer full): A copy of the draft
 */
GnostrDraft *gnostr_draft_copy(const GnostrDraft *draft);

/**
 * gnostr_draft_generate_d_tag:
 *
 * Generates a unique d-tag for a new draft.
 *
 * Returns: (transfer full): A new unique d-tag string
 */
char *gnostr_draft_generate_d_tag(void);

/**
 * gnostr_draft_to_json:
 * @draft: The draft
 *
 * Serializes a draft to JSON for NIP-44 encryption.
 *
 * Returns: (transfer full): JSON string representation
 */
char *gnostr_draft_to_json(const GnostrDraft *draft);

/**
 * gnostr_draft_from_json:
 * @json: The JSON string
 *
 * Deserializes a draft from JSON (after NIP-44 decryption).
 *
 * Returns: (transfer full) (nullable): The draft or NULL on error
 */
GnostrDraft *gnostr_draft_from_json(const char *json);

G_END_DECLS

#endif /* GNOSTR_DRAFTS_H */
