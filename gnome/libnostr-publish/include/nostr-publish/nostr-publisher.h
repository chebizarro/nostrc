/* nostr-publisher.h - Multi-relay publish engine (NIP-65 outbox commit)
 *
 * SPDX-License-Identifier: MIT
 *
 * NostrPublisher owns "how to publish": sign (optionally), fan an EVENT
 * frame out to every relay in a target set, aggregate the per-relay OK
 * frames under a NostrPublishPolicy, enforce the OK-wait deadline, and
 * hand the caller one final verdict. It stores nothing durable — "what
 * to publish" and "where retry state lives" stay with the caller (e.g.
 * nostr-dav's SQLite outbox), which schedules retries using
 * nostr_publish_policy_backoff_delay().
 *
 * Verdict rules (per request):
 *   * any relay answers with a permanent rejection
 *       -> FAILED_PERMANENT immediately (remaining relays are ignored);
 *   * otherwise, once every relay has settled (OK, unreachable, or timed
 *     out) -> PUBLISHED if the ACK count meets the policy quorum (the
 *     default quorum 0: every relay ACK'd), else RETRY.
 *
 * Transports:
 *   * nostr_publisher_bind_transport() shares a caller-managed transport
 *     (e.g. one owned by a relay-sync layer). The publisher takes a ref
 *     but never connects it or installs callbacks on it; the caller routes
 *     that transport's OK frames into nostr_publisher_record_ok(). A bound
 *     transport that is not connected at dispatch makes that relay
 *     UNREACHABLE for the attempt.
 *   * With nostr_publisher_set_transport_factory(), an unbound URL gets a
 *     publisher-owned transport: the publisher connects it, queues frames
 *     until it is connected, routes its OK frames itself, answers NIP-42
 *     AUTH challenges with the publisher's signer, and marks the relay
 *     UNREACHABLE if the connection fails.
 *   Without a factory (the default), unbound URLs are UNREACHABLE.
 *
 * Time: every timestamp is wall-clock unix seconds (callers persist
 * retry times derived from them). Callers pass `now_ts` to publish and
 * tick so tests can drive time synthetically; verdicts reached from an
 * incoming OK frame use the real wall clock.
 *
 * Threading and reentrancy: not thread-safe; use from one main context,
 * where every callback fires. Callbacks may fire synchronously from
 * publish_signed(), publish(), record_ok() and tick() — do not hold locks
 * or open SQLite transactions across those calls. From inside a callback
 * it is allowed to call publish_signed()/publish() (the settled request
 * has already been removed, so re-publishing the same event id is not
 * ALREADY_IN_FLIGHT), record_ok() and tick(); destroying the publisher
 * from inside a callback is forbidden.
 */
#ifndef NOSTR_PUBLISHER_H
#define NOSTR_PUBLISHER_H

#include <glib.h>

#include "nostr-publish-macros.h"
#include "nostr-publish-policy.h"
#include "nostr-publish-signer.h"
#include "nostr-publish-transport.h"

G_BEGIN_DECLS

typedef struct _NostrPublisher     NostrPublisher;
typedef struct _NostrPublishResult NostrPublishResult;

/**
 * NostrPublishRelayStatus:
 * @NOSTR_PUBLISH_RELAY_PENDING: EVENT sent (or queued); no answer yet
 * @NOSTR_PUBLISH_RELAY_ACCEPTED: OK true, or duplicate:
 * @NOSTR_PUBLISH_RELAY_REJECTED_TRANSIENT: OK false, retryable reason
 * @NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT: OK false, permanent reason
 * @NOSTR_PUBLISH_RELAY_UNREACHABLE: no transport, send failed, or the
 *   publisher-owned connection failed
 * @NOSTR_PUBLISH_RELAY_TIMED_OUT: no OK before the policy deadline
 */
typedef enum {
  NOSTR_PUBLISH_RELAY_PENDING = 0,
  NOSTR_PUBLISH_RELAY_ACCEPTED,
  NOSTR_PUBLISH_RELAY_REJECTED_TRANSIENT,
  NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT,
  NOSTR_PUBLISH_RELAY_UNREACHABLE,
  NOSTR_PUBLISH_RELAY_TIMED_OUT
} NostrPublishRelayStatus;

typedef enum {
  NOSTR_PUBLISH_VERDICT_PUBLISHED = 0,
  NOSTR_PUBLISH_VERDICT_RETRY,
  NOSTR_PUBLISH_VERDICT_FAILED_PERMANENT
} NostrPublishVerdict;

/**
 * NostrPublishRelayFunc:
 * @event_id: id of the event this update belongs to
 * @relay_url: relay whose status changed
 * @status: new (settled) status; never PENDING
 * @reason: (nullable): relay OK reason, transport error message, or a
 *   short diagnostic ("no transport", "timed out")
 *
 * Optional per-relay progress callback, fired once per relay when its
 * status settles.
 */
typedef void (*NostrPublishRelayFunc)(NostrPublisher          *publisher,
                                      const gchar             *event_id,
                                      const gchar             *relay_url,
                                      NostrPublishRelayStatus  status,
                                      const gchar             *reason,
                                      gpointer                 user_data);

/**
 * NostrPublishDoneFunc:
 * @result: (transfer none): valid only for the duration of the call
 *
 * Fired exactly once per accepted request with the final verdict.
 */
typedef void (*NostrPublishDoneFunc)(NostrPublisher           *publisher,
                                     const NostrPublishResult *result,
                                     gpointer                  user_data);

/**
 * nostr_publisher_new:
 * @signer: (nullable) (transfer none): used by nostr_publisher_publish()
 *   and to answer NIP-42 AUTH on publisher-owned transports; the
 *   publisher takes a ref
 */
NOSTR_PUBLISH_API NostrPublisher *nostr_publisher_new(NostrPublishSigner *signer);

/**
 * nostr_publisher_free:
 *
 * Drops every in-flight request WITHOUT invoking its done callback (its
 * destroy notify still runs), releases bound transport refs, and
 * disconnects publisher-owned transports.
 */
NOSTR_PUBLISH_API void nostr_publisher_free(NostrPublisher *self);

/**
 * nostr_publisher_bind_transport:
 * @transport: (nullable) (transfer none): NULL removes the binding
 *
 * Shares a caller-managed transport for @relay_url (see file comment).
 * Replaces any previous binding for the same URL, releasing the old ref
 * immediately. Relays of in-flight requests that were already sent keep
 * waiting for their OK (or the deadline) regardless of rebinding.
 */
NOSTR_PUBLISH_API
void nostr_publisher_bind_transport(NostrPublisher        *self,
                                    const gchar           *relay_url,
                                    NostrPublishTransport *transport);

/**
 * nostr_publisher_set_transport_factory:
 * @factory: (nullable): NULL disables on-demand transports
 *
 * Enables publisher-owned transports for URLs with no binding. Pass
 * nostr_publish_transport_factory_websocket for real relays.
 */
NOSTR_PUBLISH_API
void nostr_publisher_set_transport_factory(NostrPublisher               *self,
                                           NostrPublishTransportFactory  factory,
                                           gpointer                      factory_data,
                                           GDestroyNotify                factory_data_destroy);

/**
 * nostr_publisher_publish_signed:
 * @signed_json: complete signed event JSON (must carry a string `id`)
 * @relays: (array zero-terminated=1): target relay set; duplicates are
 *   collapsed
 * @policy: (nullable): copied; NULL = nostr_publish_policy_init() defaults
 * @now_ts: wall-clock unix seconds used for the OK-wait deadline; <= 0
 *   means "now". Tests pass synthetic values.
 * @relay_cb: (nullable): per-relay progress
 * @done_cb: (not nullable): final verdict
 * @user_data: passed to both callbacks
 * @destroy: (nullable): releases @user_data; called exactly once: right
 *   after @done_cb returns, or before this function returns FALSE, or
 *   from nostr_publisher_free() (in unspecified request order)
 *
 * Sends `["EVENT", <signed_json>]` to every target. If no relay can be
 * reached, @relay_cb/@done_cb (verdict RETRY) run before this function
 * returns.
 *
 * Returns: TRUE if the request was accepted (@done_cb will fire or has
 *   fired). FALSE with @error set (NOSTR_PUBLISH_ERROR: INVALID_EVENT,
 *   NO_RELAYS, ALREADY_IN_FLIGHT) otherwise; no callback fires.
 */
NOSTR_PUBLISH_API
gboolean nostr_publisher_publish_signed(NostrPublisher           *self,
                                        const gchar              *signed_json,
                                        const gchar *const       *relays,
                                        const NostrPublishPolicy *policy,
                                        gint64                    now_ts,
                                        NostrPublishRelayFunc     relay_cb,
                                        NostrPublishDoneFunc      done_cb,
                                        gpointer                  user_data,
                                        GDestroyNotify            destroy,
                                        GError                  **error);

/**
 * nostr_publisher_publish:
 * @unsigned_json: unsigned event JSON, signed with the publisher's signer
 *
 * nostr_publish_signer_sign_event_json() + nostr_publisher_publish_signed().
 * The signed JSON is available from nostr_publish_result_get_signed_json().
 * Callers that must persist the signed event before it goes out (so a
 * retry does not re-prompt the signer) sign themselves and use
 * nostr_publisher_publish_signed().
 *
 * Returns: as nostr_publisher_publish_signed(); additionally FALSE with
 *   NOSTR_PUBLISH_ERROR_NO_SIGNER, or with the signer's GError propagated
 *   unchanged (normally NOSTR_PUBLISH_SIGNER_ERROR; classify with
 *   nostr_publish_signer_error_is_permanent()).
 */
NOSTR_PUBLISH_API
gboolean nostr_publisher_publish(NostrPublisher           *self,
                                 const gchar              *unsigned_json,
                                 const gchar *const       *relays,
                                 const NostrPublishPolicy *policy,
                                 gint64                    now_ts,
                                 NostrPublishRelayFunc     relay_cb,
                                 NostrPublishDoneFunc      done_cb,
                                 gpointer                  user_data,
                                 GDestroyNotify            destroy,
                                 GError                  **error);

/**
 * nostr_publisher_record_ok:
 *
 * Feeds an OK frame from @relay_url in. Publisher-owned transports call
 * this themselves; callers route bound transports' OK callbacks here.
 * Unknown event ids, relays outside the request's target set, and relays
 * that already settled are ignored.
 *
 * Returns: TRUE if the frame settled a pending relay of an in-flight
 *   request.
 */
NOSTR_PUBLISH_API
gboolean nostr_publisher_record_ok(NostrPublisher *self,
                               const gchar    *relay_url,
                               const gchar    *event_id,
                               gboolean        accepted,
                               const gchar    *reason);

/**
 * nostr_publisher_tick:
 * @now_ts: wall-clock unix seconds
 *
 * Marks relays still PENDING on requests whose deadline has passed as
 * TIMED_OUT and settles those requests. Call periodically (nostr-dav:
 * every 30 s); the publisher arms no timers of its own.
 *
 * Returns: TRUE while requests remain in flight.
 */
NOSTR_PUBLISH_API gboolean nostr_publisher_tick(NostrPublisher *self, gint64 now_ts);

NOSTR_PUBLISH_API gboolean nostr_publisher_is_in_flight   (NostrPublisher *self,
                                                           const gchar    *event_id);
NOSTR_PUBLISH_API guint    nostr_publisher_get_n_in_flight(NostrPublisher *self);

/* ---- Result accessors (valid only inside NostrPublishDoneFunc) ---- */

NOSTR_PUBLISH_API NostrPublishVerdict nostr_publish_result_get_verdict    (const NostrPublishResult *result);
NOSTR_PUBLISH_API const gchar        *nostr_publish_result_get_event_id   (const NostrPublishResult *result);
NOSTR_PUBLISH_API const gchar        *nostr_publish_result_get_signed_json(const NostrPublishResult *result);
/** Returns: (nullable): the permanent-rejection reason for FAILED_PERMANENT. */
NOSTR_PUBLISH_API const gchar        *nostr_publish_result_get_reason     (const NostrPublishResult *result);
/** Returns: wall-clock unix seconds at which the verdict was reached: the
 *  tick's @now_ts for timeouts, the dispatch @now_ts for an attempt where
 *  no relay was reachable, the real wall clock for OK-driven verdicts.
 *  Base retry times on it. */
NOSTR_PUBLISH_API gint64              nostr_publish_result_get_completed_at(const NostrPublishResult *result);
NOSTR_PUBLISH_API guint               nostr_publish_result_get_n_relays   (const NostrPublishResult *result);
NOSTR_PUBLISH_API guint               nostr_publish_result_get_n_accepted (const NostrPublishResult *result);

/**
 * nostr_publish_result_get_relay:
 * @index: 0 .. n_relays-1, in target-set order
 * @out_status: (out) (optional)
 * @out_reason: (out) (optional) (nullable) (transfer none)
 *
 * Returns: (transfer none): the relay URL.
 */
NOSTR_PUBLISH_API
const gchar *nostr_publish_result_get_relay(const NostrPublishResult  *result,
                                            guint                      index,
                                            NostrPublishRelayStatus   *out_status,
                                            const gchar              **out_reason);

G_END_DECLS
#endif /* NOSTR_PUBLISHER_H */
