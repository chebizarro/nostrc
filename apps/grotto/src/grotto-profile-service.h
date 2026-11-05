/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
/* Trusted Grotto host only. Uses the session signer and per-identity relay
 * configuration. File selection/preview stays on GTK; all I/O runs off-thread. */
void grotto_profile_service_async(GtkWindow *parent, GrottoProfileAction action,
  const char *npub, const GrottoProfileInfo *info, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);
