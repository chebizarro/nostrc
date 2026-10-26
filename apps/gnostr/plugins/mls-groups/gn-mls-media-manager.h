/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-media-manager.h - MLS Encrypted Media Manager (MIP-04)
 *
 * Handles encrypted media upload and download for MLS group messages.
 *
 * Upload flow:
 *   1. Read file bytes
 *   2. marmot_media_encrypt() → ciphertext + v2 reference + source epoch
 *   3. Upload encrypted blob to Blossom server
 *   4. Return imeta tag array for inclusion in the message event
 *
 * Download flow:
 *   1. Parse imeta tag from message event
 *   2. Download encrypted blob from Blossom URL
 *   3. marmot_media_decrypt() → plaintext bytes
 *   4. Return GBytes for display
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_MEDIA_MANAGER_H
#define GN_MLS_MEDIA_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include <marmot/marmot-media.h>
#include "gn-marmot-service.h"

/* Forward declaration */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_MLS_MEDIA_MANAGER (gn_mls_media_manager_get_type())
G_DECLARE_FINAL_TYPE(GnMlsMediaManager, gn_mls_media_manager,
                     GN, MLS_MEDIA_MANAGER, GObject)

/**
 * GnMlsMediaUploadResult:
 * @blossom_url: URL of the uploaded encrypted blob
 * @imeta_tags: NULL-terminated array of imeta tag strings for the message event
 * @source_epoch: Epoch in which the carrying message must be created
 *
 * Result of a successful media upload.
 */
typedef struct {
  gchar  *blossom_url;
  gchar **imeta_tags;   /* NULL-terminated encrypted-media-v2 fields */
  guint64 source_epoch; /* carrying message must be sent in this epoch */
} GnMlsMediaUploadResult;

void gn_mls_media_upload_result_free(GnMlsMediaUploadResult *result);

/**
 * gn_mls_media_manager_new:
 * @service: The marmot service
 * @plugin_context: The plugin context (borrowed)
 * @blossom_server_url: (nullable): Blossom server URL, or NULL for default
 *
 * Returns: (transfer full): A new #GnMlsMediaManager
 */
GnMlsMediaManager *gn_mls_media_manager_new(GnMarmotService     *service,
                                               GnostrPluginContext *plugin_context,
                                               const gchar         *blossom_server_url);

/**
 * gn_mls_media_manager_upload_async:
 * @self: The manager
 * @group_id_hex: MLS group ID (used to derive encryption key)
 * @file: The file to encrypt and upload
 * @cancellable: (nullable): a #GCancellable
 * @callback: Completion callback
 * @user_data: User data
 *
 * Encrypts @file using the group's MLS exporter secret and uploads
 * the ciphertext to the configured Blossom server.
 */
void gn_mls_media_manager_upload_async(GnMlsMediaManager   *self,
                                         const gchar          *group_id_hex,
                                         GFile                *file,
                                         GCancellable         *cancellable,
                                         GAsyncReadyCallback   callback,
                                         gpointer              user_data);

/**
 * gn_mls_media_manager_upload_finish:
 * @self: The manager
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): Upload result, or NULL on error.
 *   Free with gn_mls_media_upload_result_free().
 */
GnMlsMediaUploadResult *gn_mls_media_manager_upload_finish(GnMlsMediaManager *self,
                                                              GAsyncResult       *result,
                                                              GError            **error);

/**
 * gn_mls_media_manager_download_async:
 * @self: The manager
 * @group_id_hex: MLS group ID (used to derive decryption key)
 * @imeta_tag_json: Complete encrypted-media-v2 imeta JSON array
 * @source_epoch: Epoch of the decrypted carrying message (not the current epoch)
 * @cancellable: (nullable): a #GCancellable
 * @callback: Completion callback
 * @user_data: User data
 *
 * Downloads and decrypts a media blob from a Blossom server.
 */
void gn_mls_media_manager_download_async(GnMlsMediaManager   *self,
                                           const gchar          *group_id_hex,
                                           const gchar          *imeta_tag_json,
                                           guint64               source_epoch,
                                           GCancellable         *cancellable,
                                           GAsyncReadyCallback   callback,
                                           gpointer              user_data);

/**
 * gn_mls_media_manager_download_finish:
 * @self: The manager
 * @result: a #GAsyncResult
 * @out_content_type: (out) (transfer full) (nullable): Authenticated canonical MIME type
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): Decrypted file bytes, or NULL on error
 */
GBytes *gn_mls_media_manager_download_finish(GnMlsMediaManager *self,
                                               GAsyncResult       *result,
                                               gchar             **out_content_type,
                                               GError            **error);

/**
 * gn_mls_media_manager_parse_imeta:
 * @imeta_tag_json: JSON array string of the imeta tag value
 * @out: (out): Validated reference; clear with marmot_media_reference_clear()
 * @error: (nullable): return location for a #GError
 *
 * Parses an imeta tag from a group message event.
 *
 * Returns: TRUE if the tag was successfully parsed
 */
gboolean gn_mls_media_manager_parse_imeta(const gchar *imeta_tag_json,
                                            MarmotMediaReference *out,
                                            GError **error);

/* Call in the same uninterrupted turn as message creation, with no Commit
 * processed between this check and creation. A stale upload must be redone.
 * Upload completion also checks this, but the caller may delay sending. */
gboolean gn_mls_media_manager_check_epoch(GnMlsMediaManager *self,
                                            const gchar *group_id_hex,
                                            guint64 source_epoch,
                                            GError **error);

G_END_DECLS

#endif /* GN_MLS_MEDIA_MANAGER_H */
