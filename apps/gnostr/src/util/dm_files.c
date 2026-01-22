#define G_LOG_DOMAIN "gnostr-dm-files"

/**
 * GnostrDmFiles - NIP-17 File Message (Kind 15) Implementation
 *
 * Implements encrypted file attachments for direct messages using:
 * - AES-256-GCM for file encryption
 * - Blossom for file hosting
 * - NIP-17 kind 15 event structure
 */

#include "dm_files.h"
#include "blossom.h"
#include "blossom_settings.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* For AES-GCM we use GLib's GCipher if available, otherwise OpenSSL or libsodium */
#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#elif defined(HAVE_GCRYPT)
#include <gcrypt.h>
#else
/* Fallback: use GIO's GChecksum for hashing, implement basic AES-GCM stub */
/* In production, one of the above crypto libraries should be available */
#endif

G_DEFINE_QUARK(gnostr-dm-file-error-quark, gnostr_dm_file_error)

void gnostr_dm_file_attachment_free(GnostrDmFileAttachment *attachment) {
  if (!attachment) return;

  g_free(attachment->original_path);
  g_free(attachment->mime_type);
  g_free(attachment->original_sha256);
  g_free(attachment->encrypted_sha256);
  g_free(attachment->upload_url);
  g_free(attachment->blurhash);
  g_free(attachment->thumb_url);
  g_free(attachment->thumb_sha256);

  /* Clear sensitive key material */
  memset(attachment->key, 0, GNOSTR_DM_FILES_AES_KEY_SIZE);
  memset(attachment->nonce, 0, GNOSTR_DM_FILES_AES_NONCE_SIZE);

  g_free(attachment);
}

void gnostr_dm_file_message_free(GnostrDmFileMessage *msg) {
  if (!msg) return;

  g_free(msg->sender_pubkey);
  g_free(msg->file_url);
  g_free(msg->file_type);
  g_free(msg->encryption_algorithm);
  g_free(msg->decryption_key_b64);
  g_free(msg->decryption_nonce_b64);
  g_free(msg->encrypted_hash);
  g_free(msg->original_hash);
  g_free(msg->blurhash);
  g_free(msg->thumb_url);

  if (msg->fallback_urls) {
    for (char **p = msg->fallback_urls; *p; p++) {
      g_free(*p);
    }
    g_free(msg->fallback_urls);
  }

  g_free(msg);
}

gboolean gnostr_dm_file_random_bytes(uint8_t *buffer, gsize len) {
  if (!buffer || len == 0) return FALSE;

#ifdef HAVE_OPENSSL
  return RAND_bytes(buffer, (int)len) == 1;
#elif defined(HAVE_GCRYPT)
  gcry_randomize(buffer, len, GCRY_STRONG_RANDOM);
  return TRUE;
#else
  /* Fallback: use GLib's random which reads from /dev/urandom on Unix */
  GRand *rand = g_rand_new();
  for (gsize i = 0; i < len; i++) {
    buffer[i] = (uint8_t)(g_rand_int(rand) & 0xFF);
  }
  g_rand_free(rand);
  return TRUE;
#endif
}

gboolean gnostr_dm_file_aes_gcm_encrypt(const uint8_t *plaintext,
                                         gsize plaintext_len,
                                         const uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE],
                                         const uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_SIZE],
                                         uint8_t *ciphertext,
                                         gsize *ciphertext_len) {
  if (!plaintext || !key || !nonce || !ciphertext || !ciphertext_len) {
    return FALSE;
  }

#ifdef HAVE_OPENSSL
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return FALSE;

  int len = 0;
  int total_len = 0;

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GNOSTR_DM_FILES_AES_NONCE_SIZE, NULL) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, (int)plaintext_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }
  total_len = len;

  if (EVP_EncryptFinal_ex(ctx, ciphertext + total_len, &len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }
  total_len += len;

  /* Append the authentication tag */
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GNOSTR_DM_FILES_AES_TAG_SIZE,
                           ciphertext + total_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }
  total_len += GNOSTR_DM_FILES_AES_TAG_SIZE;

  EVP_CIPHER_CTX_free(ctx);
  *ciphertext_len = (gsize)total_len;
  return TRUE;

#elif defined(HAVE_GCRYPT)
  gcry_cipher_hd_t hd;
  gcry_error_t err;

  err = gcry_cipher_open(&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
  if (err) return FALSE;

  err = gcry_cipher_setkey(hd, key, GNOSTR_DM_FILES_AES_KEY_SIZE);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  err = gcry_cipher_setiv(hd, nonce, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  err = gcry_cipher_encrypt(hd, ciphertext, plaintext_len, plaintext, plaintext_len);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  /* Get authentication tag */
  err = gcry_cipher_gettag(hd, ciphertext + plaintext_len, GNOSTR_DM_FILES_AES_TAG_SIZE);
  gcry_cipher_close(hd);

  if (err) return FALSE;

  *ciphertext_len = plaintext_len + GNOSTR_DM_FILES_AES_TAG_SIZE;
  return TRUE;

#else
  /* Stub implementation - in production, one of the above should be available */
  g_warning("AES-GCM encryption not available - no crypto library compiled in");
  return FALSE;
#endif
}

gboolean gnostr_dm_file_aes_gcm_decrypt(const uint8_t *ciphertext,
                                         gsize ciphertext_len,
                                         const uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE],
                                         const uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_SIZE],
                                         uint8_t *plaintext,
                                         gsize *plaintext_len) {
  if (!ciphertext || !key || !nonce || !plaintext || !plaintext_len) {
    return FALSE;
  }

  if (ciphertext_len < GNOSTR_DM_FILES_AES_TAG_SIZE) {
    return FALSE;
  }

  gsize data_len = ciphertext_len - GNOSTR_DM_FILES_AES_TAG_SIZE;

#ifdef HAVE_OPENSSL
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return FALSE;

  int len = 0;
  int total_len = 0;

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GNOSTR_DM_FILES_AES_NONCE_SIZE, NULL) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, (int)data_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }
  total_len = len;

  /* Set the authentication tag */
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GNOSTR_DM_FILES_AES_TAG_SIZE,
                           (void *)(ciphertext + data_len)) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }

  /* Verify and finalize */
  if (EVP_DecryptFinal_ex(ctx, plaintext + total_len, &len) != 1) {
    /* Authentication failed */
    EVP_CIPHER_CTX_free(ctx);
    return FALSE;
  }
  total_len += len;

  EVP_CIPHER_CTX_free(ctx);
  *plaintext_len = (gsize)total_len;
  return TRUE;

#elif defined(HAVE_GCRYPT)
  gcry_cipher_hd_t hd;
  gcry_error_t err;

  err = gcry_cipher_open(&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
  if (err) return FALSE;

  err = gcry_cipher_setkey(hd, key, GNOSTR_DM_FILES_AES_KEY_SIZE);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  err = gcry_cipher_setiv(hd, nonce, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  err = gcry_cipher_decrypt(hd, plaintext, data_len, ciphertext, data_len);
  if (err) {
    gcry_cipher_close(hd);
    return FALSE;
  }

  /* Verify authentication tag */
  err = gcry_cipher_checktag(hd, ciphertext + data_len, GNOSTR_DM_FILES_AES_TAG_SIZE);
  gcry_cipher_close(hd);

  if (err) {
    /* Authentication failed */
    return FALSE;
  }

  *plaintext_len = data_len;
  return TRUE;

#else
  g_warning("AES-GCM decryption not available - no crypto library compiled in");
  return FALSE;
#endif
}

/* Compute SHA-256 hash of data and return hex string */
static char *compute_sha256_hex(const uint8_t *data, gsize len) {
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, data, len);

  const char *hash = g_checksum_get_string(checksum);
  char *result = g_strdup(hash);
  g_checksum_free(checksum);

  return result;
}

/* Convert binary data to base64 */
static char *to_base64(const uint8_t *data, gsize len) {
  return g_base64_encode(data, len);
}

/* Convert base64 to binary data */
static uint8_t *from_base64(const char *b64, gsize *out_len) {
  return g_base64_decode(b64, out_len);
}

/* ---- Async upload context ---- */

typedef struct {
  char *file_path;
  char *mime_type;
  GnostrDmFileUploadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GnostrDmFileAttachment *attachment;
  char *temp_encrypted_path;  /* Temp file for encrypted data */
} DmFileUploadContext;

static void upload_ctx_free(DmFileUploadContext *ctx) {
  if (!ctx) return;
  g_free(ctx->file_path);
  g_free(ctx->mime_type);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->attachment) gnostr_dm_file_attachment_free(ctx->attachment);
  if (ctx->temp_encrypted_path) {
    g_unlink(ctx->temp_encrypted_path);  /* Clean up temp file */
    g_free(ctx->temp_encrypted_path);
  }
  g_free(ctx);
}

static void on_blossom_upload_complete(GnostrBlossomBlob *blob, GError *error, gpointer user_data) {
  DmFileUploadContext *ctx = (DmFileUploadContext *)user_data;

  if (error) {
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
    }
    upload_ctx_free(ctx);
    return;
  }

  if (!blob || !blob->url) {
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_UPLOAD_FAILED,
                               "Upload succeeded but no URL returned");
    if (ctx->callback) {
      ctx->callback(NULL, err, ctx->user_data);
    }
    g_error_free(err);
    if (blob) gnostr_blossom_blob_free(blob);
    upload_ctx_free(ctx);
    return;
  }

  /* Store upload URL and update encrypted hash from server response */
  ctx->attachment->upload_url = g_strdup(blob->url);
  if (blob->sha256) {
    g_free(ctx->attachment->encrypted_sha256);
    ctx->attachment->encrypted_sha256 = g_strdup(blob->sha256);
  }
  ctx->attachment->encrypted_size = blob->size;

  gnostr_blossom_blob_free(blob);

  /* Transfer ownership of attachment to callback */
  GnostrDmFileAttachment *result = ctx->attachment;
  ctx->attachment = NULL;

  if (ctx->callback) {
    ctx->callback(result, NULL, ctx->user_data);
  } else {
    gnostr_dm_file_attachment_free(result);
  }

  upload_ctx_free(ctx);
}

void gnostr_dm_file_encrypt_and_upload_async(const char *file_path,
                                              const char *mime_type,
                                              GnostrDmFileUploadCallback callback,
                                              gpointer user_data,
                                              GCancellable *cancellable) {
  if (!file_path) {
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_READ_FAILED,
                               "No file path provided");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Check if Blossom servers are configured */
  gsize n_servers = gnostr_blossom_settings_get_server_count();
  if (n_servers == 0) {
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_NO_SERVERS,
                               "No Blossom servers configured");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Read the file */
  GFile *file = g_file_new_for_path(file_path);
  GError *error = NULL;
  char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents(file, NULL, &contents, &length, NULL, &error)) {
    g_object_unref(file);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_READ_FAILED,
                               "Failed to read file: %s", error->message);
    g_clear_error(&error);
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }
  g_object_unref(file);

  /* Create attachment structure */
  GnostrDmFileAttachment *attachment = g_new0(GnostrDmFileAttachment, 1);
  attachment->original_path = g_strdup(file_path);
  attachment->mime_type = g_strdup(mime_type ? mime_type : gnostr_blossom_detect_mime_type(file_path));
  attachment->original_size = (gint64)length;
  attachment->original_sha256 = compute_sha256_hex((uint8_t *)contents, length);

  /* Generate random key and nonce */
  if (!gnostr_dm_file_random_bytes(attachment->key, GNOSTR_DM_FILES_AES_KEY_SIZE) ||
      !gnostr_dm_file_random_bytes(attachment->nonce, GNOSTR_DM_FILES_AES_NONCE_SIZE)) {
    g_free(contents);
    gnostr_dm_file_attachment_free(attachment);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_ENCRYPT_FAILED,
                               "Failed to generate random key/nonce");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Allocate buffer for ciphertext (plaintext + auth tag) */
  gsize ciphertext_alloc = length + GNOSTR_DM_FILES_AES_TAG_SIZE;
  uint8_t *ciphertext = g_malloc(ciphertext_alloc);
  gsize ciphertext_len = 0;

  /* Encrypt */
  if (!gnostr_dm_file_aes_gcm_encrypt((uint8_t *)contents, length,
                                       attachment->key, attachment->nonce,
                                       ciphertext, &ciphertext_len)) {
    g_free(contents);
    g_free(ciphertext);
    gnostr_dm_file_attachment_free(attachment);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_ENCRYPT_FAILED,
                               "AES-GCM encryption failed");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  g_free(contents);

  /* Compute SHA-256 of encrypted data */
  attachment->encrypted_sha256 = compute_sha256_hex(ciphertext, ciphertext_len);
  attachment->encrypted_size = (gint64)ciphertext_len;

  /* Write encrypted data to temp file for upload */
  char *temp_path = g_build_filename(g_get_tmp_dir(), "gnostr-dm-file-XXXXXX", NULL);
  int fd = g_mkstemp(temp_path);
  if (fd < 0) {
    g_free(ciphertext);
    g_free(temp_path);
    gnostr_dm_file_attachment_free(attachment);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_ENCRYPT_FAILED,
                               "Failed to create temp file for encrypted data");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  gssize written = write(fd, ciphertext, ciphertext_len);
  close(fd);
  g_free(ciphertext);

  if (written != (gssize)ciphertext_len) {
    g_unlink(temp_path);
    g_free(temp_path);
    gnostr_dm_file_attachment_free(attachment);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_ENCRYPT_FAILED,
                               "Failed to write encrypted data to temp file");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  /* Create upload context */
  DmFileUploadContext *ctx = g_new0(DmFileUploadContext, 1);
  ctx->file_path = g_strdup(file_path);
  ctx->mime_type = g_strdup(attachment->mime_type);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->attachment = attachment;
  ctx->temp_encrypted_path = temp_path;

  g_message("[DM_FILES] Encrypting and uploading file: %s (original=%s bytes, encrypted=%zu bytes)",
            file_path, attachment->original_sha256, (size_t)ciphertext_len);

  /* Upload encrypted file to Blossom with fallback */
  /* Use application/octet-stream for encrypted files regardless of original type */
  gnostr_blossom_upload_with_fallback_async(temp_path, "application/octet-stream",
                                             on_blossom_upload_complete, ctx,
                                             cancellable);
}

/* ---- Async download context ---- */

typedef struct {
  GnostrDmFileMessage *msg;
  GnostrDmFileDownloadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  uint8_t *key;
  gsize key_len;
  uint8_t *nonce;
  gsize nonce_len;
} DmFileDownloadContext;

static void download_ctx_free(DmFileDownloadContext *ctx) {
  if (!ctx) return;
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  if (ctx->key) {
    memset(ctx->key, 0, ctx->key_len);
    g_free(ctx->key);
  }
  if (ctx->nonce) {
    memset(ctx->nonce, 0, ctx->nonce_len);
    g_free(ctx->nonce);
  }
  /* Note: msg is not owned by context */
  g_free(ctx);
}

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>

static void on_download_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  DmFileDownloadContext *ctx = (DmFileDownloadContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                                 GNOSTR_DM_FILE_ERROR_DOWNLOAD_FAILED,
                                 "Download failed: %s", error->message);
      ctx->callback(NULL, 0, err, ctx->user_data);
      g_error_free(err);
    }
    g_clear_error(&error);
    download_ctx_free(ctx);
    return;
  }

  gsize ciphertext_len = 0;
  const uint8_t *ciphertext = g_bytes_get_data(bytes, &ciphertext_len);

  /* Verify SHA-256 of encrypted file */
  if (ctx->msg->encrypted_hash) {
    char *computed_hash = compute_sha256_hex(ciphertext, ciphertext_len);
    if (g_ascii_strcasecmp(computed_hash, ctx->msg->encrypted_hash) != 0) {
      g_free(computed_hash);
      g_bytes_unref(bytes);
      GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                                 GNOSTR_DM_FILE_ERROR_HASH_MISMATCH,
                                 "Encrypted file hash mismatch");
      if (ctx->callback) {
        ctx->callback(NULL, 0, err, ctx->user_data);
      }
      g_error_free(err);
      download_ctx_free(ctx);
      return;
    }
    g_free(computed_hash);
  }

  /* Decrypt */
  if (ciphertext_len < GNOSTR_DM_FILES_AES_TAG_SIZE) {
    g_bytes_unref(bytes);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_DECRYPT_FAILED,
                               "Encrypted data too short");
    if (ctx->callback) {
      ctx->callback(NULL, 0, err, ctx->user_data);
    }
    g_error_free(err);
    download_ctx_free(ctx);
    return;
  }

  gsize plaintext_alloc = ciphertext_len - GNOSTR_DM_FILES_AES_TAG_SIZE;
  uint8_t *plaintext = g_malloc(plaintext_alloc);
  gsize plaintext_len = 0;

  if (!gnostr_dm_file_aes_gcm_decrypt(ciphertext, ciphertext_len,
                                       ctx->key, ctx->nonce,
                                       plaintext, &plaintext_len)) {
    g_free(plaintext);
    g_bytes_unref(bytes);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_AUTH_FAILED,
                               "AES-GCM decryption/authentication failed");
    if (ctx->callback) {
      ctx->callback(NULL, 0, err, ctx->user_data);
    }
    g_error_free(err);
    download_ctx_free(ctx);
    return;
  }

  g_bytes_unref(bytes);

  /* Verify SHA-256 of original file */
  if (ctx->msg->original_hash) {
    char *computed_hash = compute_sha256_hex(plaintext, plaintext_len);
    if (g_ascii_strcasecmp(computed_hash, ctx->msg->original_hash) != 0) {
      g_free(computed_hash);
      g_free(plaintext);
      GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                                 GNOSTR_DM_FILE_ERROR_HASH_MISMATCH,
                                 "Decrypted file hash mismatch");
      if (ctx->callback) {
        ctx->callback(NULL, 0, err, ctx->user_data);
      }
      g_error_free(err);
      download_ctx_free(ctx);
      return;
    }
    g_free(computed_hash);
  }

  /* Success - transfer ownership of plaintext to callback */
  if (ctx->callback) {
    ctx->callback(plaintext, plaintext_len, NULL, ctx->user_data);
  } else {
    g_free(plaintext);
  }

  download_ctx_free(ctx);
}

void gnostr_dm_file_download_and_decrypt_async(GnostrDmFileMessage *msg,
                                                 GnostrDmFileDownloadCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable) {
  if (!msg || !msg->file_url) {
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_INVALID_MESSAGE,
                               "Invalid file message or missing URL");
    if (callback) callback(NULL, 0, err, user_data);
    g_error_free(err);
    return;
  }

  if (!msg->decryption_key_b64 || !msg->decryption_nonce_b64) {
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_INVALID_MESSAGE,
                               "Missing decryption key or nonce");
    if (callback) callback(NULL, 0, err, user_data);
    g_error_free(err);
    return;
  }

  /* Decode key and nonce from base64 */
  gsize key_len = 0, nonce_len = 0;
  uint8_t *key = from_base64(msg->decryption_key_b64, &key_len);
  uint8_t *nonce = from_base64(msg->decryption_nonce_b64, &nonce_len);

  if (!key || key_len != GNOSTR_DM_FILES_AES_KEY_SIZE ||
      !nonce || nonce_len != GNOSTR_DM_FILES_AES_NONCE_SIZE) {
    g_free(key);
    g_free(nonce);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_INVALID_MESSAGE,
                               "Invalid decryption key or nonce length");
    if (callback) callback(NULL, 0, err, user_data);
    g_error_free(err);
    return;
  }

  /* Create download context */
  DmFileDownloadContext *ctx = g_new0(DmFileDownloadContext, 1);
  ctx->msg = msg;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  ctx->key = key;
  ctx->key_len = key_len;
  ctx->nonce = nonce;
  ctx->nonce_len = nonce_len;

  /* Download the file */
  SoupSession *session = soup_session_new();
  soup_session_set_timeout(session, 60);

  SoupMessage *soup_msg = soup_message_new("GET", msg->file_url);
  if (!soup_msg) {
    g_object_unref(session);
    download_ctx_free(ctx);
    GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                               GNOSTR_DM_FILE_ERROR_DOWNLOAD_FAILED,
                               "Invalid download URL");
    if (callback) callback(NULL, 0, err, user_data);
    g_error_free(err);
    return;
  }

  g_message("[DM_FILES] Downloading and decrypting file from: %s", msg->file_url);

  soup_session_send_and_read_async(session, soup_msg, G_PRIORITY_DEFAULT,
                                    ctx->cancellable, on_download_complete, ctx);

  g_object_unref(soup_msg);
  g_object_unref(session);
}

#else /* !HAVE_SOUP3 */

void gnostr_dm_file_download_and_decrypt_async(GnostrDmFileMessage *msg,
                                                 GnostrDmFileDownloadCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable) {
  (void)msg; (void)cancellable;
  GError *err = g_error_new(GNOSTR_DM_FILE_ERROR,
                             GNOSTR_DM_FILE_ERROR_DOWNLOAD_FAILED,
                             "File download requires libsoup3");
  if (callback) callback(NULL, 0, err, user_data);
  g_error_free(err);
}

#endif /* HAVE_SOUP3 */

char *gnostr_dm_file_build_rumor_json(const char *sender_pubkey,
                                       const char *recipient_pubkey,
                                       GnostrDmFileAttachment *attachment,
                                       gint64 created_at) {
  if (!sender_pubkey || !recipient_pubkey || !attachment || !attachment->upload_url) {
    return NULL;
  }

  if (created_at == 0) {
    created_at = (gint64)time(NULL);
  }

  /* Encode key and nonce as base64 */
  char *key_b64 = to_base64(attachment->key, GNOSTR_DM_FILES_AES_KEY_SIZE);
  char *nonce_b64 = to_base64(attachment->nonce, GNOSTR_DM_FILES_AES_NONCE_SIZE);

  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* pubkey */
  json_builder_set_member_name(builder, "pubkey");
  json_builder_add_string_value(builder, sender_pubkey);

  /* created_at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, created_at);

  /* kind 15 */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, 15);

  /* content: the file URL */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, attachment->upload_url);

  /* tags array */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* p tag for recipient */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "p");
  json_builder_add_string_value(builder, recipient_pubkey);
  json_builder_end_array(builder);

  /* file-type tag */
  if (attachment->mime_type) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "file-type");
    json_builder_add_string_value(builder, attachment->mime_type);
    json_builder_end_array(builder);
  }

  /* encryption-algorithm tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "encryption-algorithm");
  json_builder_add_string_value(builder, "aes-gcm");
  json_builder_end_array(builder);

  /* decryption-key tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "decryption-key");
  json_builder_add_string_value(builder, key_b64);
  json_builder_end_array(builder);

  /* decryption-nonce tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "decryption-nonce");
  json_builder_add_string_value(builder, nonce_b64);
  json_builder_end_array(builder);

  /* x tag: SHA-256 of encrypted file */
  if (attachment->encrypted_sha256) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "x");
    json_builder_add_string_value(builder, attachment->encrypted_sha256);
    json_builder_end_array(builder);
  }

  /* ox tag: SHA-256 of original file */
  if (attachment->original_sha256) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "ox");
    json_builder_add_string_value(builder, attachment->original_sha256);
    json_builder_end_array(builder);
  }

  /* size tag */
  if (attachment->encrypted_size > 0) {
    char size_str[32];
    g_snprintf(size_str, sizeof(size_str), "%" G_GINT64_FORMAT, attachment->encrypted_size);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "size");
    json_builder_add_string_value(builder, size_str);
    json_builder_end_array(builder);
  }

  /* dim tag (for images) */
  if (attachment->width > 0 && attachment->height > 0) {
    char dim_str[32];
    g_snprintf(dim_str, sizeof(dim_str), "%dx%d", attachment->width, attachment->height);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "dim");
    json_builder_add_string_value(builder, dim_str);
    json_builder_end_array(builder);
  }

  /* blurhash tag */
  if (attachment->blurhash) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "blurhash");
    json_builder_add_string_value(builder, attachment->blurhash);
    json_builder_end_array(builder);
  }

  /* thumb tag */
  if (attachment->thumb_url) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "thumb");
    json_builder_add_string_value(builder, attachment->thumb_url);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);  /* End tags */

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  char *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);
  g_free(key_b64);
  g_free(nonce_b64);

  return json_str;
}

GnostrDmFileMessage *gnostr_dm_file_parse_message(const char *event_json) {
  if (!event_json) return NULL;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind 15 */
  if (!json_object_has_member(obj, "kind") ||
      json_object_get_int_member(obj, "kind") != 15) {
    g_object_unref(parser);
    return NULL;
  }

  GnostrDmFileMessage *msg = g_new0(GnostrDmFileMessage, 1);

  /* pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    msg->sender_pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* created_at */
  if (json_object_has_member(obj, "created_at")) {
    msg->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* content (file URL) */
  if (json_object_has_member(obj, "content")) {
    msg->file_url = g_strdup(json_object_get_string_member(obj, "content"));
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint tags_len = json_array_get_length(tags);
    GPtrArray *fallbacks = NULL;

    for (guint i = 0; i < tags_len; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      const char *tag_value = json_array_get_string_element(tag, 1);

      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "file-type") == 0) {
        msg->file_type = g_strdup(tag_value);
      } else if (strcmp(tag_name, "encryption-algorithm") == 0) {
        msg->encryption_algorithm = g_strdup(tag_value);
      } else if (strcmp(tag_name, "decryption-key") == 0) {
        msg->decryption_key_b64 = g_strdup(tag_value);
      } else if (strcmp(tag_name, "decryption-nonce") == 0) {
        msg->decryption_nonce_b64 = g_strdup(tag_value);
      } else if (strcmp(tag_name, "x") == 0) {
        msg->encrypted_hash = g_strdup(tag_value);
      } else if (strcmp(tag_name, "ox") == 0) {
        msg->original_hash = g_strdup(tag_value);
      } else if (strcmp(tag_name, "size") == 0) {
        msg->size = g_ascii_strtoll(tag_value, NULL, 10);
      } else if (strcmp(tag_name, "dim") == 0) {
        /* Parse WxH format */
        int w = 0, h = 0;
        if (sscanf(tag_value, "%dx%d", &w, &h) == 2) {
          msg->width = w;
          msg->height = h;
        }
      } else if (strcmp(tag_name, "blurhash") == 0) {
        msg->blurhash = g_strdup(tag_value);
      } else if (strcmp(tag_name, "thumb") == 0) {
        msg->thumb_url = g_strdup(tag_value);
      } else if (strcmp(tag_name, "fallback") == 0) {
        if (!fallbacks) {
          fallbacks = g_ptr_array_new();
        }
        g_ptr_array_add(fallbacks, g_strdup(tag_value));
      }
    }

    /* Convert fallbacks to NULL-terminated array */
    if (fallbacks) {
      g_ptr_array_add(fallbacks, NULL);
      msg->fallback_urls = (char **)g_ptr_array_free(fallbacks, FALSE);
    }
  }

  g_object_unref(parser);
  return msg;
}
