/* gnostr-publish-gate — see gnostr-publish-gate.h (nostrc-46h7). */
#include "gnostr-publish-gate.h"

#include <nostr-gtk-1.0/nostr-note-card-row.h>

/* Main context only. */
static char *s_reason;
static gulong s_map_hook;

const char *
gnostr_publish_gate_get_reason(void)
{
  return s_reason;
}

gboolean
gnostr_publish_gate_effective_logged_in(gboolean signed_in)
{
  return signed_in && s_reason == NULL;
}

/* The same test the views use when they build a card. */
static gboolean
signed_in_now(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  g_autofree char *npub = g_settings_get_string(settings, "current-npub");
  return npub && *npub;
}

static void
apply_to_tree(GtkWidget *widget, gboolean logged_in)
{
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(widget))
    nostr_gtk_note_card_row_set_logged_in(NOSTR_GTK_NOTE_CARD_ROW(widget), logged_in);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child))
    apply_to_tree(child, logged_in);
}

static gboolean
on_widget_map(GSignalInvocationHint *hint, guint n_params, const GValue *params,
              gpointer data)
{
  (void)hint;
  (void)data;
  if (n_params > 0 && s_reason) {
    GObject *obj = g_value_get_object(&params[0]);
    if (NOSTR_GTK_IS_NOTE_CARD_ROW(obj))
      nostr_gtk_note_card_row_set_logged_in(NOSTR_GTK_NOTE_CARD_ROW(obj), FALSE);
  }
  return TRUE; /* stay installed */
}

void
gnostr_publish_gate_set_reason(const char *reason, GtkWidget *root)
{
  if (reason && !*reason)
    reason = NULL;
  gboolean was_blocked = s_reason != NULL;
  if (g_strcmp0(s_reason, reason) == 0)
    return;
  g_free(s_reason);
  s_reason = g_strdup(reason);

  guint map_id = g_signal_lookup("map", GTK_TYPE_WIDGET);
  if (s_reason && !s_map_hook)
    s_map_hook = g_signal_add_emission_hook(map_id, 0, on_widget_map, NULL, NULL);
  else if (!s_reason && s_map_hook) {
    g_signal_remove_emission_hook(map_id, s_map_hook);
    s_map_hook = 0;
  }

  if (root && was_blocked != (s_reason != NULL))
    apply_to_tree(root, gnostr_publish_gate_effective_logged_in(signed_in_now()));
}
