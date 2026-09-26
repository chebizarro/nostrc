/* test_nm_errors — D-Bus error -> NIP-07 bridge error mapping (nostrc-jjyp) */
#include "nm_errors.h"

#include <gio/gio.h>

static NmErrorCode map_remote(const gchar *name) {
  g_autoptr(GError) e = g_dbus_error_new_for_dbus_error(name, "daemon said no");
  return nm_error_from_dbus(e);
}

static void test_signer_errors(void) {
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.ApprovalDenied"), ==, NM_ERR_REJECTED);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.PermissionDenied"), ==, NM_ERR_REJECTED);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.RateLimited"), ==, NM_ERR_RATE_LIMITED);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.InvalidInput"), ==, NM_ERR_INVALID_REQUEST);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.NoKeyConfigured"), ==, NM_ERR_NO_KEY);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.NotFound"), ==, NM_ERR_NOT_FOUND);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.InvalidConfig"), ==, NM_ERR_INTERNAL);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.Internal"), ==, NM_ERR_INTERNAL);
  g_assert_cmpint(map_remote("org.nostr.Signer.Error.SomethingNew"), ==, NM_ERR_INTERNAL);
}

static void test_bus_errors(void) {
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.ServiceUnknown"), ==, NM_ERR_SIGNER_UNAVAILABLE);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.NameHasNoOwner"), ==, NM_ERR_SIGNER_UNAVAILABLE);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.Spawn.ChildExited"), ==, NM_ERR_SIGNER_UNAVAILABLE);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.NoReply"), ==, NM_ERR_TIMEOUT);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.Timeout"), ==, NM_ERR_TIMEOUT);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.UnknownMethod"), ==, NM_ERR_UNSUPPORTED);
  g_assert_cmpint(map_remote("org.freedesktop.DBus.Error.AccessDenied"), ==, NM_ERR_REJECTED);

  g_autoptr(GError) t = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Timeout was reached");
  g_assert_cmpint(nm_error_from_dbus(t), ==, NM_ERR_TIMEOUT);
  g_autoptr(GError) c = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "closed");
  g_assert_cmpint(nm_error_from_dbus(c), ==, NM_ERR_SIGNER_UNAVAILABLE);
  g_autoptr(GError) su = g_error_new_literal(G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN, "x");
  g_assert_cmpint(nm_error_from_dbus(su), ==, NM_ERR_SIGNER_UNAVAILABLE);
  g_autoptr(GError) other = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "x");
  g_assert_cmpint(nm_error_from_dbus(other), ==, NM_ERR_INTERNAL);
  g_assert_cmpint(nm_error_from_dbus(NULL), ==, NM_ERR_INTERNAL);
}

static void test_wire_strings(void) {
  g_assert_cmpstr(nm_error_code_str(NM_ERR_REJECTED), ==, "rejected");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_ORIGIN_DENIED), ==, "origin_denied");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_SIGNER_UNAVAILABLE), ==, "signer_unavailable");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_TIMEOUT), ==, "timeout");
  g_assert_cmpstr(nm_error_code_str((NmErrorCode)999), ==, "internal");
  for (int c = 0; c < NM_ERR_N_CODES; c++) {
    g_assert_nonnull(nm_error_code_str((NmErrorCode)c));
    g_assert_nonnull(nm_error_default_message((NmErrorCode)c));
  }
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nmh/errors/signer", test_signer_errors);
  g_test_add_func("/nmh/errors/bus", test_bus_errors);
  g_test_add_func("/nmh/errors/wire-strings", test_wire_strings);
  return g_test_run();
}
