/**
 * @file media_upload.h
 * @brief Unified media upload API (Blossom + NIP-96 fallback)
 *
 * Provides a single entry point for media uploads that tries Blossom
 * servers first, then falls back to NIP-96 if all Blossom servers fail
 * or none are configured.
 *
 * nostrc-fs5g: NIP-96 file storage upload support.
 */

#ifndef GNOSTR_MEDIA_UPLOAD_H
#define GNOSTR_MEDIA_UPLOAD_H

#include "blossom.h"  /* GnostrBlossomBlob, GnostrBlossomUploadCallback */

G_BEGIN_DECLS

/**
 * Upload a file using the best available media server protocol.
 *
 * Strategy:
 * 1. Try Blossom servers from the user's configured list (kind 10063)
 * 2. If all Blossom servers fail (or none configured), try NIP-96 fallback
 * 3. Return the first successful result
 *
 * Uses the same callback/result types as Blossom for full compatibility.
 *
 * @param file_path Path to the file to upload
 * @param mime_type MIME type of the file, or NULL to auto-detect
 * @param callback Callback when upload completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_media_upload_async(const char *file_path,
                                const char *mime_type,
                                GnostrBlossomUploadCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable);

G_END_DECLS

#endif /* GNOSTR_MEDIA_UPLOAD_H */
