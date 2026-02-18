/* sheet-social-recovery.c - Social Recovery dialog implementation
 *
 * Provides comprehensive UI for:
 * - Setting up social recovery with guardian selection
 * - Configuring threshold (k-of-n)
 * - Distributing encrypted shares to guardians
 * - Recovering key from collected shares
 * - Managing existing recovery configuration
 */
#include "sheet-social-recovery.h"
#include "../app-resources.h"
#include "../../social-recovery.h"
#include "../../secret_store.h"
#include "../../accounts_store.h"
#include "../../secure-mem.h"
#include "../../backup-recovery.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

/* Maximum guardians supported in UI */
#define MAX_GUARDIANS 10

/* Clipboard clear timeout for shares */
#define SHARE_CLIPBOARD_TIMEOUT_SECONDS 120

struct _SheetSocialRecovery {
  AdwDialog parent_instance;

  /* View stack for modes */
  AdwViewStack *view_stack;
  GtkButton *btn_close;

  /* ===== SETUP MODE ===== */
  /* Account info */
  AdwActionRow *row_setup_account;

  /* Guardian list */
  GtkListBox *list_guardians;
  AdwEntryRow *entry_guardian_npub;
  AdwEntryRow *entry_guardian_name;
  GtkButton *btn_add_guardian;
  GtkLabel *lbl_guardian_count;

  /* Threshold configuration */
  AdwSpinRow *spin_threshold;
  GtkLabel *lbl_threshold_info;

  /* Setup action */
  GtkButton *btn_setup_recovery;
  AdwPreferencesGroup *group_setup_result;
  GtkLabel *lbl_setup_status;

  /* Share distribution */
  AdwPreferencesGroup *group_shares;
  GtkListBox *list_shares;

  /* ===== MANAGE MODE ===== */
  AdwActionRow *row_manage_account;
  AdwStatusPage *status_no_config;
  AdwPreferencesGroup *group_config_info;
  AdwActionRow *row_config_threshold;
  AdwActionRow *row_config_created;
  GtkListBox *list_manage_guardians;
  GtkButton *btn_delete_recovery;
  GtkButton *btn_test_recovery;

  /* ===== RECOVER MODE ===== */
  AdwEntryRow *entry_recover_npub;
  AdwSpinRow *spin_recover_threshold;
  GtkListBox *list_collected_shares;
  AdwEntryRow *entry_share_input;
  GtkButton *btn_add_share;
  GtkLabel *lbl_share_count;
  GtkButton *btn_recover;
  AdwPreferencesGroup *group_recovery_result;
  AdwActionRow *row_recovered_npub;
  GtkButton *btn_import_recovered;

  /* ===== State ===== */
  SheetSocialRecoveryMode mode;
  gchar *current_npub;
  gchar *cached_nsec;

  /* Setup state */
  GPtrArray *pending_guardians;  /* GnGuardian* */
  GnRecoveryConfig *setup_config;
  GPtrArray *encrypted_shares;   /* gchar* */

  /* Manage state */
  GnRecoveryConfig *loaded_config;

  /* Recovery state */
  GPtrArray *collected_shares;   /* GnSSSShare* */
  gchar *recovered_nsec;

  /* Callback */
  SheetSocialRecoveryCallback on_complete;
  gpointer on_complete_ud;
};

G_DEFINE_TYPE(SheetSocialRecovery, sheet_social_recovery, ADW_TYPE_DIALOG)

/* Forward declarations */
static void clear_sensitive_data(SheetSocialRecovery *self);
static void update_threshold_ui(SheetSocialRecovery *self);
static void update_guardian_list(SheetSocialRecovery *self);
static void update_share_count(SheetSocialRecovery *self);
static void load_existing_config(SheetSocialRecovery *self);

/* ============================================================
 * Helpers
 * ============================================================ */

static void secure_free_string(gchar **str) {
  if (str && *str) {
    gnostr_secure_clear(*str, strlen(*str));
    g_free(*str);
    *str = NULL;
  }
}

static void clear_sensitive_data(SheetSocialRecovery *self) {
  if (!self) return;

  secure_free_string(&self->cached_nsec);
  secure_free_string(&self->recovered_nsec);

  if (self->setup_config) {
    gn_recovery_config_free(self->setup_config);
    self->setup_config = NULL;
  }

  g_clear_pointer(&self->encrypted_shares, g_ptr_array_unref);
  g_clear_pointer(&self->collected_shares, g_ptr_array_unref);
}

static const gchar *get_nsec(SheetSocialRecovery *self) {
  if (!self) return NULL;
  if (self->cached_nsec) return self->cached_nsec;

  if (self->current_npub) {
    gchar *nsec = NULL;
    SecretStoreResult res = secret_store_get_secret(self->current_npub, &nsec);
    if (res == SECRET_STORE_OK && nsec) {
      self->cached_nsec = nsec;
      return self->cached_nsec;
    }
  }

  return NULL;
}

static void show_error(SheetSocialRecovery *self, const gchar *title, const gchar *message) {
  if (!self) return;
  GtkAlertDialog *ad = gtk_alert_dialog_new("%s", title);
  gtk_alert_dialog_set_detail(ad, message);
  gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
  g_object_unref(ad);
}

static void show_toast(SheetSocialRecovery *self, const gchar *message) {
  if (!self || !message) return;
  GtkAlertDialog *ad = gtk_alert_dialog_new("%s", message);
  gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
  g_object_unref(ad);
}

static void copy_to_clipboard(SheetSocialRecovery *self, const gchar *text) {
  if (!self || !text) return;
  GdkDisplay *dpy = gtk_widget_get_display(GTK_WIDGET(self));
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) {
      gdk_clipboard_set_text(cb, text);
    }
  }
}

/* ============================================================
 * Guardian Row Widget
 * ============================================================ */

typedef struct {
  gchar *npub;
  gchar *label;
  guint index;
} GuardianRowData;

static void guardian_row_data_free(GuardianRowData *data) {
  if (!data) return;
  g_free(data->npub);
  g_free(data->label);
  g_free(data);
}

static void on_guardian_remove(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  SheetSocialRecovery *self = g_object_get_data(G_OBJECT(row), "dialog-self");
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));

  GuardianRowData *data = g_object_get_data(G_OBJECT(row), "guardian-data");
  if (data && self->pending_guardians) {
    /* Find and remove from pending guardians */
    for (guint i = 0; i < self->pending_guardians->len; i++) {
      GnGuardian *g = g_ptr_array_index(self->pending_guardians, i);
      if (g_strcmp0(g->npub, data->npub) == 0) {
        g_ptr_array_remove_index(self->pending_guardians, i);
        break;
      }
    }
  }

  gtk_list_box_remove(list, GTK_WIDGET(row));
  update_threshold_ui(self);
  update_guardian_list(self);
}

static GtkWidget *create_guardian_row(SheetSocialRecovery *self,
                                      const gchar *npub,
                                      const gchar *label,
                                      guint index) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: label or truncated npub */
  const gchar *title = label;
  gchar *title_owned = NULL;
  if (!title || !*title) {
    title_owned = g_strdup_printf("Guardian %u (%.12s...)", index + 1, npub + 5);
    title = title_owned;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

  /* Subtitle: truncated npub */
  gchar *subtitle = g_strdup_printf("%.16s...", npub);
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);
  g_free(title_owned);

  /* Index icon */
  gchar *index_label = g_strdup_printf("%u", index + 1);
  GtkLabel *idx_widget = GTK_LABEL(gtk_label_new(index_label));
  gtk_widget_add_css_class(GTK_WIDGET(idx_widget), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(idx_widget), "caption");
  adw_action_row_add_prefix(row, GTK_WIDGET(idx_widget));
  g_free(index_label);

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("list-remove-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Store data */
  GuardianRowData *data = g_new0(GuardianRowData, 1);
  data->npub = g_strdup(npub);
  data->label = g_strdup(label);
  data->index = index;
  g_object_set_data_full(G_OBJECT(row), "guardian-data", data, (GDestroyNotify)guardian_row_data_free);
  g_object_set_data(G_OBJECT(row), "dialog-self", self);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_guardian_remove), row);

  return GTK_WIDGET(row);
}

/* ============================================================
 * Share Row Widget (for distribution)
 * ============================================================ */

typedef struct {
  gchar *guardian_npub;
  gchar *guardian_label;
  gchar *encrypted_share;
  guint index;
} ShareRowData;

static void share_row_data_free(ShareRowData *data) {
  if (!data) return;
  g_free(data->guardian_npub);
  g_free(data->guardian_label);
  if (data->encrypted_share) {
    gnostr_secure_clear(data->encrypted_share, strlen(data->encrypted_share));
    g_free(data->encrypted_share);
  }
  g_free(data);
}

static void on_copy_share(GtkButton *btn, gpointer user_data) {
  AdwActionRow *row = ADW_ACTION_ROW(user_data);
  SheetSocialRecovery *self = g_object_get_data(G_OBJECT(row), "dialog-self");
  ShareRowData *data = g_object_get_data(G_OBJECT(row), "share-data");

  if (data && data->encrypted_share) {
    /* Format message with instructions */
    gchar *message = gn_social_recovery_format_share_message(
      data->encrypted_share,
      data->guardian_label,
      self->current_npub
    );

    copy_to_clipboard(self, message);
    g_free(message);

    show_toast(self, "Share message copied to clipboard!");
  }
}

static GtkWidget *create_share_row(SheetSocialRecovery *self,
                                   const gchar *guardian_npub,
                                   const gchar *guardian_label,
                                   const gchar *encrypted_share,
                                   guint index) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title */
  const gchar *title = guardian_label;
  gchar *title_owned = NULL;
  if (!title || !*title) {
    title_owned = g_strdup_printf("Guardian %u", index + 1);
    title = title_owned;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

  /* Subtitle */
  gchar *subtitle = g_strdup_printf("%.16s...", guardian_npub);
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);
  g_free(title_owned);

  /* Copy button */
  GtkButton *btn_copy = GTK_BUTTON(gtk_button_new_from_icon_name("edit-copy-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_copy), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_copy), "flat");
  gtk_widget_set_tooltip_text(GTK_WIDGET(btn_copy), "Copy share message to clipboard");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_copy));

  /* Store data */
  ShareRowData *data = g_new0(ShareRowData, 1);
  data->guardian_npub = g_strdup(guardian_npub);
  data->guardian_label = g_strdup(guardian_label);
  data->encrypted_share = g_strdup(encrypted_share);
  data->index = index;
  g_object_set_data_full(G_OBJECT(row), "share-data", data, (GDestroyNotify)share_row_data_free);
  g_object_set_data(G_OBJECT(row), "dialog-self", self);

  g_signal_connect(btn_copy, "clicked", G_CALLBACK(on_copy_share), row);

  return GTK_WIDGET(row);
}

/* ============================================================
 * Collected Share Row (for recovery)
 * ============================================================ */

static void on_remove_collected_share(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  SheetSocialRecovery *self = g_object_get_data(G_OBJECT(row), "dialog-self");
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));

  guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "share-index"));

  if (self->collected_shares && index < self->collected_shares->len) {
    g_ptr_array_remove_index(self->collected_shares, index);
  }

  gtk_list_box_remove(list, GTK_WIDGET(row));
  update_share_count(self);
}

static GtkWidget *create_collected_share_row(SheetSocialRecovery *self,
                                             GnSSSShare *share,
                                             guint list_index) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  gchar *title = g_strdup_printf("Share #%u", share->index);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_free(title);

  gchar *subtitle = g_strdup_printf("%zu bytes", share->data_len);
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("list-remove-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  g_object_set_data(G_OBJECT(row), "dialog-self", self);
  g_object_set_data(G_OBJECT(row), "share-index", GUINT_TO_POINTER(list_index));

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_remove_collected_share), row);

  return GTK_WIDGET(row);
}

/* ============================================================
 * UI Update Functions
 * ============================================================ */

static void update_guardian_list(SheetSocialRecovery *self) {
  if (!self) return;

  guint count = self->pending_guardians ? self->pending_guardians->len : 0;
  gchar *text = g_strdup_printf("%u guardian%s", count, count == 1 ? "" : "s");
  if (self->lbl_guardian_count) {
    gtk_label_set_text(self->lbl_guardian_count, text);
  }
  g_free(text);

  /* Enable/disable setup button */
  if (self->btn_setup_recovery) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_setup_recovery), count >= 2);
  }
}

static void update_threshold_ui(SheetSocialRecovery *self) {
  if (!self || !self->spin_threshold) return;

  guint count = self->pending_guardians ? self->pending_guardians->len : 0;

  /* Set bounds on threshold spinner */
  GtkAdjustment *adj = adw_spin_row_get_adjustment(self->spin_threshold);
  gtk_adjustment_set_upper(adj, count > 0 ? count : 1);
  gtk_adjustment_set_lower(adj, count >= 2 ? 2 : 1);

  /* Ensure current value is valid */
  gdouble current = adw_spin_row_get_value(self->spin_threshold);
  if (current > count && count > 0) {
    adw_spin_row_set_value(self->spin_threshold, count);
  }
  if (current < 2 && count >= 2) {
    adw_spin_row_set_value(self->spin_threshold, 2);
  }

  /* Update info label */
  if (self->lbl_threshold_info && count >= 2) {
    guint8 threshold = (guint8)adw_spin_row_get_value(self->spin_threshold);
    gchar *info = g_strdup_printf(
      "%u of %u guardians required for recovery",
      threshold, count
    );
    gtk_label_set_text(self->lbl_threshold_info, info);
    g_free(info);
  }
}

static void update_share_count(SheetSocialRecovery *self) {
  if (!self) return;

  guint count = self->collected_shares ? self->collected_shares->len : 0;
  guint8 threshold = (guint8)adw_spin_row_get_value(self->spin_recover_threshold);

  if (self->lbl_share_count) {
    gchar *text = g_strdup_printf("%u of %u shares collected", count, threshold);
    gtk_label_set_text(self->lbl_share_count, text);
    g_free(text);
  }

  /* Enable recover button if threshold met */
  if (self->btn_recover) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_recover), count >= threshold);
  }
}

static void clear_list_box(GtkListBox *list) {
  if (!list) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
    gtk_list_box_remove(list, child);
  }
}

/* ============================================================
 * Event Handlers - Setup Mode
 * ============================================================ */

static gboolean is_valid_npub(const gchar *s) {
  if (!s) return FALSE;
  if (!g_str_has_prefix(s, "npub1")) return FALSE;
  if (strlen(s) < 60) return FALSE;  /* npub is ~63 chars */
  return TRUE;
}

static void on_add_guardian(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  const gchar *npub = gtk_editable_get_text(GTK_EDITABLE(self->entry_guardian_npub));
  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_guardian_name));

  if (!npub || !*npub) {
    show_error(self, "Guardian Required", "Please enter the guardian's npub.");
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_guardian_npub));
    return;
  }

  if (!is_valid_npub(npub)) {
    show_error(self, "Invalid npub",
               "Please enter a valid npub (starts with 'npub1').");
    return;
  }

  /* Check for duplicate */
  for (guint i = 0; i < self->pending_guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(self->pending_guardians, i);
    if (g_strcmp0(g->npub, npub) == 0) {
      show_error(self, "Duplicate Guardian",
                 "This guardian has already been added.");
      return;
    }
  }

  /* Check not adding self */
  if (self->current_npub && g_strcmp0(npub, self->current_npub) == 0) {
    show_error(self, "Cannot Add Self",
               "You cannot be your own recovery guardian.");
    return;
  }

  /* Check maximum */
  if (self->pending_guardians->len >= MAX_GUARDIANS) {
    show_error(self, "Maximum Guardians",
               "Maximum 10 guardians can be added.");
    return;
  }

  /* Add guardian */
  GnGuardian *guardian = gn_guardian_new(npub, name && *name ? name : NULL);
  g_ptr_array_add(self->pending_guardians, guardian);

  /* Add row to UI */
  GtkWidget *row = create_guardian_row(self, npub,
                                       name && *name ? name : NULL,
                                       self->pending_guardians->len - 1);
  gtk_list_box_append(self->list_guardians, row);

  /* Clear inputs */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_guardian_npub), "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_guardian_name), "");
  gtk_widget_grab_focus(GTK_WIDGET(self->entry_guardian_npub));

  update_threshold_ui(self);
  update_guardian_list(self);
}

static void on_threshold_changed(AdwSpinRow *spin, GParamSpec *pspec, gpointer user_data) {
  (void)spin;
  (void)pspec;
  SheetSocialRecovery *self = user_data;
  update_threshold_ui(self);
}

static void on_setup_recovery(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  if (!self->pending_guardians || self->pending_guardians->len < 2) {
    show_error(self, "Not Enough Guardians",
               "At least 2 guardians are required for social recovery.");
    return;
  }

  const gchar *nsec = get_nsec(self);
  if (!nsec) {
    show_error(self, "Key Not Available",
               "Could not retrieve secret key from secure storage.");
    return;
  }

  guint8 threshold = (guint8)adw_spin_row_get_value(self->spin_threshold);

  /* Clear previous setup */
  if (self->setup_config) {
    gn_recovery_config_free(self->setup_config);
    self->setup_config = NULL;
  }
  if (self->encrypted_shares) {
    g_ptr_array_unref(self->encrypted_shares);
    self->encrypted_shares = NULL;
  }

  /* Perform setup */
  GError *error = NULL;
  GnRecoveryConfig *config = NULL;
  GPtrArray *shares = NULL;

  if (!gn_social_recovery_setup(nsec, threshold, self->pending_guardians,
                                &config, &shares, &error)) {
    show_error(self, "Setup Failed",
               error ? error->message : "Failed to set up social recovery.");
    g_clear_error(&error);
    return;
  }

  self->setup_config = config;
  self->encrypted_shares = shares;

  /* Save configuration */
  if (!gn_recovery_config_save(config, &error)) {
    show_error(self, "Save Warning",
               error ? error->message : "Configuration created but could not be saved.");
    g_clear_error(&error);
  }

  /* Update UI to show shares */
  gtk_widget_set_visible(GTK_WIDGET(self->group_setup_result), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->group_shares), TRUE);

  if (self->lbl_setup_status) {
    gchar *status = g_strdup_printf(
      "Social recovery configured: %u-of-%u threshold.\n"
      "Send the encrypted shares below to each guardian.",
      threshold, (guint)self->pending_guardians->len
    );
    gtk_label_set_text(self->lbl_setup_status, status);
    g_free(status);
  }

  /* Populate share distribution list */
  clear_list_box(self->list_shares);
  for (guint i = 0; i < config->guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(config->guardians, i);
    const gchar *encrypted = g_ptr_array_index(shares, i);

    GtkWidget *row = create_share_row(self, g->npub, g->label, encrypted, i);
    gtk_list_box_append(self->list_shares, row);
  }

  /* Disable setup button */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_setup_recovery), FALSE);

  /* Notify callback */
  if (self->on_complete) {
    self->on_complete(self->current_npub, self->on_complete_ud);
  }

  show_toast(self, "Social recovery set up successfully!");
}

/* ============================================================
 * Event Handlers - Manage Mode
 * ============================================================ */

static void load_existing_config(SheetSocialRecovery *self) {
  if (!self || !self->current_npub) return;

  if (self->loaded_config) {
    gn_recovery_config_free(self->loaded_config);
    self->loaded_config = NULL;
  }

  GError *error = NULL;
  self->loaded_config = gn_recovery_config_load(self->current_npub, &error);

  if (!self->loaded_config) {
    /* No config - show setup prompt */
    gtk_widget_set_visible(GTK_WIDGET(self->status_no_config), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->group_config_info), FALSE);
    return;
  }

  /* Show config info */
  gtk_widget_set_visible(GTK_WIDGET(self->status_no_config), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->group_config_info), TRUE);

  /* Update threshold display */
  if (self->row_config_threshold) {
    gchar *threshold_text = g_strdup_printf(
      "%u of %u guardians",
      self->loaded_config->threshold,
      self->loaded_config->total_shares
    );
    adw_action_row_set_subtitle(self->row_config_threshold, threshold_text);
    g_free(threshold_text);
  }

  /* Update created date */
  if (self->row_config_created) {
    GDateTime *dt = g_date_time_new_from_unix_local(self->loaded_config->created_at);
    if (dt) {
      gchar *date_str = g_date_time_format(dt, "%Y-%m-%d %H:%M");
      adw_action_row_set_subtitle(self->row_config_created, date_str);
      g_free(date_str);
      g_date_time_unref(dt);
    }
  }

  /* Populate guardian list */
  clear_list_box(self->list_manage_guardians);
  for (guint i = 0; i < self->loaded_config->guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(self->loaded_config->guardians, i);

    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

    const gchar *title = g->label;
    gchar *title_owned = NULL;
    if (!title || !*title) {
      title_owned = g_strdup_printf("Guardian %u", i + 1);
      title = title_owned;
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

    gchar *subtitle = g_strdup_printf("Share #%u - %.12s...", g->share_index, g->npub + 5);
    adw_action_row_set_subtitle(row, subtitle);
    g_free(subtitle);
    g_free(title_owned);

    /* Status icon */
    const gchar *icon = g->confirmed ? "emblem-ok-symbolic" : "emblem-important-symbolic";
    GtkImage *status = GTK_IMAGE(gtk_image_new_from_icon_name(icon));
    gtk_widget_set_tooltip_text(GTK_WIDGET(status),
                                g->confirmed ? "Confirmed" : "Pending confirmation");
    adw_action_row_add_suffix(row, GTK_WIDGET(status));

    gtk_list_box_append(self->list_manage_guardians, GTK_WIDGET(row));
  }
}

static void on_delete_recovery_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetSocialRecovery *self = user_data;
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);

  GError *error = NULL;
  int choice = gtk_alert_dialog_choose_finish(dialog, result, &error);

  if (error) {
    g_error_free(error);
    return;
  }

  if (choice == 1) {  /* Delete */
    GError *del_error = NULL;
    if (!gn_recovery_config_delete(self->current_npub, &del_error)) {
      show_error(self, "Delete Failed",
                 del_error ? del_error->message : "Failed to delete configuration.");
      g_clear_error(&del_error);
      return;
    }

    if (self->loaded_config) {
      gn_recovery_config_free(self->loaded_config);
      self->loaded_config = NULL;
    }

    load_existing_config(self);
    show_toast(self, "Social recovery configuration deleted.");
  }
}

static void on_delete_recovery(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  GtkAlertDialog *ad = gtk_alert_dialog_new("Delete Social Recovery?");
  gtk_alert_dialog_set_detail(ad,
    "This will delete your social recovery configuration. "
    "You will no longer be able to recover this key using guardian shares.\n\n"
    "Any existing shares held by guardians will become useless.\n\n"
    "This action cannot be undone.");

  const char *buttons[] = { "Cancel", "Delete", NULL };
  gtk_alert_dialog_set_buttons(ad, buttons);
  gtk_alert_dialog_set_cancel_button(ad, 0);
  gtk_alert_dialog_set_default_button(ad, 0);

  gtk_alert_dialog_choose(ad,
                          GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                          NULL,
                          on_delete_recovery_response,
                          self);
  g_object_unref(ad);
}

/* ============================================================
 * Event Handlers - Recover Mode
 * ============================================================ */

static void on_add_share(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  const gchar *share_str = gtk_editable_get_text(GTK_EDITABLE(self->entry_share_input));

  if (!share_str || !*share_str) {
    show_error(self, "Share Required", "Please paste an encrypted share.");
    return;
  }

  /* Try to decode - could be raw share or encrypted JSON */
  GnSSSShare *share = NULL;
  GError *error = NULL;

  if (g_str_has_prefix(share_str, "sss1:")) {
    /* Raw share format */
    share = gn_sss_share_decode(share_str, &error);
  } else if (share_str[0] == '{') {
    /* JSON encrypted share - need to decrypt
     * For recovery, we need the user's nsec (as guardian) and owner's npub */
    show_error(self, "Encrypted Share",
               "This appears to be an encrypted share. Please ask the guardian "
               "to decrypt it first using their key, or provide the raw share format (sss1:...).");
    return;
  } else {
    show_error(self, "Invalid Share Format",
               "Share must start with 'sss1:' or be a valid encrypted JSON object.");
    return;
  }

  if (!share) {
    show_error(self, "Invalid Share",
               error ? error->message : "Failed to decode share.");
    g_clear_error(&error);
    return;
  }

  /* Check for duplicate index */
  for (guint i = 0; i < self->collected_shares->len; i++) {
    GnSSSShare *existing = g_ptr_array_index(self->collected_shares, i);
    if (existing->index == share->index) {
      gn_sss_share_free(share);
      show_error(self, "Duplicate Share",
                 "A share with this index has already been added.");
      return;
    }
  }

  /* Add share */
  g_ptr_array_add(self->collected_shares, share);

  /* Add row */
  GtkWidget *row = create_collected_share_row(self, share, self->collected_shares->len - 1);
  gtk_list_box_append(self->list_collected_shares, row);

  /* Clear input */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_share_input), "");

  update_share_count(self);
}

static void on_recover(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  if (!self->collected_shares || self->collected_shares->len == 0) {
    show_error(self, "No Shares", "Please add at least one share.");
    return;
  }

  guint8 threshold = (guint8)adw_spin_row_get_value(self->spin_recover_threshold);

  if (self->collected_shares->len < threshold) {
    show_error(self, "Not Enough Shares",
               "You need more shares to meet the threshold.");
    return;
  }

  /* Attempt recovery */
  GError *error = NULL;
  gchar *nsec = NULL;

  if (!gn_social_recovery_recover(self->collected_shares, threshold, &nsec, &error)) {
    show_error(self, "Recovery Failed",
               error ? error->message : "Failed to reconstruct key from shares.");
    g_clear_error(&error);
    return;
  }

  /* Get npub for display */
  gchar *npub = NULL;
  if (!gn_backup_get_npub(nsec, &npub, &error)) {
    secure_free_string(&nsec);
    show_error(self, "Key Error",
               error ? error->message : "Recovered key appears invalid.");
    g_clear_error(&error);
    return;
  }

  /* Store for import */
  secure_free_string(&self->recovered_nsec);
  self->recovered_nsec = nsec;

  /* Show result */
  gtk_widget_set_visible(GTK_WIDGET(self->group_recovery_result), TRUE);

  if (self->row_recovered_npub) {
    adw_action_row_set_subtitle(self->row_recovered_npub, npub);
  }

  g_free(npub);

  show_toast(self, "Key recovered successfully!");
}

static void on_import_recovered(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;

  if (!self->recovered_nsec) {
    show_error(self, "No Key", "No recovered key available to import.");
    return;
  }

  /* Import via accounts store */
  AccountsStore *accounts = accounts_store_get_default();
  gchar *npub = NULL;
  GError *error = NULL;

  /* Note: This is a simplified import. In production, you'd want to
   * use the same D-Bus mechanism as sheet-backup.c for consistency */
  if (accounts) {
    if (!accounts_store_import_key(accounts, self->recovered_nsec, "Recovered Key", &npub, NULL)) {
      show_error(self, "Import Failed",
                 "Failed to import recovered key to accounts.");
      return;
    }
  } else {
    /* Fallback to secret store directly */
    SecretStoreResult res = secret_store_add(self->recovered_nsec, "Recovered Key", TRUE);
    if (res != SECRET_STORE_OK) {
      show_error(self, "Import Failed",
                 "Failed to store recovered key.");
      return;
    }
    gn_backup_get_npub(self->recovered_nsec, &npub, NULL);
  }

  /* Notify callback */
  if (self->on_complete && npub) {
    self->on_complete(npub, self->on_complete_ud);
  }

  show_toast(self, "Key imported successfully!");

  /* Clear and close */
  clear_sensitive_data(self);
  g_free(npub);
  adw_dialog_close(ADW_DIALOG(self));
}

/* ============================================================
 * Common Handlers
 * ============================================================ */

static void on_close(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSocialRecovery *self = user_data;
  clear_sensitive_data(self);
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_view_changed(AdwViewStack *stack, GParamSpec *pspec, gpointer user_data) {
  (void)stack;
  (void)pspec;
  SheetSocialRecovery *self = user_data;

  const gchar *name = adw_view_stack_get_visible_child_name(stack);

  if (g_strcmp0(name, "setup") == 0) {
    self->mode = SHEET_SOCIAL_RECOVERY_MODE_SETUP;
  } else if (g_strcmp0(name, "manage") == 0) {
    self->mode = SHEET_SOCIAL_RECOVERY_MODE_MANAGE;
    load_existing_config(self);
  } else if (g_strcmp0(name, "recover") == 0) {
    self->mode = SHEET_SOCIAL_RECOVERY_MODE_RECOVER;
  }
}

/* ============================================================
 * GObject Implementation
 * ============================================================ */

static void sheet_social_recovery_dispose(GObject *obj) {
  SheetSocialRecovery *self = SHEET_SOCIAL_RECOVERY(obj);

  clear_sensitive_data(self);

  g_free(self->current_npub);
  self->current_npub = NULL;

  g_clear_pointer(&self->pending_guardians, g_ptr_array_unref);

  if (self->loaded_config) {
    gn_recovery_config_free(self->loaded_config);
    self->loaded_config = NULL;
  }

  G_OBJECT_CLASS(sheet_social_recovery_parent_class)->dispose(obj);
}

static void sheet_social_recovery_class_init(SheetSocialRecoveryClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->dispose = sheet_social_recovery_dispose;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-social-recovery.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, view_stack);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_close);

  /* Setup mode */
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, row_setup_account);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, list_guardians);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, entry_guardian_npub);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, entry_guardian_name);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_add_guardian);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, lbl_guardian_count);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, spin_threshold);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, lbl_threshold_info);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_setup_recovery);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, group_setup_result);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, lbl_setup_status);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, group_shares);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, list_shares);

  /* Manage mode */
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, row_manage_account);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, status_no_config);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, group_config_info);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, row_config_threshold);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, row_config_created);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, list_manage_guardians);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_delete_recovery);

  /* Recover mode */
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, entry_recover_npub);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, spin_recover_threshold);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, list_collected_shares);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, entry_share_input);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_add_share);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, lbl_share_count);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_recover);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, group_recovery_result);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, row_recovered_npub);
  gtk_widget_class_bind_template_child(wc, SheetSocialRecovery, btn_import_recovered);
}

static void sheet_social_recovery_init(SheetSocialRecovery *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize arrays */
  self->pending_guardians = g_ptr_array_new_with_free_func((GDestroyNotify)gn_guardian_free);
  self->collected_shares = g_ptr_array_new_with_free_func((GDestroyNotify)gn_sss_share_free);

  /* Connect signals */
  if (self->btn_close)
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close), self);

  /* Setup mode signals */
  if (self->btn_add_guardian)
    g_signal_connect(self->btn_add_guardian, "clicked", G_CALLBACK(on_add_guardian), self);
  if (self->spin_threshold)
    g_signal_connect(self->spin_threshold, "notify::value", G_CALLBACK(on_threshold_changed), self);
  if (self->btn_setup_recovery)
    g_signal_connect(self->btn_setup_recovery, "clicked", G_CALLBACK(on_setup_recovery), self);

  /* Manage mode signals */
  if (self->btn_delete_recovery)
    g_signal_connect(self->btn_delete_recovery, "clicked", G_CALLBACK(on_delete_recovery), self);

  /* Recover mode signals */
  if (self->btn_add_share)
    g_signal_connect(self->btn_add_share, "clicked", G_CALLBACK(on_add_share), self);
  if (self->btn_recover)
    g_signal_connect(self->btn_recover, "clicked", G_CALLBACK(on_recover), self);
  if (self->btn_import_recovered)
    g_signal_connect(self->btn_import_recovered, "clicked", G_CALLBACK(on_import_recovered), self);

  /* View stack changes */
  if (self->view_stack)
    g_signal_connect(self->view_stack, "notify::visible-child-name",
                     G_CALLBACK(on_view_changed), self);

  /* Initial state */
  update_guardian_list(self);
  update_share_count(self);
}

/* ============================================================
 * Public API
 * ============================================================ */

SheetSocialRecovery *sheet_social_recovery_new(void) {
  return g_object_new(TYPE_SHEET_SOCIAL_RECOVERY, NULL);
}

void sheet_social_recovery_set_account(SheetSocialRecovery *self, const gchar *npub) {
  g_return_if_fail(SHEET_IS_SOCIAL_RECOVERY(self));

  g_free(self->current_npub);
  self->current_npub = g_strdup(npub);

  /* Update account display in setup mode */
  if (self->row_setup_account && npub) {
    gchar *truncated = NULL;
    if (strlen(npub) > 20) {
      truncated = g_strdup_printf("%.12s...%.8s", npub, npub + strlen(npub) - 8);
    } else {
      truncated = g_strdup(npub);
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_setup_account), truncated);
    g_free(truncated);
  }

  /* Update manage mode */
  if (self->row_manage_account && npub) {
    gchar *truncated = NULL;
    if (strlen(npub) > 20) {
      truncated = g_strdup_printf("%.12s...%.8s", npub, npub + strlen(npub) - 8);
    } else {
      truncated = g_strdup(npub);
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_manage_account), truncated);
    g_free(truncated);
  }

  /* Load existing config if in manage mode */
  if (self->mode == SHEET_SOCIAL_RECOVERY_MODE_MANAGE) {
    load_existing_config(self);
  }
}

void sheet_social_recovery_set_mode(SheetSocialRecovery *self, SheetSocialRecoveryMode mode) {
  g_return_if_fail(SHEET_IS_SOCIAL_RECOVERY(self));

  self->mode = mode;

  if (self->view_stack) {
    switch (mode) {
      case SHEET_SOCIAL_RECOVERY_MODE_SETUP:
        adw_view_stack_set_visible_child_name(self->view_stack, "setup");
        break;
      case SHEET_SOCIAL_RECOVERY_MODE_MANAGE:
        adw_view_stack_set_visible_child_name(self->view_stack, "manage");
        load_existing_config(self);
        break;
      case SHEET_SOCIAL_RECOVERY_MODE_RECOVER:
        adw_view_stack_set_visible_child_name(self->view_stack, "recover");
        break;
    }
  }
}

void sheet_social_recovery_set_on_complete(SheetSocialRecovery *self,
                                           SheetSocialRecoveryCallback callback,
                                           gpointer user_data) {
  g_return_if_fail(SHEET_IS_SOCIAL_RECOVERY(self));
  self->on_complete = callback;
  self->on_complete_ud = user_data;
}
