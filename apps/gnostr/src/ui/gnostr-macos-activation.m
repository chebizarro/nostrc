#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include "gnostr-macos-activation.h"

typedef struct {
  GtkWindow *window;
} GnostrMacosForegroundCtx;

static void
gnostr_macos_foreground_ctx_free(gpointer user_data)
{
  GnostrMacosForegroundCtx *ctx = user_data;
  if (!ctx) return;
  if (ctx->window) {
    g_object_unref(ctx->window);
  }
  g_free(ctx);
}

static void
gnostr_macos_transform_to_foreground(void)
{
  ProcessSerialNumber psn = { 0, kCurrentProcess };
  TransformProcessType(&psn, kProcessTransformToForegroundApplication);
  SetFrontProcess(&psn);

  if (NSApp) {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  }

  NSRunningApplication *currentApp = [NSRunningApplication currentApplication];
  if (currentApp) {
    [currentApp activateWithOptions:NSApplicationActivateIgnoringOtherApps];
  } else if (NSApp) {
    [NSApp activateIgnoringOtherApps:YES];
  }
}

static gboolean
request_foreground_idle(gpointer user_data)
{
  GnostrMacosForegroundCtx *ctx = user_data;

  gnostr_macos_transform_to_foreground();

  if (ctx && ctx->window && GTK_IS_WINDOW(ctx->window)) {
    if (!gtk_widget_get_visible(GTK_WIDGET(ctx->window))) {
      gtk_widget_set_visible(GTK_WIDGET(ctx->window), TRUE);
    }
    gtk_window_present(ctx->window);
  }

  return G_SOURCE_REMOVE;
}

void
gnostr_macos_activation_prepare_regular_policy(void)
{
  gnostr_macos_transform_to_foreground();
}

void
gnostr_macos_activation_request_foreground(GtkWindow *window)
{
  g_return_if_fail(GTK_IS_WINDOW(window));

  GnostrMacosForegroundCtx *ctx = g_new0(GnostrMacosForegroundCtx, 1);
  ctx->window = g_object_ref(window);
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                  request_foreground_idle,
                  ctx,
                  gnostr_macos_foreground_ctx_free);
}

#endif /* __APPLE__ */
