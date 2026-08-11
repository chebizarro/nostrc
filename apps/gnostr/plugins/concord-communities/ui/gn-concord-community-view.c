#include "gn-concord-community-view.h"
#include "gn-concord-composer.h"
#include "../model/gn-concord-channel-item.h"
#include "../model/gn-concord-message-item.h"

#include <string.h>

struct _GnConcordCommunityView {
  GtkBox parent_instance;
  GnConcordCommunityService *service;
  GnConcordCommunityItem *item;
  gchar *community_id;
  gchar *channel_id;
  GtkListView *channel_list;
  GtkWidget *message_holder;
  GtkLabel *channel_title;
  GtkLabel *status;
  GnConcordComposer *composer;
  GCancellable *publish_cancellable;
  gulong service_updated;
};

G_DEFINE_TYPE(GnConcordCommunityView, gn_concord_community_view, GTK_TYPE_BOX)

static GtkWidget *left_label(const char *text, const char *css_class) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  if (css_class) gtk_widget_add_css_class(label, css_class);
  return label;
}

static gchar *short_hex(const char *hex) {
  if (!hex) return g_strdup("unknown");
  return strlen(hex) > 16 ? g_strdup_printf("%.16s…", hex) : g_strdup(hex);
}

static void set_status(GnConcordCommunityView *self, const char *message,
                       gboolean error) {
  gtk_label_set_text(self->status, message ? message : "");
  if (error) gtk_widget_add_css_class(GTK_WIDGET(self->status), "error");
  else gtk_widget_remove_css_class(GTK_WIDGET(self->status), "error");
}

/* --- the channel sidebar --- */

static void on_channel_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), left_label(NULL, NULL));
  gtk_box_append(GTK_BOX(box), left_label(NULL, "dim-label"));
  gtk_list_item_set_child(list_item, box);
}

static void on_channel_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  GnConcordChannelItem *channel = gtk_list_item_get_item(list_item);
  GtkWidget *box = gtk_list_item_get_child(list_item);
  GtkWidget *name = gtk_widget_get_first_child(box);
  GtkWidget *meta = gtk_widget_get_next_sibling(name);

  const char *label = gn_concord_channel_item_get_name(channel);
  g_autofree gchar *fallback =
    short_hex(gn_concord_channel_item_get_id(channel));
  g_autofree gchar *title =
    g_strdup_printf("# %s", label && *label ? label : fallback);
  gtk_label_set_text(GTK_LABEL(name), title);

  /* A private Channel holds its own key with its own epoch; a public one
   * derives from the community_root and rotates with the base (CORD-03). */
  g_autofree gchar *detail = g_strdup_printf(
    "%s · epoch %" G_GUINT64_FORMAT,
    gn_concord_channel_item_get_is_private(channel) ? "private" : "public",
    gn_concord_channel_item_get_epoch(channel));
  gtk_label_set_text(GTK_LABEL(meta), detail);
}

/* --- the message list --- */

static void on_message_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_box_append(GTK_BOX(box), left_label(NULL, "caption-heading"));
  gtk_box_append(GTK_BOX(box), left_label(NULL, NULL));
  gtk_box_append(GTK_BOX(box), left_label(NULL, "dim-label"));
  gtk_list_item_set_child(list_item, box);
}

static void on_message_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  GnConcordMessageItem *item = gtk_list_item_get_item(list_item);
  GtkWidget *box = gtk_list_item_get_child(list_item);
  GtkWidget *author = gtk_widget_get_first_child(box);
  GtkWidget *content = gtk_widget_get_next_sibling(author);
  GtkWidget *meta = gtk_widget_get_next_sibling(content);

  g_autofree gchar *who =
    short_hex(gn_concord_message_item_get_author(item));
  gtk_label_set_text(GTK_LABEL(author), who);
  gtk_label_set_text(GTK_LABEL(content),
                     gn_concord_message_item_get_content(item));

  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(
    gn_concord_message_item_get_created_at(item));
  g_autofree gchar *stamp =
    when ? g_date_time_format(when, "%Y-%m-%d %H:%M") : NULL;
  /* Show the ms remainder: it is the ordering basis, not decoration. */
  g_autofree gchar *details = g_strdup_printf(
    "%s.%03d", stamp ? stamp : "unknown time",
    gn_concord_message_item_get_ms(item));
  gtk_label_set_text(GTK_LABEL(meta), details);
}

static void show_channel(GnConcordCommunityView *self,
                         GnConcordChannelItem *channel) {
  g_free(self->channel_id);
  self->channel_id = g_strdup(gn_concord_channel_item_get_id(channel));

  const char *name = gn_concord_channel_item_get_name(channel);
  g_autofree gchar *fallback = short_hex(self->channel_id);
  g_autofree gchar *title =
    g_strdup_printf("# %s", name && *name ? name : fallback);
  gtk_label_set_text(self->channel_title, title);

  GtkWidget *old = gtk_widget_get_first_child(self->message_holder);
  if (old) gtk_box_remove(GTK_BOX(self->message_holder), old);

  GListModel *messages = gn_concord_community_service_get_messages(
    self->service, self->community_id, self->channel_id);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_message_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_message_bind), NULL);
  GtkNoSelection *selection =
    gtk_no_selection_new(messages ? g_object_ref(messages) : NULL);
  GtkWidget *list =
    gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(self->message_holder), scroll);

  gn_concord_composer_set_send_sensitive(
    self->composer,
    gn_concord_community_service_get_current_pubkey(self->service) != NULL);
  set_status(self,
             gn_concord_community_service_get_current_pubkey(self->service)
               ? "Messages are sealed to your key and wrapped under the "
                 "channel's stream key."
               : "Sign in to publish into this channel.",
             FALSE);

  gn_concord_community_service_refresh_channel(
    self->service, self->community_id, self->channel_id);
}

static void on_channel_activate(GtkListView *list, guint position,
                                gpointer user_data) {
  (void)list;
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(user_data);
  if (!self->item) return;
  GListModel *channels = gn_concord_community_item_get_channels(self->item);
  g_autoptr(GnConcordChannelItem) channel =
    g_list_model_get_item(channels, position);
  if (channel) show_channel(self, channel);
}

/* --- publishing --- */

static void on_publish_done(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
  (void)source;
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(user_data);
  g_autoptr(GError) error = NULL;
  gboolean ok = gn_concord_community_service_publish_message_finish(
    self->service, result, &error);
  g_clear_object(&self->publish_cancellable);
  gn_concord_composer_set_send_sensitive(
    self->composer,
    gn_concord_community_service_get_current_pubkey(self->service) != NULL);
  if (ok) {
    gn_concord_composer_clear(self->composer);
    set_status(self, "Message published to the channel's stream.", FALSE);
  } else {
    set_status(self, error ? error->message : "Failed to publish", TRUE);
  }
  g_object_unref(self);
}

static void on_send_requested(GnConcordComposer *composer, const char *content,
                              gpointer user_data) {
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(user_data);
  if (!self->channel_id) {
    set_status(self, "Pick a channel first.", TRUE);
    return;
  }
  if (self->publish_cancellable)
    g_cancellable_cancel(self->publish_cancellable);
  g_clear_object(&self->publish_cancellable);
  self->publish_cancellable = g_cancellable_new();
  gn_concord_composer_set_send_sensitive(composer, FALSE);
  set_status(self, "Requesting a signature for the seal…", FALSE);
  gn_concord_community_service_publish_message_async(
    self->service, self->community_id, self->channel_id, content,
    self->publish_cancellable, on_publish_done, g_object_ref(self));
}

static void on_service_updated(GnConcordCommunityService *service,
                               const char *community_id, guint flags,
                               gpointer user_data) {
  (void)flags;
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(user_data);
  if (g_strcmp0(community_id, self->community_id) != 0) return;
  g_clear_object(&self->item);
  self->item = gn_concord_community_service_lookup_community(
    service, self->community_id);
}

/* --- lifecycle --- */

static void gn_concord_community_view_dispose(GObject *object) {
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(object);
  if (self->publish_cancellable)
    g_cancellable_cancel(self->publish_cancellable);
  g_clear_object(&self->publish_cancellable);
  if (self->service && self->service_updated)
    g_signal_handler_disconnect(self->service, self->service_updated);
  self->service_updated = 0;
  g_clear_object(&self->service);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_concord_community_view_parent_class)->dispose(object);
}

static void gn_concord_community_view_finalize(GObject *object) {
  GnConcordCommunityView *self = GN_CONCORD_COMMUNITY_VIEW(object);
  g_free(self->community_id);
  g_free(self->channel_id);
  G_OBJECT_CLASS(gn_concord_community_view_parent_class)->finalize(object);
}

static void gn_concord_community_view_class_init(
    GnConcordCommunityViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_concord_community_view_dispose;
  object_class->finalize = gn_concord_community_view_finalize;
}

static void gn_concord_community_view_init(GnConcordCommunityView *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GnConcordCommunityView *gn_concord_community_view_new(
    GnConcordCommunityService *service, GnConcordCommunityItem *item) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(item), NULL);
  GnConcordCommunityView *self =
    g_object_new(GN_TYPE_CONCORD_COMMUNITY_VIEW, NULL);
  self->service = g_object_ref(service);
  self->item = g_object_ref(item);
  self->community_id =
    g_strdup(gn_concord_community_item_get_community_id(item));

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_margin_start(header, 12);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 10);
  gtk_widget_set_margin_bottom(header, 8);
  const char *name = gn_concord_community_item_get_name(item);
  g_autofree gchar *short_id = short_hex(self->community_id);
  gtk_box_append(GTK_BOX(header),
                 left_label(name && *name ? name : short_id, "title-2"));
  /* The owner is what the community_id commits to, so showing it is showing
   * the one fact the id self-certifies (CORD-02 §1). */
  g_autofree gchar *owner = short_hex(gn_concord_community_item_get_owner(item));
  g_autofree gchar *subtitle = g_strdup_printf(
    "owner %s · %s", owner,
    gn_concord_community_item_get_primary_relay(item)
      ? gn_concord_community_item_get_primary_relay(item)
      : "no relay in the bundle");
  gtk_box_append(GTK_BOX(header), left_label(subtitle, "dim-label"));
  gtk_box_append(GTK_BOX(self), header);

  GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand(columns, TRUE);

  GtkListItemFactory *channel_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(channel_factory, "setup", G_CALLBACK(on_channel_setup),
                   NULL);
  g_signal_connect(channel_factory, "bind", G_CALLBACK(on_channel_bind), NULL);
  GListModel *channels = gn_concord_community_item_get_channels(item);
  GtkSingleSelection *channel_selection =
    gtk_single_selection_new(g_object_ref(channels));
  gtk_single_selection_set_autoselect(channel_selection, FALSE);
  gtk_single_selection_set_can_unselect(channel_selection, TRUE);
  self->channel_list = GTK_LIST_VIEW(gtk_list_view_new(
    GTK_SELECTION_MODEL(channel_selection), channel_factory));
  gtk_list_view_set_single_click_activate(self->channel_list, TRUE);
  g_signal_connect(self->channel_list, "activate",
                   G_CALLBACK(on_channel_activate), self);
  GtkWidget *channel_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(channel_scroll),
                                GTK_WIDGET(self->channel_list));
  gtk_widget_set_size_request(channel_scroll, 220, -1);
  gtk_box_append(GTK_BOX(columns), channel_scroll);
  gtk_box_append(GTK_BOX(columns),
                 gtk_separator_new(GTK_ORIENTATION_VERTICAL));

  GtkWidget *chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(chat, TRUE);
  self->channel_title = GTK_LABEL(gtk_label_new("Select a channel"));
  gtk_label_set_xalign(self->channel_title, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->channel_title), "heading");
  gtk_widget_set_margin_start(GTK_WIDGET(self->channel_title), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self->channel_title), 8);
  gtk_box_append(GTK_BOX(chat), GTK_WIDGET(self->channel_title));

  self->message_holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(self->message_holder, TRUE);
  gtk_box_append(GTK_BOX(chat), self->message_holder);

  self->status = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->status, TRUE);
  gtk_label_set_xalign(self->status, 0);
  gtk_widget_set_margin_start(GTK_WIDGET(self->status), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self->status), 12);
  gtk_box_append(GTK_BOX(chat), GTK_WIDGET(self->status));

  self->composer = gn_concord_composer_new();
  gn_concord_composer_set_send_sensitive(self->composer, FALSE);
  g_signal_connect(self->composer, "send-requested",
                   G_CALLBACK(on_send_requested), self);
  gtk_box_append(GTK_BOX(chat), GTK_WIDGET(self->composer));
  gtk_box_append(GTK_BOX(columns), chat);
  gtk_box_append(GTK_BOX(self), columns);

  self->service_updated = g_signal_connect(
    service, "community-updated", G_CALLBACK(on_service_updated), self);

  /* Open the first Channel so the view is never an empty frame — Genesis
   * always mints one public Channel, `#general` (CORD-02 §1). */
  if (g_list_model_get_n_items(channels) > 0) {
    g_autoptr(GnConcordChannelItem) first = g_list_model_get_item(channels, 0);
    if (first) show_channel(self, first);
  } else {
    set_status(self, "This invite granted no channels.", FALSE);
  }
  return self;
}
