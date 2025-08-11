#include "gnostr-main-window.h"
#include "gnostr-composer.h"
#include "../ipc/signer_ipc.h"
#include <gio/gio.h>
#include <time.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

struct _GnostrMainWindow {
  GtkApplicationWindow parent_instance;
  // Template children
  GtkWidget *stack;
  GtkWidget *timeline;
  GtkWidget *btn_settings;
  GtkWidget *composer;
};

G_DEFINE_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void on_settings_clicked(GnostrMainWindow *self, GtkButton *button) {
  (void)self; (void)button;
  g_message("settings clicked");
}

/* No proxy signals: org.nostr.Signer.xml does not define approval signals. */

typedef struct {
  NostrSignerProxy *proxy; /* not owned */
  char *event_json;        /* owned */
  char *current_user;      /* owned */
  char *app_id;            /* owned */
} PostCtx;

static void post_ctx_free(PostCtx *ctx){
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx->current_user);
  g_free(ctx->app_id);
  g_free(ctx);
}

static void on_sign_event_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GError *error = NULL;
  char *signature = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signature, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("SignEvent async failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    PostCtx *ctx = (PostCtx*)user_data;
    post_ctx_free(ctx);
    return;
  }
  g_message("signature: %s", signature ? signature : "<null>");
  g_free(signature);
  PostCtx *ctx = (PostCtx*)user_data;
  post_ctx_free(ctx);
}

static void on_get_pubkey_done(GObject *source, GAsyncResult *res, gpointer user_data){
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  PostCtx *ctx = (PostCtx*)user_data;
  GError *error = NULL;
  char *npub = NULL;
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_finish(proxy, &npub, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("GetPublicKey failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    post_ctx_free(ctx);
    return;
  }
  g_message("using identity npub=%s", npub ? npub : "<null>");
  g_free(npub);
  /* Proceed to SignEvent */
  g_message("calling SignEvent (async)... json-len=%zu", strlen(ctx->event_json));
  nostr_org_nostr_signer_call_sign_event(
      proxy,
      ctx->event_json,
      ctx->current_user,
      ctx->app_id,
      NULL,
      on_sign_event_done,
      ctx);
}

static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data) {
  g_message("on_composer_post_requested enter composer=%p user_data=%p", (void*)composer, user_data);
  if (!GNOSTR_IS_COMPOSER(composer)) { g_warning("composer instance invalid"); return; }
  if (!GNOSTR_IS_MAIN_WINDOW(user_data)) { g_warning("main window user_data invalid"); return; }
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!text || !*text) {
    g_message("empty post ignored");
    return;
  }
  g_message("post-requested: %s", text);

  // Build minimal Nostr event JSON (kind=1) for signing
  time_t now = time(NULL);
  gchar *escaped = g_strescape(text, NULL);
  gchar *event_json = g_strdup_printf("{\"kind\":1,\"created_at\":%ld,\"tags\":[],\"content\":\"%s\"}", (long)now, escaped ? escaped : "");
  g_free(escaped);

  GError *error = NULL;
  g_message("acquiring signer proxy...");
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
  if (!proxy) {
    g_warning("Signer proxy unavailable: %s", error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(event_json);
    return;
  }

  // Ensure the service is actually present on the bus; otherwise avoid long timeouts
  const gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!owner || !*owner) {
    g_warning("Signer service is not running (no name owner). Start gnostr-signer-daemon and retry.");
    g_free(event_json);
    return;
  }

  // For SignEvent, allow sufficient time for user approval in the signer UI
  g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(proxy), 600000); // 10 minutes

  PostCtx *ctx = g_new0(PostCtx, 1);
  ctx->proxy = proxy;
  ctx->event_json = event_json; /* take ownership */
  ctx->current_user = g_strdup(""); // default session identity
  ctx->app_id = g_strdup("org.gnostr.Client");

  /* Pre-check identity exists and is configured */
  g_message("calling GetPublicKey (async) to verify identity...");
  nostr_org_nostr_signer_call_get_public_key(
      proxy,
      NULL,
      on_get_pubkey_done,
      ctx);

  // TODO: assemble full event (id/sig/pubkey), publish to relay, push into timeline
}

static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, composer);
  gtk_widget_class_bind_template_callback(widget_class, on_settings_clicked);
}

static void gnostr_main_window_init(GnostrMainWindow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_return_if_fail(self->composer != NULL);
  g_message("connecting post-requested handler on composer=%p", (void*)self->composer);
  g_signal_connect(self->composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);
}

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}
