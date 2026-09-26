/* nm_errors.c - NIP-07 bridge error vocabulary (nostrc-jjyp) */
#include "nm_errors.h"

#include <gio/gio.h>

static const struct {
  const gchar *code;
  const gchar *message;
} table[NM_ERR_N_CODES] = {
  [NM_ERR_INVALID_REQUEST]    = { "invalid_request",    "Invalid request" },
  [NM_ERR_UNKNOWN_METHOD]     = { "unknown_method",     "Unknown method" },
  [NM_ERR_ORIGIN_DENIED]      = { "origin_denied",      "Only secure (https or localhost) origins may use the signer" },
  [NM_ERR_REJECTED]           = { "rejected",           "The user rejected the request" },
  [NM_ERR_RATE_LIMITED]       = { "rate_limited",       "Too many requests; try again shortly" },
  [NM_ERR_NO_KEY]             = { "no_key",             "No Nostr identity is configured in the signer" },
  [NM_ERR_NOT_FOUND]          = { "not_found",          "Not found" },
  [NM_ERR_SIGNER_UNAVAILABLE] = { "signer_unavailable", "The desktop signer (org.nostr.Signer) is not running" },
  [NM_ERR_TIMEOUT]            = { "timeout",            "The signer did not answer in time" },
  [NM_ERR_BUSY]               = { "busy",               "Too many requests in flight" },
  [NM_ERR_TOO_LARGE]          = { "too_large",          "Message too large" },
  [NM_ERR_UNSUPPORTED]        = { "unsupported",        "Not supported" },
  [NM_ERR_INTERNAL]           = { "internal",           "Internal signer error" },
};

const gchar *nm_error_code_str(NmErrorCode code) {
  if ((guint)code >= NM_ERR_N_CODES) code = NM_ERR_INTERNAL;
  return table[code].code;
}

const gchar *nm_error_default_message(NmErrorCode code) {
  if ((guint)code >= NM_ERR_N_CODES) code = NM_ERR_INTERNAL;
  return table[code].message;
}

static const struct {
  const gchar *name;
  NmErrorCode code;
} remote_map[] = {
  /* org.nostr.Signer (nips/nip55l/include/nip55l_dbus_errors.h) */
  { "org.nostr.Signer.Error.ApprovalDenied",   NM_ERR_REJECTED },
  { "org.nostr.Signer.Error.PermissionDenied", NM_ERR_REJECTED },
  { "org.nostr.Signer.Error.RateLimited",      NM_ERR_RATE_LIMITED },
  { "org.nostr.Signer.Error.InvalidInput",     NM_ERR_INVALID_REQUEST },
  { "org.nostr.Signer.Error.NoKeyConfigured",  NM_ERR_NO_KEY },
  { "org.nostr.Signer.Error.NotFound",         NM_ERR_NOT_FOUND },
  { "org.nostr.Signer.Error.InvalidConfig",    NM_ERR_INTERNAL },
  { "org.nostr.Signer.Error.Internal",         NM_ERR_INTERNAL },
  /* bus-level */
  { "org.freedesktop.DBus.Error.ServiceUnknown",   NM_ERR_SIGNER_UNAVAILABLE },
  { "org.freedesktop.DBus.Error.NameHasNoOwner",   NM_ERR_SIGNER_UNAVAILABLE },
  { "org.freedesktop.DBus.Error.Disconnected",     NM_ERR_SIGNER_UNAVAILABLE },
  { "org.freedesktop.DBus.Error.NoReply",          NM_ERR_TIMEOUT },
  { "org.freedesktop.DBus.Error.Timeout",          NM_ERR_TIMEOUT },
  { "org.freedesktop.DBus.Error.TimedOut",         NM_ERR_TIMEOUT },
  { "org.freedesktop.DBus.Error.UnknownMethod",    NM_ERR_UNSUPPORTED },
  { "org.freedesktop.DBus.Error.UnknownInterface", NM_ERR_UNSUPPORTED },
  { "org.freedesktop.DBus.Error.UnknownObject",    NM_ERR_UNSUPPORTED },
  { "org.freedesktop.DBus.Error.AccessDenied",     NM_ERR_REJECTED },
  { "org.freedesktop.DBus.Error.LimitsExceeded",   NM_ERR_RATE_LIMITED },
};

NmErrorCode nm_error_from_dbus(const GError *error) {
  if (!error) return NM_ERR_INTERNAL;

  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  if (remote) {
    for (gsize i = 0; i < G_N_ELEMENTS(remote_map); i++)
      if (g_strcmp0(remote, remote_map[i].name) == 0) return remote_map[i].code;
    if (g_str_has_prefix(remote, "org.freedesktop.DBus.Error.Spawn."))
      return NM_ERR_SIGNER_UNAVAILABLE;
    return NM_ERR_INTERNAL;
  }

  /* Local (non-remote) errors: GDBus maps well-known bus errors onto
   * G_IO_ERROR / G_DBUS_ERROR codes without a remote name. */
  if (error->domain == G_IO_ERROR) {
    switch (error->code) {
      case G_IO_ERROR_TIMED_OUT:    return NM_ERR_TIMEOUT;
      case G_IO_ERROR_CANCELLED:    return NM_ERR_SIGNER_UNAVAILABLE;
      case G_IO_ERROR_CLOSED:       return NM_ERR_SIGNER_UNAVAILABLE;
      case G_IO_ERROR_NOT_FOUND:    return NM_ERR_SIGNER_UNAVAILABLE;
      case G_IO_ERROR_DBUS_ERROR:   return NM_ERR_INTERNAL;
      default:                      break;
    }
  }
  if (error->domain == G_DBUS_ERROR) {
    switch (error->code) {
      case G_DBUS_ERROR_SERVICE_UNKNOWN:
      case G_DBUS_ERROR_NAME_HAS_NO_OWNER:
      case G_DBUS_ERROR_DISCONNECTED:
      case G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND:
      case G_DBUS_ERROR_SPAWN_EXEC_FAILED:
      case G_DBUS_ERROR_SPAWN_CHILD_EXITED:
        return NM_ERR_SIGNER_UNAVAILABLE;
      case G_DBUS_ERROR_NO_REPLY:
      case G_DBUS_ERROR_TIMEOUT:
      case G_DBUS_ERROR_TIMED_OUT:
        return NM_ERR_TIMEOUT;
      case G_DBUS_ERROR_UNKNOWN_METHOD:
      case G_DBUS_ERROR_UNKNOWN_INTERFACE:
      case G_DBUS_ERROR_UNKNOWN_OBJECT:
        return NM_ERR_UNSUPPORTED;
      case G_DBUS_ERROR_ACCESS_DENIED:
        return NM_ERR_REJECTED;
      case G_DBUS_ERROR_LIMITS_EXCEEDED:
        return NM_ERR_RATE_LIMITED;
      default:
        break;
    }
  }
  return NM_ERR_INTERNAL;
}
