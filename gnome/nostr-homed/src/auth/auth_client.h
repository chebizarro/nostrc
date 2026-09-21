#ifndef NH_AUTH_CLIENT_H
#define NH_AUTH_CLIENT_H

#include "nostr_auth_protocol.h"

#include <stddef.h>

/* Maximum invalid interactive attempts a PAM run allows before giving up. The
 * budget is enforced by the PAM module; kept here so tests can pin the value. */
#define NH_AUTH_PAM_MAX_INVALID_ATTEMPTS 3u

/* Canonical provider names emitted by BEGIN_LOGIN and accepted by
 * SELECT_PROVIDER / nh_auth_client_login_with. */
#define NH_AUTH_PROVIDER_NAME_LOCAL "local"
#define NH_AUTH_PROVIDER_NAME_NIP46 "nip46"

/* Provider list returned by BEGIN_LOGIN. Two providers exist today; the array
 * is sized for headroom. Names are canonical lowercase ASCII (see the
 * NH_AUTH_PROVIDER_NAME_* macros); count == 0 means the account has none
 * enabled. */
#define NH_AUTH_PROVIDER_NAME_MAX 31u
#define NH_AUTH_PROVIDER_LIST_CAP 4u

typedef struct nh_auth_provider_list {
  size_t count;
  char names[NH_AUTH_PROVIDER_LIST_CAP][NH_AUTH_PROVIDER_NAME_MAX + 1];
} nh_auth_provider_list;

/* True iff the list contains the canonical provider name. */
int nh_auth_provider_list_has(const nh_auth_provider_list *list,
                              const char *canonical_name);

/* Parses a raw provider choice — as typed at a PAM prompt — into one of the
 * canonical names. Accepts only trimmed lowercase ASCII "local" or
 * "remote"/"nip46"; any non-printable, non-ASCII, or unknown token yields
 * NULL. The returned pointer is a static string literal ("local" / "nip46"). */
const char *nh_auth_provider_choice_parse(const char *raw);

/* Connects to a broker SOCK_SEQPACKET endpoint. Returns 0 and sets *fd_out. */
int nh_auth_client_connect(const char *socket_path, int *fd_out);

/* Sends CHECK_ACCOUNT for username and returns the broker result in *result_out.
 * Returns 0 if a well-formed response was received, -1 on transport/protocol
 * failure. */
int nh_auth_client_check_account(int fd, const char *username,
                                 nh_auth_result *result_out);

/* Sends BEGIN_LOGIN for the given (username, service) and, on OK, fills
 * *providers_out with the set of provider names the account has enabled (as
 * reported by the broker). *result_out is set to the broker result regardless
 * of the outcome. Returns 0 on transport success, -1 on transport failure.
 * providers_out may be NULL when the caller does not need the list. */
int nh_auth_client_begin_login(int fd, const char *username, const char *service,
                               nh_auth_provider_list *providers_out,
                               nh_auth_result *result_out);

/* Drives SELECT_PROVIDER(provider) -> SUBMIT_UNLOCK on a connection that has
 * already completed BEGIN_LOGIN. `provider` must be a canonical name (see
 * NH_AUTH_PROVIDER_NAME_*). For "local", `passphrase` must be non-NULL and is
 * sent as the SUBMIT_UNLOCK secret. For "nip46", `passphrase` may be NULL and
 * an approval placeholder is sent (the external signer ignores its value and
 * gates approval at the bunker). Returns 0 on transport success, -1 on
 * transport failure. */
int nh_auth_client_submit_selection(int fd, const char *provider,
                                    const char *passphrase,
                                    nh_auth_result *result_out);

/* Drives BEGIN_LOGIN -> SELECT_PROVIDER(provider) -> SUBMIT_UNLOCK on one
 * connection and returns the final broker result. See
 * nh_auth_client_submit_selection for the provider/passphrase contract. */
int nh_auth_client_login_with(int fd, const char *username, const char *service,
                              const char *provider, const char *passphrase,
                              nh_auth_result *result_out);

/* Backwards-compatible convenience wrapper: forces provider="local". */
int nh_auth_client_login(int fd, const char *username, const char *service,
                         const char *passphrase, nh_auth_result *result_out);

void nh_auth_client_close(int fd);

#endif /* NH_AUTH_CLIENT_H */
