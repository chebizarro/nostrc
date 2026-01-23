/* gn-secure-entry.h - Secure password entry widget
 *
 * A GTK4 widget for secure password entry with:
 * - Secure memory storage (zeroed on destruction)
 * - Clipboard copy/paste disabled for security
 * - Show/hide password toggle
 * - Password strength indicator
 * - Caps lock warning
 * - Minimum length indicator
 * - Auto-clear after configurable timeout (default 60s inactivity)
 * - Password requirements display
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_SECURE_ENTRY (gn_secure_entry_get_type())
G_DECLARE_FINAL_TYPE(GnSecureEntry, gn_secure_entry, GN, SECURE_ENTRY, GtkWidget)

/* Password strength levels */
typedef enum {
  GN_PASSWORD_STRENGTH_NONE = 0,
  GN_PASSWORD_STRENGTH_WEAK = 1,
  GN_PASSWORD_STRENGTH_FAIR = 2,
  GN_PASSWORD_STRENGTH_GOOD = 3,
  GN_PASSWORD_STRENGTH_STRONG = 4,
  GN_PASSWORD_STRENGTH_VERY_STRONG = 5
} GnPasswordStrength;

/**
 * gn_secure_entry_new:
 *
 * Creates a new secure password entry widget.
 *
 * Returns: (transfer full): A new #GnSecureEntry
 */
GtkWidget *gn_secure_entry_new(void);

/**
 * gn_secure_entry_get_text:
 * @self: A #GnSecureEntry
 *
 * Gets the current text from the secure entry.
 * The returned string is stored in secure memory and should
 * be cleared by the caller when no longer needed using
 * gn_secure_entry_free_text().
 *
 * Returns: (transfer full) (nullable): A newly allocated copy of the password
 *          stored in secure memory, or %NULL if empty
 */
gchar *gn_secure_entry_get_text(GnSecureEntry *self);

/**
 * gn_secure_entry_free_text:
 * @text: (transfer full) (nullable): Text previously returned by gn_secure_entry_get_text()
 *
 * Securely clears and frees text returned by gn_secure_entry_get_text().
 * This zeros the memory before freeing.
 */
void gn_secure_entry_free_text(gchar *text);

/**
 * gn_secure_entry_set_text:
 * @self: A #GnSecureEntry
 * @text: (nullable): The text to set
 *
 * Sets the text in the secure entry.
 */
void gn_secure_entry_set_text(GnSecureEntry *self, const gchar *text);

/**
 * gn_secure_entry_clear:
 * @self: A #GnSecureEntry
 *
 * Clears the secure entry, zeroing all internal buffers.
 */
void gn_secure_entry_clear(GnSecureEntry *self);

/**
 * gn_secure_entry_set_show_password:
 * @self: A #GnSecureEntry
 * @show: Whether to show the password
 *
 * Sets whether the password is visible.
 */
void gn_secure_entry_set_show_password(GnSecureEntry *self, gboolean show);

/**
 * gn_secure_entry_get_show_password:
 * @self: A #GnSecureEntry
 *
 * Gets whether the password is currently visible.
 *
 * Returns: %TRUE if the password is visible
 */
gboolean gn_secure_entry_get_show_password(GnSecureEntry *self);

/**
 * gn_secure_entry_set_placeholder_text:
 * @self: A #GnSecureEntry
 * @text: (nullable): The placeholder text
 *
 * Sets the placeholder text shown when the entry is empty.
 */
void gn_secure_entry_set_placeholder_text(GnSecureEntry *self, const gchar *text);

/**
 * gn_secure_entry_get_placeholder_text:
 * @self: A #GnSecureEntry
 *
 * Gets the placeholder text.
 *
 * Returns: (nullable): The placeholder text
 */
const gchar *gn_secure_entry_get_placeholder_text(GnSecureEntry *self);

/**
 * gn_secure_entry_set_min_length:
 * @self: A #GnSecureEntry
 * @min_length: Minimum required length (0 to disable)
 *
 * Sets the minimum required password length.
 */
void gn_secure_entry_set_min_length(GnSecureEntry *self, guint min_length);

/**
 * gn_secure_entry_get_min_length:
 * @self: A #GnSecureEntry
 *
 * Gets the minimum required password length.
 *
 * Returns: The minimum length, or 0 if not set
 */
guint gn_secure_entry_get_min_length(GnSecureEntry *self);

/**
 * gn_secure_entry_set_timeout:
 * @self: A #GnSecureEntry
 * @timeout_seconds: Timeout in seconds (0 to disable auto-clear)
 *
 * Sets the inactivity timeout after which the entry is automatically cleared.
 * Default is 60 seconds.
 */
void gn_secure_entry_set_timeout(GnSecureEntry *self, guint timeout_seconds);

/**
 * gn_secure_entry_get_timeout:
 * @self: A #GnSecureEntry
 *
 * Gets the inactivity timeout.
 *
 * Returns: The timeout in seconds, or 0 if disabled
 */
guint gn_secure_entry_get_timeout(GnSecureEntry *self);

/**
 * gn_secure_entry_get_strength:
 * @self: A #GnSecureEntry
 *
 * Gets the current password strength.
 *
 * Returns: The password strength level
 */
GnPasswordStrength gn_secure_entry_get_strength(GnSecureEntry *self);

/**
 * gn_secure_entry_get_strength_text:
 * @self: A #GnSecureEntry
 *
 * Gets a human-readable description of the current password strength.
 *
 * Returns: A localized strength description string
 */
const gchar *gn_secure_entry_get_strength_text(GnSecureEntry *self);

/**
 * gn_secure_entry_meets_requirements:
 * @self: A #GnSecureEntry
 *
 * Checks if the current password meets all configured requirements.
 *
 * Returns: %TRUE if the password meets requirements
 */
gboolean gn_secure_entry_meets_requirements(GnSecureEntry *self);

/**
 * gn_secure_entry_set_show_strength_indicator:
 * @self: A #GnSecureEntry
 * @show: Whether to show the strength indicator
 *
 * Sets whether to show the password strength indicator.
 */
void gn_secure_entry_set_show_strength_indicator(GnSecureEntry *self, gboolean show);

/**
 * gn_secure_entry_get_show_strength_indicator:
 * @self: A #GnSecureEntry
 *
 * Gets whether the strength indicator is shown.
 *
 * Returns: %TRUE if the strength indicator is visible
 */
gboolean gn_secure_entry_get_show_strength_indicator(GnSecureEntry *self);

/**
 * gn_secure_entry_set_show_caps_warning:
 * @self: A #GnSecureEntry
 * @show: Whether to show caps lock warning
 *
 * Sets whether to show a warning when caps lock is active.
 */
void gn_secure_entry_set_show_caps_warning(GnSecureEntry *self, gboolean show);

/**
 * gn_secure_entry_get_show_caps_warning:
 * @self: A #GnSecureEntry
 *
 * Gets whether the caps lock warning is enabled.
 *
 * Returns: %TRUE if caps lock warning is enabled
 */
gboolean gn_secure_entry_get_show_caps_warning(GnSecureEntry *self);

/**
 * gn_secure_entry_set_requirements_text:
 * @self: A #GnSecureEntry
 * @text: (nullable): The requirements text to display
 *
 * Sets a custom requirements description text.
 */
void gn_secure_entry_set_requirements_text(GnSecureEntry *self, const gchar *text);

/**
 * gn_secure_entry_get_requirements_text:
 * @self: A #GnSecureEntry
 *
 * Gets the custom requirements text.
 *
 * Returns: (nullable): The requirements text
 */
const gchar *gn_secure_entry_get_requirements_text(GnSecureEntry *self);

/**
 * gn_secure_entry_reset_timeout:
 * @self: A #GnSecureEntry
 *
 * Resets the auto-clear timeout counter.
 * Call this when the user interacts with related UI elements.
 */
void gn_secure_entry_reset_timeout(GnSecureEntry *self);

/**
 * gn_secure_entry_grab_focus:
 * @self: A #GnSecureEntry
 *
 * Sets keyboard focus to the entry.
 *
 * Returns: %TRUE if focus was successfully grabbed
 */
gboolean gn_secure_entry_grab_focus_entry(GnSecureEntry *self);

G_END_DECLS
