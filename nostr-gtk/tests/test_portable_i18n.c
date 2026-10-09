/* Portable nostr-gtk widgets translate through the "nostr-gtk" domain
 * (nostrc-gofet.10): the generated en@pseudo catalog loads, and widget labels
 * built after the domain is bound come from it. */
#include <nostr-gtk-1.0/gn-portable-i18n.h>
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdio.h>

#include "nostrc-test-gdk-frame.h"

static gboolean have_display;

static void
test_domain(void)
{
  g_assert_cmpstr(gn_portable_gettext_domain(), ==, "nostr-gtk");
  g_assert_cmpstr(g_dgettext(GN_PORTABLE_GETTEXT_DOMAIN, "Load preview"), ==,
                  "⟦Ļöàđ þŕéṽîéŵ⟧");
  /* The host default domain is untouched. */
  g_assert_cmpstr(gettext("Load preview"), ==, "Load preview");
}

static GtkWidget *
find_button(GtkWidget *root, const char *label)
{
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    if (GTK_IS_BUTTON(c) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(c)), label) == 0)
      return c;
    GtkWidget *found = find_button(c, label);
    if (found)
      return found;
  }
  return NULL;
}

static void
test_widget_labels(void)
{
  if (!have_display) {
    g_test_skip("no display");
    return;
  }
  GtkWidget *card = g_object_ref_sink(GTK_WIDGET(gn_og_preview_card_new()));
  g_assert_nonnull(find_button(card, "⟦Ļöàđ þŕéṽîéŵ⟧"));
  g_object_unref(card);
}

int
main(int argc, char **argv)
{
  static const char *const locales[] = { "en_US.UTF-8", "C.UTF-8", "C.utf8" };
  gboolean ok = FALSE;
  g_setenv("LANGUAGE", "en@pseudo", TRUE);
  for (gsize i = 0; i < G_N_ELEMENTS(locales) && !ok; i++) {
    g_setenv("LC_ALL", locales[i], TRUE);
    ok = setlocale(LC_ALL, "") != NULL;
  }
  if (!ok) {
    fprintf(stderr, "no non-C UTF-8 locale available; skipping\n");
    return 77;
  }
  gn_portable_gettext_domain();
  bindtextdomain(GN_PORTABLE_GETTEXT_DOMAIN, NOSTR_GTK_TEST_LOCALEDIR);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  have_display = gtk_init_check();
  g_test_add_func("/nostr-gtk/portable/i18n/domain", test_domain);
  g_test_add_func("/nostr-gtk/portable/i18n/widget-labels", test_widget_labels);
  return g_test_run();
}
