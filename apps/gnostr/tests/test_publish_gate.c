/*
 * test_publish_gate — per-note actions follow read-only mode (nostrc-46h7).
 *
 * Real NostrGtkNoteCardRow widgets. The actions' state is read back from
 * the card's buttons (the ones whose tooltip names the action or asks to
 * sign in), since the card has no getter for it:
 *   - blocking greys out the cards under the given root at once, and
 *     unblocking restores them for a signed-in user;
 *   - a card a view creates as "logged in" while blocked (nostr-gtk's
 *     thread view and profile pane do) is greyed out when it is mapped;
 *   - gnostr_publish_gate_effective_logged_in() for bind-time callers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/gnostr-publish-gate.h"

#include <gtk/gtk.h>
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <string.h>

extern GResource *nostr_gtk_get_resource(void);

static const char *const ACTIONS[] = { "Reply", "Repost", "Like", "Zap" };
#define SIGN_IN "Sign in to use this feature"

typedef struct { guint enabled, disabled; } Count;

static void
count_actions(GtkWidget *w, Count *c)
{
  const char *tip = gtk_widget_get_tooltip_text(w);
  if (tip && GTK_IS_BUTTON(w)) {
    gboolean action = g_strcmp0(tip, SIGN_IN) == 0;
    for (guint i = 0; i < G_N_ELEMENTS(ACTIONS) && !action; i++)
      action = g_strcmp0(tip, ACTIONS[i]) == 0;
    if (action) {
      if (gtk_widget_get_sensitive(w) && g_strcmp0(tip, SIGN_IN) != 0) c->enabled++;
      else c->disabled++;
    }
  }
  for (GtkWidget *ch = gtk_widget_get_first_child(w); ch; ch = gtk_widget_get_next_sibling(ch))
    count_actions(ch, c);
}

static gboolean
actions_enabled(NostrGtkNoteCardRow *row)
{
  Count c = { 0 };
  count_actions(GTK_WIDGET(row), &c);
  g_assert_cmpuint(c.enabled + c.disabled, >=, G_N_ELEMENTS(ACTIONS));
  g_assert_true(c.enabled == 0 || c.disabled == 0);
  return c.enabled > 0;
}

static void
wait_mapped(GtkWidget *w)
{
  for (int i = 0; i < 500 && !gtk_widget_get_mapped(w); i++)
    g_main_context_iteration(NULL, FALSE), g_usleep(2000);
}

static void
test_gate(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  g_settings_set_string(settings, "current-npub",
      "npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m");

  GtkWidget *win = gtk_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), box);
  NostrGtkNoteCardRow *a = nostr_gtk_note_card_row_new();
  nostr_gtk_note_card_row_set_logged_in(a, TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(a));
  gtk_window_present(GTK_WINDOW(win));
  wait_mapped(GTK_WIDGET(a));
  g_assert_true(actions_enabled(a));
  g_assert_null(gnostr_publish_gate_get_reason());
  g_assert_true(gnostr_publish_gate_effective_logged_in(TRUE));

  /* Read-only: the visible card greys out at once. */
  gnostr_publish_gate_set_reason("GNostr is read-only", win);
  g_assert_cmpstr(gnostr_publish_gate_get_reason(), ==, "GNostr is read-only");
  g_assert_false(actions_enabled(a));
  g_assert_false(gnostr_publish_gate_effective_logged_in(TRUE));
  g_assert_false(gnostr_publish_gate_effective_logged_in(FALSE));

  /* A view builds a new card as "logged in" meanwhile: mapping greys it. */
  NostrGtkNoteCardRow *b = nostr_gtk_note_card_row_new();
  nostr_gtk_note_card_row_set_logged_in(b, TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(b));
  wait_mapped(GTK_WIDGET(b));
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(b)));
  g_assert_false(actions_enabled(b));

  /* The signer is back: both come back for the signed-in user. */
  gnostr_publish_gate_set_reason(NULL, win);
  g_assert_true(actions_enabled(a));
  g_assert_true(actions_enabled(b));
  /* ... and the hook is gone: a card built as signed out stays so. */
  NostrGtkNoteCardRow *c = nostr_gtk_note_card_row_new();
  nostr_gtk_note_card_row_set_logged_in(c, TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(c));
  wait_mapped(GTK_WIDGET(c));
  g_assert_true(actions_enabled(c));

  /* Signed out while unblocking: cards follow the account, not the gate. */
  gnostr_publish_gate_set_reason("read-only", win);
  g_settings_set_string(settings, "current-npub", "");
  gnostr_publish_gate_set_reason(NULL, win);
  g_assert_false(actions_enabled(a));

  gtk_window_destroy(GTK_WINDOW(win));
}

int
main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  g_resources_register(nostr_gtk_get_resource());
  g_test_add_func("/publish-gate/note-cards", test_gate);
  return g_test_run();
}
