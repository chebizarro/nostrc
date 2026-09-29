#include <adwaita.h>

#include "gh-app-services.h"
#include "gh-window.h"

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

void groundhog_register_resource(void);

static gboolean smoke_mode = FALSE;
static int smoke_status = 0;
/* Every process-owned service (account, relays, conversations, inbox, the
 * encrypted store and its outbox) lives in the container; see
 * gh-app-services.h. */
static GhAppServices *app_services;

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
  g_signal_connect(app, "startup", G_CALLBACK(app_startup), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  /* smoke_status is also set when the services could not start. */
  return smoke_status != 0 ? smoke_status : status;
}
