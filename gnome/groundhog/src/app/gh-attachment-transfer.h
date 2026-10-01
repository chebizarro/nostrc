#ifndef GH_ATTACHMENT_TRANSFER_H
#define GH_ATTACHMENT_TRANSFER_H

#include <gio/gio.h>

#include "gh-nip17-file.h"

G_BEGIN_DECLS

/*
 * GhAttachmentTransfer: what one kind-15 file message's attachment card
 * shows (privacy charter §6 "Receive", G22), GTK-free. A conversation's rows
 * are recycled as it scrolls, so the state of a file lives here, with the
 * account's attachment service (src/media/gh-attachments.h), which alone
 * changes it; every card that shows the message binds to the same object.
 *
 * Nothing here fetches anything: a transfer starts IDLE and moves only when
 * the user chooses Download (PD-2, AT-7). The plaintext of a READY transfer
 * is held in memory that is wiped when freed (gh-attachment.h), never in a
 * file. "previewable" says the plaintext passed the decode guard
 * (gh_attachment_check_preview(): PNG or JPEG by magic bytes, dimensions
 * within the limit, AT-4); only then may a UI decode it, and "preview" keeps
 * what it decoded (e.g. a GdkTexture) with the plaintext, so a recycled row
 * does not decode it again. Both go when the transfer is reset.
 *
 * Properties (read-only, notified on change): "state" (GhAttachmentState),
 * "error" (FAILED: the sentence to show), "can-retry", "previewable",
 * "preview" (settable) and "from-cache". Main context only.
 */

typedef enum {
  GH_ATTACHMENT_STATE_IDLE,        /* nothing fetched: the card offers Download */
  GH_ATTACHMENT_STATE_DOWNLOADING, /* the user asked; Cancel is possible */
  GH_ATTACHMENT_STATE_READY,       /* the decrypted file is here */
  GH_ATTACHMENT_STATE_FAILED       /* "error" says why */
} GhAttachmentState;

GType gh_attachment_state_get_type(void);
#define GH_TYPE_ATTACHMENT_STATE (gh_attachment_state_get_type())

#define GH_TYPE_ATTACHMENT_TRANSFER (gh_attachment_transfer_get_type())
G_DECLARE_FINAL_TYPE(GhAttachmentTransfer, gh_attachment_transfer, GH, ATTACHMENT_TRANSFER,
                     GObject)

/* The transfer of the file (copied) that the message rumor_id carries. */
GhAttachmentTransfer *gh_attachment_transfer_new(const gchar *rumor_id, const GhNip17File *file);

/* The transfer of an encrypted group's file (W25, nostrc-q3a6): key names
 * it for its service (the message and the file's index), media_type is the
 * declared type and sender_filename the name the sender chose (any UTF-8:
 * only gh_attachment_transfer_sanitize_name() of it is ever suggested). */
GhAttachmentTransfer *gh_attachment_transfer_new_described(const gchar *key,
                                                           const gchar *media_type,
                                                           const gchar *sender_filename);

/* The key it was made with (a kind-15 file: its message's rumor id). */
const gchar *gh_attachment_transfer_get_rumor_id(GhAttachmentTransfer *self);
/* The kind-15 file, or NULL for an encrypted group's file. */
const GhNip17File *gh_attachment_transfer_get_file(GhAttachmentTransfer *self);
/* The declared MIME type (either kind; never trusted to decode). */
const gchar *gh_attachment_transfer_get_media_type(GhAttachmentTransfer *self);
/* Save As's suggestion from the sender's name (sanitized), or NULL. */
const gchar *gh_attachment_transfer_get_suggested_name(GhAttachmentTransfer *self);
/* A file name safe to suggest in the Save dialog from one a sender chose:
 * '/', '\\', ':', control and invisible or bidirectional-override
 * characters become '_', leading dots and spaces and trailing ones go (no
 * hidden file, no "..") and it is cut to 200 bytes keeping a short
 * extension. NULL when nothing usable is left (transfer full). */
gchar *gh_attachment_transfer_sanitize_name(const gchar *name);
GhAttachmentState gh_attachment_transfer_get_state(GhAttachmentTransfer *self);
/* FAILED: what went wrong, in words for the user; NULL otherwise. */
const gchar *gh_attachment_transfer_get_error(GhAttachmentTransfer *self);
/* FAILED: whether trying again can help (FALSE for a damaged file). */
gboolean gh_attachment_transfer_get_can_retry(GhAttachmentTransfer *self);
/* READY: the decrypted file (borrowed); NULL otherwise. */
GBytes *gh_attachment_transfer_get_plaintext(GhAttachmentTransfer *self);
/* READY and the plaintext passed the decode guard. */
gboolean gh_attachment_transfer_get_previewable(GhAttachmentTransfer *self);
/* READY: it came from the account's encrypted cache, not the network. */
gboolean gh_attachment_transfer_get_from_cache(GhAttachmentTransfer *self);

/* The decoded image a UI made from a previewable plaintext (a reference is
 * kept until the transfer is reset); NULL clears it. Ignored unless
 * previewable. */
void gh_attachment_transfer_set_preview(GhAttachmentTransfer *self, GObject *preview);
GObject *gh_attachment_transfer_get_preview(GhAttachmentTransfer *self);

/* ---- for the attachment service only ------------------------------------------ */

/* IDLE or FAILED -> DOWNLOADING. */
void gh_attachment_transfer_start(GhAttachmentTransfer *self);
/* -> READY with plaintext (a reference is kept). */
void gh_attachment_transfer_succeed(GhAttachmentTransfer *self, GBytes *plaintext,
                                    gboolean previewable, gboolean from_cache);
/* -> FAILED with error (the user's words). */
void gh_attachment_transfer_fail(GhAttachmentTransfer *self, const gchar *error,
                                 gboolean can_retry);
/* -> IDLE: plaintext, preview and error dropped (cancelled, or let go). */
void gh_attachment_transfer_reset(GhAttachmentTransfer *self);

G_END_DECLS
#endif
