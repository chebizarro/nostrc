/* test_caller.c - caller identity parsing (Flatpak info, snap/systemd cgroups).
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-caller.h"

#include <glib.h>

static void
test_flatpak_info(void)
{
  g_autofree gchar *id = nwa_caller_parse_flatpak_info(
    "[Application]\nname=org.gnostr.gnostr\nruntime=runtime/org.gnome.Platform/x86_64/47\n"
    "\n[Instance]\ninstance-id=123\n");
  g_assert_cmpstr(id, ==, "org.gnostr.gnostr");
  g_assert_null(nwa_caller_parse_flatpak_info("[Runtime]\nname=org.gnome.Platform\n"));
  g_assert_null(nwa_caller_parse_flatpak_info("[Application]\nname=../../etc\n"));
  g_assert_null(nwa_caller_parse_flatpak_info("garbage"));
  g_assert_null(nwa_caller_parse_flatpak_info(NULL));
}

static void
test_cgroups(void)
{
  static const struct { const gchar *cg; const gchar *id; NwaCallerKind kind; } cases[] = {
    { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-org.gnostr.gnostr-4242.scope\n",
      "org.gnostr.gnostr", NWA_CALLER_SYSTEMD_SCOPE },
    { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-org.gnome.Nautilus@abc.service\n",
      "org.gnome.Nautilus", NWA_CALLER_SYSTEMD_SCOPE },
    { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-dbus\\x2d:1.2\\x2dorg.nostr.Settings.slice/"
      "app-gnome-org.example.My\\x2dApp-99.scope\n",
      "org.example.My-App", NWA_CALLER_SYSTEMD_SCOPE },
    { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/snap.firefox.firefox-1a2b.scope\n",
      "snap.firefox", NWA_CALLER_SNAP },
    { "12:pids:/user.slice\n0::/user.slice/user-1000.slice/session-2.scope\n", NULL, NWA_CALLER_UNKNOWN },
    { "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-firefox-1234.scope\n",
      NULL, NWA_CALLER_UNKNOWN }, /* no reverse-DNS id: fall through to exe */
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    NwaCallerKind k = NWA_CALLER_UNKNOWN;
    g_autofree gchar *id = nwa_caller_parse_cgroup(cases[i].cg, &k);
    g_assert_cmpstr(id, ==, cases[i].id);
    if (cases[i].id) g_assert_cmpint(k, ==, cases[i].kind);
  }
}

static void
test_self(void)
{
  g_autoptr(NwaCaller) c = nwa_caller_new_self();
  g_assert_cmpint(c->kind, ==, NWA_CALLER_SELF);
  g_assert_true(c->same_uid);
  g_autoptr(NwaCaller) d = nwa_caller_copy(c);
  g_assert_cmpstr(d->app_id, ==, c->app_id);
  g_assert_true(d->app_id != c->app_id);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/caller/flatpak-info", test_flatpak_info);
  g_test_add_func("/caller/cgroups", test_cgroups);
  g_test_add_func("/caller/self", test_self);
  return g_test_run();
}
