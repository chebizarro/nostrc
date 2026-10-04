#include "ui/gh-about-dialog.h"
#include "ui/gh-status.h"
#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>

#include "gh-app-services.h"
#if defined(GH_MLS_TEST_HOOKS) && GROUNDHOG_HAVE_MLS
#include "app/gh-test-control.h"
#endif
#if GROUNDHOG_HAVE_BACKGROUND
#include "gh-background.h"
#endif
#include "gh-window.h"
#ifdef GH_TEST_FONTCONFIG_CLEANUP
#include <fontconfig/fontconfig.h>
#endif

/* nostrc-v59q: the D-Bus probe uses a supervised GLib connection. */
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

void groundhog_register_resource(void);

static gboolean smoke_mode = FALSE;
static int smoke_status = 0;
/* Every process-owned service (account, relays, conversations, inbox, the
 * encrypted store and its outbox) lives in the container; see
 * gh-app-services.h. */
static GhAppServices *app_services;

/* TRUE when the session bus was detected as unresponsive (nostrc-v59q) and
 * Groundhog falls back to non-unique mode. The account controller sees a
 * NULL D-Bus connection and shows the SIGNER_NO_BUS banner. */
static gboolean bus_fallback = FALSE;

/* The instance name, if running as a named instance (nostrc-lrac). NULL for
 * the default instance. Validated in setup_instance(). */
static const gchar *instance_name;
static gchar *parsed_instance_option;
static gboolean parsed_smoke_option;
static const GOptionEntry instance_options[] = {
  { "instance", 0, 0, G_OPTION_ARG_STRING, &parsed_instance_option,
    "Use an isolated Groundhog device instance", "NAME" },
  { "smoke", 0, 0, G_OPTION_ARG_NONE, &parsed_smoke_option,
    "Check the Groundhog GUI and exit", NULL },
  { 0 }
};

/* ---- Instance support (nostrc-lrac) ----------------------------------------
 *
 * --instance NAME (or GROUNDHOG_INSTANCE=NAME) runs Groundhog with:
 *   - app id  org.nostr.Groundhog.NAME
 *   - XDG dirs  under <original>/.groundhog-instances/NAME
 *   - GSettings  keyfile backend (portable, no dconf needed)
 *   - encrypted store and state  follow XDG dirs; background autostart is disabled
 *
 * Each instance is a separate device from the protocol's point of view:
 * its own keys, store, relay lists and MLS state. The privacy charter
 * treats two instances as two independent users of the same machine. */

/* Returns TRUE if name is a valid instance identifier:
 * 1–32 characters, ASCII alphanumeric or underscore, first char a letter. */
static gboolean
valid_instance_name(const char *name)
{
  if (!name || !name[0])
    return FALSE;
  if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
    return FALSE;
  for (size_t i = 1; name[i]; i++) {
    if (i >= 32)
      return FALSE;
    char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return FALSE;
  }
  return TRUE;
}

/* Must be called before any GLib function that caches XDG directories.
 * Returns the instance name (owned by the process) or NULL. */
static const char *
setup_instance(int argc, char **argv)
{
  /* Select the profile before GLib caches XDG paths. GApplication parses
   * the registered option later; do not mutate argv behind its back. */
  const char *name = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--instance") == 0) {
      if (name || i + 1 >= argc || argv[i + 1][0] == '-') {
        fprintf(stderr, "Groundhog: --instance needs exactly one NAME\n");
        exit(1);
      }
      name = argv[++i];
    } else if (strncmp(argv[i], "--instance=", 11) == 0) {
      if (name) {
        fprintf(stderr, "Groundhog: --instance may only be specified once\n");
        exit(1);
      }
      name = argv[i] + 11;
    }
  }
  if (!name)
    name = getenv("GROUNDHOG_INSTANCE");

  if (!name)
    return NULL;

  if (!valid_instance_name(name)) {
    fprintf(stderr, "Groundhog: invalid instance name '%s' "
            "(1-32 chars, alphanumeric/underscore, starts with letter)\n", name);
    exit(1);  /* Fail closed: never silently launch the default profile. */
  }

  /* Child services (including background policy) see the CLI-selected name. */
  if (name != getenv("GROUNDHOG_INSTANCE"))
    setenv("GROUNDHOG_INSTANCE", name, 1);

  /* 3. Override XDG directories: each instance gets its own subtree.
   * The originals are read before any GLib caching can happen. */
  const char *xdg_vars[] = {
    "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"
  };
  const char *defaults[] = {
    NULL, NULL, NULL, NULL
  };
  /* Compute defaults (before we override them). */
  const char *home = getenv("HOME");
  if (!home)
    home = "/tmp";
  char config_default[PATH_MAX], data_default[PATH_MAX],
       cache_default[PATH_MAX], state_default[PATH_MAX];
  snprintf(config_default, sizeof(config_default), "%s/.config", home);
  snprintf(data_default, sizeof(data_default), "%s/.local/share", home);
  snprintf(cache_default, sizeof(cache_default), "%s/.cache", home);
  snprintf(state_default, sizeof(state_default), "%s/.local/state", home);
  defaults[0] = config_default;
  defaults[1] = data_default;
  defaults[2] = cache_default;
  defaults[3] = state_default;

  for (size_t i = 0; i < 4; i++) {
    const char *current = getenv(xdg_vars[i]);
    if (!current || !current[0])
      current = defaults[i];
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s/.groundhog-instances/%s", current, name);
    setenv(xdg_vars[i], buf, 1);
  }

  /* A named device must never share dconf's fixed schema path with another
   * device, regardless of a caller-supplied backend override. */
  setenv("GSETTINGS_BACKEND", "keyfile", 1);

  return g_strdup(name);
}

/* Build the app ID with the instance suffix, if any. The returned string
 * is static or process-lifetime. */
static const char *
app_id_for_instance(const char *name)
{
  if (!name)
    return GROUNDHOG_APP_ID;
  /* "org.nostr.Groundhog.NAME" — safe because name is validated. */
  static char buf[128];
  snprintf(buf, sizeof(buf), "%s.%s", GROUNDHOG_APP_ID, name);
  return buf;
}

/* A child performs GLib's real bus connection, including the complete AUTH
 * exchange, without allowing a stalled auth worker to hang the UI process.
 * The single monotonic deadline covers address resolution, connect and AUTH
 * for Unix paths, abstract sockets, TCP and launchd addresses alike. */
static gboolean
session_bus_connects(guint timeout_ms, gboolean *timed_out)
{
  *timed_out = FALSE;
  pid_t pid = fork();
  if (pid < 0)
    return FALSE;
  if (pid == 0) {
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (bus)
      g_object_unref(bus);
    g_clear_error(&error);
    _exit(bus ? 0 : 1);
  }

  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  int status = 0;
  for (;;) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (result < 0 && errno != EINTR)
      return FALSE;
    if (g_get_monotonic_time() >= deadline)
      break;
    g_usleep(20 * 1000);
  }
  *timed_out = TRUE;
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  return FALSE;
}

/* Check whether the session bus is reachable; if not, arrange for
 * GApplication to skip it. Called before g_application_run(). */
static void
probe_session_bus(GApplicationFlags *flags)
{
  /* No address at all: nothing to probe, GApplication will fail fast. */
  const gchar *addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");
#ifdef __APPLE__
  const gchar *launchd_sock = g_getenv("DBUS_LAUNCHD_SESSION_BUS_SOCKET");
  if ((!addr || !*addr) && (!launchd_sock || !*launchd_sock))
    return;
#else
  if (!addr || !*addr)
    return;
#endif

  gboolean timed_out = FALSE;
  if (session_bus_connects(5000, &timed_out) || !timed_out)
    return;

  /* A bus that never completed AUTH timed out. Point the address at a path
   * that fails fast (connect → ENOTSOCK) so g_bus_get_sync() returns
   * an error instead of blocking, and switch to non-unique so
   * g_application_register() does not fail. An empty string would
   * trigger GLib's Linux fallback to $XDG_RUNTIME_DIR/bus or X11
   * autolaunch, which may also block. */
  g_message("Groundhog: the session bus did not respond within 5 s — "
            "Nostr Signer and notifications are unavailable (nostrc-v59q)");
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/dev/null", TRUE);
  *flags |= G_APPLICATION_NON_UNIQUE;
  bus_fallback = TRUE;
}

/* ---- application callbacks -------------------------------------------------- */

/* The widget tree is the GhWindow template (data/ui/gh-window.blp and the
 * page templates it names); only behaviour is attached here. */
static GtkWidget *
create_window(AdwApplication *app)
{
  GhWindow *window = gh_window_new(GTK_APPLICATION(app));
  if (app_services)
    gh_app_services_attach_window(app_services, window);
  return GTK_WIDGET(window);
}

static gboolean
smoke_check(gpointer user_data)
{
  GApplication *app = G_APPLICATION(user_data);
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(app));
  /* Proves the compiled template resource instantiated the whole tree. */
  AdwNavigationSplitView *split = GH_IS_WINDOW(window) ? gh_window_get_split(GH_WINDOW(window))
                                                       : NULL;

  if (!ADW_IS_NAVIGATION_SPLIT_VIEW(split) ||
      !GH_IS_SIDEBAR_PAGE(adw_navigation_split_view_get_sidebar(split)) ||
      !GH_IS_CONTENT_PAGE(adw_navigation_split_view_get_content(split)))
    smoke_status = 1;
  else {
    GtkStack *stack = gh_sidebar_page_get_stack(gh_window_get_sidebar(GH_WINDOW(window)));

    /* The compact drill-down API must remain usable with an empty model. */
    adw_navigation_split_view_set_collapsed(split, TRUE);
    adw_navigation_split_view_set_show_content(split, TRUE);
    if (!adw_navigation_split_view_get_show_content(split))
      smoke_status = 1;
#if GROUNDHOG_HAVE_ACCOUNTS
    /* The sidebar always reflects an account state; never a blank stack. */
    if (!gtk_stack_get_visible_child_name(stack) ||
        !gtk_widget_activate_action(GTK_WIDGET(window), "account.refresh", NULL))
      smoke_status = 1;
#if GROUNDHOG_HAVE_INBOX
    /* The conversation list is bound to the process store. */
    if (!gtk_list_view_get_factory(gh_sidebar_page_get_list(gh_window_get_sidebar(GH_WINDOW(window)))))
      smoke_status = 1;
#endif
#if GROUNDHOG_HAVE_BACKGROUND
    /* Background delivery is up (onboarding and Preferences find it). */
    if (!gh_background_get_for_application(G_APPLICATION(app)))
      smoke_status = 1;
#endif
#if GROUNDHOG_HAVE_ACCOUNT_STORE
    /* The store banners' buttons have somewhere to go. */
    if (!gh_app_services_get_account_store(app_services) ||
        !g_action_map_lookup_action(G_ACTION_MAP(app), "store-unlock") ||
        !g_action_map_lookup_action(G_ACTION_MAP(app), "store-retry") ||
        !g_action_map_lookup_action(G_ACTION_MAP(app), "store-continue-without-saving"))
      smoke_status = 1;
#endif
#else
    if (g_strcmp0(gtk_stack_get_visible_child_name(stack), "onboarding") != 0)
      smoke_status = 1;
#endif
  }
  if (window)
    gtk_window_destroy(window);
  g_application_quit(app);
  return G_SOURCE_REMOVE;
}

static void
app_startup(GApplication *app, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  (void)user_data;
  gh_window_setup_application(GTK_APPLICATION(app));
  app_services = gh_app_services_new(GTK_APPLICATION(app), &error);
  if (!app_services) {
    g_printerr("%s\n", error->message);
    smoke_status = 1;
    g_application_quit(app);
    return;
  }
#if defined(GH_MLS_TEST_HOOKS) && GROUNDHOG_HAVE_MLS
  if (g_strcmp0(g_getenv("GH_TEST_CONTROL"), "1") == 0) {
    GDBusConnection *bus = g_application_get_dbus_connection(app);
    g_message("TestControl: startup bus=%p", (void *)bus);
    if (bus)
      gh_test_control_register(app_services, bus);
  }
#endif
}

/* Tears the services down in reverse order: the account's store closes
 * after its inbox and outbox stopped, before the account is revoked. */
static void
app_shutdown(GApplication *app, gpointer user_data)
{
  (void)app;
  (void)user_data;
#if defined(GH_MLS_TEST_HOOKS) && GROUNDHOG_HAVE_MLS
  if (g_strcmp0(g_getenv("GH_TEST_CONTROL"), "1") == 0)
    gh_test_control_unregister();
#endif
  g_clear_pointer(&app_services, gh_app_services_free);
}

/* SIGTERM (the session ending, `kill`) and SIGINT run the normal shutdown:
 * scopes close and the store checkpoints, as for app.quit (charter §5.3 B3). */
static gboolean
on_terminate(gpointer data)
{
  g_application_quit(G_APPLICATION(data));
  return G_SOURCE_CONTINUE;
}

/* Not called in service mode (`--gapplication-service`, the autostart
 * command): the process then runs windowless until something activates it,
 * held by the background service while run-in-background is on. */
static void
activate(GApplication *app, gpointer user_data)
{
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(app));
  (void)user_data;
  if (!window) {
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gh_about_dialog_register_icons();
    const gchar *app_id = g_application_get_application_id(app);
    gtk_window_set_default_icon_name(app_id ? app_id : GROUNDHOG_APP_ID);
    window = GTK_WINDOW(create_window(ADW_APPLICATION(app)));
  }
  gtk_window_present(window);
  if (smoke_mode)
    g_idle_add(smoke_check, app);
}

int
main(int argc, char **argv)
{
  GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
  g_autoptr(AdwApplication) app = NULL;
  int status;

  /* Instance support (nostrc-lrac): must run before any GLib function that
   * caches XDG directories. Uses only libc, not GLib. */
  instance_name = setup_instance(argc, argv);

  /* --version is handled locally, with or without --instance. Other options
   * still reach GApplication's parser rather than being silently ignored. */
  gboolean version_only = FALSE;
  gboolean smoke_requested = FALSE;
  gboolean known_version_args = TRUE;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0)
      version_only = TRUE;
    else if (strcmp(argv[i], "--smoke") == 0) {
      smoke_requested = TRUE;
      known_version_args = FALSE;
    } else if (strcmp(argv[i], "--instance") == 0)
      i++;  /* setup_instance() already validated the following name. */
    else if (strncmp(argv[i], "--instance=", 11) != 0)
      known_version_args = FALSE;
  }
  if (version_only && known_version_args) {
    g_print("Groundhog %s\n", GROUNDHOG_VERSION);
    return 0;
  }

  /* D-Bus probe (nostrc-v59q): detect a stalled session bus before
   * any GLib or GTK function that might connect to D-Bus. On Linux,
   * gtk_init_check() initialises AT-SPI accessibility via the session
   * bus, so a stalled bus would hang there before the probe ever ran.
   * Must come before the --smoke block's gtk_init_check(). */
  probe_session_bus(&flags);
  gh_status_set_bus_unresponsive(bus_fallback);

  if (smoke_requested) {
    smoke_mode = TRUE;
#ifdef __APPLE__
    /* CLI test runners may not own a macOS WindowServer session. */
    const char *gui_smoke = g_getenv("GROUNDHOG_RUN_GUI_SMOKE");
    if (!gui_smoke || !g_str_equal(gui_smoke, "1")) {
      g_printerr("Groundhog launch smoke skipped: no requested macOS GUI session\n");
      return 77;
    }
#endif
    if (!gtk_init_check()) {
      g_printerr("Groundhog launch smoke skipped: no graphical display\n");
      return 77;
    }
    flags |= G_APPLICATION_NON_UNIQUE;
  }

#if GROUNDHOG_HAVE_ACCOUNTS
  {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    g_autoptr(GSettingsSchema) schema =
      source ? g_settings_schema_source_lookup(source, GROUNDHOG_APP_ID, TRUE) : NULL;
    if (!schema) {
      g_printerr("Groundhog settings schema %s is not installed\n", GROUNDHOG_APP_ID);
      return 1;
    }
  }
#endif

  const char *app_id = app_id_for_instance(instance_name);
  if (instance_name)
    g_message("Groundhog instance '%s' (app id %s)", instance_name, app_id);

  groundhog_register_resource();
  app = adw_application_new(app_id, flags);
  g_application_add_main_option_entries(G_APPLICATION(app), instance_options);
  /* Logout ends the session's clients: GTK quits on the session manager's
   * EndSession/Stop, so a background process shuts down cleanly too. */
  g_object_set(app, "register-session", TRUE, NULL);
  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  guint sigterm = g_unix_signal_add(SIGTERM, on_terminate, app);
  guint sigint = g_unix_signal_add(SIGINT, on_terminate, app);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_source_remove(sigterm);
  g_source_remove(sigint);
#ifdef GH_TEST_FONTCONFIG_CLEANUP
  if (g_strcmp0(g_getenv("GH_TEST_CONTROL"), "1") == 0)
    FcFini();
#endif
  /* smoke_status is also set when the services could not start. */
  return smoke_status != 0 ? smoke_status : status;
}
