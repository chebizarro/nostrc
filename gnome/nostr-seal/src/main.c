/*
 * nostr-seal — encrypt files for Nostr public keys (or a passphrase).
 * SPDX-License-Identifier: MIT
 *
 *   nostr-seal encrypt --to npub1… [--to …] [--to-self] FILE|-
 *   nostr-seal encrypt --passphrase FILE|-
 *   nostr-seal decrypt [--identity npub1…] FILE.nsealed
 *   nostr-seal inspect FILE.nsealed
 *
 * Bead nostrc-da9c. Format: README.md. The nsec never enters this process:
 * decrypt goes through org.nostr.Signer (see nseal-signer.h).
 */

#include "nostr-seal.h"
#include "nseal-private.h"
#include "nseal-signer.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <openssl/crypto.h>

#ifndef NOSTR_SEAL_VERSION
#define NOSTR_SEAL_VERSION "0.1.0"
#endif

static void wipe_free(char *s) {
  if (!s) return;
  OPENSSL_cleanse(s, strlen(s));
  g_free(s);
}

/* ─── Passphrase input ───────────────────────────────────────────────── */

static char *read_line_fd(int fd) {
  GString *s = g_string_new(NULL);
  char c;
  for (;;) {
    ssize_t n = read(fd, &c, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || c == '\n') break;
    g_string_append_c(s, c);
  }
  if (s->len && s->str[s->len - 1] == '\r') g_string_truncate(s, s->len - 1);
  return g_string_free(s, FALSE);
}

static char *prompt_passphrase(const char *prompt, GError **error) {
  int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                        "no terminal for a passphrase prompt; use --passphrase-file");
    return NULL;
  }
  struct termios old, quiet;
  gboolean restore = tcgetattr(fd, &old) == 0;
  if (restore) { quiet = old; quiet.c_lflag &= ~(tcflag_t)ECHO; (void)tcsetattr(fd, TCSAFLUSH, &quiet); }
  if (write(fd, prompt, strlen(prompt)) < 0) { /* best effort */ }
  char *line = read_line_fd(fd);
  if (write(fd, "\n", 1) < 0) { /* best effort */ }
  if (restore) (void)tcsetattr(fd, TCSAFLUSH, &old);
  close(fd);
  return line;
}

static char *get_passphrase(const char *file, gboolean confirm, GError **error) {
  char *pw = NULL;
  if (file) {
    int fd = g_str_equal(file, "-") ? dup(STDIN_FILENO) : open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "%s: %s", file, g_strerror(e));
      return NULL;
    }
    pw = read_line_fd(fd);
    close(fd);
  } else {
    pw = prompt_passphrase("Passphrase: ", error);
    if (!pw) return NULL;
    if (confirm) {
      char *again = prompt_passphrase("Repeat passphrase: ", error);
      if (!again) { wipe_free(pw); return NULL; }
      gboolean same = g_str_equal(pw, again);
      wipe_free(again);
      if (!same) {
        wipe_free(pw);
        g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "passphrases do not match");
        return NULL;
      }
    }
  }
  if (!*pw) {
    wipe_free(pw);
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "empty passphrase");
    return NULL;
  }
  return pw;
}

/* ─── Commands ───────────────────────────────────────────────────────── */

static gboolean fail(GError *e) {
  g_printerr("nostr-seal: %s\n", e ? e->message : "failed");
  if (e) g_error_free(e);
  return FALSE;
}

static gboolean cmd_encrypt(int argc, char **argv) {
  g_auto(GStrv) to = NULL;
  gboolean to_self = FALSE, ask_pass = FALSE, force = FALSE;
  g_autofree char *pass_file = NULL, *out = NULL, *identity = NULL;
  int work = 0, chunk_log2 = 0;
  GOptionEntry entries[] = {
    { "to", 't', 0, G_OPTION_ARG_STRING_ARRAY, &to, "Recipient (npub1… or 64-hex); repeatable", "NPUB" },
    { "to-self", 0, 0, G_OPTION_ARG_NONE, &to_self, "Add the signer's identity as a recipient", NULL },
    { "identity", 'i', 0, G_OPTION_ARG_STRING, &identity, "Signer identity for --to-self", "NPUB" },
    { "passphrase", 'p', 0, G_OPTION_ARG_NONE, &ask_pass, "Seal with a passphrase (NIP-49) instead", NULL },
    { "passphrase-file", 0, 0, G_OPTION_ARG_FILENAME, &pass_file, "Read the passphrase from the first line of FILE (- = stdin)", "FILE" },
    { "work-factor", 0, 0, G_OPTION_ARG_INT, &work, "scrypt log2(N) for --passphrase (16-20, default 16)", "N" },
    { "chunk-size-log2", 0, 0, G_OPTION_ARG_INT, &chunk_log2, "Chunk size as log2 bytes (12-24, default 20)", "N" },
    { "output", 'o', 0, G_OPTION_ARG_FILENAME, &out, "Output path (- = stdout; default FILE.nsealed)", "PATH" },
    { "force", 'f', 0, G_OPTION_ARG_NONE, &force, "Overwrite an existing output", NULL },
    G_OPTION_ENTRY_NULL
  };
  g_autoptr(GOptionContext) ctx = g_option_context_new("FILE|- — seal a file");
  g_option_context_add_main_entries(ctx, entries, NULL);
  GError *e = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &e)) return fail(e);
  if (argc != 2) { g_printerr("usage: nostr-seal encrypt [--to NPUB…|--to-self|--passphrase] FILE\n"); return FALSE; }
  const char *in_path = argv[1];
  const gboolean pass = ask_pass || pass_file;
  if (pass && (to || to_self)) {
    g_printerr("nostr-seal: --passphrase cannot be combined with recipients\n");
    return FALSE;
  }
  if (work && (work < NSEAL_LOG_N_MIN || work > NSEAL_LOG_N_MAX)) {
    g_printerr("nostr-seal: --work-factor must be %d..%d\n", NSEAL_LOG_N_MIN, NSEAL_LOG_N_MAX);
    return FALSE;
  }
  if (chunk_log2 && (chunk_log2 < NSEAL_CHUNK_LOG2_MIN || chunk_log2 > NSEAL_CHUNK_LOG2_MAX)) {
    g_printerr("nostr-seal: --chunk-size-log2 must be %d..%d\n", NSEAL_CHUNK_LOG2_MIN, NSEAL_CHUNK_LOG2_MAX);
    return FALSE;
  }

  g_autoptr(GArray) rcpts = g_array_new(FALSE, FALSE, NSEAL_PUBKEY_LEN);
  for (char **p = to; p && *p; p++) {
    uint8_t pk[32];
    if (!nseal_parse_pubkey(*p, pk, &e)) return fail(e);
    g_array_append_vals(rcpts, pk, 1);
  }
  if (to_self) {
    g_autoptr(NsealSigner) s = nseal_signer_new(identity, &e);
    uint8_t pk[32];
    if (!s || !nseal_signer_public_key(s, pk, &e)) return fail(e);
    gboolean dup = FALSE;
    for (guint i = 0; i < rcpts->len; i++)
      dup |= memcmp(&g_array_index(rcpts, uint8_t, i * 32), pk, 32) == 0;
    if (!dup) g_array_append_vals(rcpts, pk, 1);
  }
  if (!pass && rcpts->len == 0) {
    g_printerr("nostr-seal: give at least one --to, --to-self or --passphrase\n");
    return FALSE;
  }

  g_autofree char *out_path = out ? g_strdup(out)
                            : g_str_equal(in_path, "-") ? NULL
                            : g_strconcat(in_path, NSEAL_SUFFIX, NULL);
  if (!out_path) { g_printerr("nostr-seal: reading stdin needs -o PATH (or -o -)\n"); return FALSE; }

  char *pw = NULL;
  if (pass && !(pw = get_passphrase(pass_file, TRUE, &e))) return fail(e);

  int in_fd = g_str_equal(in_path, "-") ? STDIN_FILENO : open(in_path, O_RDONLY | O_CLOEXEC);
  if (in_fd < 0) {
    int err = errno;
    wipe_free(pw);
    g_printerr("nostr-seal: %s: %s\n", in_path, g_strerror(err));
    return FALSE;
  }
  NsealOutput o;
  if (!nseal_output_open(&o, out_path, force, FALSE, &e)) { wipe_free(pw); if (in_fd != STDIN_FILENO) close(in_fd); return fail(e); }

  NsealEncryptOptions opts = {
    .recipients = (const uint8_t (*)[32])(void *)rcpts->data,
    .n_recipients = pass ? 0 : rcpts->len,
    .passphrase = pw,
    .log_n = (guint8)work,
    .chunk_log2 = (guint8)chunk_log2,
  };
  gboolean ok = nseal_encrypt_fd(in_fd, o.fd, &opts, &e);
  wipe_free(pw);
  if (in_fd != STDIN_FILENO) close(in_fd);
  ok = nseal_output_close(&o, ok, ok ? &e : NULL) && ok;
  if (!ok) return fail(e);
  if (!g_str_equal(out_path, "-")) g_printerr("sealed → %s\n", out_path);
  return TRUE;
}

static void print_header(const NsealHeader *h, FILE *f) {
  fprintf(f, "format: nsealed v%d, chunk size %u bytes\n", NSEAL_FORMAT_VERSION,
          1u << nseal_header_chunk_log2(h));
  if (nseal_header_is_passphrase(h)) { fprintf(f, "sealed with: passphrase (NIP-49)\n"); return; }
  for (gsize i = 0; i < nseal_header_n_stanzas(h); i++) {
    g_autofree char *npub = nseal_pubkey_to_npub(nseal_header_stanza_recipient(h, i));
    fprintf(f, "recipient: %s\n", npub ? npub : "?");
  }
}

static gboolean cmd_inspect(int argc, char **argv) {
  if (argc != 2) { g_printerr("usage: nostr-seal inspect FILE.nsealed\n"); return FALSE; }
  int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd < 0) { int err = errno; g_printerr("nostr-seal: %s: %s\n", argv[1], g_strerror(err)); return FALSE; }
  GError *e = NULL;
  g_autoptr(NsealHeader) h = nseal_header_read_fd(fd, &e);
  close(fd);
  if (!h) return fail(e);
  print_header(h, stdout);
  return TRUE;
}

static gboolean cmd_decrypt(int argc, char **argv) {
  g_autofree char *identity = NULL, *pass_file = NULL, *out = NULL;
  gboolean force = FALSE;
  GOptionEntry entries[] = {
    { "identity", 'i', 0, G_OPTION_ARG_STRING, &identity, "Signer identity to decrypt as (default: the signer's active identity)", "NPUB" },
    { "passphrase-file", 0, 0, G_OPTION_ARG_FILENAME, &pass_file, "Read the passphrase from the first line of FILE (- = stdin)", "FILE" },
    { "output", 'o', 0, G_OPTION_ARG_FILENAME, &out, "Output path (- = stdout; default FILE without .nsealed)", "PATH" },
    { "force", 'f', 0, G_OPTION_ARG_NONE, &force, "Overwrite an existing output", NULL },
    G_OPTION_ENTRY_NULL
  };
  g_autoptr(GOptionContext) ctx = g_option_context_new("FILE.nsealed — open a sealed file");
  g_option_context_add_main_entries(ctx, entries, NULL);
  GError *e = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &e)) return fail(e);
  if (argc != 2) { g_printerr("usage: nostr-seal decrypt [--identity NPUB] FILE.nsealed\n"); return FALSE; }
  const char *in_path = argv[1];

  g_autofree char *out_path = NULL;
  if (out) out_path = g_strdup(out);
  else if (g_str_has_suffix(in_path, NSEAL_SUFFIX) && strlen(in_path) > strlen(NSEAL_SUFFIX))
    out_path = g_strndup(in_path, strlen(in_path) - strlen(NSEAL_SUFFIX));
  else { g_printerr("nostr-seal: %s has no .nsealed suffix; pass -o PATH\n", in_path); return FALSE; }

  int in_fd = open(in_path, O_RDONLY | O_CLOEXEC);
  if (in_fd < 0) { int err = errno; g_printerr("nostr-seal: %s: %s\n", in_path, g_strerror(err)); return FALSE; }
  g_autoptr(NsealHeader) h = nseal_header_read_fd(in_fd, &e);
  if (!h) { close(in_fd); return fail(e); }

  NsealDecryptOptions opts = {0};
  g_autoptr(NsealSigner) signer = NULL;
  uint8_t me[32];
  char *pw = NULL;
  if (nseal_header_is_passphrase(h)) {
    if (!(pw = get_passphrase(pass_file, FALSE, &e))) { close(in_fd); return fail(e); }
    opts.passphrase = pw;
  } else {
    signer = nseal_signer_new(identity, &e);
    if (!signer || !nseal_signer_public_key(signer, me, &e)) { close(in_fd); return fail(e); }
    if (!nseal_header_has_recipient(h, me)) {
      g_autofree char *npub = nseal_pubkey_to_npub(me);
      g_printerr("nostr-seal: %s is not a recipient of %s\n", npub ? npub : "the signer identity", in_path);
      print_header(h, stderr);
      g_printerr("hint: pick one of the recipients with --identity NPUB\n");
      close(in_fd);
      return FALSE;
    }
    opts.identity_pubkey = me;
    opts.unwrap = nseal_signer_unwrap;
    opts.unwrap_data = signer;
    if (isatty(STDERR_FILENO)) g_printerr("asking the signer to open the key stanza (approve it in your signer)…\n");
  }

  NsealOutput o;
  if (!nseal_output_open(&o, out_path, force, TRUE, &e)) { wipe_free(pw); close(in_fd); return fail(e); }
  gboolean ok = nseal_decrypt_fd(in_fd, o.fd, &opts, &e);
  wipe_free(pw);
  close(in_fd);
  ok = nseal_output_close(&o, ok, ok ? &e : NULL) && ok;
  if (!ok) return fail(e);
  if (!g_str_equal(out_path, "-")) g_printerr("opened → %s\n", out_path);
  return TRUE;
}

static void usage(FILE *f) {
  fprintf(f,
    "usage: nostr-seal COMMAND [OPTIONS] FILE\n"
    "\n"
    "  encrypt --to NPUB [--to NPUB…] [--to-self] FILE   seal for Nostr public keys\n"
    "  encrypt --passphrase FILE                          seal with a passphrase (NIP-49)\n"
    "  decrypt [--identity NPUB] FILE.nsealed             open via org.nostr.Signer\n"
    "  inspect FILE.nsealed                               list recipients\n"
    "\n"
    "Run 'nostr-seal COMMAND --help' for options.\n");
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  if (argc < 2) { usage(stderr); return 2; }
  const char *cmd = argv[1];
  if (g_str_equal(cmd, "--help") || g_str_equal(cmd, "-h") || g_str_equal(cmd, "help")) { usage(stdout); return 0; }
  if (g_str_equal(cmd, "--version")) { printf("nostr-seal %s (nsealed v%d)\n", NOSTR_SEAL_VERSION, NSEAL_FORMAT_VERSION); return 0; }
  argv[1] = argv[0];
  gboolean ok;
  if (g_str_equal(cmd, "encrypt") || g_str_equal(cmd, "seal")) ok = cmd_encrypt(argc - 1, argv + 1);
  else if (g_str_equal(cmd, "decrypt") || g_str_equal(cmd, "open")) ok = cmd_decrypt(argc - 1, argv + 1);
  else if (g_str_equal(cmd, "inspect")) ok = cmd_inspect(argc - 1, argv + 1);
  else { usage(stderr); return 2; }
  return ok ? 0 : 1;
}
