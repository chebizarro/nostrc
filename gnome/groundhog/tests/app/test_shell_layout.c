/* Adaptive-layout/accessibility check for the read-only shell templates
 * (data/ui/gh-window.blp, gh-sidebar-page.blp, gh-content-page.blp and
 * gh-onboarding-page.blp). It registers the compiled Groundhog GResource and
 * instantiates GhWindow/GhSidebarPage/GhContentPage exactly as the
 * application does, without GSettings or the signer. It proves that the
 * compiled templates build the expected widgets, stack pages, roles and
 * accessible labels without GLib/GTK criticals (G_DEBUG=fatal-criticals) in
 * both text directions, and that the 600sp breakpoint collapses the split
 * view in a mapped window. Keyboard-navigation order, screen-reader phrasing,
 * and high-contrast rendering still require a manual check on a real GNOME
 * session; see the bead notes for what remains.
 */
#include "gh-window.h"

void groundhog_register_resource(void);

static void
assert_label(gpointer accessible, const char *label)
{
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(accessible),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, label);
}

static void
assert_status_page(GtkWidget *page)
{
  g_assert_true(ADW_IS_STATUS_PAGE(page));
  g_assert_true(gtk_widget_has_css_class(page, "groundhog-shell-status"));
  g_assert_nonnull(adw_status_page_get_title(ADW_STATUS_PAGE(page)));
  g_assert_nonnull(adw_status_page_get_description(ADW_STATUS_PAGE(page)));
}

static void
assert_stack_pages(GtkStack *stack, gboolean onboarding)
{
  static const char *expected[] = {"conversations", "empty", "error"};
  guint n_pages = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(stack)); c;
       c = gtk_widget_get_next_sibling(c))
    n_pages++;

  for (guint i = 0; i < G_N_ELEMENTS(expected); i++)
    g_assert_nonnull(gtk_stack_get_child_by_name(stack, expected[i]));
  assert_status_page(gtk_stack_get_child_by_name(stack, "empty"));
  assert_status_page(gtk_stack_get_child_by_name(stack, "error"));
  if (onboarding) {
    assert_status_page(gtk_stack_get_child_by_name(stack, "onboarding"));
    /* The account-free build (GROUNDHOG_HAVE_ACCOUNTS=0) shows onboarding. */
    g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "onboarding");
    g_assert_cmpuint(n_pages, ==, 4);
  } else {
    g_assert_null(gtk_stack_get_child_by_name(stack, "onboarding"));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversations");
    g_assert_cmpuint(n_pages, ==, 3);
  }
}

static void
test_sidebar_page_structure(void)
{
  GhSidebarPage *page = g_object_ref_sink(g_object_new(GH_TYPE_SIDEBAR_PAGE, NULL));
  GtkStack *stack = gh_sidebar_page_get_stack(page);

  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Conversations");
  g_assert_true(ADW_IS_HEADER_BAR(gh_sidebar_page_get_header(page)));
  g_assert_true(adw_header_bar_get_title_widget(gh_sidebar_page_get_header(page)) ==
                GTK_WIDGET(gh_sidebar_page_get_window_title(page)));
  g_assert_cmpstr(adw_window_title_get_title(gh_sidebar_page_get_window_title(page)), ==,
                  "Groundhog");
  /* Standalone builds never set a subtitle; it stays empty, not unset. */
  g_assert_cmpstr(adw_window_title_get_subtitle(gh_sidebar_page_get_window_title(page)), ==, "");
  g_assert_true(GTK_IS_STACK(stack));
  assert_stack_pages(stack, FALSE);

  GtkWidget *list = gtk_stack_get_child_by_name(stack, "conversations");
  g_assert_true(GTK_IS_LIST_BOX(list));
  g_assert_null(gtk_widget_get_first_child(list));
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(list)), ==,
                  GTK_ACCESSIBLE_ROLE_LIST);
  /* The Blueprint accessibility blocks reach the widgets' AT context. */
  assert_label(list, "Conversations");
  assert_label(stack, "Conversation list status");

  gh_sidebar_page_show_onboarding(page);
  assert_stack_pages(stack, TRUE);

  g_object_unref(page);
}

static void
test_content_page_structure(void)
{
  GhContentPage *page = g_object_ref_sink(g_object_new(GH_TYPE_CONTENT_PAGE, NULL));
  AdwBanner *banner = gh_content_page_get_banner(page);

  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Messages");
  g_assert_true(ADW_IS_BANNER(banner));
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==,
                  "Read-only shell: sending and receiving are not available");
  assert_status_page(gtk_widget_get_next_sibling(GTK_WIDGET(banner)));

  g_object_unref(page);
}

static void
test_window_structure(void)
{
  GhWindow *window = gh_window_new(NULL);
  AdwNavigationSplitView *split = gh_window_get_split(window);

  g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(window)), ==, "Groundhog");
  g_assert_cmpstr(gtk_window_get_icon_name(GTK_WINDOW(window)), ==, "org.nostr.Groundhog");
  int width = 0, height = 0;
  gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
  g_assert_cmpint(width, ==, 900);
  g_assert_cmpint(height, ==, 600);

  g_assert_true(adw_application_window_get_content(ADW_APPLICATION_WINDOW(window)) ==
                GTK_WIDGET(gh_window_get_toasts(window)));
  g_assert_true(adw_toast_overlay_get_child(gh_window_get_toasts(window)) == GTK_WIDGET(split));
  g_assert_true(adw_navigation_split_view_get_sidebar(split) ==
                ADW_NAVIGATION_PAGE(gh_window_get_sidebar(window)));
  g_assert_true(adw_navigation_split_view_get_content(split) ==
                ADW_NAVIGATION_PAGE(gh_window_get_content(window)));
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  g_assert_false(adw_navigation_split_view_get_collapsed(split));
  assert_stack_pages(gh_sidebar_page_get_stack(gh_window_get_sidebar(window)), FALSE);

  gtk_window_destroy(GTK_WINDOW(window));
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
is_laid_out(GhWindow *window)
{
  return gtk_widget_get_mapped(GTK_WIDGET(window)) &&
         gtk_widget_get_width(GTK_WIDGET(window)) > 0;
}

static gboolean
is_collapsed(GhWindow *window)
{
  return is_laid_out(window) &&
         adw_navigation_split_view_get_collapsed(gh_window_get_split(window));
}

static void
spin_until_at(gboolean (*pred)(GhWindow *), GhWindow *window, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(window) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, window) spin_until_at((pred), (window), __LINE__)

/* A mapped window, sized so libadwaita can evaluate breakpoints. The
 * template itself has no minimum size (pre-existing, nostrc-qp24.8.4), for
 * which libadwaita logs a warning that g_test makes fatal; the GNOME HIG
 * minimum set here only avoids that and does not change the 600sp condition
 * under test. */
static GhWindow *
new_mapped_window(int width)
{
  GhWindow *window = gh_window_new(NULL);
  gtk_widget_set_size_request(GTK_WIDGET(window), 360, 294);
  if (width > 0)
    gtk_window_set_default_size(GTK_WINDOW(window), width, 600);
  gtk_window_present(GTK_WINDOW(window));
  return window;
}

/* The template's AdwBreakpoint collapses the split view below 600sp and
 * leaves it expanded at the 900px default width. */
static void
test_breakpoint_collapses_below_600sp(void)
{
  GhWindow *wide = new_mapped_window(0);
  spin_until(is_laid_out, wide);
  /* Let a pending breakpoint transition, if any, run before sampling. */
  for (int i = 0; i < 100 && g_main_context_iteration(NULL, FALSE); i++)
    ;
  g_assert_null(adw_application_window_get_current_breakpoint(ADW_APPLICATION_WINDOW(wide)));
  g_assert_false(adw_navigation_split_view_get_collapsed(gh_window_get_split(wide)));
  gtk_window_destroy(GTK_WINDOW(wide));

  GhWindow *narrow = new_mapped_window(400);
  spin_until(is_collapsed, narrow);
  AdwBreakpoint *breakpoint =
    adw_application_window_get_current_breakpoint(ADW_APPLICATION_WINDOW(narrow));
  g_assert_nonnull(breakpoint);
  g_autofree char *condition =
    adw_breakpoint_condition_to_string(adw_breakpoint_get_condition(breakpoint));
  g_assert_cmpstr(condition, ==, "max-width: 600sp");
  gtk_window_destroy(GTK_WINDOW(narrow));
}

/* Building the same templates under a forced RTL default direction must not
 * trip a GTK critical. This proves construction is bidi-safe; it cannot
 * prove correct visual mirroring, which still needs a manual GNOME check
 * with an RTL locale. */
static void
test_rtl_construction_is_safe(void)
{
  GtkTextDirection previous = gtk_widget_get_default_direction();
  gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);

  GhWindow *window = gh_window_new(NULL);
  GhSidebarPage *sidebar = gh_window_get_sidebar(window);
  gh_sidebar_page_show_onboarding(sidebar);
  assert_stack_pages(gh_sidebar_page_get_stack(sidebar), TRUE);
  g_assert_nonnull(gh_window_get_content(window));
  gtk_window_destroy(GTK_WINDOW(window));

  gtk_widget_set_default_direction(previous);
}

int
main(int argc, char **argv)
{
  /* Matches groundhog-launch's own skip convention (see CMakeLists.txt):
   * a missing display is an environment limitation, not a test failure. */
  if (!gtk_init_check()) {
    g_printerr("groundhog-shell test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/shell/sidebar-page-structure", test_sidebar_page_structure);
  g_test_add_func("/groundhog/shell/content-page-structure", test_content_page_structure);
  g_test_add_func("/groundhog/shell/window-structure", test_window_structure);
  g_test_add_func("/groundhog/shell/breakpoint-collapses-below-600sp",
                  test_breakpoint_collapses_below_600sp);
  g_test_add_func("/groundhog/shell/rtl-construction-is-safe", test_rtl_construction_is_safe);
  return g_test_run();
}
