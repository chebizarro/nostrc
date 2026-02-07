/* gn-signer-error.h - Central error domain for gnostr-signer
 *
 * This module defines the GN_SIGNER_ERROR domain for consistent error
 * handling throughout the gnostr-signer application. Use this for
 * general application errors. Module-specific errors (backup, key provider,
 * secret store) have their own domains.
 *
 * Usage:
 *   g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
 *                       "Account ID cannot be empty");
 *
 *   g_propagate_error(error, local_error);
 *
 *   g_prefix_error(error, "Failed to import key: ");
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * GN_SIGNER_ERROR:
 *
 * Error domain for gnostr-signer application errors.
 * Errors in this domain will be from the #GnSignerError enumeration.
 */
#define GN_SIGNER_ERROR (gn_signer_error_quark())
GQuark gn_signer_error_quark(void);

/**
 * GnSignerError:
 * @GN_SIGNER_ERROR_INVALID_INPUT: Invalid input parameter (NULL, empty, malformed)
 * @GN_SIGNER_ERROR_NOT_FOUND: Requested resource not found (account, key, etc.)
 * @GN_SIGNER_ERROR_ALREADY_EXISTS: Resource already exists (duplicate account)
 * @GN_SIGNER_ERROR_STORAGE_FAILED: Storage operation failed (file I/O, database)
 * @GN_SIGNER_ERROR_CRYPTO_FAILED: Cryptographic operation failed
 * @GN_SIGNER_ERROR_BACKEND_FAILED: Backend service failed (secret store, HSM)
 * @GN_SIGNER_ERROR_PERMISSION_DENIED: Operation not permitted
 * @GN_SIGNER_ERROR_NOT_SUPPORTED: Feature or operation not supported
 * @GN_SIGNER_ERROR_INTERNAL: Internal error (should not happen)
 *
 * Error codes for gnostr-signer operations.
 */
typedef enum {
  GN_SIGNER_ERROR_INVALID_INPUT,
  GN_SIGNER_ERROR_NOT_FOUND,
  GN_SIGNER_ERROR_ALREADY_EXISTS,
  GN_SIGNER_ERROR_STORAGE_FAILED,
  GN_SIGNER_ERROR_CRYPTO_FAILED,
  GN_SIGNER_ERROR_BACKEND_FAILED,
  GN_SIGNER_ERROR_PERMISSION_DENIED,
  GN_SIGNER_ERROR_NOT_SUPPORTED,
  GN_SIGNER_ERROR_INTERNAL
} GnSignerError;

/**
 * gn_signer_error_to_string:
 * @code: A #GnSignerError code
 *
 * Get a human-readable string for an error code.
 *
 * Returns: (transfer none): Static string describing the error
 */
const gchar *gn_signer_error_to_string(GnSignerError code);

G_END_DECLS
