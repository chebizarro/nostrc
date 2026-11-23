/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
#if defined(_WIN32)
# ifdef GROTTO_CONTROL_CLIENT_COMPILATION
#  define GROTTO_CONTROL_CLIENT_API __declspec(dllexport)
# else
#  define GROTTO_CONTROL_CLIENT_API __declspec(dllimport)
# endif
#else
# define GROTTO_CONTROL_CLIENT_API __attribute__((visibility("default")))
#endif

G_BEGIN_DECLS
#define GROTTO_TYPE_PANELS_CONTROL1_SERVICE (grotto_panels_control1_service_get_type())
GROTTO_CONTROL_CLIENT_API
G_DECLARE_FINAL_TYPE(GrottoPanelsControl1Service, grotto_panels_control1_service,
                     GROTTO, PANELS_CONTROL1_SERVICE, GObject)
/* Control1 transport adapter; does not link Grotto application code. Construct/keep it on
 * the GTK main context, independently of panel mount/unmount. The supplied
 * proxy must address org.nostr.Signer /org/nostr/signer/grotto Control1 and be
 * created on that context. Subscribes before taking initial snapshots.
 * Coarse feature flags do NOT authorize mutations. Missing caller-specific
 * management-capabilities keep can() read-only. Public identities come only
 * from Control1.ListIdentities; Grotto may overlay its local metadata. */
GROTTO_CONTROL_CLIENT_API GrottoPanelsService *grotto_panels_control1_service_new(GDBusProxy *proxy);
/* Settings fails closed on missing/skewed role metadata and exposes only the
 * cosmetic manager command, even if a peer advertises approver capabilities. */
GROTTO_CONTROL_CLIENT_API GrottoPanelsService *grotto_panels_control1_service_new_manager(GDBusProxy *proxy);
/* Grotto host hook only: the caller checks management lock and protocol version.
 * This does not change shared service capabilities or daemon authorization. */
GROTTO_CONTROL_CLIENT_API void grotto_panels_control1_service_host_command_async(GrottoPanelsService *service,
  GrottoPanelCommand command, const GrottoPanelCommandArgs *args, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);

/* Shared GUI cosmetic preference; no daemon policy or backup acknowledgement. */
GROTTO_CONTROL_CLIENT_API GSettings *grotto_panels_backup_reminder_settings(const char *public_key);
GROTTO_CONTROL_CLIENT_API gboolean grotto_panels_backup_banner_dismissed(const char *public_key);
GROTTO_CONTROL_CLIENT_API gboolean grotto_panels_dismiss_backup_banner(const char *public_key);

G_END_DECLS
