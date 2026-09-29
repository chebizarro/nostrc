#ifndef GH_NIP17_FILE_H
#define GH_NIP17_FILE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * The metadata of a NIP-17 kind-15 file message (privacy charter §6, G21).
 * The rumor's content is the URL of the encrypted file and its tags say how
 * to open it:
 *
 *   ["file-type", "<MIME type>"]            what the sender says it is
 *   ["encryption-algorithm", "aes-gcm"]     the only algorithm accepted
 *   ["decryption-key", "<64 hex>"]          AES-256 key
 *   ["decryption-nonce", "<24 or 32 hex>"]  GCM nonce: 12 bytes (what Groundhog
 *                                           writes) or 16 (Amethyst, 0xchat)
 *   ["x", "<64 hex>"]                       SHA-256 of the encrypted file
 *   ["ox", "<64 hex>"]                      SHA-256 of the file (optional)
 *   ["size", "<bytes>"]                     of the encrypted file (optional)
 *   ["dim", "<w>x<h>"]                      pixels, for images (optional)
 *
 * NIP-17 names no encoding for the key and nonce; Amethyst, 0xchat, gnostr
 * and nostr-share all write lowercase hex, and so does Groundhog. blurhash,
 * thumb and fallback are not written (charter §6: none in v1) and are
 * ignored when read, as are the p, e, subject and expiration tags, which
 * belong to the message itself. The ciphertext is AES-256-GCM output with the
 * 16-byte tag appended (what WebCrypto, Amethyst and 0xchat produce).
 *
 * GTK-free and network-free: reading a file message never fetches it
 * (charter PD-2, AT-7).
 */

#define GH_NIP17_FILE_KIND        15
#define GH_NIP17_FILE_KEY_SIZE    32
#define GH_NIP17_FILE_NONCE_SIZE  12 /* what Groundhog writes */
#define GH_NIP17_FILE_NONCE_MAX   16 /* also read: Amethyst's and 0xchat's */
#define GH_NIP17_FILE_TAG_SIZE    16
#define GH_NIP17_FILE_MAX_URL     2048
#define GH_NIP17_FILE_MAX_TYPE    127

typedef struct {
  gchar *url;          /* the rumor content: where the ciphertext is */
  gchar *file_type;    /* the declared MIME type (lowercase); never trusted to
                        * decide how bytes are decoded (magic bytes do) */
  guint8 key[GH_NIP17_FILE_KEY_SIZE];
  guint8 nonce[GH_NIP17_FILE_NONCE_MAX];
  gsize nonce_size;    /* 12 or 16 */
  gchar x[65];         /* lowercase hex SHA-256 of the ciphertext */
  gchar ox[65];        /* of the plaintext; "" when the sender gave none */
  guint64 size;        /* size tag, 0 when absent */
  guint width;         /* dim tag, 0 when absent */
  guint height;
} GhNip17File;

/* A copy (the key and nonce too). */
GhNip17File *gh_nip17_file_copy(const GhNip17File *file);
/* Wipes the key and nonce before freeing. */
void gh_nip17_file_free(GhNip17File *file);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip17File, gh_nip17_file_free)

/* The file of an unsigned kind-15 rumor (JSON), validated as described
 * above: exactly one of each required tag, well-formed values, an http(s)
 * URL with a host and no user info. G_IO_ERROR_INVALID_DATA otherwise
 * (G_IO_ERROR_NOT_SUPPORTED for an encryption algorithm other than aes-gcm).
 * Does not check the rumor's id or recipients (the unwrap does). */
GhNip17File *gh_nip17_file_from_rumor(const gchar *rumor_json, GError **error);

/* The file tags of a kind-15 rumor for file (complete: URL, type, key, a 12-
 * or 16-byte nonce and x), in their canonical order: a GPtrArray of
 * NULL-terminated (key, value) GStrv pairs, wiped when freed (they hold the
 * key). G_IO_ERROR_INVALID_ARGUMENT for an incomplete file. The one list
 * both rumor builders use (this file's and gh_nip17_rumor_new_file_room() in
 * gh-nip17-envelope.h, for rooms). */
GPtrArray *gh_nip17_file_dup_tags(const GhNip17File *file, GError **error);

/* The canonical unsigned kind-15 rumor from sender to recipient (a note to
 * self when equal) carrying file, created at created_at (> 0), with
 * ["expiration", expires_at] when expires_at is not 0 (NIP-40, charter §3.7;
 * later than created_at). file must be complete: URL, type, key, a 12- or
 * 16-byte nonce and x. @out_rumor_id (nullable) receives the rumor id.
 * G_IO_ERROR_INVALID_ARGUMENT otherwise. The same shape as
 * gh_nip17_rumor_new_expiring(), so the outbox seals it the same way. */
gchar *gh_nip17_file_rumor_new(const gchar *sender_pubkey_hex, const gchar *recipient_pubkey_hex,
                               const GhNip17File *file, gint64 created_at, gint64 expires_at,
                               gchar **out_rumor_id, GError **error);

/* "type/subtype", lowercased, of a MIME type (parameters such as
 * ";charset=..." dropped), or NULL when value is not one. */
gchar *gh_nip17_file_normalize_type(const gchar *value);

/* TRUE for "image/..." (the file may be a photo; the magic bytes decide). */
gboolean gh_nip17_file_is_image(const GhNip17File *file);

G_END_DECLS
#endif
