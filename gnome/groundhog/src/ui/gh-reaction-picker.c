#include "gh-reaction-picker.h"
#include "gh-emoji-images.h"
#include <glib/gi18n.h>

struct _GhReactionPicker {
  GtkPopover parent_instance;
  GtkBox *quick_box;
  GtkBox *sections;
  GtkWidget *custom;
  GhSocialLists *lists;
  GSettings *settings;
  GhWebContent *web;
  GtkMenuButton *more_button;
  GtkEmojiChooser *emoji_chooser;
};

enum { SIGNAL_EMOJI_PICKED, SIGNAL_CUSTOM_PICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhReactionPicker, gh_reaction_picker, GTK_TYPE_POPOVER)

static void
quick_clicked(GtkButton *button, gpointer data)
{
  GhReactionPicker *self = GH_REACTION_PICKER(data);
  const gchar *emoji = gtk_widget_get_name(GTK_WIDGET(button));
  g_signal_emit(self, signals[SIGNAL_EMOJI_PICKED], 0, emoji);
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void
on_emoji_chosen(GtkEmojiChooser *chooser, const gchar *emoji, gpointer data)
{
  (void)chooser;
  GhReactionPicker *self = GH_REACTION_PICKER(data);
  g_signal_emit(self, signals[SIGNAL_EMOJI_PICKED], 0, emoji);
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void
custom_picked(GhEmojiImages *images, const gchar *emoji, GPtrArray *tags, GhReactionPicker *self)
{
  (void)images;
  g_signal_emit(self, signals[SIGNAL_CUSTOM_PICKED], 0, emoji, tags);
  gtk_popover_popdown(GTK_POPOVER(self));
}

static void
refresh_custom(GhReactionPicker *self)
{
  if (self->custom) gtk_box_remove(self->sections, self->custom);
  self->custom = NULL;
  g_autoptr(GPtrArray) sets = gh_emoji_catalog(self->lists);
  if (!sets->len) return;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *title = gtk_label_new(_("Custom"));
  gtk_widget_add_css_class(title, "heading");
  gtk_box_append(GTK_BOX(box), title);
  for (guint i = 0; i < sets->len; i++) {
    GhEmojiSet *set = g_ptr_array_index(sets, i);
    GtkWidget *label = gtk_label_new(set->title);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *images = gh_emoji_images_new(set->tags, NULL, self->settings, self->web);
    g_signal_connect(images, "picked", G_CALLBACK(custom_picked), self);
    gtk_box_append(GTK_BOX(box), images);
  }
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 280);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  self->custom = scroll;
  gtk_box_append(self->sections, scroll);
}

void
gh_reaction_picker_set_emoji_source(GhReactionPicker *self, GhSocialLists *lists,
                                    GSettings *settings, GhWebContent *web)
{
  if (self->lists) g_signal_handlers_disconnect_by_data(self->lists, self);
  g_set_object(&self->lists, lists);
  g_set_object(&self->settings, settings);
  g_set_object(&self->web, web);
  if (lists) g_signal_connect_swapped(lists, "changed", G_CALLBACK(refresh_custom), self);
  refresh_custom(self);
}

static void
dispose(GObject *object)
{
  GhReactionPicker *self = GH_REACTION_PICKER(object);
  if (self->lists) g_signal_handlers_disconnect_by_data(self->lists, self);
  g_clear_object(&self->lists);
  g_clear_object(&self->settings);
  g_clear_object(&self->web);
  G_OBJECT_CLASS(gh_reaction_picker_parent_class)->dispose(object);
}

/* ---- GObject ---------------------------------------------------------------- */

static void
gh_reaction_picker_class_init(GhReactionPickerClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  signals[SIGNAL_CUSTOM_PICKED] = g_signal_new("custom-emoji-picked", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_PTR_ARRAY);
  signals[SIGNAL_EMOJI_PICKED] =
    g_signal_new("emoji-picked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_reaction_picker_init(GhReactionPicker *self)
{
  self->quick_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
  gtk_widget_set_halign(GTK_WIDGET(self->quick_box), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(GTK_WIDGET(self->quick_box), 6);
  gtk_widget_set_margin_end(GTK_WIDGET(self->quick_box), 6);
  gtk_widget_set_margin_top(GTK_WIDGET(self->quick_box), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->quick_box), 6);

  for (guint i = 0; i < GH_REACTION_QUICK_SET_SIZE; i++) {
    GtkWidget *btn = gtk_button_new_with_label(gh_reaction_quick_set[i]);
    gtk_widget_set_name(btn, gh_reaction_quick_set[i]);
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "circular");
    gtk_widget_add_css_class(btn, "groundhog-reaction-quick");
    gtk_accessible_update_property(GTK_ACCESSIBLE(btn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   gh_reaction_quick_set[i], -1);
    g_signal_connect(btn, "clicked", G_CALLBACK(quick_clicked), self);
    gtk_box_append(self->quick_box, btn);
  }

  /* "…" button that shows the GTK emoji chooser. */
  self->emoji_chooser = GTK_EMOJI_CHOOSER(gtk_emoji_chooser_new());
  g_signal_connect(self->emoji_chooser, "emoji-picked", G_CALLBACK(on_emoji_chosen), self);

  self->more_button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(self->more_button, "face-smile-symbolic");
  gtk_menu_button_set_popover(self->more_button, GTK_WIDGET(self->emoji_chooser));
  gtk_widget_add_css_class(GTK_WIDGET(self->more_button), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(self->more_button), "circular");
  gtk_accessible_update_property(GTK_ACCESSIBLE(GTK_WIDGET(self->more_button)),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("More emoji"), -1);
  gtk_box_append(self->quick_box, GTK_WIDGET(self->more_button));

  self->sections = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
  gtk_box_append(self->sections, GTK_WIDGET(self->quick_box));
  gtk_popover_set_child(GTK_POPOVER(self), GTK_WIDGET(self->sections));
  gtk_popover_set_autohide(GTK_POPOVER(self), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "groundhog-reaction-picker");
}

GtkWidget *
gh_reaction_picker_new(void)
{
  return g_object_new(GH_TYPE_REACTION_PICKER, NULL);
}
