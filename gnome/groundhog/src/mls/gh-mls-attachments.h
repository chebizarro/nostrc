#ifndef GH_MLS_ATTACHMENTS_H
#define GH_MLS_ATTACHMENTS_H

#include "gh-attachments.h"
#include "gh-mls-media.h"
#include "gh-mls-service.h"

G_BEGIN_DECLS

/*
 * GhMlsAttachments: an encrypted group's files and picture as the window uses
 * them (W25; nostrc-q3a6, nostrc-m6tp), GTK-free, over gh-mls-media.h (MIP-04
 * encrypted-media-v2 through libmarmot, Blossom through GhBlossomClient and
 * so GhNetHttp: the network mode, Tor, public hosts only). It borrows the
 * account's GhAttachments for its Blossom client (servers, upload consent),
 * its encrypted store (the cache) and its error words, and follows the open
 * store's GhMlsService (gh_mls_attachments_set_service()); either changing
 * cancels every transfer. Main context only (libmarmot's thread).
 *
 * The same rules as a NIP-17 file (privacy charter §6, PD-2, AT-7):
 *
 *  - Received files. gh_mls_attachments_lookup() gives the
 *    GhAttachmentTransfer of file `index` of an MLS message (its
 *    gh_message_get_attachment()), IDLE the first time: nothing is fetched by
 *    looking. There is no auto-download preference in Groundhog, so only
 *    gh_mls_attachments_download(), the user's Download, fetches: the
 *    encrypted cache first (this very file: its group, source epoch,
 *    hashes, nonce, type and name), else the file's own blossom-v1 locators,
 *    then the group's verified 0x800b media servers, each body checked
 *    against the ciphertext hash before the AEAD, opened with the epoch
 *    libmarmot authenticated for the message (never a tag). Failures are
 *    plain words; a file whose epoch key is gone, or a message stored before
 *    its epoch was kept, says it can't be opened, and never invalidates the
 *    message. A plaintext that passes the decode guard is "previewable";
 *    held plaintext is bounded like GhAttachments' (GH_ATTACHMENTS_HELD_MAX).
 *  - Sent files. gh_mls_attachments_send_async(): a file read on this device
 *    only (the caller: gh_mls_media_read_file_async() or the sheet's
 *    bytes), JPEG/PNG metadata removed, encrypted for the group's current
 *    epoch, uploaded with GhAttachments' client (its servers and consents),
 *    then sent as one kind-9 inner event with its ordered v2 imeta. A group's
 *    verified 0x800b Blossom endpoints, when present, override the account's
 *    server choice; no policy keeps the account or sheet choice. If a
 *    Commit moved the group during the upload, it is sealed and uploaded
 *    again (at most GH_MLS_ATTACHMENTS_SEND_TRIES times). The name sent is
 *    neutral ("photo.jpg", "file.pdf"): a camera's IMG_20260930_142233.jpg
 *    or a document's own name would tell every member, and the server
 *    nothing, more than the file. The sender's card shows the file from the
 *    cache at once.
 *  - The group picture (0x8002, adopted groups). Its state is read from the
 *    group's components; a URL avatar (0x8007) wins and is never fetched
 *    (GH_FEATURE_REMOTE_IMAGES is 0; an unverified one never at all): a
 *    placeholder. A Blossom picture is fetched only on the user's request
 *    from the group's media servers (0x8002 names none), checked against its
 *    hash, opened, and kept in the encrypted store (gh_store_group_image_*)
 *    for that very state. An admin sets one (a JPEG or PNG on this device,
 *    metadata removed, encrypted, uploaded to the group's media servers
 *    whose host is public -- the rule for every URL someone else names, W25
 *    review M1 -- signed by the picture's own upload key, then one Commit)
 *    or removes it; the window confirms both first, naming every server.
 *    Groundhog has no picture support for a legacy group:
 *    GH_MLS_PICTURE_UNSUPPORTED.
 */

#define GH_MLS_ATTACHMENTS_SEND_TRIES 3

#define GH_TYPE_MLS_ATTACHMENTS (gh_mls_attachments_get_type())
G_DECLARE_FINAL_TYPE(GhMlsAttachments, gh_mls_attachments, GH, MLS_ATTACHMENTS, GObject)

/* attachments: the account's (a reference is kept). */
GhMlsAttachments *gh_mls_attachments_new(GhAttachments *attachments);
/* The open store's service (borrowed until the next call), or NULL. */
void gh_mls_attachments_set_service(GhMlsAttachments *self, GhMlsService *service);
/* Or asked at every use (the application's: the service is made with the
 * store's outbox): a different one than last time is set as above. */
typedef GhMlsService *(*GhMlsAttachmentsServiceFunc)(gpointer data);
void gh_mls_attachments_set_service_func(GhMlsAttachments *self,
                                         GhMlsAttachmentsServiceFunc func, gpointer data);
GhMlsService *gh_mls_attachments_get_service(GhMlsAttachments *self);

/* ---- received files ------------------------------------------------------------- */

/* The transfer of file index of message (borrowed), or NULL when message
 * is not an MLS message of the service's groups with that file. */
GhAttachmentTransfer *gh_mls_attachments_lookup(GhMlsAttachments *self, GhMessage *message,
                                                guint index);
void gh_mls_attachments_download(GhMlsAttachments *self, GhAttachmentTransfer *transfer);
void gh_mls_attachments_cancel(GhMlsAttachments *self, GhAttachmentTransfer *transfer);
/* What Download contacts and who learns what (transfer full), or NULL. */
gchar *gh_mls_attachments_download_note(GhMlsAttachments *self, GhAttachmentTransfer *transfer);
/* Downloads started (the cache or the network), for tests. */
guint gh_mls_attachments_get_downloads_started(GhMlsAttachments *self);

/* ---- sent files ----------------------------------------------------------------- */

/* The neutral file name sent for a file of type mime chosen as name
 * (transfer full): "photo.jpg", "photo.png", else "file" with name's
 * extension when it is 1 to 8 ASCII letters or digits. */
gchar *gh_mls_attachments_neutral_name(const gchar *mime, const gchar *name);

/* Sends file (1 .. GH_BLOSSOM_MAX_FILE_SIZE bytes) in group with caption
 * (nullable). Finishes with the sent message, and in out_server (nullable)
 * the server used or, with GH_BLOSSOM_ERROR_AUTH_REQUIRED, the one that asked
 * for an account it knows. GH_MLS_SERVICE_ERROR_UNSUPPORTED when the group's
 * media policy forbids Blossom locators; GH_MLS_SERVICE_ERROR_EPOCH_CHANGED
 * after GH_MLS_ATTACHMENTS_SEND_TRIES. */
void gh_mls_attachments_send_async(GhMlsAttachments *self, GhMlsGroup *group, GBytes *file,
                                   const gchar *name, const gchar *mime_hint,
                                   const gchar *caption, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data);
/* The sheet's explicit server choice is kept for a group without 0x800b
 * policy, including an epoch-change retry. A policy's verified endpoints
 * override it. If a Commit changes that policy during an epoch retry, the
 * send stops instead of uploading to a newly introduced endpoint. NULL uses
 * the client's configured list when no policy exists. */
void gh_mls_attachments_send_on_servers_async(GhMlsAttachments *self, GhMlsGroup *group,
                                              GBytes *file, const gchar *name,
                                              const gchar *mime_hint, const gchar *caption,
                                              const gchar *const *servers,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback, gpointer user_data);
GhMessage *gh_mls_attachments_send_finish(GhMlsAttachments *self, GAsyncResult *result,
                                          gchar **out_server, GError **error);

/* ---- the group picture ------------------------------------------------------------ */

typedef enum {
  GH_MLS_PICTURE_NONE,            /* no picture */
  GH_MLS_PICTURE_UNSUPPORTED,     /* a legacy group: no picture component */
  GH_MLS_PICTURE_WEB,             /* a URL avatar: never fetched here, a placeholder */
  GH_MLS_PICTURE_WEB_UNVERIFIED,  /* a URL avatar libmarmot can't verify: a placeholder */
  GH_MLS_PICTURE_AVAILABLE,       /* an encrypted picture: Show Picture fetches it */
  GH_MLS_PICTURE_NO_SERVER,       /* an encrypted picture the group names no server for */
  GH_MLS_PICTURE_READY            /* the picture, from the encrypted store */
} GhMlsPictureState;

/* The state of a group's picture source (marmot_group_avatar_select()'s
 * tri-state): a URL avatar wins over an encrypted picture, unverified or
 * not. Pure; never READY, NO_SERVER or UNSUPPORTED. */
GhMlsPictureState gh_mls_picture_state_for_source(MarmotGroupAvatarSource source);
/* Whether a picture in state may ever be loaded, given the remote-image
 * policy (GH_FEATURE_REMOTE_IMAGES and its setting): an encrypted picture on
 * the user's request; a verified URL avatar only under that policy; an
 * unverified one never. Everything else shows a placeholder. */
gboolean gh_mls_picture_may_load(GhMlsPictureState state, gboolean remote_images);

/* group's picture now: the state, and for READY its bytes (wiped when freed),
 * for AVAILABLE every host Show Picture may contact, in order (public hosts
 * only; with none, NO_SERVER). Reads the store, never the network; forgets a
 * stored picture the group no longer has. */
GhMlsPictureState gh_mls_attachments_get_picture(GhMlsAttachments *self, GhMlsGroup *group,
                                                 GBytes **out_picture, GStrv *out_hosts);
/* The hosts a new picture of group would be uploaded to, in order: the
 * group's verified media servers whose host is public (W25 review M1);
 * empty when there is none (set_picture then fails with NO_SERVER). For the
 * admin's confirmation (gh_mls_picture_upload_note()). */
GStrv gh_mls_attachments_dup_picture_upload_hosts(GhMlsAttachments *self, GhMlsGroup *group);
/* The user's Show Picture: fetch, check, open and keep it. */
void gh_mls_attachments_fetch_picture_async(GhMlsAttachments *self, GhMlsGroup *group,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
GBytes *gh_mls_attachments_fetch_picture_finish(GhMlsAttachments *self, GAsyncResult *result,
                                                GError **error);
/* An admin's new picture (file: a JPEG or PNG read on this device; NULL
 * removes the picture). confirmed_hosts (required with file): the hosts the
 * admin was shown (gh_mls_attachments_dup_picture_upload_hosts()); the
 * upload goes to exactly those, and when the group's are no longer the same
 * nothing is sealed or uploaded: GH_MLS_SERVICE_ERROR_SERVERS_CHANGED,
 * confirm again (W25 re-review R4). GH_BLOSSOM_ERROR_NO_SERVER when the
 * group names no media server to keep it on. Completes once the Commit is
 * merged. */
void gh_mls_attachments_set_picture_async(GhMlsAttachments *self, GhMlsGroup *group,
                                          GBytes *file, const gchar *mime_hint,
                                          const gchar *const *confirmed_hosts,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_mls_attachments_set_picture_finish(GhMlsAttachments *self, GAsyncResult *result,
                                               GError **error);

/* Copy (W25 review L3: every server that may be asked is named).
 * "a.example", "a.example or b.example", "a.example, b.example or c.example";
 * NULL for none. */
gchar *gh_mls_describe_hosts(const gchar *const *hosts);
/* Download's note for a file whose servers are hosts (in the order asked). */
gchar *gh_mls_download_note(const gchar *const *hosts, gboolean tor);
/* The admin's confirmation before a picture upload: where it goes, what
 * the server learns, who sees it. NULL without a host. */
gchar *gh_mls_picture_upload_note(const gchar *const *hosts, gboolean tor);

/* Whether the network mode is Tor (what a fetch reveals to the server). */
gboolean gh_mls_attachments_get_tor(GhMlsAttachments *self);

/* The user's words for an error of these operations (download: the
 * direction). */
gchar *gh_mls_attachments_describe(GhMlsAttachments *self, const GError *error,
                                   GhAttachmentsDirection direction, const gchar *host,
                                   gboolean *out_can_retry);

G_END_DECLS
#endif
