/*
 * session_outbox — durable store-and-forward queue of the session relay's
 * upstream federation client (bead nostrc-7d96).
 *
 * One SQLite database next to the nostrdb store
 * ($XDG_DATA_HOME/nostr/session-relay/outbox.sqlite3), WAL mode,
 * synchronous=FULL. Thread-safe: every call takes the outbox mutex; the
 * relay's event loop calls nsr_outbox_ingest(), the federation thread does
 * the rest, the D-Bus thread reads stats.
 *
 * Durability contract. nsr_outbox_ingest() commits (fsync) BEFORE the
 * relay stores the event in nostrdb and answers OK true, so "OK true from
 * relay.sock" means "held locally and queued for upstream". Delivery is
 * at-least-once, keyed by event id (a resend after a crash is answered
 * `duplicate:` = accepted by the remote).
 *
 * Event states (events.state):
 *   new         queued, not yet classified / routed
 *   unroutable  no upstream target known yet (e.g. no kind 10002 cached,
 *               or local account not known yet); re-routed with backoff and
 *               immediately whenever a relay-list event is ingested
 *   pending     targets resolved; at least one target not yet settled
 *   forwarded   every target acknowledged (OK true / duplicate:)
 *   partial     all settled, some acknowledged, some failed
 *   failed      all settled, none acknowledged (rejected / expired)
 *   skipped     never forwarded: not a local account's event, or invalid
 *   superseded  a newer version of the same replaceable/addressable event
 *               was queued before this one went out
 *   cancelled   deleted (NIP-09 kind 5 by the author) before it went out
 * Target states (targets.state): pending, acked, failed, cancelled.
 *
 * In-flight sends are leases: nsr_outbox_take_due() pushes the target's
 * next_attempt_at forward by the lease; a crash therefore retries it after
 * the lease, a live engine records the result first.
 */
#ifndef NSR_SESSION_OUTBOX_H
#define NSR_SESSION_OUTBOX_H

#include <glib.h>
#include <stdint.h>

#include "session_fed_policy.h"

G_BEGIN_DECLS

typedef struct NsrOutbox NsrOutbox;

NsrOutbox *nsr_outbox_open(const char *path, GError **error);
void nsr_outbox_close(NsrOutbox *ob);

typedef struct {
  const char *id;          /* 64-hex */
  const char *pubkey;      /* 64-hex */
  int kind;
  int64_t created_at;
  const char *replace_key; /* nullable; see nsr_fed_replace_key() */
  const char *json;        /* signed event */
  gboolean enqueue;        /* queue for upstream (passed the static verdict) */
  gboolean hint;           /* relay-list kind (10002/10050/10009): remember it */
} NsrOutboxIngest;

/* One durable transaction: optional queue row (idempotent by id) and
 * optional relay-list hint (newest created_at per (pubkey, kind) wins; a
 * newer hint makes every unroutable event due for re-routing). Called
 * after the local store accepted the event. 0 or -1. */
int nsr_outbox_ingest(NsrOutbox *ob, const NsrOutboxIngest *in, int64_t now, GError **error);

/* ── Routing (federation thread) ──────────────────────────────────────── */

typedef struct {
  char *id, *pubkey, *json, *replace_key;
  int kind;
  int64_t enqueued_at;
  guint route_attempts;
} NsrOutboxEvent;
void nsr_outbox_event_free(gpointer p);

/* Events in state new, or unroutable with next_route_at <= now (oldest
 * first). Element type NsrOutboxEvent. */
GPtrArray *nsr_outbox_take_routable(NsrOutbox *ob, int64_t now, guint limit);

/* new|unroutable -> pending with one pending target per relay. */
int nsr_outbox_set_routed(NsrOutbox *ob, const char *id, const char *const *relays,
                          NsrFedLane lane, int64_t now);
/* new|unroutable -> unroutable (route_attempts+1). @waiting_account: held
 * only because the local account is not known yet — such events are never
 * expired by nsr_outbox_expire() (the user's own events must not be lost
 * to a missing signer); nsr_outbox_reroute_all() releases them. */
int nsr_outbox_set_unroutable(NsrOutbox *ob, const char *id, const char *reason,
                              int64_t next_route_at, gboolean waiting_account, int64_t now);
/* Any unsettled state -> @state (skipped|failed), with @reason. */
int nsr_outbox_set_final(NsrOutbox *ob, const char *id, const char *state,
                         const char *reason, int64_t now);

/* Unsettled events with the same replace key and an older created_at (or
 * same created_at, lower id) than @created_at become `superseded`; their
 * pending targets `cancelled`. Returns the number superseded. */
int nsr_outbox_supersede(NsrOutbox *ob, const char *replace_key, const char *keep_id,
                         int64_t created_at, int64_t now);
/* NIP-09: unsettled events by @pubkey whose id (or coordinate, when
 * @is_coordinate) is @ref become `cancelled`. Returns the number. */
int nsr_outbox_cancel_ref(NsrOutbox *ob, const char *pubkey, const char *ref,
                          gboolean is_coordinate, int64_t now);
/* Relays that acknowledged @ref (event id or coordinate), from delivery
 * records that outlive pruned outbox rows (kept ~400 days). GStrv. */
GStrv nsr_outbox_acked_relays(NsrOutbox *ob, const char *ref, gboolean is_coordinate);
/* Newest relay-list hint JSON for (pubkey, kind), or NULL. g_free(). */
char *nsr_outbox_hint_json(NsrOutbox *ob, const char *pubkey, int kind);

/* ── Delivery (federation thread) ─────────────────────────────────────── */

typedef struct {
  char *event_id, *relay, *json;
  NsrFedLane lane;
  guint attempts;
} NsrOutboxDue;
void nsr_outbox_due_free(gpointer p);

/* Pending targets with next_attempt_at <= now, oldest event first; each
 * returned target is leased (next_attempt_at = now + @lease_seconds) so it
 * is not handed out again while in flight; the lease must exceed the
 * engine's own deadline for the attempt. */
GPtrArray *nsr_outbox_take_due(NsrOutbox *ob, int64_t now, int lease_seconds, guint limit);

typedef enum {
  NSR_OUTBOX_ACKED = 0,
  NSR_OUTBOX_TRANSIENT,  /* retry after backoff (attempts+1) */
  NSR_OUTBOX_PERMANENT,  /* target failed for good */
} NsrOutboxResult;

/* Record a delivery outcome for a pending target. TRANSIENT computes the
 * next attempt with nsr_fed_backoff_delay(@cfg, attempts, jitter); a
 * transient failure past cfg->max_age_seconds since enqueue becomes
 * failed ("expired: …"). Recomputes the event state; returns the new
 * target state ("acked"/"pending"/"failed") via @out_target_state and the
 * event state via @out_event_state (both static strings, may be NULL).
 * Returns 0, or -1 if the target is not pending (late / duplicate OK). */
int nsr_outbox_record(NsrOutbox *ob, const NsrFedConfig *cfg, const char *event_id,
                      const char *relay, NsrOutboxResult result, const char *reason,
                      double jitter01, int64_t now, const char **out_target_state,
                      const char **out_event_state);

/* Pending targets / unroutable events older than @max_age: failed
 * ("expired"); events held for the local account are exempt. Returns the
 * number of events settled by it. */
int nsr_outbox_expire(NsrOutbox *ob, int64_t now, int64_t max_age);
/* Delete settled events settled before now - @keep (counters persist). */
int nsr_outbox_prune(NsrOutbox *ob, int64_t now, int64_t keep);

/* Make every unroutable event due for re-routing now (e.g. the local
 * account just became known). */
void nsr_outbox_reroute_all(NsrOutbox *ob);
/* Only the events held for the local account (set_unroutable with
 * @waiting_account): an org.nostr.Signer answer just came in. */
void nsr_outbox_reroute_held(NsrOutbox *ob);
/* Clean shutdown: a leased, still-pending target becomes due at @now
 * again instead of waiting for the lease to lapse. */
void nsr_outbox_release_lease(NsrOutbox *ob, const char *event_id, const char *relay,
                              int64_t now);

/* Earliest time anything becomes due (target attempt or re-route), or
 * INT64_MAX when nothing is queued. */
int64_t nsr_outbox_next_due(NsrOutbox *ob);

/* ── Observability ────────────────────────────────────────────────────── */

typedef struct {
  guint64 queued;          /* new + unroutable + pending */
  guint64 unroutable;
  guint64 pending_targets;
  guint64 forwarded;       /* lifetime counters (persisted) */
  guint64 partial;
  guint64 failed;
  guint64 skipped;
  char *last_error;        /* g_free; "" if none */
} NsrOutboxStats;
void nsr_outbox_stats(NsrOutbox *ob, NsrOutboxStats *out);
void nsr_outbox_stats_clear(NsrOutboxStats *s);
void nsr_outbox_set_last_error(NsrOutbox *ob, const char *msg);

typedef struct {
  char *relay, *state, *reason;
  guint attempts;
  int64_t updated_at, acked_at; /* acked_at 0 unless acked */
} NsrOutboxTargetInfo;
void nsr_outbox_target_info_free(gpointer p);

/* 0 and outputs set, or -1 (unknown id). @targets: NsrOutboxTargetInfo. */
int nsr_outbox_event_status(NsrOutbox *ob, const char *id, char **state, char **detail,
                            GPtrArray **targets);

typedef struct {
  char *relay;
  guint pending;
  guint64 acked, failed;
} NsrOutboxRelayCounts;
void nsr_outbox_relay_counts_free(gpointer p);
/* Per-relay target counts over the rows still in the outbox. */
GPtrArray *nsr_outbox_relay_counts(NsrOutbox *ob);

/* Retention hook for nostrc-8rxk / nostrc-prqu.17 (eviction is NOT
 * implemented here): TRUE when the session relay owes nothing upstream
 * for @id — not queued, or settled (forwarded / partial / failed /
 * skipped / superseded / cancelled). An evictor must keep events for
 * which this is FALSE, and must never evict local-only events on this
 * basis alone (they were never meant to exist anywhere else). */
gboolean nsr_outbox_eviction_eligible(NsrOutbox *ob, const char *id);

/* Accounts learned from org.nostr.Signer, persisted across restarts. */
GStrv nsr_outbox_get_accounts(NsrOutbox *ob);
void nsr_outbox_add_account(NsrOutbox *ob, const char *pubkey_hex);

G_END_DECLS

#endif /* NSR_SESSION_OUTBOX_H */
