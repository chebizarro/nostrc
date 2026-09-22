#ifndef NH_AUTH_CLIENT_H
#define NH_AUTH_CLIENT_H

#include "nostr_auth_protocol.h"
#include "nostr_identity.h"
#include "secure_buf.h"

#include <stddef.h>
#include <stdint.h>

/* Maximum invalid interactive attempts a PAM run allows before giving up. The
 * budget is enforced by the PAM module; kept here so tests can pin the value. */
#define NH_AUTH_PAM_MAX_INVALID_ATTEMPTS 3u

/* Canonical provider names emitted by BEGIN_LOGIN and accepted by
 * SELECT_PROVIDER / nh_auth_client_login_with. */
#define NH_AUTH_PROVIDER_NAME_LOCAL "local"
#define NH_AUTH_PROVIDER_NAME_NIP46 "nip46"
#define NH_AUTH_PROVIDER_NAME_NIP46_QR "nip46qr"

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

/* Volatile SMB credential envelope returned by nh_auth_client_smb_proof_with.
 * The password lives in a mlock'd secure buffer; the caller MUST invoke
 * nh_auth_smb_envelope_clear() before dropping the struct. Safe on a
 * zero-initialised envelope. */
#define NH_AUTH_SMB_CREDENTIAL_ID_CAP 37u
#define NH_AUTH_SMB_PASSWORD_MAX_LEN 64u

typedef struct nh_auth_smb_envelope {
  char credential_id[NH_AUTH_SMB_CREDENTIAL_ID_CAP];
  char username[NH_IDENTITY_USERNAME_CAP];
  uint64_t issued_at_ms;
  uint64_t expires_at_ms;
  size_t password_len; /* excludes trailing NUL */
  nostr_secure_buf password;
} nh_auth_smb_envelope;

/* True iff the list contains the canonical provider name. */
int nh_auth_provider_list_has(const nh_auth_provider_list *list,
                              const char *canonical_name);

/* Parses a raw provider choice — as typed at a PAM prompt — into one of the
 * canonical names. Accepts only trimmed lowercase ASCII: "local",
 * "remote"/"nip46", or "qr"/"nip46qr". Any non-printable, non-ASCII, or
 * unknown token yields NULL. The returned pointer is a static string
 * literal ("local" / "nip46" / "nip46qr"). */
const char *nh_auth_provider_choice_parse(const char *raw);

/* Optional display payload returned by SELECT_PROVIDER for the QR /
 * nostrconnect flow (design §5.3). All fields are NUL-terminated. Absent
 * (kind[0] == '\0') for the local / bunker paths. */
#define NH_AUTH_DISPLAY_URI_MAX 512u
#define NH_AUTH_DISPLAY_HINT_MAX 128u
#define NH_AUTH_DISPLAY_PAIRING_MAX 16u

typedef struct nh_auth_display {
  char kind[16];                            /* "nostrconnect" (or "") */
  char uri[NH_AUTH_DISPLAY_URI_MAX];
  char hint[NH_AUTH_DISPLAY_HINT_MAX];
  char pairing_code[NH_AUTH_DISPLAY_PAIRING_MAX];
  uint32_t expires_in_ms;
} nh_auth_display;

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

/* Sends BEGIN_SMB_PROOF for the connected peer's OWN identity (SO_PEERCRED
 * uid). Requires a USER-endpoint (user.sock) connection. `service` may be
 * NULL to accept the broker's default. On OK, fills *providers_out with the
 * account's enabled providers. Returns 0 on transport success. */
int nh_auth_client_begin_smb_proof(int fd, const char *service,
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

/* Split SELECT_PROVIDER / SUBMIT_UNLOCK entrypoint so the PAM module can
 * render the display payload between the two RPCs. Runs SELECT_PROVIDER,
 * fills *display_out (zeroed first) if the broker attached a display
 * object, then invokes `on_display` (may be NULL) BEFORE the long-blocking
 * SUBMIT_UNLOCK. `secret` is sent as the SUBMIT_UNLOCK payload; for the QR
 * flow pass "qr", for the local flow the passphrase, for the pre-paired
 * bunker flow "approve". Returns 0 on transport success. */
typedef void (*nh_auth_display_callback)(void *ctx,
                                         const nh_auth_display *display);

int nh_auth_client_submit_selection_display(int fd, const char *provider,
                                            const char *secret,
                                            nh_auth_display *display_out,
                                            nh_auth_display_callback on_display,
                                            void *on_display_ctx,
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

/* Drives BEGIN_SMB_PROOF -> SELECT_PROVIDER -> SUBMIT_UNLOCK on one
 * user.sock connection. On NH_AUTH_RESULT_OK, *envelope_out is populated
 * with the freshly-minted password in a zeroizing secure buffer; the caller
 * MUST invoke nh_auth_smb_envelope_clear once the password has been handed
 * off (or immediately on error). On any other result envelope_out is left
 * untouched (still safe to clear). Returns 0 on transport success, -1 on
 * transport failure. */
int nh_auth_client_smb_proof_with(int fd, const char *service,
                                  const char *provider, const char *passphrase,
                                  nh_auth_smb_envelope *envelope_out,
                                  nh_auth_result *result_out);

/* Wipes and frees an SMB envelope. Safe on {0}. */
void nh_auth_smb_envelope_clear(nh_auth_smb_envelope *envelope);

void nh_auth_client_close(int fd);

#endif /* NH_AUTH_CLIENT_H */
