#ifndef GH_ATTACHMENT_H
#define GH_ATTACHMENT_H

#include "gh-blossom-client.h"
#include "gh-metadata-strip.h"
#include "gh-nip17-file.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * Encrypted NIP-17 attachments, the core (privacy charter §6, §0.1 item 9,
 * D6, G21; the UI is G22).
 *
 * Send: gh_attachment_upload_async() takes the file's bytes (read into
 * memory, at most the cap), removes JPEG/PNG metadata
 * (gh-metadata-strip.h), computes ox, encrypts with AES-256-GCM (OpenSSL
 * EVP; a random 32-byte key and 12-byte nonce per file), computes x, uploads
 * the ciphertext (gh-blossom-client.h) and finishes with the complete
 * GhNip17File, URL included, for gh_outbox_send_file() (gh-outbox.h).
 *
 * Receive: a kind-15 message is only metadata (gh_message_dup_file());
 * nothing is fetched until the user asks (PD-2, AT-7).
 * gh_attachment_download_async() is that request: it looks in the account's
 * encrypted cache first for this very file, its x with its key and nonce
 * (gh-store-media.h; never x alone, W17 review B1; no network then, and
 * ox still checked), else downloads
 * the ciphertext into memory (capped, AT-3), checks x BEFORE decrypting, then
 * the GCM tag, then ox if the sender gave one; any mismatch discards
 * everything with "This file was changed or damaged." (AT-2). The plaintext
 * goes only to that cache and to the caller, in memory that is wiped when
 * freed; never to a temporary or cache file (P3, AT-5). Saving it anywhere
 * else is the user's explicit export (G22, through the portal).
 *
 * Preview: gh_attachment_check_preview() is the decode guard to call before
 * gdk_texture_new_from_bytes(): PNG or JPEG by magic bytes only, header
 * dimensions at most GH_ATTACHMENT_PREVIEW_MAX_DIMENSION (AT-4); anything
 * else gets a card only.
 *
 * Main context only.
 */

#define GH_ATTACHMENT_PREVIEW_MAX_DIMENSION 8192

typedef enum {
  GH_ATTACHMENT_ERROR_EMPTY = 1,          /* nothing to send */
  GH_ATTACHMENT_ERROR_TOO_LARGE,          /* over the size cap (D6) */
  GH_ATTACHMENT_ERROR_DAMAGED,            /* x, GCM tag or ox mismatch (AT-2) */
  GH_ATTACHMENT_ERROR_NO_PREVIEW,         /* not PNG or JPEG: a card only (AT-4) */
  GH_ATTACHMENT_ERROR_PREVIEW_TOO_LARGE,  /* header dimensions over the limit (AT-4) */
  GH_ATTACHMENT_ERROR_CRYPTO              /* the cipher itself failed */
} GhAttachmentError;
#define GH_ATTACHMENT_ERROR gh_attachment_error_quark()
GQuark gh_attachment_error_quark(void);

/* ---- the steps, usable one by one ------------------------------------------------ */

typedef struct {
  GBytes *plaintext;          /* what will be encrypted: metadata removed */
  gchar *mime;                /* image/jpeg or image/png from the magic bytes,
                               * else the hint when it is a MIME type, else
                               * application/octet-stream */
  guint width, height;        /* JPEG/PNG header size; 0 otherwise */
  gboolean stripped;          /* JPEG/PNG metadata was removed */
  gboolean may_have_metadata; /* another type: show the one-time notice */
} GhAttachmentPrepared;

void gh_attachment_prepared_free(GhAttachmentPrepared *prepared);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAttachmentPrepared, gh_attachment_prepared_free)

/* Step 2 of §6: file (1 .. max_size bytes) with its metadata removed.
 * GH_ATTACHMENT_ERROR_EMPTY / _TOO_LARGE, or G_IO_ERROR_INVALID_DATA for a
 * damaged JPEG or PNG (never sent with its metadata). */
GhAttachmentPrepared *gh_attachment_prepare(GBytes *file, const gchar *mime_hint, gsize max_size,
                                            GError **error);
/* TRUE when file is not a JPEG or PNG, whose metadata Groundhog can remove:
 * the UI's one-time "Files can contain hidden details such as location." */
gboolean gh_attachment_may_have_metadata(GBytes *file);

typedef struct {
  GBytes *ciphertext;  /* AES-256-GCM output, tag appended */
  GhNip17File *file;   /* key, nonce, x, ox, size (of the ciphertext), type,
                        * dim; url NULL until uploaded */
} GhAttachmentSealed;

void gh_attachment_sealed_free(GhAttachmentSealed *sealed);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAttachmentSealed, gh_attachment_sealed_free)

/* Step 3 of §6: ox, then encryption with a fresh random key and nonce,
 * then x. */
GhAttachmentSealed *gh_attachment_encrypt(const GhAttachmentPrepared *prepared, GError **error);

/* Receive steps 4 of §6: x, then the GCM tag, then ox (when given); the
 * plaintext (wiped when freed) or GH_ATTACHMENT_ERROR_DAMAGED. */
GBytes *gh_attachment_decrypt(const GhNip17File *file, GBytes *ciphertext, GError **error);

/* The decode guard (AT-4). out_format, out_width and out_height are
 * nullable. */
gboolean gh_attachment_check_preview(GBytes *plaintext, GhMediaFormat *out_format,
                                     guint *out_width, guint *out_height, GError **error);

/* ---- the flows ------------------------------------------------------------------- */

/* Prepare, encrypt and upload file (at most the client's max file size).
 * Finishes with the complete GhNip17File to send. Errors of each step, the
 * client's GH_BLOSSOM_ERROR_* (e.g. NO_SERVER, AUTH_REQUIRED) and
 * G_IO_ERROR_CANCELLED; nothing is ever written to disk. */
void gh_attachment_upload_async(GhBlossomClient *client, GBytes *file, const gchar *mime_hint,
                                GCancellable *cancellable, GAsyncReadyCallback callback,
                                gpointer user_data);
GhNip17File *gh_attachment_upload_finish(GAsyncResult *result, GError **error);

/* The user's "Download" (PD-2: never automatic). cache (nullable) is the
 * account's open store: a file already there is returned without any
 * request, and a downloaded one is kept there. The store is borrowed: cancel
 * the download before closing it (an account switch), and a cancelled
 * download never touches it. Finishes with the plaintext
 * (wiped when freed) and, in out_cached (nullable), whether it came from the
 * cache. */
void gh_attachment_download_async(GhBlossomClient *client, GhStore *cache,
                                  const GhNip17File *file, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);
GBytes *gh_attachment_download_finish(GAsyncResult *result, gboolean *out_cached,
                                      GError **error);

G_END_DECLS
#endif
