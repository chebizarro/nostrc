/*
 * session_fed_policy — the pure (I/O-free) half of the session relay's
 * upstream federation client (bead nostrc-7d96).
 *
 * Everything here is deterministic and unit-tested
 * (tests/test_session_fed_policy.c): the `federation_*` keys of
 * session-relay.conf, the "what may leave the machine" contract, the
 * relay-URL admission rule, route resolution per session_routing class,
 * the retry backoff curve and NIP-01 OK classification. The engine that
 * owns sockets and threads is session_federation.h; persistence is
 * session_outbox.h. apps/relayd/README.md "Upstream federation" is the
 * user-facing statement of the same contract.
 *
 * Forwarding contract (summary; README is normative):
 *   - Never forwarded, whatever the config says: events carrying a NIP-70
 *     `["-"]` tag, NIP-17 seals (13) and rumors (14), NIP-42 AUTH (22242)
 *     and every ephemeral kind (20000-29999).
 *   - Never forwarded, by configuration: kinds listed in
 *     `federation_local_only_kinds`.
 *   - Only events authored by a local account are forwarded (the
 *     `federation_accounts` list, else the pubkey org.nostr.Signer
 *     reports). Kind-1059 gift wraps are signed by throw-away keys and are
 *     exempt from the author check.
 *   - Targets come only from the user's own data: the author's kind-10002
 *     write relays, the recipient's kind-10050 inbox relays (1059), the
 *     group's relay for NIP-29 kinds and any h-tagged event
 *     (nostr_session_route_class_event(): relay hint in the `h` tag, the
 *     author's kind-10009 `group` entry, or a `host'group-id` identifier).
 *     There is no built-in or fallback relay.
 */
#ifndef NSR_SESSION_FED_POLICY_H
#define NSR_SESSION_FED_POLICY_H

#include <glib.h>
#include <stdint.h>

#include "nostr-event.h"
#include "nostr-tag.h"

G_BEGIN_DECLS

#define NSR_FED_MAX_ACCOUNTS 16
#define NSR_FED_MAX_KIND_RANGES 32
#define NSR_FED_MAX_URL_LEN 512

typedef struct {
  uint32_t lo, hi; /* inclusive */
} NsrFedKindRange;

typedef struct {
  /* federation = 1|0 (default 1). Also off when the relay is cache-less. */
  int enabled;

  /* federation_accounts = "npub1…, <hex>" — authoritative when non-empty;
   * otherwise the engine asks org.nostr.Signer.GetPublicKey. Stored as
   * lowercase hex. */
  char accounts[NSR_FED_MAX_ACCOUNTS][65];
  size_t n_accounts;

  /* federation_local_only_kinds = "30078, 31000-31999" */
  NsrFedKindRange local_only[NSR_FED_MAX_KIND_RANGES];
  size_t n_local_only;

  /* federation_allow_plaintext_ws = 0|1 (default 0): ws:// to non-loopback
   * hosts. ws://localhost / 127.0.0.0/8 / [::1] are always allowed. */
  int allow_plaintext_ws;

  int backoff_initial_seconds;     /* default 15 */
  int backoff_max_seconds;         /* default 3600 */
  int ok_timeout_seconds;          /* default 30: EVENT sent -> OK */
  int max_age_seconds;             /* default 604800 (7 d): give up */
  int keep_settled_seconds;        /* default 604800: prune settled rows */
  int max_relays_per_event;        /* default 16 */
  int max_inflight_per_relay;      /* default 32 */
  int idle_disconnect_seconds;     /* default 60 */
} NsrFedConfig;

void nsr_fed_config_defaults(NsrFedConfig *cfg);

/* Parse the `federation_*` keys of session-relay.conf (same key = value
 * grammar as relayd_config.c; every other key is ignored here). A missing
 * file or NULL path leaves the defaults. Returns 0, or -1 with @err set
 * (bad value for a federation key). */
int nsr_fed_config_load(const char *path, NsrFedConfig *cfg, char *err, size_t err_sz);

/* Apply one key. Returns 1 if the key is a federation key and was applied,
 * 0 if it is not a federation key, -1 on a bad value. Exposed for tests. */
int nsr_fed_config_apply(NsrFedConfig *cfg, const char *key, const char *val);

/* npub1… (NIP-19 bech32) or 64-hex → lowercase hex. 0 ok, -1 invalid. */
int nsr_fed_parse_pubkey(const char *text, char out_hex[65]);

gboolean nsr_fed_config_has_account(const NsrFedConfig *cfg, const char *pubkey_hex);

/* ── What may leave the machine ────────────────────────────────────────── */

typedef enum {
  NSR_FED_FORWARD = 0,
  NSR_FED_SKIP_PROTECTED,   /* NIP-70 ["-"] tag */
  NSR_FED_SKIP_NEVER_KIND,  /* 13, 14, 22242, 20000-29999 */
  NSR_FED_SKIP_LOCAL_KIND,  /* federation_local_only_kinds */
} NsrFedVerdict;

/* Decision that needs nothing but the event itself and the config. The
 * author check (needs the account set) is the engine's. */
NsrFedVerdict nsr_fed_static_verdict(const NsrFedConfig *cfg, int kind, NostrTags *tags);
const char *nsr_fed_verdict_reason(NsrFedVerdict v);

gboolean nsr_fed_kind_is_ephemeral(int kind);
gboolean nsr_fed_kind_is_replaceable(int kind); /* 0, 3, 10000-19999 */
gboolean nsr_fed_kind_is_addressable(int kind); /* 30000-39999 */

/* `a`-tag coordinate "kind:pubkey:d" for replaceable (d = "") and
 * addressable kinds; NULL otherwise. g_free(). */
char *nsr_fed_replace_key(int kind, const char *pubkey, NostrTags *tags);

/* ── Relay URLs ────────────────────────────────────────────────────────── */

/* TRUE for wss://host[...] and for ws:// to a loopback host (or any host
 * when @allow_plaintext_ws). Rejects userinfo, whitespace/control chars,
 * empty hosts and URLs longer than NSR_FED_MAX_URL_LEN. URLs are otherwise
 * used verbatim (libnostr-publish does not normalise either). */
gboolean nsr_fed_url_acceptable(const char *url, gboolean allow_plaintext_ws);

/* ── Route resolution ──────────────────────────────────────────────────── */

typedef enum {
  /* May answer NIP-42 AUTH with the account's key (via org.nostr.Signer). */
  NSR_FED_LANE_IDENTIFIED = 0,
  /* Never authenticates: NIP-17 gift wraps are signed by throw-away keys
   * precisely so the relay cannot tell who sent them; authenticating the
   * delivering connection as the user would undo that. Separate
   * connections from the identified lane. */
  NSR_FED_LANE_ANONYMOUS = 1,
} NsrFedLane;

typedef struct {
  /* Newest relay-list event (kind 10002 / 10050 / 10009) by @pubkey_hex as
   * JSON, or NULL. g_free()d by the caller. */
  char *(*relay_list_json)(void *ud, const char *pubkey_hex, int kind);
  /* Relays that acknowledged the outbox event @ref (event id, or a
   * coordinate when @is_coordinate) — used to send NIP-09 deletions where
   * the deleted events went. NULL or a GStrv. */
  GStrv (*acked_relays)(void *ud, const char *ref, gboolean is_coordinate);
  void *ud;
} NsrFedLookup;

typedef enum {
  NSR_FED_ROUTE_OK = 0,          /* *out_relays non-empty */
  NSR_FED_ROUTE_UNROUTABLE = 1,  /* no target known yet; retry later */
  NSR_FED_ROUTE_INVALID = 2,     /* can never be routed (e.g. 1059 w/o p) */
} NsrFedRouteStatus;

/* Resolve the upstream relays for @ev (already past the static verdict and
 * the author check). *out_relays: deduplicated, admitted by
 * nsr_fed_url_acceptable(), capped at cfg->max_relays_per_event.
 * *out_reason (g_free) explains UNROUTABLE / INVALID. */
NsrFedRouteStatus nsr_fed_resolve(const NsrFedConfig *cfg, NostrEvent *ev,
                                  const NsrFedLookup *lookup, GStrv *out_relays,
                                  NsrFedLane *out_lane, char **out_reason);

/* Individual resolvers, exposed for tests. Each returns a GStrv (maybe
 * empty) of admitted URLs. */
GStrv nsr_fed_write_relays_from_10002(const NsrFedConfig *cfg, const char *json);
GStrv nsr_fed_inbox_relays_from_10050(const NsrFedConfig *cfg, const char *json);
/* Group relay for @group_id from a kind-10009 list: first
 * ["group", <id>, <relay>, ...] whose id matches. NULL if none. */
char *nsr_fed_group_relay_from_10009(const NsrFedConfig *cfg, const char *json,
                                     const char *group_id);

/* ── Retry ─────────────────────────────────────────────────────────────── */

/* Delay before retry number @attempts+1 (attempts already failed >= 1):
 * min(initial * 2^(attempts-1), max), scaled by a jitter factor in
 * [0.8, 1.2) derived from @jitter01 in [0,1), never above max, never
 * below 1 s. */
int64_t nsr_fed_backoff_delay(const NsrFedConfig *cfg, unsigned attempts, double jitter01);

typedef enum {
  NSR_FED_OK_ACCEPTED = 0,   /* OK true, or `duplicate:` */
  NSR_FED_OK_TRANSIENT,      /* rate-limited:, error:, unknown, no reason */
  NSR_FED_OK_PERMANENT,      /* invalid:, blocked:, banned:, restricted:, pow: */
  NSR_FED_OK_AUTH_REQUIRED,  /* auth-required: — retry after NIP-42 AUTH */
} NsrFedOkClass;

/* libnostr-publish's classification, except that `auth-required:` is its
 * own class: the engine answers AUTH and resends instead of giving up. */
NsrFedOkClass nsr_fed_classify_ok(gboolean accepted, const char *reason);

G_END_DECLS

#endif /* NSR_SESSION_FED_POLICY_H */
