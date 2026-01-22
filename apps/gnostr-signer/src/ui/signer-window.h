#pragma once

#include <adwaita.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define TYPE_SIGNER_WINDOW (signer_window_get_type())
G_DECLARE_FINAL_TYPE(SignerWindow, signer_window, SIGNER, WINDOW, AdwApplicationWindow)

SignerWindow *signer_window_new(AdwApplication *app);
void signer_window_show_page(SignerWindow *self, const char *name);

/**
 * signer_window_get_gsettings:
 * @self: a #SignerWindow
 *
 * Returns the #GSettings instance used by this window for persistence.
 *
 * Returns: (transfer none) (nullable): the #GSettings instance
 */
GSettings *signer_window_get_gsettings(SignerWindow *self);

/**
 * signer_get_app_settings:
 *
 * Gets or creates a #GSettings instance for the signer app.
 * Caller owns the returned reference and must free with g_object_unref().
 *
 * Returns: (transfer full) (nullable): a #GSettings instance, or %NULL if schema not available
 */
GSettings *signer_get_app_settings(void);

G_END_DECLS
