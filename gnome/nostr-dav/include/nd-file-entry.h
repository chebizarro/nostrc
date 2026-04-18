/* nd-file-entry.h - WebDAV file metadata backed by NIP-94 (kind 1063)
 *
 * SPDX-License-Identifier: MIT
 *
 * Represents a file in both WebDAV and Nostr domains. The struct holds
 * the raw content bytes for local serving (in-memory MVP) alongside
 * NIP-94-compatible metadata (sha256, mime, size, path).
 */
#ifndef ND_FILE_ENTRY_H
#define ND_FILE_ENTRY_H

#include <glib.h>

G_BEGIN_DECLS

/* NIP-94 file metadata event kind */
#define ND_NIP94_KIND 1063

/**
 * NdFileEntry:
 *
 * Represents a file in the WebDAV /files/ collection.
 */
typedef struct {
  gchar  *path;          /* Relative path within /files/ (e.g. "notes/todo.txt") */
  gchar  *sha256;        /* Hex-encoded SHA-256 of content */
  gchar  *mime_type;     /* MIME type (e.g. "text/plain") */
  gsize   size;          /* Content length in bytes */

  GBytes *content;       /* Raw file content (owned) */

  /* NIP-94 / Nostr metadata */
  gchar  *blossom_url;   /* Blossom URL if uploaded (nullable) */
  gchar  *pubkey;        /* Creator pubkey (hex, nullable) */
  gint64  created_at;    /* Event created_at */

  /* Display metadata */
  gchar  *display_name;  /* Filename component of path */
  gint64  modified_at;   /* Last modified timestamp */
} NdFileEntry;

/**
 * nd_file_entry_free:
 * @entry: (transfer full): entry to free
 */
void nd_file_entry_free(NdFileEntry *entry);

/**
 * nd_file_entry_new:
 * @path: relative file path
 * @content: (transfer full): file content bytes
 * @mime_type: (nullable): MIME type, guessed if NULL
 *
 * Creates a new file entry, computing SHA-256 from content.
 *
 * Returns: (transfer full): new file entry.
 */
NdFileEntry *nd_file_entry_new(const gchar *path,
                               GBytes      *content,
                               const gchar *mime_type);

/**
 * nd_file_entry_compute_etag:
 * @entry: the file entry
 *
 * Returns: (transfer full): ETag string (quoted, based on SHA-256).
 */
gchar *nd_file_entry_compute_etag(const NdFileEntry *entry);

/**
 * nd_file_entry_to_nip94_json:
 * @entry: the file entry
 *
 * Builds unsigned NIP-94 (kind 1063) event JSON from a file entry.
 * Tags: ["url", blossom_url], ["x", sha256], ["m", mime], ["size", bytes],
 *       ["path", relative_path].
 *
 * Returns: (transfer full) (nullable): JSON string or NULL on error.
 */
gchar *nd_file_entry_to_nip94_json(const NdFileEntry *entry);

/**
 * nd_file_entry_from_nip94_json:
 * @json_str: NIP-94 event JSON
 * @error: (out) (optional): location for error
 *
 * Parses a NIP-94 file metadata event (kind 1063) into an NdFileEntry.
 * Note: content is NOT populated — caller must fetch from Blossom.
 *
 * Returns: (transfer full) (nullable): parsed entry or NULL on error.
 */
NdFileEntry *nd_file_entry_from_nip94_json(const gchar *json_str,
                                           GError     **error);

G_END_DECLS
#endif /* ND_FILE_ENTRY_H */
