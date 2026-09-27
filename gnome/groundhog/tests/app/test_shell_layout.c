/* Headless adaptive-layout/accessibility check for the read-only shell chrome
 * in src/app/gh-shell.c. This never loads the compiled GResource, GSettings,
 * or the signer, so it needs no installed schema and cannot claim anything
 * about a real GNOME session: it only proves that the sidebar/content
 * widgets, their stack pages, and their accessible-name calls construct
 * cleanly (no GLib/GTK criticals under G_DEBUG=fatal-criticals) in both
 * left-to-right and right-to-left text direction. Keyboard-navigation order,
 * screen-reader phrasing, and high-contrast rendering still require a manual
 * check on a real GNOME session; see the bead notes for what remains.
 */
#include "gh-shell.h"

static void
assert_stack_pages(GtkStack *stack)
{
  static const char *expected[] = {"conversations", "empty", "error", "onboarding"};
  for (guint i = 0; i < G_N_ELEMENTS(expected); i++)
    g_assert_nonnull(gtk_stack_get_child_by_name(stack, expected[i]));
  /* Built with GROUNDHOG_HAVE_ACCOUNTS=0 (see CMakeLists.txt), so the
   * account-free onboarding page is the initial visible child, exactly as
   * a standalone (no in-tree nostr/nip19) build behaves. */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "onboarding");
}

static void
test_sidebar_page_structure(void)
{
  GtkWidget *header, *title, *stack;
  AdwNavigationPage *page = gh_shell_sidebar_page(&header, &title, &stack);

  g_assert_nonnull(page);
  g_assert_true(ADW_IS_HEADER_BAR(header));
  g_assert_true(ADW_IS_WINDOW_TITLE(title));
  g_assert_true(GTK_IS_STACK(stack));
  assert_stack_pages(GTK_STACK(stack));

  GtkWidget *list = gtk_stack_get_child_by_name(GTK_STACK(stack), "conversations");
  g_assert_true(GTK_IS_LIST_BOX(list));
  /* GTK has no public getter for a pushed accessible property value (it is
   * handed to the platform AT context, not cached for readback), so the
   * accessible-name calls in gh_shell_sidebar_page are only verified here by
   * not tripping a GTK critical over an invalid property-for-role pairing;
   * a real screen reader still needs a manual GNOME check. */
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(list)), ==,
                  GTK_ACCESSIBLE_ROLE_LIST);

  g_object_ref_sink(page);
  g_object_unref(page);
}

static void
test_content_page_structure(void)
{
  GtkWidget *banner;
  AdwNavigationPage *page = gh_shell_content_page(&banner);

  g_assert_nonnull(page);
  g_assert_true(ADW_IS_BANNER(banner));
  g_assert_true(adw_banner_get_revealed(ADW_BANNER(banner)));
  g_assert_cmpstr(adw_banner_get_title(ADW_BANNER(banner)), ==,
                  "Read-only shell: sending and receiving are not available");

  g_object_ref_sink(page);
  g_object_unref(page);
}

/* Building the same shell chrome under a forced RTL default direction must
 * not trip a GTK critical. This proves construction is bidi-safe; it cannot
 * prove correct visual mirroring, which still needs a manual GNOME check
 * with an RTL locale. */
static void
test_rtl_construction_is_safe(void)
{
  GtkTextDirection previous = gtk_widget_get_default_direction();
  gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);

  GtkWidget *header, *title, *stack, *banner;
  AdwNavigationPage *sidebar = gh_shell_sidebar_page(&header, &title, &stack);
  AdwNavigationPage *content = gh_shell_content_page(&banner);

  g_assert_nonnull(sidebar);
  g_assert_nonnull(content);
  assert_stack_pages(GTK_STACK(stack));

  g_object_ref_sink(sidebar);
  g_object_unref(sidebar);
  g_object_ref_sink(content);
  g_object_unref(content);

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

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/shell/sidebar-page-structure", test_sidebar_page_structure);
  g_test_add_func("/groundhog/shell/content-page-structure", test_content_page_structure);
  g_test_add_func("/groundhog/shell/rtl-construction-is-safe", test_rtl_construction_is_safe);
  return g_test_run();
}
