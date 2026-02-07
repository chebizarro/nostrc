/* gn-signer-error.c - Error domain implementation for gnostr-signer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gn-signer-error.h"

G_DEFINE_QUARK(gn-signer-error-quark, gn_signer_error)

const gchar *
gn_signer_error_to_string(GnSignerError code)
{
  switch (code) {
    case GN_SIGNER_ERROR_INVALID_INPUT:
      return "Invalid input";
    case GN_SIGNER_ERROR_NOT_FOUND:
      return "Not found";
    case GN_SIGNER_ERROR_ALREADY_EXISTS:
      return "Already exists";
    case GN_SIGNER_ERROR_STORAGE_FAILED:
      return "Storage operation failed";
    case GN_SIGNER_ERROR_CRYPTO_FAILED:
      return "Cryptographic operation failed";
    case GN_SIGNER_ERROR_BACKEND_FAILED:
      return "Backend service failed";
    case GN_SIGNER_ERROR_PERMISSION_DENIED:
      return "Permission denied";
    case GN_SIGNER_ERROR_NOT_SUPPORTED:
      return "Not supported";
    case GN_SIGNER_ERROR_INTERNAL:
      return "Internal error";
    default:
      return "Unknown error";
  }
}
