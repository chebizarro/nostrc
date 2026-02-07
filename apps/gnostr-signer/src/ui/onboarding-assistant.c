/**
 * onboarding-assistant.c - Multi-step onboarding wizard implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "onboarding-assistant.h"
#include "app-resources.h"
#include "../accounts_store.h"
#include "../secret_store.h"
#include "../backup-recovery.h"
#include "../secure-mem.h"
#include "widgets/gn-secure-entry.h"
#include <gio/gio.h>
#include <string.h>
#include <math.h>

/* GSettings keys */
#define SIGNER_GSETTINGS_ID "org.gnostr.Signer"
#define ONBOARDING_COMPLETED_KEY "onboarding-completed"

/* Onboarding step indices */
typedef enum {
  STEP_WELCOME = 0,
  STEP_SECURITY,
  STEP_CHOOSE_PATH,
  STEP_CREATE_PASSPHRASE,
  STEP_IMPORT_METHOD,
  STEP_SEED_PHRASE,      /* New: Display generated seed phrase */
  STEP_BACKUP_REMINDER,
  STEP_READY,
  STEP_COUNT
} OnboardingStep;

/* User's chosen path */
typedef enum {
  PATH_NONE = 0,
  PATH_CREATE,
  PATH_IMPORT
} OnboardingPath;

struct _OnboardingAssistant {
  AdwWindow parent_instance;

  /* Template children */
  AdwCarousel *carousel;
  AdwCarouselIndicatorDots *carousel_dots;
  GtkButton *btn_back;
  GtkButton *btn_next;
  GtkButton *btn_skip;

  /* Step pages */
  GtkWidget *page_welcome;
  GtkWidget *page_security;
  GtkWidget *page_choose_path;
  GtkWidget *page_create_passphrase;
  GtkWidget *page_import_method;
  GtkWidget *page_seed_phrase;
  GtkWidget *page_backup_reminder;
  GtkWidget *page_ready;

  /* Create profile widgets */
  AdwEntryRow *entry_profile_name;
  GtkBox *box_passphrase_container;
  GtkBox *box_confirm_container;
  GnSecureEntry *secure_passphrase;
  GnSecureEntry *secure_passphrase_confirm;
  GtkLevelBar *passphrase_strength;
  GtkLabel *passphrase_hint;
  GtkLabel *passphrase_match_label;

  /* Legacy passphrase entries (for template binding) */
  GtkPasswordEntry *entry_passphrase;
  GtkPasswordEntry *entry_passphrase_confirm;

  /* Path selection buttons */
  GtkCheckButton *radio_create;
  GtkCheckButton *radio_import;

  /* Import method widgets */
  GtkCheckButton *radio_import_nsec;
  GtkCheckButton *radio_import_seed;
  GtkCheckButton *radio_import_file;

  /* Import input widgets */
  GtkTextView *text_import_data;
  GtkBox *box_import_passphrase_container;
  GnSecureEntry *secure_import_passphrase;
  GtkDropDown *dropdown_word_count;

  /* Backup checkbox */
  GtkCheckButton *backup_understood;

  /* Seed phrase display widgets */
  GtkFlowBox *seed_phrase_grid;
  GtkButton *btn_copy_seed;
  GtkCheckButton *seed_written_down;

  /* Status widgets */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* State */
  OnboardingPath chosen_path;
  OnboardingStep current_step;
  gboolean profile_created;
  gchar *created_npub;
  gchar *generated_mnemonic;  /* BIP-39 seed phrase for backup */
  gchar *generated_nsec;      /* Private key derived from mnemonic */

  /* Callback */
  OnboardingAssistantFinishedCb on_finished;
  gpointer on_finished_data;
};

G_DEFINE_TYPE(OnboardingAssistant, onboarding_assistant, ADW_TYPE_WINDOW)

/* Forward declarations */
static void update_navigation_buttons(OnboardingAssistant *self);
static void go_to_step(OnboardingAssistant *self, OnboardingStep step);
static void update_passphrase_strength(OnboardingAssistant *self);
static void set_status(OnboardingAssistant *self, const gchar *message, gboolean spinning);
static gboolean perform_profile_creation(OnboardingAssistant *self);
static gboolean perform_profile_import(OnboardingAssistant *self);
static void populate_seed_phrase_grid(OnboardingAssistant *self);
static void clear_seed_phrase_data(OnboardingAssistant *self);
static gboolean restore_copy_button_cb(gpointer user_data);
static gboolean generate_seed_phrase_and_key(OnboardingAssistant *self);

/* Helper to get GSettings if schema is available */
static GSettings *get_signer_settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(source, SIGNER_GSETTINGS_ID, TRUE);
  if (!schema) {
    g_debug("GSettings schema %s not found", SIGNER_GSETTINGS_ID);
    return NULL;
  }
  g_settings_schema_unref(schema);
  return g_settings_new(SIGNER_GSETTINGS_ID);
}

/* Calculate passphrase strength (0.0-1.0) */
static gdouble calculate_passphrase_strength(const char *passphrase) {
  if (!passphrase || !*passphrase) return 0.0;

  size_t len = strlen(passphrase);
  gdouble score = 0.0;

  /* Length scoring */
  if (len >= 8) score += 0.2;
  if (len >= 12) score += 0.1;
  if (len >= 16) score += 0.1;
  if (len >= 20) score += 0.1;

  /* Character variety */
  gboolean has_lower = FALSE, has_upper = FALSE, has_digit = FALSE, has_special = FALSE;
  for (size_t i = 0; i < len; i++) {
    char c = passphrase[i];
    if (g_ascii_islower(c)) has_lower = TRUE;
    else if (g_ascii_isupper(c)) has_upper = TRUE;
    else if (g_ascii_isdigit(c)) has_digit = TRUE;
    else has_special = TRUE;
  }

  if (has_lower) score += 0.1;
  if (has_upper) score += 0.1;
  if (has_digit) score += 0.1;
  if (has_special) score += 0.2;

  return fmin(score, 1.0);
}

/* Get strength hint text */
static const char *get_strength_hint(gdouble strength) {
  if (strength < 0.2) return "Very weak - use a longer passphrase";
  if (strength < 0.4) return "Weak - add numbers or symbols";
  if (strength < 0.6) return "Fair - consider making it longer";
  if (strength < 0.8) return "Good - getting stronger";
  return "Strong - excellent passphrase!";
}

/* Set status message with optional spinner */
static void set_status(OnboardingAssistant *self, const gchar *message, gboolean spinning) {
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) {
      gtk_label_set_text(self->lbl_status, message);
      /* Announce status change to screen readers via live region */
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

/* Securely clear generated seed phrase data */
static void clear_seed_phrase_data(OnboardingAssistant *self) {
  if (!self) return;

  if (self->generated_mnemonic) {
    gnostr_secure_clear(self->generated_mnemonic, strlen(self->generated_mnemonic));
    g_free(self->generated_mnemonic);
    self->generated_mnemonic = NULL;
  }

  if (self->generated_nsec) {
    gnostr_secure_clear(self->generated_nsec, strlen(self->generated_nsec));
    g_free(self->generated_nsec);
    self->generated_nsec = NULL;
  }
}

/* Create a widget for a single seed word with its index */
static GtkWidget *create_seed_word_widget(int index, const gchar *word) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "seed-word-box");

  /* Index label */
  gchar *idx_text = g_strdup_printf("%d.", index);
  GtkWidget *idx_label = gtk_label_new(idx_text);
  g_free(idx_text);
  gtk_widget_add_css_class(idx_label, "dim-label");
  gtk_widget_add_css_class(idx_label, "caption");
  gtk_widget_set_size_request(idx_label, 24, -1);
  gtk_label_set_xalign(GTK_LABEL(idx_label), 1.0);
  gtk_box_append(GTK_BOX(box), idx_label);

  /* Word label */
  GtkWidget *word_label = gtk_label_new(word);
  gtk_widget_add_css_class(word_label, "monospace");
  gtk_label_set_xalign(GTK_LABEL(word_label), 0.0);
  gtk_widget_set_hexpand(word_label, TRUE);
  gtk_box_append(GTK_BOX(box), word_label);

  return box;
}

/* Populate the seed phrase grid with words */
static void populate_seed_phrase_grid(OnboardingAssistant *self) {
  if (!self || !self->seed_phrase_grid || !self->generated_mnemonic) return;

  /* Clear existing children */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->seed_phrase_grid))) != NULL) {
    gtk_flow_box_remove(self->seed_phrase_grid, child);
  }

  /* Split mnemonic into words */
  gchar **words = g_strsplit(self->generated_mnemonic, " ", -1);
  if (!words) return;

  int index = 1;
  for (gchar **w = words; *w; w++, index++) {
    if (**w) {  /* Skip empty strings */
      GtkWidget *word_widget = create_seed_word_widget(index, *w);
      gtk_flow_box_append(self->seed_phrase_grid, word_widget);
    }
  }

  g_strfreev(words);
}

/* Copy seed phrase to clipboard with timeout for auto-clear */
static gboolean clear_clipboard_timeout(gpointer user_data) {
  GdkDisplay *display = GDK_DISPLAY(user_data);
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (clipboard) {
      /* Set empty content to clear the seed phrase */
      gdk_clipboard_set_text(clipboard, "");
      g_debug("Clipboard auto-cleared after timeout");
    }
  }
  return G_SOURCE_REMOVE;
}

static void on_copy_seed_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  if (!self || !self->generated_mnemonic) return;

  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
  if (!display) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(display);
  if (clipboard) {
    gdk_clipboard_set_text(clipboard, self->generated_mnemonic);

    /* Show feedback */
    if (self->btn_copy_seed) {
      /* Temporarily change button label to show success */
      GtkWidget *original_child = gtk_button_get_child(self->btn_copy_seed);
      if (original_child) {
        gtk_widget_set_visible(original_child, FALSE);
      }
      gtk_button_set_label(self->btn_copy_seed, "Copied! (Will clear in 60s)");

      /* Schedule clipboard clear after 60 seconds */
      g_timeout_add_seconds(60, clear_clipboard_timeout, display);

      /* Restore button after 3 seconds */
      g_timeout_add_seconds(3, (GSourceFunc)restore_copy_button_cb, self);
    }

    g_debug("Seed phrase copied to clipboard");
  }
}

/* Lambda-like function to restore copy button */
static gboolean restore_copy_button_cb(gpointer user_data) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  if (self && self->btn_copy_seed) {
    gtk_button_set_label(self->btn_copy_seed, NULL);
    GtkWidget *child = gtk_button_get_child(self->btn_copy_seed);
    if (child) {
      gtk_widget_set_visible(child, TRUE);
    }
  }
  return G_SOURCE_REMOVE;
}

/* Handle seed_written_down checkbox toggle */
static void on_seed_written_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

/* Generate a BIP-39 mnemonic and derive the private key */
static gboolean generate_seed_phrase_and_key(OnboardingAssistant *self) {
  if (!self) return FALSE;

  /* Clear any existing seed phrase data */
  clear_seed_phrase_data(self);

  GError *error = NULL;
  gchar *mnemonic = NULL;
  gchar *nsec = NULL;

  /* Generate 12-word mnemonic (standard for most wallets) */
  if (!gn_backup_generate_mnemonic(12, NULL, &mnemonic, &nsec, &error)) {
    g_warning("Failed to generate mnemonic: %s", error ? error->message : "unknown");
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to generate recovery phrase: %s",
                                               error ? error->message : "Unknown error");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_clear_error(&error);
    return FALSE;
  }

  /* Store in secure memory */
  self->generated_mnemonic = g_strdup(mnemonic);
  self->generated_nsec = g_strdup(nsec);

  /* Securely clear the temporary buffers */
  if (mnemonic) {
    gnostr_secure_clear(mnemonic, strlen(mnemonic));
    g_free(mnemonic);
  }
  if (nsec) {
    gnostr_secure_clear(nsec, strlen(nsec));
    g_free(nsec);
  }

  g_debug("Generated seed phrase successfully");
  return TRUE;
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

/* Context for async profile creation */
typedef struct {
  OnboardingAssistant *self;
  gchar *display_name;
  gchar *passphrase;
} CreateProfileCtx;

static void create_profile_ctx_free(CreateProfileCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->display_name);
  if (ctx->passphrase) {
    /* Securely clear passphrase */
    memset(ctx->passphrase, 0, strlen(ctx->passphrase));
    g_free(ctx->passphrase);
  }
  g_free(ctx);
}

/* D-Bus profile creation callback */
static void on_create_profile_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  (void)src;
  CreateProfileCtx *ctx = (CreateProfileCtx *)user_data;
  if (!ctx || !ctx->self) {
    create_profile_ctx_free(ctx);
    return;
  }

  OnboardingAssistant *self = ctx->self;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  set_status(self, NULL, FALSE);

  /* Re-enable navigation */
  if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), TRUE);
  if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), TRUE);

  gboolean ok = FALSE;
  const char *npub_in = NULL;

  if (err) {
    g_warning("CreateProfile D-Bus error: %s", err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Profile creation failed: %s", err->message);
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_clear_error(&err);
    create_profile_ctx_free(ctx);
    return;
  }

  if (ret) {
    g_variant_get(ret, "(bs)", &ok, &npub_in);

    if (ok && npub_in && *npub_in) {
      self->profile_created = TRUE;
      g_free(self->created_npub);
      self->created_npub = g_strdup(npub_in);
      g_debug("Profile created successfully: %s", npub_in);

      /* Clear secure entries on success */
      if (self->secure_passphrase)
        gn_secure_entry_clear(self->secure_passphrase);
      if (self->secure_passphrase_confirm)
        gn_secure_entry_clear(self->secure_passphrase_confirm);

      /* Proceed to backup reminder */
      go_to_step(self, STEP_BACKUP_REMINDER);
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Profile creation failed.\n\nPlease try again.");
      gtk_alert_dialog_show(ad, GTK_WINDOW(self));
      g_object_unref(ad);
    }

    g_variant_unref(ret);
  }

  create_profile_ctx_free(ctx);
}

/* Perform actual profile creation - generate mnemonic locally first */
static gboolean perform_profile_creation(OnboardingAssistant *self) {
  if (!self) return FALSE;

  const gchar *display_name = NULL;
  if (self->entry_profile_name) {
    display_name = gtk_editable_get_text(GTK_EDITABLE(self->entry_profile_name));
  }

  gchar *passphrase = NULL;
  if (self->secure_passphrase) {
    passphrase = gn_secure_entry_get_text(self->secure_passphrase);
  }

  if (!display_name || *display_name == '\0') {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Please enter a profile name.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    if (passphrase) gn_secure_entry_free_text(passphrase);
    return FALSE;
  }

  if (!passphrase || strlen(passphrase) < 8) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase must be at least 8 characters.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    if (passphrase) gn_secure_entry_free_text(passphrase);
    return FALSE;
  }

  /* Show status while generating */
  set_status(self, "Generating key...", TRUE);

  /* Generate seed phrase and derive key locally */
  if (!generate_seed_phrase_and_key(self)) {
    set_status(self, NULL, FALSE);
    gn_secure_entry_free_text(passphrase);
    return FALSE;
  }

  /* Get the npub from the generated nsec */
  gchar *npub = NULL;
  GError *error = NULL;
  if (!gn_backup_get_npub(self->generated_nsec, &npub, &error)) {
    g_warning("Failed to derive npub: %s", error ? error->message : "unknown");
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to derive public key: %s",
                                               error ? error->message : "Unknown error");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_clear_error(&error);
    set_status(self, NULL, FALSE);
    gn_secure_entry_free_text(passphrase);
    clear_seed_phrase_data(self);
    return FALSE;
  }

  /* Store the npub */
  g_free(self->created_npub);
  self->created_npub = npub;

  /* Clear status */
  set_status(self, NULL, FALSE);

  /* Clear secure entries */
  if (self->secure_passphrase)
    gn_secure_entry_clear(self->secure_passphrase);
  if (self->secure_passphrase_confirm)
    gn_secure_entry_clear(self->secure_passphrase_confirm);
  gn_secure_entry_free_text(passphrase);

  /* Mark profile as created (key generation successful) */
  self->profile_created = TRUE;

  g_debug("Key generated successfully: %s", self->created_npub);

  /* Proceed to seed phrase display step */
  go_to_step(self, STEP_SEED_PHRASE);

  return TRUE;
}

/* Store the generated key to secure storage after user confirms seed phrase */
static void store_generated_key_async_done(GObject *src, GAsyncResult *res, gpointer user_data);

static gboolean store_generated_key(OnboardingAssistant *self) {
  if (!self || !self->generated_nsec) return FALSE;

  /* Disable navigation while storing */
  if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), FALSE);
  if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), FALSE);
  set_status(self, "Storing key securely...", TRUE);

  /* Get D-Bus connection */
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    set_status(self, NULL, FALSE);
    if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), TRUE);
    if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), TRUE);

    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to connect to session bus: %s", e ? e->message : "unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    if (e) g_clear_error(&e);
    return FALSE;
  }

  /* Get display name */
  const gchar *display_name = "";
  if (self->entry_profile_name) {
    display_name = gtk_editable_get_text(GTK_EDITABLE(self->entry_profile_name));
  }

  /* Call StoreKey D-Bus method to store the generated key.
   * StoreKey accepts nsec format directly - no separate ImportNsec method. */
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "StoreKey",
                         g_variant_new("(ss)", self->generated_nsec, display_name),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         10000,
                         NULL,
                         store_generated_key_async_done,
                         self);

  g_object_unref(bus);
  return TRUE;
}

static void store_generated_key_async_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  (void)src;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  if (!self) return;

  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  set_status(self, NULL, FALSE);

  /* Re-enable navigation */
  if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), TRUE);
  if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), TRUE);

  gboolean ok = FALSE;
  const char *npub_in = NULL;

  if (err) {
    g_warning("StoreKey D-Bus error: %s", err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to store key: %s", err->message);
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_clear_error(&err);
    return;
  }

  if (ret) {
    g_variant_get(ret, "(bs)", &ok, &npub_in);
    g_variant_unref(ret);

    if (ok) {
      g_debug("Key stored successfully");

      /* Clear the sensitive data now that it's stored */
      clear_seed_phrase_data(self);

      /* Proceed to backup reminder */
      go_to_step(self, STEP_BACKUP_REMINDER);
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to store key.\n\nPlease try again.");
      gtk_alert_dialog_show(ad, GTK_WINDOW(self));
      g_object_unref(ad);
    }
  }
}

/* Context for async profile import */
typedef struct {
  OnboardingAssistant *self;
  gchar *data;
  gchar *passphrase;
  gboolean is_mnemonic;
} ImportProfileCtx;

static void import_profile_ctx_free(ImportProfileCtx *ctx) {
  if (!ctx) return;
  if (ctx->data) {
    memset(ctx->data, 0, strlen(ctx->data));
    g_free(ctx->data);
  }
  if (ctx->passphrase) {
    memset(ctx->passphrase, 0, strlen(ctx->passphrase));
    g_free(ctx->passphrase);
  }
  g_free(ctx);
}

/* D-Bus profile import callback */
static void on_import_profile_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  (void)src;
  ImportProfileCtx *ctx = (ImportProfileCtx *)user_data;
  if (!ctx || !ctx->self) {
    import_profile_ctx_free(ctx);
    return;
  }

  OnboardingAssistant *self = ctx->self;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  set_status(self, NULL, FALSE);

  /* Re-enable navigation */
  if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), TRUE);
  if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), TRUE);

  gboolean ok = FALSE;
  const char *npub_in = NULL;

  if (err) {
    g_warning("ImportProfile D-Bus error: %s", err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed: %s", err->message);
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_clear_error(&err);
    import_profile_ctx_free(ctx);
    return;
  }

  if (ret) {
    g_variant_get(ret, "(bs)", &ok, &npub_in);

    if (ok && npub_in && *npub_in) {
      self->profile_created = TRUE;
      g_free(self->created_npub);
      self->created_npub = g_strdup(npub_in);
      g_debug("Profile imported successfully: %s", npub_in);

      /* Clear secure entry on success */
      if (self->secure_import_passphrase)
        gn_secure_entry_clear(self->secure_import_passphrase);

      /* Proceed to backup reminder */
      go_to_step(self, STEP_BACKUP_REMINDER);
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed.\n\nPlease check your input and try again.");
      gtk_alert_dialog_show(ad, GTK_WINDOW(self));
      g_object_unref(ad);
    }

    g_variant_unref(ret);
  }

  import_profile_ctx_free(ctx);
}

/* Perform actual profile import via D-Bus */
static gboolean perform_profile_import(OnboardingAssistant *self) {
  if (!self) return FALSE;

  gchar *data = NULL;
  if (self->text_import_data) {
    data = get_text_view_content(self->text_import_data);
    if (data) g_strstrip(data);
  }

  if (!data || *data == '\0') {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Please enter your key data.");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    g_free(data);
    return FALSE;
  }

  gchar *passphrase = NULL;
  if (self->secure_import_passphrase) {
    passphrase = gn_secure_entry_get_text(self->secure_import_passphrase);
  }

  /* Determine import method based on selected radio and data format */
  gboolean is_mnemonic = self->radio_import_seed &&
                         gtk_check_button_get_active(self->radio_import_seed);
  gboolean is_ncryptsec = g_str_has_prefix(data, "ncryptsec1");
  gboolean is_nsec = g_str_has_prefix(data, "nsec1");

  const char *dbus_method = NULL;

  if (is_mnemonic) {
    dbus_method = "ImportMnemonic";
  } else if (is_ncryptsec) {
    dbus_method = "ImportNip49";
    if (!passphrase || *passphrase == '\0') {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase required for NIP-49 encrypted backup.");
      gtk_alert_dialog_show(ad, GTK_WINDOW(self));
      g_object_unref(ad);
      g_free(data);
      if (passphrase) gn_secure_entry_free_text(passphrase);
      return FALSE;
    }
  } else if (is_nsec) {
    /* Direct nsec import - StoreKey handles nsec format directly */
    dbus_method = "StoreKey";
  } else {
    /* Try as hex - StoreKey handles hex format too */
    dbus_method = "StoreKey";
  }

  /* Disable navigation while processing */
  if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), FALSE);
  if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), FALSE);
  set_status(self, "Importing profile...", TRUE);

  /* Get D-Bus connection */
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    set_status(self, NULL, FALSE);
    if (self->btn_next) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), TRUE);
    if (self->btn_back) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_back), TRUE);

    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to connect to session bus: %s", e ? e->message : "unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(self));
    g_object_unref(ad);
    if (e) g_clear_error(&e);
    g_free(data);
    if (passphrase) gn_secure_entry_free_text(passphrase);
    return FALSE;
  }

  /* Create context for async call */
  ImportProfileCtx *ctx = g_new0(ImportProfileCtx, 1);
  ctx->self = self;
  ctx->data = data; /* Transfer ownership */
  ctx->passphrase = passphrase; /* Transfer ownership */
  ctx->is_mnemonic = is_mnemonic;

  g_debug("Calling D-Bus method %s for import", dbus_method);

  /* Call appropriate D-Bus method */
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         dbus_method,
                         g_variant_new("(ss)", ctx->data, ctx->passphrase ? ctx->passphrase : ""),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         30000,
                         NULL,
                         on_import_profile_done,
                         ctx);

  g_object_unref(bus);
  return TRUE; /* Async - will proceed in callback */
}

static void update_passphrase_strength(OnboardingAssistant *self) {
  if (!self->passphrase_strength || !self->passphrase_hint)
    return;

  const char *passphrase = NULL;
  gchar *secure_passphrase = NULL;

  /* Try secure entry first, fall back to legacy */
  if (self->secure_passphrase) {
    secure_passphrase = gn_secure_entry_get_text(self->secure_passphrase);
    passphrase = secure_passphrase;
  } else if (self->entry_passphrase) {
    passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  }

  gdouble strength = calculate_passphrase_strength(passphrase);

  gtk_level_bar_set_value(self->passphrase_strength, strength);
  gtk_label_set_text(self->passphrase_hint, get_strength_hint(strength));

  /* Update level bar colors via CSS classes */
  GtkWidget *bar = GTK_WIDGET(self->passphrase_strength);
  gtk_widget_remove_css_class(bar, "strength-weak");
  gtk_widget_remove_css_class(bar, "strength-fair");
  gtk_widget_remove_css_class(bar, "strength-good");
  gtk_widget_remove_css_class(bar, "strength-strong");

  if (strength < 0.4) {
    gtk_widget_add_css_class(bar, "strength-weak");
  } else if (strength < 0.6) {
    gtk_widget_add_css_class(bar, "strength-fair");
  } else if (strength < 0.8) {
    gtk_widget_add_css_class(bar, "strength-good");
  } else {
    gtk_widget_add_css_class(bar, "strength-strong");
  }

  /* Update passphrase match label */
  if (self->passphrase_match_label) {
    gchar *confirm = NULL;
    const char *confirm_passphrase = NULL;

    if (self->secure_passphrase_confirm) {
      confirm = gn_secure_entry_get_text(self->secure_passphrase_confirm);
      confirm_passphrase = confirm;
    } else if (self->entry_passphrase_confirm) {
      confirm_passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase_confirm));
    }

    if (confirm_passphrase && *confirm_passphrase) {
      GtkWidget *match_widget = GTK_WIDGET(self->passphrase_match_label);
      if (g_strcmp0(passphrase, confirm_passphrase) == 0) {
        gtk_label_set_text(self->passphrase_match_label, "Passphrases match");
        gtk_widget_remove_css_class(match_widget, "error");
        gtk_widget_add_css_class(match_widget, "success");
      } else {
        gtk_label_set_text(self->passphrase_match_label, "Passphrases do not match");
        gtk_widget_remove_css_class(match_widget, "success");
        gtk_widget_add_css_class(match_widget, "error");
      }
      gtk_widget_set_visible(match_widget, TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->passphrase_match_label), FALSE);
    }

    if (confirm) gn_secure_entry_free_text(confirm);
  }

  if (secure_passphrase) gn_secure_entry_free_text(secure_passphrase);
}

/* Get the step index to navigate to based on step enum */
static int get_carousel_position_for_step(OnboardingAssistant *self, OnboardingStep step) {
  (void)self; /* Used for future path-dependent logic */

  /* Steps 0-2 are always shown: Welcome, Security, Choose Path */
  if (step <= STEP_CHOOSE_PATH) return (int)step;

  /* Steps 3-4 depend on chosen path: Create Passphrase (3) or Import Method (4) */
  if (step == STEP_CREATE_PASSPHRASE) return 3;
  if (step == STEP_IMPORT_METHOD) return 4;

  /* Step 5: Seed Phrase (only for create path, but in carousel order) */
  if (step == STEP_SEED_PHRASE) return 5;

  /* Steps 6-7: Backup Reminder and Ready */
  if (step == STEP_BACKUP_REMINDER) return 6;
  if (step == STEP_READY) return 7;

  return 0;
}

static OnboardingStep get_next_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_WELCOME:
      return STEP_SECURITY;
    case STEP_SECURITY:
      return STEP_CHOOSE_PATH;
    case STEP_CHOOSE_PATH:
      return (self->chosen_path == PATH_CREATE) ? STEP_CREATE_PASSPHRASE : STEP_IMPORT_METHOD;
    case STEP_CREATE_PASSPHRASE:
      return STEP_SEED_PHRASE; /* Show seed phrase after creating profile */
    case STEP_SEED_PHRASE:
      return STEP_BACKUP_REMINDER;
    case STEP_IMPORT_METHOD:
      return STEP_BACKUP_REMINDER; /* Import goes directly to backup reminder */
    case STEP_BACKUP_REMINDER:
      return STEP_READY;
    case STEP_READY:
    default:
      return STEP_READY;
  }
}

static OnboardingStep get_prev_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_WELCOME:
      return STEP_WELCOME;
    case STEP_SECURITY:
      return STEP_WELCOME;
    case STEP_CHOOSE_PATH:
      return STEP_SECURITY;
    case STEP_CREATE_PASSPHRASE:
    case STEP_IMPORT_METHOD:
      return STEP_CHOOSE_PATH;
    case STEP_SEED_PHRASE:
      return STEP_CREATE_PASSPHRASE; /* Back from seed phrase goes to create */
    case STEP_BACKUP_REMINDER:
      /* Back depends on which path was taken */
      if (self->chosen_path == PATH_CREATE) {
        return STEP_SEED_PHRASE;
      } else {
        return STEP_IMPORT_METHOD;
      }
    case STEP_READY:
      return STEP_BACKUP_REMINDER;
    default:
      return STEP_WELCOME;
  }
}

static gboolean can_proceed_from_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_CHOOSE_PATH:
      return self->chosen_path != PATH_NONE;

    case STEP_CREATE_PASSPHRASE: {
      /* Check profile name */
      const char *name = NULL;
      if (self->entry_profile_name) {
        name = gtk_editable_get_text(GTK_EDITABLE(self->entry_profile_name));
      }
      if (!name || !*name) return FALSE;

      /* Check passphrase using secure entry or legacy */
      gchar *pass1 = NULL;
      gchar *pass2 = NULL;

      if (self->secure_passphrase) {
        pass1 = gn_secure_entry_get_text(self->secure_passphrase);
      } else if (self->entry_passphrase) {
        pass1 = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase)));
      }

      if (self->secure_passphrase_confirm) {
        pass2 = gn_secure_entry_get_text(self->secure_passphrase_confirm);
      } else if (self->entry_passphrase_confirm) {
        pass2 = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase_confirm)));
      }

      if (!pass1 || !*pass1) {
        g_free(pass1);
        g_free(pass2);
        return FALSE;
      }
      if (g_strcmp0(pass1, pass2) != 0) {
        g_free(pass1);
        g_free(pass2);
        return FALSE;
      }
      /* Require at least fair strength */
      gdouble strength = calculate_passphrase_strength(pass1);
      gboolean result = strength >= 0.4;

      /* Securely clear */
      if (pass1) { memset(pass1, 0, strlen(pass1)); g_free(pass1); }
      if (pass2) { memset(pass2, 0, strlen(pass2)); g_free(pass2); }

      return result;
    }

    case STEP_IMPORT_METHOD: {
      /* Check that we have valid import data */
      gchar *data = NULL;
      if (self->text_import_data) {
        data = get_text_view_content(self->text_import_data);
        if (data) g_strstrip(data);
      }
      gboolean has_data = (data && *data);
      g_free(data);

      /* For NIP-49, require passphrase */
      if (self->radio_import_nsec && gtk_check_button_get_active(self->radio_import_nsec)) {
        /* Check if data looks like ncryptsec */
        gchar *raw = get_text_view_content(self->text_import_data);
        gboolean is_ncryptsec = (raw && g_str_has_prefix(g_strstrip(raw), "ncryptsec1"));
        g_free(raw);

        if (is_ncryptsec && self->secure_import_passphrase) {
          gchar *pass = gn_secure_entry_get_text(self->secure_import_passphrase);
          gboolean has_pass = (pass && *pass);
          if (pass) gn_secure_entry_free_text(pass);
          return has_data && has_pass;
        }
      }

      return has_data;
    }

    case STEP_SEED_PHRASE:
      /* Must confirm seed phrase has been written down */
      return self->seed_written_down &&
             gtk_check_button_get_active(self->seed_written_down);

    case STEP_BACKUP_REMINDER:
      /* Must acknowledge backup importance */
      return self->backup_understood &&
             gtk_check_button_get_active(self->backup_understood);

    default:
      return TRUE;
  }
}

static void update_navigation_buttons(OnboardingAssistant *self) {
  /* Back button: hidden on first step */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back),
                         self->current_step > STEP_WELCOME);

  /* Next button text changes on last step */
  if (self->current_step == STEP_READY) {
    gtk_button_set_label(self->btn_next, "Get Started");
    gtk_widget_add_css_class(GTK_WIDGET(self->btn_next), "suggested-action");
  } else {
    gtk_button_set_label(self->btn_next, "Next");
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_next), "suggested-action");
  }

  /* Enable/disable next based on validation */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next),
                           can_proceed_from_step(self));

  /* Skip button: always visible except on ready page */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_skip),
                         self->current_step != STEP_READY);
}

static void go_to_step(OnboardingAssistant *self, OnboardingStep step) {
  self->current_step = step;

  /* Calculate actual carousel position */
  int target_pos = get_carousel_position_for_step(self, step);

  /* Get the appropriate page widget */
  GtkWidget *target_page = NULL;
  switch (step) {
    case STEP_WELCOME: target_page = self->page_welcome; break;
    case STEP_SECURITY: target_page = self->page_security; break;
    case STEP_CHOOSE_PATH: target_page = self->page_choose_path; break;
    case STEP_CREATE_PASSPHRASE: target_page = self->page_create_passphrase; break;
    case STEP_IMPORT_METHOD: target_page = self->page_import_method; break;
    case STEP_SEED_PHRASE: target_page = self->page_seed_phrase; break;
    case STEP_BACKUP_REMINDER: target_page = self->page_backup_reminder; break;
    case STEP_READY: target_page = self->page_ready; break;
    default: target_page = self->page_welcome; break;
  }

  /* When entering seed phrase step, populate the grid */
  if (step == STEP_SEED_PHRASE && self->generated_mnemonic) {
    populate_seed_phrase_grid(self);
  }

  if (target_page && self->carousel) {
    adw_carousel_scroll_to(self->carousel, target_page, TRUE);
  }

  update_navigation_buttons(self);
}

/* Signal handlers */
static void on_btn_back_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  OnboardingStep prev = get_prev_step(self);
  go_to_step(self, prev);
}

static void on_btn_next_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  if (self->current_step == STEP_READY) {
    /* Onboarding complete! */
    onboarding_assistant_mark_completed();
    if (self->on_finished) {
      self->on_finished(TRUE, self->on_finished_data);
    }
    gtk_window_close(GTK_WINDOW(self));
    return;
  }

  /* Handle steps that require async operations */
  if (self->current_step == STEP_CREATE_PASSPHRASE && self->chosen_path == PATH_CREATE) {
    /* Generate mnemonic and derive key - will proceed to seed phrase step on success */
    perform_profile_creation(self);
    return;
  }

  if (self->current_step == STEP_SEED_PHRASE) {
    /* User confirmed seed phrase - now store the key securely */
    store_generated_key(self);
    return;
  }

  if (self->current_step == STEP_IMPORT_METHOD && self->chosen_path == PATH_IMPORT) {
    /* Perform actual profile import - async, will proceed to next step on success */
    perform_profile_import(self);
    return;
  }

  OnboardingStep next = get_next_step(self);
  go_to_step(self, next);
}

/* Callback for skip confirmation dialog */
static void on_skip_dialog_response(GObject *src, GAsyncResult *res, gpointer data) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(data);
  GError *err = NULL;
  int response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  if (response == 0) {
    /* User chose to skip */
    onboarding_assistant_mark_completed();
    if (self->on_finished) {
      self->on_finished(FALSE, self->on_finished_data);
    }
    gtk_window_close(GTK_WINDOW(self));
  }
}

static void on_btn_skip_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  /* Show warning dialog before skipping */
  GtkAlertDialog *dlg = gtk_alert_dialog_new(
    "Skip Onboarding?\n\n"
    "You can always access onboarding later from Settings.\n"
    "However, we recommend completing it to understand "
    "how gnostr-signer protects your keys.");
  gtk_alert_dialog_set_buttons(dlg, (const char * const[]){
    "Skip Anyway", "Continue Setup", NULL
  });
  gtk_alert_dialog_set_default_button(dlg, 1);
  gtk_alert_dialog_set_cancel_button(dlg, 1);

  gtk_alert_dialog_choose(dlg, GTK_WINDOW(self), NULL, on_skip_dialog_response, self);
  g_object_unref(dlg);
}

static void on_path_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  if (gtk_check_button_get_active(self->radio_create)) {
    self->chosen_path = PATH_CREATE;
  } else if (gtk_check_button_get_active(self->radio_import)) {
    self->chosen_path = PATH_IMPORT;
  } else {
    self->chosen_path = PATH_NONE;
  }

  update_navigation_buttons(self);
}

static void on_passphrase_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_passphrase_strength(self);
  update_navigation_buttons(self);
}

static void on_backup_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

static void on_carousel_page_changed(AdwCarousel *carousel, guint index, gpointer user_data) {
  (void)carousel;
  (void)index;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

static void onboarding_assistant_dispose(GObject *object) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(object);

  /* Clear secure entries before disposal */
  if (self->secure_passphrase) {
    gn_secure_entry_clear(self->secure_passphrase);
  }
  if (self->secure_passphrase_confirm) {
    gn_secure_entry_clear(self->secure_passphrase_confirm);
  }
  if (self->secure_import_passphrase) {
    gn_secure_entry_clear(self->secure_import_passphrase);
  }

  /* Securely clear seed phrase data */
  clear_seed_phrase_data(self);

  g_clear_pointer(&self->created_npub, g_free);

  G_OBJECT_CLASS(onboarding_assistant_parent_class)->dispose(object);
}

static void onboarding_assistant_class_init(OnboardingAssistantClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = onboarding_assistant_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/onboarding-assistant.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, carousel);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, carousel_dots);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_back);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_next);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_skip);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_welcome);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_security);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_choose_path);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_create_passphrase);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_import_method);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_seed_phrase);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_backup_reminder);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_ready);

  /* Create profile widgets */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, entry_profile_name);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, box_passphrase_container);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, box_confirm_container);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, passphrase_strength);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, passphrase_hint);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, passphrase_match_label);

  /* Legacy passphrase entries (for backward compatibility) */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, entry_passphrase);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, entry_passphrase_confirm);

  /* Path selection */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_create);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import);

  /* Import method widgets */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_nsec);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_seed);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_file);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, text_import_data);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, box_import_passphrase_container);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, dropdown_word_count);

  /* Backup checkbox */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, backup_understood);

  /* Seed phrase display widgets */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, seed_phrase_grid);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_copy_seed);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, seed_written_down);

  /* Status widgets */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, box_status);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, spinner_status);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, lbl_status);
}

/* Secure entry change handler */
static void on_secure_passphrase_changed(GnSecureEntry *entry, gpointer user_data) {
  (void)entry;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_passphrase_strength(self);
  update_navigation_buttons(self);
}

/* Import data text buffer change handler */
static void on_import_data_changed(GtkTextBuffer *buffer, gpointer user_data) {
  (void)buffer;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

/* Profile name entry change handler */
static void on_profile_name_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

static void onboarding_assistant_init(OnboardingAssistant *self) {
  /* Ensure GnSecureEntry type is registered */
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  gtk_widget_init_template(GTK_WIDGET(self));

  self->chosen_path = PATH_NONE;
  self->current_step = STEP_WELCOME;
  self->on_finished = NULL;
  self->on_finished_data = NULL;
  self->profile_created = FALSE;
  self->created_npub = NULL;
  self->generated_mnemonic = NULL;
  self->generated_nsec = NULL;

  /* Create secure passphrase entry for profile creation */
  if (self->box_passphrase_container) {
    self->secure_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
    gn_secure_entry_set_placeholder_text(self->secure_passphrase, "Enter passphrase");
    gn_secure_entry_set_min_length(self->secure_passphrase, 8);
    gn_secure_entry_set_show_strength_indicator(self->secure_passphrase, TRUE);
    gn_secure_entry_set_show_caps_warning(self->secure_passphrase, TRUE);
    gn_secure_entry_set_timeout(self->secure_passphrase, 120);
    gtk_box_append(self->box_passphrase_container, GTK_WIDGET(self->secure_passphrase));
    g_signal_connect(self->secure_passphrase, "changed", G_CALLBACK(on_secure_passphrase_changed), self);
  }

  /* Create secure confirm passphrase entry */
  if (self->box_confirm_container) {
    self->secure_passphrase_confirm = GN_SECURE_ENTRY(gn_secure_entry_new());
    gn_secure_entry_set_placeholder_text(self->secure_passphrase_confirm, "Confirm passphrase");
    gn_secure_entry_set_min_length(self->secure_passphrase_confirm, 8);
    gn_secure_entry_set_show_strength_indicator(self->secure_passphrase_confirm, FALSE);
    gn_secure_entry_set_show_caps_warning(self->secure_passphrase_confirm, TRUE);
    gn_secure_entry_set_timeout(self->secure_passphrase_confirm, 120);
    gtk_box_append(self->box_confirm_container, GTK_WIDGET(self->secure_passphrase_confirm));
    g_signal_connect(self->secure_passphrase_confirm, "changed", G_CALLBACK(on_secure_passphrase_changed), self);
  }

  /* Create secure passphrase entry for import (NIP-49) */
  if (self->box_import_passphrase_container) {
    self->secure_import_passphrase = GN_SECURE_ENTRY(gn_secure_entry_new());
    gn_secure_entry_set_placeholder_text(self->secure_import_passphrase, "Decryption passphrase");
    gn_secure_entry_set_show_strength_indicator(self->secure_import_passphrase, FALSE);
    gn_secure_entry_set_show_caps_warning(self->secure_import_passphrase, TRUE);
    gn_secure_entry_set_timeout(self->secure_import_passphrase, 120);
    gtk_box_append(self->box_import_passphrase_container, GTK_WIDGET(self->secure_import_passphrase));
    g_signal_connect(self->secure_import_passphrase, "changed", G_CALLBACK(on_secure_passphrase_changed), self);
  }

  /* Connect signals */
  if (self->btn_back)
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_btn_back_clicked), self);
  if (self->btn_next)
    g_signal_connect(self->btn_next, "clicked", G_CALLBACK(on_btn_next_clicked), self);
  if (self->btn_skip)
    g_signal_connect(self->btn_skip, "clicked", G_CALLBACK(on_btn_skip_clicked), self);

  /* Path selection */
  if (self->radio_create)
    g_signal_connect(self->radio_create, "toggled", G_CALLBACK(on_path_toggled), self);
  if (self->radio_import)
    g_signal_connect(self->radio_import, "toggled", G_CALLBACK(on_path_toggled), self);

  /* Profile name entry */
  if (self->entry_profile_name)
    g_signal_connect(self->entry_profile_name, "changed", G_CALLBACK(on_profile_name_changed), self);

  /* Legacy passphrase fields (for backward compatibility) */
  if (self->entry_passphrase)
    g_signal_connect(self->entry_passphrase, "changed", G_CALLBACK(on_passphrase_changed), self);
  if (self->entry_passphrase_confirm)
    g_signal_connect(self->entry_passphrase_confirm, "changed", G_CALLBACK(on_passphrase_changed), self);

  /* Import data text buffer */
  if (self->text_import_data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_import_data);
    if (buffer) g_signal_connect(buffer, "changed", G_CALLBACK(on_import_data_changed), self);
  }

  /* Backup checkbox */
  if (self->backup_understood)
    g_signal_connect(self->backup_understood, "toggled", G_CALLBACK(on_backup_toggled), self);

  /* Seed phrase page */
  if (self->btn_copy_seed)
    g_signal_connect(self->btn_copy_seed, "clicked", G_CALLBACK(on_copy_seed_clicked), self);
  if (self->seed_written_down)
    g_signal_connect(self->seed_written_down, "toggled", G_CALLBACK(on_seed_written_toggled), self);

  /* Carousel page change */
  if (self->carousel)
    g_signal_connect(self->carousel, "page-changed", G_CALLBACK(on_carousel_page_changed), self);

  /* Hide status initially */
  if (self->box_status)
    gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);

  /* Hide passphrase match label initially */
  if (self->passphrase_match_label)
    gtk_widget_set_visible(GTK_WIDGET(self->passphrase_match_label), FALSE);

  /* Initial state */
  update_navigation_buttons(self);
}

OnboardingAssistant *onboarding_assistant_new(void) {
  return g_object_new(TYPE_ONBOARDING_ASSISTANT, NULL);
}

void onboarding_assistant_set_on_finished(OnboardingAssistant *self,
                                          OnboardingAssistantFinishedCb cb,
                                          gpointer user_data) {
  g_return_if_fail(ONBOARDING_IS_ASSISTANT(self));
  self->on_finished = cb;
  self->on_finished_data = user_data;
}

gboolean onboarding_assistant_check_should_show(void) {
  g_autoptr(GSettings) settings = get_signer_settings();

  /* Check GSettings flag first */
  if (settings) {
    gboolean completed = g_settings_get_boolean(settings, ONBOARDING_COMPLETED_KEY);
    if (completed) {
      return FALSE;
    }
  }

  /* Also check if any identities exist in accounts or secret store */
  AccountsStore *as = accounts_store_new();
  accounts_store_load(as);
  guint account_count = accounts_store_count(as);
  accounts_store_free(as);

  if (account_count > 0) {
    /* Identities exist, mark onboarding as complete and skip */
    if (settings) {
      g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, TRUE);
    }
    return FALSE;
  }

  /* Also check secret store directly */
  GPtrArray *secrets = secret_store_list();
  guint secret_count = secrets ? secrets->len : 0;
  if (secrets) {
    g_ptr_array_unref(secrets);
  }

  if (secret_count > 0) {
    /* Secrets exist, mark onboarding as complete and skip */
    if (settings) {
      g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, TRUE);
    }
    return FALSE;
  }

  /* No identities found - show onboarding */
  return TRUE;
}

void onboarding_assistant_mark_completed(void) {
  g_autoptr(GSettings) settings = get_signer_settings();
  if (!settings) return;

  g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, TRUE);
  g_debug("Onboarding marked as completed");
}

void onboarding_assistant_reset(void) {
  g_autoptr(GSettings) settings = get_signer_settings();
  if (!settings) return;

  g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, FALSE);
  g_debug("Onboarding reset - will show on next launch");
}
