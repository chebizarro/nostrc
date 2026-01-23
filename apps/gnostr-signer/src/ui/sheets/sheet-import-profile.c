/* sheet-import-profile.c - Import Profile dialog implementation
 *
 * Provides a UI for importing an existing Nostr profile with multiple methods:
 * - NIP-49 Encrypted Backup (ncryptsec)
 * - Mnemonic Seed Phrase (12/24 words)
 * - Hardware Security Module (HSM via PKCS#11)
 *
 * Includes rate limiting for authentication attempts (nostrc-1g1).
 * Uses secure memory for sensitive data (passphrases, mnemonics) (nostrc-6s2).
 * HSM support added for hardware security modules (nostrc-n30).
 */
#include "sheet-import-profile.h"
#include "../app-resources.h"
#include "../widgets/gn-secure-entry.h"
#include "../../rate-limiter.h"
#include "../../secure-memory.h"
#include "../../hsm_provider.h"
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

  /* HSM widgets */
  AdwBanner *banner_hsm_status;
  AdwPreferencesGroup *group_hsm_devices;
  GtkListBox *listbox_hsm_devices;
  GtkButton *btn_hsm_refresh;
  AdwPreferencesGroup *group_hsm_keys;
  GtkListBox *listbox_hsm_keys;
  GtkButton *btn_hsm_generate_key;
  AdwPreferencesGroup *group_hsm_pin;
  GtkBox *box_hsm_pin_container;
  GtkButton *btn_hsm_unlock;
  AdwStatusPage *status_no_hsm;
  GnSecureEntry *secure_hsm_pin;

  /* HSM state */
  GnHsmManager *hsm_manager;
  GnHsmProvider *selected_provider;
  guint64 selected_slot_id;
  gchar *selected_key_id;
  gboolean hsm_logged_in;
  GCancellable *hsm_cancellable;

  /* Passphrase input (shared for NIP-49 and mnemonic) - using GnSecureEntry */
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

/* Update lockout UI to show remaining time */
static void update_lockout_ui(SheetImportProfile *self) {
  if (!self) return;

  guint remaining = gn_rate_limiter_get_remaining_lockout(self->rate_limiter);

  if (remaining > 0) {
    /* Show lockout message */
    g_autofree gchar *msg = g_strdup_printf("Too many attempts. Please wait %u seconds before trying again.", remaining);
    if (self->lbl_lockout) {
      gtk_label_set_text(self->lbl_lockout, msg);
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_lockout), TRUE);
      gtk_widget_add_css_class(GTK_WIDGET(self->lbl_lockout), "error");
    }
    /* Also update the main status */
    set_status(self, msg, FALSE);

    /* Disable import button */
    if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);
  } else {
    /* Hide lockout message */
    if (self->lbl_lockout) {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_lockout), FALSE);
    }
    set_status(self, NULL, FALSE);

    /* Re-enable import button based on input validity */
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

  /* Lockout expired */
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

  /* Start countdown timer if not already running */
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

/* ============================================================================
 * HSM Helper Functions
 * ============================================================================ */

/* Forward declarations for HSM functions */
static void hsm_refresh_devices(SheetImportProfile *self);
static void hsm_refresh_keys(SheetImportProfile *self);
static void hsm_update_ui_state(SheetImportProfile *self);

/* Create a device row for the HSM device list */
static GtkWidget *
create_hsm_device_row(GnHsmDeviceInfo *info)
{
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), info->label ? info->label : "Unknown Device");

  g_autofree gchar *subtitle = g_strdup_printf("%s %s (Serial: %s)",
                                               info->manufacturer ? info->manufacturer : "",
                                               info->model ? info->model : "",
                                               info->serial ? info->serial : "N/A");
  adw_action_row_set_subtitle(row, subtitle);

  /* Add lock icon if PIN required */
  if (info->needs_pin) {
    GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name("channel-secure-symbolic"));
    adw_action_row_add_suffix(row, GTK_WIDGET(icon));
  }

  /* Add check mark icon for selection */
  GtkImage *check = GTK_IMAGE(gtk_image_new_from_icon_name("object-select-symbolic"));
  gtk_widget_set_visible(GTK_WIDGET(check), FALSE);
  gtk_widget_set_name(GTK_WIDGET(check), "check-icon");
  adw_action_row_add_suffix(row, GTK_WIDGET(check));

  /* Store slot_id in the row for later retrieval */
  g_object_set_data(G_OBJECT(row), "slot-id", GUINT_TO_POINTER((guint)info->slot_id));
  g_object_set_data(G_OBJECT(row), "needs-pin", GUINT_TO_POINTER(info->needs_pin ? 1 : 0));

  return GTK_WIDGET(row);
}

/* Create a key row for the HSM key list */
static GtkWidget *
create_hsm_key_row(GnHsmKeyInfo *info)
{
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), info->label ? info->label : "Unnamed Key");

  g_autofree gchar *subtitle = NULL;
  if (info->npub) {
    /* Show truncated npub */
    gsize npub_len = strlen(info->npub);
    if (npub_len > 20) {
      subtitle = g_strdup_printf("%.12s...%.8s", info->npub, info->npub + npub_len - 8);
    } else {
      subtitle = g_strdup(info->npub);
    }
  } else {
    subtitle = g_strdup_printf("Key ID: %s", info->key_id);
  }
  adw_action_row_set_subtitle(row, subtitle);

  /* Add key icon */
  GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name("dialog-password-symbolic"));
  adw_action_row_add_prefix(row, GTK_WIDGET(icon));

  /* Add check mark icon for selection */
  GtkImage *check = GTK_IMAGE(gtk_image_new_from_icon_name("object-select-symbolic"));
  gtk_widget_set_visible(GTK_WIDGET(check), FALSE);
  gtk_widget_set_name(GTK_WIDGET(check), "check-icon");
  adw_action_row_add_suffix(row, GTK_WIDGET(check));

  /* Store key_id in the row */
  g_object_set_data_full(G_OBJECT(row), "key-id", g_strdup(info->key_id), g_free);
  g_object_set_data_full(G_OBJECT(row), "npub", info->npub ? g_strdup(info->npub) : NULL, g_free);

  return GTK_WIDGET(row);
}

/* Handle device detection completion */
static void
on_hsm_detect_devices_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  if (!self || !self->hsm_manager)
    return;

  GnHsmProvider *provider = GN_HSM_PROVIDER(source);
  GError *error = NULL;
  GPtrArray *devices = gn_hsm_provider_detect_devices_finish(provider, result, &error);

  if (error) {
    g_warning("HSM device detection failed: %s", error->message);
    if (self->banner_hsm_status) {
      adw_banner_set_title(self->banner_hsm_status, error->message);
    }
    g_error_free(error);
    return;
  }

  /* Clear existing device list */
  if (self->listbox_hsm_devices) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->listbox_hsm_devices))) != NULL) {
      gtk_list_box_remove(self->listbox_hsm_devices, child);
    }
  }

  if (!devices || devices->len == 0) {
    /* No devices found */
    if (self->banner_hsm_status) {
      adw_banner_set_title(self->banner_hsm_status, "No hardware security devices found");
      adw_banner_set_revealed(self->banner_hsm_status, TRUE);
    }
    if (self->status_no_hsm) {
      gtk_widget_set_visible(GTK_WIDGET(self->status_no_hsm), TRUE);
    }
    if (self->group_hsm_devices) {
      gtk_widget_set_visible(GTK_WIDGET(self->group_hsm_devices), FALSE);
    }
    if (devices)
      g_ptr_array_unref(devices);
    return;
  }

  /* Hide "no devices" status and show device list */
  if (self->status_no_hsm) {
    gtk_widget_set_visible(GTK_WIDGET(self->status_no_hsm), FALSE);
  }
  if (self->group_hsm_devices) {
    gtk_widget_set_visible(GTK_WIDGET(self->group_hsm_devices), TRUE);
  }
  if (self->banner_hsm_status) {
    g_autofree gchar *msg = g_strdup_printf("Found %u device(s)", devices->len);
    adw_banner_set_title(self->banner_hsm_status, msg);
    adw_banner_set_revealed(self->banner_hsm_status, TRUE);
  }

  /* Populate device list */
  for (guint i = 0; i < devices->len; i++) {
    GnHsmDeviceInfo *info = g_ptr_array_index(devices, i);
    GtkWidget *row = create_hsm_device_row(info);

    /* Also store provider reference */
    g_object_set_data(G_OBJECT(row), "provider", provider);

    gtk_list_box_append(self->listbox_hsm_devices, row);
  }

  g_ptr_array_unref(devices);
  hsm_update_ui_state(self);
}

/* Refresh the device list */
static void
hsm_refresh_devices(SheetImportProfile *self)
{
  if (!self || !self->hsm_manager)
    return;

  /* Update status banner */
  if (self->banner_hsm_status) {
    adw_banner_set_title(self->banner_hsm_status, "Scanning for hardware devices...");
    adw_banner_set_revealed(self->banner_hsm_status, TRUE);
  }

  /* Cancel any pending operation */
  if (self->hsm_cancellable) {
    g_cancellable_cancel(self->hsm_cancellable);
    g_object_unref(self->hsm_cancellable);
  }
  self->hsm_cancellable = g_cancellable_new();

  /* Reset selection */
  self->selected_provider = NULL;
  self->selected_slot_id = 0;
  g_clear_pointer(&self->selected_key_id, g_free);
  self->hsm_logged_in = FALSE;

  /* Get available providers and detect devices */
  GList *providers = gn_hsm_manager_get_available_providers(self->hsm_manager);
  for (GList *l = providers; l != NULL; l = l->next) {
    GnHsmProvider *provider = GN_HSM_PROVIDER(l->data);

    /* Initialize provider if needed */
    GError *err = NULL;
    if (!gn_hsm_provider_init(provider, &err)) {
      if (err) {
        g_warning("Failed to init HSM provider %s: %s",
                  gn_hsm_provider_get_name(provider), err->message);
        g_error_free(err);
      }
      continue;
    }

    /* Start async detection */
    gn_hsm_provider_detect_devices_async(provider, self->hsm_cancellable,
                                         on_hsm_detect_devices_done, self);
  }
  g_list_free(providers);
}

/* Refresh keys for the selected device */
static void
hsm_refresh_keys(SheetImportProfile *self)
{
  if (!self || !self->selected_provider)
    return;

  /* Clear existing key list */
  if (self->listbox_hsm_keys) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->listbox_hsm_keys))) != NULL) {
      gtk_list_box_remove(self->listbox_hsm_keys, child);
    }
  }

  GError *error = NULL;
  GPtrArray *keys = gn_hsm_provider_list_keys(self->selected_provider,
                                               self->selected_slot_id,
                                               &error);
  if (error) {
    g_warning("Failed to list keys: %s", error->message);
    g_error_free(error);
    return;
  }

  if (!keys || keys->len == 0) {
    /* No keys on device - that's OK, user can generate one */
    if (keys)
      g_ptr_array_unref(keys);
    return;
  }

  /* Populate key list */
  for (guint i = 0; i < keys->len; i++) {
    GnHsmKeyInfo *info = g_ptr_array_index(keys, i);
    GtkWidget *row = create_hsm_key_row(info);
    gtk_list_box_append(self->listbox_hsm_keys, row);
  }

  g_ptr_array_unref(keys);
  hsm_update_ui_state(self);
}

/* Update HSM UI state based on current selections */
static void
hsm_update_ui_state(SheetImportProfile *self)
{
  if (!self)
    return;

  gboolean has_device = (self->selected_provider != NULL);
  gboolean device_needs_pin = FALSE;
  gboolean is_logged_in = self->hsm_logged_in;

  /* Check if selected device needs PIN */
  if (has_device && self->listbox_hsm_devices) {
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(self->listbox_hsm_devices);
    if (selected) {
      device_needs_pin = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(selected), "needs-pin")) != 0;
    }
  }

  /* Show/hide PIN entry group */
  if (self->group_hsm_pin) {
    gtk_widget_set_visible(GTK_WIDGET(self->group_hsm_pin),
                           has_device && device_needs_pin && !is_logged_in);
  }

  /* Show/hide key selection group */
  if (self->group_hsm_keys) {
    gtk_widget_set_visible(GTK_WIDGET(self->group_hsm_keys),
                           has_device && (!device_needs_pin || is_logged_in));
  }

  /* Update import button sensitivity */
  update_import_button_sensitivity(self);
}

/* Handle device row selection */
static void
on_hsm_device_row_activated(GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  if (!self || !row)
    return;

  /* Get device info from row */
  GnHsmProvider *provider = g_object_get_data(G_OBJECT(row), "provider");
  guint slot_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "slot-id"));
  gboolean needs_pin = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "needs-pin")) != 0;

  /* Update selection state */
  self->selected_provider = provider;
  self->selected_slot_id = slot_id;
  g_clear_pointer(&self->selected_key_id, g_free);
  self->hsm_logged_in = !needs_pin; /* Auto-logged-in if no PIN needed */

  /* Update check marks on all rows */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(listbox));
  while (child) {
    GtkWidget *check = gtk_widget_get_first_child(child);
    while (check) {
      if (g_strcmp0(gtk_widget_get_name(check), "check-icon") == 0) {
        gtk_widget_set_visible(check, (GtkWidget *)row == child);
        break;
      }
      check = gtk_widget_get_next_sibling(check);
    }
    child = gtk_widget_get_next_sibling(child);
  }

  hsm_update_ui_state(self);

  /* If no PIN required, refresh keys immediately */
  if (!needs_pin) {
    hsm_refresh_keys(self);
  }
}

/* Handle key row selection */
static void
on_hsm_key_row_activated(GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  if (!self || !row)
    return;

  /* Get key ID from row */
  const gchar *key_id = g_object_get_data(G_OBJECT(row), "key-id");

  /* Update selection */
  g_clear_pointer(&self->selected_key_id, g_free);
  self->selected_key_id = g_strdup(key_id);

  /* Update check marks */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(listbox));
  while (child) {
    GtkWidget *check = gtk_widget_get_first_child(child);
    while (check) {
      if (g_strcmp0(gtk_widget_get_name(check), "check-icon") == 0) {
        gtk_widget_set_visible(check, (GtkWidget *)row == child);
        break;
      }
      check = gtk_widget_get_next_sibling(check);
    }
    child = gtk_widget_get_next_sibling(child);
  }

  update_import_button_sensitivity(self);
}

/* Handle refresh devices button */
static void
on_hsm_refresh_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  hsm_refresh_devices(self);
}

/* Handle unlock device button */
static void
on_hsm_unlock_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  if (!self || !self->selected_provider)
    return;

  gchar *pin = NULL;
  if (self->secure_hsm_pin) {
    pin = gn_secure_entry_get_text(self->secure_hsm_pin);
  }

  GError *error = NULL;
  gboolean success = gn_hsm_provider_login(self->selected_provider,
                                            self->selected_slot_id,
                                            pin, &error);

  if (pin) {
    gn_secure_entry_free_text(pin);
  }

  if (!success) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to unlock device: %s",
                                               error ? error->message : "Unknown error");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    if (error)
      g_error_free(error);
    return;
  }

  self->hsm_logged_in = TRUE;

  /* Clear PIN entry */
  if (self->secure_hsm_pin) {
    gn_secure_entry_clear(self->secure_hsm_pin);
  }

  hsm_update_ui_state(self);
  hsm_refresh_keys(self);
}

/* Handle generate key button */
static void
on_hsm_generate_key_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetImportProfile *self = SHEET_IMPORT_PROFILE(user_data);
  if (!self || !self->selected_provider)
    return;

  /* Generate a new key on the device */
  GError *error = NULL;
  GnHsmKeyInfo *info = gn_hsm_provider_generate_key(self->selected_provider,
                                                     self->selected_slot_id,
                                                     "Nostr Key",
                                                     GN_HSM_KEY_TYPE_SECP256K1,
                                                     &error);
  if (!info) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to generate key: %s",
                                               error ? error->message : "Unknown error");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    if (error)
      g_error_free(error);
    return;
  }

  /* Success - refresh key list and select the new key */
  g_clear_pointer(&self->selected_key_id, g_free);
  self->selected_key_id = g_strdup(info->key_id);

  gn_hsm_key_info_free(info);

  /* Show success message */
  GtkAlertDialog *ad = gtk_alert_dialog_new("Key generated successfully!");
  gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
  g_object_unref(ad);

  hsm_refresh_keys(self);
  update_import_button_sensitivity(self);
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

  /* When switching to hardware mode, scan for devices */
  if (show_hardware && self->hsm_manager) {
    hsm_refresh_devices(self);
  }

  update_import_button_sensitivity(self);
}

/* Check if we have valid input for the current method */
static gboolean has_valid_input(SheetImportProfile *self) {
  if (!self) return FALSE;

  /* Check rate limiting first */
  if (self->rate_limiter && !gn_rate_limiter_check_allowed(self->rate_limiter)) {
    return FALSE;
  }

  gchar *passphrase = NULL;
  if (self->secure_passphrase) {
    passphrase = gn_secure_entry_get_text(self->secure_passphrase);
  }
  gboolean has_passphrase = (passphrase && *passphrase);

  gboolean result = FALSE;
  switch (self->current_method) {
    case IMPORT_METHOD_NIP49: {
      g_autofree gchar *ncryptsec = get_text_view_content(self->text_ncryptsec);
      gboolean valid_ncryptsec = is_valid_ncryptsec(ncryptsec);
      result = valid_ncryptsec && has_passphrase;
      break;
    }

    case IMPORT_METHOD_MNEMONIC: {
      g_autofree gchar *mnemonic = get_text_view_content(self->text_mnemonic);
      int expected = get_expected_word_count(self);
      gboolean valid_mnemonic = is_valid_mnemonic(mnemonic, expected);
      /* Passphrase is optional for mnemonic but we need at least the mnemonic */
      result = valid_mnemonic;
      break;
    }

    case IMPORT_METHOD_HARDWARE:
      /* Hardware import requires a selected device and key, and being logged in */
      result = (self->selected_provider != NULL &&
                self->selected_key_id != NULL &&
                self->hsm_logged_in);
      break;

    default:
      result = FALSE;
  }

  /* Securely clear passphrase */
  if (passphrase) {
    gn_secure_entry_free_text(passphrase);
  }

  return result;
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
static void on_secure_passphrase_changed(GnSecureEntry *entry, gpointer user_data) {
  (void)entry;
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
  if (self) {
    /* Clear secure entry before closing */
    if (self->secure_passphrase)
      gn_secure_entry_clear(self->secure_passphrase);
    adw_dialog_close(ADW_DIALOG(self));
  }
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
  /* Securely clear and free sensitive data */
  gn_secure_clear_string(ctx->data);  /* Mnemonic or ncryptsec */
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
      /* Record successful authentication attempt - resets rate limiter */
      if (self->rate_limiter) {
        gn_rate_limiter_record_attempt(self->rate_limiter, TRUE);
      }

      /* Clear secure entry on success */
      if (self->secure_passphrase)
        gn_secure_entry_clear(self->secure_passphrase);

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
      /* Record failed authentication attempt */
      if (self->rate_limiter) {
        gn_rate_limiter_record_attempt(self->rate_limiter, FALSE);
        update_lockout_ui(self);
      }

      /* Check if we're now locked out */
      guint remaining = self->rate_limiter ? gn_rate_limiter_get_remaining_lockout(self->rate_limiter) : 0;
      if (remaining > 0) {
        g_autofree gchar *msg = g_strdup_printf("Import failed. Too many attempts.\n\nPlease wait %u seconds before trying again.", remaining);
        GtkAlertDialog *ad = gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
      } else {
        guint attempts_left = self->rate_limiter ? gn_rate_limiter_get_attempts_remaining(self->rate_limiter) : 0;
        g_autofree gchar *msg = NULL;
        if (attempts_left > 0) {
          msg = g_strdup_printf("Import failed.\n\nPlease check your input and try again.\n(%u attempts remaining)", attempts_left);
        } else {
          msg = g_strdup("Import failed.\n\nPlease check your input and try again.");
        }
        GtkAlertDialog *ad = gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_show(ad, ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
      }
    }
  }

  import_ctx_free(ctx);
}

/* Import button handler */
static void on_import(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetImportProfile *self = (SheetImportProfile *)user_data;
  if (!self) return;

  /* Check rate limiting before attempting import */
  if (self->rate_limiter && !gn_rate_limiter_check_allowed(self->rate_limiter)) {
    guint remaining = gn_rate_limiter_get_remaining_lockout(self->rate_limiter);
    g_autofree gchar *msg = g_strdup_printf("Too many attempts.\n\nPlease wait %u seconds before trying again.", remaining);
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
        gn_secure_entry_free_text(passphrase);
        return;
      }
      if (!passphrase || *passphrase == '\0') {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Passphrase required.\n\nPlease enter the passphrase used to encrypt this backup.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
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
          gn_secure_entry_free_text(passphrase);
          return;
        }
      }
      method_name = "Mnemonic";
      dbus_method = "ImportMnemonic";
      break;

    case IMPORT_METHOD_HARDWARE: {
      /* HSM import - get the public key from the selected key on the device */
      if (!self->selected_provider || !self->selected_key_id) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Please select a device and key first.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        gn_secure_entry_free_text(passphrase);
        return;
      }

      GError *hsm_error = NULL;
      GnHsmKeyInfo *key_info = gn_hsm_provider_get_public_key(self->selected_provider,
                                                               self->selected_slot_id,
                                                               self->selected_key_id,
                                                               &hsm_error);
      if (!key_info) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to get key info: %s",
                                                   hsm_error ? hsm_error->message : "Unknown error");
        gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
        g_object_unref(ad);
        if (hsm_error) g_error_free(hsm_error);
        gn_secure_entry_free_text(passphrase);
        return;
      }

      /* For HSM import, the "data" is the HSM key reference info
       * We use a special format: "hsm:<provider>:<slot>:<key_id>:<npub>"
       */
      data = g_strdup_printf("hsm:%s:%" G_GUINT64_FORMAT ":%s:%s",
                             gn_hsm_provider_get_name(self->selected_provider),
                             self->selected_slot_id,
                             self->selected_key_id,
                             key_info->npub ? key_info->npub : "");

      method_name = "HSM";
      dbus_method = "ImportHsm";

      gn_hsm_key_info_free(key_info);
      break;
    }

    default:
      gn_secure_entry_free_text(passphrase);
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
    gn_secure_entry_free_text(passphrase);
    return;
  }

  /* Create context for async call - passphrase ownership transfers to ctx */
  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->method = self->current_method;
  ctx->data = g_strdup(data);
  ctx->passphrase = passphrase; /* Transfer ownership */

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
                         g_variant_new("(ss)", data, ctx->passphrase ? ctx->passphrase : ""),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         30000, /* 30 second timeout for key derivation */
                         NULL,
                         import_dbus_done,
                         ctx);

  g_object_unref(bus);
}

static void sheet_import_profile_dispose(GObject *obj) {
  SheetImportProfile *self = (SheetImportProfile *)obj;

  /* Clear secure entries before disposal */
  if (self->secure_passphrase) {
    gn_secure_entry_clear(self->secure_passphrase);
  }
  if (self->secure_hsm_pin) {
    gn_secure_entry_clear(self->secure_hsm_pin);
  }

  /* Cancel any pending HSM operations */
  if (self->hsm_cancellable) {
    g_cancellable_cancel(self->hsm_cancellable);
    g_clear_object(&self->hsm_cancellable);
  }

  G_OBJECT_CLASS(sheet_import_profile_parent_class)->dispose(obj);
}

static void sheet_import_profile_finalize(GObject *obj) {
  SheetImportProfile *self = (SheetImportProfile *)obj;

  /* Cancel lockout timer if running */
  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  /* Disconnect rate limiter signals */
  if (self->rate_limiter) {
    if (self->rate_limit_handler_id > 0) {
      g_signal_handler_disconnect(self->rate_limiter, self->rate_limit_handler_id);
    }
    if (self->lockout_expired_handler_id > 0) {
      g_signal_handler_disconnect(self->rate_limiter, self->lockout_expired_handler_id);
    }
    /* Don't unref if using singleton */
  }

  /* Clean up HSM state */
  g_clear_pointer(&self->selected_key_id, g_free);

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
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, radio_hardware);

  /* Input sections */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_nip49);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_ncryptsec);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, text_mnemonic);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, dropdown_word_count);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_hardware);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_passphrase);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_passphrase_container);

  /* HSM widgets */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, banner_hsm_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, group_hsm_devices);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, listbox_hsm_devices);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_hsm_refresh);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, group_hsm_keys);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, listbox_hsm_keys);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_hsm_generate_key);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, group_hsm_pin);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_hsm_pin_container);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, btn_hsm_unlock);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, status_no_hsm);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, box_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetImportProfile, lbl_status);
}

static void sheet_import_profile_init(SheetImportProfile *self) {
  /* Ensure GnSecureEntry type is registered */
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize current method */
  self->current_method = IMPORT_METHOD_NIP49;

  /* Initialize rate limiter (use singleton instance for shared state) */
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
  gn_secure_entry_set_timeout(self->secure_passphrase, 120); /* 2 minute timeout */

  if (self->box_passphrase_container) {
    gtk_box_append(self->box_passphrase_container, GTK_WIDGET(self->secure_passphrase));
  }

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

  /* Connect secure passphrase entry handler */
  g_signal_connect(self->secure_passphrase, "changed", G_CALLBACK(on_secure_passphrase_changed), self);

  /* Connect word count dropdown handler */
  if (self->dropdown_word_count) {
    g_signal_connect(self->dropdown_word_count, "notify::selected", G_CALLBACK(on_word_count_changed), self);
  }

  /* Initialize HSM manager and state */
  self->hsm_manager = gn_hsm_manager_get_default();
  self->selected_provider = NULL;
  self->selected_slot_id = 0;
  self->selected_key_id = NULL;
  self->hsm_logged_in = FALSE;
  self->hsm_cancellable = NULL;

  /* Create secure PIN entry for HSM */
  self->secure_hsm_pin = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_hsm_pin, "Enter device PIN");
  gn_secure_entry_set_show_strength_indicator(self->secure_hsm_pin, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_hsm_pin, FALSE);
  gn_secure_entry_set_timeout(self->secure_hsm_pin, 60); /* 1 minute timeout */

  if (self->box_hsm_pin_container) {
    gtk_box_append(self->box_hsm_pin_container, GTK_WIDGET(self->secure_hsm_pin));
  }

  /* Connect HSM button handlers */
  if (self->btn_hsm_refresh) {
    g_signal_connect(self->btn_hsm_refresh, "clicked", G_CALLBACK(on_hsm_refresh_clicked), self);
  }
  if (self->btn_hsm_unlock) {
    g_signal_connect(self->btn_hsm_unlock, "clicked", G_CALLBACK(on_hsm_unlock_clicked), self);
  }
  if (self->btn_hsm_generate_key) {
    g_signal_connect(self->btn_hsm_generate_key, "clicked", G_CALLBACK(on_hsm_generate_key_clicked), self);
  }

  /* Connect HSM list selection handlers */
  if (self->listbox_hsm_devices) {
    g_signal_connect(self->listbox_hsm_devices, "row-activated",
                     G_CALLBACK(on_hsm_device_row_activated), self);
  }
  if (self->listbox_hsm_keys) {
    g_signal_connect(self->listbox_hsm_keys, "row-activated",
                     G_CALLBACK(on_hsm_key_row_activated), self);
  }

  /* Initially disable import button */
  if (self->btn_import) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_import), FALSE);

  /* Check if already locked out (from previous dialog usage) */
  if (gn_rate_limiter_is_locked_out(self->rate_limiter)) {
    self->lockout_timer_id = g_timeout_add_seconds(1, update_lockout_countdown, self);
    update_lockout_ui(self);
  }

  /* Set initial visibility */
  update_visible_sections(self);

  /* Setup keyboard navigation (nostrc-tz8w):
   * - Focus ncryptsec text view on dialog open (for NIP-49 method)
   * - Import button is default (Enter activates when form is valid) */
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
