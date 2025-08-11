#include <gtk/gtk.h>
#include "../policy_store.h"

/* Forward declaration for internal refresh API used by callbacks */
void gnostr_permissions_page_refresh(GtkWidget *page, PolicyStore *ps);

typedef struct {
  PolicyStore *ps;
  GtkWidget *page; /* container widget for refresh */
  GtkWidget *list;
} PermsPage;

static void clear_list(GtkWidget *list) {
  GtkWidget *child = gtk_widget_get_first_child(list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(list), child);
    child = next;
  }
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
  PermsPage *pp = (PermsPage*)user_data;
  const gchar *identity = g_object_get_data(G_OBJECT(btn), "identity");
  const gchar *app_id = g_object_get_data(G_OBJECT(btn), "app_id");
  if (pp && pp->ps && identity && app_id) {
    policy_store_unset(pp->ps, app_id, identity);
    policy_store_save(pp->ps);
    // Refresh
    if (pp->page)
      gnostr_permissions_page_refresh(pp->page, pp->ps);
  }
}

static void on_switch_active_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PermsPage *pp = (PermsPage*)user_data;
  GtkSwitch *self = GTK_SWITCH(object);
  const gchar *identity = g_object_get_data(G_OBJECT(self), "identity");
  const gchar *app_id = g_object_get_data(G_OBJECT(self), "app_id");
  if (!pp || !pp->ps || !identity || !app_id) return;
  gboolean allow = gtk_switch_get_active(self);
  policy_store_set(pp->ps, app_id, identity, allow);
  policy_store_save(pp->ps);
  if (pp->page) gnostr_permissions_page_refresh(pp->page, pp->ps);
}

void gnostr_permissions_page_refresh(GtkWidget *page, PolicyStore *ps) {
  PermsPage *pp = g_object_get_data(G_OBJECT(page), "perms_page");
  if (!pp) return;
  pp->ps = ps;
  clear_list(pp->list);
  GPtrArray *items = policy_store_list(ps);
  if (items) {
    for (guint i = 0; i < items->len; i++) {
      PolicyEntry *e = g_ptr_array_index(items, i);
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      /* Toggle switch for decision */
      GtkWidget *sw = gtk_switch_new();
      gtk_switch_set_active(GTK_SWITCH(sw), e->decision);
      g_object_set_data_full(G_OBJECT(sw), "identity", g_strdup(e->identity), g_free);
      g_object_set_data_full(G_OBJECT(sw), "app_id", g_strdup(e->app_id), g_free);
      /* notify::active handler to persist change */
      g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch_active_notify), pp);
      gtk_widget_set_margin_end(sw, 8);

      gchar *label_text = g_strdup_printf("%s — %s", e->identity, e->app_id);
      GtkWidget *lbl = gtk_label_new(label_text);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      GtkWidget *btn = gtk_button_new_with_label("Remove");
      g_object_set_data_full(G_OBJECT(btn), "identity", g_strdup(e->identity), g_free);
      g_object_set_data_full(G_OBJECT(btn), "app_id", g_strdup(e->app_id), g_free);
      g_signal_connect(btn, "clicked", G_CALLBACK(on_remove_clicked), pp);
      gtk_box_append(GTK_BOX(row), sw);
      gtk_box_append(GTK_BOX(row), lbl);
      gtk_box_append(GTK_BOX(row), btn);
      gtk_list_box_append(GTK_LIST_BOX(pp->list), row);
      g_free(label_text);
      /* Free entry */
      g_free(e->identity); g_free(e->app_id); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
}

typedef struct { PermsPage *pp; } ResetCtx;

static void on_reset_confirm_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  ResetCtx *rc = user_data;
  GError *err = NULL;
  int response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &err);
  if (err) {
    g_warning("Reset confirm dialog failed: %s", err->message);
    g_clear_error(&err);
    response = -1;
  }
  if (response == 0 && rc && rc->pp && rc->pp->ps) { /* 0 => first button */
    GPtrArray *items = policy_store_list(rc->pp->ps);
    if (items) {
      for (guint i = 0; i < items->len; i++) {
        PolicyEntry *e = g_ptr_array_index(items, i);
        policy_store_unset(rc->pp->ps, e->app_id, e->identity);
        g_free(e->identity); g_free(e->app_id); g_free(e);
      }
      g_ptr_array_free(items, TRUE);
    }
    policy_store_save(rc->pp->ps);
    if (rc->pp->page)
      gnostr_permissions_page_refresh(rc->pp->page, rc->pp->ps);
  }
  g_free(rc);
}

static void on_reset_clicked(GtkButton *btn, gpointer user_data) {
  PermsPage *pp = (PermsPage*)user_data;
  if (!pp || !pp->ps) return;
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Reset all saved permissions?");
  const char *buttons[] = { "Reset", "Cancel", NULL };
  gtk_alert_dialog_set_buttons(dlg, buttons);
  ResetCtx *rc = g_new0(ResetCtx, 1);
  rc->pp = pp;
  gtk_alert_dialog_choose(dlg, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))), NULL, on_reset_confirm_done, rc);
}

GtkWidget *gnostr_permissions_page_new(PolicyStore *ps) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  PermsPage *pp = g_new0(PermsPage, 1);
  g_object_set_data_full(G_OBJECT(box), "perms_page", pp, g_free);
  pp->page = box;

  GtkWidget *title = gtk_label_new("Permissions");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *reset = gtk_button_new_with_label("Reset permissions");
  g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_clicked), pp);
  gtk_box_append(GTK_BOX(box), reset);

  GtkWidget *list = gtk_list_box_new();
  pp->list = list;
  gtk_box_append(GTK_BOX(box), list);

  pp->ps = ps;
  gnostr_permissions_page_refresh(box, ps);

  return box;
}
