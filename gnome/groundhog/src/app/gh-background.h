#ifndef GH_BACKGROUND_H
#define GH_BACKGROUND_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * GhBackground: background delivery with the window closed (privacy charter
 * §5.3, §8.2 G15; decision D11). One per process, made by gh-app-services.c
 * and reachable with gh_background_get_for_application().
 *
 * Process model (B3). While `run-in-background` is on, the application is
 * held (g_application_hold()), so closing the last window keeps the process,
 * its store and its inbox running; the window's widgets are destroyed. The
 * key's default (on) is honoured as is: the hold does not wait for the user
 * to confirm the choice. Turning the key off releases the hold, so a process
 * with no window then exits. `app.quit` (Ctrl+Q) always exits, whatever the
 * key says, and the services shut down in their usual order.
 * `groundhog --gapplication-service` (the autostart command) starts without
 * a window: GApplication's service mode never emits "activate" by itself.
 *
 * The first time the last window is closed with background delivery on, the
 * close is held back for a one-time explanation (an AdwAlertDialog: keep
 * running, or quit). It is remembered in a marker file under the state
 * directory, never in GSettings.
 *
 * Autostart (B2) follows only a confirmed choice: the key has a user value
 * (set by onboarding, Preferences or gh_background_set_enabled_async()).
 * With the default merely in effect, nothing is written and no portal is
 * asked. Confirmed on: start at login; confirmed off (or reset): not.
 *   - Host install (GH_BACKGROUND_METHOD_FILE): writes or removes
 *     $XDG_CONFIG_HOME/autostart/org.nostr.Groundhog.desktop
 *     (Exec=<bindir>/groundhog --gapplication-service; the template is
 *     data/org.nostr.Groundhog-autostart.desktop.in). Only a file carrying
 *     X-Groundhog-Autostart=true is ever replaced or removed, and one the
 *     user switched off in the desktop (Hidden=true or
 *     X-GNOME-Autostart-enabled=false) is left alone.
 *   - Sandboxed (GH_BACKGROUND_METHOD_PORTAL):
 *     org.freedesktop.portal.Background.RequestBackground with `autostart`
 *     and `commandline` [groundhog, --gapplication-service]. If the portal
 *     refuses background activity, the key is turned off (the system will
 *     not let Groundhog run without a window) and the request fails with
 *     G_IO_ERROR_PERMISSION_DENIED.
 * GH_BACKGROUND_METHOD_AUTO picks the portal only inside a Flatpak sandbox:
 * xdg-desktop-portal refuses SetStatus to unsandboxed callers and cannot
 * write autostart entries for a host app it cannot identify.
 *
 * Status (no tray icon): with the portal, SetStatus (Background version 2)
 * tells the desktop's background-apps list what Groundhog is doing, derived
 * from the account store: receiving, locked, not receiving. The text never
 * names an account or a conversation.
 *
 * Notifications for a locked store (NO-11) are G16's.
 */

typedef enum {
  GH_BACKGROUND_METHOD_AUTO,   /* the portal in a Flatpak sandbox, else the file */
  GH_BACKGROUND_METHOD_FILE,   /* host: the XDG autostart file */
  GH_BACKGROUND_METHOD_PORTAL, /* sandboxed: org.freedesktop.portal.Background */
} GhBackgroundMethod;

typedef struct {
  GSettings *settings;         /* required: org.nostr.Groundhog (run-in-background) */
  GDBusConnection *connection; /* the portal's bus; NULL = the application's */
  GhBackgroundMethod method;
  const gchar *config_dir;     /* absolute; NULL = g_get_user_config_dir() */
  const gchar *state_dir;      /* absolute; NULL = g_get_user_state_dir()/groundhog */
  GObject *account_store;      /* nullable GhAccountStore: the status text */
} GhBackgroundConfig;

/* The autostart entry's file name (under <config_dir>/autostart). */
#define GH_BACKGROUND_AUTOSTART_FILE "org.nostr.Groundhog.desktop"
/* The one-time explanation's marker (under state_dir). */
#define GH_BACKGROUND_EXPLAINED_FILE "background-explained"

/* Portal status texts (at most 96 characters, per the portal). */
#define GH_BACKGROUND_STATUS_RECEIVING "Receiving messages"
#define GH_BACKGROUND_STATUS_LOCKED "Messages are locked. Open Groundhog to unlock them."
#define GH_BACKGROUND_STATUS_STOPPED "Not receiving messages. Open Groundhog for details."
#define GH_BACKGROUND_STATUS_NO_ACCOUNT "No account selected"

#define GH_TYPE_BACKGROUND (gh_background_get_type())
G_DECLARE_FINAL_TYPE(GhBackground, gh_background, GH, BACKGROUND, GObject)

/* app: the process's application (a GtkApplication gets the close
 * handling). Takes the hold at once when the key is on and reconciles a
 * confirmed autostart choice. Dispose (g_object_run_dispose()) releases the
 * hold, cancels portal requests and detaches from app. Properties:
 * "enabled" and "holding" (booleans), "status" (string), "explained". */
GhBackground *gh_background_new(GApplication *app, const GhBackgroundConfig *config);

/* Borrowed; NULL when the build or the process has none. For onboarding and
 * Preferences. */
GhBackground *gh_background_get_for_application(GApplication *app);

/* run-in-background, as now in effect (the default included). */
gboolean gh_background_get_enabled(GhBackground *self);
/* Whether the application hold is taken. */
gboolean gh_background_get_holding(GhBackground *self);
/* FILE or PORTAL (AUTO resolved). */
GhBackgroundMethod gh_background_get_method(GhBackground *self);
/* What the portal status says (or would say) now; NULL while off. */
const gchar *gh_background_get_status(GhBackground *self);
/* Whether the one-time explanation was shown (or background mode was
 * explained by the caller of gh_background_set_enabled_async()). */
gboolean gh_background_get_explained(GhBackground *self);

/* The user's explicit choice (onboarding, D11): writes run-in-background,
 * marks background mode as explained, updates the hold and applies
 * autostart. Completes once the autostart entry or the portal agrees.
 * Errors: G_IO_ERROR_PERMISSION_DENIED (the portal refused; the key is now
 * off), G_IO_ERROR_NOT_SUPPORTED (no portal, or autostart unavailable), a
 * GFileError for the autostart file, G_IO_ERROR_CANCELLED on dispose. */
void gh_background_set_enabled_async(GhBackground *self, gboolean enabled,
                                     GCancellable *cancellable, GAsyncReadyCallback callback,
                                     gpointer user_data);
gboolean gh_background_set_enabled_finish(GhBackground *self, GAsyncResult *result,
                                          GError **error);

G_END_DECLS
#endif
