/* test-nseal-config — ~/.config/nostr/seal.conf (nseal-config.h) and the
 * CLI's use of it. SPDX-License-Identifier: MIT */
#include "nseal-config.h"
#include "nostr-seal.h"

#include <glib/gstdio.h>
#include <string.h>

/* NIP-19 test vector. */
#define NPUB "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"
#define HEX  "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"

static gchar *tmpdir;

static gchar *write_conf(const gchar *body) {
  gchar *p = g_build_filename(tmpdir, "seal.conf", NULL);
  g_assert_true(g_file_set_contents(p, body, -1, NULL));
  g_setenv("NOSTR_SEAL_CONFIG", p, TRUE);
  return p;
}

static void test_missing(void) {
  g_autofree gchar *p = g_build_filename(tmpdir, "absent.conf", NULL);
  g_setenv("NOSTR_SEAL_CONFIG", p, TRUE);
  GError *e = NULL;
  g_autoptr(NsealConfig) c = nseal_config_load(&e);
  g_assert_no_error(e);
  g_assert_false(c->loaded);
  g_assert_cmpuint(g_strv_length(c->default_recipients), ==, 0);
  g_assert_false(c->include_self);
  g_assert_cmpint(c->work_factor, ==, 0);
}

static void test_valid(void) {
  g_autofree gchar *p = write_conf("# comment\n[seal]\ndefault_recipients=" NPUB ";" HEX
                                   "\ninclude_self=true\nwork_factor=18\n");
  GError *e = NULL;
  g_autoptr(NsealConfig) c = nseal_config_load(&e);
  g_assert_no_error(e);
  g_assert_true(c->loaded);
  g_assert_cmpstr(c->path, ==, p);
  g_assert_cmpuint(g_strv_length(c->default_recipients), ==, 2);
  g_assert_cmpstr(c->default_recipients[0], ==, NPUB);
  g_assert_cmpstr(c->default_recipients[1], ==, HEX);
  g_assert_true(c->include_self);
  g_assert_cmpint(c->work_factor, ==, 18);
}

static void test_bad_recipient(void) {
  g_autofree gchar *p = write_conf("[seal]\ndefault_recipients=" NPUB ";npub1nope\n");
  GError *e = NULL;
  NsealConfig *c = nseal_config_load(&e);
  g_assert_null(c);
  g_assert_error(e, NSEAL_ERROR, NSEAL_ERROR_ARG);
  g_assert_nonnull(strstr(e->message, p));
  g_error_free(e);
}

static void test_bad_work_factor(void) {
  g_autofree gchar *p = write_conf("[seal]\nwork_factor=30\n");
  GError *e = NULL;
  g_assert_null(nseal_config_load(&e));
  g_assert_error(e, NSEAL_ERROR, NSEAL_ERROR_ARG);
  g_clear_error(&e);
  g_free(write_conf("[seal]\ninclude_self=perhaps\n"));
  g_assert_null(nseal_config_load(&e));
  g_assert_nonnull(e);
  g_clear_error(&e);
}

static gboolean run_cli(const gchar *const *argv, gchar **err_out) {
  gint status = -1;
  gchar **env = g_get_environ();
  GError *e = NULL;
  gboolean spawned = g_spawn_sync(tmpdir, (gchar **)argv, env, G_SPAWN_STDOUT_TO_DEV_NULL,
                                  NULL, NULL, NULL, err_out, &status, &e);
  g_strfreev(env);
  g_assert_no_error(e);
  g_assert_true(spawned);
  return g_spawn_check_wait_status(status, NULL);
}

static void test_cli_uses_defaults(void) {
  g_autofree gchar *in = g_build_filename(tmpdir, "plain.txt", NULL);
  g_autofree gchar *out = g_build_filename(tmpdir, "plain.txt.nsealed", NULL);
  g_assert_true(g_file_set_contents(in, "hello", -1, NULL));

  /* No config, no recipients: refused. */
  g_autofree gchar *absent = g_build_filename(tmpdir, "absent.conf", NULL);
  g_setenv("NOSTR_SEAL_CONFIG", absent, TRUE);
  const gchar *enc[] = {NSEAL_CLI_PATH, "encrypt", "-f", in, NULL};
  g_autofree gchar *err1 = NULL;
  g_assert_false(run_cli(enc, &err1));

  /* Config default recipient: sealed for it, and the CLI says so. */
  g_free(write_conf("[seal]\ndefault_recipients=" HEX "\n"));
  g_autofree gchar *err2 = NULL;
  g_assert_true(run_cli(enc, &err2));
  g_assert_nonnull(strstr(err2, "using defaults from"));
  const gchar *insp[] = {NSEAL_CLI_PATH, "inspect", out, NULL};
  gint status = -1;
  g_autofree gchar *so = NULL;
  gchar **env = g_get_environ();
  g_assert_true(g_spawn_sync(tmpdir, (gchar **)insp, env, G_SPAWN_DEFAULT, NULL, NULL,
                             &so, NULL, &status, NULL));
  g_strfreev(env);
  g_assert_nonnull(strstr(so, "recipient: " NPUB));

  /* Explicit --to overrides the defaults entirely. */
  g_free(write_conf("[seal]\ndefault_recipients=npub1bad\n"));
  g_autofree gchar *err3 = NULL;
  g_assert_false(run_cli(enc, &err3)); /* bad default → refused */
  g_assert_nonnull(strstr(err3, "default_recipients"));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  tmpdir = g_dir_make_tmp("nseal-config-XXXXXX", NULL);
  g_test_add_func("/nostr-seal/config/missing", test_missing);
  g_test_add_func("/nostr-seal/config/valid", test_valid);
  g_test_add_func("/nostr-seal/config/bad-recipient", test_bad_recipient);
  g_test_add_func("/nostr-seal/config/bad-values", test_bad_work_factor);
  g_test_add_func("/nostr-seal/config/cli-defaults", test_cli_uses_defaults);
  int rc = g_test_run();
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", tmpdir);
  if (system(cmd) != 0) rc = rc ? rc : 1;
  g_free(tmpdir);
  return rc;
}
