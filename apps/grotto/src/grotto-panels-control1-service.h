/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
G_BEGIN_DECLS
#define GROTTO_TYPE_PANELS_CONTROL1_SERVICE (grotto_panels_control1_service_get_type())
G_DECLARE_FINAL_TYPE(GrottoPanelsControl1Service, grotto_panels_control1_service,
                     GROTTO, PANELS_CONTROL1_SERVICE, GObject)
/* Host-owned adapter, not a library dependency on Grotto. Construct/keep it on
 * the GTK main context, independently of panel mount/unmount. The supplied
 * proxy must address org.nostr.Signer /org/nostr/signer/grotto Control1 and be
 * created on that context. Subscribes before taking initial snapshots.
 * Current coarse feature flags do NOT authorize mutations. Until W6 supplies
 * caller-specific management-capabilities, can() is deliberately read-only.
 * Identities stay empty until W3's public metadata composite is installed. */
GrottoPanelsService *grotto_panels_control1_service_new(GDBusProxy *proxy);
/* Grotto host hook only: the caller checks management lock and protocol version.
 * This does not change shared service capabilities or daemon authorization. */
void grotto_panels_control1_service_host_command_async(GrottoPanelsService *service,
  GrottoPanelCommand command, const GrottoPanelCommandArgs *args, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);
G_END_DECLS
