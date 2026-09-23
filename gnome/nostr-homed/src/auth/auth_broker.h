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

/* Enable/disable NIP-05 identifier resolution at BEGIN_LOGIN. When
 * disabled the broker treats an `@`-containing username exactly like
 * any other username (typically UNKNOWN_ACCOUNT). @drop_user is the
 * uname the unprivileged nostr-homed-nip05 helper drops to; NULL
 * defaults to "nobody". @helper_path may be NULL to look the helper
 * up on PATH; tests set it to the build-tree binary. @positive_ttl
 * is the (address -> pubkey) cache TTL in seconds; 0 selects the
 * built-in default (600 s). Safe to call at any time; existing
 * cache entries survive a TTL change and re-evaluate at next lookup.
 * Not thread safe with active connections. */
void nh_auth_broker_set_nip05(nh_auth_broker *broker, int enabled,
                              const char *helper_path,
                              const char *drop_user,
                              int positive_ttl_seconds);

/* Test seam: override the resolver function so unit tests can exercise
 * the broker's BEGIN_LOGIN canonicalisation path without shelling out
 * to the real helper. The default resolver runs the helper via
 * nh_nip05_client_resolve; passing NULL restores the default. Not
 * thread safe with active connections. */
struct nh_nip05_address;
struct nh_nip05_result;
typedef int /* nh_nip05_rc */ (*nh_auth_broker_nip05_resolver_fn)(
    void *ctx, const struct nh_nip05_address *addr,
    struct nh_nip05_result *result_out);
void nh_auth_broker_set_nip05_resolver(nh_auth_broker *broker,
                                       nh_auth_broker_nip05_resolver_fn fn,
                                       void *ctx);

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

  /* B5-profile: relays queried for kind-0 (user metadata) events, and the
   * on/off switch for the post-login refresh hook. When
   * profile_relays_count == 0 the profile refresher falls back to its own
   * compiled-in default list (docs/reviews/nostr-profile-avatar…). */
  char profile_relays[NH_AUTH_CONF_RELAYS_MAX][NH_AUTH_CONF_RELAY_URL_MAX + 1];
  size_t profile_relays_count;
  /* Tri-state: 0 = unset (default on), 1 = on, 2 = off. */
  uint8_t profile_fetch;
  /* Uname the image-downloader helper drops to. "" = "nobody". */
  char profile_image_user[64];

  /* NIP-05 login identifier resolution (B5-NIP-05, nostrc-bit0):
   *   nip05_resolve — Tri-state: 0 = unset (default on), 1 = on,
   *                   2 = off. When off the broker treats an
   *                   `@`-containing username exactly like any
   *                   other username (typically UNKNOWN_ACCOUNT).
   *   nip05_cache_ttl_seconds — positive-cache TTL in seconds for
   *                   the broker's (address -> pubkey) cache. 0 =
   *                   use compile-time default (600 s). The negative
   *                   TTL (SSRF / transport / NAME failures) is
   *                   fixed at NH_NIP05_CACHE_NEGATIVE_TTL_SEC so
   *                   the config surface stays small.
   *   nip05_image_user — uname the NIP-05 helper drops to;
   *                   defaults to profile_image_user, then "nobody". */
  uint8_t nip05_resolve;
  uint32_t nip05_cache_ttl_seconds;
  char nip05_image_user[64];
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

/* Optional account-block payload for the greeter artifact. Populated
 * by the broker whenever it has enough information to name the
 * account whose login is pending — either because the client typed
 * the canonical local username directly, or because a NIP-05
 * identifier canonicalised to one. Any field may be NULL/empty to
 * skip its emission; when the whole struct is NULL the "account"
 * block is omitted entirely (matches the pre-NIP-05 artifact shape).
 *
 * `icon_source_path` is copied under the greeter directory as
 * `avatar.png` (0644, root-owned) if it exists and is readable;
 * on failure the manifest omits the "avatar" field but the rest of
 * the account block is still emitted so the greeter has the label
 * even without a picture. Typical value:
 * `/var/lib/AccountsService/icons/<user>`. */
typedef struct nh_broker_greeter_account {
  const char *username;        /* canonical local username */
  const char *display_name;    /* profile display / real name; may be "" */
  const char *identifier;      /* NIP-05 as typed; NULL if not via NIP-05 */
  const char *icon_source_path;/* absolute path to source PNG; NULL to skip */
} nh_broker_greeter_account;

/* Greeter artifact drop — the broker writes /run/nostr-auth/greeter/
 * current.json (and, when a PNG source is available, current.png) so a
 * gnome-shell greeter extension can render the QR image live. Design §5.3
 * and greeter-extension/README.md. Contract implemented here:
 *   {"tx_id":"...","png":"current.png","uri":"...","pairing_code":"...",
 *    "expires_at":<monotonic-ms>,"hint":"...",
 *    "account":{"username":"...","display_name":"...","identifier":"...",
 *               "avatar":"avatar.png"}}
 * The `account` block is present only when the caller passes a non-NULL
 * @account; "identifier" appears only when @account->identifier is
 * non-empty (i.e. the login was initiated via NIP-05); "avatar" appears
 * only when a source PNG was copied successfully.
 *
 * The QR PNG (`current.png`) is only produced by an optional side
 * channel (see pam_qr_render); the broker just publishes what it has.
 *
 * write() overwrites atomically (write to <name>.tmp + rename). remove()
 * unlinks best-effort and is safe to call when nothing was written.
 *
 * write() returns 0 on success, -1 on any I/O failure (never fatal to a
 * login — the artifact is a rendering hint). */
int nh_broker_greeter_artifact_write(const char *tx_id,
                                     const char *display_json,
                                     const nh_broker_greeter_account *account);
void nh_broker_greeter_artifact_remove(void);

/* Override the greeter-artifact directory. NULL/"" resets to the default
 * (/run/nostr-auth/greeter). The path is not created recursively; the
 * broker will attempt to mkdir the leaf with mode 0750 owned by the
 * greeter group (see nh_broker_greeter_artifact_set_group). Test seam
 * only — production leaves it at the default so gdm's greeter can find
 * it. */
void nh_broker_greeter_artifact_set_dir(const char *dir);

/* Override the confidentiality group applied to the greeter-artifact
 * directory and its files. The broker publishes /run/nostr-auth/greeter/
 * mode 0750 and current.{json,png} + avatar.png mode 0640, chown'd to
 * root:<group>, so that only members of that group (gdm at the greeter,
 * the seated user for unlock-dialog via pam_group) can read the
 * one-time NIP-46 pairing secret carried by the manifest and QR.
 *
 * NULL/"" resets to the default ("nostr-auth-greeter"). A group name
 * that fails to resolve at write time is treated as "no group" — the
 * files are still published mode 0640 root:root (readable only to
 * root; the greeter extension will NOT be able to render but the
 * pairing secret is protected), and the failure is logged once.
 * Test seam so headless tests can pick a group the running uid is
 * already a member of. */
void nh_broker_greeter_artifact_set_group(const char *group_name);

#endif /* NH_AUTH_BROKER_H */
