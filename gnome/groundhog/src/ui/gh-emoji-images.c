#include "gh-emoji-images.h"
#include "gh-link-policy.h"
#include <nostr/nip30/nip30.h>
#include <glib/gi18n.h>

struct _GhEmojiImages {
  GtkBox parent_instance;
  GPtrArray *tags;
  gchar *text;
  GSettings *settings;
  GhWebContent *web;
  GCancellable *cancel;
  guint64 epoch;
  guint pending;
  GHashTable *textures;
  GtkWidget *content;
  GtkWidget *load;
  AdwAlertDialog *dialog;
};
G_DEFINE_FINAL_TYPE(GhEmojiImages, gh_emoji_images, GTK_TYPE_BOX)
static guint picked_signal, changed_signal;
static void render(GhEmojiImages *self);

gboolean
gh_emoji_images_has_artwork(GhEmojiImages *self)
{
  return g_hash_table_size(self->textures) > 0;
}

void
gh_emoji_images_revoke(GhEmojiImages *self)
{
  self->epoch++;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  g_clear_object(&self->cancel);
  if (self->dialog) adw_dialog_force_close(ADW_DIALOG(self->dialog));
  g_clear_object(&self->dialog);
  self->pending = 0;
  g_hash_table_remove_all(self->textures);
  render(self);
}

static void
picked(GtkButton *button, GhEmojiImages *self)
{
  gchar **tag = g_object_get_data(G_OBJECT(button), "emoji-tag");
  g_autofree gchar *token = g_strdup_printf(":%s:", tag[1]);
  g_autoptr(GPtrArray) tags = gh_emoji_tags_new();
  g_ptr_array_add(tags, g_strdupv(tag));
  g_signal_emit(self, picked_signal, 0, token, tags);
}

static void
render(GhEmojiImages *self)
{
  if (self->content) gtk_box_remove(GTK_BOX(self), self->content);
  self->content = NULL;
  if (self->text) {
    if (gh_emoji_images_has_artwork(self)) {
      GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
      GtkTextView *view = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(buffer));
      gtk_text_view_set_editable(view, FALSE);
      gtk_text_view_set_cursor_visible(view, FALSE);
      gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD_CHAR);
      gtk_widget_add_css_class(GTK_WIDGET(view), "emoji-text");
      NostrNip30Match matches[256];
      size_t n = 0, offset = 0;
      nostr_nip30_find_all(self->text, matches, G_N_ELEMENTS(matches), &n);
      GtkTextIter end;
      gtk_text_buffer_get_end_iter(buffer, &end);
      for (size_t i = 0; i < n; i++) {
        g_autofree gchar *name = g_strndup(matches[i].name, matches[i].name_len);
        GdkTexture *texture = g_hash_table_lookup(self->textures, name);
        if (!texture) continue;
        gtk_text_buffer_insert(buffer, &end, self->text + offset, matches[i].start - offset);
        GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &end);
        GtkWidget *image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
        gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
        g_autofree gchar *token = g_strdup_printf(":%s:", name);
        gtk_widget_set_tooltip_text(image, token);
        gtk_accessible_update_property(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_PROPERTY_LABEL, token, -1);
        gtk_text_view_add_child_at_anchor(view, image, anchor);
        offset = matches[i].end;
      }
      gtk_text_buffer_insert(buffer, &end, self->text + offset, -1);
      g_object_unref(buffer);
      self->content = GTK_WIDGET(view);
    }
  } else {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (guint i = 0; i < self->tags->len; i++) {
      gchar **tag = g_ptr_array_index(self->tags, i);
      g_autofree gchar *token = g_strdup_printf(":%s:", tag[1]);
      GtkWidget *button = gtk_button_new();
      GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GdkTexture *texture = g_hash_table_lookup(self->textures, tag[1]);
      if (texture) {
        GtkWidget *image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
        gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
        gtk_box_append(GTK_BOX(line), image);
      }
      GtkWidget *label = gtk_label_new(token);
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
      gtk_box_append(GTK_BOX(line), label);
      gtk_button_set_child(GTK_BUTTON(button), line);
      gtk_widget_add_css_class(button, "flat");
      gtk_widget_set_tooltip_text(button, token);
      g_object_set_data(G_OBJECT(button), "emoji-tag", tag);
      g_signal_connect(button, "clicked", G_CALLBACK(picked), self);
      gtk_box_append(GTK_BOX(box), button);
    }
    self->content = box;
  }
  if (self->content) gtk_box_prepend(GTK_BOX(self), self->content);
  gtk_widget_set_visible(self->load, self->tags->len > 0 && self->web != NULL);
  gtk_widget_set_sensitive(self->load, self->cancel == NULL);
  g_signal_emit(self, changed_signal, 0);
}

typedef struct { GWeakRef owner; guint64 epoch; gchar *name; } Load;
static void
loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Load *load = data;
  g_autoptr(GhEmojiImages) self = g_weak_ref_get(&load->owner);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhWebResult) image = gh_web_content_load_finish(GH_WEB_CONTENT(source), result, &error);
  if (self && self->epoch == load->epoch) {
    if (image && image->texture)
      g_hash_table_replace(self->textures, g_strdup(load->name), g_object_ref(image->texture));
    if (self->pending && --self->pending == 0) g_clear_object(&self->cancel);
    render(self);
  }
  g_weak_ref_clear(&load->owner);
  g_free(load->name);
  g_free(load);
}

static void
response(AdwAlertDialog *dialog, const gchar *response_id, GhEmojiImages *self)
{
  if (dialog != self->dialog) return;
  g_clear_object(&self->dialog);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_POPOVER);
  if (popover) gtk_popover_set_autohide(GTK_POPOVER(popover), TRUE);
  if (g_strcmp0(response_id, "load") || !self->web) return;
  self->cancel = g_cancellable_new();
  self->pending = self->tags->len;
  gtk_widget_set_sensitive(self->load, FALSE);
  for (guint i = 0; i < self->tags->len; i++) {
    gchar **tag = g_ptr_array_index(self->tags, i);
    Load *load = g_new0(Load, 1);
    g_weak_ref_init(&load->owner, self);
    load->epoch = self->epoch;
    load->name = g_strdup(tag[1]);
    gh_web_content_load_async(self->web, tag[2], GH_WEB_EMOJI, self->cancel, loaded, load);
  }
}

static void
ask(GtkButton *button, GhEmojiImages *self)
{
  (void)button;
  if (self->dialog || self->cancel || !self->web) return;
  GString *hosts = g_string_new(NULL);
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < self->tags->len; i++) {
    gchar **tag = g_ptr_array_index(self->tags, i);
    g_autofree gchar *host = gh_link_policy_dup_host(tag[2]);
    if (host && !g_hash_table_contains(seen, host)) {
      if (hosts->len) g_string_append(hosts, ", ");
      g_string_append(hosts, host);
      g_hash_table_add(seen, g_strdup(host));
    }
  }
  g_autofree gchar *mode = self->settings ? g_settings_get_string(self->settings, "network-mode") : NULL;
  g_autofree gchar *body = g_strdup_printf(g_strcmp0(mode, "tor") == 0
    ? _("Connect through Tor to %s to load emoji images? Permission lasts only while this view is open.")
    : _("Connect to %s to load emoji images? These websites will learn your IP address. Permission lasts only while this view is open."), hosts->str);
  g_string_free(hosts, TRUE);
  self->dialog = ADW_ALERT_DIALOG(g_object_ref_sink(adw_alert_dialog_new(_("Load Emoji Images?"), body)));
  adw_alert_dialog_add_responses(self->dialog, "cancel", _("_Cancel"), "load", _("_Load Images"), NULL);
  adw_alert_dialog_set_default_response(self->dialog, "cancel");
  adw_alert_dialog_set_close_response(self->dialog, "cancel");
  g_signal_connect_object(self->dialog, "response", G_CALLBACK(response), self, 0);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_POPOVER);
  if (popover) gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
  adw_dialog_present(ADW_DIALOG(self->dialog), GTK_WIDGET(self));
}

static void
setting_changed(GSettings *settings, const gchar *key, GhEmojiImages *self)
{
  if (!g_settings_get_boolean(settings, key)) gh_emoji_images_revoke(self);
}
static void
unmap(GtkWidget *widget)
{
  gh_emoji_images_revoke(GH_EMOJI_IMAGES(widget));
  GTK_WIDGET_CLASS(gh_emoji_images_parent_class)->unmap(widget);
}
static void
dispose(GObject *object)
{
  GhEmojiImages *self = GH_EMOJI_IMAGES(object);
  self->epoch++;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  g_clear_object(&self->cancel);
  if (self->dialog) adw_dialog_force_close(ADW_DIALOG(self->dialog));
  g_clear_object(&self->dialog);
  g_clear_object(&self->web);
  if (self->settings) g_signal_handlers_disconnect_by_data(self->settings, self);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_emoji_images_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
  GhEmojiImages *self = GH_EMOJI_IMAGES(object);
  g_ptr_array_unref(self->tags);
  g_hash_table_unref(self->textures);
  g_free(self->text);
  G_OBJECT_CLASS(gh_emoji_images_parent_class)->finalize(object);
}
static void
gh_emoji_images_class_init(GhEmojiImagesClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
  GTK_WIDGET_CLASS(klass)->unmap = unmap;
  picked_signal = g_signal_new("picked", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_PTR_ARRAY);
  changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void
gh_emoji_images_init(GhEmojiImages *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  self->textures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->tags = gh_emoji_tags_new();
  self->load = gtk_button_new_with_label(_("Load Emoji Images…"));
  gtk_widget_add_css_class(self->load, "flat");
  gtk_widget_set_halign(self->load, GTK_ALIGN_START);
  g_signal_connect(self->load, "clicked", G_CALLBACK(ask), self);
  gtk_box_append(GTK_BOX(self), self->load);
}
GtkWidget *
gh_emoji_images_new(GPtrArray *tags, const gchar *text, GSettings *settings, GhWebContent *web)
{
  GhEmojiImages *self = g_object_new(GH_TYPE_EMOJI_IMAGES, NULL);
  self->text = g_strdup(text);
  g_set_object(&self->settings, settings);
  g_set_object(&self->web, web);
  if (settings) g_signal_connect(settings, "changed::load-remote-images", G_CALLBACK(setting_changed), self);
  for (guint i = 0; tags && i < tags->len && self->tags->len < 256; i++) {
    gchar **tag = g_ptr_array_index(tags, i);
    if (gh_emoji_tag_valid((const gchar *const *)tag)) g_ptr_array_add(self->tags, g_strdupv(tag));
  }
  if (text) gtk_button_set_label(GTK_BUTTON(self->load), _("Load Custom Emoji"));
  render(self);
  return GTK_WIDGET(self);
}
