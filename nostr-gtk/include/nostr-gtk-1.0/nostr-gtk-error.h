/**
 * nostr-gtk-error.h - Error domain for nostr-gtk library
 *
 * Provides structured error reporting for nostr-gtk widget APIs,
 * enabling programmatic error handling in language bindings (Python,
 * Vala, JS via GIR) instead of silent g_warning() log messages.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOSTR_GTK_ERROR_H
#define NOSTR_GTK_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * NOSTR_GTK_ERROR:
 *
 * Error domain for nostr-gtk library errors.
 * Errors in this domain will be from the #NostrGtkError enumeration.
 * See #GError for more information on error domains.
 */
#define NOSTR_GTK_ERROR (nostr_gtk_error_quark())

GQuark nostr_gtk_error_quark(void);

/**
 * NostrGtkError:
 * @NOSTR_GTK_ERROR_RENDER_FAILED: Content rendering or markup generation failed
 * @NOSTR_GTK_ERROR_INVALID_INPUT: Invalid input (bad JSON, empty pubkey, NULL content)
 * @NOSTR_GTK_ERROR_LOAD_FAILED: Thread, profile, or media load/fetch failed
 * @NOSTR_GTK_ERROR_RESOURCE_MISSING: Blueprint template or icon resource not found
 * @NOSTR_GTK_ERROR_STORAGE_FAILED: NDB query or transaction failure from widget layer
 *
 * Error codes for the %NOSTR_GTK_ERROR error domain.
 */
typedef enum {
  NOSTR_GTK_ERROR_RENDER_FAILED,
  NOSTR_GTK_ERROR_INVALID_INPUT,
  NOSTR_GTK_ERROR_LOAD_FAILED,
  NOSTR_GTK_ERROR_RESOURCE_MISSING,
  NOSTR_GTK_ERROR_STORAGE_FAILED,
} NostrGtkError;

G_END_DECLS

#endif /* NOSTR_GTK_ERROR_H */
