/* sheet-account-backup.c - Account backup dialog implementation
 *
 * Provides UI for backing up Nostr identity keys:
 * - Show/copy raw nsec (with warnings)
 * - Create NIP-49 encrypted backup (ncryptsec)
 * - Show mnemonic seed words (if applicable)
 * - QR code display for scanning
 */
#include "sheet-account-backup.h"
#include "../app-resources.h"
#include "../../backup-recovery.h"
#include "../../secret_store.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct _SheetAccountBackup {
  AdwDialog parent_instance;

  /* Header/navigation */
  GtkButton *btn_back;

  /* Key display buttons */
  GtkButton *btn_show_seed;
  GtkButton *btn_copy_secret;
  GtkButton *btn_show_qr;

  /* NIP-49 encryption section */
  GtkPasswordEntry *entry_password;
  GtkButton *btn_create_ncrypt;

  /* Status/result display */
  GtkBox *box_result;
  GtkLabel *lbl_result;
  GtkButton *btn_copy_result;

  /* Current account info */
  gchar *current_npub;     /* npub of the account being backed up */
  gchar *cached_nsec;      /* Cached nsec for the current account */
  gchar *cached_ncryptsec; /* Last generated ncryptsec */
};

G_DEFINE_TYPE(SheetAccountBackup, sheet_account_backup, ADW_TYPE_DIALOG)

/* Forward declarations */
static void clear_sensitive_data(SheetAccountBackup *self);
static void show_result(SheetAccountBackup *self, const gchar *text, gboolean is_sensitive);
static void hide_result(SheetAccountBackup *self);

/* Securely clear a string */
static void secure_free_string(gchar **str) {
  if (str && *str) {
    gsize len = strlen(*str);
    volatile gchar *p = (volatile gchar *)*str;
    while (len--) *p++ = 0;
    g_free(*str);
    *str = NULL;
  }
}

/* Clear all cached sensitive data */
static void clear_sensitive_data(SheetAccountBackup *self) {
  if (!self) return;
  secure_free_string(&self->cached_nsec);
  secure_free_string(&self->cached_ncryptsec);
}

/* Get the nsec for the current account (lazy load from secret store) */
static const gchar *get_nsec(SheetAccountBackup *self) {
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

/* Show a result with copy button */
static void show_result(SheetAccountBackup *self, const gchar *text, gboolean is_sensitive) {
  if (!self) return;

  if (self->lbl_result) {
    gtk_label_set_text(self->lbl_result, text);
    /* For sensitive data, use a monospace font */
    if (is_sensitive) {
      gtk_widget_add_css_class(GTK_WIDGET(self->lbl_result), "monospace");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->lbl_result), "monospace");
    }
  }
  if (self->box_result) {
    gtk_widget_set_visible(GTK_WIDGET(self->box_result), TRUE);
  }
}

/* Hide the result area */
static void hide_result(SheetAccountBackup *self) {
  if (!self) return;
  if (self->box_result) {
    gtk_widget_set_visible(GTK_WIDGET(self->box_result), FALSE);
  }
}

/* Copy text to clipboard */
static void copy_to_clipboard(SheetAccountBackup *self, const gchar *text) {
  if (!self || !text) return;

  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) gdk_clipboard_set_text(cb, text);
  }
}

/* Show alert dialog */
static void show_alert(SheetAccountBackup *self, const gchar *message) {
  if (!self || !message) return;

  GtkAlertDialog *ad = gtk_alert_dialog_new("%s", message);
  gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
  g_object_unref(ad);
}

/* Handler: Back button */
static void on_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = user_data;
  if (self) {
    clear_sensitive_data(self);
    adw_dialog_close(ADW_DIALOG(self));
  }
}

/* Handler: Show seed words button */
static void on_show_seed(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self) return;

  /* Mnemonic recovery is not possible from derived key */
  show_alert(self, "Seed word recovery is not available.\n\n"
                   "If your key was created from a mnemonic, you should have "
                   "saved those words separately.\n\n"
                   "To backup your key, use the NIP-49 encrypted backup feature below.");
}

/* Callback for copy secret dialog choice */
static void on_copy_secret_dialog_done(GObject *src, GAsyncResult *res, gpointer data) {
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(data);
  if (!self) return;

  GError *err = NULL;
  int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
  if (err) {
    g_error_free(err);
    return;
  }
  if (choice == 1) { /* "Copy Anyway" */
    const gchar *nsec = get_nsec(self);
    if (nsec) {
      copy_to_clipboard(self, nsec);
      show_result(self, "Secret key copied to clipboard.\n\n"
                        "Clear your clipboard after use!", FALSE);
    }
  }
}

/* Handler: Copy secret key button */
static void on_copy_secret(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self) return;

  const gchar *nsec = get_nsec(self);
  if (!nsec) {
    show_alert(self, "Could not retrieve secret key.\n\n"
                     "Make sure the key is stored in the secret store.");
    return;
  }

  /* Show warning before copying */
  GtkAlertDialog *ad = gtk_alert_dialog_new(
    "Warning: Copying your secret key\n\n"
    "Your secret key (nsec) gives full control over your Nostr identity. "
    "Anyone with this key can:\n\n"
    "  - Post messages as you\n"
    "  - Read your encrypted messages\n"
    "  - Access your account everywhere\n\n"
    "Never share this with anyone. Developers will NEVER ask for it.\n\n"
    "The key will be copied to your clipboard.");
  gtk_alert_dialog_set_buttons(ad, (const char *[]){"Cancel", "Copy Anyway", NULL});
  gtk_alert_dialog_set_cancel_button(ad, 0);
  gtk_alert_dialog_set_default_button(ad, 0);

  /* Show dialog and handle response */
  gtk_alert_dialog_choose(ad,
                          GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))),
                          NULL,
                          on_copy_secret_dialog_done,
                          self);
  g_object_unref(ad);
}

/* Handler: Show QR code button */
static void on_show_qr(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self) return;

  /* QR code display is a placeholder for now */
  show_alert(self, "QR code display is not yet implemented.\n\n"
                   "This feature will allow you to scan your key with a mobile app.");
}

/* Handler: Create NIP-49 encrypted backup */
static void on_create_ncrypt(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self) return;

  /* Get password */
  const gchar *password = NULL;
  if (self->entry_password) {
    password = gtk_editable_get_text(GTK_EDITABLE(self->entry_password));
  }

  if (!password || !*password) {
    show_alert(self, "Please enter a password for encryption.");
    return;
  }

  /* Warn about weak passwords */
  if (strlen(password) < 8) {
    show_alert(self, "Password should be at least 8 characters.\n\n"
                     "A weak password makes your backup easier to crack.");
    return;
  }

  /* Get the nsec */
  const gchar *nsec = get_nsec(self);
  if (!nsec) {
    show_alert(self, "Could not retrieve secret key.\n\n"
                     "Make sure the key is stored in the secret store.");
    return;
  }

  /* Create encrypted backup */
  gchar *ncryptsec = NULL;
  GError *error = NULL;

  gboolean ok = gn_backup_export_nip49(nsec,
                                        password,
                                        GN_BACKUP_SECURITY_NORMAL,
                                        &ncryptsec,
                                        &error);

  if (!ok) {
    gchar *msg = g_strdup_printf("Encryption failed: %s",
                                  error ? error->message : "Unknown error");
    show_alert(self, msg);
    g_free(msg);
    if (error) g_error_free(error);
    return;
  }

  /* Cache the result */
  secure_free_string(&self->cached_ncryptsec);
  self->cached_ncryptsec = ncryptsec;

  /* Show the result */
  gchar *display = g_strdup_printf("Encrypted backup created!\n\n%s\n\n"
                                    "Save this string and your password securely.\n"
                                    "You can use it to recover your key with any NIP-49 compatible app.",
                                    ncryptsec);
  show_result(self, display, TRUE);
  g_free(display);

  /* Copy to clipboard */
  copy_to_clipboard(self, ncryptsec);

  /* Clear password field */
  if (self->entry_password) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_password), "");
  }
}

/* Handler: Copy result to clipboard */
static void on_copy_result(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self) return;

  if (self->cached_ncryptsec) {
    copy_to_clipboard(self, self->cached_ncryptsec);
    show_alert(self, "Copied to clipboard!");
  } else if (self->lbl_result) {
    const gchar *text = gtk_label_get_text(self->lbl_result);
    if (text && *text) {
      copy_to_clipboard(self, text);
    }
  }
}

/* Handler: Password entry changed */
static void on_password_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(user_data);
  if (!self || !self->btn_create_ncrypt) return;

  const gchar *password = NULL;
  if (self->entry_password) {
    password = gtk_editable_get_text(GTK_EDITABLE(self->entry_password));
  }

  /* Enable button only if password is not empty */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create_ncrypt),
                           password && *password);
}

static void sheet_account_backup_dispose(GObject *obj) {
  SheetAccountBackup *self = SHEET_ACCOUNT_BACKUP(obj);
  clear_sensitive_data(self);
  g_free(self->current_npub);
  self->current_npub = NULL;
  G_OBJECT_CLASS(sheet_account_backup_parent_class)->dispose(obj);
}

static void sheet_account_backup_class_init(SheetAccountBackupClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->dispose = sheet_account_backup_dispose;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-account-backup.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_back);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_show_seed);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_copy_secret);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_show_qr);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, entry_password);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_create_ncrypt);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, box_result);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, lbl_result);
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_copy_result);
}

static void sheet_account_backup_init(SheetAccountBackup *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signal handlers */
  if (self->btn_back)
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back), self);
  if (self->btn_show_seed)
    g_signal_connect(self->btn_show_seed, "clicked", G_CALLBACK(on_show_seed), self);
  if (self->btn_copy_secret)
    g_signal_connect(self->btn_copy_secret, "clicked", G_CALLBACK(on_copy_secret), self);
  if (self->btn_show_qr)
    g_signal_connect(self->btn_show_qr, "clicked", G_CALLBACK(on_show_qr), self);
  if (self->btn_create_ncrypt)
    g_signal_connect(self->btn_create_ncrypt, "clicked", G_CALLBACK(on_create_ncrypt), self);
  if (self->entry_password)
    g_signal_connect(self->entry_password, "changed", G_CALLBACK(on_password_changed), self);
  if (self->btn_copy_result)
    g_signal_connect(self->btn_copy_result, "clicked", G_CALLBACK(on_copy_result), self);

  /* Initially disable create button until password is entered */
  if (self->btn_create_ncrypt)
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create_ncrypt), FALSE);
}

SheetAccountBackup *sheet_account_backup_new(void) {
  return g_object_new(TYPE_SHEET_ACCOUNT_BACKUP, NULL);
}

void sheet_account_backup_set_account(SheetAccountBackup *self, const gchar *npub) {
  g_return_if_fail(SHEET_IS_ACCOUNT_BACKUP(self));

  /* Clear any cached data from previous account */
  clear_sensitive_data(self);
  g_free(self->current_npub);

  self->current_npub = g_strdup(npub);
}
