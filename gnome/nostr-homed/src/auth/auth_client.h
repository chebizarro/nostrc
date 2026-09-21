#ifndef NH_AUTH_CLIENT_H
#define NH_AUTH_CLIENT_H

#include "nostr_auth_protocol.h"

/* Connects to a broker SOCK_SEQPACKET endpoint. Returns 0 and sets *fd_out. */
int nh_auth_client_connect(const char *socket_path, int *fd_out);

/* Sends CHECK_ACCOUNT for username and returns the broker result in *result_out.
 * Returns 0 if a well-formed response was received, -1 on transport/protocol
 * failure. */
int nh_auth_client_check_account(int fd, const char *username,
                                 nh_auth_result *result_out);

/* Drives BEGIN_LOGIN -> SELECT_PROVIDER(local) -> SUBMIT_UNLOCK on one
 * connection and returns the final broker result. */
int nh_auth_client_login(int fd, const char *username, const char *service,
                         const char *passphrase, nh_auth_result *result_out);

void nh_auth_client_close(int fd);

#endif /* NH_AUTH_CLIENT_H */
