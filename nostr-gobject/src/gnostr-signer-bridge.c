/*
 * gnostr-signer-bridge.c — registration seam implementation.
 *
 * See gnostr-signer-bridge.h. All state is file-static; installation is
 * expected to happen once at app startup on the main thread, before any
 * signer-driven code path in the library runs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-signer-bridge.h"

#include <glib.h>
#include <gio/gio.h>

static GnostrSignerBridge s_bridge = { NULL, NULL, NULL, NULL };

void
gnostr_signer_bridge_install(const GnostrSignerBridge *vtable)
{
  if (vtable) {
    s_bridge = *vtable;
  } else {
    s_bridge.is_available    = NULL;
    s_bridge.sign_event_async  = NULL;
    s_bridge.sign_event_finish = NULL;
    s_bridge.proxy_get         = NULL;
  }
}

gboolean
gnostr_signer_bridge_is_available(void)
{
  return s_bridge.is_available ? s_bridge.is_available() : FALSE;
}

/* When no bridge is installed but sign_event_async is called anyway, we
 * still owe the caller an async completion. Complete a GTask with a
 * G_IO_ERROR_NOT_INITIALIZED error so their existing finish path handles
 * it uniformly. */
static void
bridge_report_not_initialized(GAsyncReadyCallback callback,
                              gpointer user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "gnostr signer bridge not installed");
  g_object_unref(task);
}

void
gnostr_signer_bridge_sign_event_async(const char *event_json,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  if (s_bridge.sign_event_async) {
    s_bridge.sign_event_async(event_json, cancellable, callback, user_data);
    return;
  }
  if (callback) {
    bridge_report_not_initialized(callback, user_data);
  }
}

gboolean
gnostr_signer_bridge_sign_event_finish(GAsyncResult *res,
                                        char **out_signed_event,
                                        GError **error)
{
  if (s_bridge.sign_event_finish) {
    return s_bridge.sign_event_finish(res, out_signed_event, error);
  }
  /* Fallback for results produced by our own not-initialized GTask. */
  if (G_IS_TASK(res)) {
    char *r = g_task_propagate_pointer(G_TASK(res), error);
    if (r) {
      if (out_signed_event) *out_signed_event = r;
      else g_free(r);
      return TRUE;
    }
    return FALSE;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                      "gnostr signer bridge not installed");
  return FALSE;
}

NostrOrgNostrSigner *
gnostr_signer_bridge_proxy_get(GError **error)
{
  if (s_bridge.proxy_get) {
    return s_bridge.proxy_get(error);
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                      "gnostr signer bridge not installed");
  return NULL;
}
