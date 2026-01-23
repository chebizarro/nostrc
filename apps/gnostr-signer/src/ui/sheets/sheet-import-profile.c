/* sheet-import-profile.c - Import Profile dialog implementation
 *
 * Provides a UI for importing an existing Nostr profile with multiple methods:
 * - NIP-49 Encrypted Backup (ncryptsec)
 * - Mnemonic Seed Phrase (12/24 words)
 * - External Hardware Device (placeholder)
 */
#include "sheet-import-profile.h"
#include "../app-resources.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include <string.h>

struct _SheetImportProfile {
  AdwDialog parent_instance;

  /* Header buttons */
  GtkButton *btn_cancel;
  GtkButton *btn_import;

  /* Import method selection */
  GtkCheckButton *radio_nip49;
  GtkCheckButton *radio_mnemonic;
  GtkCheckButton *radio_hardware;

  /* NIP-49 input section */
  GtkBox *box_nip49;
  GtkTextView *text_ncryptsec;

  /* Mnemonic input section */
  GtkBox *box_mnemonic;
  GtkTextView *text_mnemonic;
  GtkDropDown *dropdown_word_count;

  /* Hardware section */
  GtkBox *box_hardware;

  /* Passphrase input (shared for NIP-49 and mnemonic) */
  GtkBox *box_passphrase;
  AdwPasswordEntryRow *entry_passphrase;

  /* Status widgets */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* Current selected method */
  ImportMethod current_method;

  /* Success callback wiring */
  SheetImportProfileSuccessCb on_success;
  gpointer on_success_ud;
};

G_DEFINE_TYPE(SheetImportProfile, sheet_import_profile, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_import_button_sensitivity(SheetImportProfile *self);
static void update_visible_sections(SheetImportProfile *self);

/* Set status message with optional spinner */
static void set_status(SheetImportProfile *self, const gchar *message, gboolean spinning) {
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) gtk_label_set_text(self->lbl_status, message);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, spinning);
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), TRUE);
  } else {
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, FALSE);
  }
}

/* Helper to get text from a GtkTextView */
static gchar *get_text_view_content(GtkTextView *tv) {
  if (!tv) return NULL;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(tv);
  if (!buffer) return NULL;

  GtkTextIter start, end;
  gtk_text_buffer_get_start_iter(buffer, &start);
  gtk_text_buffer_get_end_iter(buffer, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

/* Validate ncryptsec format (basic check) */
static gboolean is_valid_ncryptsec(const gchar *text) {
  if (!text || *text == '\0') return FALSE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text));
  /* ncryptsec strings start with "ncryptsec1" */
  return g_str_has_prefix(trimmed, "ncryptsec1");
}

/* Validate mnemonic (basic word count check) */
static gboolean is_valid_mnemonic(const gchar *text, int expected_words) {
  if (!text || *text == '\0') return FALSE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text));

  /* Count words */
  gchar **words = g_strsplit_set(trimmed, " \t\n", -1);
  int count = 0;
  for (int i = 0; words[i] != NULL; i++) {
    if (words[i][0] != '\0') count++;
  }
  g_strfreev(words);

  return (count == expected_words);
}

/* Get expected word count from dropdown */
static int get_expected_word_count(SheetImportProfile *self) {
  if (!self || !self->dropdown_word_count) return 12;
  guint selected = gtk_drop_down_get_selected(self->dropdown_word_count);
  return (selected == 0) ? 12 : 24;
}

/* Update which input sections are visible based on selected method */
static void update_visible_sections(SheetImportProfile *self) {
  if (!self) return;

  gboolean show_nip49 = (self->current_method == IMPORT_METHOD_NIP49);
  gboolean show_mnemonic = (self->current_method == IMPORT_METHOD_MNEMONIC);
  gboolean show_hardware = (self->current_method == IMPORT_METHOD_HARDWARE);
  gboolean show_passphrase = (self->current_method == IMPORT_METHOD_NIP49 ||
                              self->current_method == IMPORT_METHOD_MNEMONIC);

  if (self->box_nip49) gtk_widget_set_visible(GTK_WIDGET(self->box_nip49), show_nip49);
  if (self->box_mnemonic) gtk_widget_set_visible(GTK_WIDGET(self->box_mnemonic), show_mnemonic);
  if (self->box_hardware) gtk_widget_set_visible(GTK_WIDGET(self->box_hardware), show_hardware);
  if (self->box_passphrase) gtk_widget_set_visible(GTK_WIDGET(self->box_passphrase), show_passphrase);

  update_import_button_sensitivity(self);
}

/* Check if we have valid input for the current method */
static gboolean has_valid_input(SheetImportProfile *self) {
  if (!self) return FALSE;

  const gchar *passphrase = NULL;
  if (self->entry_passphrase) {
    passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  }

  switch (self->current_method) {
    case IMPORT_METHOD_NIP49: {
      g_autofree gchar *ncryptsec = get_text_view_content(self->text_ncryptsec);
      gboolean valid_ncryptsec = is_valid_ncryptsec(ncryptsec);
      gboolean has_passphrase = (passphrase && *passphrase);
      return valid_ncryptsec && has_passphrase;
    }

    case IMPORT_METHOD_MNEMONIC: {
      g_autofree gchar *mnemonic = get_text_view_content(self->text_mnemonic);
      int expected = get_expected_word_count(self);
      gboolean valid_mnemonic = is_valid_mnemonic(mnemonic, expected);
      /* Passphrase is optional for mnemonic but we need at least the mnemonic */
      return valid_mnemonic;
    }

    case IMPORT_METHOD_HARDWARE:
      /* Hardware import is a placeholder - always disabled for now */
      return FALSE;

    default:
      return FALSE;
  }
}

/* Update import button sensitivity based on input validity */
static void update_import_button_sensitivity(SheetImportProfile *self) {
  if (!self || !self->btn_import) return;
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), has_valid_input(self));
}

/* Radio button toggled handler */
static void on_radio_toggled(GtkCheckButton *btn, gpointer user_data) {
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self || !gtk_check_button_get_active(btn)) return;

  if (btn == self->radio_nip49) {
    self->current_method = IMPORT_METHOD_NIP49;
  } else if (btn == self->radio_mnemonic) {
    self->current_method = IMPORT_METHOD_MNEMONIC;
  } else if (btn == self->radio_hardware) {
    self->current_method = IMPORT_METHOD_HARDWARE;
  }

  update_visible_sections(self);
}

/* Text buffer changed handler */
static void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
  (void)buffer;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  update_import_button_sensitivity(self);
}

/* Passphrase entry changed handler */
static void on_passphrase_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  update_import_button_sensitivity(self);
}

/* Word count dropdown changed handler */
static void on_word_count_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
  (void)object;
  (void)pspec;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  update_import_button_sensitivity(self);
}

/* Cancel button handler */
static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetImportProfile *self = user_data;
  if (self) adw_dialog_close(ADW_DIALOG(self));
}

/* Context for async import operation */
typedef struct {
  SheetImportProfile *self;
  GtkWindow *parent;
  ImportMethod method;
  gchar *data;      /* ncryptsec or mnemonic */
  gchar *passphrase;
} ImportCtx;

static void import_ctx_free(ImportCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->data);
  /* Securely clear passphrase */
  if (ctx->passphrase) {
    memset(ctx->passphrase, 0, strlen(ctx->passphrase));
    g_free(ctx->passphrase);
  }
  g_free(ctx);
}

/* D-Bus call completion handler */
static void import_dbus_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  (void)src;
  ImportCtx *ctx = (ImportCtx *)user_data;
  if (!ctx || !ctx->self) {
    import_ctx_free(ctx);
    return;
  }

  SheetImportProfile *self = ctx->self;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  /* Hide status */
  set_status(self, NULL, FALSE);

  /* Re-enable buttons */
  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), TRUE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

  gboolean ok = FALSE;
  g_autofree gchar *npub = NULL;

  if (err) {
    const char *domain = g_quark_to_string(err->domain);
    g_warning("ImportProfile DBus error: [%s] code=%d msg=%s", domain ? domain : "?", err->code, err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed: %s", err->message);
    gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    g_clear_error(&err);
    import_ctx_free(ctx);
    return;
  }

  if (ret) {
    const char *npub_in = NULL;
    g_variant_get(ret, "(bs)", &ok, &npub_in);
    if (npub_in) npub = g_strdup(npub_in);
    g_variant_unref(ret);
    g_message("ImportProfile reply ok=%s npub='%s'", ok ? "true" : "false", (npub && *npub) ? npub : "(empty)");

    if (ok) {
      /* Copy npub to clipboard for convenience */
      if (npub && *npub) {
        GtkWidget *w = GTK_WIDGET(self);
        GdkDisplay *dpy = gtk_widget_get_display(w);
        if (dpy) {
          GdkClipboard *cb = gdk_display_get_clipboard(dpy);
          if (cb) gdk_clipboard_set_text(cb, npub);
        }
      }

      /* Show success message */
      GtkAlertDialog *ad = gtk_alert_dialog_new("Profile imported successfully!\n\nPublic key: %s\n(copied to clipboard)",
                                                 (npub && *npub) ? npub : "(unavailable)");
      gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);

      /* Notify via callback */
      if (self->on_success) {
        self->on_success(npub ? npub : "", ctx->method, self->on_success_ud);
      }

      adw_dialog_close(ADW_DIALOG(self));
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed.\n\nPlease check your input and try again.");
      gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);
    }
  }

  import_ctx_free(ctx);
}

/* Import button handler */
static void on_import(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self) return;

  const gchar *passphrase = NULL;
  if (self->entry_passphrase) {
    passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  }

  g_autofree gchar *data = NULL;
  const char *method_name = NULL;
  const char *dbus_method = NULL;

  switch (self->current_method) {
    case IMPORT_METHOD_NIP49:
      data = get_text_view_content(self->text_ncryptsec);
      if (data) g_strstrip(data);
      if (!is_valid_ncryptsec(data)) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid ncryptsec format.\n\nPlease enter a valid NIP-49 encrypted backup string starting with 'ncryptsec1'.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        return;
      }
      if (!passphrase || *passphrase == '\0') {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase required.\n\nPlease enter the passphrase used to encrypt this backup.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        return;
      }
      method_name = "NIP-49";
      dbus_method = "ImportNip49";
      break;

    case IMPORT_METHOD_MNEMONIC:
      data = get_text_view_content(self->text_mnemonic);
      if (data) g_strstrip(data);
      {
        int expected = get_expected_word_count(self);
        if (!is_valid_mnemonic(data, expected)) {
          GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid mnemonic.\n\nPlease enter exactly %d words.", expected);
          gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
          g_object_unref(ad);
          return;
        }
      }
      method_name = "Mnemonic";
      dbus_method = "ImportMnemonic";
      break;

    case IMPORT_METHOD_HARDWARE:
      GtkAlertDialog *ad = gtk_alert_dialog_new("Hardware device import is not yet implemented.");
      gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);
      return;

    default:
      return;
  }

  /* Disable buttons while processing */
  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  set_status(self, "Importing profile...", TRUE);

  /* Get D-Bus connection */
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    set_status(self, NULL, FALSE);
    if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), TRUE);
    if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to connect to session bus: %s", e ? e->message : "unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    if (e) g_clear_error(&e);
    return;
  }

  /* Create context for async call */
  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->method = self->current_method;
  ctx->data = g_strdup(data);
  ctx->passphrase = g_strdup(passphrase ? passphrase : "");

  g_message("Calling %s via D-Bus method %s", method_name, dbus_method);

  /* Call appropriate D-Bus method based on import method
   * Expected signature: (ss) -> data, passphrase
   * Returns: (bs) -> success, npub
   */
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         dbus_method,
                         g_variant_new("(ss)", data, passphrase ? passphrase : ""),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         30000, /* 30 second timeout for key derivation */
                         NULL,
                         import_dbus_done,
                         ctx);

  g_object_unref(bus);
}

static void sheet_import_profile_finalize(GObject *obj) {
  /* No allocated state to free currently */
  G_OBJECT_CLASS(sheet_import_profile_parent_class)->finalize(obj);
}

static void sheet_import_profile_class_init(SheetImportProfileClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_import_profile_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-import-profile.ui");

  /* Header buttons */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_import);

  /* Radio buttons */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_nip49);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_hardware);

  /* Input sections */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_nip49);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_ncryptsec);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, dropdown_word_count);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_hardware);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, entry_passphrase);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, lbl_status);
}

static void sheet_import_profile_init(SheetImportProfile *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize current method */
  self->current_method = IMPORT_METHOD_NIP49;

  /* Connect button handlers */
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_import) g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import), self);

  /* Connect radio button handlers */
  if (self->radio_nip49) g_signal_connect(self->radio_nip49, "toggled", G_CALLBACK(on_radio_toggled), self);
  if (self->radio_mnemonic) g_signal_connect(self->radio_mnemonic, "toggled", G_CALLBACK(on_radio_toggled), self);
  if (self->radio_hardware) g_signal_connect(self->radio_hardware, "toggled", G_CALLBACK(on_radio_toggled), self);

  /* Connect text buffer changed handlers */
  if (self->text_ncryptsec) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_ncryptsec);
    if (buffer) g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), self);
  }
  if (self->text_mnemonic) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_mnemonic);
    if (buffer) g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), self);
  }

  /* Connect passphrase entry handler */
  if (self->entry_passphrase) {
    g_signal_connect(self->entry_passphrase, "changed", G_CALLBACK(on_passphrase_changed), self);
  }

  /* Connect word count dropdown handler */
  if (self->dropdown_word_count) {
    g_signal_connect(self->dropdown_word_count, "notify::selected", G_CALLBACK(on_word_count_changed), self);
  }

  /* Initially disable import button */
  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);

  /* Set initial visibility */
  update_visible_sections(self);

  /* Focus the ncryptsec text view */
  if (self->text_ncryptsec) gtk_widget_grab_focus(GTK_WIDGET(self->text_ncryptsec));
}

SheetImportProfile *sheet_import_profile_new(void) {
  return g_object_new(TYPE_SHEET_IMPORT_PROFILE, NULL);
}

void sheet_import_profile_set_on_success(SheetImportProfile *self,
                                         SheetImportProfileSuccessCb cb,
                                         gpointer user_data) {
  if (!self) return;
  self->on_success = cb;
  self->on_success_ud = user_data;
}
