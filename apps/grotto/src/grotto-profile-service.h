/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
/* Trusted Grotto host only. Uses the session signer and per-identity relay
 * configuration. File selection/preview stays on GTK; all I/O runs off-thread. */
/* Internal host helpers shared with focused UI/privacy regressions; not installed. */
GrottoNip05Status grotto_profile_nip05_response(const char *data, gsize length, const char *name, const char *hex);
GBytes *grotto_profile_crop_image(GdkTexture *source, gboolean banner, double zoom, double x, double y);
GdkTexture *grotto_profile_decode_image(GBytes *bytes, char **mime, GError **error);
AdwAlertDialog *grotto_profile_upload_confirmation(GdkTexture *preview, gboolean banner, const char *const *servers);
AdwAlertDialog *grotto_profile_images_confirmation(const GrottoProfileInfo *info);
void grotto_profile_service_async(GtkWindow *parent, GrottoProfileAction action,
  const char *npub, const GrottoProfileInfo *info, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);
