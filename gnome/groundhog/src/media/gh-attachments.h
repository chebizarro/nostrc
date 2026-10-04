#ifndef GH_ATTACHMENTS_H
#define GH_ATTACHMENTS_H

#include "gh-attachment-transfer.h"
#include "gh-attachment.h"
#include "gh-message.h"

G_BEGIN_DECLS

/*
 * GhAttachments: the account's encrypted attachments as the UI uses them
 * (privacy charter §6, D6, G22), GTK-free, over G21's core
 * (gh-attachment.h, gh-blossom-client.h). Main context only.
 *
 * One Blossom client per open store. gh_attachments_set_store() follows the
 * account's open store: every change cancels every download and upload of
 * the previous one (before that store closes: a download borrows it as its
 * cache), drops its transfers and client, and makes a client for the new
 * account, signed by the account signer only for the servers the account
 * consented to (below). "reset" is emitted after each change.
 *
 * Received files (charter §6 "Receive", PD-2, AT-7). gh_attachments_lookup()
 * gives the GhAttachmentTransfer of a kind-15 message, IDLE the first time:
 * looking a file up fetches nothing. gh_attachments_download() is the user's
 * Download: the account's encrypted cache first (only this very file: its x,
 * key and nonce), else the network through GhNetHttp in the network mode,
 * with x, the GCM tag and ox checked before anything is shown
 * (gh_attachment_download_async()). A plaintext that passes the decode guard
 * is "previewable". gh_attachments_cancel() stops a download at once: the
 * transfer is IDLE again and nothing is kept (AT-8). Failures are said in
 * plain words (gh_attachments_describe_error()): damaged, too large, not on
 * a public server, no longer there, the server unreachable, Tor unreachable.
 * Decrypted files held in memory for the cards are bounded: beyond
 * GH_ATTACHMENTS_HELD_MAX bytes (the plaintext, and for a photo the 4 bytes
 * per pixel a card decodes it to) the least recently finished READY
 * transfers are let go (IDLE again, their decoded photo with them; Download
 * then finds them in the encrypted cache). The newest is always kept.
 *
 * Sent files. gh_attachments_upload_async() prepares (metadata removed),
 * encrypts and uploads a file (gh_attachment_upload_async()), and says which
 * server took it, or which one asked for an account it knows
 * (GH_BLOSSOM_ERROR_AUTH_REQUIRED). Sending the message is the outbox's
 * (gh_outbox_send_file_room()); gh_attachments_remember_sent() then keeps
 * the file in the encrypted cache as the sent message's, so the sender's own
 * card shows it without a download.
 *
 * Upload consent (charter §6 step 4, AT-6; nostrc-dnsc). A server that
 * accepts uploads only from accounts it knows gets the account's signature
 * only after the user's per-server consent, gh_attachments_set_consent(),
 * which is kept in the account's encrypted store (gh-store-blossom.h) and
 * loaded into the client of every later session of that account only;
 * revoking it deletes it, and the account signer is not asked again.
 *
 * The cache (charter §3.3 `media`): its size and "Clear", which deletes
 * every decrypted copy and returns every transfer to IDLE.
 */

/* Decrypted bytes (plaintext and decoded pixels) the transfers may hold in
 * memory at once. */
#define GH_ATTACHMENTS_HELD_MAX (64 * 1024 * 1024)

typedef struct {
  GSettings *settings;                 /* nullable: blossom-servers, network-mode */
  GhNetHttp *http;                     /* required: the app's (a reference is kept) */
  /* The account signer (gh_account_controller_sign_with_cancellable_async()
   * and _finish() in the application), for consented servers only;
   * sign_data is borrowed and must outlive the service. NULL: never. */
  GhBlossomSignAsyncFunc sign_async;
  GhBlossomSignFinishFunc sign_finish;
  gpointer sign_data;
} GhAttachmentsConfig;

#define GH_TYPE_ATTACHMENTS (gh_attachments_get_type())
G_DECLARE_FINAL_TYPE(GhAttachments, gh_attachments, GH, ATTACHMENTS, GObject)

GhAttachments *gh_attachments_new(const GhAttachmentsConfig *config);

/* The account's open store (borrowed until the next call; NULL: none). A
 * read-only (damaged) store still downloads, without keeping anything. */
void gh_attachments_set_store(GhAttachments *self, GhStore *store);
GhStore *gh_attachments_get_store(GhAttachments *self);
/* The current client (NULL without a store), e.g. for tests. */
GhBlossomClient *gh_attachments_get_client(GhAttachments *self);
/* Tests only: every later client may download from loopback and other
 * non-public hosts (gh_blossom_client_set_allow_private_hosts()). */
void gh_attachments_set_allow_private_hosts(GhAttachments *self, gboolean allow);
/* Tests only: a lower bound than GH_ATTACHMENTS_HELD_MAX on the decrypted
 * bytes held in memory. */
void gh_attachments_set_held_max(GhAttachments *self, guint64 held_max);

/* ---- received files ------------------------------------------------------------- */

/* The transfer of message's file (borrowed; the service keeps it until the
 * store changes), or NULL for a message without one or without a store.
 * Fetches nothing. */
GhAttachmentTransfer *gh_attachments_lookup(GhAttachments *self, GhMessage *message);
/* The user's Download of an IDLE or FAILED transfer of the current store;
 * nothing otherwise. */
void gh_attachments_download(GhAttachments *self, GhAttachmentTransfer *transfer);
/* Cancels a DOWNLOADING transfer: IDLE at once, nothing kept. */
void gh_attachments_cancel(GhAttachments *self, GhAttachmentTransfer *transfer);
/* Downloads started since the service was made (the user's Downloads,
 * served by the cache or the network), for tests. */
guint gh_attachments_get_downloads_started(GhAttachments *self);

/* ---- sent files ----------------------------------------------------------------- */

/* Prepares, encrypts and uploads file (mime: the chosen file's type, a
 * hint). Finishes with the complete GhNip17File to send, and in out_server
 * (nullable) the server used or, with GH_BLOSSOM_ERROR_AUTH_REQUIRED, the
 * server that asked for a known account. G_IO_ERROR_NOT_INITIALIZED without
 * a store; G_IO_ERROR_CANCELLED also when the store changed meanwhile. */
void gh_attachments_upload_async(GhAttachments *self, GBytes *file, const gchar *mime,
                                 GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer user_data);
/* As above, but bind one sheet's selected servers to this upload only. */
void gh_attachments_upload_on_servers_async(GhAttachments *self, const gchar *const *servers,
                                            GBytes *file, const gchar *mime,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
GhNip17File *gh_attachments_upload_finish(GhAttachments *self, GAsyncResult *result,
                                          gchar **out_server, GError **error);
/* The file of the own message rumor_id was queued with its plaintext (what
 * was encrypted): kept in the encrypted cache, and its transfer READY.
 * Nothing when the plaintext is not the file's (ox) or no store is open. */
void gh_attachments_remember_sent(GhAttachments *self, const gchar *rumor_id,
                                  const GhNip17File *file, GBytes *plaintext);

/* ---- upload consent (nostrc-dnsc) --------------------------------------------------- */

/* Records (or revokes) the account's consent to upload to server as the
 * account, in the store and the client. G_IO_ERROR_INVALID_ARGUMENT for a
 * server that is not an http(s) URL, G_IO_ERROR_NOT_INITIALIZED without a
 * store, or the store's error. */
gboolean gh_attachments_set_consent(GhAttachments *self, const gchar *server, gboolean consent,
                                    GError **error);
gboolean gh_attachments_get_consent(GhAttachments *self, const gchar *server);
/* The servers the account consented to (normalized); empty without a store. */
GStrv gh_attachments_dup_consents(GhAttachments *self);

/* ---- the cache ---------------------------------------------------------------------- */

/* Bytes of decrypted files the account's encrypted cache holds. */
gboolean gh_attachments_get_cache_size(GhAttachments *self, gint64 *out_bytes, GError **error);
/* Deletes every decrypted copy: downloads in flight are cancelled and every
 * transfer is IDLE again (the same objects, so cards stay bound). */
gboolean gh_attachments_clear_cache(GhAttachments *self, GError **error);

/* ---- errors ---------------------------------------------------------------------------- */

typedef enum {
  GH_ATTACHMENTS_DOWNLOAD,
  GH_ATTACHMENTS_UPLOAD
} GhAttachmentsDirection;

/* The user's words for error of a download or an upload: damaged, too large,
 * an address Groundhog won't fetch, a .onion outside Tor, gone from the
 * server, refused, the server unreachable, or Tor unreachable (tor: Tor is
 * the network mode; tor_unreachable: nothing answers at the Tor address).
 * host (nullable) names the server. out_can_retry (nullable): whether trying
 * again can help. */
gchar *gh_attachments_describe_error(const GError *error, GhAttachmentsDirection direction,
                                     gboolean tor, gboolean tor_unreachable, const gchar *host,
                                     gboolean *out_can_retry);
/* The same, for this service's network mode and Tor state. */
gchar *gh_attachments_describe(GhAttachments *self, const GError *error,
                               GhAttachmentsDirection direction, const gchar *host,
                               gboolean *out_can_retry);
/* Whether the network mode is Tor (network-mode = "tor"). */
gboolean gh_attachments_get_tor(GhAttachments *self);

G_END_DECLS
#endif
