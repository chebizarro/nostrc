#define _GNU_SOURCE
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int conversation(int n, const struct pam_message **messages,
                        struct pam_response **out, void *data) {
  struct pam_response *r = calloc((size_t)n, sizeof *r);
  if (!r) return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF || messages[i]->msg_style == PAM_PROMPT_ECHO_ON)
      r[i].resp = strdup(data);
    else fprintf(stderr, "PAM: %s\n", messages[i]->msg);
  }
  *out = r; return PAM_SUCCESS;
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (setsid() < 0) return 2;
  char password[256];
  if (!fgets(password, sizeof password, stdin)) return 2;
  password[strcspn(password, "\n")] = 0;
  struct pam_conv conv = {conversation, password};
  pam_handle_t *p = NULL;
  int rc = pam_start(argv[1], argv[2], &conv, &p);
  if (rc == PAM_SUCCESS) rc = pam_authenticate(p, 0);
  if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(p, 0);
  if (rc == PAM_SUCCESS) rc = pam_open_session(p, 0);
  explicit_bzero(password, sizeof password);
  if (rc == PAM_SUCCESS) {
    puts("READY"); fflush(stdout);
    char command[16];
    if (!fgets(command, sizeof command, stdin)) rc = PAM_SYSTEM_ERR;
    int closed = pam_close_session(p, 0);
    if (rc == PAM_SUCCESS) rc = closed;
  }
  if (p) pam_end(p, rc);
  return rc == PAM_SUCCESS ? 0 : 1;
}
