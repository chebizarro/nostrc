/* gh-test-dialog.h — waiting for an AdwDialog to be shown (nostrc-qp24.88).
 * Header-only.
 *
 * adw_dialog_present() maps the dialog at once, but its sheet opens, and
 * maps the content, only two frame-clock ticks later. libadwaita 1.5 (Ubuntu
 * 24.04, the CI job, and Groundhog's minimum) loses an adw_dialog_close() or
 * adw_dialog_force_close() that comes before that: the sheet opens afterwards
 * and stays, and "closed" is never emitted. Later libadwaita closes a sheet
 * that never opened. A test that presents a dialog waits for
 * gh_test_dialog_shown() before it closes the dialog or acts on it, as a user
 * sees it first. */
#ifndef GH_TEST_DIALOG_H
#define GH_TEST_DIALOG_H

#include <adwaita.h>

static G_GNUC_UNUSED gboolean
gh_test_dialog_shown(gpointer dialog)
{
  GtkWidget *content = adw_dialog_get_child(ADW_DIALOG(dialog));
  return content && gtk_widget_get_mapped(content);
}

#endif
