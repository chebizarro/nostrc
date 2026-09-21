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

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

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
  const char *prompt = "Sign in with (local/remote): ";
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
    pam_error(pamh, "Unknown provider. Type 'local' or 'remote'.");
  }
  return PAM_MAXTRIES;
}

static void wipe(char *s) {
  if (!s) return;
  volatile char *p = (volatile char *)s;
  while (*p) *p++ = 0;
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
  if (is_nip46)
    pam_info(pamh,
             "Approve the sign-in request on your Nostr signer (bunker).");

  /* Passphrase retry loop for the local provider. Each SUBMIT_UNLOCK runs on a
   * fresh connection because the broker retires the transaction after any
   * verification outcome. For the nip46 provider the placeholder secret is
   * derived from the client library and we only try once (unless the bunker
   * responds with a transient signal). */
  int last_pam_rc = PAM_AUTH_ERR;
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  while (attempts_left > 0) {
    char *pass = NULL;
    if (!is_nip46) {
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
    trc = nh_auth_client_login_with(fd, user, service, chosen, pass, &result);
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
    if (is_nip46) {
      /* The bunker denied approval; do not silently loop forever. */
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
