/* test_caller.c - caller identity parsing (Flatpak info, snap/systemd cgroups).
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-caller.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

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

static void
test_web_origin_syntax(void)
{
  static const gchar *const good[] = {
    "https://snort.social", "https://a.example:8443", "https://xn--bcher-kva.example",
    "https://1.2.3.4", "https://[2001:db8::1]", "https://[2001:db8::1]:8443",
    "http://localhost", "http://localhost:5173", "http://app.localhost:3000",
    "http://127.0.0.1:8080", "http://[::1]", "http://[::1]:3000",
  };
  static const gchar *const bad[] = {
    "", "null", "snort.social", "https://", "https://Snort.social", "https://snort.social/",
    "https://snort.social/path", "https://snort.social?q", "https://snort.social#f",
    "https://u@snort.social", "https://snort.social:443", "https://snort.social:0",
    "https://snort.social:08443", "https://snort.social:65536", "https://snort.social:",
    "https://snort..social", "https://-a.example", "https://a-.example", "https://.example",
    "https://a.example.", "https://sn%6Frt.social", "https://sn ort.social", "https://snört.social",
    "http://snort.social", "http://localhost:80", "http://localhost.evil.com", "http://127.0.0.2",
    "ws://localhost", "file:///etc/passwd", "exe:/usr/bin/nostr-signer-webext-host",
    "org.mozilla.firefox", "snap.firefox", "https://[::1", "https://[zz::1]",
  };
  for (guint i = 0; i < G_N_ELEMENTS(good); i++)
    g_assert_true(nwa_caller_is_web_origin(good[i]) || (g_printerr("good: %s\n", good[i]), FALSE));
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++)
    g_assert_false(nwa_caller_is_web_origin(bad[i]) && (g_printerr("bad: %s\n", bad[i]), TRUE));
  g_assert_false(nwa_caller_is_web_origin(NULL));
  g_autofree gchar *longo = g_strconcat("https://", g_strnfill(510, 'a'), NULL);
  g_assert_false(nwa_caller_is_web_origin(longo));
}

/* A caller shaped like the bridge after identification: exe path + inode. */
static NwaCaller *
bridge_caller(const gchar *exe, NwaCallerKind kind)
{
  NwaCaller *c = g_new0(NwaCaller, 1);
  GStatBuf st;
  g_assert_cmpint(g_stat(exe, &st), ==, 0);
  c->sender = g_strdup(":1.42");
  c->kind = kind;
  c->app_id = kind == NWA_CALLER_EXE ? g_strconcat("exe:", exe, NULL) : g_strdup("org.mozilla.firefox");
  c->display_name = g_strdup(kind == NWA_CALLER_EXE ? "nostr-signer-webext-host" : "Firefox");
  c->same_uid = TRUE;
  c->exe = g_strdup(exe);
  c->exe_dev = (guint64)st.st_dev;
  c->exe_ino = (guint64)st.st_ino;
  return c;
}

static void
test_origin_bridge_gate(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("nwa-bridge-XXXXXX", NULL);
  g_autofree gchar *host = g_build_filename(dir, "nostr-signer-webext-host", NULL);
  g_autofree gchar *other = g_build_filename(dir, "impostor", NULL);
  g_autofree gchar *link = g_build_filename(dir, "link", NULL);
  g_assert_true(g_file_set_contents(host, "#!/bin/true\n", -1, NULL));
  g_assert_true(g_file_set_contents(other, "#!/bin/true\n", -1, NULL));
  g_assert_cmpint(symlink(host, link), ==, 0);
  const gchar *const bridges[] = { "relative/nostr-signer-webext-host", host, NULL };

  /* the bridge in the browser's systemd scope, and as a bare executable */
  g_autoptr(NwaCaller) scoped = bridge_caller(host, NWA_CALLER_SYSTEMD_SCOPE);
  g_assert_true(nwa_caller_may_assert_origin(scoped, bridges));
  g_autoptr(NwaCaller) bare = bridge_caller(host, NWA_CALLER_EXE);
  g_assert_true(nwa_caller_may_assert_origin(bare, bridges));
  /* sandboxed / synthetic / half-resolved kinds never qualify, whatever
   * their exe says */
  static const NwaCallerKind never[] = { NWA_CALLER_FLATPAK, NWA_CALLER_SNAP, NWA_CALLER_SELF,
                                         NWA_CALLER_WEB_ORIGIN, NWA_CALLER_UNKNOWN };
  for (guint i = 0; i < G_N_ELEMENTS(never); i++) {
    g_autoptr(NwaCaller) c = bridge_caller(host, never[i]);
    g_assert_false(nwa_caller_may_assert_origin(c, bridges));
  }

  /* another binary; a different path to the same file; a foreign uid */
  g_autoptr(NwaCaller) imp = bridge_caller(other, NWA_CALLER_EXE);
  g_assert_false(nwa_caller_may_assert_origin(imp, bridges));
  g_autoptr(NwaCaller) via_link = bridge_caller(link, NWA_CALLER_EXE);
  g_assert_false(nwa_caller_may_assert_origin(via_link, bridges));
  g_autoptr(NwaCaller) foreign = bridge_caller(host, NWA_CALLER_EXE);
  foreign->same_uid = FALSE;
  g_assert_false(nwa_caller_may_assert_origin(foreign, bridges));

  /* right path, wrong inode: the file was replaced after the bridge started
   * (or the path names a different file in the caller's mount namespace) */
  g_autoptr(NwaCaller) stale = bridge_caller(host, NWA_CALLER_EXE);
  stale->exe_ino ^= 1;
  g_assert_false(nwa_caller_may_assert_origin(stale, bridges));
  g_autoptr(NwaCaller) noino = bridge_caller(host, NWA_CALLER_EXE);
  noino->exe_ino = 0;
  g_assert_false(nwa_caller_may_assert_origin(noino, bridges));
  g_autoptr(NwaCaller) noexe = bridge_caller(host, NWA_CALLER_EXE);
  g_clear_pointer(&noexe->exe, g_free);
  g_assert_false(nwa_caller_may_assert_origin(noexe, bridges));
  g_assert_false(nwa_caller_may_assert_origin(scoped, NULL));
  const gchar *const none[] = { NULL };
  g_assert_false(nwa_caller_may_assert_origin(scoped, none));

  /* bridge list: the env override (test builds only) replaces the default */
  g_setenv("NOSTR_WALLET_AGENT_ORIGIN_BRIDGES", "/opt/a/host:/opt/b/host", TRUE);
  g_auto(GStrv) list = nwa_caller_origin_bridges();
#ifdef NWA_ORIGIN_BRIDGE_ENV
  g_assert_cmpuint(g_strv_length(list), ==, 2);
  g_assert_cmpstr(list[1], ==, "/opt/b/host");
#else
  g_assert_cmpuint(g_strv_length(list), ==, 1);
  g_assert_cmpstr(list[0], !=, "/opt/a/host");
#endif
  g_unsetenv("NOSTR_WALLET_AGENT_ORIGIN_BRIDGES");
  g_auto(GStrv) def = nwa_caller_origin_bridges();
  g_assert_cmpuint(g_strv_length(def), ==, 1);
  g_assert_true(g_str_has_suffix(def[0], "/nostr-signer-webext-host"));
  g_assert_true(g_path_is_absolute(def[0]));

  g_unlink(link);
  g_unlink(other);
  g_unlink(host);
  g_rmdir(dir);
}

static void
test_for_origin(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("nwa-bridge-XXXXXX", NULL);
  g_autofree gchar *host = g_build_filename(dir, "nostr-signer-webext-host", NULL);
  g_assert_true(g_file_set_contents(host, "x", -1, NULL));

  g_autoptr(NwaCaller) scoped = bridge_caller(host, NWA_CALLER_SYSTEMD_SCOPE);
  g_autoptr(NwaCaller) site = nwa_caller_for_origin(scoped, "https://snort.social");
  g_assert_cmpstr(site->app_id, ==, "https://snort.social");
  g_assert_cmpint(site->kind, ==, NWA_CALLER_WEB_ORIGIN);
  g_assert_false(site->attested);
  g_assert_true(site->same_uid);
  g_assert_cmpstr(site->display_name, ==, "snort.social");
  g_assert_cmpstr(site->via, ==, "Firefox");
  g_assert_cmpstr(site->sender, ==, ":1.42");
  g_assert_cmpstr(nwa_caller_kind_to_string(site->kind), ==, "website");
  /* a principal derived from an origin cannot assert another one */
  const gchar *const bridges[] = { host, NULL };
  g_assert_false(nwa_caller_may_assert_origin(site, bridges));

  g_autoptr(NwaCaller) bare = bridge_caller(host, NWA_CALLER_EXE);
  g_autoptr(NwaCaller) local = nwa_caller_for_origin(bare, "http://localhost:5173");
  g_assert_cmpstr(local->display_name, ==, "localhost:5173");
  g_assert_null(local->via); /* no browser name to show for a bare exe identity */
  g_autoptr(NwaCaller) copy = nwa_caller_copy(local);
  g_assert_cmpstr(copy->exe, ==, host);

  g_unlink(host);
  g_rmdir(dir);
}

/* The settings app is trusted by the bridge's rule (nwa_caller_exe_is),
 * against its own path list; the bridge is not a settings app and vice
 * versa. */
static void
test_settings_app_gate(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("nwa-settings-XXXXXX", NULL);
  g_autofree gchar *settings = g_build_filename(dir, "nostr-settings", NULL);
  g_autofree gchar *host = g_build_filename(dir, "nostr-signer-webext-host", NULL);
  g_autofree gchar *copy = g_build_filename(dir, "copy-of-settings", NULL);
  g_assert_true(g_file_set_contents(settings, "#!/bin/true\n", -1, NULL));
  g_assert_true(g_file_set_contents(host, "#!/bin/true\n", -1, NULL));
  g_assert_true(g_file_set_contents(copy, "#!/bin/true\n", -1, NULL));
  const gchar *const apps[] = { settings, NULL };
  const gchar *const bridges[] = { host, NULL };

  g_autoptr(NwaCaller) s_scope = bridge_caller(settings, NWA_CALLER_SYSTEMD_SCOPE);
  g_autoptr(NwaCaller) s_bare = bridge_caller(settings, NWA_CALLER_EXE);
  g_assert_true(nwa_caller_exe_is(s_scope, apps));
  g_assert_true(nwa_caller_exe_is(s_bare, apps));
  g_assert_false(nwa_caller_may_assert_origin(s_bare, bridges));
  g_autoptr(NwaCaller) bridge = bridge_caller(host, NWA_CALLER_EXE);
  g_assert_false(nwa_caller_exe_is(bridge, apps));
  g_autoptr(NwaCaller) cp = bridge_caller(copy, NWA_CALLER_EXE);
  g_assert_false(nwa_caller_exe_is(cp, apps));
  /* a Flatpak or snap that execs the host binary keeps its sandbox identity */
  g_autoptr(NwaCaller) fp = bridge_caller(settings, NWA_CALLER_FLATPAK);
  g_assert_false(nwa_caller_exe_is(fp, apps));
  g_autoptr(NwaCaller) sn = bridge_caller(settings, NWA_CALLER_SNAP);
  g_assert_false(nwa_caller_exe_is(sn, apps));
  /* a web origin principal carries the bridge's exe but is never the app */
  g_autoptr(NwaCaller) site = nwa_caller_for_origin(s_bare, "https://evil.example");
  g_assert_false(nwa_caller_exe_is(site, apps));

  g_setenv("NOSTR_WALLET_AGENT_SETTINGS_APPS", "/opt/x/nostr-settings", TRUE);
  g_auto(GStrv) list = nwa_caller_settings_apps();
#ifdef NWA_ORIGIN_BRIDGE_ENV
  g_assert_cmpstr(list[0], ==, "/opt/x/nostr-settings");
#else
  g_assert_cmpstr(list[0], !=, "/opt/x/nostr-settings");
#endif
  g_unsetenv("NOSTR_WALLET_AGENT_SETTINGS_APPS");
  g_auto(GStrv) def = nwa_caller_settings_apps();
  g_assert_cmpuint(g_strv_length(def), ==, 1);
  g_assert_true(g_path_is_absolute(def[0]));
  g_assert_true(g_str_has_suffix(def[0], "/bin/nostr-settings"));

  g_unlink(settings);
  g_unlink(host);
  g_unlink(copy);
  g_rmdir(dir);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/caller/flatpak-info", test_flatpak_info);
  g_test_add_func("/caller/cgroups", test_cgroups);
  g_test_add_func("/caller/self", test_self);
  g_test_add_func("/caller/web-origin-syntax", test_web_origin_syntax);
  g_test_add_func("/caller/origin-bridge-gate", test_origin_bridge_gate);
  g_test_add_func("/caller/for-origin", test_for_origin);
  g_test_add_func("/caller/settings-app-gate", test_settings_app_gate);
  return g_test_run();
}
