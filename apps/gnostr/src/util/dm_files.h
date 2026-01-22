/**
 * GnostrDmFiles - NIP-17 File Message (Kind 15) Support
 *
 * Provides encrypted file attachment support for direct messages:
 * - AES-GCM encryption of files before upload
 * - Upload to Blossom servers
 * - Kind 15 event creation with proper metadata tags
 * - Decryption and display of received file messages
 *
 * Per NIP-17, kind 15 file messages are wrapped in the same gift wrap
 * structure as kind 14 text messages.
 */

#ifndef GNOSTR_DM_FILES_H
#define GNOSTR_DM_FILES_H

#include <glib-object.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * AES-GCM encryption parameters
 */
#define GNOSTR_DM_FILES_AES_KEY_SIZE    32  /* 256 bits */
#define GNOSTR_DM_FILES_AES_NONCE_SIZE  12  /* 96 bits */
#define GNOSTR_DM_FILES_AES_TAG_SIZE    16  /* 128 bits */

/**
 * GnostrDmFileAttachment:
 *
 * Represents an encrypted file attachment ready to be sent via kind 15 event.
 */
typedef struct {
  /* Original file info */
  char *original_path;        /* Local path to original file */
  char *mime_type;            /* MIME type (e.g., "image/jpeg") */
  gint64 original_size;       /* Size of original file in bytes */
  char *original_sha256;      /* SHA-256 of original file (hex) */

  /* Encryption parameters */
  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];     /* AES-256 key */
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_SIZE]; /* GCM nonce */

  /* Encrypted file info */
  char *encrypted_sha256;     /* SHA-256 of encrypted file (hex) */
  gint64 encrypted_size;      /* Size of encrypted file in bytes */
  char *upload_url;           /* URL after upload to Blossom */

  /* Optional metadata (for images) */
  int width;                  /* Image width (0 if unknown) */
  int height;                 /* Image height (0 if unknown) */
  char *blurhash;             /* Blurhash string (optional) */

  /* Thumbnail (optional, for images) */
  char *thumb_url;            /* Encrypted thumbnail URL */
  char *thumb_sha256;         /* SHA-256 of encrypted thumbnail */
} GnostrDmFileAttachment;

/**
 * GnostrDmFileMessage:
 *
 * Represents a received kind 15 file message (decrypted from gift wrap).
 */
typedef struct {
  char *sender_pubkey;        /* Sender's public key (hex) */
  gint64 created_at;          /* Timestamp of the message */

  /* File info from tags */
  char *file_url;             /* URL of encrypted file (from content) */
  char *file_type;            /* MIME type from file-type tag */
  char *encryption_algorithm; /* Should be "aes-gcm" */
  char *decryption_key_b64;   /* Base64-encoded decryption key */
  char *decryption_nonce_b64; /* Base64-encoded decryption nonce */
  char *encrypted_hash;       /* SHA-256 of encrypted file (x tag) */
  char *original_hash;        /* SHA-256 of original file (ox tag) */
  gint64 size;                /* File size in bytes */

  /* Optional image metadata */
  int width;
  int height;
  char *blurhash;
  char *thumb_url;

  /* Fallback URLs */
  char **fallback_urls;       /* NULL-terminated array of fallback URLs */
} GnostrDmFileMessage;

/**
 * Free a file attachment structure.
 */
void gnostr_dm_file_attachment_free(GnostrDmFileAttachment *attachment);

/**
 * Free a file message structure.
 */
void gnostr_dm_file_message_free(GnostrDmFileMessage *msg);

/**
 * Callback for async file encryption/upload completion.
 *
 * @param attachment The encrypted and uploaded attachment (NULL on error)
 * @param error Error details if operation failed (NULL on success)
 * @param user_data User-provided data
 */
typedef void (*GnostrDmFileUploadCallback)(GnostrDmFileAttachment *attachment,
                                            GError *error,
                                            gpointer user_data);

/**
 * Callback for async file download/decryption completion.
 *
 * @param decrypted_data The decrypted file data (NULL on error)
 * @param decrypted_size Size of decrypted data in bytes
 * @param error Error details if operation failed (NULL on success)
 * @param user_data User-provided data
 */
typedef void (*GnostrDmFileDownloadCallback)(uint8_t *decrypted_data,
                                               gsize decrypted_size,
                                               GError *error,
                                               gpointer user_data);

/**
 * Encrypt and upload a file for sending as a DM attachment.
 *
 * This function:
 * 1. Generates random AES-256 key and 96-bit nonce
 * 2. Encrypts the file using AES-GCM
 * 3. Computes SHA-256 of both original and encrypted files
 * 4. Uploads encrypted file to Blossom server(s) with fallback
 * 5. Returns attachment structure with all metadata for kind 15 event
 *
 * @param file_path Path to the file to attach
 * @param mime_type MIME type (NULL to auto-detect)
 * @param callback Callback when upload completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_dm_file_encrypt_and_upload_async(const char *file_path,
                                              const char *mime_type,
                                              GnostrDmFileUploadCallback callback,
                                              gpointer user_data,
                                              GCancellable *cancellable);

/**
 * Download and decrypt a file from a kind 15 message.
 *
 * This function:
 * 1. Downloads encrypted file from URL (tries fallbacks if needed)
 * 2. Verifies SHA-256 matches the x tag
 * 3. Decrypts using AES-GCM with key/nonce from tags
 * 4. Verifies decrypted SHA-256 matches ox tag
 * 5. Returns decrypted file data
 *
 * @param msg The parsed file message
 * @param callback Callback when download completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_dm_file_download_and_decrypt_async(GnostrDmFileMessage *msg,
                                                 GnostrDmFileDownloadCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable);

/**
 * Build a kind 15 file message event JSON (unsigned rumor).
 *
 * The returned JSON can be used to create the rumor for gift wrapping.
 *
 * @param sender_pubkey Sender's public key (hex)
 * @param recipient_pubkey Recipient's public key (hex)
 * @param attachment The encrypted attachment with all metadata
 * @param created_at Timestamp (0 for current time)
 * @return Newly allocated JSON string (caller frees), or NULL on error
 */
char *gnostr_dm_file_build_rumor_json(const char *sender_pubkey,
                                       const char *recipient_pubkey,
                                       GnostrDmFileAttachment *attachment,
                                       gint64 created_at);

/**
 * Parse a kind 15 event into a file message structure.
 *
 * @param event_json JSON string of the kind 15 event
 * @return Parsed file message (caller frees), or NULL on error
 */
GnostrDmFileMessage *gnostr_dm_file_parse_message(const char *event_json);

/**
 * Encrypt data using AES-256-GCM.
 *
 * @param plaintext Input data to encrypt
 * @param plaintext_len Length of input data
 * @param key 32-byte AES key
 * @param nonce 12-byte GCM nonce
 * @param ciphertext Output buffer (must be plaintext_len + 16 bytes)
 * @param ciphertext_len Output: actual ciphertext length
 * @return TRUE on success, FALSE on error
 */
gboolean gnostr_dm_file_aes_gcm_encrypt(const uint8_t *plaintext,
                                         gsize plaintext_len,
                                         const uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE],
                                         const uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_SIZE],
                                         uint8_t *ciphertext,
                                         gsize *ciphertext_len);

/**
 * Decrypt data using AES-256-GCM.
 *
 * @param ciphertext Input encrypted data (includes auth tag)
 * @param ciphertext_len Length of ciphertext
 * @param key 32-byte AES key
 * @param nonce 12-byte GCM nonce
 * @param plaintext Output buffer (must be at least ciphertext_len - 16 bytes)
 * @param plaintext_len Output: actual plaintext length
 * @return TRUE on success (authentication passed), FALSE on error
 */
gboolean gnostr_dm_file_aes_gcm_decrypt(const uint8_t *ciphertext,
                                         gsize ciphertext_len,
                                         const uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE],
                                         const uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_SIZE],
                                         uint8_t *plaintext,
                                         gsize *plaintext_len);

/**
 * Generate cryptographically secure random bytes.
 *
 * @param buffer Output buffer
 * @param len Number of random bytes to generate
 * @return TRUE on success, FALSE on error
 */
gboolean gnostr_dm_file_random_bytes(uint8_t *buffer, gsize len);

/**
 * Error domain for DM file operations
 */
#define GNOSTR_DM_FILE_ERROR (gnostr_dm_file_error_quark())
GQuark gnostr_dm_file_error_quark(void);

typedef enum {
  GNOSTR_DM_FILE_ERROR_READ_FAILED,
  GNOSTR_DM_FILE_ERROR_ENCRYPT_FAILED,
  GNOSTR_DM_FILE_ERROR_DECRYPT_FAILED,
  GNOSTR_DM_FILE_ERROR_UPLOAD_FAILED,
  GNOSTR_DM_FILE_ERROR_DOWNLOAD_FAILED,
  GNOSTR_DM_FILE_ERROR_HASH_MISMATCH,
  GNOSTR_DM_FILE_ERROR_AUTH_FAILED,
  GNOSTR_DM_FILE_ERROR_INVALID_MESSAGE,
  GNOSTR_DM_FILE_ERROR_NO_SERVERS
} GnostrDmFileError;

G_END_DECLS

#endif /* GNOSTR_DM_FILES_H */
