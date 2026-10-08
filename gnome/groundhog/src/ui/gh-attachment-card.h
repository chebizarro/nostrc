#ifndef GH_ATTACHMENT_CARD_H
#define GH_ATTACHMENT_CARD_H

#include <adwaita.h>

#include "gh-attachment-transfer.h"
#include "gh-message.h"

G_BEGIN_DECLS

/*
 * GhAttachmentCard (data/ui/gh-attachment-card.blp; privacy charter §6
 * "Receive", §7.4, §7.14, G22): a kind-15 file message in its bubble, in
 * place of the "Photo"/"File" text G21 showed. GTK-only.
 *
 * It says what the file is and who sent it: a type icon, "Photo", "Video",
 * "Audio" or "File (PDF)" from the declared type (never trusted to decode),
 * the size and "from <sender>". Lookup fetches nothing; in an accepted
 * conversation, the enabled media preference starts visible image and audio
 * downloads. Otherwise the Download button is the explicit request and says
 * which server it contacts and who learns what (the provider's
 * download_note). While downloading it shows a spinner and Cancel; a failure
 * is said in the transfer's words, with Try Again when that can help. Once
 * downloaded, a photo whose plaintext passed the decode guard
 * (GhAttachmentTransfer:previewable, gh_attachment_check_preview()) is shown
 * inline, decoded here with gdk_texture_new_from_bytes() (GTK's own PNG and
 * JPEG loaders, never gdk-pixbuf) and kept on the transfer. Downloaded audio
 * gets in-memory GtkMediaFile controls when GTK has a media backend; other
 * files stay cards. "Save As…" exports the plaintext only through the
 * provider's save, the Save dialog (the portal), with the note that saved
 * files aren't protected by Groundhog.
 *
 * The state lives in a GhAttachmentTransfer, not here: the conversation's
 * rows are recycled, so a card finds its transfer through the provider set
 * on an ancestor widget (the window, gh_attachment_card_set_provider()) and
 * asks it again at every action. Without a provider (a build without
 * attachments, or before the account's storage opens) the card says that
 * the file can't be downloaded here, and offers nothing.
 *
 * Keyboard: every button is focusable (Tab), with its label and a
 * description. The card is a group whose accessible label says the file,
 * its sender and its state ("Photo, 2.1 MB, from You. Not downloaded.");
 * a download that finishes or fails is announced politely.
 *
 * An encrypted group message (W25, nostrc-q3a6) shows one card per file
 * ("index"), described by what the MLS layer read from its imeta tags
 * (gh_message_get_attachment()), with the same opt-in automatic download,
 * image decode guard, audio playback and Save As.
 *
 * Actions (widget actions): attachment.download, attachment.cancel and
 * attachment.save. Properties: "message" (a kind-15 GhMessage or an MLS
 * message with files, or NULL), "index", "compact" (a smaller preview),
 * "summary" (read-only, the accessible text).
 */

typedef struct {
  /* The transfer of message's file (borrowed), or NULL; fetches nothing. */
  GhAttachmentTransfer *(*lookup)(GhMessage *message, gpointer data);
  void (*download)(GhAttachmentTransfer *transfer, gpointer data);
  void (*cancel)(GhAttachmentTransfer *transfer, gpointer data);
  /* Save As…: the Save dialog, then the plaintext written there. */
  void (*save)(GhAttachmentTransfer *transfer, GtkWidget *card, gpointer data);
  /* Nullable: what Download contacts and who learns what (transfer full). */
  gchar *(*download_note)(GhAttachmentTransfer *transfer, gpointer data);
  /* Nullable (W25): the transfer of file index of a message with several
   * (an encrypted group's); NULL: lookup() for index 0 only. */
  GhAttachmentTransfer *(*lookup_at)(GhMessage *message, guint index, gpointer data);
  /* Called for a mapped card; the provider decides whether to auto-download. */
  void (*auto_download)(GtkWidget *card, GhMessage *message, guint index,
                        GhAttachmentTransfer *transfer, gpointer data);
} GhAttachmentCardProvider;

/* Every card inside widget (e.g. the window) uses provider (copied) with
 * data; NULL removes it. destroy (nullable) frees data when replaced or
 * when widget goes. */
void gh_attachment_card_set_provider(GtkWidget *widget, const GhAttachmentCardProvider *provider,
                                     gpointer data, GDestroyNotify destroy);

#define GH_TYPE_ATTACHMENT_CARD (gh_attachment_card_get_type())
G_DECLARE_FINAL_TYPE(GhAttachmentCard, gh_attachment_card, GH, ATTACHMENT_CARD, GtkWidget)

GtkWidget *gh_attachment_card_new(void);
void gh_attachment_card_set_message(GhAttachmentCard *self, GhMessage *message);
GhMessage *gh_attachment_card_get_message(GhAttachmentCard *self);
void gh_attachment_card_set_compact(GhAttachmentCard *self, gboolean compact);
/* Which of the message's files (W25: an encrypted group message may carry
 * several, gh_message_get_attachment()); 0 for a kind-15 message. */
void gh_attachment_card_set_index(GhAttachmentCard *self, guint index);
guint gh_attachment_card_get_index(GhAttachmentCard *self);
/* The transfer the card shows (borrowed), or NULL. */
GhAttachmentTransfer *gh_attachment_card_get_transfer(GhAttachmentCard *self);
/* Re-check auto-download when the media preference changes. */
void gh_attachment_card_maybe_auto_download(GhAttachmentCard *self);
const gchar *gh_attachment_card_get_summary(GhAttachmentCard *self);

/* "Photo", "Video", "Audio", "File (PDF)" or "File" for a declared MIME
 * type (translated; transfer full). */
gchar *gh_attachment_card_describe_type(const gchar *mime);
/* A symbolic icon name for a declared MIME type. */
const gchar *gh_attachment_card_type_icon(const gchar *mime);
/* The file name Save As suggests for a declared MIME type and the
 * plaintext's magic bytes (plaintext nullable): "photo.jpg", "file.pdf"... */
gchar *gh_attachment_card_suggest_name(const gchar *mime, GBytes *plaintext);
/* The extension the plaintext's magic bytes say (jpg, png, gif, webp, pdf,
 * zip), or NULL. */
const gchar *gh_attachment_card_sniff_extension(GBytes *plaintext);
/* Save As for a file whose sender chose its name (W25 review L4;
 * sender_name already sanitized, nullable): the sender's stem with the
 * extension of the bytes (sniffed), else of the declared type, else the
 * sender's own only if it is a document, media or archive extension: never
 * .desktop, a script, an executable or a launcher. Without an extension of
 * these, the stem keeps no dot ("holiday.desktop.bin" is "holiday_desktop");
 * a leading dot never survives (no hidden file). */
gchar *gh_attachment_card_safe_save_name(const gchar *sender_name, const gchar *mime,
                                         GBytes *plaintext);

G_END_DECLS
#endif
