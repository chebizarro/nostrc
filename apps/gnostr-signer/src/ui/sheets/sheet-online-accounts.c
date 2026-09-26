/* sheet-online-accounts.c - GNOME Online Accounts onboarding wizard
 *
 * SPDX-License-Identifier: MIT
 *
 * Three-step wizard:
 *   1. Start nostr-dav.service (systemd --user); wait for the credentials file.
 *   2. Read the bearer token from $XDG_CONFIG_HOME/nostr-dav/token (0600 file
 *      nostr-dav writes on first launch — see nostrc-0e7k). Show it to copy.
 *   3. Open GNOME Settings -> Online Accounts.
 *
 * History (nostrc-0e7k): the pre-hardening wizard minted its own random token
 * and pointed at port 7654. nostr-dav now owns credentials (fails closed at
 * startup and writes a 0600 token file), listens on 127.0.0.1:7680, and
 * refuses to bind off loopback. This wizard reads the file nostr-dav writes;
 * it never invents a token, so what the user sees is what the server will
 * actually accept.
 */

#include "sheet-online-accounts.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

/* nostr-dav listen address after the fix-closed landing (nostrc-0e7k). */
#define NOSTR_DAV_PORT 7680
#define NOSTR_DAV_URL  "http://127.0.0.1:7680"

/* How long to poll for the token file after starting the service. The 0600
 * file appears synchronously in nostr-dav's startup path, so this is a
 * safety margin for the transient dbus/systemd race, not a real wait. */
#define TOKEN_POLL_INTERVAL_MS 200
#define TOKEN_POLL_ATTEMPTS    25    /* 5 seconds total */

struct _SheetOnlineAccounts {
  AdwDialog parent_instance;

  /* Template children */
  AdwNavigationView *nav_view;

  /* Step 1 */
  GtkButton *btn_step1_next;
  GtkSpinner *spinner_service;
  GtkImage *img_service_status;
  GtkLabel *lbl_service_status;

  /* Step 2 */
  GtkButton *btn_step2_back;
  GtkButton *btn_step2_next;
  GtkButton *btn_copy_url;
  GtkButton *btn_copy_username;
  GtkButton *btn_copy_password;
  AdwActionRow *row_password;
  AdwBanner *banner_copied;

  /* Step 3 */
  GtkButton *btn_finish;

  /* State */
  gchar *bearer_token;
  gboolean service_started;
  guint token_poll_id;
  guint token_poll_attempts;
};

G_DEFINE_TYPE(SheetOnlineAccounts, sheet_online_accounts, ADW_TYPE_DIALOG)

/* ---- Helpers ---- */

static void
copy_to_clipboard(SheetOnlineAccounts *self, const gchar *text)
{
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, text);
  }

  /* Flash banner */
  adw_banner_set_revealed(self->banner_copied, TRUE);

  /* Auto-hide after 2 seconds (using a timeout) */
  g_timeout_add_seconds(2, (GSourceFunc)adw_banner_set_revealed,
                        self->banner_copied);
  /* The FALSE return from the non-existent callback is fine —
     adw_banner_set_revealed(banner, 0) hides it */
}

static void
show_service_status(SheetOnlineAccounts *self,
                    const gchar *text,
                    gboolean spinning,
                    gboolean success)
{
  gtk_widget_set_visible(GTK_WIDGET(self->spinner_service), spinning);
  gtk_spinner_set_spinning(self->spinner_service, spinning);
  gtk_widget_set_visible(GTK_WIDGET(self->img_service_status), success);
  gtk_widget_set_visible(GTK_WIDGET(self->lbl_service_status), TRUE);
  gtk_label_set_text(self->lbl_service_status, text);
}

/* Build the on-disk path nostr-dav writes its bearer token to. Prefers
 * $XDG_CONFIG_HOME, falls back to ~/.config per the XDG base-directory spec;
 * a relative XDG_CONFIG_HOME is ignored, same way nostr-dav resolves it. */
static gchar *
nostr_dav_token_path(void)
{
  const gchar *xdg = g_getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] == '/')
    return g_build_filename(xdg, "nostr-dav", "token", NULL);
  const gchar *home = g_get_home_dir();
  if (!home || !*home) return NULL;
  return g_build_filename(home, ".config", "nostr-dav", "token", NULL);
}

/* Reads the token nostr-dav wrote. NULL if the file is missing, unreadable,
 * or empty; caller frees. Trims trailing whitespace so a hand-edited file
 * with a newline works. */
static gchar *
read_nostr_dav_token(GError **error)
{
  g_autofree gchar *path = nostr_dav_token_path();
  if (!path) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "Cannot resolve XDG_CONFIG_HOME or $HOME");
    return NULL;
  }
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, error))
    return NULL;
  g_strchomp(contents);
  if (contents[0] == '\0') {
    g_free(contents);
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                "%s is empty; nostr-dav has not yet written a token", path);
    return NULL;
  }
  return contents;
}

/* Polls for nostr-dav's token file after we start the service. Runs in the
 * main loop, one shot per interval; on success it advances to step 2. */
static gboolean
poll_for_token(gpointer user_data)
{
  SheetOnlineAccounts *self = user_data;
  self->token_poll_attempts++;

  GError *err = NULL;
  gchar *tok = read_nostr_dav_token(&err);
  if (tok) {
    g_clear_pointer(&self->bearer_token, g_free);
    self->bearer_token = tok;
    adw_action_row_set_subtitle(self->row_password, "••••••••••••");
    show_service_status(self, "Bridge is running", FALSE, TRUE);
    adw_navigation_view_push_by_tag(self->nav_view, "step-token");
    self->token_poll_id = 0;
    return G_SOURCE_REMOVE;
  }

  if (self->token_poll_attempts >= TOKEN_POLL_ATTEMPTS) {
    g_warning("nostr-dav: token file not present after %u attempts: %s",
              self->token_poll_attempts, err ? err->message : "unknown");
    g_clear_error(&err);
    show_service_status(self,
      "Could not read nostr-dav credentials. Run `nostr-dav --show-credentials` in a terminal, or check `journalctl --user -u nostr-dav`.",
      FALSE, FALSE);
    self->token_poll_id = 0;
    return G_SOURCE_REMOVE;
  }

  g_clear_error(&err);
  return G_SOURCE_CONTINUE;
}

/* ---- Service management ---- */

static void
on_systemctl_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  SheetOnlineAccounts *self = user_data;
  GSubprocess *proc = G_SUBPROCESS(source);
  GError *err = NULL;

  g_subprocess_wait_finish(proc, result, &err);

  if (err) {
    show_service_status(self, "Failed to start service", FALSE, FALSE);
    g_warning("nostr-dav: systemctl failed: %s", err->message);
    g_error_free(err);
    return;
  }

  int exit_status = g_subprocess_get_exit_status(proc);
  if (exit_status != 0) {
    show_service_status(self, "Service failed to start", FALSE, FALSE);
    return;
  }

  self->service_started = TRUE;

  /* Fast path: the token file is often already there before systemctl start
   * returns; if not, poll a few times so the wizard is not racing systemd. */
  self->token_poll_attempts = 0;
  if (poll_for_token(self) == G_SOURCE_CONTINUE) {
    show_service_status(self, "Reading credentials…", TRUE, FALSE);
    self->token_poll_id = g_timeout_add(TOKEN_POLL_INTERVAL_MS, poll_for_token, self);
  }
}

static void
start_nostr_dav_service(SheetOnlineAccounts *self)
{
  show_service_status(self, "Starting bridge…", TRUE, FALSE);

  GError *err = NULL;
  GSubprocess *proc = g_subprocess_new(
    G_SUBPROCESS_FLAGS_NONE,
    &err,
    "systemctl", "--user", "start", "nostr-dav.service", NULL);

  if (proc == NULL) {
    /* systemctl not available (macOS dev, sandboxed session). Try reading
     * a token file that may already exist so the developer can still exercise
     * the wizard end-to-end; otherwise show the manual recipe. */
    g_warning("nostr-dav: systemctl not available: %s", err ? err->message : "?");
    g_clear_error(&err);

    GError *read_err = NULL;
    gchar *tok = read_nostr_dav_token(&read_err);
    if (tok) {
      g_clear_pointer(&self->bearer_token, g_free);
      self->bearer_token = tok;
      self->service_started = TRUE;
      show_service_status(self, "Bridge ready (manual mode)", FALSE, TRUE);
      adw_action_row_set_subtitle(self->row_password, "••••••••••••");
      adw_navigation_view_push_by_tag(self->nav_view, "step-token");
    } else {
      g_clear_error(&read_err);
      show_service_status(self,
        "Start `systemctl --user start nostr-dav.service` in a terminal, then reopen this wizard.",
        FALSE, FALSE);
    }
    return;
  }

  g_subprocess_wait_async(proc, NULL, on_systemctl_finished, self);
  g_object_unref(proc);
}

/* ---- Signal handlers ---- */

static void
on_step1_next(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetOnlineAccounts *self = user_data;
  start_nostr_dav_service(self);
}

static void
on_step2_back(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetOnlineAccounts *self = user_data;
  adw_navigation_view_pop(self->nav_view);
}

static void
on_copy_url(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  copy_to_clipboard(user_data, NOSTR_DAV_URL);
}

static void
on_copy_username(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  copy_to_clipboard(user_data, "nostr");
}

static void
on_copy_password(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetOnlineAccounts *self = user_data;
  if (self->bearer_token)
    copy_to_clipboard(self, self->bearer_token);
}

static void
on_step2_next(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetOnlineAccounts *self = user_data;

  /* Copy password to clipboard for easy paste */
  if (self->bearer_token)
    copy_to_clipboard(self, self->bearer_token);

  /* Open GNOME Settings → Online Accounts */
  GError *err = NULL;
  GAppInfo *settings = g_app_info_create_from_commandline(
    "gnome-control-center online-accounts",
    "GNOME Settings",
    G_APP_INFO_CREATE_NONE,
    &err);

  if (settings) {
    g_app_info_launch(settings, NULL, NULL, &err);
    g_object_unref(settings);
  }

  if (err) {
    g_warning("nostr-dav: failed to open Settings: %s", err->message);
    g_error_free(err);
  }

  /* Navigate to done page */
  adw_navigation_view_push_by_tag(self->nav_view, "step-done");
}

static void
on_finish(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetOnlineAccounts *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

/* ---- GObject lifecycle ---- */

static void
sheet_online_accounts_dispose(GObject *obj)
{
  SheetOnlineAccounts *self = SHEET_ONLINE_ACCOUNTS(obj);
  if (self->token_poll_id) {
    g_source_remove(self->token_poll_id);
    self->token_poll_id = 0;
  }
  if (self->bearer_token) {
    /* Best effort: wipe before free so a heap dump doesn't see the token. */
    memset(self->bearer_token, 0, strlen(self->bearer_token));
    g_clear_pointer(&self->bearer_token, g_free);
  }
  G_OBJECT_CLASS(sheet_online_accounts_parent_class)->dispose(obj);
}

static void
sheet_online_accounts_class_init(SheetOnlineAccountsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = sheet_online_accounts_dispose;

  gtk_widget_class_set_template_from_resource(
    widget_class,
    "/org/gnostr/signer/ui/sheets/sheet-online-accounts.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, nav_view);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_step1_next);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, spinner_service);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, img_service_status);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, lbl_service_status);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_step2_back);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_step2_next);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_copy_url);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_copy_username);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_copy_password);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, row_password);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, banner_copied);
  gtk_widget_class_bind_template_child(widget_class, SheetOnlineAccounts, btn_finish);
}

static void
sheet_online_accounts_init(SheetOnlineAccounts *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  self->bearer_token = NULL;
  self->service_started = FALSE;
  self->token_poll_id = 0;
  self->token_poll_attempts = 0;

  /* Connect signals */
  g_signal_connect(self->btn_step1_next, "clicked",
                   G_CALLBACK(on_step1_next), self);
  g_signal_connect(self->btn_step2_back, "clicked",
                   G_CALLBACK(on_step2_back), self);
  g_signal_connect(self->btn_step2_next, "clicked",
                   G_CALLBACK(on_step2_next), self);
  g_signal_connect(self->btn_copy_url, "clicked",
                   G_CALLBACK(on_copy_url), self);
  g_signal_connect(self->btn_copy_username, "clicked",
                   G_CALLBACK(on_copy_username), self);
  g_signal_connect(self->btn_copy_password, "clicked",
                   G_CALLBACK(on_copy_password), self);
  g_signal_connect(self->btn_finish, "clicked",
                   G_CALLBACK(on_finish), self);
}

/* ---- Public API ---- */

SheetOnlineAccounts *
sheet_online_accounts_new(void)
{
  return g_object_new(SHEET_TYPE_ONLINE_ACCOUNTS, NULL);
}
