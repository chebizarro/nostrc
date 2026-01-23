/* sheet-create-profile.c - Create Profile dialog implementation
 *
 * Provides a UI for creating a new Nostr profile with passphrase protection.
 * Features:
 * - Display name input
 * - Passphrase input with visibility toggle
 * - Confirm passphrase input
 * - Recovery hint input (optional)
 * - Hardware key checkbox
 * - Passphrase strength validation
 * - Passphrase match validation
 */
#include "sheet-create-profile.h"
#include "../app-resources.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct _SheetCreateProfile {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_create;
  AdwEntryRow *entry_display_name;
  AdwPasswordEntryRow *entry_passphrase;
  AdwPasswordEntryRow *entry_confirm_passphrase;
  AdwEntryRow *entry_recovery_hint;
  GtkCheckButton *chk_hardware_key;

  /* Status/feedback widgets */
  GtkLabel *lbl_passphrase_strength;
  GtkLabel *lbl_passphrase_match;
  GtkLevelBar *level_passphrase_strength;
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* Success callback wiring */
  SheetCreateProfileSuccessCb on_success;
  gpointer on_success_ud;
};

G_DEFINE_TYPE(SheetCreateProfile, sheet_create_profile, ADW_TYPE_DIALOG)

/* Passphrase strength levels */
typedef enum {
  PASSPHRASE_STRENGTH_WEAK = 0,
  PASSPHRASE_STRENGTH_FAIR = 1,
  PASSPHRASE_STRENGTH_GOOD = 2,
  PASSPHRASE_STRENGTH_STRONG = 3,
  PASSPHRASE_STRENGTH_VERY_STRONG = 4
} PassphraseStrength;

/* Calculate passphrase strength based on various criteria */
static PassphraseStrength calculate_passphrase_strength(const gchar *passphrase) {
  if (!passphrase || *passphrase == '\0') {
    return PASSPHRASE_STRENGTH_WEAK;
  }

  size_t len = strlen(passphrase);
  gboolean has_lower = FALSE;
  gboolean has_upper = FALSE;
  gboolean has_digit = FALSE;
  gboolean has_special = FALSE;
  int score = 0;

  /* Check character classes */
  for (size_t i = 0; i < len; i++) {
    char c = passphrase[i];
    if (c >= 'a' && c <= 'z') has_lower = TRUE;
    else if (c >= 'A' && c <= 'Z') has_upper = TRUE;
    else if (c >= '0' && c <= '9') has_digit = TRUE;
    else has_special = TRUE;
  }

  /* Score based on length */
  if (len >= 8) score++;
  if (len >= 12) score++;
  if (len >= 16) score++;
  if (len >= 20) score++;

  /* Score based on character variety */
  if (has_lower) score++;
  if (has_upper) score++;
  if (has_digit) score++;
  if (has_special) score++;

  /* Map score to strength level */
  if (score <= 2) return PASSPHRASE_STRENGTH_WEAK;
  if (score <= 4) return PASSPHRASE_STRENGTH_FAIR;
  if (score <= 6) return PASSPHRASE_STRENGTH_GOOD;
  if (score <= 7) return PASSPHRASE_STRENGTH_STRONG;
  return PASSPHRASE_STRENGTH_VERY_STRONG;
}

/* Get display string for passphrase strength */
static const gchar *get_strength_label(PassphraseStrength strength) {
  switch (strength) {
    case PASSPHRASE_STRENGTH_WEAK: return "Weak";
    case PASSPHRASE_STRENGTH_FAIR: return "Fair";
    case PASSPHRASE_STRENGTH_GOOD: return "Good";
    case PASSPHRASE_STRENGTH_STRONG: return "Strong";
    case PASSPHRASE_STRENGTH_VERY_STRONG: return "Very Strong";
    default: return "";
  }
}

/* Get CSS class for passphrase strength */
static const gchar *get_strength_class(PassphraseStrength strength) {
  switch (strength) {
    case PASSPHRASE_STRENGTH_WEAK: return "error";
    case PASSPHRASE_STRENGTH_FAIR: return "warning";
    case PASSPHRASE_STRENGTH_GOOD: return "accent";
    case PASSPHRASE_STRENGTH_STRONG: return "success";
    case PASSPHRASE_STRENGTH_VERY_STRONG: return "success";
    default: return "";
  }
}

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
  const gchar *passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  const gchar *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_confirm_passphrase));

  gboolean has_display_name = display_name && *display_name;
  gboolean has_passphrase = passphrase && *passphrase;
  gboolean passphrases_match = g_strcmp0(passphrase, confirm) == 0;
  gboolean passphrase_long_enough = strlen(passphrase ? passphrase : "") >= 8;

  /* Update passphrase strength indicator */
  PassphraseStrength strength = calculate_passphrase_strength(passphrase);
  if (self->lbl_passphrase_strength) {
    if (has_passphrase) {
      gtk_label_set_text(self->lbl_passphrase_strength, get_strength_label(strength));
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_strength), TRUE);

      /* Update CSS class for color coding */
      GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(self->lbl_passphrase_strength));
      gtk_style_context_remove_class(ctx, "error");
      gtk_style_context_remove_class(ctx, "warning");
      gtk_style_context_remove_class(ctx, "accent");
      gtk_style_context_remove_class(ctx, "success");
      gtk_style_context_add_class(ctx, get_strength_class(strength));
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_strength), FALSE);
    }
  }

  /* Update level bar */
  if (self->level_passphrase_strength) {
    gtk_level_bar_set_value(self->level_passphrase_strength, (gdouble)strength);
    gtk_widget_set_visible(GTK_WIDGET(self->level_passphrase_strength), has_passphrase);
  }

  /* Update passphrase match indicator */
  if (self->lbl_passphrase_match) {
    const gchar *confirm_text = gtk_editable_get_text(GTK_EDITABLE(self->entry_confirm_passphrase));
    gboolean has_confirm = confirm_text && *confirm_text;

    if (has_confirm) {
      if (passphrases_match) {
        gtk_label_set_text(self->lbl_passphrase_match, "Passphrases match");
        GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(self->lbl_passphrase_match));
        gtk_style_context_remove_class(ctx, "error");
        gtk_style_context_add_class(ctx, "success");
      } else {
        gtk_label_set_text(self->lbl_passphrase_match, "Passphrases do not match");
        GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(self->lbl_passphrase_match));
        gtk_style_context_remove_class(ctx, "success");
        gtk_style_context_add_class(ctx, "error");
      }
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_match), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_match), FALSE);
    }
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

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateProfile *self = user_data;
  if (self) adw_dialog_close(ADW_DIALOG(self));
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
  /* Securely clear passphrase */
  if (ctx->passphrase) {
    memset(ctx->passphrase, 0, strlen(ctx->passphrase));
    g_free(ctx->passphrase);
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
  const gchar *passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  const gchar *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_confirm_passphrase));
  const gchar *recovery_hint = gtk_editable_get_text(GTK_EDITABLE(self->entry_recovery_hint));
  gboolean use_hardware_key = gtk_check_button_get_active(self->chk_hardware_key);

  /* Validate inputs */
  if (!display_name || *display_name == '\0') {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Please enter a display name.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  if (!passphrase || strlen(passphrase) < 8) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase must be at least 8 characters.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  if (g_strcmp0(passphrase, confirm) != 0) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrases do not match.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

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
    return;
  }

  /* Create context for async call */
  CreateCtx *ctx = g_new0(CreateCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->display_name = g_strdup(display_name);
  ctx->passphrase = g_strdup(passphrase);
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
                         g_variant_new("(ssssb)", display_name, passphrase, recovery_hint ? recovery_hint : "", "", use_hardware_key),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         10000,
                         NULL,
                         create_profile_dbus_done,
                         ctx);

  g_object_unref(bus);
}

static void sheet_create_profile_finalize(GObject *obj) {
  /* No allocated state to free currently */
  G_OBJECT_CLASS(sheet_create_profile_parent_class)->finalize(obj);
}

static void sheet_create_profile_class_init(SheetCreateProfileClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_create_profile_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-profile.ui");

  /* Header buttons */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, btn_create);

  /* Form entries */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_display_name);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_confirm_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, entry_recovery_hint);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, chk_hardware_key);

  /* Feedback widgets */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, lbl_passphrase_strength);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, lbl_passphrase_match);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, level_passphrase_strength);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, box_status);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetCreateProfile, lbl_status);
}

static void sheet_create_profile_init(SheetCreateProfile *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect button handlers */
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_create) g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create), self);

  /* Connect entry change handlers for validation */
  if (self->entry_display_name)
    g_signal_connect(self->entry_display_name, "changed", G_CALLBACK(on_entry_changed), self);
  if (self->entry_passphrase)
    g_signal_connect(self->entry_passphrase, "changed", G_CALLBACK(on_entry_changed), self);
  if (self->entry_confirm_passphrase)
    g_signal_connect(self->entry_confirm_passphrase, "changed", G_CALLBACK(on_entry_changed), self);

  /* Initially disable create button */
  if (self->btn_create) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_create), FALSE);

  /* Hide feedback labels initially */
  if (self->lbl_passphrase_strength) gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_strength), FALSE);
  if (self->lbl_passphrase_match) gtk_widget_set_visible(GTK_WIDGET(self->lbl_passphrase_match), FALSE);
  if (self->level_passphrase_strength) gtk_widget_set_visible(GTK_WIDGET(self->level_passphrase_strength), FALSE);

  /* Focus display name entry */
  if (self->entry_display_name) gtk_widget_grab_focus(GTK_WIDGET(self->entry_display_name));
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
