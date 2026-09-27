#include "gh-shell.h"

GtkWidget *
gh_shell_status_page(const char *icon, const char *title, const char *description)
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

AdwNavigationPage *
gh_shell_sidebar_page(GtkWidget **header_out, GtkWidget **title_out, GtkWidget **stack_out)
{
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *title = adw_window_title_new("Groundhog", "");
  GtkWidget *stack = gtk_stack_new();
  GtkWidget *list = gtk_list_box_new();

  /* The list stays empty until a real conversation backend exists (see
   * main.c); without a name a screen reader would only ever report an
   * unlabelled, childless list here. */
  gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Conversations", -1);
  /* The stack itself is the region whose visible child changes as account
   * state changes (discovering, empty, error, onboarding); name it so that
   * region has an announced purpose distinct from the list it contains. */
  gtk_accessible_update_property(GTK_ACCESSIBLE(stack), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Conversation list status", -1);

  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

  /* Keep the list as the real empty model host; never fabricate conversations. */
  gtk_stack_add_named(GTK_STACK(stack), list, "conversations");
  gtk_stack_add_named(GTK_STACK(stack),
                      gh_shell_status_page("mail-unread-symbolic", "No conversations yet",
                                           "Messaging services are not implemented in this build, "
                                           "so no conversations can be loaded."),
                      "empty");
  gtk_stack_add_named(GTK_STACK(stack),
                      gh_shell_status_page("dialog-warning-symbolic", "Conversations unavailable",
                                           "A conversation could not be loaded. Nothing was sent; "
                                           "try again after the service is available."),
                      "error");
#if !GROUNDHOG_HAVE_ACCOUNTS
  gtk_stack_add_named(GTK_STACK(stack),
                      gh_shell_status_page("mail-unread-symbolic", "Welcome to Groundhog",
                                           "Account support is not included in this build."),
                      "onboarding");
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "onboarding");
#endif
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);
  *header_out = header;
  *title_out = title;
  *stack_out = stack;
  return adw_navigation_page_new(toolbar, "Conversations");
}

AdwNavigationPage *
gh_shell_content_page(GtkWidget **banner_out)
{
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *banner = adw_banner_new("Read-only shell: sending and receiving are not available");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
  gtk_box_append(GTK_BOX(box), banner);
  gtk_box_append(GTK_BOX(box),
                 gh_shell_status_page("mail-read-symbolic", "No conversation selected",
                                      "Messages will appear here when conversation services "
                                      "are implemented."));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
  *banner_out = banner;
  return adw_navigation_page_new(toolbar, "Messages");
}
