#include "gn-communikeys-communities-panel.h"
#include "../model/gn-communikeys-community-item.h"

struct _GnCommunikeysCommunitiesPanel {
  AdwBin parent_instance;
  GnCommunikeysCommunityService *service;
  GtkListBox *list;
  GtkWidget *empty;
};
G_DEFINE_TYPE(GnCommunikeysCommunitiesPanel,
              gn_communikeys_communities_panel, ADW_TYPE_BIN)

static void rebuild(GnCommunikeysCommunitiesPanel *self) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list))) != NULL)
    gtk_list_box_remove(self->list, child);
  GListModel *model = gn_communikeys_community_service_get_model(self->service);
  guint n = g_list_model_get_n_items(model);
  gtk_widget_set_visible(self->empty, n == 0);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysCommunityItem) item =
      g_list_model_get_item(model, i);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    const char *description =
      gn_communikeys_community_item_get_description(item);
    g_autofree gchar *title = description && *description
      ? g_strdup(description)
      : g_strdup_printf("%.16s…",
          gn_communikeys_community_item_get_pubkey(item));
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_widget_add_css_class(title_label, "heading");
    g_autofree gchar *detail = g_strdup_printf(
      "%u sections · %s",
      gn_communikeys_community_item_get_section_count(item),
      gn_communikeys_community_item_get_main_relay(item)
        ? gn_communikeys_community_item_get_main_relay(item)
        : "relay unavailable");
    GtkWidget *detail_label = gtk_label_new(detail);
    gtk_label_set_xalign(GTK_LABEL(detail_label), 0.0f);
    gtk_widget_add_css_class(detail_label, "dim-label");
    gtk_box_append(GTK_BOX(box), title_label);
    gtk_box_append(GTK_BOX(box), detail_label);
    gtk_list_box_append(self->list, box);
  }
}

static void on_items_changed(GListModel *model, guint position,
                             guint removed, guint added, gpointer user_data) {
  (void)model; (void)position; (void)removed; (void)added;
  rebuild(GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data));
}
static void on_refresh(GtkButton *button, gpointer user_data) {
  (void)button;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gn_communikeys_community_service_refresh(self->service);
}
static void gn_communikeys_communities_panel_dispose(GObject *object) {
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(object);
  g_clear_object(&self->service);
  G_OBJECT_CLASS(gn_communikeys_communities_panel_parent_class)->dispose(object);
}
static void gn_communikeys_communities_panel_class_init(
    GnCommunikeysCommunitiesPanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gn_communikeys_communities_panel_dispose;
}
static void gn_communikeys_communities_panel_init(
    GnCommunikeysCommunitiesPanel *self) { (void)self; }
GnCommunikeysCommunitiesPanel *gn_communikeys_communities_panel_new(
    GnCommunikeysCommunityService *service) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(service), NULL);
  GnCommunikeysCommunitiesPanel *self = g_object_new(
    GN_TYPE_COMMUNIKEYS_COMMUNITIES_PANEL, NULL);
  self->service = g_object_ref(service);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(header, 12); gtk_widget_set_margin_bottom(header, 8);
  gtk_widget_set_margin_start(header, 12); gtk_widget_set_margin_end(header, 12);
  GtkWidget *title = gtk_label_new("Communities");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(refresh, "Refresh community definitions");
  gtk_box_append(GTK_BOX(header), title); gtk_box_append(GTK_BOX(header), refresh);
  self->empty = gtk_label_new("No Communikeys definitions found");
  gtk_widget_add_css_class(self->empty, "dim-label");
  gtk_widget_set_margin_top(self->empty, 24);
  self->list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->list, GTK_SELECTION_NONE);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->list));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(root), header);
  gtk_box_append(GTK_BOX(root), self->empty);
  gtk_box_append(GTK_BOX(root), scroll);
  adw_bin_set_child(ADW_BIN(self), root);
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), self);
  GListModel *model = gn_communikeys_community_service_get_model(service);
  g_signal_connect_object(model, "items-changed", G_CALLBACK(on_items_changed),
                          self, 0);
  rebuild(self);
  return self;
}
