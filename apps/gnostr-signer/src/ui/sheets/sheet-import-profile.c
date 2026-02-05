/* sheet-import-profile.c - Import Profile dialog implementation
 *
 * Provides a UI for importing an existing Nostr profile with multiple methods:
 * - NIP-49 Encrypted Backup (ncryptsec)
 * - Mnemonic Seed Phrase (12/24 words)
 * - Raw nsec Private Key
 *
 * Includes rate limiting for authentication attempts (nostrc-1g1).
 * Uses secure memory for sensitive data (passphrases, keys) (nostrc-6s2).
 */
#include "sheet-import-profile.h"
#include "../app-resources.h"
#include "../widgets/gn-secure-entry.h"
#include "../../rate-limiter.h"
#include "../../secure-memory.h"
#include "../../keyboard-nav.h"

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
  GtkCheckButton *radio_nsec;

  /* NIP-49 input section */
  GtkBox *box_nip49;
  GtkTextView *text_ncryptsec;

  /* Mnemonic input section */
  GtkBox *box_mnemonic;
  GtkTextView *text_mnemonic;

  /* nsec input section */
  GtkBox *box_nsec;
  GtkBox *box_nsec_container;
  GnSecureEntry *secure_nsec;

  /* Passphrase input (for NIP-49) */
  GtkBox *box_passphrase;
  GtkBox *box_passphrase_container;
  GnSecureEntry *secure_passphrase;

  /* Status widgets */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* Current selected method */
  ImportMethod current_method;

  /* Success callback wiring */
  SheetImportProfileSuccessCb on_success;
  gpointer on_success_ud;

  /* Rate limiting */
  GnRateLimiter *rate_limiter;
  GtkLabel *lbl_lockout;
  guint lockout_timer_id;
  gulong rate_limit_handler_id;
  gulong lockout_expired_handler_id;
};

G_DEFINE_TYPE(SheetImportProfile, sheet_import_profile, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_import_button_sensitivity(SheetImportProfile *self);
static void update_visible_sections(SheetImportProfile *self);
static void update_lockout_ui(SheetImportProfile *self);
static gboolean update_lockout_countdown(gpointer user_data);

/* Set status message with optional spinner */
static void set_status(SheetImportProfile *self, const gchar *message, gboolean spinning) {
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) {
      gtk_label_set_text(self->lbl_status, message);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_status),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL, message,
                                     -1);
    }
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, spinning);
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), TRUE);
  } else {
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, FALSE);
  }
}

/* Update lockout UI to show remaining time */
static void update_lockout_ui(SheetImportProfile *self) {
  if (!self) return;

  guint remaining = gn_rate_limiter_get_remaining_lockout(self->rate_limiter);

  if (remaining > 0) {
    g_autofree gchar *msg = g_strdup_printf("Too many attempts. Please wait %u seconds.", remaining);
    set_status(self, msg, FALSE);
    if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);
  } else {
    set_status(self, NULL, FALSE);
    update_import_button_sensitivity(self);
  }
}

/* Timer callback to update lockout countdown */
static gboolean update_lockout_countdown(gpointer user_data) {
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self) return G_SOURCE_REMOVE;

  guint remaining = gn_rate_limiter_get_remaining_lockout(self->rate_limiter);

  if (remaining > 0) {
    update_lockout_ui(self);
    return G_SOURCE_CONTINUE;
  }

  self->lockout_timer_id = 0;
  update_lockout_ui(self);
  return G_SOURCE_REMOVE;
}

/* Handler for rate limit exceeded signal */
static void on_rate_limit_exceeded(GnRateLimiter *limiter, guint lockout_seconds, gpointer user_data) {
  (void)limiter;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self) return;

  g_message("Rate limit exceeded: locked out for %u seconds", lockout_seconds);

  if (self->lockout_timer_id == 0) {
    self->lockout_timer_id = g_timeout_add_seconds(1, update_lockout_countdown, self);
  }

  update_lockout_ui(self);
}

/* Handler for lockout expired signal */
static void on_lockout_expired(GnRateLimiter *limiter, gpointer user_data) {
  (void)limiter;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self) return;

  g_message("Rate limit lockout expired");
  update_lockout_ui(self);
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
  return g_str_has_prefix(trimmed, "ncryptsec1");
}

/* Validate mnemonic (basic word count check) */
static gboolean is_valid_mnemonic(const gchar *text) {
  if (!text || *text == '\0') return FALSE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text));

  gchar **words = g_strsplit_set(trimmed, " \t\n", -1);
  int count = 0;
  for (int i = 0; words[i] != NULL; i++) {
    if (words[i][0] != '\0') count++;
  }
  g_strfreev(words);

  return (count == 12 || count == 24);
}

/* Validate nsec format */
static gboolean is_valid_nsec(const gchar *text) {
  if (!text || *text == '\0') return FALSE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text));
  return g_str_has_prefix(trimmed, "nsec1");
}

/* Update which input sections are visible based on selected method */
static void update_visible_sections(SheetImportProfile *self) {
  if (!self) return;

  gboolean show_nip49 = (self->current_method == IMPORT_METHOD_NIP49);
  gboolean show_mnemonic = (self->current_method == IMPORT_METHOD_MNEMONIC);
  gboolean show_nsec = (self->current_method == IMPORT_METHOD_NSEC);
  gboolean show_passphrase = (self->current_method == IMPORT_METHOD_NIP49);

  if (self->box_nip49) gtk_widget_set_visible(GTK_WIDGET(self->box_nip49), show_nip49);
  if (self->box_mnemonic) gtk_widget_set_visible(GTK_WIDGET(self->box_mnemonic), show_mnemonic);
  if (self->box_nsec) gtk_widget_set_visible(GTK_WIDGET(self->box_nsec), show_nsec);
  if (self->box_passphrase) gtk_widget_set_visible(GTK_WIDGET(self->box_passphrase), show_passphrase);

  update_import_button_sensitivity(self);
}

/* Check if we have valid input for the current method */
static gboolean has_valid_input(SheetImportProfile *self) {
  if (!self) return FALSE;

  if (self->rate_limiter && !gn_rate_limiter_check_allowed(self->rate_limiter)) {
    return FALSE;
  }

  switch (self->current_method) {
    case IMPORT_METHOD_NIP49: {
      g_autofree gchar *ncryptsec = get_text_view_content(self->text_ncryptsec);
      gchar *passphrase = self->secure_passphrase ? gn_secure_entry_get_text(self->secure_passphrase) : NULL;
      gboolean valid = is_valid_ncryptsec(ncryptsec) && (passphrase && *passphrase);
      if (passphrase) gn_secure_entry_free_text(passphrase);
      return valid;
    }

    case IMPORT_METHOD_MNEMONIC: {
      g_autofree gchar *mnemonic = get_text_view_content(self->text_mnemonic);
      return is_valid_mnemonic(mnemonic);
    }

    case IMPORT_METHOD_NSEC: {
      gchar *nsec = self->secure_nsec ? gn_secure_entry_get_text(self->secure_nsec) : NULL;
      gboolean valid = is_valid_nsec(nsec);
      if (nsec) gn_secure_entry_free_text(nsec);
      return valid;
    }

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
  } else if (btn == self->radio_nsec) {
    self->current_method = IMPORT_METHOD_NSEC;
  }

  update_visible_sections(self);
}

/* Text buffer changed handler */
static void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
  (void)buffer;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  update_import_button_sensitivity(self);
}

/* Secure entry changed handler */
static void on_secure_entry_changed(GnSecureEntry *entry, gpointer user_data) {
  (void)entry;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  update_import_button_sensitivity(self);
}

/* Cancel button handler */
static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetImportProfile *self = user_data;
  if (self) {
    if (self->secure_passphrase) gn_secure_entry_clear(self->secure_passphrase);
    if (self->secure_nsec) gn_secure_entry_clear(self->secure_nsec);
    adw_dialog_close(ADW_DIALOG(self));
  }
}

/* Context for async import operation */
typedef struct {
  SheetImportProfile *self;
  GtkWindow *parent;
  ImportMethod method;
  gchar *data;
  gchar *passphrase;
} ImportCtx;

static void import_ctx_free(ImportCtx *ctx) {
  if (!ctx) return;
  gn_secure_clear_string(ctx->data);
  if (ctx->passphrase) {
    gn_secure_entry_free_text(ctx->passphrase);
    ctx->passphrase = NULL;
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

  set_status(self, NULL, FALSE);

  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), TRUE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

  gboolean ok = FALSE;
  g_autofree gchar *npub = NULL;

  if (err) {
    g_warning("ImportProfile DBus error: %s", err->message);
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

    if (ok) {
      if (self->rate_limiter) {
        gn_rate_limiter_record_attempt(self->rate_limiter, TRUE);
      }

      if (self->secure_passphrase) gn_secure_entry_clear(self->secure_passphrase);
      if (self->secure_nsec) gn_secure_entry_clear(self->secure_nsec);

      if (npub && *npub) {
        GtkWidget *w = GTK_WIDGET(self);
        GdkDisplay *dpy = gtk_widget_get_display(w);
        if (dpy) {
          GdkClipboard *cb = gdk_display_get_clipboard(dpy);
          if (cb) gdk_clipboard_set_text(cb, npub);
        }
      }

      GtkAlertDialog *ad = gtk_alert_dialog_new("Profile imported successfully!\n\nPublic key: %s\n(copied to clipboard)",
                                                 (npub && *npub) ? npub : "(unavailable)");
      gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);

      if (self->on_success) {
        self->on_success(npub ? npub : "", ctx->method, self->on_success_ud);
      }

      adw_dialog_close(ADW_DIALOG(self));
    } else {
      if (self->rate_limiter) {
        gn_rate_limiter_record_attempt(self->rate_limiter, FALSE);
        update_lockout_ui(self);
      }

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

  if (self->rate_limiter && !gn_rate_limiter_check_allowed(self->rate_limiter)) {
    guint remaining = gn_rate_limiter_get_remaining_lockout(self->rate_limiter);
    g_autofree gchar *msg = g_strdup_printf("Too many attempts.\n\nPlease wait %u seconds.", remaining);
    GtkAlertDialog *ad = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  gchar *passphrase = NULL;
  if (self->secure_passphrase) {
    passphrase = gn_secure_entry_get_text(self->secure_passphrase);
  }

  g_autofree gchar *data = NULL;
  const char *dbus_method = NULL;

  switch (self->current_method) {
    case IMPORT_METHOD_NIP49:
      data = get_text_view_content(self->text_ncryptsec);
      if (data) g_strstrip(data);
      if (!is_valid_ncryptsec(data)) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid ncryptsec format.\n\nPlease enter a valid backup starting with 'ncryptsec1'.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
        return;
      }
      if (!passphrase || *passphrase == '\0') {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase required.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
        return;
      }
      dbus_method = "ImportNip49";
      break;

    case IMPORT_METHOD_MNEMONIC:
      data = get_text_view_content(self->text_mnemonic);
      if (data) g_strstrip(data);
      if (!is_valid_mnemonic(data)) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid mnemonic.\n\nPlease enter 12 or 24 words.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
        return;
      }
      dbus_method = "ImportMnemonic";
      break;

    case IMPORT_METHOD_NSEC: {
      gchar *nsec = self->secure_nsec ? gn_secure_entry_get_text(self->secure_nsec) : NULL;
      if (!is_valid_nsec(nsec)) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid nsec format.\n\nPlease enter a valid key starting with 'nsec1'.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
        if (nsec) gn_secure_entry_free_text(nsec);
        return;
      }
      data = g_strdup(nsec);
      if (nsec) gn_secure_entry_free_text(nsec);
      dbus_method = "ImportNsec";
      break;
    }

    default:
      gn_secure_entry_free_text(passphrase);
      return;
  }

  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  set_status(self, "Importing profile...", TRUE);

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
    gn_secure_entry_free_text(passphrase);
    return;
  }

  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->method = self->current_method;
  ctx->data = g_strdup(data);
  ctx->passphrase = passphrase;

  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         dbus_method,
                         g_variant_new("(ss)", data, ctx->passphrase ? ctx->passphrase : ""),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         30000,
                         NULL,
                         import_dbus_done,
                         ctx);

  g_object_unref(bus);
}

static void sheet_import_profile_dispose(GObject *obj) {
  SheetImportProfile *self = (SheetImportProfile *)obj;

  if (self->secure_passphrase) {
    gn_secure_entry_clear(self->secure_passphrase);
  }
  if (self->secure_nsec) {
    gn_secure_entry_clear(self->secure_nsec);
  }

  G_OBJECT_CLASS(sheet_import_profile_parent_class)->dispose(obj);
}

static void sheet_import_profile_finalize(GObject *obj) {
  SheetImportProfile *self = (SheetImportProfile *)obj;

  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  if (self->rate_limiter) {
    if (self->rate_limit_handler_id > 0) {
      g_signal_handler_disconnect(self->rate_limiter, self->rate_limit_handler_id);
    }
    if (self->lockout_expired_handler_id > 0) {
      g_signal_handler_disconnect(self->rate_limiter, self->lockout_expired_handler_id);
    }
  }

  G_OBJECT_CLASS(sheet_import_profile_parent_class)->finalize(obj);
}

static void sheet_import_profile_class_init(SheetImportProfileClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->dispose = sheet_import_profile_dispose;
  oc->finalize = sheet_import_profile_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-import-profile.ui");

  /* Header buttons */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_import);

  /* Radio buttons */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_nip49);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_nsec);

  /* Input sections */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_nip49);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_ncryptsec);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_nsec);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_nsec_container);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_passphrase_container);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, lbl_status);
}

static void sheet_import_profile_init(SheetImportProfile *self) {
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  gtk_widget_init_template(GTK_WIDGET(self));

  self->current_method = IMPORT_METHOD_NIP49;

  /* Initialize rate limiter */
  self->rate_limiter = gn_rate_limiter_get_default();
  self->rate_limit_handler_id = g_signal_connect(self->rate_limiter,
                                                   "rate-limit-exceeded",
                                                   G_CALLBACK(on_rate_limit_exceeded),
                                                   self);
  self->lockout_expired_handler_id = g_signal_connect(self->rate_limiter,
                                                        "lockout-expired",
                                                        G_CALLBACK(on_lockout_expired),
                                                        self);

  /* Create secure passphrase entry */
  self->secure_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_passphrase, "Enter passphrase");
  gn_secure_entry_set_show_strength_indicator(self->secure_passphrase, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_passphrase, TRUE);
  gn_secure_entry_set_timeout(self->secure_passphrase, 120);

  if (self->box_passphrase_container) {
    gtk_box_append(self->box_passphrase_container, GTK_WIDGET(self->secure_passphrase));
  }

  /* Create secure nsec entry */
  self->secure_nsec = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_nsec, "nsec1...");
  gn_secure_entry_set_show_strength_indicator(self->secure_nsec, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_nsec, FALSE);
  gn_secure_entry_set_timeout(self->secure_nsec, 120);

  if (self->box_nsec_container) {
    gtk_box_append(self->box_nsec_container, GTK_WIDGET(self->secure_nsec));
  }

  /* Connect button handlers */
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_import) g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import), self);

  /* Connect radio button handlers */
  if (self->radio_nip49) g_signal_connect(self->radio_nip49, "toggled", G_CALLBACK(on_radio_toggled), self);
  if (self->radio_mnemonic) g_signal_connect(self->radio_mnemonic, "toggled", G_CALLBACK(on_radio_toggled), self);
  if (self->radio_nsec) g_signal_connect(self->radio_nsec, "toggled", G_CALLBACK(on_radio_toggled), self);

  /* Connect text buffer changed handlers */
  if (self->text_ncryptsec) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_ncryptsec);
    if (buffer) g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), self);
  }
  if (self->text_mnemonic) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_mnemonic);
    if (buffer) g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), self);
  }

  /* Connect secure entry handlers */
  g_signal_connect(self->secure_passphrase, "changed", G_CALLBACK(on_secure_entry_changed), self);
  g_signal_connect(self->secure_nsec, "changed", G_CALLBACK(on_secure_entry_changed), self);

  /* Initially disable import button */
  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);

  /* Check if already locked out */
  if (gn_rate_limiter_is_locked_out(self->rate_limiter)) {
    self->lockout_timer_id = g_timeout_add_seconds(1, update_lockout_countdown, self);
    update_lockout_ui(self);
  }

  /* Set initial visibility */
  update_visible_sections(self);

  /* Setup keyboard navigation */
  gn_keyboard_nav_setup_dialog(ADW_DIALOG(self),
                                GTK_WIDGET(self->text_ncryptsec),
                                GTK_WIDGET(self->btn_import));
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
