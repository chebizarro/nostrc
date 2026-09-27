#include <adwaita.h>

#define GROUNDHOG_APP_ID "org.nostr.Groundhog"

GResource *groundhog_get_resource(void);

static gboolean smoke_mode = FALSE;
static int smoke_status = 0;

static GtkWidget *
status_page(const char *icon, const char *title, const char *description)
{
  GtkWidget *page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon);
  adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
  adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
  gtk_widget_add_css_class(page, "groundhog-shell-status");
  gtk_widget_set_hexpand(page, TRUE);
  gtk_widget_set_vexpand(page, TRUE);
  return page;
}

static AdwNavigationPage *
sidebar_page(void)
{
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *title = gtk_label_new("Groundhog");
  GtkWidget *stack = gtk_stack_new();
  GtkWidget *list = gtk_list_box_new();

  gtk_widget_add_css_class(title, "title");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

  /* Keep the list as the real empty model host; never fabricate conversations. */
  gtk_stack_add_named(GTK_STACK(stack), list, "conversations");
  gtk_stack_add_named(GTK_STACK(stack),
                      status_page("mail-unread-symbolic", "No conversations yet",
                                  "Conversations will appear here after an account is connected."),
                      "empty");
  gtk_stack_add_named(GTK_STACK(stack),
                      status_page("dialog-warning-symbolic", "Conversations unavailable",
                                  "A conversation could not be loaded. Nothing was sent; "
                                  "try again after the service is available."),
                      "error");
  gtk_stack_add_named(GTK_STACK(stack),
                      status_page("mail-unread-symbolic", "Welcome to Groundhog",
                                  "No account is connected. Identity setup and messaging "
                                  "will be available in a later build."),
                      "onboarding");
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "onboarding");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);
  return adw_navigation_page_new(toolbar, "Conversations");
}

static AdwNavigationPage *
content_page(void)
{
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *banner = adw_banner_new("Read-only shell: sending and receiving are not available");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
  gtk_box_append(GTK_BOX(box), banner);
  gtk_box_append(GTK_BOX(box),
                 status_page("mail-read-symbolic", "No conversation selected",
                             "Messages will appear here when account and conversation "
                             "services are implemented."));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
  return adw_navigation_page_new(toolbar, "Messages");
}

static GtkWidget *
create_window(AdwApplication *app)
{
  GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));
  GtkWidget *split = adw_navigation_split_view_new();
  AdwBreakpointCondition *condition;
  AdwBreakpoint *breakpoint;
  GValue collapsed = G_VALUE_INIT;

  gtk_window_set_title(GTK_WINDOW(window), "Groundhog");
  gtk_window_set_icon_name(GTK_WINDOW(window), GROUNDHOG_APP_ID);
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split), sidebar_page());
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split), content_page());
  adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split), FALSE);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), split);

  condition = adw_breakpoint_condition_parse("max-width: 600sp");
  breakpoint = adw_breakpoint_new(condition);
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), breakpoint);

  g_object_set_data(G_OBJECT(window), "groundhog-split", split);
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
  }
  if (window)
    gtk_window_destroy(window);
  g_application_quit(app);
  return G_SOURCE_REMOVE;
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

  g_resources_register(groundhog_get_resource());
  app = adw_application_new(GROUNDHOG_APP_ID, flags);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  return smoke_mode && smoke_status != 0 ? smoke_status : status;
}
