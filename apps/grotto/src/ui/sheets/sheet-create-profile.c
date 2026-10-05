/* sheet-create-profile.c - Create Profile dialog implementation
 *
 * Provides a UI for creating a new Nostr profile with passphrase protection.
 * Features:
 * - Display name input
 * - Passphrase input with visibility toggle (using GnSecureEntry)
 * - Confirm passphrase input (using GnSecureEntry)
 * - Recovery hint input (optional)
 * - Hardware key checkbox
 * - Passphrase strength validation
 * - Passphrase match validation
 * - Rate limiting for authentication attempts (nostrc-1g1)
 * - Secure password entry with auto-clear timeout (nostrc-6s2)
 * - Full keyboard navigation (nostrc-tz8w)
 */
#include "sheet-create-profile.h"
#include "sheet-backup.h"
#include "../app-resources.h"
#include "../widgets/gn-secure-entry.h"
#include "../../rate-limiter.h"
#include "../../keyboard-nav.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct _SheetCreateProfile {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_create;
  AdwEntryRow *entry_display_name;
  AdwEntryRow *entry_recovery_hint;
  GtkCheckButton *chk_hardware_key;

  /* Secure password entries (created programmatically) */
  GnSecureEntry *secure_passphrase;
  GnSecureEntry *secure_confirm_passphrase;
  GtkBox *box_passphrase_container;
  GtkBox *box_confirm_container;

  /* Status/feedback widgets */
  GtkLabel *lbl_passphrase_match;
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* Success callback wiring */
  SheetCreateProfileSuccessCb on_success;
  gpointer on_success_ud;
};

G_DEFINE_TYPE(SheetCreateProfile, sheet_create_profile, ADW_TYPE_DIALOG)

static void set_status(SheetCreateProfile *self, const gchar *message, gboolean spinning) {
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

static void update_validation(SheetCreateProfile *self) {
  if (!self) return;

  const gchar *display_name = gtk_editable_get_text(GTK_EDITABLE(self->entry_display_name));
  gchar *passphrase = gn_secure_entry_get_text(self->secure_passphrase);
  gchar *confirm = gn_secure_entry_get_text(self->secure_confirm_passphrase);

  gboolean has_display_name = display_name && *display_name;
  gboolean has_passphrase = passphrase && *passphrase;
  gboolean passphrases_match = g_strcmp0(passphrase, confirm) == 0;
  gboolean passphrase_long_enough = gn_secure_entry_meets_requirements(self->secure_passphrase);

  /* Update passphrase match indicator */
  if (self->lbl_passphrase_match) {
    gboolean has_confirm = confirm && *confirm;

    if (has_confirm) {
      GtkWidget *match_widget = GTK_WIDGET(self->lbl_passphrase_match);
      if (passphrases_match) {
        gtk_label_set_text(self->lbl_passphrase_match, "Passphrases match");
        gtk_widget_remove_css_class(match_widget, "error");
        gtk_widget_add_css_class(match_widget, "success");
      } else {
        gtk_label_set_text(self->lbl_passphrase_match, "Passphrases do not match");
        gtk_widget_remove_css_class(match_widget, "success");
        gtk_widget_add_css_class(match_widget, "error");
      }
      gtk_widget_set_visible(match_widget, TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_match), FALSE);
    }
  }

  /* Securely clear the retrieved passwords */
  if (passphrase) {
    gn_secure_entry_free_text(passphrase);
  }
  if (confirm) {
    gn_secure_entry_free_text(confirm);
  }

  /* Enable/disable create button */
  gboolean can_create = has_display_name && has_passphrase && passphrases_match && passphrase_long_enough;
  if (self->btn_create) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), can_create);
  }
}

static void on_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetCreateProfile *self = user_data;
  update_validation(self);
}

static void on_secure_entry_changed(GnSecureEntry *entry, gpointer user_data) {
  (void)entry;
  SheetCreateProfile *self = user_data;
  update_validation(self);
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateProfile *self = user_data;
  if (self) {
    /* Clear secure entries before closing */
    if (self->secure_passphrase)
      gn_secure_entry_clear(self->secure_passphrase);
    if (self->secure_confirm_passphrase)
      gn_secure_entry_clear(self->secure_confirm_passphrase);
    adw_dialog_close(ADW_DIALOG(self));
  }
}

/* Context for async profile creation */
typedef struct {
  SheetCreateProfile *self;
  GtkWindow *parent;
  gchar *display_name;
  gchar *passphrase;
  gchar *recovery_hint;
  gboolean use_hardware_key;
} CreateCtx;

static void create_ctx_free(CreateCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->display_name);
  /* Securely clear passphrase using our helper */
  if (ctx->passphrase) {
    gn_secure_entry_free_text(ctx->passphrase);
    ctx->passphrase = NULL;
  }
  g_free(ctx->recovery_hint);
  g_free(ctx);
}

static void create_profile_dbus_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  (void)src;
  CreateCtx *ctx = (CreateCtx *)user_data;
  if (!ctx || !ctx->self) {
    create_ctx_free(ctx);
    return;
  }

  SheetCreateProfile *self = ctx->self;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  /* Hide status */
  set_status(self, NULL, FALSE);

  /* Re-enable buttons */
  if (self->btn_create) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), TRUE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

  gboolean ok = FALSE;
  g_autofree gchar *npub = NULL;

  if (err) {
    const char *domain = g_quark_to_string(err->domain);
    g_warning("CreateProfile DBus error: [%s] code=%d msg=%s", domain ? domain : "?", err->code, err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Profile creation failed: %s", err->message);
    gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    g_clear_error(&err);
    create_ctx_free(ctx);
    return;
  }

  if (ret) {
    const char *npub_in = NULL;
    g_variant_get(ret, "(bs)", &ok, &npub_in);
    if (npub_in) npub = g_strdup(npub_in);
    g_variant_unref(ret);
    g_message("CreateProfile reply ok=%s npub='%s'", ok ? "true" : "false", (npub && *npub) ? npub : "(empty)");

    if (ok) {
      /* Clear secure entries on success */
      if (self->secure_passphrase)
        gn_secure_entry_clear(self->secure_passphrase);
      if (self->secure_confirm_passphrase)
        gn_secure_entry_clear(self->secure_confirm_passphrase);

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
      GtkAlertDialog *ad = gtk_alert_dialog_new("Profile created successfully!\n\nPublic key: %s\n(copied to clipboard)",
                                                 (npub && *npub) ? npub : "(unavailable)");
      gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);

      /* Notify via callback */
      if (self->on_success) {
        self->on_success(npub ? npub : "", ctx->display_name, ctx->use_hardware_key, self->on_success_ud);
      }

      adw_dialog_close(ADW_DIALOG(self));

      /* Trigger backup reminder for newly created key */
      if (npub && *npub && ctx->parent) {
        sheet_backup_trigger_reminder(ctx->parent, npub);
      }
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Profile creation failed.\n\nPlease try again.");
      gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);
    }
  }

  create_ctx_free(ctx);
}

static void on_create(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateProfile *self = (SheetCreateProfile *)user_data;
  if (!self) return;

  const gchar *display_name = gtk_editable_get_text(GTK_EDITABLE(self->entry_display_name));
  gchar *passphrase = gn_secure_entry_get_text(self->secure_passphrase);
  gchar *confirm = gn_secure_entry_get_text(self->secure_confirm_passphrase);
  const gchar *recovery_hint = gtk_editable_get_text(GTK_EDITABLE(self->entry_recovery_hint));
  gboolean use_hardware_key = gtk_check_button_get_active(self->chk_hardware_key);

  /* Validate inputs */
  if (!display_name || *display_name == '\0') {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Please enter a display name.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    gn_secure_entry_free_text(passphrase);
    gn_secure_entry_free_text(confirm);
    return;
  }

  if (!passphrase || strlen(passphrase) < 8) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase must be at least 8 characters.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    gn_secure_entry_free_text(passphrase);
    gn_secure_entry_free_text(confirm);
    return;
  }

  if (g_strcmp0(passphrase, confirm) != 0) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrases do not match.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    gn_secure_entry_free_text(passphrase);
    gn_secure_entry_free_text(confirm);
    return;
  }

  /* Securely clear confirm (we don't need it anymore) */
  gn_secure_entry_free_text(confirm);
  confirm = NULL;

  /* Disable buttons while processing */
  if (self->btn_create) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  set_status(self, "Creating profile...", TRUE);

  /* Get D-Bus connection */
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    set_status(self, NULL, FALSE);
    if (self->btn_create) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), TRUE);
    if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to connect to session bus: %s", e ? e->message : "unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    if (e) g_clear_error(&e);
    gn_secure_entry_free_text(passphrase);
    return;
  }

  /* Create context for async call - passphrase ownership transfers to ctx */
  CreateCtx *ctx = g_new0(CreateCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->display_name = g_strdup(display_name);
  ctx->passphrase = passphrase; /* Transfer ownership */
  ctx->recovery_hint = g_strdup(recovery_hint ? recovery_hint : "");
  ctx->use_hardware_key = use_hardware_key;

  /* Call CreateProfile D-Bus method
   * Expected signature: (ssssb) -> display_name, passphrase, recovery_hint, label, use_hardware_key
   * Returns: (bs) -> success, npub
   */
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "CreateProfile",
                         g_variant_new("(ssssb)", display_name, ctx->passphrase, recovery_hint ? recovery_hint : "", "", use_hardware_key),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         10000,
                         NULL,
                         create_profile_dbus_done,
                         ctx);

  g_object_unref(bus);
}

static void sheet_create_profile_dispose(GObject *obj) {
  SheetCreateProfile *self = SHEET_CREATE_PROFILE(obj);

  /* Clear secure entries before disposal */
  if (self->secure_passphrase) {
    gn_secure_entry_clear(self->secure_passphrase);
  }
  if (self->secure_confirm_passphrase) {
    gn_secure_entry_clear(self->secure_confirm_passphrase);
  }

  G_OBJECT_CLASS(sheet_create_profile_parent_class)->dispose(obj);
}

static void sheet_create_profile_finalize(GObject *obj) {
  G_OBJECT_CLASS(sheet_create_profile_parent_class)->finalize(obj);
}

static void sheet_create_profile_class_init(SheetCreateProfileClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->dispose = sheet_create_profile_dispose;
  oc->finalize = sheet_create_profile_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-profile.ui");

  /* Header buttons */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, btn_create);

  /* Form entries */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_display_name);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_recovery_hint);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, chk_hardware_key);

  /* Containers for secure entries */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, box_passphrase_container);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, box_confirm_container);

  /* Feedback widgets */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, lbl_passphrase_match);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, box_status);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, lbl_status);
}

static void sheet_create_profile_init(SheetCreateProfile *self) {
  /* Ensure GnSecureEntry type is registered */
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Create secure passphrase entry */
  self->secure_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_passphrase, "Enter passphrase");
  gn_secure_entry_set_min_length(self->secure_passphrase, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_passphrase, TRUE);
  gn_secure_entry_set_show_caps_warning(self->secure_passphrase, TRUE);
  gn_secure_entry_set_requirements_text(self->secure_passphrase,
    "Use at least 8 characters with mixed case, numbers, and symbols for a strong passphrase.");
  gn_secure_entry_set_timeout(self->secure_passphrase, 120); /* 2 minute timeout */

  if (self->box_passphrase_container) {
    gtk_box_append(self->box_passphrase_container, GTK_WIDGET(self->secure_passphrase));
  }

  /* Create secure confirm passphrase entry */
  self->secure_confirm_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_confirm_passphrase, "Confirm passphrase");
  gn_secure_entry_set_min_length(self->secure_confirm_passphrase, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_confirm_passphrase, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_confirm_passphrase, TRUE);
  gn_secure_entry_set_timeout(self->secure_confirm_passphrase, 120);

  if (self->box_confirm_container) {
    gtk_box_append(self->box_confirm_container, GTK_WIDGET(self->secure_confirm_passphrase));
  }

  /* Connect button handlers */
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_create) g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create), self);

  /* Connect entry change handlers for validation */
  if (self->entry_display_name)
    g_signal_connect(self->entry_display_name, "changed", G_CALLBACK(on_entry_changed), self);

  /* Connect secure entry change handlers */
  g_signal_connect(self->secure_passphrase, "changed", G_CALLBACK(on_secure_entry_changed), self);
  g_signal_connect(self->secure_confirm_passphrase, "changed", G_CALLBACK(on_secure_entry_changed), self);

  /* Initially disable create button */
  if (self->btn_create) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), FALSE);

  /* Hide feedback labels initially */
  if (self->lbl_passphrase_match) gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_match), FALSE);

  /* Setup keyboard navigation (nostrc-tz8w):
   * - Focus display name entry on dialog open
   * - Create button is default (Enter activates when form is valid) */
  gn_keyboard_nav_setup_dialog(ADW_DIALOG(self),
                                GTK_WIDGET(self->entry_display_name),
                                GTK_WIDGET(self->btn_create));
}

SheetCreateProfile *sheet_create_profile_new(void) {
  return g_object_new(TYPE_SHEET_CREATE_PROFILE, NULL);
}

void sheet_create_profile_set_on_success(SheetCreateProfile *self,
                                          SheetCreateProfileSuccessCb cb,
                                          gpointer user_data) {
  if (!self) return;
  self->on_success = cb;
  self->on_success_ud = user_data;
}
