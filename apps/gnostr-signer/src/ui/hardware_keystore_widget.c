/* hardware_keystore_widget.c - Hardware Keystore Settings Widget implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "hardware_keystore_widget.h"
#include "../settings_manager.h"

struct _HwKeystoreWidget {
  GtkBox parent_instance;

  HwKeystoreManager *manager;
  gboolean owns_manager;

  /* Header */
  GtkWidget *header_box;
  GtkWidget *title_label;
  GtkWidget *status_icon;
  GtkWidget *status_label;

  /* Enable switch */
  GtkWidget *enable_row;
  GtkWidget *enable_switch;

  /* Details expander */
  GtkWidget *details_revealer;
  GtkWidget *details_box;

  /* Hardware info */
  GtkWidget *info_grid;
  GtkWidget *backend_label;
  GtkWidget *backend_value;
  GtkWidget *status_detail_label;
  GtkWidget *status_detail_value;
  GtkWidget *master_key_label;
  GtkWidget *master_key_value;

  /* Action buttons */
  GtkWidget *button_box;
  GtkWidget *setup_button;
  GtkWidget *reset_button;
  GtkWidget *delete_button;

  /* Fallback options */
  GtkWidget *fallback_row;
  GtkWidget *fallback_switch;

  /* Signal handlers */
  gulong mode_changed_handler;
  gulong status_changed_handler;
};

G_DEFINE_TYPE(HwKeystoreWidget, hw_keystore_widget, GTK_TYPE_BOX)

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void on_enable_switch_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_fallback_switch_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_setup_button_clicked(GtkButton *button, gpointer user_data);
static void on_reset_button_clicked(GtkButton *button, gpointer user_data);
static void on_delete_button_clicked(GtkButton *button, gpointer user_data);
static void on_mode_changed(HwKeystoreManager *manager, gint mode, gpointer user_data);
static void on_status_changed(HwKeystoreManager *manager, gint status, gpointer user_data);
static void update_ui(HwKeystoreWidget *self);

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
hw_keystore_widget_dispose(GObject *object)
{
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(object);

  if (self->manager) {
    if (self->mode_changed_handler > 0) {
      g_signal_handler_disconnect(self->manager, self->mode_changed_handler);
      self->mode_changed_handler = 0;
    }
    if (self->status_changed_handler > 0) {
      g_signal_handler_disconnect(self->manager, self->status_changed_handler);
      self->status_changed_handler = 0;
    }
    if (self->owns_manager) {
      g_clear_object(&self->manager);
    }
  }

  G_OBJECT_CLASS(hw_keystore_widget_parent_class)->dispose(object);
}

static void
hw_keystore_widget_class_init(HwKeystoreWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = hw_keystore_widget_dispose;
}

static void
hw_keystore_widget_init(HwKeystoreWidget *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 12);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 0);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 0);

  /* Apply CSS class for styling */
  gtk_widget_add_css_class(GTK_WIDGET(self), "hardware-keystore-widget");

  /* === Header section === */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(self), self->header_box);

  /* Status icon */
  self->status_icon = gtk_image_new_from_icon_name("security-high-symbolic");
  gtk_widget_add_css_class(self->status_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->header_box), self->status_icon);

  /* Title and status */
  GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(title_box, TRUE);
  gtk_box_append(GTK_BOX(self->header_box), title_box);

  self->title_label = gtk_label_new("Hardware Keystore");
  gtk_widget_add_css_class(self->title_label, "title-4");
  gtk_widget_set_halign(self->title_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(title_box), self->title_label);

  self->status_label = gtk_label_new("Checking hardware...");
  gtk_widget_add_css_class(self->status_label, "dim-label");
  gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(title_box), self->status_label);

  /* === Enable row === */
  self->enable_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(self->enable_row, "card");
  gtk_widget_set_margin_top(self->enable_row, 8);
  gtk_box_append(GTK_BOX(self), self->enable_row);

  GtkWidget *enable_label = gtk_label_new("Enable hardware-backed keys");
  gtk_widget_set_hexpand(enable_label, TRUE);
  gtk_widget_set_halign(enable_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start(enable_label, 12);
  gtk_widget_set_margin_top(enable_label, 8);
  gtk_widget_set_margin_bottom(enable_label, 8);
  gtk_box_append(GTK_BOX(self->enable_row), enable_label);

  self->enable_switch = gtk_switch_new();
  gtk_widget_set_valign(self->enable_switch, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(self->enable_switch, 12);
  gtk_box_append(GTK_BOX(self->enable_row), self->enable_switch);

  g_signal_connect(self->enable_switch, "notify::active",
                   G_CALLBACK(on_enable_switch_toggled), self);

  /* === Details revealer === */
  self->details_revealer = gtk_revealer_new();
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->details_revealer), FALSE);
  gtk_revealer_set_transition_type(GTK_REVEALER(self->details_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_box_append(GTK_BOX(self), self->details_revealer);

  self->details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_revealer_set_child(GTK_REVEALER(self->details_revealer), self->details_box);

  /* === Hardware info grid === */
  self->info_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(self->info_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(self->info_grid), 12);
  gtk_widget_add_css_class(self->info_grid, "card");
  gtk_widget_set_margin_start(self->info_grid, 12);
  gtk_widget_set_margin_end(self->info_grid, 12);
  gtk_widget_set_margin_top(self->info_grid, 8);
  gtk_widget_set_margin_bottom(self->info_grid, 8);
  gtk_box_append(GTK_BOX(self->details_box), self->info_grid);

  /* Backend row */
  self->backend_label = gtk_label_new("Backend:");
  gtk_widget_add_css_class(self->backend_label, "dim-label");
  gtk_widget_set_halign(self->backend_label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->backend_label, 0, 0, 1, 1);

  self->backend_value = gtk_label_new("Unknown");
  gtk_widget_set_halign(self->backend_value, GTK_ALIGN_START);
  gtk_widget_set_hexpand(self->backend_value, TRUE);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->backend_value, 1, 0, 1, 1);

  /* Status detail row */
  self->status_detail_label = gtk_label_new("Status:");
  gtk_widget_add_css_class(self->status_detail_label, "dim-label");
  gtk_widget_set_halign(self->status_detail_label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->status_detail_label, 0, 1, 1, 1);

  self->status_detail_value = gtk_label_new("Unknown");
  gtk_widget_set_halign(self->status_detail_value, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->status_detail_value, 1, 1, 1, 1);

  /* Master key row */
  self->master_key_label = gtk_label_new("Master Key:");
  gtk_widget_add_css_class(self->master_key_label, "dim-label");
  gtk_widget_set_halign(self->master_key_label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->master_key_label, 0, 2, 1, 1);

  self->master_key_value = gtk_label_new("Not created");
  gtk_widget_set_halign(self->master_key_value, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(self->info_grid), self->master_key_value, 1, 2, 1, 1);

  /* === Action buttons === */
  self->button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(self->button_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self->details_box), self->button_box);

  self->setup_button = gtk_button_new_with_label("Create Master Key");
  gtk_widget_add_css_class(self->setup_button, "suggested-action");
  gtk_box_append(GTK_BOX(self->button_box), self->setup_button);
  g_signal_connect(self->setup_button, "clicked",
                   G_CALLBACK(on_setup_button_clicked), self);

  self->reset_button = gtk_button_new_with_label("Reset Master Key");
  gtk_box_append(GTK_BOX(self->button_box), self->reset_button);
  g_signal_connect(self->reset_button, "clicked",
                   G_CALLBACK(on_reset_button_clicked), self);

  self->delete_button = gtk_button_new_with_label("Delete Master Key");
  gtk_widget_add_css_class(self->delete_button, "destructive-action");
  gtk_box_append(GTK_BOX(self->button_box), self->delete_button);
  g_signal_connect(self->delete_button, "clicked",
                   G_CALLBACK(on_delete_button_clicked), self);

  /* === Fallback row === */
  self->fallback_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(self->fallback_row, "card");
  gtk_widget_set_margin_top(self->fallback_row, 4);
  gtk_box_append(GTK_BOX(self->details_box), self->fallback_row);

  GtkWidget *fallback_label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(fallback_label_box, TRUE);
  gtk_widget_set_margin_start(fallback_label_box, 12);
  gtk_widget_set_margin_top(fallback_label_box, 8);
  gtk_widget_set_margin_bottom(fallback_label_box, 8);
  gtk_box_append(GTK_BOX(self->fallback_row), fallback_label_box);

  GtkWidget *fallback_title = gtk_label_new("Software fallback");
  gtk_widget_set_halign(fallback_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(fallback_label_box), fallback_title);

  GtkWidget *fallback_desc = gtk_label_new("Use software keystore if hardware unavailable");
  gtk_widget_add_css_class(fallback_desc, "dim-label");
  gtk_widget_add_css_class(fallback_desc, "caption");
  gtk_widget_set_halign(fallback_desc, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(fallback_label_box), fallback_desc);

  self->fallback_switch = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(self->fallback_switch), TRUE);
  gtk_widget_set_valign(self->fallback_switch, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(self->fallback_switch, 12);
  gtk_box_append(GTK_BOX(self->fallback_row), self->fallback_switch);

  g_signal_connect(self->fallback_switch, "notify::active",
                   G_CALLBACK(on_fallback_switch_toggled), self);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_enable_switch_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);

  if (!self->manager)
    return;

  gboolean active = gtk_switch_get_active(sw);
  hw_keystore_manager_set_enabled(self->manager, active);

  /* Reveal details when enabled */
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->details_revealer), active);

  /* Save to settings */
  SettingsManager *settings = settings_manager_get_default();
  settings_manager_set_hardware_keystore_enabled(settings, active);

  update_ui(self);
}

static void
on_fallback_switch_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);

  if (!self->manager)
    return;

  GnHsmProviderTpm *provider = hw_keystore_manager_get_provider(self->manager);
  if (provider) {
    gboolean active = gtk_switch_get_active(sw);
    gn_hsm_provider_tpm_set_fallback_enabled(provider, active);

    /* Save to settings */
    SettingsManager *settings = settings_manager_get_default();
    settings_manager_set_hardware_keystore_fallback(settings, active);
  }
}

static void
on_setup_button_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);

  if (!self->manager)
    return;

  GError *error = NULL;
  if (!hw_keystore_manager_setup_master_key(self->manager, &error)) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
    GtkAlertDialog *dialog = gtk_alert_dialog_new(
      "Failed to create master key: %s",
      error ? error->message : "Unknown error");
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
    g_clear_error(&error);
    return;
  }

  update_ui(self);

  /* Show success message */
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  GtkAlertDialog *dialog = gtk_alert_dialog_new(
    "Master key created successfully. Hardware-backed signing is now available.");
  gtk_alert_dialog_show(dialog, parent);
  g_object_unref(dialog);
}

static void
on_reset_confirm(GObject *source, GAsyncResult *res, gpointer user_data)
{
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);
  GError *err = NULL;

  gint response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  if (response != 0)
    return; /* Not confirmed */

  GError *error = NULL;
  if (!hw_keystore_manager_reset_master_key(self->manager, &error)) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
    GtkAlertDialog *dialog = gtk_alert_dialog_new(
      "Failed to reset master key: %s",
      error ? error->message : "Unknown error");
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
    g_clear_error(&error);
    return;
  }

  update_ui(self);
}

static void
on_reset_button_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);

  if (!self->manager)
    return;

  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  GtkAlertDialog *dialog = gtk_alert_dialog_new(
    "Reset master key?\n\nWARNING: This will create a new master key. "
    "All existing hardware-derived signing keys will become unusable!");
  gtk_alert_dialog_set_buttons(dialog, (const char *const[]){"Reset", "Cancel", NULL});

  gtk_alert_dialog_choose(dialog, parent, NULL, on_reset_confirm, self);
}

static void
on_delete_confirm(GObject *source, GAsyncResult *res, gpointer user_data)
{
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);
  GError *err = NULL;

  gint response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  if (response != 0)
    return; /* Not confirmed */

  GError *error = NULL;
  if (!hw_keystore_manager_delete_master_key(self->manager, &error)) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
    GtkAlertDialog *dialog = gtk_alert_dialog_new(
      "Failed to delete master key: %s",
      error ? error->message : "Unknown error");
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
    g_clear_error(&error);
    return;
  }

  update_ui(self);
}

static void
on_delete_button_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);

  if (!self->manager)
    return;

  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  GtkAlertDialog *dialog = gtk_alert_dialog_new(
    "Delete master key?\n\nWARNING: This will permanently delete the master key. "
    "All hardware-derived signing keys will become unusable and cannot be recovered!");
  gtk_alert_dialog_set_buttons(dialog, (const char *const[]){"Delete", "Cancel", NULL});

  gtk_alert_dialog_choose(dialog, parent, NULL, on_delete_confirm, self);
}

static void
on_mode_changed(HwKeystoreManager *manager, gint mode, gpointer user_data)
{
  (void)manager;
  (void)mode;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);
  update_ui(self);
}

static void
on_status_changed(HwKeystoreManager *manager, gint status, gpointer user_data)
{
  (void)manager;
  (void)status;
  HwKeystoreWidget *self = HW_KEYSTORE_WIDGET(user_data);
  update_ui(self);
}

/* ============================================================================
 * UI Update
 * ============================================================================ */

static void
update_ui(HwKeystoreWidget *self)
{
  if (!self->manager)
    return;

  gboolean hardware_available = hw_keystore_manager_is_hardware_available(self->manager);
  gboolean enabled = hw_keystore_manager_is_enabled(self->manager);
  gboolean has_master_key = hw_keystore_manager_has_master_key(self->manager);
  const gchar *backend_name = hw_keystore_manager_get_backend_name(self->manager);
  HwKeystoreSetupStatus setup_status = hw_keystore_manager_get_setup_status(self->manager);

  /* Update status icon and label */
  if (!hardware_available) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->status_icon), "dialog-warning-symbolic");
    gtk_widget_add_css_class(self->status_icon, "warning");
    gtk_widget_remove_css_class(self->status_icon, "success");
    gtk_label_set_text(GTK_LABEL(self->status_label), "No hardware keystore detected (software fallback available)");
  } else if (!enabled) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->status_icon), "security-low-symbolic");
    gtk_widget_remove_css_class(self->status_icon, "warning");
    gtk_widget_remove_css_class(self->status_icon, "success");
    gtk_label_set_text(GTK_LABEL(self->status_label), "Hardware keystore available but disabled");
  } else if (!has_master_key) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->status_icon), "security-medium-symbolic");
    gtk_widget_add_css_class(self->status_icon, "warning");
    gtk_widget_remove_css_class(self->status_icon, "success");
    gtk_label_set_text(GTK_LABEL(self->status_label), "Master key not created - setup required");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->status_icon), "security-high-symbolic");
    gtk_widget_add_css_class(self->status_icon, "success");
    gtk_widget_remove_css_class(self->status_icon, "warning");
    gtk_label_set_text(GTK_LABEL(self->status_label), "Hardware-backed signing ready");
  }

  /* Update enable switch (avoid triggering callback) */
  g_signal_handlers_block_by_func(self->enable_switch, on_enable_switch_toggled, self);
  gtk_switch_set_active(GTK_SWITCH(self->enable_switch), enabled);
  g_signal_handlers_unblock_by_func(self->enable_switch, on_enable_switch_toggled, self);

  /* Update details */
  gtk_label_set_text(GTK_LABEL(self->backend_value), backend_name);
  gtk_label_set_text(GTK_LABEL(self->status_detail_value),
                     hw_keystore_setup_status_to_string(setup_status));

  if (has_master_key) {
    gtk_label_set_text(GTK_LABEL(self->master_key_value), "Created (stored in hardware)");
    gtk_widget_add_css_class(self->master_key_value, "success");
  } else {
    gtk_label_set_text(GTK_LABEL(self->master_key_value), "Not created");
    gtk_widget_remove_css_class(self->master_key_value, "success");
  }

  /* Update button visibility */
  gtk_widget_set_visible(self->setup_button, !has_master_key);
  gtk_widget_set_visible(self->reset_button, has_master_key);
  gtk_widget_set_visible(self->delete_button, has_master_key);

  /* Update fallback switch */
  GnHsmProviderTpm *provider = hw_keystore_manager_get_provider(self->manager);
  if (provider) {
    gboolean fallback = gn_hsm_provider_tpm_get_fallback_enabled(provider);
    g_signal_handlers_block_by_func(self->fallback_switch, on_fallback_switch_toggled, self);
    gtk_switch_set_active(GTK_SWITCH(self->fallback_switch), fallback);
    g_signal_handlers_unblock_by_func(self->fallback_switch, on_fallback_switch_toggled, self);
  }

  /* Show/hide details based on enabled state */
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->details_revealer), enabled);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GtkWidget *
hw_keystore_widget_new(void)
{
  return hw_keystore_widget_new_with_manager(NULL);
}

GtkWidget *
hw_keystore_widget_new_with_manager(HwKeystoreManager *manager)
{
  HwKeystoreWidget *self = g_object_new(HW_TYPE_KEYSTORE_WIDGET, NULL);

  if (manager) {
    self->manager = g_object_ref(manager);
    self->owns_manager = FALSE;
  } else {
    self->manager = hw_keystore_manager_get_default();
    self->owns_manager = FALSE; /* default manager is singleton */
  }

  /* Connect signals */
  self->mode_changed_handler = g_signal_connect(
    self->manager, "mode-changed",
    G_CALLBACK(on_mode_changed), self);

  self->status_changed_handler = g_signal_connect(
    self->manager, "setup-status-changed",
    G_CALLBACK(on_status_changed), self);

  /* Load settings */
  SettingsManager *settings = settings_manager_get_default();
  gboolean enabled = settings_manager_get_hardware_keystore_enabled(settings);
  gboolean fallback = settings_manager_get_hardware_keystore_fallback(settings);

  /* Apply settings to manager */
  GnHsmProviderTpm *provider = hw_keystore_manager_get_provider(self->manager);
  if (provider) {
    gn_hsm_provider_tpm_set_fallback_enabled(provider, fallback);
  }
  if (enabled) {
    hw_keystore_manager_set_enabled(self->manager, TRUE);
  }

  /* Initial UI update */
  update_ui(self);

  return GTK_WIDGET(self);
}

HwKeystoreManager *
hw_keystore_widget_get_manager(HwKeystoreWidget *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_WIDGET(self), NULL);
  return self->manager;
}

void
hw_keystore_widget_refresh(HwKeystoreWidget *self)
{
  g_return_if_fail(HW_IS_KEYSTORE_WIDGET(self));
  update_ui(self);
}

void
hw_keystore_widget_set_expanded(HwKeystoreWidget *self, gboolean expanded)
{
  g_return_if_fail(HW_IS_KEYSTORE_WIDGET(self));
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->details_revealer), expanded);
}

gboolean
hw_keystore_widget_get_expanded(HwKeystoreWidget *self)
{
  g_return_val_if_fail(HW_IS_KEYSTORE_WIDGET(self), FALSE);
  return gtk_revealer_get_reveal_child(GTK_REVEALER(self->details_revealer));
}
