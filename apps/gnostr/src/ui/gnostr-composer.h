#ifndef GNOSTR_COMPOSER_H
#define GNOSTR_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMPOSER (gnostr_composer_get_type())

G_DECLARE_FINAL_TYPE(GnostrComposer, gnostr_composer, GNOSTR, COMPOSER, GtkWidget)

GtkWidget *gnostr_composer_new(void);

/* Clear the composer text */
void gnostr_composer_clear(GnostrComposer *self);

/* Reply context for NIP-10 threading */
void gnostr_composer_set_reply_context(GnostrComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name);
void gnostr_composer_clear_reply_context(GnostrComposer *self);
gboolean gnostr_composer_is_reply(GnostrComposer *self);
const char *gnostr_composer_get_reply_to_id(GnostrComposer *self);
const char *gnostr_composer_get_root_id(GnostrComposer *self);
const char *gnostr_composer_get_reply_to_pubkey(GnostrComposer *self);

/* Quote context for NIP-18 quote posts (kind 1 with q-tag) */
void gnostr_composer_set_quote_context(GnostrComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name);
void gnostr_composer_clear_quote_context(GnostrComposer *self);
gboolean gnostr_composer_is_quote(GnostrComposer *self);
const char *gnostr_composer_get_quote_id(GnostrComposer *self);
const char *gnostr_composer_get_quote_pubkey(GnostrComposer *self);
const char *gnostr_composer_get_quote_nostr_uri(GnostrComposer *self);

/* Media upload state (Blossom) */
gboolean gnostr_composer_is_uploading(GnostrComposer *self);
void gnostr_composer_cancel_upload(GnostrComposer *self);

/* Media metadata for NIP-92 imeta tags */
typedef struct {
  char *url;        /* Uploaded file URL */
  char *sha256;     /* SHA-256 hash (hex) */
  char *mime_type;  /* MIME type */
  gint64 size;      /* File size in bytes */
} GnostrComposerMedia;

/**
 * Get the list of uploaded media for this composer session.
 * Returns a NULL-terminated array of GnostrComposerMedia pointers.
 * The array and its contents are owned by the composer; do not free.
 */
GnostrComposerMedia **gnostr_composer_get_uploaded_media(GnostrComposer *self);

/**
 * Get the count of uploaded media items.
 */
gsize gnostr_composer_get_uploaded_media_count(GnostrComposer *self);

/**
 * Clear all uploaded media metadata.
 * Called when composer is cleared after successful post.
 */
void gnostr_composer_clear_uploaded_media(GnostrComposer *self);

/* NIP-14: Subject tag support */
/**
 * Get the current subject text from the composer.
 * Returns the subject text or NULL if empty.
 * The returned string is owned by the entry widget; do not free.
 */
const char *gnostr_composer_get_subject(GnostrComposer *self);

/* NIP-40: Expiration timestamp support */
/**
 * Set expiration timestamp for the next post.
 * @param expiration_secs: Unix timestamp when the note should expire, or 0 for no expiration.
 */
void gnostr_composer_set_expiration(GnostrComposer *self, gint64 expiration_secs);

/**
 * Get the currently set expiration timestamp.
 * Returns 0 if no expiration is set.
 */
gint64 gnostr_composer_get_expiration(GnostrComposer *self);

/**
 * Clear the expiration setting.
 */
void gnostr_composer_clear_expiration(GnostrComposer *self);

/**
 * Check if an expiration is set.
 */
gboolean gnostr_composer_has_expiration(GnostrComposer *self);

/* NIP-36: Content warning / sensitive content support */
/**
 * Check if the note is marked as sensitive (content-warning).
 */
gboolean gnostr_composer_is_sensitive(GnostrComposer *self);

/**
 * Set whether the note should be marked as sensitive.
 */
void gnostr_composer_set_sensitive(GnostrComposer *self, gboolean sensitive);

/* NIP-22: Comment context for kind 1111 events */
/**
 * Set comment context for creating a NIP-22 comment (kind 1111).
 * Comments can be on any event type, not just kind 1.
 * @param root_id: The event ID being commented on (hex)
 * @param root_kind: The kind of the root event (e.g., 1 for text note)
 * @param root_pubkey: The pubkey of the root event author (hex)
 * @param display_name: Display name of root event author for UI indicator
 */
void gnostr_composer_set_comment_context(GnostrComposer *self,
                                         const char *root_id,
                                         int root_kind,
                                         const char *root_pubkey,
                                         const char *display_name);

/**
 * Clear the comment context.
 */
void gnostr_composer_clear_comment_context(GnostrComposer *self);

/**
 * Check if the composer is in comment mode (NIP-22).
 */
gboolean gnostr_composer_is_comment(GnostrComposer *self);

/**
 * Get the comment root event ID.
 */
const char *gnostr_composer_get_comment_root_id(GnostrComposer *self);

/**
 * Get the comment root event kind.
 */
int gnostr_composer_get_comment_root_kind(GnostrComposer *self);

/**
 * Get the comment root event pubkey.
 */
const char *gnostr_composer_get_comment_root_pubkey(GnostrComposer *self);

/* NIP-37: Draft support */

/* Forward declaration for GnostrDraft (defined in gnostr-drafts.h) */
typedef struct _GnostrDraft GnostrDraft;

/**
 * Load a draft into the composer.
 * Clears any existing content and restores the draft state.
 * @param draft: The draft to load (will be copied, caller retains ownership)
 */
void gnostr_composer_load_draft(GnostrComposer *self, const GnostrDraft *draft);

/**
 * Get the d-tag of the currently loaded draft (for updates).
 * Returns NULL if no draft is loaded.
 */
const char *gnostr_composer_get_current_draft_d_tag(GnostrComposer *self);

/**
 * Clear the draft context (called after publishing to clean up).
 */
void gnostr_composer_clear_draft_context(GnostrComposer *self);

/**
 * Check if a draft is currently loaded in the composer.
 */
gboolean gnostr_composer_has_draft_loaded(GnostrComposer *self);

/**
 * Get the current text content from the composer.
 * Returns a newly allocated string that must be freed with g_free().
 */
char *gnostr_composer_get_text(GnostrComposer *self);

G_END_DECLS

#endif /* GNOSTR_COMPOSER_H */
