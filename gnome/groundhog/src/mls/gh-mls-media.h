#ifndef GH_MLS_MEDIA_H
#define GH_MLS_MEDIA_H

/* Encrypted group attachments: MIP-04 encrypted-media-v2 (nostrc-u7cb).
 *
 * The libmarmot steps (seal, open, the epoch check) are synchronous and run
 * on the thread that owns the Marmot, as every other GhMlsService call:
 * pass gh_mls_service_get_marmot() and gh_mls_group_get_group_id(). The
 * network steps only touch a GhBlossomClient, so every byte goes through
 * GhNetHttp (Tor and the network mode; public hosts only on download).
 *
 * Sending, in order:
 *   1. gh_mls_media_read_file_async(): a file on this device only (no remote
 *      GVfs location, no GVfs FUSE path), refused over max_size before it is
 *      read.
 *   2. gh_mls_media_seal(): JPEG and PNG metadata removed
 *      (gh_attachment_prepare); every other type is sent unchanged and the
 *      result says so (may_have_metadata: show the one-time notice), then
 *      encrypted for the group's current epoch.
 *   3. gh_mls_media_upload_async(): the ciphertext to Blossom; the server's
 *      URL becomes the blossom-v1 locator.
 *   4. In the send transaction, gh_mls_media_check_epoch() before
 *      marmot_create_message(): media is bound to the epoch it was sealed in,
 *      so a Commit in between means sealing and uploading again. The kind-9
 *      inner event carries one gh_mls_attachment_dup_imeta() tag per file.
 * Receiving: gh_mls_attachments_from_inner_event() with the epoch libmarmot
 * reported (MarmotMessageResult.app_msg.epoch), then on the user's Download
 * gh_mls_media_fetch_async() and gh_mls_media_open(). A rejected or
 * unavailable attachment never invalidates its message. */

#include "gh-blossom-client.h"

#include <gio/gio.h>
#include <marmot/marmot.h>

G_BEGIN_DECLS

typedef enum {
  GH_MLS_MEDIA_ERROR_NOT_LOCAL = 1, /* not a file on this device */
  GH_MLS_MEDIA_ERROR_INVALID,       /* a malformed or unusable reference */
  GH_MLS_MEDIA_ERROR_UNSUPPORTED,   /* not encrypted-media-v2 */
  GH_MLS_MEDIA_ERROR_EPOCH_CHANGED, /* the group moved on: seal and upload again */
  GH_MLS_MEDIA_ERROR_UNAVAILABLE,   /* no locator to fetch, or every one failed */
  GH_MLS_MEDIA_ERROR_DAMAGED,       /* a hash or the authentication tag mismatched */
  GH_MLS_MEDIA_ERROR_NO_KEY,        /* that epoch's key is no longer kept */
  GH_MLS_MEDIA_ERROR_FAILED,        /* libmarmot failed otherwise */
  GH_MLS_MEDIA_ERROR_TOO_LARGE      /* over the size limit, refused before reading */
} GhMlsMediaError;

#define GH_MLS_MEDIA_ERROR gh_mls_media_error_quark()
GQuark gh_mls_media_error_quark(void);

/* One attachment: an encrypted-media-v2 reference and its source epoch. */
typedef struct _GhMlsAttachment GhMlsAttachment;

#define GH_TYPE_MLS_ATTACHMENT (gh_mls_attachment_get_type())
GType gh_mls_attachment_get_type(void);
GhMlsAttachment *gh_mls_attachment_copy(const GhMlsAttachment *attachment);
void gh_mls_attachment_free(GhMlsAttachment *attachment);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsAttachment, gh_mls_attachment_free)

guint64 gh_mls_attachment_get_source_epoch(const GhMlsAttachment *attachment);
const gchar *gh_mls_attachment_get_media_type(const GhMlsAttachment *attachment);
const gchar *gh_mls_attachment_get_filename(const GhMlsAttachment *attachment);
/* The render hint, NULL when absent. */
const gchar *gh_mls_attachment_get_dim(const GhMlsAttachment *attachment);
/* Lowercase hex of the ciphertext SHA-256 (the Blossom blob id). */
gchar *gh_mls_attachment_dup_ciphertext_sha256(const GhMlsAttachment *attachment);
/* The blossom-v1 locator URLs, in tag order (NULL-terminated). */
GStrv gh_mls_attachment_dup_blossom_urls(const GhMlsAttachment *attachment);

/* The ordered v2 imeta tag, "imeta" first. Fails (GH_MLS_MEDIA_ERROR_INVALID)
 * without a blossom-v1 locator or with a kind the default policy forbids. */
GStrv gh_mls_attachment_dup_imeta(const GhMlsAttachment *attachment, GError **error);
/* Parses and validates one imeta tag. */
GhMlsAttachment *gh_mls_attachment_new_from_imeta(const gchar *const *tag, guint64 source_epoch,
                                                  GError **error);
/* Every valid v2 attachment of an inner event (unsigned JSON), in order.
 * A malformed tag is skipped alone (attachment-local rejection);
 * out_rejected (nullable) counts them. */
GPtrArray *gh_mls_attachments_from_inner_event(const gchar *inner_json, guint64 source_epoch,
                                               guint *out_rejected);

/* Step 1: loads a file on this device. Refused (GH_MLS_MEDIA_ERROR_NOT_LOCAL)
 * before any I/O when it is not native, or its path (as given, then with
 * symlinks resolved) is under a GVfs FUSE mount ($XDG_RUNTIME_DIR/gvfs,
 * ~/.gvfs): GVfs would fetch it outside GhNetHttp and Tor. Not a regular
 * file, or on a filesystem of type gvfs: NOT_LOCAL too. Over max_size
 * (standard::size): GH_MLS_MEDIA_ERROR_TOO_LARGE, before reading it.
 * Finishes with its bytes; out_name and out_type (nullable) get the display
 * name and the content type's MIME type. */
void gh_mls_media_read_file_async(GFile *file, gsize max_size, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);
GBytes *gh_mls_media_read_file_finish(GAsyncResult *result, gchar **out_name,
                                      gchar **out_type, GError **error);

typedef struct {
  GBytes *ciphertext;          /* to upload; its SHA-256 is the blob id */
  GhMlsAttachment *attachment; /* no locator until uploaded */
  guint width, height;         /* JPEG/PNG header size; 0 otherwise */
  gboolean stripped;           /* JPEG/PNG metadata was removed */
  gboolean may_have_metadata;  /* another type, sent unchanged: it may carry
                                * location, author or device details (show
                                * the one-time notice, as for NIP-17) */
} GhMlsMediaSealed;
void gh_mls_media_sealed_free(GhMlsMediaSealed *sealed);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsMediaSealed, gh_mls_media_sealed_free)

/* Step 2: file (1 .. max_size bytes), JPEG/PNG metadata removed and any
 * other type unchanged (may_have_metadata), encrypted for the group's
 * current epoch. filename is the display name (1..255 bytes of UTF-8, sent
 * as is: a camera name like IMG_20260930_142233.jpg is itself a
 * timestamp); mime_hint as gh_attachment_prepare(). */
GhMlsMediaSealed *gh_mls_media_seal(Marmot *marmot, const gchar *group_id_hex, GBytes *file,
                                    const gchar *mime_hint, const gchar *filename,
                                    gsize max_size, GError **error);

/* Step 3: uploads sealed->ciphertext and finishes with a copy of the
 * attachment holding the server's URL as its blossom-v1 locator. */
void gh_mls_media_upload_async(GhBlossomClient *client, const GhMlsMediaSealed *sealed,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data);
GhMlsAttachment *gh_mls_media_upload_finish(GAsyncResult *result, GError **error);

/* Step 4: TRUE when a message sent now is in the attachment's epoch
 * (marmot_media_check_epoch(): an interrupted epoch transition is
 * reconciled first, as marmot_create_message() will). Call it in the same
 * main-loop turn as marmot_create_message(). */
gboolean gh_mls_media_check_epoch(Marmot *marmot, const gchar *group_id_hex,
                                  const GhMlsAttachment *attachment, GError **error);

/* The user's Download: tries each blossom-v1 locator in order and finishes
 * with the first ciphertext whose SHA-256 is the reference's. A locator
 * GhBlossomClient refuses (a private host, a URL not naming the blob) is
 * skipped; with none left, GH_MLS_MEDIA_ERROR_UNAVAILABLE. */
void gh_mls_media_fetch_async(GhBlossomClient *client, const GhMlsAttachment *attachment,
                              GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data);
GBytes *gh_mls_media_fetch_finish(GAsyncResult *result, GError **error);

/* Decrypts ciphertext with the source epoch's key: ciphertext hash, then
 * the AEAD, then the plaintext hash (GH_MLS_MEDIA_ERROR_DAMAGED). The
 * returned bytes are wiped when freed. */
GBytes *gh_mls_media_open(Marmot *marmot, const gchar *group_id_hex,
                          const GhMlsAttachment *attachment, GBytes *ciphertext,
                          GError **error);

G_END_DECLS

#endif
