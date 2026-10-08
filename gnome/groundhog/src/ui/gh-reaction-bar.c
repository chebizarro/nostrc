#include "gh-reaction-bar.h"
#include <glib/gi18n.h>

struct _GhReactionBar {
  GtkWidget parent_instance;
  GtkFlowBox *flow;
  GtkButton *add_button;
  GhReactionSummary *summary;
  gulong total_handler;
  gboolean rebuilding;
};

enum { SIGNAL_REACTION_TOGGLED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhReactionBar, gh_reaction_bar, GTK_TYPE_WIDGET)

/* A single chip: emoji + count, with a toggled-own style class. */
static GtkWidget *
make_chip(const GhReactionChip *chip)
{
  g_autofree gchar *label = chip->count > 1
    ? g_strdup_printf("%s %u", chip->emoji, chip->count)
    : g_strdup(chip->emoji);

  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "groundhog-reaction-chip");
  if (chip->is_own)
    gtk_widget_add_css_class(button, "groundhog-reaction-own");

  /* Accessible text: "thumbs up, 3, including you". */
  g_autofree gchar *desc = chip->is_own
    ? g_strdup_printf("%s, %u, %s", chip->emoji, chip->count, _("including you"))
    : g_strdup_printf("%s, %u", chip->emoji, chip->count);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, desc, -1);

  /* The emoji string is stored as the widget name for the click handler. */
  gtk_widget_set_name(button, chip->emoji);

  return button;
}

static void
chip_clicked(GtkButton *button, gpointer data)
{
  GhReactionBar *self = GH_REACTION_BAR(data);
  if (self->rebuilding)
    return;
  const gchar *emoji = gtk_widget_get_name(GTK_WIDGET(button));
  gboolean is_own = gtk_widget_has_css_class(GTK_WIDGET(button), "groundhog-reaction-own");
  /* Toggle: own chip → remove; non-own chip → add. */
  g_signal_emit(self, signals[SIGNAL_REACTION_TOGGLED], 0, emoji, !is_own);
}

static void
rebuild(GhReactionBar *self)
{
  self->rebuilding = TRUE;

  /* Remove all children from the flow box. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->flow))))
    gtk_flow_box_remove(self->flow, child);

  guint total = self->summary ? gh_reaction_summary_get_total_count(self->summary) : 0;
  gtk_widget_set_visible(GTK_WIDGET(self), total > 0);
  if (total == 0) {
    self->rebuilding = FALSE;
    return;
  }

  const GPtrArray *chips = gh_reaction_summary_get_chips(self->summary);
  if (chips) {
    for (guint i = 0; i < chips->len; i++) {
      GhReactionChip *chip = g_ptr_array_index(chips, i);
      GtkWidget *btn = make_chip(chip);
      g_signal_connect(btn, "clicked", G_CALLBACK(chip_clicked), self);
      gtk_flow_box_append(self->flow, btn);
    }
  }

  /* No "+" here (W33, owner): the message's react button is the one way to
   * add a reaction; a chip toggles your own. */

  self->rebuilding = FALSE;
}

static void
on_total_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
  (void)object;
  (void)pspec;
  rebuild(GH_REACTION_BAR(data));
}

/* ---- layout ----------------------------------------------------------------- */

/* GtkFlowBox asks for room for more chips than it holds, and packs them at
 * its start: on a meta line that leaves a gap between the chips and what
 * follows (nostrc-l1kn6.5). The bar's natural width is its chips' own, in
 * one line; narrower than that, the flow box wraps them as before. */
static void
gh_reaction_bar_measure(GtkWidget *widget, GtkOrientation orientation, int for_size,
                        int *minimum, int *natural, int *minimum_baseline,
                        int *natural_baseline)
{
  GhReactionBar *self = GH_REACTION_BAR(widget);
  gtk_widget_measure(GTK_WIDGET(self->flow), orientation, for_size, minimum, natural,
                     minimum_baseline, natural_baseline);
  if (orientation != GTK_ORIENTATION_HORIZONTAL)
    return;
  int line = 0;
  guint chips = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->flow)); child;
       child = gtk_widget_get_next_sibling(child)) {
    if (!gtk_widget_should_layout(child))
      continue;
    int child_natural = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &child_natural, NULL, NULL);
    line += child_natural;
    chips++;
  }
  if (chips > 1)
    line += (int)(chips - 1) * (int)gtk_flow_box_get_column_spacing(self->flow);
  *natural = MAX(*minimum, line);
}

static void
gh_reaction_bar_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate(GTK_WIDGET(GH_REACTION_BAR(widget)->flow), width, height, baseline, NULL);
}

static GtkSizeRequestMode
gh_reaction_bar_get_request_mode(GtkWidget *widget)
{
  return gtk_widget_get_request_mode(GTK_WIDGET(GH_REACTION_BAR(widget)->flow));
}

/* ---- GObject ---------------------------------------------------------------- */

static void
gh_reaction_bar_dispose(GObject *object)
{
  GhReactionBar *self = GH_REACTION_BAR(object);
  if (self->summary && self->total_handler) {
    g_signal_handler_disconnect(self->summary, self->total_handler);
    self->total_handler = 0;
  }
  g_clear_object(&self->summary);
  gtk_widget_unparent(GTK_WIDGET(self->flow));
  G_OBJECT_CLASS(gh_reaction_bar_parent_class)->dispose(object);
}

static void
gh_reaction_bar_class_init(GhReactionBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_reaction_bar_dispose;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->measure = gh_reaction_bar_measure;
  widget_class->size_allocate = gh_reaction_bar_size_allocate;
  widget_class->get_request_mode = gh_reaction_bar_get_request_mode;
  gtk_widget_class_set_css_name(widget_class, "groundhog-reaction-bar");

  signals[SIGNAL_REACTION_TOGGLED] =
    g_signal_new("reaction-toggled",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

static void
gh_reaction_bar_init(GhReactionBar *self)
{
  self->flow = GTK_FLOW_BOX(gtk_flow_box_new());
  gtk_flow_box_set_selection_mode(self->flow, GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(self->flow, FALSE);
  gtk_flow_box_set_max_children_per_line(self->flow, 20);
  gtk_flow_box_set_column_spacing(self->flow, 4);
  gtk_flow_box_set_row_spacing(self->flow, 2);
  gtk_widget_set_halign(GTK_WIDGET(self->flow), GTK_ALIGN_FILL);
  gtk_widget_set_parent(GTK_WIDGET(self->flow), GTK_WIDGET(self));
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

GtkWidget *
gh_reaction_bar_new(void)
{
  return g_object_new(GH_TYPE_REACTION_BAR, NULL);
}

void
gh_reaction_bar_set_summary(GhReactionBar *self, GhReactionSummary *summary)
{
  g_return_if_fail(GH_IS_REACTION_BAR(self));
  if (self->summary == summary)
    return;

  if (self->summary && self->total_handler) {
    g_signal_handler_disconnect(self->summary, self->total_handler);
    self->total_handler = 0;
  }
  g_set_object(&self->summary, summary);
  if (summary) {
    self->total_handler = g_signal_connect(summary, "notify::total-count",
                                           G_CALLBACK(on_total_changed), self);
  }
  rebuild(self);
}

GhReactionSummary *
gh_reaction_bar_get_summary(GhReactionBar *self)
{
  g_return_val_if_fail(GH_IS_REACTION_BAR(self), NULL);
  return self->summary;
}
