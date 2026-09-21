/*
 * nostr-smb-acquire: desktop tool that obtains a short-lived SMB password
 * from the local nostr-authd broker via a Nostr proof and delivers it to
 * a Samba credentials= file (and/or stdout for piping).
 *
 * See nostr_smb_acquire.h for the module contract.  This file provides
 * both the testable core (nh_smb_acquire_run) and the thin main() wrapper.
 * The core does not fork, exec, prompt the tty, or read env vars — the
 * CLI wraps it with those concerns so the headless integration test can
 * drive the core over a socketpair without a real tty.
 *
 * Tracks beads nostrc-rb0e.8.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nostr_smb_acquire.h"

#include "auth_client.h"
#include "nostr_auth_protocol.h"
#include "secure_buf.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- *
 * Testable core.
 * -------------------------------------------------------------------------- */

static void wipe_stack(void *p, size_t n) {
  /* Route through secure_wipe so the wipe cannot be optimised away. */
  secure_wipe(p, n);
}

/* Map a broker protocol result to an acquire status.  Called only when
 * the transport itself succeeded and the result was NOT OK. */
static nh_smb_acquire_status map_broker_result(nh_auth_result r) {
  switch (r) {
    case NH_AUTH_RESULT_OK:
      return NH_SMB_ACQUIRE_OK;
    case NH_AUTH_RESULT_INVALID_PROOF:
    case NH_AUTH_RESULT_UNKNOWN_ACCOUNT:
    case NH_AUTH_RESULT_NOT_READY:
      return NH_SMB_ACQUIRE_ERR_PROOF;
    case NH_AUTH_RESULT_DENIED:
    case NH_AUTH_RESULT_DISABLED:
      return NH_SMB_ACQUIRE_ERR_DENIED;
    case NH_AUTH_RESULT_EXPIRED:
      return NH_SMB_ACQUIRE_ERR_EXPIRED;
    case NH_AUTH_RESULT_RATE_LIMITED:
      return NH_SMB_ACQUIRE_ERR_RATE_LIMITED;
    default:
      return NH_SMB_ACQUIRE_ERR_OTHER;
  }
}

/* Write a Samba-style credentials file at `path`, mode 0600.  Overwrites
 * any pre-existing file at the same path.  The password bytes are held
 * only in `buf` (stack) here and are wiped before we return. */
static int write_credentials_file(const char *path,
                                  const char *username,
                                  const char *pw, size_t pw_len,
                                  FILE *err) {
  /* Enforce a hard bound to keep the stack buffer safe.  password_len is
   * already bounded by NH_AUTH_SMB_PASSWORD_MAX_LEN (64), and the username
   * by NH_IDENTITY_USERNAME_CAP (~64), so 512 leaves head room. */
  char buf[512];
  int off = snprintf(buf, sizeof buf, "username=%s\npassword=",
                     username ? username : "");
  if (off < 0 || (size_t)off + pw_len + 1 >= sizeof buf) {
    wipe_stack(buf, sizeof buf);
    if (err) fprintf(err, "nostr-smb-acquire: credentials line too long\n");
    return -1;
  }
  memcpy(buf + off, pw, pw_len);
  off += (int)pw_len;
  buf[off++] = '\n';

  /* Unlink first: we want a fresh inode with mode 0600 owned by us.
   * Ignore ENOENT; anything else is fine — O_EXCL will catch a race. */
  if (unlink(path) != 0 && errno != ENOENT) {
    wipe_stack(buf, sizeof buf);
    if (err) fprintf(err, "nostr-smb-acquire: unlink(%s): %s\n", path,
                     strerror(errno));
    return -1;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    wipe_stack(buf, sizeof buf);
    if (err) fprintf(err, "nostr-smb-acquire: open(%s): %s\n", path,
                     strerror(errno));
    return -1;
  }
  /* Belt-and-braces: some umasks / filesystems may still narrow further,
   * but we do NOT want to widen.  Force exact 0600 with fchmod. */
  if (fchmod(fd, 0600) != 0) {
    int e = errno;
    close(fd);
    (void)unlink(path);
    wipe_stack(buf, sizeof buf);
    if (err) fprintf(err, "nostr-smb-acquire: fchmod(%s): %s\n", path,
                     strerror(e));
    return -1;
  }
  ssize_t total = 0;
  while (total < off) {
    ssize_t n = write(fd, buf + total, (size_t)(off - total));
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      close(fd);
      (void)unlink(path);
      wipe_stack(buf, sizeof buf);
      if (err) fprintf(err, "nostr-smb-acquire: write(%s): %s\n", path,
                       strerror(e));
      return -1;
    }
    total += n;
  }
  if (close(fd) != 0) {
    int e = errno;
    (void)unlink(path);
    wipe_stack(buf, sizeof buf);
    if (err) fprintf(err, "nostr-smb-acquire: close(%s): %s\n", path,
                     strerror(e));
    return -1;
  }
  wipe_stack(buf, sizeof buf);
  return 0;
}

static int write_stdout_password(FILE *out, const char *pw, size_t pw_len,
                                 FILE *err) {
  if (fwrite(pw, 1, pw_len, out) != pw_len ||
      fputc('\n', out) == EOF) {
    if (err) fprintf(err, "nostr-smb-acquire: stdout write failed: %s\n",
                     strerror(errno));
    return -1;
  }
  if (fflush(out) != 0) {
    if (err) fprintf(err, "nostr-smb-acquire: stdout flush failed: %s\n",
                     strerror(errno));
    return -1;
  }
  return 0;
}

nh_smb_acquire_status nh_smb_acquire_run(int fd,
                                         const char *service,
                                         const char *provider,
                                         const char *passphrase,
                                         const nh_smb_acquire_sink *sink,
                                         nh_auth_result *result_out,
                                         nh_smb_acquire_diag *diag_out,
                                         FILE *err_stream) {
  nh_auth_result scratch = NH_AUTH_RESULT_INTERNAL_ERROR;
  if (!result_out) result_out = &scratch;
  *result_out = NH_AUTH_RESULT_INTERNAL_ERROR;

  if (diag_out) memset(diag_out, 0, sizeof *diag_out);

  if (fd < 0 || !provider || !sink) {
    if (err_stream) fprintf(err_stream, "nostr-smb-acquire: bad arguments\n");
    return NH_SMB_ACQUIRE_ERR_INTERNAL;
  }
  if (!sink->creds_path && !sink->stdout_stream) {
    if (err_stream)
      fprintf(err_stream, "nostr-smb-acquire: no delivery sink configured\n");
    return NH_SMB_ACQUIRE_ERR_INTERNAL;
  }
  if (!service) service = NH_SMB_ACQUIRE_DEFAULT_SERVICE;

  nh_auth_smb_envelope env;
  memset(&env, 0, sizeof env);

  int rc = nh_auth_client_smb_proof_with(fd, service, provider,
                                         passphrase, &env, result_out);
  if (rc != 0) {
    if (err_stream)
      fprintf(err_stream,
              "nostr-smb-acquire: transport failure driving SMB proof\n");
    nh_auth_smb_envelope_clear(&env);
    if (diag_out) diag_out->envelope_cleared = (env.password.ptr == NULL);
    return NH_SMB_ACQUIRE_ERR_TRANSPORT;
  }

  if (*result_out != NH_AUTH_RESULT_OK) {
    if (err_stream)
      fprintf(err_stream,
              "nostr-smb-acquire: broker rejected proof: %s\n",
              nh_auth_result_name(*result_out));
    nh_auth_smb_envelope_clear(&env);
    if (diag_out) diag_out->envelope_cleared = (env.password.ptr == NULL);
    return map_broker_result(*result_out);
  }

  /* Sanity: the broker should have populated the envelope. */
  if (!env.password.ptr || env.password_len == 0 ||
      env.password_len > NH_AUTH_SMB_PASSWORD_MAX_LEN ||
      env.username[0] == '\0') {
    if (err_stream)
      fprintf(err_stream,
              "nostr-smb-acquire: malformed envelope from broker\n");
    nh_auth_smb_envelope_clear(&env);
    if (diag_out) diag_out->envelope_cleared = (env.password.ptr == NULL);
    return NH_SMB_ACQUIRE_ERR_INTERNAL;
  }

  if (diag_out) {
    snprintf(diag_out->account_username, sizeof diag_out->account_username,
             "%s", env.username);
  }

  /* Deliver.  Both sinks may be configured; if either fails, we clear
   * the envelope and any partially-written file before returning. */
  const char *pw = (const char *)env.password.ptr;
  size_t pw_len = env.password_len;

  bool file_ok = false, stdout_ok = false;
  if (sink->creds_path) {
    if (write_credentials_file(sink->creds_path, env.username, pw, pw_len,
                               err_stream) == 0) {
      file_ok = true;
    }
  }
  if (sink->stdout_stream) {
    /* Attempt stdout even if the file sink failed, then fail overall.
     * (If the file failed we still unlink below; the pipe read is a
     * one-shot and there's no meaningful rollback.) */
    if (write_stdout_password(sink->stdout_stream, pw, pw_len,
                              err_stream) == 0) {
      stdout_ok = true;
    }
  }

  /* Wipe the envelope BEFORE we return so no code below observes the
   * plaintext.  This wipes env.password's mlock'd buffer via secure_free. */
  nh_auth_smb_envelope_clear(&env);
  if (diag_out) {
    diag_out->envelope_cleared = (env.password.ptr == NULL);
    diag_out->creds_file_written = file_ok;
    diag_out->stdout_written = stdout_ok;
  }

  bool wanted_file = (sink->creds_path != NULL);
  bool wanted_stdout = (sink->stdout_stream != NULL);
  if ((wanted_file && !file_ok) || (wanted_stdout && !stdout_ok)) {
    /* If the file sink was partially written but the stdout sink failed,
     * remove the file so the user is not left with a valid credentials
     * file they did not receive over the pipe. */
    if (file_ok && sink->creds_path) (void)unlink(sink->creds_path);
    return NH_SMB_ACQUIRE_ERR_DELIVERY;
  }
  return NH_SMB_ACQUIRE_OK;
}

/* -------------------------------------------------------------------------- *
 * Thin CLI wrapper.
 * -------------------------------------------------------------------------- */

#ifndef NH_SMB_ACQUIRE_NO_MAIN

static void print_usage(FILE *out, const char *argv0) {
  fprintf(out,
    "Usage: %s [options]\n"
    "\n"
    "Obtain a short-lived SMB password for your Nostr-homed account from\n"
    "the local auth broker and deliver it locally.\n"
    "\n"
    "Options:\n"
    "  --socket PATH     Path to the broker user.sock (default: %s;\n"
    "                    override with $%s).\n"
    "  --service NAME    Broker service token (default: %s).\n"
    "  --provider NAME   Unlock provider: local | nip46 (default: local).\n"
    "  --file PATH       Write Samba credentials= file here (default:\n"
    "                    $XDG_RUNTIME_DIR/nostr-smb/credentials, 0600).\n"
    "  --stdout          Print only the password to stdout (for piping).\n"
    "                    May be combined with --file.\n"
    "  --no-file         Disable the credentials file sink (requires --stdout).\n"
    "  -h, --help        Show this help.\n"
    "\n"
    "The local provider passphrase is read from tty (getpass) or from\n"
    "$%s if set.\n"
    "\n"
    "Exit codes: 0 ok; 2 proof failed; 3 denied; 4 expired; 5 rate\n"
    "limited; 6 delivery error; 1 other/transport/internal.\n",
    argv0, NH_SMB_ACQUIRE_DEFAULT_SOCKET, NH_SMB_ACQUIRE_ENV_SOCKET,
    NH_SMB_ACQUIRE_DEFAULT_SERVICE, NH_SMB_ACQUIRE_ENV_PASSPHRASE);
}

static int cli_exit_code(nh_smb_acquire_status st) {
  switch (st) {
    case NH_SMB_ACQUIRE_OK:              return 0;
    case NH_SMB_ACQUIRE_ERR_PROOF:       return 2;
    case NH_SMB_ACQUIRE_ERR_DENIED:      return 3;
    case NH_SMB_ACQUIRE_ERR_EXPIRED:     return 4;
    case NH_SMB_ACQUIRE_ERR_RATE_LIMITED: return 5;
    case NH_SMB_ACQUIRE_ERR_DELIVERY:    return 6;
    case NH_SMB_ACQUIRE_ERR_TRANSPORT:
    case NH_SMB_ACQUIRE_ERR_INTERNAL:
    case NH_SMB_ACQUIRE_ERR_OTHER:
    default:                             return 1;
  }
}

/* Return a heap-allocated default creds file path, or NULL on failure.
 * Falls back to /tmp/nostr-smb-<uid>/credentials if XDG_RUNTIME_DIR is
 * unset — the tool will still work for a smoke test without a session. */
static char *default_creds_path(FILE *err) {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  char buf[PATH_MAX];
  const char *base;
  char fallback[PATH_MAX];
  if (xdg && xdg[0] == '/') {
    base = xdg;
  } else {
    snprintf(fallback, sizeof fallback, "/tmp/nostr-smb-%u",
             (unsigned)geteuid());
    base = fallback;
  }
  int n1 = snprintf(buf, sizeof buf, "%s/nostr-smb", base);
  if (n1 < 0 || (size_t)n1 >= sizeof buf) {
    if (err) fprintf(err, "nostr-smb-acquire: creds dir path too long\n");
    return NULL;
  }
  /* mkdir -p (one level).  0700 is plenty; parents (XDG_RUNTIME_DIR) are
   * already user-private per the freedesktop spec. */
  if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
    if (err) fprintf(err, "nostr-smb-acquire: mkdir(%s): %s\n", buf,
                     strerror(errno));
    return NULL;
  }
  char full[PATH_MAX];
  int n2 = snprintf(full, sizeof full, "%s/credentials", buf);
  if (n2 < 0 || (size_t)n2 >= sizeof full) {
    if (err) fprintf(err, "nostr-smb-acquire: creds path too long\n");
    return NULL;
  }
  return strdup(full);
}

/* Read the local-provider passphrase.  Env var wins for scripting; otherwise
 * getpass() prompts on the tty.  Returned pointer is either a static buffer
 * (getpass) or a heap dup of the env value; the caller must wipe it and,
 * for the env case, free it.  Returns NULL on failure. */
static char *read_passphrase(FILE *err, bool *out_heap) {
  *out_heap = false;
  const char *env = getenv(NH_SMB_ACQUIRE_ENV_PASSPHRASE);
  if (env && env[0]) {
    char *dup = strdup(env);
    if (!dup) {
      if (err) fprintf(err, "nostr-smb-acquire: out of memory\n");
      return NULL;
    }
    *out_heap = true;
    return dup;
  }
  /* getpass writes to /dev/tty; if there is no tty this returns NULL. */
  char *pw = getpass("Passphrase: ");
  if (!pw) {
    if (err) fprintf(err, "nostr-smb-acquire: no tty for passphrase; set $%s\n",
                     NH_SMB_ACQUIRE_ENV_PASSPHRASE);
    return NULL;
  }
  return pw;
}

int main(int argc, char **argv) {
  const char *socket_path = getenv(NH_SMB_ACQUIRE_ENV_SOCKET);
  if (!socket_path || !socket_path[0])
    socket_path = NH_SMB_ACQUIRE_DEFAULT_SOCKET;
  const char *service = NH_SMB_ACQUIRE_DEFAULT_SERVICE;
  const char *provider = NH_AUTH_PROVIDER_NAME_LOCAL;
  const char *cli_file = NULL;
  bool want_stdout = false;
  bool no_file = false;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
      print_usage(stdout, argv[0]);
      return 0;
    } else if (!strcmp(a, "--stdout")) {
      want_stdout = true;
    } else if (!strcmp(a, "--no-file")) {
      no_file = true;
    } else if (!strcmp(a, "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (!strcmp(a, "--service") && i + 1 < argc) {
      service = argv[++i];
    } else if (!strcmp(a, "--provider") && i + 1 < argc) {
      provider = argv[++i];
    } else if (!strcmp(a, "--file") && i + 1 < argc) {
      cli_file = argv[++i];
    } else {
      fprintf(stderr, "nostr-smb-acquire: unknown argument: %s\n", a);
      print_usage(stderr, argv[0]);
      return 1;
    }
  }
  if (no_file && !want_stdout) {
    fprintf(stderr, "nostr-smb-acquire: --no-file requires --stdout\n");
    return 1;
  }

  char *default_path = NULL;
  const char *creds_path = cli_file;
  if (!creds_path && !no_file) {
    default_path = default_creds_path(stderr);
    if (!default_path) return 1;
    creds_path = default_path;
  }

  /* Passphrase (local provider only). */
  char *pw = NULL; bool pw_heap = false;
  if (!strcmp(provider, NH_AUTH_PROVIDER_NAME_LOCAL)) {
    pw = read_passphrase(stderr, &pw_heap);
    if (!pw) { free(default_path); return 1; }
  }

  int fd = -1;
  int cc = nh_auth_client_connect(socket_path, &fd);
  if (cc != 0) {
    fprintf(stderr, "nostr-smb-acquire: connect(%s): %s\n", socket_path,
            strerror(errno));
    if (pw) {
      size_t n = strlen(pw);
      wipe_stack(pw, n);
      if (pw_heap) free(pw);
    }
    free(default_path);
    return 1;
  }

  nh_smb_acquire_sink sink = {
    .creds_path = no_file ? NULL : creds_path,
    .stdout_stream = want_stdout ? stdout : NULL,
  };
  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
  nh_smb_acquire_diag diag;
  nh_smb_acquire_status st =
      nh_smb_acquire_run(fd, service, provider, pw, &sink, &r, &diag, stderr);

  nh_auth_client_close(fd);
  if (pw) {
    size_t n = strlen(pw);
    wipe_stack(pw, n);
    if (pw_heap) free(pw);
  }
  free(default_path);

  if (st == NH_SMB_ACQUIRE_OK && sink.creds_path) {
    fprintf(stderr, "nostr-smb-acquire: wrote %s (user=%s)\n",
            sink.creds_path,
            diag.account_username[0] ? diag.account_username : "?");
  }
  return cli_exit_code(st);
}

#endif /* NH_SMB_ACQUIRE_NO_MAIN */
