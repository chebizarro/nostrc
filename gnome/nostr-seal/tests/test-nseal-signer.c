/* test-nseal-signer.c — nostr-seal CLI against the real nostr-signer-daemon.
 * SPDX-License-Identifier: MIT
 *
 * Helper-managed private session bus, the actual daemon with an env-lane key,
 * and the installed-shape CLI driven as a subprocess:
 *   encrypt --to-self --to <other>  → inspect lists both
 *   decrypt (NIP44DecryptB64ForApp, granted to the nostr-seal CLI)
 *   decrypt of a file not sealed for the signer → "not a recipient", no output
 *   decrypt once the grant says deny → "denied", no output
 *   passphrase round-trip through --passphrase-file
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nostr-seal.h"
#include "signer_permissions.h"
#include "nostrc-test-bus.h"
#include <keys.h>
#include <nostr-utils.h>

#ifndef NSEAL_DAEMON_PATH
#error "NSEAL_DAEMON_PATH must be defined"
#endif
#ifndef NSEAL_CLI_PATH
#error "NSEAL_CLI_PATH must be defined"
#endif

#define CHECK(c) do { if (!(c)) { g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char *tmp;

static int run_cli(char **out, char **err, ...) {
  GPtrArray *argv = g_ptr_array_new();
  g_ptr_array_add(argv, (gpointer)NSEAL_CLI_PATH);
  va_list ap; va_start(ap, err);
  for (const char *a; (a = va_arg(ap, const char *));) g_ptr_array_add(argv, (gpointer)a);
  va_end(ap);
  g_ptr_array_add(argv, NULL);
  int status = 0; GError *e = NULL;
  gchar *o = NULL, *r = NULL;
  CHECK(g_spawn_sync(tmp, (gchar **)argv->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, &o, &r, &status, &e));
  g_ptr_array_free(argv, TRUE);
  if (out) *out = o; else g_free(o);
  if (err) *err = r; else g_free(r);
  return g_spawn_check_wait_status(status, NULL) ? 0 : 1;
}

static char *npub_of_hex(const char *hex) {
  uint8_t pk[32]; CHECK(nostr_hex2bin(pk, hex, 32));
  return nseal_pubkey_to_npub(pk);
}

static void appeared(GDBusConnection *c, const gchar *n, const gchar *o, gpointer data) {
  (void)c; (void)n; (void)o; *(gboolean *)data = TRUE;
}
static gboolean timeout(gpointer data) { (void)data; g_error("signer did not acquire its name"); return G_SOURCE_REMOVE; }
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) return 77;
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  tmp = (char *)nostrc_test_bus_get_dir(bus);
  g_setenv("NOSTRC_TEST_ISOLATION", "1", TRUE);
  g_autofree char *cfg = g_build_filename(tmp, "config", NULL);
  g_autofree char *gdir = g_build_filename(cfg, "gnostr", NULL);
  g_mkdir_with_parents(gdir, 0700);
  g_setenv("XDG_CONFIG_HOME", cfg, TRUE);
  g_setenv("HOME", tmp, TRUE);

  char *sk = nostr_key_generate_private(), *pk = nostr_key_get_public(sk);
  char *osk = nostr_key_generate_private(), *opk = nostr_key_get_public(osk);
  CHECK(sk && pk && osk && opk);
  g_autofree char *me = npub_of_hex(pk), *other = npub_of_hex(opk);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", sk, TRUE);

  /* nip55l 0.4.0 grants: [kind] "<principal>|<npub>", where the principal
   * is derived from the caller's connection - here the CLI's executable
   * (exe:<path>; run outside an app scope) - and on buses that report no
   * PID (macOS) the explicit named app claim ("org.nostr.Seal"). The grant covers every selector that
   * resolves to this key. */
  g_autofree char *cli_real = realpath(NSEAL_CLI_PATH, NULL);
  CHECK(cli_real);
  g_autofree char *grants = g_build_filename(gdir, "signer-grants.ini", NULL);
  g_autofree char *grants_body = g_strdup_printf(
      "[get_public_key]\nexe:%1$s|*=allow\nclaimed:org.nostr.Seal|*=allow\n"
      "[nip44_decrypt]\nexe:%1$s|%2$s=allow\nclaimed:org.nostr.Seal|%2$s=allow\n",
      cli_real, me);
  CHECK(g_file_set_contents(grants, grants_body, -1, NULL));

  GDBusConnection *c = nostrc_test_bus_connect(bus);
  gboolean online = FALSE;
  guint watch = g_bus_watch_name_on_connection(c, "org.nostr.Signer", 0, appeared, NULL, &online, NULL);
  const gchar *command[] = {NSEAL_DAEMON_PATH, NULL};
  nostrc_test_bus_spawn_supervised(bus, "nseal-daemon.log", NULL, command);
  guint bound = g_timeout_add_seconds(20, timeout, NULL);
  while (!online) g_main_context_iteration(NULL, TRUE);
  g_source_remove(bound);

  g_autofree char *plain = g_build_filename(tmp, "report.pdf", NULL);
  const char *body = "%PDF-1.7 pretend\n\x00\x01\x02 binary tail";
  CHECK(g_file_set_contents(plain, body, 30, NULL));

  /* Seal for the signer (--to-self → GetPublicKey) and someone else. */
  char *o = NULL, *r = NULL;
  int rc = run_cli(&o, &r, "encrypt", "--to-self", "--to", other, "--chunk-size-log2", "12", "report.pdf", NULL);
  if (rc) { g_printerr("encrypt: %s\n", r); exit(1); }
  g_free(o); g_free(r);
  g_autofree char *sealed = g_build_filename(tmp, "report.pdf.nsealed", NULL);
  CHECK(g_file_test(sealed, G_FILE_TEST_EXISTS));
  /* Refuses to clobber without --force. */
  CHECK(run_cli(NULL, NULL, "encrypt", "--to", other, "report.pdf", NULL) != 0);

  rc = run_cli(&o, &r, "inspect", "report.pdf.nsealed", NULL);
  CHECK(rc == 0 && strstr(o, me) && strstr(o, other));
  g_free(o); g_free(r);

  /* Decrypt through NIP44DecryptB64ForApp. */
  CHECK(g_unlink(plain) == 0);
  rc = run_cli(&o, &r, "decrypt", "report.pdf.nsealed", NULL);
  if (rc) { g_printerr("decrypt: %s\n", r); exit(1); }
  g_free(o); g_free(r);
  gchar *back = NULL; gsize n = 0;
  CHECK(g_file_get_contents(plain, &back, &n, NULL));
  CHECK(n == 30 && memcmp(back, body, 30) == 0);
  g_free(back);
  struct stat st; CHECK(g_stat(plain, &st) == 0 && (st.st_mode & 0077) == 0);  /* plaintext is 0600 */

  /* Sealed only for someone else. */
  rc = run_cli(NULL, &r, "encrypt", "--to", other, "-o", "theirs.nsealed", "report.pdf", NULL);
  CHECK(rc == 0); g_free(r);
  rc = run_cli(NULL, &r, "decrypt", "-o", "theirs.out", "theirs.nsealed", NULL);
  CHECK(rc != 0 && strstr(r, "not a recipient"));
  g_free(r);
  g_autofree char *theirs_out = g_build_filename(tmp, "theirs.out", NULL);
  CHECK(!g_file_test(theirs_out, G_FILE_TEST_EXISTS));

  /* After the one-time legacy import, only v2 can change authority. */
  g_autoptr(SignerPermissions) permissions = signer_permissions_new(cfg);
  g_autofree char *exe_principal = g_strdup_printf("exe:%s", cli_real);
  CHECK(signer_permissions_remember(permissions, exe_principal, me,
                                   "nip44_decrypt", -1, FALSE, 0, NULL));
  CHECK(signer_permissions_remember(permissions, "claimed:org.nostr.Seal", me,
                                   "nip44_decrypt", -1, FALSE, 0, NULL));
  rc = run_cli(NULL, &r, "decrypt", "-o", "denied.out", "report.pdf.nsealed", NULL);
  CHECK(rc != 0 && strstr(r, "denied"));
  g_free(r);
  g_autofree char *denied_out = g_build_filename(tmp, "denied.out", NULL);
  CHECK(!g_file_test(denied_out, G_FILE_TEST_EXISTS));

  /* Passphrase lane needs no signer at all. */
  g_autofree char *pwf = g_build_filename(tmp, "pw", NULL);
  CHECK(g_file_set_contents(pwf, "hunter2 hunter2\n", -1, NULL));
  rc = run_cli(NULL, &r, "encrypt", "--passphrase-file", pwf, "-o", "pw.nsealed", "report.pdf", NULL);
  if (rc) { g_printerr("encrypt pw: %s\n", r); exit(1); }
  g_free(r);
  rc = run_cli(&o, NULL, "inspect", "pw.nsealed", NULL);
  CHECK(rc == 0 && strstr(o, "passphrase"));
  g_free(o);
  rc = run_cli(NULL, &r, "decrypt", "--passphrase-file", pwf, "-o", "pw.out", "pw.nsealed", NULL);
  if (rc) { g_printerr("decrypt pw: %s\n", r); exit(1); }
  g_free(r);
  g_autofree char *pw_out = g_build_filename(tmp, "pw.out", NULL);
  CHECK(g_file_get_contents(pw_out, &back, &n, NULL) && n == 30 && memcmp(back, body, 30) == 0);
  g_free(back);

  g_bus_unwatch_name(watch);
  nostrc_test_bus_down(bus);
  free(sk); free(pk); free(osk); free(opk);
  g_print("test-nostr-seal-signer: PASS\n");
  return 0;
}
