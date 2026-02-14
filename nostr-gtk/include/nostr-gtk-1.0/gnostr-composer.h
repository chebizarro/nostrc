/* gnostr-composer.h — Nostr event composition widget (library version)
 *
 * A GTK4 widget for composing Nostr text notes with support for:
 * NIP-10 (threading), NIP-14 (subject), NIP-18 (quotes), NIP-22 (comments),
 * NIP-36 (content warning), NIP-37 (drafts UI), NIP-40 (expiration),
 * NIP-92 (media tags).
 *
 * App-specific services (signing, media upload, draft persistence, toast
 * notifications) are decoupled via GObject signals. The caller connects to
 * these signals to provide the actual service implementations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

/* Media upload state */
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
const char *nostr_gtk_composer_get_subject(NostrGtkComposer *self);

/* NIP-40: Expiration timestamp support */
void nostr_gtk_composer_set_expiration(NostrGtkComposer *self, gint64 expiration_secs);
gint64 nostr_gtk_composer_get_expiration(NostrGtkComposer *self);
void nostr_gtk_composer_clear_expiration(NostrGtkComposer *self);
gboolean nostr_gtk_composer_has_expiration(NostrGtkComposer *self);

/* NIP-36: Content warning / sensitive content support */
gboolean nostr_gtk_composer_is_sensitive(NostrGtkComposer *self);
void nostr_gtk_composer_set_sensitive(NostrGtkComposer *self, gboolean sensitive);

/* NIP-22: Comment context for kind 1111 events */
void nostr_gtk_composer_set_comment_context(NostrGtkComposer *self,
                                         const char *root_id,
                                         int root_kind,
                                         const char *root_pubkey,
                                         const char *display_name);
void nostr_gtk_composer_clear_comment_context(NostrGtkComposer *self);
gboolean nostr_gtk_composer_is_comment(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_comment_root_id(NostrGtkComposer *self);
int nostr_gtk_composer_get_comment_root_kind(NostrGtkComposer *self);
const char *nostr_gtk_composer_get_comment_root_pubkey(NostrGtkComposer *self);

/* ---- Media upload (decoupled via signals) ---- */

/**
 * nostr_gtk_composer_upload_complete:
 * @self: the composer
 * @url: the URL of the uploaded file
 * @sha256: (nullable): SHA-256 hash of the file (hex string)
 * @mime_type: (nullable): MIME type of the file
 * @size: file size in bytes
 *
 * Called by the upload-requested signal handler when the upload succeeds.
 * Inserts the URL into the text and stores media metadata for NIP-92.
 */
void nostr_gtk_composer_upload_complete(NostrGtkComposer *self,
                                     const char *url,
                                     const char *sha256,
                                     const char *mime_type,
                                     gint64 size);

/**
 * nostr_gtk_composer_upload_failed:
 * @self: the composer
 * @message: error message to display
 *
 * Called by the upload-requested signal handler when the upload fails.
 * Hides the upload progress and emits a toast-requested signal.
 */
void nostr_gtk_composer_upload_failed(NostrGtkComposer *self,
                                   const char *message);

/* ---- NIP-37: Drafts (decoupled via signals) ---- */

/**
 * NostrGtkComposerDraftInfo:
 *
 * Lightweight struct for populating the drafts list and loading drafts.
 * Strings are borrowed (not owned); the caller must keep them alive for
 * the duration of the call.
 */
typedef struct {
  const char *d_tag;
  const char *content;
  const char *subject;
  const char *reply_to_id;
  const char *root_id;
  const char *reply_to_pubkey;
  const char *quote_id;
  const char *quote_pubkey;
  const char *quote_nostr_uri;
  gboolean is_sensitive;
  int target_kind;
  gint64 updated_at;
} NostrGtkComposerDraftInfo;

/**
 * nostr_gtk_composer_load_draft:
 * @self: the composer
 * @info: draft info to load into the composer
 *
 * Load a draft into the composer. Clears existing content and restores
 * the draft state. The info struct is borrowed; the caller retains ownership.
 */
void nostr_gtk_composer_load_draft(NostrGtkComposer *self,
                                const NostrGtkComposerDraftInfo *info);

const char *nostr_gtk_composer_get_current_draft_d_tag(NostrGtkComposer *self);
void nostr_gtk_composer_clear_draft_context(NostrGtkComposer *self);
gboolean nostr_gtk_composer_has_draft_loaded(NostrGtkComposer *self);

/**
 * nostr_gtk_composer_add_draft_row:
 * @self: the composer
 * @d_tag: draft d-tag identifier
 * @preview_text: short preview of draft content
 * @updated_at: Unix timestamp of last update
 *
 * Add a draft entry to the drafts popover list.
 * Called by the load-drafts-requested signal handler.
 */
void nostr_gtk_composer_add_draft_row(NostrGtkComposer *self,
                                   const char *d_tag,
                                   const char *preview_text,
                                   gint64 updated_at);

/**
 * nostr_gtk_composer_clear_draft_rows:
 * @self: the composer
 *
 * Clear all draft entries from the drafts popover list.
 */
void nostr_gtk_composer_clear_draft_rows(NostrGtkComposer *self);

/**
 * nostr_gtk_composer_draft_save_complete:
 * @self: the composer
 * @success: whether the save succeeded
 * @error_message: (nullable): error message on failure
 * @d_tag: (nullable): the d-tag assigned by the service (for updates)
 *
 * Called by the save-draft-requested signal handler when done.
 */
void nostr_gtk_composer_draft_save_complete(NostrGtkComposer *self,
                                         gboolean success,
                                         const char *error_message,
                                         const char *d_tag);

/**
 * nostr_gtk_composer_draft_delete_complete:
 * @self: the composer
 * @d_tag: the d-tag that was deleted
 * @success: whether the delete succeeded
 *
 * Called by the draft-delete-requested signal handler when done.
 */
void nostr_gtk_composer_draft_delete_complete(NostrGtkComposer *self,
                                           const char *d_tag,
                                           gboolean success);

/**
 * Get the current text content from the composer.
 * Returns a newly allocated string that must be freed with g_free().
 */
char *nostr_gtk_composer_get_text(NostrGtkComposer *self);

/*
 * Signals (connect from the application to provide services):
 *
 * "post-requested"        (const char *text)
 *    Emitted when the user clicks Post. The handler should build the
 *    unsigned event and sign it.
 *
 * "toast-requested"       (const char *message)
 *    Emitted when the composer needs to show a notification.
 *
 * "upload-requested"      (const char *file_path)
 *    Emitted when the user selects a file to upload. The handler should
 *    perform the upload and call nostr_gtk_composer_upload_complete() or
 *    nostr_gtk_composer_upload_failed().
 *
 * "save-draft-requested"  (NostrGtkComposer *self)
 *    Emitted when the user clicks Save Draft. The handler should read
 *    the composer state and persist the draft, then call
 *    nostr_gtk_composer_draft_save_complete().
 *
 * "load-drafts-requested" (NostrGtkComposer *self)
 *    Emitted when the drafts popover opens. The handler should call
 *    nostr_gtk_composer_clear_draft_rows() then nostr_gtk_composer_add_draft_row()
 *    for each available draft.
 *
 * "draft-load-requested"  (const char *d_tag)
 *    Emitted when the user clicks Load on a draft row. The handler should
 *    find the draft and call nostr_gtk_composer_load_draft().
 *
 * "draft-delete-requested" (const char *d_tag)
 *    Emitted when the user clicks Delete on a draft row. The handler should
 *    delete the draft and call nostr_gtk_composer_draft_delete_complete().
 *
 * "draft-saved"           (void)
 *    Emitted after a draft is successfully saved.
 *
 * "draft-loaded"          (void)
 *    Emitted after a draft is loaded into the composer.
 *
 * "draft-deleted"         (void)
 *    Emitted after a draft is deleted.
 */

G_END_DECLS

#endif /* NOSTR_GTK_COMPOSER_H */
