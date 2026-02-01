/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-row.c - Plugin list row widget implementation
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-row.h"
#include <adwaita.h>

struct _GnostrPluginRow
{
  GtkWidget parent_instance;

  /* Template widgets */
  GtkCheckButton *chk_enabled;
  GtkImage       *plugin_icon;
  GtkLabel       *lbl_name;
  GtkLabel       *lbl_version;
  GtkLabel       *lbl_description;
  GtkBox         *status_row;
  GtkImage       *status_icon;
  GtkLabel       *lbl_status;
  GtkButton      *btn_settings;
  GtkButton      *btn_info;

  /* Data */
  PeasPluginInfo    *info;
  GnostrPluginState  state;
  gboolean           has_settings;
};

G_DEFINE_TYPE(GnostrPluginRow, gnostr_plugin_row, GTK_TYPE_WIDGET)

enum
{
  PROP_0,
  PROP_PLUGIN_INFO,
  PROP_ENABLED,
  PROP_STATE,
  PROP_HAS_SETTINGS,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
  SIGNAL_TOGGLED,
  SIGNAL_SETTINGS_CLICKED,
  SIGNAL_INFO_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_enabled_toggled(GtkCheckButton *button, GnostrPluginRow *self);
static void on_settings_clicked(GtkButton *button, GnostrPluginRow *self);
static void on_info_clicked(GtkButton *button, GnostrPluginRow *self);
static void update_state_display(GnostrPluginRow *self);

static void
gnostr_plugin_row_dispose(GObject *object)
{
  GnostrPluginRow *self = GNOSTR_PLUGIN_ROW(object);

  g_clear_object(&self->info);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PLUGIN_ROW);

  G_OBJECT_CLASS(gnostr_plugin_row_parent_class)->dispose(object);
}

static void
gnostr_plugin_row_set_property(GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  GnostrPluginRow *self = GNOSTR_PLUGIN_ROW(object);

  switch (prop_id)
    {
    case PROP_PLUGIN_INFO:
      g_clear_object(&self->info);
      self->info = g_value_dup_object(value);
      gnostr_plugin_row_update_from_info(self);
      break;

    case PROP_ENABLED:
      gnostr_plugin_row_set_enabled(self, g_value_get_boolean(value));
      break;

    case PROP_STATE:
      gnostr_plugin_row_set_state(self, g_value_get_enum(value));
      break;

    case PROP_HAS_SETTINGS:
      gnostr_plugin_row_set_has_settings(self, g_value_get_boolean(value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gnostr_plugin_row_get_property(GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  GnostrPluginRow *self = GNOSTR_PLUGIN_ROW(object);

  switch (prop_id)
    {
    case PROP_PLUGIN_INFO:
      g_value_set_object(value, self->info);
      break;

    case PROP_ENABLED:
      g_value_set_boolean(value, gnostr_plugin_row_get_enabled(self));
      break;

    case PROP_STATE:
      g_value_set_enum(value, self->state);
      break;

    case PROP_HAS_SETTINGS:
      g_value_set_boolean(value, self->has_settings);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gnostr_plugin_row_class_init(GnostrPluginRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_plugin_row_dispose;
  object_class->set_property = gnostr_plugin_row_set_property;
  object_class->get_property = gnostr_plugin_row_get_property;

  /**
   * GnostrPluginRow:plugin-info:
   *
   * The PeasPluginInfo for this row.
   */
  properties[PROP_PLUGIN_INFO] =
    g_param_spec_object("plugin-info",
                        "Plugin Info",
                        "The PeasPluginInfo for this row",
                        PEAS_TYPE_PLUGIN_INFO,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GnostrPluginRow:enabled:
   *
   * Whether the plugin is enabled.
   */
  properties[PROP_ENABLED] =
    g_param_spec_boolean("enabled",
                         "Enabled",
                         "Whether the plugin is enabled",
                         FALSE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GnostrPluginRow:state:
   *
   * The plugin state.
   */
  properties[PROP_STATE] =
    g_param_spec_enum("state",
                      "State",
                      "The plugin state",
                      /* TODO: Register GnostrPluginState as GEnum */
                      G_TYPE_INT,
                      GNOSTR_PLUGIN_STATE_UNLOADED,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GnostrPluginRow:has-settings:
   *
   * Whether the plugin has a settings page.
   */
  properties[PROP_HAS_SETTINGS] =
    g_param_spec_boolean("has-settings",
                         "Has Settings",
                         "Whether the plugin has a settings page",
                         FALSE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  /**
   * GnostrPluginRow::toggled:
   * @self: The plugin row
   * @enabled: The new enabled state
   *
   * Emitted when the enable checkbox is toggled.
   */
  signals[SIGNAL_TOGGLED] =
    g_signal_new("toggled",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /**
   * GnostrPluginRow::settings-clicked:
   * @self: The plugin row
   *
   * Emitted when the settings button is clicked.
   */
  signals[SIGNAL_SETTINGS_CLICKED] =
    g_signal_new("settings-clicked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /**
   * GnostrPluginRow::info-clicked:
   * @self: The plugin row
   *
   * Emitted when the info button is clicked.
   */
  signals[SIGNAL_INFO_CLICKED] =
    g_signal_new("info-clicked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /* Load UI template */
  gtk_widget_class_set_template_from_resource(
    widget_class,
    "/org/gnostr/ui/ui/widgets/gnostr-plugin-row.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, chk_enabled);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, plugin_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, lbl_name);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, lbl_version);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, lbl_description);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, status_row);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, status_icon);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, lbl_status);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginRow, btn_info);

  /* Template callbacks */
  gtk_widget_class_bind_template_callback(widget_class, on_enabled_toggled);
  gtk_widget_class_bind_template_callback(widget_class, on_settings_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_info_clicked);

  /* Set CSS name */
  gtk_widget_class_set_css_name(widget_class, "plugin-row");

  /* Layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
}

static void
gnostr_plugin_row_init(GnostrPluginRow *self)
{
  self->state = GNOSTR_PLUGIN_STATE_UNLOADED;
  self->has_settings = FALSE;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  g_signal_connect(self->chk_enabled, "toggled",
                   G_CALLBACK(on_enabled_toggled), self);
  g_signal_connect(self->btn_settings, "clicked",
                   G_CALLBACK(on_settings_clicked), self);
  g_signal_connect(self->btn_info, "clicked",
                   G_CALLBACK(on_info_clicked), self);
}

static void
on_enabled_toggled(GtkCheckButton *button, GnostrPluginRow *self)
{
  gboolean enabled = gtk_check_button_get_active(button);
  g_signal_emit(self, signals[SIGNAL_TOGGLED], 0, enabled);
}

static void
on_settings_clicked(GtkButton *button, GnostrPluginRow *self)
{
  g_signal_emit(self, signals[SIGNAL_SETTINGS_CLICKED], 0);
}

static void
on_info_clicked(GtkButton *button, GnostrPluginRow *self)
{
  g_signal_emit(self, signals[SIGNAL_INFO_CLICKED], 0);
}

static void
update_state_display(GnostrPluginRow *self)
{
  GtkWidget *widget = GTK_WIDGET(self);
  const char *status_msg = NULL;
  const char *icon_name = NULL;

  /* Remove all state classes */
  gtk_widget_remove_css_class(widget, "disabled");
  gtk_widget_remove_css_class(widget, "error");
  gtk_widget_remove_css_class(widget, "needs-restart");

  switch (self->state)
    {
    case GNOSTR_PLUGIN_STATE_UNLOADED:
      gtk_widget_add_css_class(widget, "disabled");
      break;

    case GNOSTR_PLUGIN_STATE_LOADED:
      gtk_widget_add_css_class(widget, "disabled");
      break;

    case GNOSTR_PLUGIN_STATE_ACTIVE:
      /* No special class */
      break;

    case GNOSTR_PLUGIN_STATE_ERROR:
      gtk_widget_add_css_class(widget, "error");
      icon_name = "dialog-error-symbolic";
      status_msg = "Error loading plugin";
      break;

    case GNOSTR_PLUGIN_STATE_NEEDS_RESTART:
      gtk_widget_add_css_class(widget, "needs-restart");
      icon_name = "dialog-warning-symbolic";
      status_msg = "Restart required";
      break;

    case GNOSTR_PLUGIN_STATE_INCOMPATIBLE:
      gtk_widget_add_css_class(widget, "error");
      icon_name = "dialog-error-symbolic";
      status_msg = "Incompatible plugin version";
      break;

    default:
      break;
    }

  /* Update status row visibility and content */
  gtk_widget_set_visible(GTK_WIDGET(self->status_row), status_msg != NULL);
  if (status_msg != NULL)
    {
      gtk_label_set_text(self->lbl_status, status_msg);
      gtk_image_set_from_icon_name(self->status_icon, icon_name);
    }
}

/* Public API */

GtkWidget *
gnostr_plugin_row_new(PeasPluginInfo *info)
{
  return g_object_new(GNOSTR_TYPE_PLUGIN_ROW,
                      "plugin-info", info,
                      NULL);
}

PeasPluginInfo *
gnostr_plugin_row_get_plugin_info(GnostrPluginRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_ROW(self), NULL);
  return self->info;
}

void
gnostr_plugin_row_set_enabled(GnostrPluginRow *self, gboolean enabled)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_ROW(self));

  /* Block signal to avoid feedback loop */
  g_signal_handlers_block_by_func(self->chk_enabled, on_enabled_toggled, self);
  gtk_check_button_set_active(self->chk_enabled, enabled);
  g_signal_handlers_unblock_by_func(self->chk_enabled, on_enabled_toggled, self);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ENABLED]);
}

gboolean
gnostr_plugin_row_get_enabled(GnostrPluginRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_ROW(self), FALSE);
  return gtk_check_button_get_active(self->chk_enabled);
}

void
gnostr_plugin_row_set_state(GnostrPluginRow *self, GnostrPluginState state)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_ROW(self));

  if (self->state != state)
    {
      self->state = state;
      update_state_display(self);
      g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    }
}

GnostrPluginState
gnostr_plugin_row_get_state(GnostrPluginRow *self)
{
  g_return_val_if_fail(GNOSTR_IS_PLUGIN_ROW(self), GNOSTR_PLUGIN_STATE_UNLOADED);
  return self->state;
}

void
gnostr_plugin_row_set_has_settings(GnostrPluginRow *self, gboolean has_settings)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_ROW(self));

  if (self->has_settings != has_settings)
    {
      self->has_settings = has_settings;
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_settings), has_settings);
      g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_HAS_SETTINGS]);
    }
}

void
gnostr_plugin_row_set_status_message(GnostrPluginRow *self, const char *message)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_ROW(self));

  if (message != NULL && *message != '\0')
    {
      gtk_label_set_text(self->lbl_status, message);
      gtk_widget_set_visible(GTK_WIDGET(self->status_row), TRUE);
    }
  else
    {
      gtk_widget_set_visible(GTK_WIDGET(self->status_row), FALSE);
    }
}

void
gnostr_plugin_row_update_from_info(GnostrPluginRow *self)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_ROW(self));

  if (self->info == NULL)
    return;

  const char *name = peas_plugin_info_get_name(self->info);
  const char *desc = peas_plugin_info_get_description(self->info);
  const char *version = peas_plugin_info_get_version(self->info);
  const char *icon_name = peas_plugin_info_get_icon_name(self->info);

  gtk_label_set_text(self->lbl_name, name ? name : "Unknown Plugin");
  gtk_label_set_text(self->lbl_description, desc ? desc : "No description available");

  if (version != NULL)
    {
      g_autofree char *version_str = g_strdup_printf("v%s", version);
      gtk_label_set_text(self->lbl_version, version_str);
    }
  else
    {
      gtk_label_set_text(self->lbl_version, "");
    }

  if (icon_name != NULL)
    {
      gtk_image_set_from_icon_name(self->plugin_icon, icon_name);
    }
  else
    {
      gtk_image_set_from_icon_name(self->plugin_icon, "application-x-addon-symbolic");
    }

  /* Update enabled state from peas */
  gboolean loaded = peas_plugin_info_is_loaded(self->info);
  gnostr_plugin_row_set_enabled(self, loaded);

  /* Update state based on plugin info */
  if (!peas_plugin_info_is_available(self->info, NULL))
    {
      gnostr_plugin_row_set_state(self, GNOSTR_PLUGIN_STATE_ERROR);
    }
  else if (loaded)
    {
      gnostr_plugin_row_set_state(self, GNOSTR_PLUGIN_STATE_ACTIVE);
    }
  else
    {
      gnostr_plugin_row_set_state(self, GNOSTR_PLUGIN_STATE_UNLOADED);
    }
}
