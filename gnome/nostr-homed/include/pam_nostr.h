#ifndef PAM_NOSTR_H
#define PAM_NOSTR_H

/* PAM constants and exported entry points */
#include <security/pam_modules.h>
#include <security/pam_ext.h>

int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv);
int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv);
int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv);
int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv);
int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv);

#endif /* PAM_NOSTR_H */
