/* sheet-backup.c - Backup and Recovery dialog implementation
 *
 * Provides UI for:
 * - NIP-49 encrypted backup export (ncryptsec)
 * - Save to file with file chooser
 * - QR code display for backup string
 * - Import from ncryptsec
 * - Import from BIP-39 mnemonic
 * - Verification before importing
 */
#include "sheet-backup.h"
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include "../../backup-recovery.h"
#include "../../secret_store.h"
#include "../../secure-delete.h"
#include "../../accounts_store.h"
#include "../../keyboard-nav.h"
#include "../../qr-code.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

/* Clipboard clear timeout in seconds */
#define CLIPBOARD_CLEAR_TIMEOUT_SECONDS 60

struct _SheetBackup {
  AdwDialog parent_instance;

  /* View Stack */
  AdwViewStack *view_stack;
  AdwViewSwitcherBar *view_switcher;

  /* Close button */
  GtkButton *btn_close;

  /* Backup tab widgets */
  AdwBanner *banner_warning;
  AdwActionRow *row_account;
  AdwPasswordEntryRow *entry_backup_password;
  AdwPasswordEntryRow *entry_backup_password_confirm;
  AdwComboRow *combo_security;
  GtkButton *btn_create_backup;
  GtkButton *btn_save_to_file;
  AdwPreferencesGroup *group_backup_result;
  GtkLabel *lbl_backup_result;
  GtkButton *btn_copy_backup;
  GtkButton *btn_show_qr;
  GtkBox *box_qr_display;
  GtkPicture *picture_qr;
  GtkButton *btn_hide_qr;
  AdwActionRow *row_copy_nsec;

  /* Recovery tab widgets */
  AdwComboRow *combo_recovery_method;
  AdwPreferencesGroup *group_ncryptsec_recovery;
  AdwEntryRow *entry_ncryptsec;
  AdwPasswordEntryRow *entry_decrypt_password;
  GtkButton *btn_load_from_file;
  AdwPreferencesGroup *group_mnemonic_recovery;
  AdwEntryRow *entry_mnemonic;
  AdwPasswordEntryRow *entry_mnemonic_passphrase;
  AdwSpinRow *spin_account_index;
  AdwPreferencesGroup *group_preview;
  AdwActionRow *row_preview_npub;
  AdwStatusPage *status_verification;
  GtkButton *btn_verify;
  GtkButton *btn_import;

  /* State */
  gchar *current_npub;
  gchar *cached_nsec;
  gchar *cached_ncryptsec;
  gchar *verified_nsec;  /* nsec from verification, ready to import */

  /* Import callback */
  SheetBackupImportCallback on_import;
  gpointer on_import_ud;
};

G_DEFINE_TYPE(SheetBackup, sheet_backup, ADW_TYPE_DIALOG)

/* Forward declarations */
static void clear_sensitive_data(SheetBackup *self);
static void on_recovery_method_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);

/* Securely clear a string */
static void secure_free_string(gchar **str) {
  if (str && *str) {
    gn_secure_shred_string(*str);
    g_free(*str);
    *str = NULL;
  }
}

/* Clear all cached sensitive data */
static void clear_sensitive_data(SheetBackup *self) {
  if (!self) return;
  secure_free_string(&self->cached_nsec);
  secure_free_string(&self->cached_ncryptsec);
  secure_free_string(&self->verified_nsec);
}

/* Get the nsec for the current account (lazy load from secret store) */
static const gchar *get_nsec(SheetBackup *self) {
  if (!self) return NULL;

  /* Return cached value if available */
  if (self->cached_nsec) return self->cached_nsec;

  /* Try to get from secret store */
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

/* Copy text to clipboard with auto-clear */
static void copy_to_clipboard(SheetBackup *self, const gchar *text, gboolean schedule_clear) {
  if (!self || !text) return;

  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) {
      gdk_clipboard_set_text(cb, text);
      if (schedule_clear) {
        gn_clipboard_clear_after(cb, CLIPBOARD_CLEAR_TIMEOUT_SECONDS);
      }
    }
  }
}

/* Show toast notification */
static void show_toast(SheetBackup *self, const gchar *message) {
  if (!self || !message) return;

  AdwToast *toast = adw_toast_new(message);
  adw_toast_set_timeout(toast, 3);

  /* Find the toast overlay in the dialog */
  GtkWidget *root = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_DIALOG);
  if (root) {
    /* Use an alert dialog as fallback since AdwDialog doesn't have built-in toast */
    GtkAlertDialog *ad = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
  }
  g_object_unref(toast);
}

/* Show error dialog */
static void show_error(SheetBackup *self, const gchar *title, const gchar *message) {
  if (!self) return;

  GtkAlertDialog *ad = gtk_alert_dialog_new("%s", title);
  gtk_alert_dialog_set_detail(ad, message);
  gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
  g_object_unref(ad);
}

/* Map security combo index to GnBackupSecurityLevel */
static GnBackupSecurityLevel get_security_level(SheetBackup *self) {
  if (!self || !self->combo_security) return GN_BACKUP_SECURITY_NORMAL;

  guint idx = adw_combo_row_get_selected(self->combo_security);
  switch (idx) {
    case 1: return GN_BACKUP_SECURITY_HIGH;
    case 2: return GN_BACKUP_SECURITY_PARANOID;
    default: return GN_BACKUP_SECURITY_NORMAL;
  }
}

/* Handler: Close button */
static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (self) {
    clear_sensitive_data(self);
    adw_dialog_close(ADW_DIALOG(self));
  }
}

/* Handler: Create backup button */
static void on_create_backup(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  /* Get and validate passwords */
  const gchar *password = gtk_editable_get_text(GTK_EDITABLE(self->entry_backup_password));
  const gchar *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_backup_password_confirm));

  if (!password || !*password) {
    show_error(self, "Password Required", "Please enter a password for encryption.");
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_backup_password));
    return;
  }

  if (strlen(password) < 8) {
    show_error(self, "Weak Password",
               "Password should be at least 8 characters for adequate security.");
    return;
  }

  if (g_strcmp0(password, confirm) != 0) {
    show_error(self, "Password Mismatch", "The passwords do not match.");
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_backup_password_confirm));
    return;
  }

  /* Get nsec */
  const gchar *nsec = get_nsec(self);
  if (!nsec) {
    show_error(self, "Key Not Found",
               "Could not retrieve secret key from secure storage.");
    return;
  }

  /* Create encrypted backup */
  gchar *ncryptsec = NULL;
  GError *error = NULL;
  GnBackupSecurityLevel security = get_security_level(self);

  gboolean ok = gn_backup_export_nip49(nsec, password, security, &ncryptsec, &error);

  if (!ok) {
    show_error(self, "Encryption Failed",
               error ? error->message : "Unknown error during encryption.");
    g_clear_error(&error);
    return;
  }

  /* Cache and display result */
  secure_free_string(&self->cached_ncryptsec);
  self->cached_ncryptsec = ncryptsec;

  gtk_label_set_text(self->lbl_backup_result, ncryptsec);
  gtk_widget_set_visible(GTK_WIDGET(self->group_backup_result), TRUE);

  /* Clear password fields */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_backup_password), "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_backup_password_confirm), "");

  show_toast(self, "Backup created successfully!");
}

/* Get identity name from accounts store */
static gchar *get_identity_name(SheetBackup *self) {
  if (!self || !self->current_npub) return NULL;

  /* Try to get from accounts store */
  AccountsStore *accounts = accounts_store_get_default();
  if (accounts) {
    return accounts_store_get_display_name(accounts, self->current_npub);
  }
  return NULL;
}

/* File save dialog callback */
static void on_save_file_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetBackup *self = SHEET_BACKUP(user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);

  GError *error = NULL;
  GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
      show_error(self, "Save Failed", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!file) return;

  /* Get password and validate */
  const gchar *password = gtk_editable_get_text(GTK_EDITABLE(self->entry_backup_password));
  const gchar *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_backup_password_confirm));

  if (!password || !*password) {
    show_error(self, "Password Required", "Please enter a password for encryption.");
    g_object_unref(file);
    return;
  }

  if (g_strcmp0(password, confirm) != 0) {
    show_error(self, "Password Mismatch", "The passwords do not match.");
    g_object_unref(file);
    return;
  }

  /* Get nsec */
  const gchar *nsec = get_nsec(self);
  if (!nsec) {
    show_error(self, "Key Not Found",
               "Could not retrieve secret key from secure storage.");
    g_object_unref(file);
    return;
  }

  /* Export to file with metadata */
  gchar *filepath = g_file_get_path(file);
  g_object_unref(file);

  GError *export_error = NULL;
  GnBackupSecurityLevel security = get_security_level(self);

  /* Get identity name for metadata */
  gchar *identity_name = get_identity_name(self);

  /* Use the new metadata-aware export function */
  gboolean ok = gn_backup_export_to_file_with_metadata(nsec, password, security,
                                                         identity_name, filepath,
                                                         &export_error);
  g_free(identity_name);
  g_free(filepath);

  if (!ok) {
    show_error(self, "Export Failed",
               export_error ? export_error->message : "Failed to write backup file.");
    g_clear_error(&export_error);
    return;
  }

  /* Clear password fields */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_backup_password), "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_backup_password_confirm), "");

  show_toast(self, "Backup saved to file successfully!");
}

/* Handler: Save to file button */
static void on_save_to_file(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  /* Validate password first */
  const gchar *password = gtk_editable_get_text(GTK_EDITABLE(self->entry_backup_password));
  if (!password || !*password) {
    show_error(self, "Password Required",
               "Please enter a password for encryption before saving.");
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_backup_password));
    return;
  }

  /* Create file dialog */
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Save Encrypted Backup");

  /* Suggest filename - use .json for new metadata format */
  gchar *npub_short = NULL;
  if (self->current_npub && strlen(self->current_npub) > 12) {
    npub_short = g_strndup(self->current_npub + 5, 8);
  } else {
    npub_short = g_strdup("key");
  }
  g_autofree gchar *suggested_name = g_strdup_printf("nostr-backup-%s.json", npub_short);
  g_free(npub_short);

  gtk_file_dialog_set_initial_name(dialog, suggested_name);

  /* Add file filter - use JSON for new format with metadata */
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Nostr Backup Files (JSON)");
  gtk_file_filter_add_pattern(filter, "*.json");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  g_object_unref(filter);

  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  g_object_unref(filters);

  /* Show save dialog */
  gtk_file_dialog_save(dialog,
                       GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                       NULL,
                       on_save_file_response,
                       self);
  g_object_unref(dialog);
}

/* Handler: Copy backup button */
static void on_copy_backup(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self || !self->cached_ncryptsec) return;

  copy_to_clipboard(self, self->cached_ncryptsec, TRUE);
  show_toast(self, "Backup copied to clipboard (will clear in 60s)");
}

/* Handler: Show QR button */
static void on_show_qr(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self || !self->cached_ncryptsec) return;

  /* Check if QR generation is available */
  if (!gn_qr_generation_available()) {
    show_error(self, "QR Code Unavailable",
               "QR code display requires the qrencode library.\n\n"
               "Your encrypted backup string has been copied to clipboard.\n"
               "You can use an external QR code generator if needed.");
    copy_to_clipboard(self, self->cached_ncryptsec, TRUE);
    return;
  }

  /* Create and present the QR display dialog */
  SheetQrDisplay *qr_dlg = sheet_qr_display_new();
  sheet_qr_display_set_ncryptsec(qr_dlg, self->cached_ncryptsec);
  adw_dialog_present(ADW_DIALOG(qr_dlg), GTK_WIDGET(self));
}

/* Handler: Hide QR button */
static void on_hide_qr(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  gtk_widget_set_visible(GTK_WIDGET(self->box_qr_display), FALSE);
}

/* Callback for copy nsec warning dialog */
static void on_copy_nsec_warning_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetBackup *self = SHEET_BACKUP(user_data);
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);

  GError *error = NULL;
  int choice = gtk_alert_dialog_choose_finish(dialog, result, &error);

  if (error) {
    g_error_free(error);
    return;
  }

  if (choice == 1) {  /* "Copy Anyway" */
    const gchar *nsec = get_nsec(self);
    if (nsec) {
      copy_to_clipboard(self, nsec, TRUE);
      show_toast(self, "Secret key copied (will clear in 60s)");
    }
  }
}

/* Handler: Copy raw nsec row activated */
static void on_copy_nsec_activated(AdwActionRow *row, gpointer user_data) {
  (void)row;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  GtkAlertDialog *ad = gtk_alert_dialog_new("Warning: Copying Raw Secret Key");
  gtk_alert_dialog_set_detail(ad,
    "Your secret key (nsec) gives full control over your Nostr identity. "
    "Anyone with this key can:\n\n"
    "  - Post messages as you\n"
    "  - Read your encrypted messages\n"
    "  - Access your account everywhere\n\n"
    "Never share this with anyone. Developers will NEVER ask for it.\n\n"
    "Consider using the encrypted backup (ncryptsec) instead, which is "
    "password-protected and safer to store.");

  const char *buttons[] = { "Cancel", "Copy Anyway", NULL };
  gtk_alert_dialog_set_buttons(ad, buttons);
  gtk_alert_dialog_set_cancel_button(ad, 0);
  gtk_alert_dialog_set_default_button(ad, 0);

  gtk_alert_dialog_choose(ad,
                          GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                          NULL,
                          on_copy_nsec_warning_response,
                          self);
  g_object_unref(ad);
}

/* Handler: Recovery method changed */
static void on_recovery_method_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)obj;
  (void)pspec;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  guint method = adw_combo_row_get_selected(self->combo_recovery_method);

  /* Show/hide appropriate sections */
  gtk_widget_set_visible(GTK_WIDGET(self->group_ncryptsec_recovery), method == 0);
  gtk_widget_set_visible(GTK_WIDGET(self->group_mnemonic_recovery), method == 1);

  /* Reset verification state */
  gtk_widget_set_visible(GTK_WIDGET(self->group_preview), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_verification), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);
  secure_free_string(&self->verified_nsec);
}

/* File load dialog callback */
static void on_load_file_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetBackup *self = SHEET_BACKUP(user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);

  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish(dialog, result, &error);

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
      show_error(self, "Load Failed", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!file) return;

  /* Read file contents */
  gchar *contents = NULL;
  gsize length = 0;
  GError *read_error = NULL;

  gchar *filepath = g_file_get_path(file);
  g_object_unref(file);

  if (!g_file_get_contents(filepath, &contents, &length, &read_error)) {
    show_error(self, "Read Failed", read_error->message);
    g_error_free(read_error);
    g_free(filepath);
    return;
  }
  g_free(filepath);

  /* Trim whitespace */
  g_strstrip(contents);

  /* Check if this is JSON format with metadata */
  if (contents[0] == '{') {
    /* Parse JSON to extract ncryptsec */
    GnBackupMetadata *meta = NULL;
    GError *parse_error = NULL;

    if (gn_backup_parse_metadata_json(contents, &meta, &parse_error)) {
      /* Set the ncryptsec from metadata */
      gtk_editable_set_text(GTK_EDITABLE(self->entry_ncryptsec), meta->ncryptsec);

      /* Show info about loaded backup */
      if (meta->identity_name && *meta->identity_name) {
        g_autofree gchar *info = g_strdup_printf("Loaded backup for: %s", meta->identity_name);
        show_toast(self, info);
      } else if (meta->npub) {
        g_autofree gchar *info = g_strdup_printf("Loaded backup for: %.12s...", meta->npub);
        show_toast(self, info);
      }

      gn_backup_metadata_free(meta);
    } else {
      /* JSON parse failed, show error */
      show_error(self, "Invalid Backup File",
                 parse_error ? parse_error->message : "Could not parse backup file");
      g_clear_error(&parse_error);
    }
  } else if (g_str_has_prefix(contents, "ncryptsec1")) {
    /* Legacy plain ncryptsec format */
    gtk_editable_set_text(GTK_EDITABLE(self->entry_ncryptsec), contents);
  } else {
    show_error(self, "Invalid Backup File",
               "The file does not contain a valid backup. "
               "Expected ncryptsec1... or JSON backup format.");
  }

  /* Securely clear contents before freeing */
  gn_secure_shred_string(contents);
  g_free(contents);
}

/* Handler: Load from file button */
static void on_load_from_file(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Load Encrypted Backup");

  /* Add file filter for backup files */
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Nostr Backup Files");
  gtk_file_filter_add_pattern(filter, "*.json");
  gtk_file_filter_add_pattern(filter, "*.ncryptsec");
  gtk_file_filter_add_pattern(filter, "*.txt");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  g_object_unref(filter);

  GtkFileFilter *all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All Files");
  gtk_file_filter_add_pattern(all_filter, "*");
  g_list_store_append(filters, all_filter);
  g_object_unref(all_filter);

  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  g_object_unref(filters);

  gtk_file_dialog_open(dialog,
                       GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                       NULL,
                       on_load_file_response,
                       self);
  g_object_unref(dialog);
}

/* Handler: Verify button */
static void on_verify(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self) return;

  /* Clear previous verification */
  secure_free_string(&self->verified_nsec);
  gtk_widget_set_visible(GTK_WIDGET(self->group_preview), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_verification), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);

  guint method = adw_combo_row_get_selected(self->combo_recovery_method);
  gchar *nsec = NULL;
  GError *error = NULL;

  if (method == 0) {
    /* NIP-49 ncryptsec */
    const gchar *encrypted = gtk_editable_get_text(GTK_EDITABLE(self->entry_ncryptsec));
    const gchar *password = gtk_editable_get_text(GTK_EDITABLE(self->entry_decrypt_password));

    if (!encrypted || !*encrypted) {
      show_error(self, "Input Required", "Please enter the encrypted backup string.");
      gtk_widget_grab_focus(GTK_WIDGET(self->entry_ncryptsec));
      return;
    }

    if (!password || !*password) {
      show_error(self, "Password Required", "Please enter the decryption password.");
      gtk_widget_grab_focus(GTK_WIDGET(self->entry_decrypt_password));
      return;
    }

    if (!gn_backup_import_nip49(encrypted, password, &nsec, &error)) {
      show_error(self, "Decryption Failed",
                 error ? error->message : "Wrong password or corrupted backup.");
      g_clear_error(&error);
      return;
    }
  } else {
    /* BIP-39 mnemonic */
    const gchar *mnemonic = gtk_editable_get_text(GTK_EDITABLE(self->entry_mnemonic));
    const gchar *passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_mnemonic_passphrase));
    guint account = (guint)adw_spin_row_get_value(self->spin_account_index);

    if (!mnemonic || !*mnemonic) {
      show_error(self, "Input Required", "Please enter your recovery phrase.");
      gtk_widget_grab_focus(GTK_WIDGET(self->entry_mnemonic));
      return;
    }

    if (!gn_backup_import_mnemonic(mnemonic, passphrase, account, &nsec, &error)) {
      show_error(self, "Recovery Failed",
                 error ? error->message : "Invalid mnemonic phrase.");
      g_clear_error(&error);
      return;
    }
  }

  /* Get npub for preview */
  gchar *npub = NULL;
  if (!gn_backup_get_npub(nsec, &npub, &error)) {
    show_error(self, "Verification Failed",
               error ? error->message : "Could not derive public key.");
    secure_free_string(&nsec);
    g_clear_error(&error);
    return;
  }

  /* Store verified nsec for import */
  self->verified_nsec = nsec;

  /* Update preview */
  adw_action_row_set_subtitle(self->row_preview_npub, npub);
  g_free(npub);

  /* Show verification success */
  gtk_widget_set_visible(GTK_WIDGET(self->group_preview), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_verification), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), TRUE);

  show_toast(self, "Backup verified successfully!");
}

/* Import via D-Bus callback */
static void on_import_dbus_done(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetBackup *self = SHEET_BACKUP(user_data);
  GDBusConnection *bus = G_DBUS_CONNECTION(source);

  GError *error = NULL;
  GVariant *ret = g_dbus_connection_call_finish(bus, result, &error);

  if (error) {
    show_error(self, "Import Failed", error->message);
    g_error_free(error);
    return;
  }

  gboolean ok = FALSE;
  const gchar *npub_result = NULL;
  g_variant_get(ret, "(bs)", &ok, &npub_result);

  if (!ok) {
    show_error(self, "Import Failed",
               "The daemon rejected the key import.\n\n"
               "Hints:\n"
               "- Ensure daemon has NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1\n"
               "- Check if key already exists");
    g_variant_unref(ret);
    return;
  }

  gchar *npub = g_strdup(npub_result);
  g_variant_unref(ret);

  /* Notify callback */
  if (self->on_import) {
    self->on_import(npub, self->on_import_ud);
  }

  show_toast(self, "Key imported successfully!");

  /* Clear sensitive data and close */
  clear_sensitive_data(self);
  g_free(npub);

  adw_dialog_close(ADW_DIALOG(self));
}

/* Handler: Import button */
static void on_import(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetBackup *self = SHEET_BACKUP(user_data);
  if (!self || !self->verified_nsec) return;

  /* Import via D-Bus to the daemon */
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

  if (!bus) {
    show_error(self, "Connection Failed",
               error ? error->message : "Could not connect to session bus.");
    g_clear_error(&error);
    return;
  }

  /* Disable import button while request is in flight */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);

  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "StoreKey",
                         g_variant_new("(ss)", self->verified_nsec, ""),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         10000,  /* 10 second timeout for scrypt */
                         NULL,
                         on_import_dbus_done,
                         self);

  g_object_unref(bus);
}

static void sheet_backup_dispose(GObject *obj) {
  SheetBackup *self = SHEET_BACKUP(obj);
  clear_sensitive_data(self);
  g_free(self->current_npub);
  self->current_npub = NULL;
  G_OBJECT_CLASS(sheet_backup_parent_class)->dispose(obj);
}

static void sheet_backup_class_init(SheetBackupClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->dispose = sheet_backup_dispose;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-backup.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(wc, SheetBackup, view_stack);
  gtk_widget_class_bind_template_child(wc, SheetBackup, view_switcher);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_close);

  /* Backup tab */
  gtk_widget_class_bind_template_child(wc, SheetBackup, banner_warning);
  gtk_widget_class_bind_template_child(wc, SheetBackup, row_account);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_backup_password);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_backup_password_confirm);
  gtk_widget_class_bind_template_child(wc, SheetBackup, combo_security);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_create_backup);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_save_to_file);
  gtk_widget_class_bind_template_child(wc, SheetBackup, group_backup_result);
  gtk_widget_class_bind_template_child(wc, SheetBackup, lbl_backup_result);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_copy_backup);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_show_qr);
  gtk_widget_class_bind_template_child(wc, SheetBackup, box_qr_display);
  gtk_widget_class_bind_template_child(wc, SheetBackup, picture_qr);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_hide_qr);
  gtk_widget_class_bind_template_child(wc, SheetBackup, row_copy_nsec);

  /* Recovery tab */
  gtk_widget_class_bind_template_child(wc, SheetBackup, combo_recovery_method);
  gtk_widget_class_bind_template_child(wc, SheetBackup, group_ncryptsec_recovery);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_ncryptsec);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_decrypt_password);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_load_from_file);
  gtk_widget_class_bind_template_child(wc, SheetBackup, group_mnemonic_recovery);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetBackup, entry_mnemonic_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetBackup, spin_account_index);
  gtk_widget_class_bind_template_child(wc, SheetBackup, group_preview);
  gtk_widget_class_bind_template_child(wc, SheetBackup, row_preview_npub);
  gtk_widget_class_bind_template_child(wc, SheetBackup, status_verification);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_verify);
  gtk_widget_class_bind_template_child(wc, SheetBackup, btn_import);
}

static void sheet_backup_init(SheetBackup *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signal handlers */
  if (self->btn_close)
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);

  /* Backup tab handlers */
  if (self->btn_create_backup)
    g_signal_connect(self->btn_create_backup, "clicked", G_CALLBACK(on_create_backup), self);
  if (self->btn_save_to_file)
    g_signal_connect(self->btn_save_to_file, "clicked", G_CALLBACK(on_save_to_file), self);
  if (self->btn_copy_backup)
    g_signal_connect(self->btn_copy_backup, "clicked", G_CALLBACK(on_copy_backup), self);
  if (self->btn_show_qr)
    g_signal_connect(self->btn_show_qr, "clicked", G_CALLBACK(on_show_qr), self);
  if (self->btn_hide_qr)
    g_signal_connect(self->btn_hide_qr, "clicked", G_CALLBACK(on_hide_qr), self);
  if (self->row_copy_nsec)
    g_signal_connect(self->row_copy_nsec, "activated", G_CALLBACK(on_copy_nsec_activated), self);

  /* Recovery tab handlers */
  if (self->combo_recovery_method)
    g_signal_connect(self->combo_recovery_method, "notify::selected",
                     G_CALLBACK(on_recovery_method_changed), self);
  if (self->btn_load_from_file)
    g_signal_connect(self->btn_load_from_file, "clicked", G_CALLBACK(on_load_from_file), self);
  if (self->btn_verify)
    g_signal_connect(self->btn_verify, "clicked", G_CALLBACK(on_verify), self);
  if (self->btn_import)
    g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import), self);

  /* Set default security level to "Normal" */
  if (self->combo_security)
    adw_combo_row_set_selected(self->combo_security, 0);

  /* Setup keyboard navigation (nostrc-tz8w):
   * - Focus backup password entry on dialog open
   * - Create Backup button is default for backup tab */
  gn_keyboard_nav_setup_dialog(ADW_DIALOG(self),
                                GTK_WIDGET(self->entry_backup_password),
                                GTK_WIDGET(self->btn_create_backup));
}

SheetBackup *sheet_backup_new(void) {
  return g_object_new(TYPE_SHEET_BACKUP, NULL);
}

void sheet_backup_set_account(SheetBackup *self, const gchar *npub) {
  g_return_if_fail(SHEET_IS_BACKUP(self));

  /* Clear any cached data from previous account */
  clear_sensitive_data(self);
  g_free(self->current_npub);

  self->current_npub = g_strdup(npub);

  /* Update account display */
  if (self->row_account && npub) {
    /* Truncate npub for display */
    if (strlen(npub) > 20) {
      g_autofree gchar *truncated = g_strdup_printf("%.*s...%s",
                                          12, npub,
                                          npub + strlen(npub) - 8);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_account), truncated);
    } else {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->row_account), npub);
    }
  }
}

void sheet_backup_show_backup_tab(SheetBackup *self) {
  g_return_if_fail(SHEET_IS_BACKUP(self));
  if (self->view_stack)
    adw_view_stack_set_visible_child_name(self->view_stack, "backup");
}

void sheet_backup_show_recovery_tab(SheetBackup *self) {
  g_return_if_fail(SHEET_IS_BACKUP(self));
  if (self->view_stack)
    adw_view_stack_set_visible_child_name(self->view_stack, "recovery");
}

void sheet_backup_set_on_import(SheetBackup *self,
                                 SheetBackupImportCallback callback,
                                 gpointer user_data) {
  g_return_if_fail(SHEET_IS_BACKUP(self));
  self->on_import = callback;
  self->on_import_ud = user_data;
}

/* Backup reminder dialog response handler */
static void on_reminder_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
  gchar *npub = (gchar *)user_data;

  GError *error = NULL;
  int choice = gtk_alert_dialog_choose_finish(dialog, result, &error);

  if (error) {
    g_error_free(error);
    g_free(npub);
    return;
  }

  if (choice == 1) {  /* "Backup Now" */
    /* Get an active window to present the backup sheet */
    GtkApplication *app = GTK_APPLICATION(g_application_get_default());
    GtkWindow *win = gtk_application_get_active_window(app);
    if (win) {
      SheetBackup *backup_dlg = sheet_backup_new();
      sheet_backup_set_account(backup_dlg, npub);
      adw_dialog_present(ADW_DIALOG(backup_dlg), GTK_WIDGET(win));
    }
  }

  g_free(npub);
}

void sheet_backup_trigger_reminder(GtkWindow *parent, const gchar *npub) {
  g_return_if_fail(GTK_IS_WINDOW(parent));
  g_return_if_fail(npub != NULL);

  GtkAlertDialog *ad = gtk_alert_dialog_new("Backup Your New Key");
  gtk_alert_dialog_set_detail(ad,
    "Your new Nostr identity has been created!\n\n"
    "Important: Your private key is stored securely on this device, but "
    "if you lose access to this device, you will lose your identity forever.\n\n"
    "We strongly recommend creating an encrypted backup now.");

  const char *buttons[] = { "Later", "Backup Now", NULL };
  gtk_alert_dialog_set_buttons(ad, buttons);
  gtk_alert_dialog_set_default_button(ad, 1);

  gtk_alert_dialog_choose(ad, parent, NULL,
                          on_reminder_response,
                          g_strdup(npub));
  g_object_unref(ad);
}
