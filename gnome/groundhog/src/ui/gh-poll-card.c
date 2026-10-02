#include "gh-poll-card.h"

#include <glib/gi18n.h>

/* ---- GhPollCard --------------------------------------------------------- */

struct _GhPollCard {
  GtkWidget parent_instance;
  GhMlsPoll *poll;
  gulong     tallies_handler;

  /* Widgets built in code. */
  GtkLabel  *question_label;
  GtkBox    *options_box;
  GtkLabel  *status_label;
  GPtrArray *option_buttons; /* GtkToggleButton* (weak) */
};

enum {
  PROP_0,
  PROP_POLL,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

enum {
  SIG_VOTE_CAST,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhPollCard, gh_poll_card, GTK_TYPE_WIDGET)

/* ---- helpers ------------------------------------------------------------ */

static gchar *
format_status(GhMlsPoll *poll)
{
  guint voters = gh_mls_poll_get_total_voters(poll);
  gint64 ends_at = gh_mls_poll_get_ends_at(poll);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gboolean open = gh_mls_poll_is_open(poll, now);
  const gchar *type_str = gh_mls_poll_get_poll_type(poll) == GH_MLS_POLL_MULTIPLE_CHOICE
                            ? _("Multiple choice") : _("Single choice");

  if (!open)
    return g_strdup_printf(g_dngettext(NULL, "%s · %u vote · Closed",
                                       "%s · %u votes · Closed", voters),
                           type_str, voters);

  if (ends_at > 0) {
    gint64 remaining = ends_at - now;
    if (remaining > 3600)
      return g_strdup_printf(g_dngettext(NULL, "%s · %u vote · Ends in %lldh",
                                         "%s · %u votes · Ends in %lldh", voters),
                             type_str, voters, (long long)(remaining / 3600));
    if (remaining > 60)
      return g_strdup_printf(g_dngettext(NULL, "%s · %u vote · Ends in %lldm",
                                         "%s · %u votes · Ends in %lldm", voters),
                             type_str, voters, (long long)(remaining / 60));
    return g_strdup_printf(g_dngettext(NULL, "%s · %u vote · Ending soon",
                                       "%s · %u votes · Ending soon", voters),
                           type_str, voters);
  }

  return g_strdup_printf(g_dngettext(NULL, "%s · %u vote",
                                     "%s · %u votes", voters),
                         type_str, voters);
}

static void
update_option_states(GhPollCard *self)
{
  if (!self->poll) return;
  guint n_options = gh_mls_poll_get_n_options(self->poll);
  guint total_voters = gh_mls_poll_get_total_voters(self->poll);
  const gchar *const *local = gh_mls_poll_get_local_selection(self->poll);
  gboolean has_voted = gh_mls_poll_has_voted(self->poll);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gboolean open = gh_mls_poll_is_open(self->poll, now);

  for (guint i = 0; i < n_options && i < self->option_buttons->len; i++) {
    GtkToggleButton *btn = g_ptr_array_index(self->option_buttons, i);
    const GhMlsPollTally *tally = gh_mls_poll_get_option(self->poll, i);

    /* Check if this option is locally selected. */
    gboolean selected = FALSE;
    if (local) {
      for (const gchar *const *s = local; *s; s++) {
        if (g_strcmp0(*s, tally->id) == 0) { selected = TRUE; break; }
      }
    }

    /* Build label: "Label" or "Label · 42%" when someone voted. */
    g_autofree gchar *label = NULL;
    if (has_voted || !open) {
      guint pct = total_voters > 0 ? (tally->votes * 100 / total_voters) : 0;
      label = g_strdup_printf("%s · %u%%  (%u)", tally->label, pct, tally->votes);
    } else {
      label = g_strdup(tally->label);
    }
    gtk_button_set_label(GTK_BUTTON(btn), label);

    /* Toggle state (without triggering handler). */
    g_signal_handlers_block_matched(btn, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    gtk_toggle_button_set_active(btn, selected);
    g_signal_handlers_unblock_matched(btn, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);

    /* Disable if poll is closed. */
    gtk_widget_set_sensitive(GTK_WIDGET(btn), open);

    /* Style: selected options get "suggested-action". */
    if (selected)
      gtk_widget_add_css_class(GTK_WIDGET(btn), "suggested-action");
    else
      gtk_widget_remove_css_class(GTK_WIDGET(btn), "suggested-action");
  }

  /* Update status label. */
  g_autofree gchar *status = format_status(self->poll);
  gtk_label_set_text(self->status_label, status);
}

static void
on_tallies_changed(GhMlsPoll *poll, GhPollCard *self)
{
  (void) poll;
  update_option_states(self);
}

static void
on_option_toggled(GtkToggleButton *btn, GhPollCard *self)
{
  if (!self->poll) return;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (!gh_mls_poll_is_open(self->poll, now)) return;

  GhMlsPollType poll_type = gh_mls_poll_get_poll_type(self->poll);

  /* Find which option index was toggled. */
  guint toggled_idx = UINT_MAX;
  for (guint i = 0; i < self->option_buttons->len; i++) {
    if (g_ptr_array_index(self->option_buttons, i) == btn) {
      toggled_idx = i;
      break;
    }
  }
  if (toggled_idx == UINT_MAX) return;

  const GhMlsPollTally *tally = gh_mls_poll_get_option(self->poll, toggled_idx);
  if (!tally) return;

  /* Build the selection array. */
  g_autoptr(GPtrArray) selected = g_ptr_array_new();

  if (poll_type == GH_MLS_POLL_SINGLE_CHOICE) {
    /* Single choice: just the toggled option. */
    if (gtk_toggle_button_get_active(btn))
      g_ptr_array_add(selected, (gpointer) tally->id);
  } else {
    /* Multiple choice: collect all active toggles. */
    for (guint i = 0; i < self->option_buttons->len; i++) {
      GtkToggleButton *b = g_ptr_array_index(self->option_buttons, i);
      if (gtk_toggle_button_get_active(b)) {
        const GhMlsPollTally *t = gh_mls_poll_get_option(self->poll, i);
        if (t) g_ptr_array_add(selected, (gpointer) t->id);
      }
    }
  }

  if (selected->len == 0) {
    /* Deselecting in single choice is invalid; re-check. */
    g_signal_handlers_block_matched(btn, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    gtk_toggle_button_set_active(btn, TRUE);
    g_signal_handlers_unblock_matched(btn, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    return;
  }

  /* Build NULL-terminated string array. */
  g_ptr_array_add(selected, NULL);
  const gchar **ids = (const gchar **) selected->pdata;
  g_signal_emit(self, signals[SIG_VOTE_CAST], 0, ids, selected->len - 1);
}

/* ---- rebuild ------------------------------------------------------------ */

static void
rebuild_options(GhPollCard *self)
{
  /* Clear existing option buttons. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->options_box))))
    gtk_box_remove(self->options_box, child);
  g_ptr_array_set_size(self->option_buttons, 0);

  if (!self->poll) {
    gtk_label_set_text(self->question_label, "");
    gtk_label_set_text(self->status_label, "");
    return;
  }

  gtk_label_set_text(self->question_label, gh_mls_poll_get_question(self->poll));

  guint n_options = gh_mls_poll_get_n_options(self->poll);
  for (guint i = 0; i < n_options; i++) {
    const GhMlsPollTally *tally = gh_mls_poll_get_option(self->poll, i);
    GtkWidget *btn = gtk_toggle_button_new_with_label(tally->label);
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "groundhog-poll-option");
    g_signal_connect(btn, "toggled", G_CALLBACK(on_option_toggled), self);
    gtk_box_append(self->options_box, btn);
    g_ptr_array_add(self->option_buttons, btn);
  }

  update_option_states(self);
}

static void
disconnect_poll(GhPollCard *self)
{
  if (self->poll && self->tallies_handler) {
    g_signal_handler_disconnect(self->poll, self->tallies_handler);
    self->tallies_handler = 0;
  }
  g_clear_object(&self->poll);
}

/* ---- GObject ------------------------------------------------------------ */

static void
gh_poll_card_dispose(GObject *obj)
{
  GhPollCard *self = GH_POLL_CARD(obj);
  disconnect_poll(self);

  /* Dispose child widgets. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self))))
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gh_poll_card_parent_class)->dispose(obj);
}

static void
gh_poll_card_finalize(GObject *obj)
{
  GhPollCard *self = GH_POLL_CARD(obj);
  g_clear_pointer(&self->option_buttons, g_ptr_array_unref);
  G_OBJECT_CLASS(gh_poll_card_parent_class)->finalize(obj);
}

static void
gh_poll_card_set_property(GObject *obj, guint prop_id, const GValue *val,
                          GParamSpec *pspec)
{
  GhPollCard *self = GH_POLL_CARD(obj);
  switch (prop_id) {
  case PROP_POLL:
    gh_poll_card_set_poll(self, g_value_get_object(val));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void
gh_poll_card_get_property(GObject *obj, guint prop_id, GValue *val,
                          GParamSpec *pspec)
{
  GhPollCard *self = GH_POLL_CARD(obj);
  switch (prop_id) {
  case PROP_POLL:
    g_value_set_object(val, self->poll);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void
gh_poll_card_class_init(GhPollCardClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS(klass);
  obj_class->dispose = gh_poll_card_dispose;
  obj_class->finalize = gh_poll_card_finalize;
  obj_class->set_property = gh_poll_card_set_property;
  obj_class->get_property = gh_poll_card_get_property;

  props[PROP_POLL] = g_param_spec_object("poll", NULL, NULL, GH_TYPE_MLS_POLL,
                                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(obj_class, N_PROPS, props);

  signals[SIG_VOTE_CAST] =
    g_signal_new("vote-cast", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                 G_TYPE_POINTER, G_TYPE_UINT);

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "groundhog-poll");
  gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_GROUP);
}

static void
gh_poll_card_init(GhPollCard *self)
{
  self->option_buttons = g_ptr_array_new();

  GtkBoxLayout *layout = GTK_BOX_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(self)));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);
  gtk_box_layout_set_spacing(layout, 6);

  /* Question label. */
  self->question_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->question_label, TRUE);
  gtk_label_set_wrap_mode(self->question_label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(self->question_label, 0);
  gtk_label_set_max_width_chars(self->question_label, 50);
  gtk_widget_add_css_class(GTK_WIDGET(self->question_label), "heading");
  gtk_widget_set_parent(GTK_WIDGET(self->question_label), GTK_WIDGET(self));

  /* Options container. */
  self->options_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  gtk_widget_set_parent(GTK_WIDGET(self->options_box), GTK_WIDGET(self));

  /* Status label. */
  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->status_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "dim-label");
  gtk_widget_set_parent(GTK_WIDGET(self->status_label), GTK_WIDGET(self));
}

/* ---- public ------------------------------------------------------------- */

GtkWidget *
gh_poll_card_new(void)
{
  return g_object_new(GH_TYPE_POLL_CARD, NULL);
}

void
gh_poll_card_set_poll(GhPollCard *self, GhMlsPoll *poll)
{
  g_return_if_fail(GH_IS_POLL_CARD(self));
  if (self->poll == poll) return;
  disconnect_poll(self);
  if (poll) {
    self->poll = g_object_ref(poll);
    self->tallies_handler = g_signal_connect(poll, "tallies-changed",
                                             G_CALLBACK(on_tallies_changed), self);
  }
  rebuild_options(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_POLL]);
}

GhMlsPoll *
gh_poll_card_get_poll(GhPollCard *self)
{
  g_return_val_if_fail(GH_IS_POLL_CARD(self), NULL);
  return self->poll;
}
