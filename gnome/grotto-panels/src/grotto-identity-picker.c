/* SPDX-License-Identifier: MIT */
#include <grotto-panels.h>
#include <string.h>

/* The model retains public keys; only their presentation is cosmetic. */
typedef struct {
  GrottoPanelsService *service;
  GtkWidget *box;
  GWeakRef item;
  AdwAvatar *avatar;
  GtkLabel *name, *key;
  char *public_key;
} PickerItem;

static void picker_item_free(gpointer data) {
  PickerItem *p = data;
  g_clear_object(&p->service);
  g_weak_ref_clear(&p->item);
  g_free(p->public_key);
  g_free(p);
}

static void picker_render(PickerItem *p) {
  if (!p->public_key) return;
  g_autofree char *short_key = strlen(p->public_key) > 20 ?
    g_strdup_printf("%.12s…%.6s", p->public_key, p->public_key + strlen(p->public_key) - 6) : g_strdup(p->public_key);
  g_autoptr(GrottoProfileInfo) profile = p->service ?
    grotto_panels_service_dup_profile(p->service, p->public_key) : NULL;
  g_autofree char *device_label = NULL;
  const char *name = NULL;
  GListModel *identities = p->service ? grotto_panels_service_get_model(p->service, GROTTO_PANELS_IDENTITIES) : NULL;
  for (guint i = 0; identities && i < g_list_model_get_n_items(identities); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(identities, i);
    const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    if (!g_strcmp0(m->public_key, p->public_key) && m->label && *m->label) {
      device_label = g_strdup(m->label);
      name = device_label;
      break;
    }
  }
  if (!name && profile) name = profile->fields[GROTTO_PROFILE_DISPLAY_NAME];
  if (!name || !*name) name = short_key;
  gtk_label_set_text(p->name, name);
  gtk_label_set_text(p->key, short_key);
  adw_avatar_set_text(p->avatar, name);
  adw_avatar_set_custom_image(p->avatar, profile && profile->images[0] ? GDK_PAINTABLE(profile->images[0]) : NULL);
  g_autoptr(GtkListItem) item = g_weak_ref_get(&p->item);
  if (item) {
    gtk_list_item_set_accessible_label(item, name);
    gtk_list_item_set_accessible_description(item, short_key);
  }
  gtk_widget_set_tooltip_text(p->box, name);
}

static void profile_updated(GrottoPanelsService *service, const char *key, gpointer data) {
  (void)service; PickerItem *p = g_object_get_data(G_OBJECT(data), "identity-picker-item");
  if (!g_strcmp0(key, p->public_key)) picker_render(p);
}
static void identities_changed(GListModel *model, guint position, guint removed, guint added, gpointer data) {
  (void)model; (void)position; (void)removed; (void)added;
  picker_render(g_object_get_data(G_OBJECT(data), "identity-picker-item"));
}
static void picker_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; PickerItem *p = g_new0(PickerItem, 1);
  p->service = data ? g_object_ref(data) : NULL;
  g_weak_ref_init(&p->item, item);
  p->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  p->avatar = ADW_AVATAR(adw_avatar_new(32, NULL, TRUE));
  gtk_accessible_update_state(GTK_ACCESSIBLE(p->avatar), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  p->name = GTK_LABEL(gtk_label_new(NULL)); p->key = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(p->key), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(p->key), "caption");
  GtkLabel *labels[] = {p->name, p->key};
  for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
    gtk_label_set_xalign(labels[i], 0);
    gtk_label_set_ellipsize(labels[i], PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(labels[i], 24);
    gtk_box_append(GTK_BOX(text), GTK_WIDGET(labels[i]));
  }
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->avatar));
  gtk_box_append(GTK_BOX(p->box), text);
  g_object_set_data_full(G_OBJECT(p->box), "identity-picker-item", p, picker_item_free);
  if (p->service) {
    g_signal_connect_object(p->service, "profile-updated", G_CALLBACK(profile_updated), p->box, 0);
    /* Callbacks use the widget, so no callback can outlive its row. */
    GListModel *ids = grotto_panels_service_get_model(p->service, GROTTO_PANELS_IDENTITIES);
    if (ids) g_signal_connect_object(ids, "items-changed", G_CALLBACK(identities_changed), p->box, 0);
  }
  gtk_list_item_set_child(item, p->box);
}
static void picker_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data;
  PickerItem *p = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "identity-picker-item");
  g_free(p->public_key);
  p->public_key = g_strdup(gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item))));
  picker_render(p);
}
static void picker_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  (void)factory; (void)data;
  PickerItem *p = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "identity-picker-item");
  g_clear_pointer(&p->public_key, g_free);
  gtk_list_item_set_accessible_label(item, NULL);
  gtk_list_item_set_accessible_description(item, NULL);
  adw_avatar_set_custom_image(p->avatar, NULL);
}
/* AdwComboRow does not expose its selection policy. Use public model accessors
 * on its current-item view, matching the original model rather than the popup's
 * filtered selection. Both supported libadwaita versions use this model chain. */
static GtkSingleSelection *identity_selection(GtkWidget *widget, GListModel *identities) {
  if (GTK_IS_LIST_VIEW(widget)) {
    GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(widget)));
    if (GTK_IS_NO_SELECTION(model)) model = gtk_no_selection_get_model(GTK_NO_SELECTION(model));
    if (GTK_IS_SELECTION_FILTER_MODEL(model))
      model = G_LIST_MODEL(gtk_selection_filter_model_get_model(GTK_SELECTION_FILTER_MODEL(model)));
    if (GTK_IS_SINGLE_SELECTION(model) &&
        gtk_single_selection_get_model(GTK_SINGLE_SELECTION(model)) == identities)
      return GTK_SINGLE_SELECTION(model);
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkSingleSelection *selection = identity_selection(child, identities);
    if (selection) return selection;
  }
  return NULL;
}
static void selection_policy_changed(AdwComboRow *row) {
  GListModel *model = adw_combo_row_get_model(row);
  if (!model) return;
  GtkSingleSelection *selection = identity_selection(GTK_WIDGET(row), model);
  if (!selection) {
    /* Never leave an implicit identity selected on an unsupported widget tree. */
    adw_combo_row_set_model(row, NULL);
    g_warning("Identity picker selection policy unavailable; identity model removed");
    return;
  }
  /* GTK >= 4.16 enforces these flags for set_selected(INVALID_POSITION).
   * Callers still choose their initial selection explicitly after set_model(). */
  gtk_single_selection_set_autoselect(selection, FALSE);
  gtk_single_selection_set_can_unselect(selection, TRUE);
}
GtkWidget *grotto_identity_picker_new(GrottoPanelsService *service) {
  g_return_val_if_fail(!service || GROTTO_PANELS_IS_SERVICE(service), NULL);
  AdwComboRow *row = ADW_COMBO_ROW(adw_combo_row_new());
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_object_set_data_full(G_OBJECT(factory), "identity-service", service ? g_object_ref(service) : NULL, service ? g_object_unref : NULL);
  g_signal_connect(factory, "setup", G_CALLBACK(picker_setup), service);
  g_signal_connect(factory, "bind", G_CALLBACK(picker_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(picker_unbind), NULL);
  adw_combo_row_set_factory(row, factory);
  adw_combo_row_set_list_factory(row, factory);
  g_object_unref(factory);
  g_signal_connect(row, "notify::model", G_CALLBACK(selection_policy_changed), NULL);
  return GTK_WIDGET(row);
}
