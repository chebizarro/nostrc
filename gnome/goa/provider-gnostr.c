#include "provider-gnostr.h"
#include <gtk/gtk.h>
#include <libsecret/secret.h>
#include "../seahorse/secret_store.h"
#include <gio/gio.h>

struct _ProviderGnostr { GoaProvider parent_instance; };
G_DEFINE_TYPE(ProviderGnostr, provider_gnostr, GOA_TYPE_PROVIDER)

/* Dialog state for async handling */
typedef struct {
  GoaProvider *provider;
  GDBusMethodInvocation *invocation;
  GtkDialog *dialog;
  GtkListBox *list_keys;
  GtkCheckButton *rb_existing;
  GtkCheckButton *rb_generate;
  GtkCheckButton *rb_import;
  GtkWidget *frame_keys;
  GtkLabel *lbl_no_keys;
  GHashTable *keys;  /* npub|uid -> attrs hash table */
  gchar *selected_npub;
} AddAccountDialogData;

static const gchar *provider_get_provider_type(GoaProvider *provider) {
  (void)provider; return "Gnostr";
}

static gchar *shorten_npub(const gchar *npub) {
  if (!npub) return g_strdup("(unknown)");
  gsize len = strlen(npub);
  if (len <= 16) return g_strdup(npub);
  return g_strdup_printf("%.8s…%.4s", npub, npub + len - 4);
}

static gchar *hash_npub_path(const gchar *npub) {
  if (!npub) npub = "";
  guint32 h = g_str_hash(npub);
  return g_strdup_printf("/org/gnostr/goa/%08x/", h);
}

static void dialog_data_free(AddAccountDialogData *data) {
  if (!data) return;
  if (data->keys) g_hash_table_unref(data->keys);
  g_free(data->selected_npub);
  g_free(data);
}

/* Create a row for a key in the list */
static GtkWidget *create_key_row(const gchar *npub, const gchar *origin, gboolean is_first) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  /* Key icon based on origin */
  const gchar *icon_name = "dialog-password-symbolic";
  if (origin && g_str_has_prefix(origin, "hardware"))
    icon_name = "security-high-symbolic";
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(box), icon);

  /* npub display */
  gchar *display = shorten_npub(npub);
  GtkWidget *label = gtk_label_new(display);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);
  g_free(display);

  /* Origin indicator */
  if (origin && *origin) {
    GtkWidget *origin_lbl = gtk_label_new(origin);
    gtk_widget_add_css_class(origin_lbl, "dim-label");
    gtk_box_append(GTK_BOX(box), origin_lbl);
  }

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "npub", g_strdup(npub), g_free);

  return row;
}

/* Populate the key list from Secret Service */
static void populate_key_list(AddAccountDialogData *data) {
  GError *err = NULL;
  GHashTable *all = gnostr_secret_store_find_all(&err);
  if (err) {
    g_warning("Failed to load keys: %s", err->message);
    g_clear_error(&err);
  }

  data->keys = all;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->list_keys))) != NULL) {
    gtk_list_box_remove(data->list_keys, child);
  }

  if (!all || g_hash_table_size(all) == 0) {
    gtk_widget_set_visible(GTK_WIDGET(data->lbl_no_keys), TRUE);
    gtk_widget_set_visible(data->frame_keys, FALSE);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(data->lbl_no_keys), FALSE);
  gtk_widget_set_visible(data->frame_keys, TRUE);

  GHashTableIter it;
  gpointer k, v;
  gboolean first = TRUE;
  g_hash_table_iter_init(&it, all);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    GHashTable *attrs = v;
    const gchar *npub = g_hash_table_lookup(attrs, "npub");
    const gchar *origin = g_hash_table_lookup(attrs, "origin");
    if (!npub || !*npub) continue;

    GtkWidget *row = create_key_row(npub, origin, first);
    gtk_list_box_append(data->list_keys, row);

    /* Select first row by default */
    if (first) {
      gtk_list_box_select_row(data->list_keys, GTK_LIST_BOX_ROW(row));
      first = FALSE;
    }
  }
}

/* Update key list visibility based on radio selection */
static void on_radio_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  AddAccountDialogData *data = user_data;
  gboolean show_list = gtk_check_button_get_active(data->rb_existing);
  gtk_widget_set_visible(data->frame_keys, show_list && data->keys && g_hash_table_size(data->keys) > 0);
  gtk_widget_set_visible(GTK_WIDGET(data->lbl_no_keys), show_list && (!data->keys || g_hash_table_size(data->keys) == 0));
}

/* Handle OK button / dialog response */
static void on_dialog_response(GtkDialog *dialog, int response_id, gpointer user_data) {
  AddAccountDialogData *data = user_data;

  if (response_id != GTK_RESPONSE_OK) {
    /* Cancelled */
    goa_provider_respond_add_account(data->provider, data->invocation, FALSE, NULL);
    gtk_window_destroy(GTK_WINDOW(dialog));
    dialog_data_free(data);
    return;
  }

  const gchar *chosen_npub = NULL;

  if (gtk_check_button_get_active(data->rb_existing)) {
    /* Use existing key - get selected row */
    GtkListBoxRow *row = gtk_list_box_get_selected_row(data->list_keys);
    if (row) {
      chosen_npub = g_object_get_data(G_OBJECT(row), "npub");
    }
    if (!chosen_npub) {
      /* No key selected - show error and keep dialog open */
      GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(dialog),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "Please select a key from the list.");
      g_signal_connect(msg, "response", G_CALLBACK(gtk_window_destroy), NULL);
      gtk_window_present(GTK_WINDOW(msg));
      return;
    }
  } else if (gtk_check_button_get_active(data->rb_generate)) {
    /* Generate new - inform user to use Gnostr Signer */
    GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(dialog),
                                             GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_INFO,
                                             GTK_BUTTONS_OK,
                                             "To generate a new key, please use Gnostr Signer.\n\n"
                                             "After creating a key there, return here and select it.");
    g_signal_connect(msg, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(msg));
    return;
  } else if (gtk_check_button_get_active(data->rb_import)) {
    /* Import/hardware - inform user to use Gnostr Signer */
    GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(dialog),
                                             GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_INFO,
                                             GTK_BUTTONS_OK,
                                             "To import a key or bind hardware, please use Gnostr Signer.\n\n"
                                             "After setting up your key there, return here and select it.");
    g_signal_connect(msg, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(msg));
    return;
  }

  /* Store per-account settings using relocatable schema */
  gchar *path = hash_npub_path(chosen_npub);
  GSettings *gs = g_settings_new_with_path("org.gnostr.goa", path);
  g_settings_set_string(gs, "relays-json", "");
  g_settings_set_string(gs, "grants-json", "");
  g_settings_set_string(gs, "profile-json", "");
  g_object_unref(gs);
  g_free(path);

  /* Respond success */
  goa_provider_respond_add_account(data->provider, data->invocation, TRUE, NULL);

  gtk_window_destroy(GTK_WINDOW(dialog));
  dialog_data_free(data);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AddAccountDialogData *data = user_data;
  gtk_dialog_response(data->dialog, GTK_RESPONSE_CANCEL);
}

static void on_ok_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AddAccountDialogData *data = user_data;
  gtk_dialog_response(data->dialog, GTK_RESPONSE_OK);
}

static gboolean provider_add_account(GoaProvider *provider,
                                     GDBusMethodInvocation *invocation,
                                     GVariant *params,
                                     GCancellable *cancellable) {
  (void)params; (void)cancellable;

  /* Build dialog from UI file */
  GtkBuilder *builder = gtk_builder_new_from_file(
    g_build_filename(DATADIR, "goa-1.0", "goa-add-account.ui", NULL));
  if (!builder) {
    /* Fallback: try local path for development */
    builder = gtk_builder_new_from_file("gnome/goa/goa-add-account.ui");
  }

  if (!builder) {
    /* If UI file not available, fall back to original simple behavior */
    GError *err = NULL;
    GHashTable *all = gnostr_secret_store_find_all(&err);
    const gchar *chosen_npub = NULL;
    if (all) {
      GHashTableIter it; gpointer k, v;
      g_hash_table_iter_init(&it, all);
      if (g_hash_table_iter_next(&it, &k, &v)) {
        GHashTable *attrs = v;
        chosen_npub = g_hash_table_lookup(attrs, "npub");
      }
    }
    if (!chosen_npub) {
      goa_provider_respond_add_account(provider, invocation, TRUE, NULL);
      if (all) g_hash_table_unref(all);
      return TRUE;
    }
    gchar *path = hash_npub_path(chosen_npub);
    GSettings *gs = g_settings_new_with_path("org.gnostr.goa", path);
    g_settings_set_string(gs, "relays-json", "");
    g_settings_set_string(gs, "grants-json", "");
    g_settings_set_string(gs, "profile-json", "");
    g_object_unref(gs);
    g_free(path);
    goa_provider_respond_add_account(provider, invocation, TRUE, NULL);
    if (all) g_hash_table_unref(all);
    return TRUE;
  }

  /* Create dialog state */
  AddAccountDialogData *data = g_new0(AddAccountDialogData, 1);
  data->provider = provider;
  data->invocation = invocation;

  /* Get widgets from builder (they won't be template-bound since we're not using a custom class) */
  /* We need to build the dialog manually since we don't have a registered GType for GnostrAddAccountDialog */

  /* Build dialog manually instead */
  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Add Gnostr Account");
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 360);
  data->dialog = GTK_DIALOG(dialog);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(content), box);

  /* Header */
  GtkWidget *header = gtk_label_new("Choose a key source");
  gtk_label_set_xalign(GTK_LABEL(header), 0);
  gtk_widget_add_css_class(header, "heading");
  gtk_box_append(GTK_BOX(box), header);

  /* Radio buttons */
  GtkWidget *radio_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(box), radio_box);

  data->rb_existing = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Use an existing key"));
  gtk_check_button_set_active(data->rb_existing, TRUE);
  gtk_box_append(GTK_BOX(radio_box), GTK_WIDGET(data->rb_existing));

  data->rb_generate = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Generate new key"));
  gtk_check_button_set_group(data->rb_generate, data->rb_existing);
  gtk_box_append(GTK_BOX(radio_box), GTK_WIDGET(data->rb_generate));

  data->rb_import = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Import or bind hardware"));
  gtk_check_button_set_group(data->rb_import, data->rb_existing);
  gtk_box_append(GTK_BOX(radio_box), GTK_WIDGET(data->rb_import));

  /* Key list frame */
  data->frame_keys = gtk_frame_new(NULL);
  gtk_widget_set_vexpand(data->frame_keys, TRUE);
  gtk_box_append(GTK_BOX(box), data->frame_keys);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 120);
  gtk_frame_set_child(GTK_FRAME(data->frame_keys), scroll);

  data->list_keys = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(data->list_keys, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(data->list_keys), "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(data->list_keys));

  /* No keys label */
  data->lbl_no_keys = GTK_LABEL(gtk_label_new("No keys found. Use Gnostr Signer to create or import keys."));
  gtk_label_set_wrap(data->lbl_no_keys, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(data->lbl_no_keys), "dim-label");
  gtk_widget_set_visible(GTK_WIDGET(data->lbl_no_keys), FALSE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(data->lbl_no_keys));

  /* Button box */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), btn_box);

  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  gtk_box_append(GTK_BOX(btn_box), btn_cancel);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), data);

  GtkWidget *btn_ok = gtk_button_new_with_label("Add Account");
  gtk_widget_add_css_class(btn_ok, "suggested-action");
  gtk_box_append(GTK_BOX(btn_box), btn_ok);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_ok_clicked), data);

  /* Connect radio button signals */
  g_signal_connect(data->rb_existing, "toggled", G_CALLBACK(on_radio_toggled), data);
  g_signal_connect(data->rb_generate, "toggled", G_CALLBACK(on_radio_toggled), data);
  g_signal_connect(data->rb_import, "toggled", G_CALLBACK(on_radio_toggled), data);

  /* Connect dialog response */
  g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), data);

  /* Populate key list */
  populate_key_list(data);

  /* Present dialog */
  gtk_window_present(GTK_WINDOW(dialog));

  g_object_unref(builder);

  return TRUE;
}

static void provider_gnostr_class_init(ProviderGnostrClass *klass) {
  GoaProviderClass *pc = GOA_PROVIDER_CLASS(klass);
  pc->get_provider_type = provider_get_provider_type;
  pc->add_account = provider_add_account;
}

static void provider_gnostr_init(ProviderGnostr *self) { (void)self; }
