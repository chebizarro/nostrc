#ifndef GH_ATTACHMENT_UI_H
#define GH_ATTACHMENT_UI_H

#include <adwaita.h>

#include "gh-account-store.h"
#include "gh-attachment-sheet.h"
#include "gh-attachments.h"
#include "gh-window.h"

G_BEGIN_DECLS

typedef struct {
  GhAccountStore *account_store;      /* required: its outbox sends the message */
  GhConversationStore *conversations; /* required: the model the window lists */
  GhAttachments *attachments;         /* required: the account's attachment service */
  GSettings *settings;                /* nullable: blossom-servers, network-mode */
  gboolean allow_onion;               /* the build has Tor: a .onion server may be chosen */
} GhAttachmentUiConfig;

/*
 * Attachments in the window (privacy charter §6, D6, §7.7, §7.14, G22):
 *
 *  - Sending. The composer's attach button opens GtkFileDialog (the portal);
 *    a dropped file or a pasted image takes the same path. A file over the
 *    size limit (GH_BLOSSOM_MAX_FILE_SIZE, D6) is refused before it is read.
 *    The file is prepared in memory (JPEG/PNG metadata removed; a damaged
 *    JPEG or PNG is refused rather than sent with it) and shown in the
 *    GhAttachmentSheet: name, size, the photo (after the decode guard),
 *    "Location and camera data removed" or that this kind of file may carry
 *    hidden details, what the server learns in the current network mode, and
 *    that the encrypted file outlives a disappearing message. Nothing leaves
 *    before Send.
 *  - The first use without a server asks for one (the sheet's "servers"
 *    page); Groundhog never picks a server by itself, and the address the
 *    user types follows Preferences' rules (gh_preferences_server_list_add()).
 *  - Send uploads through GhAttachments (a throwaway upload key); a server
 *    that wants a known account gets the account's signature only after the
 *    user's consent for that server (kept per account, nostrc-dnsc), and
 *    then the upload is tried again. The uploaded file goes to the shown
 *    conversation through the account's durable outbox
 *    (gh_outbox_send_file_room(): one person or a NIP-17 room of up to 10),
 *    and the send UI shows it like any message; the sender's own card shows
 *    the file at once from the encrypted cache. Closing the sheet cancels
 *    the upload; errors are said on the sheet. The account changing closes
 *    it. Relay groups (NIP-29) get no attach button: their files would be
 *    public (charter §6 scope).
 *  - Any file offered: only a native file not under a GVfs FUSE mount
 *    (gh_attachment_path_on_remote_mount()) or on a gvfs filesystem; nothing
 *    of anything else is queried or read.
 *  - Received files: every GhAttachmentCard in the window gets its transfer
 *    from GhAttachments (gh_attachment_card_set_provider()): Download,
 *    Cancel, and Save As… through the Save dialog, which writes the
 *    plaintext only where the user chose, then offers to open it. Nothing
 *    is fetched otherwise (AT-7).
 * Everything is released with the window.
 */
void gh_attachment_ui_attach(GhWindow *window, const GhAttachmentUiConfig *config);

/* An encrypted group's files (W25, nostrc-q3a6), installed by
 * gh-mls-attachment-ui.c: the same attach button, sheet (server choice,
 * preview, metadata notice, consent), size limit, files on this device only,
 * cards with Download on request, and Save As as a NIP-17 file; what differs
 * (sealing for the group's epoch, the kind-9 imeta, opening with the epoch)
 * is the delegate's. Every function is required but download_note and
 * watch. */
typedef struct {
  /* Whether files can be sent in conversation (an encrypted group) now. */
  gboolean (*can_send)(GhConversation *conversation, gpointer data);
  /* Seal, upload and send file (metadata already removed) named name. */
  void (*send_async)(GhConversation *conversation, GBytes *file, const gchar *name,
                     const gchar *mime, GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer user_data, gpointer data);
  /* out_server: as gh_attachments_upload_finish(). */
  gboolean (*send_finish)(GAsyncResult *result, gchar **out_server, GError **error,
                          gpointer data);
  /* The words for a send error (host nullable). */
  gchar *(*describe)(const GError *error, const gchar *host, gpointer data);
  /* The cards of its messages (gh_message_is_mls()). */
  GhAttachmentTransfer *(*lookup)(GhMessage *message, guint index, gpointer data);
  void (*download)(GhAttachmentTransfer *transfer, gpointer data);
  void (*cancel)(GhAttachmentTransfer *transfer, gpointer data);
  gchar *(*download_note)(GhAttachmentTransfer *transfer, gpointer data);
  /* Optional: the object whose "notify" may change can_send for the shown
   * conversation (its group: left, removed, leaving, offline), or NULL
   * (transfer none). The attach button follows it while it is shown; the
   * handler is the window's (W25 re-review R2). */
  GObject *(*watch)(GhConversation *conversation, gpointer data);
} GhAttachmentUiGroups;

/* Installs (or with NULL removes) groups; destroy frees data then or with
 * the window. After gh_attachment_ui_attach(). */
void gh_attachment_ui_set_groups(GhWindow *window, const GhAttachmentUiGroups *groups,
                                 gpointer data, GDestroyNotify destroy);
/* The delegate's can_send may have changed (a group joined, left, its
 * service replaced): the attach button follows. */
void gh_attachment_ui_groups_changed(GhWindow *window);

/* The paths of a chosen, dropped or pasted file, for tests too: bytes (at
 * most the size limit) named name (shown only) of type mime (a hint). */
void gh_attachment_ui_offer_bytes(GhWindow *window, GBytes *bytes, const gchar *name,
                                  const gchar *mime);
void gh_attachment_ui_offer_file(GhWindow *window, GFile *file);
/* The sheet of the file being offered, or NULL. */
GhAttachmentSheet *gh_attachment_ui_get_sheet(GhWindow *window);

/* Tests only: instead of the Save dialog, where Save As… writes (transfer
 * full; NULL: cancelled). */
typedef GFile *(*GhAttachmentUiSaveTarget)(const gchar *suggested_name, gpointer data);
void gh_attachment_ui_set_save_target(GhWindow *window, GhAttachmentUiSaveTarget func,
                                      gpointer data);
/* The text of the last toast shown (NULL before one), for tests. */
const gchar *gh_attachment_ui_get_last_toast(GhWindow *window);

G_END_DECLS
#endif
