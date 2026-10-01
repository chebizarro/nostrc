#include "gh-attachment.h"
#include "gh-attachment-private.h"
#include "gh-store-media.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(gh-attachment-error-quark, gh_attachment_error)

#define DAMAGED_MESSAGE "This file was changed or damaged."

/* ---- memory that holds plaintext -------------------------------------------------- */

typedef struct {
  guint8 *data;
  gsize size;
} Secret;

static void
secret_free(gpointer data)
{
  Secret *secret = data;
  if (secret->data)
    OPENSSL_cleanse(secret->data, secret->size);
  g_free(secret->data);
  g_free(secret);
}

/* A buffer of size bytes whose GBytes wipes it when the last reference goes. */
static GBytes *
secret_bytes_new(gsize size, guint8 **out_data)
{
  Secret *secret = g_new0(Secret, 1);
  secret->size = size;
  secret->data = g_malloc(size ? size : 1);
  *out_data = secret->data;
  return g_bytes_new_with_free_func(secret->data, size, secret_free, secret);
}

static void
sha256_hex(GBytes *bytes, gchar out[65])
{
  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  g_checksum_update(checksum, data, (gssize)size);
  g_strlcpy(out, g_checksum_get_string(checksum), 65);
}

/* ---- prepare --------------------------------------------------------------------- */

void
gh_attachment_prepared_free(GhAttachmentPrepared *prepared)
{
  if (!prepared)
    return;
  g_clear_pointer(&prepared->plaintext, g_bytes_unref);
  g_free(prepared->mime);
  g_free(prepared);
}

gboolean
gh_attachment_may_have_metadata(GBytes *file)
{
  return gh_media_sniff(file) == GH_MEDIA_FORMAT_OTHER;
}

/* Under a GVfs FUSE mount: reading it makes gvfsd fetch it (SMB, SFTP, ...)
 * outside GhNetHttp and Tor, although g_file_is_native() says TRUE. */
static gboolean
path_under_gvfs(const gchar *path)
{
  if (!path)
    return FALSE;
  g_autofree gchar *run = g_build_filename(g_get_user_runtime_dir(), "gvfs", NULL);
  g_autofree gchar *home = g_build_filename(g_get_home_dir(), ".gvfs", NULL);
  /* Each root as given and with its own symlinks resolved (/var is
   * /private/var on macOS; a resolved file path carries the latter). */
  char run_real[PATH_MAX], home_real[PATH_MAX];
  const gchar *roots[] = { run, home, realpath(run, run_real) ? run_real : NULL,
                           realpath(home, home_real) ? home_real : NULL };
  for (guint i = 0; i < G_N_ELEMENTS(roots); i++) {
    if (!roots[i])
      continue;
    gsize n = strlen(roots[i]);
    if (strncmp(path, roots[i], n) == 0 && (path[n] == '\0' || path[n] == G_DIR_SEPARATOR))
      return TRUE;
  }
  return FALSE;
}

gboolean
gh_attachment_path_on_remote_mount(const gchar *path)
{
  if (!path)
    return FALSE;
  if (path_under_gvfs(path))
    return TRUE;
  char resolved[PATH_MAX];
  return realpath(path, resolved) && path_under_gvfs(resolved);
}


GhAttachmentPrepared *
gh_attachment_prepare(GBytes *file, const gchar *mime_hint, gsize max_size, GError **error)
{
  gsize size = file ? g_bytes_get_size(file) : 0;
  if (size == 0) {
    g_set_error_literal(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_EMPTY,
                        "The file is empty");
    return NULL;
  }
  if (size > max_size) {
    g_set_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_TOO_LARGE,
                "Files can be at most %" G_GSIZE_FORMAT " MB", max_size / (1024 * 1024));
    return NULL;
  }
  GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
  gboolean stripped = FALSE;
  g_autoptr(GBytes) plaintext = gh_metadata_strip(file, &format, &stripped, error);
  if (!plaintext)
    return NULL;
  g_autoptr(GhAttachmentPrepared) prepared = g_new0(GhAttachmentPrepared, 1);
  if (format != GH_MEDIA_FORMAT_OTHER) {
    if (!gh_media_probe_dimensions(plaintext, NULL, &prepared->width, &prepared->height, error))
      return NULL;
    prepared->mime = g_strdup(gh_media_format_mime(format));
  } else {
    prepared->mime = gh_nip17_file_normalize_type(mime_hint);
    /* A hint that claims an image the magic bytes deny is not trusted. */
    if (!prepared->mime || g_str_equal(prepared->mime, "image/jpeg") ||
        g_str_equal(prepared->mime, "image/png")) {
      g_free(prepared->mime);
      prepared->mime = g_strdup("application/octet-stream");
    }
  }
  prepared->plaintext = g_steal_pointer(&plaintext);
  prepared->stripped = stripped;
  prepared->may_have_metadata = format == GH_MEDIA_FORMAT_OTHER;
  return g_steal_pointer(&prepared);
}

/* ---- AES-256-GCM ------------------------------------------------------------------ */

static gboolean
crypto_failed(GError **error)
{
  g_set_error_literal(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_CRYPTO,
                      "The file could not be encrypted");
  return FALSE;
}

static guint decrypt_runs; /* gh-attachment-private.h */

guint
gh_attachment_test_get_decrypt_runs(void)
{
  return g_atomic_int_get(&decrypt_runs);
}

/* out receives in_size bytes; tag is written (encrypt) or checked (decrypt). */
static gboolean
aes_gcm(gboolean encrypt, const guint8 *key, const guint8 *nonce, gsize nonce_size,
        const guint8 *in, gsize in_size, guint8 *out, guint8 tag[GH_NIP17_FILE_TAG_SIZE])
{
  if (!encrypt)
    g_atomic_int_inc(&decrypt_runs);
  if (in_size > G_MAXINT)
    return FALSE;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int length = 0;
  gboolean ok = ctx != NULL;
  ok = ok && EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL, encrypt ? 1 : 0) == 1;
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_size, NULL) == 1;
  ok = ok && EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, encrypt ? 1 : 0) == 1;
  ok = ok && (in_size == 0 ||
              EVP_CipherUpdate(ctx, out, &length, in, (int)in_size) == 1);
  ok = ok && (gsize)length == in_size;
  if (ok && !encrypt)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GH_NIP17_FILE_TAG_SIZE, tag) == 1;
  int final_length = 0;
  ok = ok && EVP_CipherFinal_ex(ctx, out + length, &final_length) == 1 && final_length == 0;
  if (ok && encrypt)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GH_NIP17_FILE_TAG_SIZE, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

void
gh_attachment_sealed_free(GhAttachmentSealed *sealed)
{
  if (!sealed)
    return;
  g_clear_pointer(&sealed->ciphertext, g_bytes_unref);
  gh_nip17_file_free(sealed->file);
  g_free(sealed);
}

GhAttachmentSealed *
gh_attachment_encrypt(const GhAttachmentPrepared *prepared, GError **error)
{
  g_return_val_if_fail(prepared != NULL && prepared->plaintext != NULL, NULL);
  gsize size = 0;
  const guint8 *plain = g_bytes_get_data(prepared->plaintext, &size);
  if (size == 0) {
    g_set_error_literal(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_EMPTY,
                        "The file is empty");
    return NULL;
  }
  g_autoptr(GhNip17File) file = g_new0(GhNip17File, 1);
  file->nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  if (RAND_bytes(file->key, sizeof file->key) != 1 ||
      RAND_bytes(file->nonce, (int)file->nonce_size) != 1) {
    crypto_failed(error);
    return NULL;
  }
  sha256_hex(prepared->plaintext, file->ox);
  guint8 *out = g_malloc(size + GH_NIP17_FILE_TAG_SIZE);
  if (!aes_gcm(TRUE, file->key, file->nonce, file->nonce_size, plain, size, out, out + size)) {
    g_free(out);
    crypto_failed(error);
    return NULL;
  }
  GhAttachmentSealed *sealed = g_new0(GhAttachmentSealed, 1);
  sealed->ciphertext = g_bytes_new_take(out, size + GH_NIP17_FILE_TAG_SIZE);
  sha256_hex(sealed->ciphertext, file->x);
  file->size = size + GH_NIP17_FILE_TAG_SIZE;
  file->file_type = g_strdup(prepared->mime);
  file->width = prepared->width;
  file->height = prepared->height;
  if (file->width > 65535 || file->height > 65535)
    file->width = file->height = 0; /* not expressible as a dim tag */
  sealed->file = g_steal_pointer(&file);
  return sealed;
}

static gboolean
damaged(GError **error)
{
  g_set_error_literal(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED, DAMAGED_MESSAGE);
  return FALSE;
}

GBytes *
gh_attachment_decrypt(const GhNip17File *file, GBytes *ciphertext, GError **error)
{
  g_return_val_if_fail(file != NULL && ciphertext != NULL, NULL);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(ciphertext, &size);
  gchar x[65];
  /* x first: nothing that is not exactly the file the sender named is ever
   * given to the cipher (charter §6 receive step 4, AT-2). */
  sha256_hex(ciphertext, x);
  if (size <= GH_NIP17_FILE_TAG_SIZE || g_ascii_strcasecmp(x, file->x) != 0 ||
      (file->nonce_size != GH_NIP17_FILE_NONCE_SIZE &&
       file->nonce_size != GH_NIP17_FILE_NONCE_MAX)) {
    damaged(error);
    return NULL;
  }
  gsize plain_size = size - GH_NIP17_FILE_TAG_SIZE;
  guint8 tag[GH_NIP17_FILE_TAG_SIZE];
  memcpy(tag, data + plain_size, sizeof tag);
  guint8 *out = NULL;
  g_autoptr(GBytes) plaintext = secret_bytes_new(plain_size, &out);
  if (!aes_gcm(FALSE, file->key, file->nonce, file->nonce_size, data, plain_size, out, tag)) {
    damaged(error); /* the GCM tag: a sender-consistent x does not help (AT-2) */
    return NULL;
  }
  if (file->ox[0]) {
    gchar ox[65];
    sha256_hex(plaintext, ox);
    if (g_ascii_strcasecmp(ox, file->ox) != 0) {
      damaged(error);
      return NULL;
    }
  }
  return g_steal_pointer(&plaintext);
}

gboolean
gh_attachment_check_preview(GBytes *plaintext, GhMediaFormat *out_format, guint *out_width,
                            guint *out_height, GError **error)
{
  g_return_val_if_fail(plaintext != NULL, FALSE);
  GhMediaFormat format = GH_MEDIA_FORMAT_OTHER;
  guint width = 0, height = 0;
  g_autoptr(GError) probe_error = NULL;
  if (!gh_media_probe_dimensions(plaintext, &format, &width, &height, &probe_error)) {
    g_set_error_literal(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_NO_PREVIEW,
                        format == GH_MEDIA_FORMAT_OTHER
                          ? "Only PNG and JPEG images are previewed"
                          : "This image is damaged and can't be previewed");
    return FALSE;
  }
  if (width > GH_ATTACHMENT_PREVIEW_MAX_DIMENSION || height > GH_ATTACHMENT_PREVIEW_MAX_DIMENSION) {
    g_set_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_PREVIEW_TOO_LARGE,
                "This image is too large to preview (%u × %u)", width, height);
    return FALSE;
  }
  if (out_format)
    *out_format = format;
  if (out_width)
    *out_width = width;
  if (out_height)
    *out_height = height;
  return TRUE;
}

/* ---- upload ---------------------------------------------------------------------- */

typedef struct {
  GhAttachmentSealed *sealed;
  gchar *server; /* the server used, or the one that asked for consent */
} Upload;

static void
upload_free(gpointer data)
{
  Upload *upload = data;
  gh_attachment_sealed_free(upload->sealed);
  g_free(upload->server);
  g_free(upload);
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Upload *upload = g_task_get_task_data(task);
  GhAttachmentSealed *sealed = upload->sealed;
  GError *error = NULL;
  gchar *url = gh_blossom_client_upload_finish(GH_BLOSSOM_CLIENT(source), result,
                                               &upload->server, &error);
  if (!url) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  GhNip17File *file = gh_nip17_file_copy(sealed->file);
  file->url = url;
  g_task_return_pointer(task, file, (GDestroyNotify)gh_nip17_file_free);
  g_object_unref(task);
}

void
gh_attachment_upload_async(GhBlossomClient *client, GBytes *file, const gchar *mime_hint,
                           GCancellable *cancellable, GAsyncReadyCallback callback,
                           gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(client));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_attachment_upload_async);
  GError *error = NULL;
  g_autoptr(GhAttachmentPrepared) prepared =
    gh_attachment_prepare(file, mime_hint, gh_blossom_client_get_max_file_size(client), &error);
  GhAttachmentSealed *sealed = prepared ? gh_attachment_encrypt(prepared, &error) : NULL;
  /* The plaintext goes (and is wiped) before any byte leaves. */
  g_clear_pointer(&prepared, gh_attachment_prepared_free);
  if (!sealed) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  Upload *upload = g_new0(Upload, 1);
  upload->sealed = sealed;
  g_task_set_task_data(task, upload, upload_free);
  gh_blossom_client_upload_async(client, sealed->ciphertext, sealed->file->x, cancellable,
                                 on_uploaded, task);
}

GhNip17File *
gh_attachment_upload_finish(GAsyncResult *result, GError **error)
{
  return gh_attachment_upload_finish_full(result, NULL, error);
}

GhNip17File *
gh_attachment_upload_finish_full(GAsyncResult *result, gchar **out_server, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  if (out_server) {
    Upload *upload = g_task_get_task_data(G_TASK(result));
    *out_server = upload ? g_strdup(upload->server) : NULL;
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- download -------------------------------------------------------------------- */

typedef struct {
  GhNip17File *file;
  GhStore *cache;   /* borrowed: the caller keeps the store open */
  gboolean cached;
} Download;

static void
download_free(gpointer data)
{
  Download *download = data;
  gh_nip17_file_free(download->file);
  g_free(download);
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Download *download = g_task_get_task_data(task);
  GError *error = NULL;
  g_autoptr(GBytes) ciphertext =
    gh_blossom_client_download_finish(GH_BLOSSOM_CLIENT(source), result, &error);
  /* Cancelled (e.g. the account is switching and its store closing): the
   * store is not touched and nothing is decrypted. */
  if (ciphertext && g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  GBytes *plaintext = ciphertext ? gh_attachment_decrypt(download->file, ciphertext, &error)
                                 : NULL;
  if (!plaintext) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (download->cache) {
    g_autoptr(GError) cache_error = NULL;
    if (!gh_store_media_put(download->cache, download->file, download->file->file_type,
                            plaintext, &cache_error))
      g_debug("Attachment: not kept in the encrypted cache: %s", cache_error->message);
  }
  g_task_return_pointer(task, plaintext, (GDestroyNotify)g_bytes_unref);
  g_object_unref(task);
}

void
gh_attachment_download_async(GhBlossomClient *client, GhStore *cache, const GhNip17File *file,
                             GCancellable *cancellable, GAsyncReadyCallback callback,
                             gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(client));
  g_return_if_fail(file != NULL && file->url != NULL);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_attachment_download_async);
  Download *download = g_new0(Download, 1);
  download->file = gh_nip17_file_copy(file);
  download->cache = cache;
  g_task_set_task_data(task, download, download_free);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (cache) {
    /* Only this file (its x, key and nonce) is looked up: a message that
     * names another file's x misses, and is downloaded and verified like
     * any other (W17 review B1). */
    g_autoptr(GError) cache_error = NULL;
    GBytes *kept = gh_store_media_get(cache, download->file, NULL, &cache_error);
    if (kept) {
      /* The cached bytes are this key's decryption of this x; ox, when the
       * message gives one, still has to match, as after a download. */
      gchar ox[65];
      sha256_hex(kept, ox);
      if (download->file->ox[0] && g_ascii_strcasecmp(ox, download->file->ox) != 0) {
        g_bytes_unref(kept);
        g_task_return_new_error(task, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED,
                                DAMAGED_MESSAGE);
        g_object_unref(task);
        return;
      }
      download->cached = TRUE;
      g_task_return_pointer(task, kept, (GDestroyNotify)g_bytes_unref);
      g_object_unref(task);
      return;
    }
    if (cache_error)
      g_debug("Attachment: the encrypted cache could not be read: %s", cache_error->message);
  }
  gh_blossom_client_download_async(client, download->file->url, download->file->x,
                                   download->file->size, cancellable, on_downloaded, task);
}

GBytes *
gh_attachment_download_finish(GAsyncResult *result, gboolean *out_cached, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  if (out_cached) {
    Download *download = g_task_get_task_data(G_TASK(result));
    *out_cached = download && download->cached;
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}
