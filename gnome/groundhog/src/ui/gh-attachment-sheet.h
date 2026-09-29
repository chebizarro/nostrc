#ifndef GH_ATTACHMENT_SHEET_H
#define GH_ATTACHMENT_SHEET_H

#include <adwaita.h>

G_BEGIN_DECLS

/*
 * GhAttachmentSheet (data/ui/gh-attachment-sheet.blp; privacy charter §6
 * "Send", D6, §7.12, G22): the dialog between choosing a file and sending
 * it. GTK-only; its owner (gh-attachment-ui.c) decides what it shows:
 *
 *  - "servers": no attachment server is chosen yet, so the user types the
 *    address of one (Groundhog never picks a default); "server-chosen"
 *    (text) asks the owner to check it, which answers with
 *    gh_attachment_sheet_show_preview() or show_servers() with the reason;
 *  - "preview": the file's name, size and kind, the photo (a paintable the
 *    owner decoded after the decode guard), whether its hidden details
 *    (location, camera) were removed or can't be, what the server learns,
 *    and the disappearing timer's note; Send emits "send";
 *  - "sending": a spinner and the owner's text; Cancel emits "cancel";
 *  - "consent": the server accepts uploads only from accounts it knows;
 *    "Upload as My Account" emits "consent", "Don't Upload" "cancel".
 * Closing the dialog is always possible and cancels whatever runs (the
 * owner listens to AdwDialog::closed). Errors appear on the page they
 * concern and are announced. Keyboard: Enter uses the page's main button;
 * each page focuses its first control.
 *
 * Actions (widget actions): sheet.use-server, sheet.send, sheet.consent and
 * sheet.cancel.
 */
#define GH_TYPE_ATTACHMENT_SHEET (gh_attachment_sheet_get_type())
G_DECLARE_FINAL_TYPE(GhAttachmentSheet, gh_attachment_sheet, GH, ATTACHMENT_SHEET, AdwDialog)

GhAttachmentSheet *gh_attachment_sheet_new(void);

/* What will be sent: name (as chosen; shown, never sent), size in bytes, its
 * kind ("Photo", "File (PDF)") and the photo (nullable). */
void gh_attachment_sheet_set_file(GhAttachmentSheet *self, const gchar *name, guint64 size,
                                  const gchar *kind, GdkPaintable *thumbnail);
/* TRUE: JPEG/PNG metadata was removed; FALSE: the kind of file whose hidden
 * details Groundhog can't remove. */
void gh_attachment_sheet_set_metadata_removed(GhAttachmentSheet *self, gboolean removed);
/* What the server learns (required) and the disappearing timer's note
 * (nullable: none). */
void gh_attachment_sheet_set_notes(GhAttachmentSheet *self, const gchar *server_note,
                                   const gchar *timer_note);

void gh_attachment_sheet_show_servers(GhAttachmentSheet *self, const gchar *error);
void gh_attachment_sheet_show_preview(GhAttachmentSheet *self, const gchar *error);
void gh_attachment_sheet_show_sending(GhAttachmentSheet *self, const gchar *text);
/* host: the server that asked for a known account. */
void gh_attachment_sheet_show_consent(GhAttachmentSheet *self, const gchar *host);

/* The page shown ("servers", "preview", "sending" or "consent"). */
const gchar *gh_attachment_sheet_get_page(GhAttachmentSheet *self);
/* The error shown on the current page, or NULL. */
const gchar *gh_attachment_sheet_get_error(GhAttachmentSheet *self);
/* The metadata row's text, the server and timer notes (for tests). */
const gchar *gh_attachment_sheet_get_metadata_text(GhAttachmentSheet *self);
const gchar *gh_attachment_sheet_get_server_note(GhAttachmentSheet *self);
/* The address field of the "servers" page. */
AdwEntryRow *gh_attachment_sheet_get_server_entry(GhAttachmentSheet *self);
/* Whether a photo is shown on the preview page. */
gboolean gh_attachment_sheet_get_has_thumbnail(GhAttachmentSheet *self);

G_END_DECLS
#endif
