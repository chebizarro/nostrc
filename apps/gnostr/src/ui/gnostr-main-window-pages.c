#define G_LOG_DOMAIN "gnostr-main-window-pages"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "page-discover.h"
#include "gnostr-classifieds-view.h"

void
gnostr_main_window_on_stack_visible_child_changed_internal(GObject *stack,
                                                           GParamSpec *pspec,
                                                           gpointer user_data)
{
  (void)pspec;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GtkWidget *visible_child = NULL;
  if (ADW_IS_VIEW_STACK(stack))
    visible_child = adw_view_stack_get_visible_child(ADW_VIEW_STACK(stack));
  else if (GTK_IS_STACK(stack))
    visible_child = gtk_stack_get_visible_child(GTK_STACK(stack));

  GtkWidget *discover_page = self->session_view ? gnostr_session_view_get_discover_page(self->session_view) : NULL;
  if (visible_child == discover_page && discover_page && GNOSTR_IS_PAGE_DISCOVER(discover_page))
    gnostr_page_discover_load_profiles(GNOSTR_PAGE_DISCOVER(discover_page));

  GtkWidget *classifieds_view = self->session_view ? gnostr_session_view_get_classifieds_view(self->session_view) : NULL;
  if (visible_child == classifieds_view && classifieds_view && GNOSTR_IS_CLASSIFIEDS_VIEW(classifieds_view))
    gnostr_classifieds_view_fetch_listings(GNOSTR_CLASSIFIEDS_VIEW(classifieds_view));
}

void
gnostr_main_window_set_page(GnostrMainWindow *self, GnostrMainWindowPage page)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  const char *name = NULL;
  switch (page) {
    case GNOSTR_MAIN_WINDOW_PAGE_LOADING: name = "loading"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_SESSION: name = "session"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_LOGIN: name = "login"; break;
    case GNOSTR_MAIN_WINDOW_PAGE_ERROR: name = "error"; break;
    default: break;
  }

  if (name && self->main_stack)
    gtk_stack_set_visible_child_name(self->main_stack, name);
}
