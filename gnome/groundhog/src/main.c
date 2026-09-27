#include <adwaita.h>

#include "gh-shell.h"

#if GROUNDHOG_HAVE_ACCOUNTS
#include "gh-account-controller.h"
#include "gh-account-ui.h"
#endif
#if GROUNDHOG_HAVE_RELAYS
#include "gh-account-relays.h"
#endif

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

void groundhog_register_resource(void);

static gboolean smoke_mode = FALSE;
static int smoke_status = 0;
#if GROUNDHOG_HAVE_ACCOUNTS
/* Process-owned: the active account, signer and generation outlive windows. */
static GSettings *app_settings;
static GhAccountController *app_accounts;
#endif
#if GROUNDHOG_HAVE_RELAYS
/* Follows app_accounts' generation: the previous account's REQs are closed
 * before the next account's are opened. */
static GhAccountRelays *app_relays;
#endif

static GtkWidget *
create_window(AdwApplication *app)
{
  GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));
  GtkWidget *split = adw_navigation_split_view_new();
  GtkWidget *toasts = adw_toast_overlay_new();
  GtkWidget *header, *title, *stack, *banner;
  AdwBreakpointCondition *condition;
  AdwBreakpoint *breakpoint;
  GValue collapsed = G_VALUE_INIT;

  gtk_window_set_title(GTK_WINDOW(window), "Groundhog");
  gtk_window_set_icon_name(GTK_WINDOW(window), GROUNDHOG_APP_ID);
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split),
                                        gh_shell_sidebar_page(&header, &title, &stack));
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split),
                                        gh_shell_content_page(&banner));
  adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split), FALSE);
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toasts), split);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toasts);
#if GROUNDHOG_HAVE_ACCOUNTS
  gh_account_ui_attach(window, app_accounts, app_settings, ADW_HEADER_BAR(header),
                       ADW_WINDOW_TITLE(title), GTK_STACK(stack), ADW_BANNER(banner),
                       ADW_TOAST_OVERLAY(toasts));
#else
  (void)header;
  (void)title;
  (void)stack;
#endif

  condition = adw_breakpoint_condition_parse("max-width: 600sp");
  breakpoint = adw_breakpoint_new(condition);
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), breakpoint);

  g_object_set_data(G_OBJECT(window), "groundhog-split", split);
  g_object_set_data(G_OBJECT(window), "groundhog-sidebar-stack", stack);
  return window;
}

static gboolean
smoke_check(gpointer user_data)
{
  GApplication *app = G_APPLICATION(user_data);
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(app));
  GtkWidget *split = window ? g_object_get_data(G_OBJECT(window), "groundhog-split") : NULL;

  if (!ADW_IS_NAVIGATION_SPLIT_VIEW(split) ||
      !adw_navigation_split_view_get_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split)) ||
      !adw_navigation_split_view_get_content(ADW_NAVIGATION_SPLIT_VIEW(split)))
    smoke_status = 1;
  else {
    /* The compact drill-down API must remain usable with an empty model. */
    adw_navigation_split_view_set_collapsed(ADW_NAVIGATION_SPLIT_VIEW(split), TRUE);
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split), TRUE);
    if (!adw_navigation_split_view_get_show_content(ADW_NAVIGATION_SPLIT_VIEW(split)))
      smoke_status = 1;
#if GROUNDHOG_HAVE_ACCOUNTS
    /* The sidebar always reflects an account state; never a blank stack. */
    GtkStack *stack = g_object_get_data(G_OBJECT(window), "groundhog-sidebar-stack");
    if (!gtk_stack_get_visible_child_name(stack) ||
        !gtk_widget_activate_action(GTK_WIDGET(window), "account.refresh", NULL))
      smoke_status = 1;
#endif
  }
  if (window)
    gtk_window_destroy(window);
  g_application_quit(app);
  return G_SOURCE_REMOVE;
}

#if GROUNDHOG_HAVE_ACCOUNTS
static void
app_startup(GApplication *app, gpointer user_data)
{
  (void)user_data;
  app_settings = g_settings_new(GROUNDHOG_APP_ID);
  /* Without a session bus the signer is reported unreachable, not faked. */
  app_accounts = gh_account_controller_new(app_settings,
                                           g_application_get_dbus_connection(app));
#if GROUNDHOG_HAVE_RELAYS
  app_relays = gh_account_relays_new(app_accounts, app_settings, NULL, NULL);
#endif
}

static void
app_shutdown(GApplication *app, gpointer user_data)
{
  (void)app;
  (void)user_data;
#if GROUNDHOG_HAVE_RELAYS
  /* Closes the account's relay subscriptions before the account is revoked. */
  if (app_relays)
    g_object_run_dispose(G_OBJECT(app_relays));
  g_clear_object(&app_relays);
#endif
  /* Revokes the account generation even if a listing is still in flight. */
  if (app_accounts)
    g_object_run_dispose(G_OBJECT(app_accounts));
  g_clear_object(&app_accounts);
  g_clear_object(&app_settings);
}
#endif

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

  groundhog_register_resource();
  app = adw_application_new(GROUNDHOG_APP_ID, flags);
#if GROUNDHOG_HAVE_ACCOUNTS
  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown), NULL);
#endif
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  return smoke_mode && smoke_status != 0 ? smoke_status : status;
}
