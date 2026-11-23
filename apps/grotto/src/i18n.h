/* i18n.h - Internationalization support for grotto
 *
 * Provides gettext macros for translation support.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_I18N_H
#define APPS_GNOSTR_SIGNER_I18N_H

/* Bind presentation strings to Grotto even in embedded/test consumers. */
#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "grotto"
#endif
#include <glib/gi18n-lib.h>

/**
 * gn_i18n_init:
 *
 * Initialize internationalization support.
 * Call this early in main() before any translated strings are used.
 */
void gn_i18n_init(void);

/**
 * gn_i18n_get_language:
 *
 * Gets the current language code.
 *
 * Returns: (transfer none): The current language code (e.g., "ja", "es")
 */
const gchar *gn_i18n_get_language(void);

/**
 * gn_i18n_is_rtl:
 * @code: Language code (e.g., "fa")
 *
 * Checks if a language uses right-to-left text direction.
 *
 * Returns: %TRUE if RTL, %FALSE otherwise
 */
gboolean gn_i18n_is_rtl(const gchar *code);

/**
 * gn_i18n_is_current_rtl:
 *
 * Checks if the current language uses right-to-left text direction.
 *
 * Returns: %TRUE if RTL, %FALSE otherwise
 */
gboolean gn_i18n_is_current_rtl(void);

/**
 * gn_i18n_apply_text_direction:
 *
 * Apply the appropriate text direction (RTL/LTR) based on current language.
 * Call this after changing the language to update the UI direction.
 */
void gn_i18n_apply_text_direction(void);
#endif /* APPS_GNOSTR_SIGNER_I18N_H */
