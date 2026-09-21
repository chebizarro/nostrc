/* pam_nostr.so — broker-backed authentication (nostrc-zcll.5 [B4]).
 *
 * Replaces the legacy cache-accept flow: authentication succeeds only when the
 * privileged broker verifies a fresh cryptographic proof over auth.sock. The
 * module is stateless glue — no key material, no policy decision of its own.
 *
 * Module args: socket=<path> (default /run/nostr-auth/auth.sock).
 */
#define _GNU_SOURCE
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include "auth_client.h"
#include "nostr_auth_protocol.h"

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

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                        const char **argv) {
  (void)flags;
  const char *user = NULL;
  if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user || !user[0])
    return PAM_USER_UNKNOWN;
  const char *service = NULL;
  pam_get_item(pamh, PAM_SERVICE, (const void **)&service);
  if (!service || !service[0]) service = "nostr-login";

  const char *pass = NULL;
  if (pam_get_authtok(pamh, PAM_AUTHTOK, &pass, "Nostr passphrase: ") !=
          PAM_SUCCESS ||
      !pass)
    return PAM_AUTHINFO_UNAVAIL;

  int fd = -1;
  if (nh_auth_client_connect(socket_path(argc, argv), &fd) != 0) {
    pam_syslog(pamh, LOG_ERR, "nostr: cannot reach broker at %s",
               socket_path(argc, argv));
    return PAM_AUTHINFO_UNAVAIL;
  }
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  int rc = nh_auth_client_login(fd, user, service, pass, &result);
  nh_auth_client_close(fd);
  if (rc != 0) {
    pam_syslog(pamh, LOG_ERR, "nostr: broker transport failure for %s", user);
    return PAM_AUTHINFO_UNAVAIL;
  }
  int pr = map_result(result);
  pam_syslog(pamh, pr == PAM_SUCCESS ? LOG_INFO : LOG_NOTICE,
             "nostr: authenticate %s -> %s", user, nh_auth_result_name(result));
  return pr;
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
