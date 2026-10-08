#include "gh-nip46-auth-url.h"
#include <glib/gi18n.h>

struct _GhNip46AuthUrl {
  GObject parent_instance;
  GWeakRef app;
  GWeakRef context;
  gchar *action_name;
  gchar *notification_id;
  gchar *url;
  GCancellable *cancellable;
  guint64 serial;
  gboolean launched;
  GhNip46AuthNoticeFunc notice_hook;
  gpointer notice_data;
  gboolean disposed;
};

enum { SIGNAL_LAUNCH_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];
static guint next_action;

G_DEFINE_FINAL_TYPE(GhNip46AuthUrl, gh_nip46_auth_url, G_TYPE_OBJECT)

typedef struct {
  GhNip46AuthUrl *self;
  guint64 serial;
} Launch;

static GtkWindow *
active_window(GhNip46AuthUrl *self, GtkApplication *app)
{
  g_autoptr(GtkWidget) context = GTK_WIDGET(g_weak_ref_get(&self->context));
  GtkRoot *root = context ? gtk_widget_get_root(context) : NULL;
  GtkWindow *window = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) :
                      gtk_application_get_active_window(app);
  return window && gtk_widget_get_visible(GTK_WIDGET(window)) &&
         gtk_window_is_active(window) ? window : NULL;
}

static void
launched(GObject *source, GAsyncResult *result, gpointer data)
{
  Launch *launch = data;
  GhNip46AuthUrl *self = launch->self;
  g_autoptr(GError) error = NULL;
  gboolean ok = gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(source), result, &error);
  if (!ok && launch->serial == self->serial &&
      (!self->cancellable || !g_cancellable_is_cancelled(self->cancellable)))
    g_signal_emit(self, signals[SIGNAL_LAUNCH_FAILED], 0, error);
  g_object_unref(self);
  g_free(launch);
}

static void
open_url(GhNip46AuthUrl *self)
{
  if (!self->url || self->launched) return;
  if (self->cancellable && g_cancellable_is_cancelled(self->cancellable)) {
    gh_nip46_auth_url_clear(self);
    return;
  }
  g_autoptr(GtkApplication) app = GTK_APPLICATION(g_weak_ref_get(&self->app));
  if (!app) return;
  self->launched = TRUE;
  g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(self->url);
  Launch *launch = g_new0(Launch, 1);
  launch->self = g_object_ref(self);
  launch->serial = self->serial;
  gtk_uri_launcher_launch(launcher, active_window(self, app), self->cancellable,
                          launched, launch);
  if (g_application_get_is_registered(G_APPLICATION(app)))
    g_application_withdraw_notification(G_APPLICATION(app), self->notification_id);
}

void
gh_nip46_auth_url_activate_for_test(GhNip46AuthUrl *self)
{
  g_return_if_fail(GH_IS_NIP46_AUTH_URL(self));
  open_url(self);
}

static void
on_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action; (void)parameter;
  open_url(GH_NIP46_AUTH_URL(data));
}

void
gh_nip46_auth_url_clear(GhNip46AuthUrl *self)
{
  g_return_if_fail(GH_IS_NIP46_AUTH_URL(self));
  g_autoptr(GtkApplication) app = GTK_APPLICATION(g_weak_ref_get(&self->app));
  if (app && self->notification_id &&
      g_application_get_is_registered(G_APPLICATION(app)))
    g_application_withdraw_notification(G_APPLICATION(app), self->notification_id);
  self->serial++;
  if (self->url) {
    volatile gchar *bytes = (volatile gchar *)self->url;
    for (gsize i = 0, n = strlen(self->url); i < n; i++) bytes[i] = 0;
    g_clear_pointer(&self->url, g_free);
  }
  g_clear_object(&self->cancellable);
  self->launched = FALSE;
}

void
gh_nip46_auth_url_set_notice_hook_for_test(GhNip46AuthUrl *self,
                                                GhNip46AuthNoticeFunc hook,
                                                gpointer user_data)
{
  g_return_if_fail(GH_IS_NIP46_AUTH_URL(self));
  self->notice_hook = hook;
  self->notice_data = user_data;
}

gboolean
gh_nip46_auth_url_has_pending(GhNip46AuthUrl *self)
{
  g_return_val_if_fail(GH_IS_NIP46_AUTH_URL(self), FALSE);
  return self->url != NULL && !self->launched;
}

gboolean
gh_nip46_auth_url_handle(GhNip46AuthUrl *self, const gchar *url,
                           GCancellable *cancellable)
{
  g_return_val_if_fail(GH_IS_NIP46_AUTH_URL(self) && url != NULL, FALSE);
  g_autoptr(GtkApplication) app = GTK_APPLICATION(g_weak_ref_get(&self->app));
  if (!app || (cancellable && g_cancellable_is_cancelled(cancellable))) return FALSE;
  gh_nip46_auth_url_clear(self);
  self->url = g_strdup(url);
  self->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
  if (active_window(self, app)) {
    open_url(self);
  } else {
    g_autoptr(GNotification) notice = g_notification_new(_("Signer Authorization Needed"));
    g_notification_set_body(notice, _("Open your signer's authorization page to approve the request."));
    g_autofree gchar *action = g_strconcat("app.", self->action_name, NULL);
    g_notification_set_default_action(notice, action);
    if (self->notice_hook)
      self->notice_hook(notice, self->notice_data);
    else
      g_application_send_notification(G_APPLICATION(app), self->notification_id, notice);
  }
  return TRUE;
}

static void
dispose(GObject *object)
{
  GhNip46AuthUrl *self = GH_NIP46_AUTH_URL(object);
  if (!self->disposed) {
    self->disposed = TRUE;
    gh_nip46_auth_url_clear(self);
    g_autoptr(GtkApplication) app = GTK_APPLICATION(g_weak_ref_get(&self->app));
    if (app && self->action_name)
      g_action_map_remove_action(G_ACTION_MAP(app), self->action_name);
    g_weak_ref_clear(&self->context);
    g_weak_ref_clear(&self->app);
  }
  G_OBJECT_CLASS(gh_nip46_auth_url_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
  GhNip46AuthUrl *self = GH_NIP46_AUTH_URL(object);
  g_free(self->action_name);
  g_free(self->notification_id);
  G_OBJECT_CLASS(gh_nip46_auth_url_parent_class)->finalize(object);
}

static void
gh_nip46_auth_url_class_init(GhNip46AuthUrlClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS(klass);
  object->dispose = dispose;
  object->finalize = finalize;
  signals[SIGNAL_LAUNCH_FAILED] = g_signal_new("launch-failed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gh_nip46_auth_url_init(GhNip46AuthUrl *self)
{
  g_weak_ref_init(&self->app, NULL);
  g_weak_ref_init(&self->context, NULL);
}

GhNip46AuthUrl *
gh_nip46_auth_url_new(GtkApplication *app, GtkWidget *context)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);
  g_return_val_if_fail(context == NULL || GTK_IS_WIDGET(context), NULL);
  GhNip46AuthUrl *self = g_object_new(GH_TYPE_NIP46_AUTH_URL, NULL);
  g_weak_ref_set(&self->app, app);
  g_weak_ref_set(&self->context, context);
  guint id = ++next_action;
  self->action_name = g_strdup_printf("open-signer-authorization-%u", id);
  self->notification_id = g_strdup_printf("groundhog-signer-authorization-%u", id);
  g_autoptr(GSimpleAction) action = g_simple_action_new(self->action_name, NULL);
  g_signal_connect(action, "activate", G_CALLBACK(on_action), self);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  return self;
}
