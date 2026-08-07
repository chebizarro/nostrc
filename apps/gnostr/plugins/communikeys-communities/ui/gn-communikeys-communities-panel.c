#include "gn-communikeys-communities-panel.h"
#include "gn-communikeys-community-row.h"
#include "gn-communikeys-community-view.h"
#include "../model/gn-communikeys-community-item.h"

struct _GnCommunikeysCommunitiesPanel {
  AdwBin parent_instance;
  GnCommunikeysCommunityService *service;
  GtkStack *navigation;
  GtkStack *list_stack;
  GtkWidget *detail_holder;
  GtkLabel *error_label;
  GtkWidget *error_bar;
  gulong items_changed;
  gulong error_reported;
};

G_DEFINE_TYPE(GnCommunikeysCommunitiesPanel,
              gn_communikeys_communities_panel, ADW_TYPE_BIN)

static void on_factory_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *list_item,
                             gpointer user_data) {
  (void)factory;
  (void)user_data;
  gtk_list_item_set_child(
    list_item, GTK_WIDGET(gn_communikeys_community_row_new()));
}
static void on_factory_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *list_item,
                            gpointer user_data) {
  (void)factory;
  (void)user_data;
  gn_communikeys_community_row_bind(
    GN_COMMUNIKEYS_COMMUNITY_ROW(gtk_list_item_get_child(list_item)),
    gtk_list_item_get_item(list_item));
}
static void on_factory_unbind(GtkSignalListItemFactory *factory,
                              GtkListItem *list_item,
                              gpointer user_data) {
  (void)factory;
  (void)user_data;
  gn_communikeys_community_row_unbind(
    GN_COMMUNIKEYS_COMMUNITY_ROW(gtk_list_item_get_child(list_item)));
}

static void on_items_changed(GListModel *model, guint position,
                             guint removed, guint added,
                             gpointer user_data) {
  (void)position;
  (void)removed;
  (void)added;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gtk_stack_set_visible_child_name(
    self->list_stack,
    g_list_model_get_n_items(model) ? "list" : "empty");
}

static void on_refresh(GtkButton *button, gpointer user_data) {
  (void)button;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gn_communikeys_community_service_refresh(self->service);
}

static void on_back(GtkButton *button, gpointer user_data) {
  (void)button;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gtk_stack_set_visible_child_name(self->navigation, "communities");
}

static void on_activate(GtkListView *list, guint position,
                        gpointer user_data) {
  (void)list;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  GListModel *model =
    gn_communikeys_community_service_get_model(self->service);
  g_autoptr(GnCommunikeysCommunityItem) item =
    g_list_model_get_item(model, position);
  if (!item) return;

  GtkWidget *old = gtk_widget_get_first_child(self->detail_holder);
  if (old) gtk_box_remove(GTK_BOX(self->detail_holder), old);
  GtkWidget *detail = GTK_WIDGET(
    gn_communikeys_community_view_new(self->service, item));
  gtk_widget_set_vexpand(detail, TRUE);
  gtk_box_append(GTK_BOX(self->detail_holder), detail);
  gtk_stack_set_visible_child_name(self->navigation, "detail");
  gn_communikeys_community_service_refresh_community(
    self->service, gn_communikeys_community_item_get_pubkey(item));
}

static void on_error(GnCommunikeysCommunityService *service,
                     const char *message, gpointer user_data) {
  (void)service;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gtk_label_set_text(self->error_label, message);
  gtk_widget_set_visible(self->error_bar, TRUE);
}
static void on_dismiss(GtkButton *button, gpointer user_data) {
  (void)button;
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(user_data);
  gtk_widget_set_visible(self->error_bar, FALSE);
}

static GtkWidget *make_error_bar(GnCommunikeysCommunitiesPanel *self) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(bar, 12);
  gtk_widget_set_margin_end(bar, 12);
  gtk_widget_set_margin_top(bar, 4);
  gtk_widget_set_visible(bar, FALSE);
  GtkWidget *icon =
    gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_add_css_class(icon, "error");
  gtk_box_append(GTK_BOX(bar), icon);
  self->error_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->error_label, TRUE);
  gtk_label_set_xalign(self->error_label, 0);
  gtk_widget_set_hexpand(GTK_WIDGET(self->error_label), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->error_label), "error");
  gtk_box_append(GTK_BOX(bar), GTK_WIDGET(self->error_label));
  GtkWidget *dismiss =
    gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(dismiss, "flat");
  g_signal_connect(dismiss, "clicked", G_CALLBACK(on_dismiss), self);
  gtk_box_append(GTK_BOX(bar), dismiss);
  return bar;
}

static GtkWidget *make_communities_page(
    GnCommunikeysCommunitiesPanel *self) {
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->error_bar = make_error_bar(self);
  gtk_box_append(GTK_BOX(root), self->error_bar);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(header, 10);
  gtk_widget_set_margin_bottom(header, 8);
  gtk_widget_set_margin_start(header, 12);
  gtk_widget_set_margin_end(header, 12);
  GtkWidget *title = gtk_label_new("Communikeys Communities");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  GtkWidget *refresh =
    gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(refresh, "flat");
  gtk_widget_set_tooltip_text(
    refresh, "Refresh definitions, section ACLs, and publications");
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), self);
  gtk_box_append(GTK_BOX(header), title);
  gtk_box_append(GTK_BOX(header), refresh);
  gtk_box_append(GTK_BOX(root), header);
  gtk_box_append(GTK_BOX(root),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  self->list_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->list_stack), TRUE);
  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
  GtkWidget *empty_icon =
    gtk_image_new_from_icon_name("system-users-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(empty), empty_icon);
  GtkWidget *empty_title = gtk_label_new("No Communities Found");
  gtk_widget_add_css_class(empty_title, "title-2");
  gtk_box_append(GTK_BOX(empty), empty_title);
  GtkWidget *empty_text = gtk_label_new(
    "Signed kind-10222 definitions will appear here.");
  gtk_widget_add_css_class(empty_text, "dim-label");
  gtk_box_append(GTK_BOX(empty), empty_text);
  gtk_stack_add_named(self->list_stack, empty, "empty");

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);
  GListModel *model =
    gn_communikeys_community_service_get_model(self->service);
  GtkSingleSelection *selection =
    gtk_single_selection_new(g_object_ref(model));
  gtk_single_selection_set_autoselect(selection, FALSE);
  gtk_single_selection_set_can_unselect(selection, TRUE);
  GtkWidget *list = gtk_list_view_new(
    GTK_SELECTION_MODEL(selection), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list), TRUE);
  g_signal_connect(list, "activate", G_CALLBACK(on_activate), self);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_stack_add_named(self->list_stack, scroll, "list");
  gtk_stack_set_visible_child_name(
    self->list_stack,
    g_list_model_get_n_items(model) ? "list" : "empty");
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(self->list_stack));
  return root;
}

static GtkWidget *make_detail_page(
    GnCommunikeysCommunitiesPanel *self) {
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(header, 6);
  gtk_widget_set_margin_bottom(header, 6);
  gtk_widget_set_margin_start(header, 8);
  gtk_widget_set_margin_end(header, 8);
  GtkWidget *back =
    gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(back, "flat");
  gtk_widget_set_tooltip_text(back, "Back to communities");
  g_signal_connect(back, "clicked", G_CALLBACK(on_back), self);
  gtk_box_append(GTK_BOX(header), back);
  GtkWidget *title = gtk_label_new("Community");
  gtk_widget_add_css_class(title, "heading");
  gtk_box_append(GTK_BOX(header), title);
  gtk_box_append(GTK_BOX(root), header);
  gtk_box_append(GTK_BOX(root),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  self->detail_holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(self->detail_holder, TRUE);
  gtk_box_append(GTK_BOX(root), self->detail_holder);
  return root;
}

static void gn_communikeys_communities_panel_dispose(GObject *object) {
  GnCommunikeysCommunitiesPanel *self =
    GN_COMMUNIKEYS_COMMUNITIES_PANEL(object);
  GListModel *model = self->service
    ? gn_communikeys_community_service_get_model(self->service) : NULL;
  if (model && self->items_changed)
    g_signal_handler_disconnect(model, self->items_changed);
  if (self->service && self->error_reported)
    g_signal_handler_disconnect(self->service, self->error_reported);
  self->items_changed = 0;
  self->error_reported = 0;
  g_clear_object(&self->service);
  G_OBJECT_CLASS(gn_communikeys_communities_panel_parent_class)->dispose(object);
}
static void gn_communikeys_communities_panel_class_init(
    GnCommunikeysCommunitiesPanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose =
    gn_communikeys_communities_panel_dispose;
}
static void gn_communikeys_communities_panel_init(
    GnCommunikeysCommunitiesPanel *self) {
  (void)self;
}

GnCommunikeysCommunitiesPanel *gn_communikeys_communities_panel_new(
    GnCommunikeysCommunityService *service) {
  g_return_val_if_fail(
    GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(service), NULL);
  GnCommunikeysCommunitiesPanel *self = g_object_new(
    GN_TYPE_COMMUNIKEYS_COMMUNITIES_PANEL, NULL);
  self->service = g_object_ref(service);
  self->navigation = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(
    self->navigation, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_add_named(self->navigation,
                      make_communities_page(self), "communities");
  gtk_stack_add_named(self->navigation,
                      make_detail_page(self), "detail");
  gtk_stack_set_visible_child_name(self->navigation, "communities");
  adw_bin_set_child(ADW_BIN(self), GTK_WIDGET(self->navigation));

  GListModel *model =
    gn_communikeys_community_service_get_model(service);
  self->items_changed = g_signal_connect(
    model, "items-changed", G_CALLBACK(on_items_changed), self);
  self->error_reported = g_signal_connect(
    service, "error-reported", G_CALLBACK(on_error), self);
  return self;
}
