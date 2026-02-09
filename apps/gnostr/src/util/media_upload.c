/**
 * @file media_upload.c
 * @brief Unified media upload API (Blossom + NIP-96 fallback)
 *
 * Tries Blossom servers first, then falls back to NIP-96 if all
 * Blossom servers fail or none are configured.
 *
 * nostrc-fs5g: NIP-96 file storage upload support.
 */

#include "media_upload.h"
#include "blossom.h"
#include "blossom_settings.h"
#include "nip96.h"
#include <string.h>

/* Default NIP-96 fallback server */
#define NIP96_DEFAULT_SERVER "https://nostr.build"

typedef struct {
  char *file_path;
  char *mime_type;
  GnostrBlossomUploadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} MediaUploadContext;

static void media_upload_ctx_free(MediaUploadContext *ctx)
{
  if (!ctx) return;
  g_free(ctx->file_path);
  g_free(ctx->mime_type);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

static void on_nip96_fallback_complete(GnostrBlossomBlob *blob,
                                        GError *error,
                                        gpointer user_data)
{
  MediaUploadContext *ctx = (MediaUploadContext *)user_data;

  /* Pass through to caller - NIP-96 is the last resort */
  if (ctx->callback) {
    ctx->callback(blob, error, ctx->user_data);
  } else if (blob) {
    gnostr_blossom_blob_free(blob);
  }

  media_upload_ctx_free(ctx);
}

static void try_nip96_fallback(MediaUploadContext *ctx)
{
  g_message("media_upload: Blossom failed or no servers, trying NIP-96 fallback (%s)",
            NIP96_DEFAULT_SERVER);

  gnostr_nip96_upload_async(NIP96_DEFAULT_SERVER,
                             ctx->file_path,
                             ctx->mime_type,
                             on_nip96_fallback_complete,
                             ctx,
                             ctx->cancellable);
}

static void on_blossom_attempt_complete(GnostrBlossomBlob *blob,
                                         GError *error,
                                         gpointer user_data)
{
  MediaUploadContext *ctx = (MediaUploadContext *)user_data;

  if (blob && !error) {
    /* Blossom succeeded */
    if (ctx->callback) {
      ctx->callback(blob, NULL, ctx->user_data);
    } else {
      gnostr_blossom_blob_free(blob);
    }
    media_upload_ctx_free(ctx);
    return;
  }

  /* Blossom failed - fall back to NIP-96 */
  g_message("media_upload: Blossom upload failed: %s",
            error ? error->message : "unknown");
  if (error) g_error_free(error);

  try_nip96_fallback(ctx);
}

void gnostr_media_upload_async(const char *file_path,
                                const char *mime_type,
                                GnostrBlossomUploadCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable)
{
  if (!file_path) {
    GError *err = g_error_new_literal(GNOSTR_BLOSSOM_ERROR,
                                       GNOSTR_BLOSSOM_ERROR_FILE_NOT_FOUND,
                                       "No file path provided");
    if (callback) callback(NULL, err, user_data);
    g_error_free(err);
    return;
  }

  MediaUploadContext *ctx = g_new0(MediaUploadContext, 1);
  ctx->file_path = g_strdup(file_path);
  ctx->mime_type = g_strdup(mime_type);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  /* Check if Blossom servers are configured */
  gsize n_blossom = gnostr_blossom_settings_get_server_count();

  if (n_blossom > 0) {
    /* Try Blossom first with automatic fallback between servers */
    g_message("media_upload: trying %zu Blossom server(s) first", n_blossom);
    gnostr_blossom_upload_with_fallback_async(file_path, mime_type,
                                               on_blossom_attempt_complete, ctx,
                                               cancellable);
  } else {
    /* No Blossom servers - go straight to NIP-96 */
    try_nip96_fallback(ctx);
  }
}
