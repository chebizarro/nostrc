#include "pam_nostr.h"
#include <string.h>

static int get_username(pam_handle_t *pamh, const char **user_out){
  const char *user = NULL; int rc = pam_get_user(pamh, &user, NULL);
  if (rc != PAM_SUCCESS || !user) return PAM_USER_UNKNOWN;
  *user_out = user; return PAM_SUCCESS;
}

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  pam_info(pamh, "pam_nostr: authenticating %s via signer (stub)", user);
  /* Stub: accept for now. Real impl will request NIP-46 proof over DBus and verify. */
  return PAM_SUCCESS;
}

int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)pamh; (void)flags; (void)argc; (void)argv; return PAM_SUCCESS;
}

int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)pamh; (void)flags; (void)argc; (void)argv; return PAM_SUCCESS;
}

int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)flags; (void)argc; (void)argv;
  const char *user = NULL; int rc = get_username(pamh, &user);
  if (rc != PAM_SUCCESS) return rc;
  pam_info(pamh, "pam_nostr: opening session for %s (stub)", user);
  return PAM_SUCCESS;
}

int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv){
  (void)pamh; (void)flags; (void)argc; (void)argv; return PAM_SUCCESS;
}
