/* sheet-hw-wallet.c - Hardware wallet connection and selection sheet
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sheet-hw-wallet.h"
#include "../app-resources.h"
#include "../../accounts_store.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

/* Refresh interval for device polling (milliseconds) */
#define DEVICE_REFRESH_INTERVAL_MS 2000

/* Derivation path for Nostr */
#define NOSTR_DERIVATION_PATH "m/44'/1237'/0'/0/0"

struct _SheetHwWallet {
  AdwDialog parent_instance;

  /* Mode */
  SheetHwWalletMode mode;

  /* UI widgets - bound from template */
  AdwNavigationView *nav_view;
  AdwNavigationPage *page_devices;
  AdwNavigationPage *page_confirm;
  AdwNavigationPage *page_success;

  /* Device list page */
  GtkListBox *listbox_devices;
  AdwStatusPage *status_no_devices;
  GtkStack *stack_devices;
  GtkButton *btn_refresh;
  GtkSpinner *spinner_scanning;
  AdwBanner *banner_instructions;

  /* Confirmation page */
  AdwStatusPage *status_confirm;
  GtkButton *btn_confirm_cancel;
  GtkSpinner *spinner_confirm;
  GtkLabel *lbl_confirm_device;
  GtkLabel *lbl_confirm_action;

  /* Success page */
  AdwStatusPage *status_success;
  GtkLabel *lbl_npub_result;
  GtkButton *btn_copy_npub;
  GtkButton *btn_finish;

  /* Label entry (for import mode) */
  AdwEntryRow *entry_label;

  /* State */
  GPtrArray *devices;
  GnHwWalletDeviceInfo *selected_device;
  guint8 hash_to_sign[32];
  gboolean has_hash;
  gchar *device_filter;
  gchar *result_npub;
  gchar *result_signature;
  guint refresh_source_id;

  /* Callbacks */
  SheetHwWalletSuccessCb on_success;
  gpointer on_success_data;
  SheetHwWalletSignCb on_signed;
  gpointer on_signed_data;
};

G_DEFINE_TYPE(SheetHwWallet, sheet_hw_wallet, ADW_TYPE_DIALOG)

/* Forward declarations */
static void refresh_device_list(SheetHwWallet *self);
static void on_device_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_cancel_clicked(GtkButton *button, gpointer user_data);
static void on_finish_clicked(GtkButton *button, gpointer user_data);
static void on_copy_npub_clicked(GtkButton *button, gpointer user_data);
static void start_device_operation(SheetHwWallet *self);

/* ============================================================================
 * Device Row Widget
 * ============================================================================ */

static GtkWidget *
create_device_row(GnHwWalletDeviceInfo *info)
{
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: Device name */
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), info->product ? info->product : "Unknown Device");

  /* Subtitle: Status and type */
  const gchar *state_str = gn_hw_wallet_state_to_string(info->state);
  gchar *subtitle = g_strdup_printf("%s - %s",
                                     gn_hw_wallet_type_to_string(info->type),
                                     state_str);
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);

  /* Icon based on device type */
  const gchar *icon_name = "security-high-symbolic";
  if (gn_hw_wallet_type_is_ledger(info->type)) {
    icon_name = "drive-removable-media-symbolic";
  } else if (gn_hw_wallet_type_is_trezor(info->type)) {
    icon_name = "computer-symbolic";
  }

  GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name(icon_name));
  gtk_widget_add_css_class(GTK_WIDGET(icon), "dim-label");
  adw_action_row_add_prefix(row, GTK_WIDGET(icon));

  /* Status indicator */
  GtkImage *status_icon;
  switch (info->state) {
    case GN_HW_WALLET_STATE_READY:
      status_icon = GTK_IMAGE(gtk_image_new_from_icon_name("emblem-ok-symbolic"));
      gtk_widget_add_css_class(GTK_WIDGET(status_icon), "success");
      break;
    case GN_HW_WALLET_STATE_APP_CLOSED:
      status_icon = GTK_IMAGE(gtk_image_new_from_icon_name("dialog-warning-symbolic"));
      gtk_widget_add_css_class(GTK_WIDGET(status_icon), "warning");
      break;
    case GN_HW_WALLET_STATE_ERROR:
      status_icon = GTK_IMAGE(gtk_image_new_from_icon_name("dialog-error-symbolic"));
      gtk_widget_add_css_class(GTK_WIDGET(status_icon), "error");
      break;
    default:
      status_icon = GTK_IMAGE(gtk_image_new_from_icon_name("network-offline-symbolic"));
      gtk_widget_add_css_class(GTK_WIDGET(status_icon), "dim-label");
      break;
  }
  adw_action_row_add_suffix(row, GTK_WIDGET(status_icon));

  /* Arrow for selection */
  GtkImage *arrow = GTK_IMAGE(gtk_image_new_from_icon_name("go-next-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(arrow), "dim-label");
  adw_action_row_add_suffix(row, GTK_WIDGET(arrow));

  /* Make row activatable */
  adw_action_row_set_activatable_widget(row, GTK_WIDGET(row));

  /* Store device info in row */
  g_object_set_data_full(G_OBJECT(row), "device-info",
                         gn_hw_wallet_device_info_copy(info),
                         (GDestroyNotify)gn_hw_wallet_device_info_free);

  return GTK_WIDGET(row);
}

/* ============================================================================
 * Device Operations
 * ============================================================================ */

static void
refresh_device_list(SheetHwWallet *self)
{
  if (!self)
    return;

  /* Show scanning spinner */
  if (self->spinner_scanning)
    gtk_spinner_set_spinning(self->spinner_scanning, TRUE);

  /* Clear existing devices */
  if (self->devices) {
    g_ptr_array_unref(self->devices);
    self->devices = NULL;
  }

  /* Clear list box */
  if (self->listbox_devices) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->listbox_devices))) != NULL) {
      gtk_list_box_remove(self->listbox_devices, child);
    }
  }

  /* Enumerate devices from manager */
  GnHwWalletManager *manager = gn_hw_wallet_manager_get_default();
  GError *error = NULL;
  self->devices = gn_hw_wallet_manager_enumerate_all_devices(manager, &error);

  if (self->spinner_scanning)
    gtk_spinner_set_spinning(self->spinner_scanning, FALSE);

  if (error) {
    g_warning("Failed to enumerate devices: %s", error->message);
    g_clear_error(&error);
  }

  /* Populate list */
  gboolean has_devices = FALSE;
  if (self->devices && self->devices->len > 0) {
    for (guint i = 0; i < self->devices->len; i++) {
      GnHwWalletDeviceInfo *info = g_ptr_array_index(self->devices, i);

      /* Apply filter if set */
      if (self->device_filter && g_strcmp0(info->device_id, self->device_filter) != 0)
        continue;

      GtkWidget *row = create_device_row(info);
      gtk_list_box_append(self->listbox_devices, row);
      has_devices = TRUE;
    }
  }

  /* Switch stack to show devices or empty state */
  if (self->stack_devices) {
    gtk_stack_set_visible_child_name(self->stack_devices,
                                      has_devices ? "devices" : "empty");
  }
}

static gboolean
refresh_devices_timeout(gpointer user_data)
{
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);
  refresh_device_list(self);
  return G_SOURCE_CONTINUE;
}

static void
on_device_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);
  (void)box;

  if (!row)
    return;

  /* Get device info from row */
  GnHwWalletDeviceInfo *info = g_object_get_data(G_OBJECT(row), "device-info");
  if (!info)
    return;

  /* Store selected device */
  if (self->selected_device)
    gn_hw_wallet_device_info_free(self->selected_device);
  self->selected_device = gn_hw_wallet_device_info_copy(info);

  /* Start the operation */
  start_device_operation(self);
}

static void
start_device_operation(SheetHwWallet *self)
{
  if (!self->selected_device)
    return;

  /* Stop device refresh while operating */
  if (self->refresh_source_id) {
    g_source_remove(self->refresh_source_id);
    self->refresh_source_id = 0;
  }

  /* Update confirmation page */
  if (self->lbl_confirm_device) {
    gtk_label_set_text(self->lbl_confirm_device,
                       self->selected_device->product ? self->selected_device->product : "Hardware Wallet");
  }

  const gchar *action_text = NULL;
  switch (self->mode) {
    case SHEET_HW_WALLET_MODE_IMPORT:
      action_text = "Confirm address on your device";
      break;
    case SHEET_HW_WALLET_MODE_SIGN:
      action_text = "Confirm signing on your device";
      break;
    default:
      action_text = "Please confirm on your device";
      break;
  }
  if (self->lbl_confirm_action)
    gtk_label_set_text(self->lbl_confirm_action, action_text);

  /* Navigate to confirmation page */
  if (self->nav_view && self->page_confirm)
    adw_navigation_view_push(self->nav_view, self->page_confirm);

  /* Show spinner */
  if (self->spinner_confirm)
    gtk_spinner_set_spinning(self->spinner_confirm, TRUE);

  /* Start async operation */
  GnHwWalletManager *manager = gn_hw_wallet_manager_get_default();
  GnHwWalletProvider *provider = gn_hw_wallet_manager_get_provider_for_device(
    manager, self->selected_device->device_id);

  if (!provider) {
    g_warning("No provider found for device");
    return;
  }

  GError *error = NULL;

  /* Open device if needed */
  if (!gn_hw_wallet_provider_open_device(provider, self->selected_device->device_id, &error)) {
    g_warning("Failed to open device: %s", error->message);
    g_clear_error(&error);
    return;
  }

  if (self->mode == SHEET_HW_WALLET_MODE_SIGN && self->has_hash) {
    /* Sign mode - sign the hash */
    guint8 signature[64];
    gsize sig_len = sizeof(signature);

    if (gn_hw_wallet_provider_sign_hash(provider,
                                         self->selected_device->device_id,
                                         NOSTR_DERIVATION_PATH,
                                         self->hash_to_sign, 32,
                                         signature, &sig_len,
                                         &error)) {
      /* Convert signature to hex */
      GString *hex = g_string_new(NULL);
      for (gsize i = 0; i < sig_len; i++)
        g_string_append_printf(hex, "%02x", signature[i]);

      self->result_signature = g_string_free(hex, FALSE);

      /* Navigate to success or call callback */
      if (self->on_signed)
        self->on_signed(self->result_signature, self->on_signed_data);

      adw_dialog_close(ADW_DIALOG(self));
    } else {
      g_warning("Signing failed: %s", error ? error->message : "unknown");
      g_clear_error(&error);
      if (self->nav_view)
        adw_navigation_view_pop(self->nav_view);
    }
  } else {
    /* Import/Select mode - get public key */
    guint8 pubkey[33];
    gsize pubkey_len = sizeof(pubkey);
    gboolean confirm = (self->mode == SHEET_HW_WALLET_MODE_IMPORT);

    if (gn_hw_wallet_provider_get_public_key(provider,
                                              self->selected_device->device_id,
                                              NOSTR_DERIVATION_PATH,
                                              pubkey, &pubkey_len,
                                              confirm, &error)) {
      /* Convert pubkey to npub */
      /* Note: This would use NIP-19 encoding in practice */
      GString *hex = g_string_new("npub1");
      for (gsize i = 0; i < 32; i++)
        g_string_append_printf(hex, "%02x", pubkey[i]);

      self->result_npub = g_string_free(hex, FALSE);

      /* Update success page */
      if (self->lbl_npub_result)
        gtk_label_set_text(self->lbl_npub_result, self->result_npub);

      /* Navigate to success page */
      if (self->nav_view && self->page_success)
        adw_navigation_view_push(self->nav_view, self->page_success);

      /* Import account if in import mode */
      if (self->mode == SHEET_HW_WALLET_MODE_IMPORT) {
        const gchar *label = NULL;
        if (self->entry_label)
          label = gtk_editable_get_text(GTK_EDITABLE(self->entry_label));

        AccountsStore *store = accounts_store_get_default();
        if (store) {
          /* Add as hardware-backed account */
          gchar *stored_npub = NULL;
          accounts_store_import_pubkey(store, self->result_npub,
                                        label && *label ? label : self->selected_device->product,
                                        &stored_npub);
          g_free(stored_npub);
        }

        /* Notify callback */
        if (self->on_success) {
          self->on_success(self->result_npub,
                          self->selected_device->device_id,
                          label, self->on_success_data);
        }
      }
    } else {
      g_warning("Failed to get public key: %s", error ? error->message : "unknown");
      g_clear_error(&error);
      if (self->nav_view)
        adw_navigation_view_pop(self->nav_view);
    }
  }

  /* Close device */
  gn_hw_wallet_provider_close_device(provider, self->selected_device->device_id);

  /* Stop spinner */
  if (self->spinner_confirm)
    gtk_spinner_set_spinning(self->spinner_confirm, FALSE);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_refresh_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);
  refresh_device_list(self);
}

static void
on_cancel_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_finish_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_copy_npub_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  SheetHwWallet *self = SHEET_HW_WALLET(user_data);

  if (!self->result_npub)
    return;

  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb)
      gdk_clipboard_set_text(cb, self->result_npub);
  }
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
sheet_hw_wallet_finalize(GObject *object)
{
  SheetHwWallet *self = SHEET_HW_WALLET(object);

  if (self->refresh_source_id)
    g_source_remove(self->refresh_source_id);

  if (self->devices)
    g_ptr_array_unref(self->devices);

  if (self->selected_device)
    gn_hw_wallet_device_info_free(self->selected_device);

  g_free(self->device_filter);
  g_free(self->result_npub);
  g_free(self->result_signature);

  G_OBJECT_CLASS(sheet_hw_wallet_parent_class)->finalize(object);
}

static void
sheet_hw_wallet_class_init(SheetHwWalletClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->finalize = sheet_hw_wallet_finalize;

  /* Bind template */
  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-hw-wallet.ui");

  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, nav_view);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, page_devices);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, page_confirm);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, page_success);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, listbox_devices);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, status_no_devices);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, stack_devices);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, spinner_scanning);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, banner_instructions);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, status_confirm);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, btn_confirm_cancel);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, spinner_confirm);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, lbl_confirm_device);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, lbl_confirm_action);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, status_success);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, lbl_npub_result);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, btn_copy_npub);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, btn_finish);
  gtk_widget_class_bind_template_child(widget_class, SheetHwWallet, entry_label);
}

static void
sheet_hw_wallet_init(SheetHwWallet *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize state */
  self->mode = SHEET_HW_WALLET_MODE_SELECT;
  self->devices = NULL;
  self->selected_device = NULL;
  self->has_hash = FALSE;
  self->device_filter = NULL;
  self->result_npub = NULL;
  self->result_signature = NULL;

  /* Connect signals */
  if (self->listbox_devices)
    g_signal_connect(self->listbox_devices, "row-activated",
                     G_CALLBACK(on_device_selected), self);

  if (self->btn_refresh)
    g_signal_connect(self->btn_refresh, "clicked",
                     G_CALLBACK(on_refresh_clicked), self);

  if (self->btn_confirm_cancel)
    g_signal_connect(self->btn_confirm_cancel, "clicked",
                     G_CALLBACK(on_cancel_clicked), self);

  if (self->btn_finish)
    g_signal_connect(self->btn_finish, "clicked",
                     G_CALLBACK(on_finish_clicked), self);

  if (self->btn_copy_npub)
    g_signal_connect(self->btn_copy_npub, "clicked",
                     G_CALLBACK(on_copy_npub_clicked), self);

  /* Initial device scan */
  refresh_device_list(self);

  /* Start periodic refresh */
  self->refresh_source_id = g_timeout_add(DEVICE_REFRESH_INTERVAL_MS,
                                           refresh_devices_timeout, self);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

SheetHwWallet *
sheet_hw_wallet_new(SheetHwWalletMode mode)
{
  SheetHwWallet *self = g_object_new(TYPE_SHEET_HW_WALLET, NULL);
  self->mode = mode;

  /* Update UI based on mode */
  const gchar *title = "Hardware Wallet";
  const gchar *banner_text = "Connect your hardware wallet and unlock it";

  switch (mode) {
    case SHEET_HW_WALLET_MODE_IMPORT:
      title = "Import from Hardware Wallet";
      banner_text = "Connect your Ledger or Trezor to import your Nostr identity";
      break;
    case SHEET_HW_WALLET_MODE_SIGN:
      title = "Sign with Hardware Wallet";
      banner_text = "Confirm the transaction on your device";
      break;
    default:
      break;
  }

  adw_dialog_set_title(ADW_DIALOG(self), title);
  if (self->banner_instructions)
    adw_banner_set_title(self->banner_instructions, banner_text);

  return self;
}

void
sheet_hw_wallet_set_on_success(SheetHwWallet *self,
                                SheetHwWalletSuccessCb cb,
                                gpointer user_data)
{
  g_return_if_fail(SHEET_IS_HW_WALLET(self));
  self->on_success = cb;
  self->on_success_data = user_data;
}

void
sheet_hw_wallet_set_on_signed(SheetHwWallet *self,
                               SheetHwWalletSignCb cb,
                               gpointer user_data)
{
  g_return_if_fail(SHEET_IS_HW_WALLET(self));
  self->on_signed = cb;
  self->on_signed_data = user_data;
}

void
sheet_hw_wallet_set_hash_to_sign(SheetHwWallet *self,
                                  const guint8 *hash,
                                  gsize hash_len)
{
  g_return_if_fail(SHEET_IS_HW_WALLET(self));
  g_return_if_fail(hash != NULL);
  g_return_if_fail(hash_len == 32);

  memcpy(self->hash_to_sign, hash, 32);
  self->has_hash = TRUE;
}

void
sheet_hw_wallet_set_device_filter(SheetHwWallet *self,
                                   const char *device_id)
{
  g_return_if_fail(SHEET_IS_HW_WALLET(self));
  g_free(self->device_filter);
  self->device_filter = g_strdup(device_id);
  refresh_device_list(self);
}

void
sheet_hw_wallet_refresh_devices(SheetHwWallet *self)
{
  g_return_if_fail(SHEET_IS_HW_WALLET(self));
  refresh_device_list(self);
}
