/* hardware_keystore_widget.h - Hardware Keystore Settings Widget
 *
 * GTK4 widget for configuring hardware-backed keystore settings.
 * Provides UI for:
 * - Enabling/disabling hardware keystore
 * - Viewing hardware status and capabilities
 * - Setting up master key
 * - Configuring fallback options
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gtk/gtk.h>
#include "../hw_keystore_manager.h"

G_BEGIN_DECLS

#define HW_TYPE_KEYSTORE_WIDGET (hw_keystore_widget_get_type())

G_DECLARE_FINAL_TYPE(HwKeystoreWidget, hw_keystore_widget, HW, KEYSTORE_WIDGET, GtkBox)

/**
 * hw_keystore_widget_new:
 *
 * Creates a new hardware keystore settings widget.
 *
 * Returns: (transfer floating): A new #HwKeystoreWidget
 */
GtkWidget *hw_keystore_widget_new(void);

/**
 * hw_keystore_widget_new_with_manager:
 * @manager: (transfer none): A #HwKeystoreManager to use
 *
 * Creates a new hardware keystore settings widget with a specific manager.
 *
 * Returns: (transfer floating): A new #HwKeystoreWidget
 */
GtkWidget *hw_keystore_widget_new_with_manager(HwKeystoreManager *manager);

/**
 * hw_keystore_widget_get_manager:
 * @self: A #HwKeystoreWidget
 *
 * Gets the keystore manager used by this widget.
 *
 * Returns: (transfer none): The #HwKeystoreManager
 */
HwKeystoreManager *hw_keystore_widget_get_manager(HwKeystoreWidget *self);

/**
 * hw_keystore_widget_refresh:
 * @self: A #HwKeystoreWidget
 *
 * Refreshes the widget to reflect current hardware state.
 */
void hw_keystore_widget_refresh(HwKeystoreWidget *self);

/**
 * hw_keystore_widget_set_expanded:
 * @self: A #HwKeystoreWidget
 * @expanded: Whether to expand the details
 *
 * Expands or collapses the hardware details section.
 */
void hw_keystore_widget_set_expanded(HwKeystoreWidget *self, gboolean expanded);

/**
 * hw_keystore_widget_get_expanded:
 * @self: A #HwKeystoreWidget
 *
 * Gets whether the details section is expanded.
 *
 * Returns: %TRUE if expanded
 */
gboolean hw_keystore_widget_get_expanded(HwKeystoreWidget *self);

G_END_DECLS
