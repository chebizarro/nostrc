/* pam_nostr.so — broker-backed authentication (nostrc-zcll.5 [B4], .6 [B5]).
 *
 * Replaces the legacy cache-accept flow: authentication succeeds only when the
 * privileged broker verifies a fresh cryptographic proof over auth.sock. The
 * module is stateless glue — no key material, no policy decision of its own.
 *
 * B5 adds the GDM/PAM provider-choice UX and retry budget:
 *
 *   1. BEGIN_LOGIN reports the account's enabled providers.
 *   2. If exactly one provider is enabled, it is selected silently.
 *   3. If more than one is enabled and the caller pinned a provider via the
 *      `provider=` module arg, that provider is used non-interactively.
 *   4. Otherwise the module prompts (PAM_PROMPT_ECHO_ON) accepting only
 *      trimmed lowercase ASCII "local" or "remote"/"nip46".
 *   5. For the local encrypted vault the module prompts for a passphrase
 *      (PAM_PROMPT_ECHO_OFF) and forwards it as the SUBMIT_UNLOCK secret.
 *   6. For the external NIP-46 signer the module emits a PAM_TEXT_INFO
 *      approval message and forwards no passphrase; the bunker gates approval
 *      out-of-band.
 *   7. The module enforces a per-invocation budget of at most
 *      NH_AUTH_PAM_MAX_INVALID_ATTEMPTS (3) invalid attempts across the choice
 *      and the passphrase steps, returning PAM_MAXTRIES when exhausted.
 *
 * Module args:
 *   socket=<path>              default /run/nostr-auth/auth.sock
 *   provider=local|nip46|remote pins a provider (skips the interactive choice)
 */
#define _GNU_SOURCE
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include "auth_client.h"
#include "nostr_auth_protocol.h"
#include "pam_qr_render.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef NH_PAM_DEFAULT_SOCKET
#define NH_PAM_DEFAULT_SOCKET "/run/nostr-auth/auth.sock"
#endif

static const char *socket_path(int argc, const char **argv) {
  for (int i = 0; i < argc; i++)
    if (!strncmp(argv[i], "socket=", 7) && argv[i][7]) return argv[i] + 7;
  return NH_PAM_DEFAULT_SOCKET;
}

/* Returns the canonical provider name pinned by `provider=` (or NULL). */
static const char *pinned_provider(int argc, const char **argv) {
  for (int i = 0; i < argc; i++)
    if (!strncmp(argv[i], "provider=", 9))
      return nh_auth_provider_choice_parse(argv[i] + 9);
  return NULL;
}

/* Map a broker result to a PAM code. The mapping is the contract the GDM/system
 * stacking relies on (see packaging/pam/gdm-password.sample, beads nostrc-o1ho):
 *
 *   OK               -> PAM_SUCCESS         (accept; stack: success=done)
 *   UNKNOWN_ACCOUNT  -> PAM_USER_UNKNOWN    (not a nostr account; stack:
 *                                            user_unknown=ignore -> fall through
 *                                            to the Unix stack for local users)
 *   broker down /    -> PAM_AUTHINFO_UNAVAIL (transient; stack:
 *   transport error                          authinfo_unavail=ignore -> fall
 *                                            through so a stopped broker never
 *                                            bricks local login)
 *   DISABLED         -> PAM_ACCT_EXPIRED    } a KNOWN account that failed:
 *   RATE_LIMITED     -> PAM_MAXTRIES        } these are HARD failures. The stack
 *   INVALID_PROOF /  -> PAM_AUTH_ERR        } uses default=die so a failed nostr
 *   DENIED / etc.                           } proof is denied and is NEVER
 *                                             downgraded to Unix password auth.
 *
 * The unlisted `default` maps to PAM_AUTHINFO_UNAVAIL (treated as transient),
 * never to a silent success, so an unexpected result can only fall through to
 * the Unix stack, not accept. */
static int map_result(nh_auth_result r) {
  switch (r) {
    case NH_AUTH_RESULT_OK: return PAM_SUCCESS;
    case NH_AUTH_RESULT_UNKNOWN_ACCOUNT: return PAM_USER_UNKNOWN;
    case NH_AUTH_RESULT_DISABLED: return PAM_ACCT_EXPIRED;
    case NH_AUTH_RESULT_RATE_LIMITED: return PAM_MAXTRIES;
    case NH_AUTH_RESULT_NOT_READY:
    case NH_AUTH_RESULT_DENIED:
    case NH_AUTH_RESULT_INVALID_PROOF:
    case NH_AUTH_RESULT_EXPIRED:
    case NH_AUTH_RESULT_CANCELLED:
    case NH_AUTH_RESULT_INTERACTION_REQUIRED: return PAM_AUTH_ERR;
    default: return PAM_AUTHINFO_UNAVAIL;
  }
}

/* Prompts once via the PAM conversation. `style` is PAM_PROMPT_ECHO_ON /
 * PAM_PROMPT_ECHO_OFF / PAM_TEXT_INFO. On PAM_SUCCESS with a non-info style,
 * *out_response is a heap-allocated response the caller must free (and
 * memset-wipe if it carries a secret). For info messages *out_response is
 * NULL. */
static int converse(pam_handle_t *pamh, int style, const char *msg,
                    char **out_response) {
  if (out_response) *out_response = NULL;
  char *resp = NULL;
  int rc = pam_prompt(pamh, style, &resp, "%s", msg);
  if (rc != PAM_SUCCESS) { if (resp) free(resp); return rc; }
  if (style == PAM_TEXT_INFO || style == PAM_ERROR_MSG) {
    if (resp) free(resp);
    return PAM_SUCCESS;
  }
  if (!resp) return PAM_AUTHINFO_UNAVAIL;
  if (out_response) *out_response = resp;
  else free(resp);
  return PAM_SUCCESS;
}

/* Resolves the provider to use for this authentication.
 *
 * `pinned` is the canonical name from provider=<...> (or NULL). `providers` is
 * the set the broker reported for the account. `attempts_left` is decremented
 * for every invalid choice the user types; when it reaches zero, the function
 * returns PAM_MAXTRIES. On success *chosen_out is a static canonical name. */
static int resolve_provider(pam_handle_t *pamh, const char *pinned,
                            const nh_auth_provider_list *providers,
                            unsigned int *attempts_left,
                            const char **chosen_out) {
  *chosen_out = NULL;
  if (providers->count == 0) return PAM_AUTHINFO_UNAVAIL;

  if (pinned) {
    if (!nh_auth_provider_list_has(providers, pinned)) {
      pam_syslog(pamh, LOG_ERR,
                 "nostr: pinned provider=%s not enabled for account", pinned);
      return PAM_AUTH_ERR;
    }
    *chosen_out = pinned;
    return PAM_SUCCESS;
  }

  if (providers->count == 1) {
    *chosen_out = providers->names[0];
    return PAM_SUCCESS;
  }

  /* Interactive choice. Loop until the user types an accepted token or the
   * per-invocation invalid-attempt budget is exhausted. */
  const char *prompt = "Sign in with (local/remote/qr): ";
  while (*attempts_left > 0) {
    char *reply = NULL;
    int rc = converse(pamh, PAM_PROMPT_ECHO_ON, prompt, &reply);
    if (rc != PAM_SUCCESS) return rc;
    const char *canonical = nh_auth_provider_choice_parse(reply);
    if (reply) free(reply);
    if (canonical && nh_auth_provider_list_has(providers, canonical)) {
      *chosen_out = canonical;
      return PAM_SUCCESS;
    }
    (*attempts_left)--;
    if (*attempts_left == 0) break;
    pam_error(pamh, "Unknown provider. Type 'local', 'remote', or 'qr'.");
  }
  return PAM_MAXTRIES;
}

static void wipe(char *s) {
  if (!s) return;
  volatile char *p = (volatile char *)s;
  while (*p) *p++ = 0;
}

/* Optional greeter-artifact PNG producer (design §5.3, §6, decision D3).
 * The broker publishes /run/nostr-auth/greeter/current.json; the greeter
 * extension expects a sibling current.png. Rendering happens here (not in
 * the broker) so qrcodegen stays out of the root daemon.
 *
 * Called only when the PAM conversation is a graphical greeter (the text
 * console has the half-block QR inline). Best-effort: any I/O failure is a
 * no-op — the pairing code + URI still show. */
#ifndef NH_PAM_GREETER_DIR
#define NH_PAM_GREETER_DIR "/run/nostr-auth/greeter"
#endif
#ifndef NH_PAM_GREETER_PNG
#define NH_PAM_GREETER_PNG "current.png"
#endif

static const char *pam_greeter_dir(void) {
  const char *env = getenv("NOSTR_HOMED_GREETER_DIR");
  return (env && env[0]) ? env : NH_PAM_GREETER_DIR;
}

static void publish_greeter_png(const nh_auth_display *disp) {
  if (!disp || !disp->uri[0]) return;
  const char *dir = pam_greeter_dir();
  struct stat st;
  if (stat(dir, &st) != 0) {
    if (mkdir(dir, 0755) != 0) return;
  } else if (!S_ISDIR(st.st_mode)) {
    return;
  }
  unsigned char *png = NULL;
  size_t plen = 0;
  if (nh_pam_qr_render_png(disp->uri, 9, 8, &png, &plen) != NH_QR_RENDER_OK ||
      !png)
    return;
  char tmp[512], dst[512];
  int n1 = snprintf(dst, sizeof dst, "%s/%s", dir, NH_PAM_GREETER_PNG);
  int n2 = snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, NH_PAM_GREETER_PNG);
  if (n1 <= 0 || (size_t)n1 >= sizeof dst || n2 <= 0 ||
      (size_t)n2 >= sizeof tmp) { free(png); return; }
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) { free(png); return; }
  ssize_t off = 0;
  while ((size_t)off < plen) {
    ssize_t w = write(fd, png + off, plen - (size_t)off);
    if (w < 0) { close(fd); unlink(tmp); free(png); return; }
    off += w;
  }
  close(fd);
  if (rename(tmp, dst) != 0) unlink(tmp);
  free(png);
}

/* Heuristic: is this PAM conversation running on a text console? The design
 * (docs/reviews/qr-greeter-render-spike-2026-09-22.md) rules the half-block
 * QR NO-GO in the graphical greeter (proportional Cantarell + label wrap),
 * so we only emit it when we can be reasonably sure the client renders in a
 * monospace terminal — real TTY / ssh / pamtester. Wrong-way false negatives
 * are safe (the URI + pairing code always ship); the danger is a wrong-way
 * false positive that mangles the greeter, so err on the side of "no QR". */
static int is_text_console(pam_handle_t *pamh, const char *service) {
  const char *xdisplay = NULL;
#ifdef PAM_XDISPLAY
  pam_get_item(pamh, PAM_XDISPLAY, (const void **)&xdisplay);
  if (xdisplay && xdisplay[0]) return 0;
#endif
  const char *tty = NULL;
  pam_get_item(pamh, PAM_TTY, (const void **)&tty);
  if (tty && tty[0]) {
    /* A graphical greeter passes ":0" / ":1" / ":wayland-0" as PAM_TTY. */
    if (tty[0] == ':') return 0;
    if (!strncmp(tty, "/dev/tty", 8)) return 1;
    if (!strncmp(tty, "tty", 3)) return 1;
    if (!strncmp(tty, "pts/", 4) || !strncmp(tty, "/dev/pts/", 9)) return 1;
    /* ssh sets PAM_TTY = "ssh". */
    if (!strcmp(tty, "ssh")) return 1;
    if (!strcmp(tty, "console")) return 1;
  }
  if (service) {
    if (!strncmp(service, "gdm", 3)) return 0;
    if (!strcmp(service, "lightdm") || !strcmp(service, "sddm") ||
        !strcmp(service, "xdm")) return 0;
  }
  /* Default to text — pamtester / lockable services / init-style ttys
   * never set PAM_XDISPLAY. */
  return 1;
}

/* Render the QR pane the PAM module hands the user. On the graphical
 * greeter we ship pairing + hint only (the greeter extension renders the
 * real PNG from /run/nostr-auth/greeter/current.png). On a text console we
 * additionally embed a half-block QR of the URI so a user can scan
 * directly. The whole message is emitted in ONE PAM_TEXT_INFO — splitting
 * would hide the QR on shells that only show the most recent info line
 * (design §3.4 emission rule). */
static void render_qr_info(pam_handle_t *pamh, const nh_auth_display *disp,
                           int text_console) {
  if (!disp || !disp->uri[0]) return;
  const char *hint = disp->hint[0]
                         ? disp->hint
                         : "Scan this with your Nostr signer app";

  if (!text_console) {
    /* Graphical greeter: short pairing code + hint only. */
    char msg[256];
    snprintf(msg, sizeof msg,
             "%s\nPairing code: %s\nWaiting for your signer\xe2\x80\xa6",
             hint, disp->pairing_code);
    pam_info(pamh, "%s", msg);
    return;
  }

  /* Text console: half-block QR + URI + pairing code. ECC L, version cap 9
   * per design D11. */
  char *qr_txt = NULL;
  size_t qr_len = 0;
  nh_qr_render_rc rr = nh_pam_qr_render_halfblock(disp->uri, 9, &qr_txt,
                                                  &qr_len);
  size_t need = qr_len + strlen(disp->uri) + strlen(disp->pairing_code) + 256;
  char *buf = malloc(need);
  if (!buf) { free(qr_txt); return; }
  if (rr == NH_QR_RENDER_OK && qr_txt) {
    snprintf(buf, need,
             "%s:\n\n%s\nOr open this link on your phone:\n%s\n\n"
             "Pairing code: %s",
             hint, qr_txt, disp->uri, disp->pairing_code);
  } else {
    snprintf(buf, need,
             "%s\nOpen this link on your phone or paste into your Nostr signer:\n%s\n\n"
             "Pairing code: %s",
             hint, disp->uri, disp->pairing_code);
  }
  pam_info(pamh, "%s", buf);
  free(buf);
  free(qr_txt);
}

/* Adapter passed to nh_auth_client_submit_selection_display so the QR
 * display is rendered exactly once between SELECT_PROVIDER and the
 * blocking SUBMIT_UNLOCK (design §5.4 render-then-wait sequence). */
typedef struct qr_render_ctx {
  pam_handle_t *pamh;
  int text_console;
} qr_render_ctx;

static void qr_display_cb(void *ctx, const nh_auth_display *display) {
  qr_render_ctx *c = ctx;
  render_qr_info(c->pamh, display, c->text_console);
  if (!c->text_console) publish_greeter_png(display);
}

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                        const char **argv) {
  (void)flags;
  const char *user = NULL;
  if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user || !user[0])
    return PAM_USER_UNKNOWN;
  const char *service = NULL;
  pam_get_item(pamh, PAM_SERVICE, (const void **)&service);
  if (!service || !service[0]) service = "nostr-login";

  const char *sock = socket_path(argc, argv);
  const char *pinned = pinned_provider(argc, argv);

  unsigned int attempts_left = NH_AUTH_PAM_MAX_INVALID_ATTEMPTS;

  /* Learn the account's providers via a dedicated connection. Keeping this on
   * its own fd means selection / passphrase retries always begin with a fresh
   * transaction — the broker only owns one transaction per connection. */
  int fd = -1;
  if (nh_auth_client_connect(sock, &fd) != 0) {
    pam_syslog(pamh, LOG_ERR, "nostr: cannot reach broker at %s", sock);
    return PAM_AUTHINFO_UNAVAIL;
  }
  nh_auth_provider_list providers;
  nh_auth_result begin_result = NH_AUTH_RESULT_INTERNAL_ERROR;
  int trc = nh_auth_client_begin_login(fd, user, service, &providers,
                                       &begin_result);
  nh_auth_client_close(fd);
  if (trc != 0) {
    pam_syslog(pamh, LOG_ERR, "nostr: broker transport failure for %s", user);
    return PAM_AUTHINFO_UNAVAIL;
  }
  if (begin_result != NH_AUTH_RESULT_OK) {
    pam_syslog(pamh, LOG_NOTICE, "nostr: begin_login %s -> %s", user,
               nh_auth_result_name(begin_result));
    return map_result(begin_result);
  }

  const char *chosen = NULL;
  int prc = resolve_provider(pamh, pinned, &providers, &attempts_left, &chosen);
  if (prc != PAM_SUCCESS) return prc;

  int is_nip46 = !strcmp(chosen, NH_AUTH_PROVIDER_NAME_NIP46);
  int is_qr = !strcmp(chosen, NH_AUTH_PROVIDER_NAME_NIP46_QR);
  if (is_nip46)
    pam_info(pamh,
             "Approve the sign-in request on your Nostr signer (bunker).");
  int text_console = is_text_console(pamh, service);

  /* Retry loop. Local: passphrase re-prompt on each attempt. NIP-46 bunker:
   * one attempt (bunker denial is terminal). NIP-46 QR: one attempt (we
   * mint a fresh URI + pairing per retry via the broker, so the outer
   * budget still applies but no re-prompt on this side). */
  int last_pam_rc = PAM_AUTH_ERR;
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  while (attempts_left > 0) {
    char *pass = NULL;
    if (!is_nip46 && !is_qr) {
      /* Force a re-prompt by clearing any cached PAM_AUTHTOK so failed retries
       * do not silently replay the previous input. */
      pam_set_item(pamh, PAM_AUTHTOK, NULL);
      int rc = converse(pamh, PAM_PROMPT_ECHO_OFF, "Nostr passphrase: ", &pass);
      if (rc != PAM_SUCCESS) { if (pass) { wipe(pass); free(pass); } return rc; }
      pam_set_item(pamh, PAM_AUTHTOK, pass);
    }

    fd = -1;
    if (nh_auth_client_connect(sock, &fd) != 0) {
      if (pass) { wipe(pass); free(pass); }
      pam_syslog(pamh, LOG_ERR, "nostr: cannot reach broker at %s", sock);
      return PAM_AUTHINFO_UNAVAIL;
    }
    if (is_qr) {
      /* Split flow: begin_login first so we can then run the display-aware
       * select/submit split (design §5.4). */
      nh_auth_provider_list disc;
      nh_auth_result br = NH_AUTH_RESULT_INTERNAL_ERROR;
      trc = nh_auth_client_begin_login(fd, user, service, &disc, &br);
      if (trc != 0 || br != NH_AUTH_RESULT_OK) {
        nh_auth_client_close(fd);
        pam_syslog(pamh, LOG_ERR, "nostr: qr begin_login %s -> %s", user,
                   nh_auth_result_name(br));
        return trc != 0 ? PAM_AUTHINFO_UNAVAIL : map_result(br);
      }
      qr_render_ctx rctx = {pamh, text_console};
      trc = nh_auth_client_submit_selection_display(
          fd, chosen, NULL, NULL, qr_display_cb, &rctx, &result);
    } else {
      trc = nh_auth_client_login_with(fd, user, service, chosen, pass, &result);
    }
    nh_auth_client_close(fd);
    if (pass) { wipe(pass); free(pass); }
    if (trc != 0) {
      pam_syslog(pamh, LOG_ERR, "nostr: broker transport failure for %s",
                 user);
      return PAM_AUTHINFO_UNAVAIL;
    }

    last_pam_rc = map_result(result);
    pam_syslog(pamh, last_pam_rc == PAM_SUCCESS ? LOG_INFO : LOG_NOTICE,
               "nostr: authenticate %s (%s) -> %s", user, chosen,
               nh_auth_result_name(result));
    if (result == NH_AUTH_RESULT_OK) return PAM_SUCCESS;

    /* Only invalid-proof / denied-provider errors consume a retry slot. Other
     * failures (unknown account, disabled, storage errors) are terminal. */
    int retryable = (result == NH_AUTH_RESULT_INVALID_PROOF ||
                     result == NH_AUTH_RESULT_DENIED);
    if (!retryable) return last_pam_rc;
    attempts_left--;
    if (attempts_left == 0) break;
    if (is_nip46 || is_qr) {
      /* Signer denied approval or the scan window elapsed; do not silently
       * loop forever. */
      return last_pam_rc;
    }
    pam_error(pamh, "Nostr passphrase incorrect. Try again.");
  }
  return PAM_MAXTRIES;
}

int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)pamh; (void)flags; (void)argc; (void)argv;
  return PAM_SUCCESS;
}

int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc,
                        const char **argv) {
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL;
  pam_get_user(pamh, &user, NULL);
  pam_syslog(pamh, LOG_INFO, "nostr: open_session %s", user ? user : "?");
  /* Local homes are provisioned at enrollment; nothing to mount here. */
  return PAM_SUCCESS;
}

int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc,
                         const char **argv) {
  (void)pamh; (void)flags; (void)argc; (void)argv;
  return PAM_SUCCESS;
}

int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc,
                     const char **argv) {
  (void)flags;
  const char *user = NULL;
  if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user || !user[0])
    return PAM_USER_UNKNOWN;
  int fd = -1;
  if (nh_auth_client_connect(socket_path(argc, argv), &fd) != 0)
    return PAM_AUTHINFO_UNAVAIL;
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  int rc = nh_auth_client_check_account(fd, user, &result);
  nh_auth_client_close(fd);
  if (rc != 0) return PAM_AUTHINFO_UNAVAIL;
  switch (result) {
    case NH_AUTH_RESULT_OK: return PAM_SUCCESS;
    case NH_AUTH_RESULT_UNKNOWN_ACCOUNT: return PAM_USER_UNKNOWN;
    case NH_AUTH_RESULT_DISABLED: return PAM_ACCT_EXPIRED;
    default: return PAM_AUTH_ERR;
  }
}
