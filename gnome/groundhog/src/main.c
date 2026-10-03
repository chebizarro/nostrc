#include "ui/gh-about-dialog.h"
#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>

#include "gh-app-services.h"
#if GROUNDHOG_HAVE_BACKGROUND
#include "gh-background.h"
#endif
#include "gh-window.h"

/* nostrc-v59q: the D-Bus probe uses low-level sockets. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
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

/* ---- Instance support (nostrc-lrac) ----------------------------------------
 *
 * --instance NAME (or GROUNDHOG_INSTANCE=NAME) runs Groundhog with:
 *   - app id  org.nostr.Groundhog.NAME
 *   - XDG dirs  under <original>/.groundhog-instances/NAME
 *   - GSettings  keyfile backend (portable, no dconf needed)
 *   - encrypted store, autostart, state  follow XDG dirs automatically
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
  /* 1. Check --instance NAME (must be before GLib touches the env). */
  const char *name = NULL;
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], "--instance") == 0) {
      name = argv[i + 1];
      /* Remove from argv so GApplication doesn't see it. */
      for (int j = i; j < argc - 2; j++)
        argv[j] = argv[j + 2];
      argc -= 2;
      /* Patch argc through argv[0] convention — the caller's argc
       * is not a pointer here; we'll return and let main() handle it. */
      break;
    }
  }

  /* 2. Fall back to GROUNDHOG_INSTANCE env. */
  if (!name)
    name = getenv("GROUNDHOG_INSTANCE");

  if (!name || !name[0])
    return NULL;

  if (!valid_instance_name(name)) {
    fprintf(stderr, "Groundhog: invalid instance name '%s' "
            "(1-32 chars, alphanumeric/underscore, starts with letter)\n", name);
    return NULL;  /* Continue as default instance. */
  }

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

  /* 4. Use the keyfile GSettings backend so dconf isolation isn't needed.
   * Don't override if the caller already chose a backend (e.g. "memory"
   * for test runners). */
  if (!getenv("GSETTINGS_BACKEND"))
    setenv("GSETTINGS_BACKEND", "keyfile", 1);

  return name;
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

/* ---- D-Bus probe (nostrc-v59q) -------------------------------------------
 *
 * The session bus might accept the socket (macOS launchd holds it) but
 * never answer the D-Bus AUTH handshake. g_application_register() calls
 * g_bus_get_sync() which calls _g_dbus_auth_run_client() which blocks in
 * select() with no timeout — the process hangs forever, never shows a
 * window, and ignores SIGTERM.
 *
 * This probe opens the Unix socket, sends the D-Bus AUTH null byte, and
 * waits for a response with a bounded timeout. If the daemon never
 * answers, the caller clears DBUS_SESSION_BUS_ADDRESS so GApplication
 * registers locally with NON_UNIQUE. The existing SIGNER_NO_BUS banner
 * tells the user. */

/* Extract the Unix socket path from a D-Bus address of the form
 * "unix:path=/foo/bar[,guid=...]". Returns a newly allocated string,
 * or NULL for TCP, abstract or unrecognised addresses. */
static gchar *
dbus_unix_path(const gchar *address)
{
  const gchar *p = strstr(address, "unix:path=");
  if (!p)
    return NULL;
  p += strlen("unix:path=");
  const gchar *end = strpbrk(p, ",;");
  return end ? g_strndup(p, (gsize)(end - p)) : g_strdup(p);
}

/* Returns TRUE if the session bus daemon answers the D-Bus AUTH handshake
 * within timeout_ms milliseconds. FALSE means the bus is unreachable or
 * stalled, and g_bus_get_sync() would hang. */
static gboolean
session_bus_responds(guint timeout_ms)
{
  /* Resolve the bus address without connecting. On macOS this queries
   * launchd for DBUS_LAUNCHD_SESSION_BUS_SOCKET; it does not block. */
  g_autoptr(GError) error = NULL;
  g_autofree gchar *address =
    g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!address)
    return FALSE;

  g_autofree gchar *path = dbus_unix_path(address);
  if (!path)
    return TRUE; /* TCP or abstract: assume alive, don't block. */

  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return FALSE;

  /* Open a non-blocking Unix socket. */
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return FALSE;

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return FALSE;
  }

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  g_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));

  int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  if (r != 0 && errno != EINPROGRESS) {
    close(fd);
    return FALSE;
  }

  /* Wait for the connect to complete. */
  struct pollfd pfd = { .fd = fd, .events = POLLOUT };
  if (poll(&pfd, 1, (int)timeout_ms) <= 0) {
    close(fd);
    return FALSE;
  }

  /* Verify the connection actually succeeded. */
  int so_error = 0;
  socklen_t optlen = sizeof(so_error);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
  if (so_error != 0) {
    close(fd);
    return FALSE;
  }

  /* Send the D-Bus AUTH null byte — the very first byte a D-Bus client
   * sends. A live daemon responds with "REJECTED ..." within
   * milliseconds. A stalled socket (launchd placeholder) never answers. */
  char nul = '\0';
  if (write(fd, &nul, 1) < 0) {
    close(fd);
    return FALSE;
  }

  /* Wait for any response. */
  pfd.events = POLLIN;
  r = poll(&pfd, 1, (int)timeout_ms);
  close(fd);
  return r > 0;
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

  /* Give the daemon 5 seconds. A local Unix socket responds in <10 ms
   * when alive; 5 s is generous for CI, slow VMs and D-Bus broker startup. */
  if (session_bus_responds(5000))
    return;

  /* The bus is stalled or unreachable. Point the address at a path
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
  }
}

/* Tears the services down in reverse order: the account's store closes
 * after its inbox and outbox stopped, before the account is revoked. */
static void
app_shutdown(GApplication *app, gpointer user_data)
{
  (void)app;
  (void)user_data;
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

  if (argc == 2 && g_str_equal(argv[1], "--version")) {
    g_print("Groundhog %s\n", GROUNDHOG_VERSION);
    return 0;
  }

  /* Instance support (nostrc-lrac): must run before any GLib function that
   * caches XDG directories. Uses only libc, not GLib. */
  instance_name = setup_instance(argc, argv);

  /* Re-count argc: setup_instance may have removed --instance NAME. */
  {
    int new_argc = 0;
    while (argv[new_argc])
      new_argc++;
    argc = new_argc;
  }

  /* D-Bus probe (nostrc-v59q): detect a stalled session bus before
   * any GLib or GTK function that might connect to D-Bus. On Linux,
   * gtk_init_check() initialises AT-SPI accessibility via the session
   * bus, so a stalled bus would hang there before the probe ever ran.
   * Must come before the --smoke block's gtk_init_check(). */
  probe_session_bus(&flags);

  if (argc == 2 && g_str_equal(argv[1], "--smoke")) {
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
    argc = 1;
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
  /* smoke_status is also set when the services could not start. */
  return smoke_status != 0 ? smoke_status : status;
}
