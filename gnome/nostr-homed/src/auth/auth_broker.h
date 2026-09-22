#ifndef NH_AUTH_BROKER_H
#define NH_AUTH_BROKER_H

#include "auth_ratelimit.h"
#include "nostr_identity.h"

#include <stdint.h>

typedef struct nh_auth_broker nh_auth_broker;

/* Forward declaration: the SMB authority is optional and lives in D5's
 * smb_credential module.  Broker consumers that don't touch SMB avoid the
 * transitive SQLite include by not attaching an authority. */
typedef struct nh_smb_authority nh_smb_authority;

/* Creates a broker bound to an open identity store (authority). The store is
 * borrowed and must outlive the broker. */
nh_auth_broker *nh_auth_broker_new(nh_identity_store *store);
void nh_auth_broker_free(nh_auth_broker *broker);

/* Attaches an SMB credential authority to the broker.  Borrowed; must outlive
 * the broker.  When set, connections on the USER endpoint may run
 * BEGIN_SMB_PROOF and, on a verified proof, receive a freshly minted volatile
 * password from the authority.  Leaving this unset makes SMB proof unavailable
 * (the broker responds PROVIDER_UNAVAILABLE).  Pass NULL to detach. */
void nh_auth_broker_set_smb_authority(nh_auth_broker *broker,
                                      nh_smb_authority *authority);

/* Testable monotonic clock seam. All broker time reads (transaction begin
 * timestamps, deadline checks, provider expiry, receipt binding time, and
 * rate-limit bookkeeping) go through this hook. Production leaves it unset,
 * in which case the broker reads CLOCK_MONOTONIC directly. Tests inject a
 * function that returns a controllable millisecond counter. Passing fn=NULL
 * restores the default clock. Not thread safe with active connections. */
typedef uint64_t (*nh_auth_broker_clock_fn)(void *context);
void nh_auth_broker_set_clock(nh_auth_broker *broker,
                              nh_auth_broker_clock_fn fn, void *context);

/* Overrides the per-account failed-proof rate-limit policy. Passing NULL
 * restores the built-in defaults (see auth_ratelimit.h). Any state accumulated
 * with the previous policy is cleared. Not thread safe with active
 * connections. Returns 0 on success, -1 if the new policy could not be
 * constructed (max_failures == 0 or OOM); on failure the previous policy is
 * retained. */
int nh_auth_broker_set_ratelimit_config(nh_auth_broker *broker,
                                        const nh_auth_ratelimit_config *config);

/* Enables persistent rate-limit storage at `path`, or reverts to in-memory-only
 * when path is NULL or empty. The current rate-limit policy is preserved; any
 * counters accumulated in the previous mode are discarded (persistent state on
 * disk survives independently, so re-opening the same path restores it). When
 * persistence is enabled the limiter uses wall-clock time internally so that
 * cooldowns survive a broker restart (the broker's monotonic clock resets on
 * reboot). Returns 0 on success, -1 on OOM or a SQLite failure; on failure the
 * previous limiter is retained. Not thread safe with active connections. */
int nh_auth_broker_set_ratelimit_path(nh_auth_broker *broker, const char *path);

/* Reads one request from a connected AUTH-endpoint SOCK_SEQPACKET fd (auth.sock,
 * uid 0 only for login), enforces the SO_PEERCRED/endpoint ACL, dispatches it,
 * and writes one response.  Returns 0 if a response was sent, -1 on transport
 * failure. */
int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd);

/* Same as nh_auth_broker_handle_connection but for a USER-endpoint connection
 * (user.sock, any uid, own-account only).  The peer snapshot is taken with
 * NH_AUTH_ENDPOINT_USER; the ACL then permits BEGIN_SMB_PROOF and, on a
 * verified proof, the broker mints a short-lived SMB password from the
 * attached authority and returns it once. */
int nh_auth_broker_handle_user_connection(nh_auth_broker *broker, int fd);

/* auth.conf parsing (design decision D12).
 *
 * Trivial `key=value` format, `#` and `;` line comments, no sections. Unknown
 * keys are ignored with an implicit warning (never fatal). Missing file /
 * missing keys leave *out with all zero fields.
 *
 * Recognised keys today (Phase 3, greeter QR flow):
 *   nip46_qr_relays        Comma-separated list of relay URLs. First relay is
 *                          the one embedded in the QR; subsequent relays are
 *                          used for the client subscription. Cap: 4.
 *   nip46_qr_wait_ms       Overrides the per-attempt QR wait budget (default
 *                          78 000 ms). Must fit in uint32.
 *   nip46_qr_render        "auto" | "qr" | "uri" — hint for the PAM module.
 *   nip46_qr_max_concurrent Broker-wide cap on simultaneous QR waits.
 *
 * Fields the parser fills are DOCUMENTED here; callers copy pointers/strings
 * out before dropping the config struct. */
#define NH_AUTH_CONF_RELAYS_MAX 4u
#define NH_AUTH_CONF_RELAY_URL_MAX 255u

typedef struct nh_auth_conf {
  char nip46_qr_relays[NH_AUTH_CONF_RELAYS_MAX][NH_AUTH_CONF_RELAY_URL_MAX + 1];
  size_t nip46_qr_relays_count;
  uint32_t nip46_qr_wait_ms;         /* 0 = unset */
  char nip46_qr_render[8];           /* "" | "auto" | "qr" | "uri" */
  uint32_t nip46_qr_max_concurrent;  /* 0 = unset */
} nh_auth_conf;

/* Reads path (may be NULL / missing) into *out. Zeros *out first. Returns 0
 * on any outcome (missing file, malformed lines) — the design deliberately
 * makes this non-fatal so a startup misconfig can't brick logins. */
int nh_auth_conf_load(const char *path, nh_auth_conf *out);

/* Configure the broker's global QR fallback relay list. Overrides the
 * compiled-in NH_AUTH_NIP46_QR_DEFAULT_RELAY. Callers must keep the string
 * storage alive (typically inside an nh_auth_conf owned by the daemon).
 * Pass relays=NULL / n_relays=0 to reset. */
void nh_auth_broker_set_qr_default_relays(const char *const *relays,
                                          size_t n_relays);

/* Greeter artifact drop — the broker writes /run/nostr-auth/greeter/
 * current.json (and, when a PNG source is available, current.png) so a
 * gnome-shell greeter extension can render the QR image live. Design §5.3
 * and greeter-extension/README.md. Contract implemented here:
 *   {"tx_id":"...","png":"current.png","uri":"...","pairing_code":"...",
 *    "expires_at":<monotonic-ms>,"hint":"..."}
 * The PNG is only produced by an optional side channel (see
 * pam_qr_render); the broker just publishes what it has.
 *
 * write() overwrites atomically (write to <name>.tmp + rename). remove()
 * unlinks best-effort and is safe to call when nothing was written.
 *
 * write() returns 0 on success, -1 on any I/O failure (never fatal to a
 * login — the artifact is a rendering hint). */
int nh_broker_greeter_artifact_write(const char *tx_id,
                                     const char *display_json);
void nh_broker_greeter_artifact_remove(void);

/* Override the greeter-artifact directory. NULL/"" resets to the default
 * (/run/nostr-auth/greeter). The path is not created recursively; the
 * broker will attempt to mkdir the leaf with mode 0755. Test seam only —
 * production leaves it at the default so gdm's greeter can find it. */
void nh_broker_greeter_artifact_set_dir(const char *dir);

#endif /* NH_AUTH_BROKER_H */
