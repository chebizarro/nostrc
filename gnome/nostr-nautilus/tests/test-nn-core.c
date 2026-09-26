/* test-nn-core.c - menu gating + argv construction (no Nautilus needed)
 *
 * SPDX-License-Identifier: MIT
 */
#include "nn-core.h"
#include "nn-launch.h"

#include <glib/gstdio.h>
#include <string.h>
#if defined(G_OS_UNIX) && !defined(__APPLE__)
#include <gio/gdesktopappinfo.h>
#include <sys/wait.h>
#endif

/* org.nostr.Share.desktop's MimeType= (gnome/nostr-share/data). */
static const gchar *const share_mimes[] = {
  "text/plain", "text/uri-list", "text/markdown", "image/*", "video/*", "audio/*",
  "application/pdf", "inode/directory", "text/calendar", "text/vcard", NULL
};

static const NnTools all_tools = {
  .share_exe = "/usr/bin/nostr-share",
  .share_mimes = share_mimes,
  .seal_exe = "/usr/bin/nostr-seal",
  .seal_gtk_exe = "/usr/bin/nostr-seal-gtk",
  .seal_helper = "/usr/libexec/nostr-nautilus-seal",
};

#define REG(n, m)  { .name = n, .mime = m, .type = G_FILE_TYPE_REGULAR, \
                     .path = "/home/u/" n, .uri = "file:///home/u/" n }
#define REMOTE(n, m) { .name = n, .mime = m, .type = G_FILE_TYPE_REGULAR, \
                       .path = NULL, .uri = "sftp://host/" n }
#define DIR(n, git) { .name = n, .mime = "inode/directory", .type = G_FILE_TYPE_DIRECTORY, \
                      .path = "/home/u/" n, .uri = "file:///home/u/" n, .is_git_repo = git }

/* S=share U=upload E=encrypt D=decrypt */
static gchar *
applies(const NnFile *f, guint n, const NnTools *t)
{
  GString *s = g_string_new(NULL);
  if (nn_action_applies(NN_ACTION_SHARE, f, n, t))   g_string_append_c(s, 'S');
  if (nn_action_applies(NN_ACTION_UPLOAD, f, n, t))  g_string_append_c(s, 'U');
  if (nn_action_applies(NN_ACTION_ENCRYPT, f, n, t)) g_string_append_c(s, 'E');
  if (nn_action_applies(NN_ACTION_DECRYPT, f, n, t)) g_string_append_c(s, 'D');
  return g_string_free(s, FALSE);
}

static void
check_one(const NnFile f, const gchar *expect)
{
  g_autofree gchar *got = applies(&f, 1, &all_tools);
  if (g_strcmp0(got, expect) != 0)
    g_error("%s (%s): expected '%s', got '%s'", f.name, f.mime ? f.mime : "null", expect, got);
}

static void
test_gating_single(void)
{
  check_one((NnFile)REG("a.jpg", "image/jpeg"), "SUE");
  check_one((NnFile)REG("clip.mp4", "video/mp4"), "SUE");
  check_one((NnFile)REG("song.ogg", "audio/ogg"), "SUE");
  check_one((NnFile)REG("report.pdf", "application/pdf"), "SUE");
  /* Text families: shareable, not uploadable (nostr-share refuses
   * --kind 1063 for text/markdown/ics/vcf). */
  check_one((NnFile)REG("notes.txt", "text/plain"), "SE");
  check_one((NnFile)REG("post.md", "text/markdown"), "SE");
  check_one((NnFile)REG("post.md", "text/plain"), "SE");      /* *.md as text/plain */
  check_one((NnFile)REG("meet.ics", "text/calendar"), "SE");
  check_one((NnFile)REG("bob.vcf", "text/vcard"), "SE");
  check_one((NnFile)REG("links.uri", "text/uri-list"), "SE");
  /* Not in MimeType=: no Share, but a Blossom blob (NIP-94) is fine. */
  check_one((NnFile)REG("a.zip", "application/zip"), "UE");
  check_one((NnFile)REG("blob", NULL), "UE");
  /* Sealed files: upload the ciphertext or open it; never re-seal. */
  check_one((NnFile)REG("a.pdf.nsealed", NN_SEALED_MIME), "UD");
  check_one((NnFile)REG("a.pdf.nsealed", "application/octet-stream"), "UD");
  check_one((NnFile)REG("sealed-no-suffix", NN_SEALED_MIME), "UD");
  check_one((NnFile)REG(".nsealed", "application/octet-stream"), "UE"); /* suffix only */
  /* Directories: only git repositories (NIP-34), share only. */
  check_one((NnFile)DIR("repo", TRUE), "S");
  check_one((NnFile)DIR("photos", FALSE), "");
  /* GVfs: nostr-share takes URIs; nostr-seal needs a local path. */
  check_one((NnFile)REMOTE("a.png", "image/png"), "SU");
  check_one((NnFile)REMOTE("a.nsealed", NN_SEALED_MIME), "U");
  NnFile special = REG("fifo", "inode/fifo");
  special.type = G_FILE_TYPE_SPECIAL;
  check_one(special, "");
}

static void
test_gating_subclass(void)
{
#if defined(G_OS_UNIX) && !defined(__APPLE__)
  /* shared-mime-info: text/x-csrc ⊂ text/plain, text/x-vcard is an alias. */
  g_assert_true(nn_mime_in_list("text/x-csrc", share_mimes));
  g_assert_true(nn_mime_in_list("text/x-vcard", share_mimes));
  g_assert_false(nn_mime_in_list("application/zip", share_mimes));
  /* Open With lists nostr-share for source code (text/plain subclass);
   * nostr-share classifies it as "other file", so --kind 1063 applies. */
  check_one((NnFile)REG("main.c", "text/x-csrc"), "SUE");
#else
  g_test_skip("content types are not MIME types on this platform");
#endif
}

static void
test_gating_multi(void)
{
  NnFile media[] = { REG("a.jpg", "image/jpeg"), REG("b.png", "image/png") };
  g_autofree gchar *m = applies(media, 2, &all_tools);
  g_assert_cmpstr(m, ==, "SUE");

  NnFile mixed[] = { REG("a.jpg", "image/jpeg"), REG("a.pdf.nsealed", NN_SEALED_MIME) };
  g_autofree gchar *x = applies(mixed, 2, &all_tools);
  g_assert_cmpstr(x, ==, "U");

  NnFile text_media[] = { REG("a.jpg", "image/jpeg"), REG("n.txt", "text/plain") };
  g_autofree gchar *t = applies(text_media, 2, &all_tools);
  g_assert_cmpstr(t, ==, "SE");

  NnFile with_dir[] = { REG("a.jpg", "image/jpeg"), DIR("repo", TRUE) };
  g_autofree gchar *d = applies(with_dir, 2, &all_tools);
  g_assert_cmpstr(d, ==, "S");

  g_assert_false(nn_action_applies(NN_ACTION_SHARE, NULL, 0, &all_tools));
}

static void
test_gating_missing_tools(void)
{
  NnFile jpg = REG("a.jpg", "image/jpeg");
  NnFile sealed = REG("a.nsealed", NN_SEALED_MIME);
  NnFile nosuffix = REG("sealed", NN_SEALED_MIME);

  NnTools t = all_tools;
  t.share_exe = NULL;
  g_autofree gchar *a = applies(&jpg, 1, &t);
  g_assert_cmpstr(a, ==, "E");

  t = all_tools;
  t.seal_helper = NULL;                     /* built without libadwaita */
  g_autofree gchar *b = applies(&jpg, 1, &t);
  g_assert_cmpstr(b, ==, "SU");

  t = all_tools;
  t.seal_gtk_exe = NULL;                    /* CLI-only nostr-seal */
  g_autofree gchar *c = applies(&sealed, 1, &t);
  g_assert_cmpstr(c, ==, "UD");
  g_autofree gchar *d = applies(&nosuffix, 1, &t);
  g_assert_cmpstr(d, ==, "U");              /* CLI needs the suffix */

  t = all_tools;
  t.seal_exe = NULL;
  g_autofree gchar *e = applies(&jpg, 1, &t);
  g_assert_cmpstr(e, ==, "SU");
  g_autofree gchar *f = applies(&sealed, 1, &t);
  g_assert_cmpstr(f, ==, "UD");             /* nostr-seal-gtk alone opens */
}

static gchar *
join_argvs(GPtrArray *argvs)
{
  GString *s = g_string_new(NULL);
  for (guint i = 0; argvs && i < argvs->len; i++) {
    g_autofree gchar *j = g_strjoinv(" ", g_ptr_array_index(argvs, i));
    g_string_append_printf(s, "%s[%s]", i ? " " : "", j);
  }
  return g_string_free(s, FALSE);
}

static void
check_argv(NnAction a, const NnFile *f, guint n, const NnTools *t, const gchar *expect)
{
  g_autoptr(GPtrArray) argvs = nn_action_argvs(a, f, n, t);
  g_autofree gchar *got = join_argvs(argvs);
  g_assert_cmpstr(got, ==, expect);
}

static void
test_argv(void)
{
  NnFile two[] = { REG("a.jpg", "image/jpeg"), REG("b b.png", "image/png") };
  check_argv(NN_ACTION_SHARE, two, 2, &all_tools,
             "[/usr/bin/nostr-share /home/u/a.jpg /home/u/b b.png]");
  check_argv(NN_ACTION_UPLOAD, two, 2, &all_tools,
             "[/usr/bin/nostr-share --kind 1063 /home/u/a.jpg /home/u/b b.png]");
  check_argv(NN_ACTION_ENCRYPT, two, 2, &all_tools,
             "[/usr/libexec/nostr-nautilus-seal /home/u/a.jpg /home/u/b b.png]");
  check_argv(NN_ACTION_DECRYPT, two, 2, &all_tools, "");   /* not applicable → NULL */

  NnFile remote = REMOTE("a.png", "image/png");
  check_argv(NN_ACTION_SHARE, &remote, 1, &all_tools, "[/usr/bin/nostr-share sftp://host/a.png]");

  NnFile repo = DIR("repo", TRUE);
  check_argv(NN_ACTION_SHARE, &repo, 1, &all_tools, "[/usr/bin/nostr-share /home/u/repo]");

  NnFile sealed[] = { REG("a.nsealed", NN_SEALED_MIME), REG("b.nsealed", NN_SEALED_MIME) };
  check_argv(NN_ACTION_DECRYPT, sealed, 2, &all_tools,
             "[/usr/bin/nostr-seal-gtk /home/u/a.nsealed /home/u/b.nsealed]");
  NnTools cli = all_tools;
  cli.seal_gtk_exe = NULL;
  check_argv(NN_ACTION_DECRYPT, sealed, 2, &cli,
             "[/usr/bin/nostr-seal decrypt /home/u/a.nsealed] "
             "[/usr/bin/nostr-seal decrypt /home/u/b.nsealed]");
  check_argv(NN_ACTION_UPLOAD, sealed, 1, &all_tools,
             "[/usr/bin/nostr-share --kind 1063 /home/u/a.nsealed]");
}

static const gchar *const nasty[] = {
  "plain", "with space", "it's", "100%", "%f", "%%U", "\"dq\"", "$HOME", "`id`",
  "back\\slash", "new\nline", "-dash", "", "ünïcødé 📷.jpg", NULL
};

static void
test_exec_line(void)
{
  const gchar *argv[] = { "/usr/bin/nostr-share", "50% off.jpg", NULL };
  g_autofree gchar *line = nn_exec_line(argv);
  g_assert_cmpstr(line, ==, "'/usr/bin/nostr-share' '50%% off.jpg'");

  /* Round trip through the same two steps GDesktopAppInfo applies:
   * field-code expansion ("%%" → "%"), then g_shell_parse_argv. */
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, (gpointer)"/bin/echo");
  for (guint i = 0; nasty[i]; i++)
    g_ptr_array_add(a, (gpointer)nasty[i]);
  g_ptr_array_add(a, NULL);
  g_autofree gchar *l2 = nn_exec_line((const gchar *const *)a->pdata);
  GString *expanded = g_string_new(NULL);
  for (const gchar *p = l2; *p; p++) {
    if (p[0] == '%' && p[1] != '\0') {
      g_assert_cmpint(p[1], ==, '%');    /* no stray field codes */
      p++;
    }
    g_string_append_c(expanded, *p);
  }
  gint argc = 0;
  g_auto(GStrv) back = NULL;
  g_assert_true(g_shell_parse_argv(expanded->str, &argc, &back, NULL));
  g_assert_cmpuint((guint)argc, ==, a->len - 1);
  for (guint i = 0; i + 1 < a->len; i++)
    g_assert_cmpstr(back[i], ==, g_ptr_array_index(a, i));
  g_string_free(expanded, TRUE);
  g_ptr_array_free(a, TRUE);
}

#if defined(G_OS_UNIX) && !defined(__APPLE__)
static void
record_pid(GDesktopAppInfo *app, GPid pid, gpointer data)
{
  (void)app;
  *(GPid *)data = pid;
}
#endif

/* The real launcher: GAppInfo from nn_exec_line(), exactly what the menu
 * items run, must deliver the argv byte-for-byte. */
static void
test_launch_roundtrip(void)
{
#if defined(G_OS_UNIX) && !defined(__APPLE__)
  g_autofree gchar *dir = g_dir_make_tmp("nn-launch-XXXXXX", NULL);
  g_autofree gchar *out = g_build_filename(dir, "argv.out", NULL);
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, (gpointer)"/bin/sh");
  g_ptr_array_add(a, (gpointer)"-c");
  g_ptr_array_add(a, (gpointer)"o=$1; shift; for x in \"$@\"; do printf '%s\\0' \"$x\"; done > \"$o\"");
  g_ptr_array_add(a, (gpointer)"sh");
  g_ptr_array_add(a, out);
  for (guint i = 0; nasty[i]; i++)
    g_ptr_array_add(a, (gpointer)nasty[i]);
  g_ptr_array_add(a, NULL);

  g_autoptr(GError) e = NULL;
  g_autoptr(GAppInfo) app = nn_launch_app_info((const gchar *const *)a->pdata, &e);
  g_assert_no_error(e);
  GPid pid = 0;
  g_assert_true(g_desktop_app_info_launch_uris_as_manager(G_DESKTOP_APP_INFO(app), NULL, NULL,
                                                          G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                                          record_pid, &pid, &e));
  g_assert_no_error(e);
  g_assert_cmpint(pid, >, 0);
  int status = 0;
  g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
  g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  g_autofree gchar *buf = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(out, &buf, &len, NULL));
  guint i = 0;
  for (gsize off = 0; off < len; i++) {
    g_assert_nonnull(nasty[i]);
    g_assert_cmpstr(buf + off, ==, nasty[i]);
    off += strlen(buf + off) + 1;
  }
  g_assert_cmpuint(i, ==, g_strv_length((gchar **)nasty));
  g_unlink(out);
  g_rmdir(dir);
  g_ptr_array_free(a, TRUE);
#else
  g_test_skip("GDesktopAppInfo is Linux/BSD only");
#endif
}

/* A real key: fiatjaf's npub, and its hex. */
#define NPUB1 "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6"
#define HEX1  "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d"

static void
test_recipients(void)
{
  g_auto(GStrv) k = NULL;
  g_autoptr(GError) e = NULL;

  g_assert_true(nn_parse_recipients("  ", &k, &e));
  g_assert_cmpuint(g_strv_length(k), ==, 0);
  g_clear_pointer(&k, g_strfreev);

  g_assert_true(nn_parse_recipients(NPUB1 ", nostr:" NPUB1 ";\n" HEX1 " " NPUB1, &k, &e));
  g_assert_no_error(e);
  g_assert_cmpuint(g_strv_length(k), ==, 2);         /* duplicates dropped */
  g_assert_cmpstr(k[0], ==, NPUB1);
  g_assert_cmpstr(k[1], ==, HEX1);
  g_clear_pointer(&k, g_strfreev);

  g_autofree gchar *upper = g_ascii_strup(NPUB1, -1);
  g_assert_true(nn_parse_recipients(upper, &k, &e));  /* all-caps bech32 is valid */
  g_assert_cmpstr(k[0], ==, NPUB1);
  g_clear_pointer(&k, g_strfreev);

  /* typo (last char), mixed case, nsec, junk */
  g_assert_false(nn_parse_recipients("npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w7", &k, &e));
  g_assert_nonnull(e);
  g_assert_null(k);
  g_clear_error(&e);
  g_assert_false(nn_parse_recipients("Npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6", &k, &e));
  g_clear_error(&e);
  g_assert_false(nn_parse_recipients("nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5", &k, &e));
  g_clear_error(&e);
  g_assert_false(nn_parse_recipients(NPUB1 " bob", &k, &e));
  g_assert_true(strstr(e->message, "bob") != NULL);
  g_clear_error(&e);
}

static void
test_seal_argv(void)
{
  const gchar *to[] = { NPUB1, HEX1, NULL };
  g_auto(GStrv) a = nn_seal_encrypt_argv("/usr/bin/nostr-seal", to, TRUE, "/home/u/-x.pdf");
  g_autofree gchar *j = g_strjoinv(" ", a);
  g_assert_cmpstr(j, ==, "/usr/bin/nostr-seal encrypt --to " NPUB1 " --to " HEX1
                         " --to-self /home/u/-x.pdf");
  g_auto(GStrv) b = nn_seal_encrypt_argv("nostr-seal", NULL, TRUE, "/tmp/f");
  g_autofree gchar *jb = g_strjoinv(" ", b);
  g_assert_cmpstr(jb, ==, "nostr-seal encrypt --to-self /tmp/f");
}

#ifdef NN_SEAL_CLI_PATH
/* The argv the dialog builds, run against the real nostr-seal CLI. */
static void
test_seal_cli_contract(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("nn-seal-XXXXXX", NULL);
  g_autofree gchar *in = g_build_filename(dir, "it's 100% -x.txt", NULL);
  g_assert_true(g_file_set_contents(in, "hello nostr\n", -1, NULL));
  const gchar *to[] = { NPUB1, NULL };
  g_auto(GStrv) argv = nn_seal_encrypt_argv(NN_SEAL_CLI_PATH, to, FALSE, in);
  gint status = -1;
  g_autofree gchar *err = NULL;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                             NULL, &err, &status, NULL));
  if (!g_spawn_check_wait_status(status, NULL))
    g_error("nostr-seal encrypt failed: %s", err);

  g_autofree gchar *sealed = g_strconcat(in, NN_SEALED_SUFFIX, NULL);
  g_assert_true(g_file_test(sealed, G_FILE_TEST_IS_REGULAR));
  const gchar *inspect[] = { NN_SEAL_CLI_PATH, "inspect", sealed, NULL };
  g_autofree gchar *out = NULL;
  g_assert_true(g_spawn_sync(NULL, (gchar **)inspect, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                             &out, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_assert_true(strstr(out, NPUB1) != NULL);

  /* The Decrypt item is offered for it. */
  g_autofree gchar *base = g_path_get_basename(sealed);
  NnFile f = { .name = base, .mime = NULL, .type = G_FILE_TYPE_REGULAR,
               .path = sealed, .uri = "file:///x" };
  g_assert_true(nn_action_applies(NN_ACTION_DECRYPT, &f, 1, &all_tools));
  g_unlink(sealed);
  g_unlink(in);
  g_rmdir(dir);
}
#endif

static void
test_git_repo(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("nn-git-XXXXXX", NULL);
  g_assert_false(nn_dir_is_git_repo(dir));
  g_assert_false(nn_dir_is_git_repo(NULL));
  g_autofree gchar *dotgit = g_build_filename(dir, ".git", NULL);
  g_assert_cmpint(g_mkdir(dotgit, 0700), ==, 0);
  g_assert_true(nn_dir_is_git_repo(dir));
  g_rmdir(dotgit);
  /* bare layout */
  g_autofree gchar *head = g_build_filename(dir, "HEAD", NULL);
  g_autofree gchar *objects = g_build_filename(dir, "objects", NULL);
  g_autofree gchar *refs = g_build_filename(dir, "refs", NULL);
  g_assert_true(g_file_set_contents(head, "ref: refs/heads/main\n", -1, NULL));
  g_mkdir(objects, 0700);
  g_assert_false(nn_dir_is_git_repo(dir));
  g_mkdir(refs, 0700);
  g_assert_true(nn_dir_is_git_repo(dir));
  g_rmdir(refs);
  g_rmdir(objects);
  g_unlink(head);
  g_rmdir(dir);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-nautilus/gating/single", test_gating_single);
  g_test_add_func("/nostr-nautilus/gating/subclass", test_gating_subclass);
  g_test_add_func("/nostr-nautilus/gating/multi", test_gating_multi);
  g_test_add_func("/nostr-nautilus/gating/missing-tools", test_gating_missing_tools);
  g_test_add_func("/nostr-nautilus/argv", test_argv);
  g_test_add_func("/nostr-nautilus/exec-line", test_exec_line);
  g_test_add_func("/nostr-nautilus/launch-roundtrip", test_launch_roundtrip);
  g_test_add_func("/nostr-nautilus/recipients", test_recipients);
  g_test_add_func("/nostr-nautilus/seal-argv", test_seal_argv);
#ifdef NN_SEAL_CLI_PATH
  g_test_add_func("/nostr-nautilus/seal-cli-contract", test_seal_cli_contract);
#endif
  g_test_add_func("/nostr-nautilus/git-repo", test_git_repo);
  return g_test_run();
}
