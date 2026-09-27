/* nostr-publish-policy.h - Publish policy, OK classification, NIP-65
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure (I/O-free) helpers shared by every libnostr-publish consumer:
 *
 *   * NostrPublishPolicy — how many relays in the target set must ACK
 *     (NIP-65 outbox commit = all; numeric quorum is an operator
 *     override), how long to wait for OK frames, the retry backoff curve,
 *     and the upstream routing preference.
 *   * NIP-01 OK-reason classification (accept / transient / permanent).
 *   * NIP-65 kind-10002 relay-list parsing into read / write relay sets.
 */
#ifndef NOSTR_PUBLISH_POLICY_H
#define NOSTR_PUBLISH_POLICY_H

#include <glib.h>

#include "nostr-publish-macros.h"

G_BEGIN_DECLS

#define NOSTR_PUBLISH_ERROR (nostr_publish_error_quark())
NOSTR_PUBLISH_API GQuark nostr_publish_error_quark(void);

typedef enum {
  /* Event JSON is not an object or lacks a string `id`. */
  NOSTR_PUBLISH_ERROR_INVALID_EVENT = 1,
  /* The target relay set is empty. */
  NOSTR_PUBLISH_ERROR_NO_RELAYS,
  /* An event with the same id is already being published. */
  NOSTR_PUBLISH_ERROR_ALREADY_IN_FLIGHT,
  /* nostr_publisher_publish() called on a publisher without a signer. */
  NOSTR_PUBLISH_ERROR_NO_SIGNER,
  /* Relay-list event is not a well-formed kind-10002 event. */
  NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST
} NostrPublishError;

/**
 * NostrPublishUpstream:
 * @NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY: publish only through the
 *   session-local relay; never contact write relays directly.
 * @NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_OR_DIRECT: prefer the session
 *   relay; publish to the write relays when no session relay is known.
 * @NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY: publish to the write relays.
 *
 * Consumed by nostr_publish_policy_select_targets() only; the publisher
 * engine publishes to whatever relay set it is given. nostr-share and
 * nostr-dav (its publisher and relay-sync, nostrc-862u) route through it.
 * The session relay forwards upstream only while its FederationState is
 * `active` or `waiting-for-account` (nostrc-7d96): callers pass a session
 * relay URL here only then (nostr-publish-session-relay.h, nostrc-t24q).
 */
typedef enum {
  NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_OR_DIRECT = 0,
  NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY,
  NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY
} NostrPublishUpstream;

/**
 * NostrPublishPolicy:
 * @quorum: ACKs required for a PUBLISHED verdict. 0 (default) = every
 *   relay in the target set (NIP-65 outbox commit); N > 0 = at least N,
 *   clamped to the target-set size when the verdict is computed
 * @ok_wait_sec: how long an in-flight publish waits for OK frames before
 *   nostr_publisher_tick() marks silent relays timed out; 0 = 120
 * @backoff_initial_sec: retry delay after the first failed attempt;
 *   0 = 60
 * @backoff_max_sec: retry delay cap; 0 = 3600
 * @upstream: routing preference for nostr_publish_policy_select_targets();
 *   the zero value is SESSION_RELAY_OR_DIRECT
 *
 * Every field's zero value means "default", so `NostrPublishPolicy p =
 * {0};` and nostr_publish_policy_init() are equivalent. The trailing
 * padding keeps the struct size stable across minor releases.
 */
typedef struct {
  guint                quorum;
  guint                ok_wait_sec;
  guint                backoff_initial_sec;
  guint                backoff_max_sec;
  NostrPublishUpstream upstream;

  /*< private >*/
  gpointer             padding[4];
} NostrPublishPolicy;

#define NOSTR_PUBLISH_DEFAULT_OK_WAIT_SEC          120
#define NOSTR_PUBLISH_DEFAULT_BACKOFF_INITIAL_SEC   60
#define NOSTR_PUBLISH_DEFAULT_BACKOFF_MAX_SEC     3600

/** Zero-fills @policy (all defaults). */
NOSTR_PUBLISH_API void nostr_publish_policy_init(NostrPublishPolicy *policy);

/**
 * nostr_publish_policy_backoff_delay:
 * @attempts: number of attempts already made (0 for the first retry)
 *
 * Returns: `min(backoff_initial_sec * 2^attempts, backoff_max_sec)`
 *   seconds, with 0 fields replaced by their defaults.
 */
NOSTR_PUBLISH_API
gint64 nostr_publish_policy_backoff_delay(const NostrPublishPolicy *policy,
                                          guint                     attempts);

/**
 * nostr_publish_policy_required_acks:
 * @n_targets: size of the target relay set
 *
 * Returns: the number of ACKs needed for a PUBLISHED verdict: @n_targets
 *   when @quorum is 0, otherwise @quorum clamped to [1, @n_targets]
 *   (1 when @n_targets is 0).
 */
NOSTR_PUBLISH_API
guint nostr_publish_policy_required_acks(const NostrPublishPolicy *policy,
                                         guint                     n_targets);

/**
 * nostr_publish_policy_select_targets:
 * @write_relays: (nullable): the author's write relays (NIP-65 or a
 *   configured fallback)
 * @session_relay_url: (nullable): session-local relay URL, if known
 *
 * Applies @policy->upstream: DIRECT_ONLY -> @write_relays;
 * SESSION_RELAY_ONLY -> [@session_relay_url]; SESSION_RELAY_OR_DIRECT ->
 * [@session_relay_url] when known, else @write_relays. Order is preserved
 * and exact duplicates dropped; the session relay is never mixed with
 * write relays (it forwards upstream itself).
 *
 * Used by nostr-share (nostrc-1xak) and nostr-dav (nostrc-862u).
 *
 * Returns: (transfer full) (nullable): a non-empty relay set, or NULL
 *   with @error set to NOSTR_PUBLISH_ERROR_NO_RELAYS when the policy
 *   leaves nothing to publish to (SESSION_RELAY_ONLY with no session
 *   relay, or no write relays).
 */
NOSTR_PUBLISH_API
GStrv nostr_publish_policy_select_targets(const NostrPublishPolicy *policy,
                                          const gchar *const       *write_relays,
                                          const gchar              *session_relay_url,
                                          GError                  **error);

/**
 * NostrPublishOkClass:
 *
 * Classification of a NIP-01 `["OK", id, accepted, reason]` frame.
 * Reasons are matched by case-sensitive prefix (NIP-01 prescribes the
 * lowercase machine-readable prefixes):
 *
 * | accepted | reason prefix                                | class     |
 * |----------|----------------------------------------------|-----------|
 * | true     | (any)                                        | ACCEPT    |
 * | false    | `duplicate:`                                 | ACCEPT    |
 * | false    | `invalid:` `blocked:` `banned:`              | PERMANENT |
 * | false    | `restricted:` `auth-required:` (1)           | PERMANENT |
 * | false    | anything else (`error:`, `rate-limited:`, …) | TRANSIENT |
 * | false    | no reason                                    | TRANSIENT |
 *
 * (1) retry-after-NIP-42-AUTH is not implemented, so a relay demanding
 * AUTH would otherwise spin the retry loop forever.
 */
typedef enum {
  NOSTR_PUBLISH_OK_TRANSIENT = 0,
  NOSTR_PUBLISH_OK_ACCEPT,
  NOSTR_PUBLISH_OK_PERMANENT
} NostrPublishOkClass;

NOSTR_PUBLISH_API
NostrPublishOkClass nostr_publish_classify_ok(gboolean     accepted,
                                              const gchar *reason);

/**
 * NostrPublishNip65Usage:
 * @NOSTR_PUBLISH_NIP65_READ: relays marked `read` or unmarked (inbox)
 * @NOSTR_PUBLISH_NIP65_WRITE: relays marked `write` or unmarked (outbox)
 */
typedef enum {
  NOSTR_PUBLISH_NIP65_READ  = 1 << 0,
  NOSTR_PUBLISH_NIP65_WRITE = 1 << 1
} NostrPublishNip65Usage;

/**
 * nostr_publish_nip65_relays:
 * @relay_list_json: a kind-10002 event (signed or unsigned) as JSON
 * @usage: which marker set to extract
 *
 * Collects `["r", <url>]` / `["r", <url>, "read"|"write"]` tags matching
 * @usage, in tag order, skipping non-ws(s) URLs and exact duplicates.
 * URLs are deliberately NOT normalised (no trailing-slash or case
 * folding): they are returned verbatim so they match transports keyed by
 * the same string, and `wss://a` / `wss://a/` stay distinct.
 *
 * Returns: (transfer full): a NULL-terminated array (possibly empty), or
 *   NULL with @error set (NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST) if the
 *   JSON is not a kind-10002 event object.
 */
NOSTR_PUBLISH_API
GStrv nostr_publish_nip65_relays(const gchar            *relay_list_json,
                                 NostrPublishNip65Usage  usage,
                                 GError                **error);

G_END_DECLS
#endif /* NOSTR_PUBLISH_POLICY_H */
