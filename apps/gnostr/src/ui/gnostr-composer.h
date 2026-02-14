#ifndef NOSTR_GTK_COMPOSER_H
#define NOSTR_GTK_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_COMPOSER (nostr_gtk_composer_get_type())

G_DECLARE_FINAL_TYPE(NostrGtkComposer, nostr_gtk_composer, NOSTR_GTK, COMPOSER, GtkWidget)

GtkWidget *nostr_gtk_composer_new(void);

/* Clear the composer text */
void nostr_gtk_composer_clear(NostrGtkComposer *self);

/* Reply context for NIP-10 threading */
void nostr_gtk_composer_set_reply_context(NostrGtkComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name);
void nostr_gtk_composer_clear_reply_context(NostrGtkComposer *self);
gboolean nostr_gtk_composer_is_reply(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_reply_to_id(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_root_id(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_reply_to_pubkey(NostrGtkComposer *self);

/* Quote context for NIP-18 quote posts (kind 1 with q-tag) */
void nostr_gtk_composer_set_quote_context(NostrGtkComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name);
void nostr_gtk_composer_clear_quote_context(NostrGtkComposer *self);
gboolean nostr_gtk_composer_is_quote(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_quote_id(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_quote_pubkey(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_quote_nostr_uri(NostrGtkComposer *self);

/* Media upload state (Blossom) */
gboolean nostr_gtk_composer_is_uploading(NostrGtkComposer *self);
void nostr_gtk_composer_cancel_upload(NostrGtkComposer *self);

/* Media metadata for NIP-92 imeta tags */
typedef struct {
  char *url;        /* Uploaded file URL */
  char *sha256;     /* SHA-256 hash (hex) */
  char *mime_type;  /* MIME type */
  gint64 size;      /* File size in bytes */
} NostrGtkComposerMedia;

/**
 * Get the list of uploaded media for this composer session.
 * Returns a NULL-terminated array of NostrGtkComposerMedia pointers.
 * The array and its contents are owned by the composer; do not free.
 */
NostrGtkComposerMedia **nostr_gtk_composer_get_uploaded_media(NostrGtkComposer *self);

/**
 * Get the count of uploaded media items.
 */
gsize nostr_gtk_composer_get_uploaded_media_count(NostrGtkComposer *self);

/**
 * Clear all uploaded media metadata.
 * Called when composer is cleared after successful post.
 */
void nostr_gtk_composer_clear_uploaded_media(NostrGtkComposer *self);

/* NIP-14: Subject tag support */
/**
 * Get the current subject text from the composer.
 * Returns the subject text or NULL if empty.
 * The returned string is owned by the entry widget; do not free.
 */
const char *nostr_gtk_composer_get_subject(NostrGtkComposer *self);

/* NIP-40: Expiration timestamp support */
/**
 * Set expiration timestamp for the next post.
 * @param expiration_secs: Unix timestamp when the note should expire, or 0 for no expiration.
 */
void nostr_gtk_composer_set_expiration(NostrGtkComposer *self, gint64 expiration_secs);

/**
 * Get the currently set expiration timestamp.
 * Returns 0 if no expiration is set.
 */
gint64 nostr_gtk_composer_get_expiration(NostrGtkComposer *self);

/**
 * Clear the expiration setting.
 */
void nostr_gtk_composer_clear_expiration(NostrGtkComposer *self);

/**
 * Check if an expiration is set.
 */
gboolean nostr_gtk_composer_has_expiration(NostrGtkComposer *self);

/* NIP-36: Content warning / sensitive content support */
/**
 * Check if the note is marked as sensitive (content-warning).
 */
gboolean nostr_gtk_composer_is_sensitive(NostrGtkComposer *self);

/**
 * Set whether the note should be marked as sensitive.
 */
void nostr_gtk_composer_set_sensitive(NostrGtkComposer *self, gboolean sensitive);

/* NIP-22: Comment context for kind 1111 events */
/**
 * Set comment context for creating a NIP-22 comment (kind 1111).
 * Comments can be on any event type, not just kind 1.
 * @param root_id: The event ID being commented on (hex)
 * @param root_kind: The kind of the root event (e.g., 1 for text note)
 * @param root_pubkey: The pubkey of the root event author (hex)
 * @param display_name: Display name of root event author for UI indicator
 */
void nostr_gtk_composer_set_comment_context(NostrGtkComposer *self,
                                         const char *root_id,
                                         int root_kind,
                                         const char *root_pubkey,
                                         const char *display_name);

/**
 * Clear the comment context.
 */
void nostr_gtk_composer_clear_comment_context(NostrGtkComposer *self);

/**
 * Check if the composer is in comment mode (NIP-22).
 */
gboolean nostr_gtk_composer_is_comment(NostrGtkComposer *self);

/**
 * Get the comment root event ID.
 */
const char *nostr_gtk_composer_get_comment_root_id(NostrGtkComposer *self);

/**
 * Get the comment root event kind.
 */
int nostr_gtk_composer_get_comment_root_kind(NostrGtkComposer *self);

/**
 * Get the comment root event pubkey.
 */
const char *nostr_gtk_composer_get_comment_root_pubkey(NostrGtkComposer *self);

/* NIP-37: Draft support */

/* Forward declaration for GnostrDraft (defined in gnostr-drafts.h) */
typedef struct _GnostrDraft GnostrDraft;

/**
 * Load a draft into the composer.
 * Clears any existing content and restores the draft state.
 * @param draft: The draft to load (will be copied, caller retains ownership)
 */
void nostr_gtk_composer_load_draft(NostrGtkComposer *self, const GnostrDraft *draft);

/**
 * Get the d-tag of the currently loaded draft (for updates).
 * Returns NULL if no draft is loaded.
 */
const char *nostr_gtk_composer_get_current_draft_d_tag(NostrGtkComposer *self);

/**
 * Clear the draft context (called after publishing to clean up).
 */
void nostr_gtk_composer_clear_draft_context(NostrGtkComposer *self);

/**
 * Check if a draft is currently loaded in the composer.
 */
gboolean nostr_gtk_composer_has_draft_loaded(NostrGtkComposer *self);

/**
 * Get the current text content from the composer.
 * Returns a newly allocated string that must be freed with g_free().
 */
char *nostr_gtk_composer_get_text(NostrGtkComposer *self);

G_END_DECLS

#endif /* NOSTR_GTK_COMPOSER_H */
