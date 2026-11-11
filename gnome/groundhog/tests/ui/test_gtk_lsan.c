/* nostrc-swjdk / nostrc-4bi6r: dependency ownership regression controls.
 * These allocations grow per widget on unpatched Noble. Only font caches may
 * be suppressed; the normal Groundhog GUI suites still exercise their real
 * accessibility backends and breakpoint/render-node teardown paths. */
#include <adwaita.h>
#include "nostrc-test-gdk-frame.h"

static void
test_mnemonic_relations(void)
{
  for (guint i = 0; i < 100; i++) {
    g_autoptr(GtkWidget) row = g_object_ref_sink(adw_action_row_new());
    g_autoptr(GtkWidget) button = g_object_ref_sink(gtk_button_new_with_mnemonic("_Button"));
    g_assert_nonnull(row);
    g_assert_nonnull(button);
  }
}

static void
test_shortcut_relations(void)
{
  for (guint i = 0; i < 100; i++) {
    g_autoptr(GtkWidget) label = g_object_ref_sink(gtk_shortcut_label_new("<Control>n"));
    g_assert_nonnull(label);
  }
}

static void
test_legal_sections(void)
{
  for (guint i = 0; i < 100; i++) {
    g_autoptr(AdwDialog) dialog = g_object_ref_sink(adw_about_dialog_new());
    adw_about_dialog_add_legal_section(ADW_ABOUT_DIALOG(dialog), "Test component",
                                      "Test copyright", GTK_LICENSE_MIT_X11, NULL);
  }
}

int
main(int argc, char **argv)
{
  /* Explicit negative path for GTK's NULL-context ownership contract, not a
   * global CI workaround: the original library must fail this control in LSan. */
  g_setenv("GTK_A11Y", "none", TRUE);
  g_test_init(&argc, &argv, NULL);
  if (!gtk_init_check()) {
    g_printerr("groundhog-gtk-lsan skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/groundhog/gtk-lsan/mnemonic-relations", test_mnemonic_relations);
  g_test_add_func("/groundhog/gtk-lsan/shortcut-relations", test_shortcut_relations);
  g_test_add_func("/groundhog/gtk-lsan/legal-sections", test_legal_sections);
  return g_test_run();
}
