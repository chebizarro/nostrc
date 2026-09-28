#ifndef GH_RELAY_PUBLISH_H
#define GH_RELAY_PUBLISH_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * GhRelayPublish sends ONE already-signed Nostr event to a bounded, explicit
 * set of ws(s) relay URLs, each on its own private connection, and reports
 * exactly one terminal outcome per URL.
 *
 * What an outcome means, and what it does not:
 *  - ACCEPTED is the relay's own NIP-01 ["OK", id, true, ...] for this event
 *    id. It is relay-local acceptance only: it is NOT upstream or recipient
 *    delivery, and says nothing about propagation to other relays.
 *  - This is not a durable outbox. Nothing is persisted, and nothing is
 *    retried automatically; a caller that wants a retry starts a new publish.
 *  - No relay is ever contacted unless its URL was added explicitly. There are
 *    no fallback or default relays.
 *  - CONNECTION_FAILED covers a failed dial, a connection lost before the OK,
 *    and the per-relay failure deadline. When the EVENT may already have been
 *    written, whether the relay stored it is unknown.
 *
 * Threading: a publish is bound to the thread-default main context that was
 * current when it was created (the owning context). Every callback runs on
 * that context, and every function below, including the transport-to-publish
 * delivery functions and the final unref, must be called on it. The default
 * GNostrRelay transport additionally needs the global default main context to
 * be iterated (by any thread): nostr-gobject dispatches relay OKs there, and
 * the transport forwards them to the owning context.
 */

typedef struct _GhRelayPublish GhRelayPublish;

typedef enum {
  GH_RELAY_PUBLISH_PENDING = 0,      /* not yet terminal (never in a callback) */
  GH_RELAY_PUBLISH_ACCEPTED,         /* relay-local OK true */
  GH_RELAY_PUBLISH_REJECTED,         /* OK false, other than auth-required: */
  GH_RELAY_PUBLISH_AUTH_REQUIRED,    /* OK false "auth-required:"; AUTH is not
                                      * performed here, the caller decides */
  GH_RELAY_PUBLISH_CONNECTION_FAILED,
  GH_RELAY_PUBLISH_CANCELLED         /* only via get_outcome after cancel */
} GhRelayPublishOutcome;

/* NIP-01 / NIP-42 machine-readable OK message prefix. It is reported for any
 * OK, including accepted ones ("duplicate:" is commonly sent with true). */
typedef enum {
  GH_RELAY_OK_PREFIX_NONE = 0,       /* absent or unrecognised prefix */
  GH_RELAY_OK_PREFIX_DUPLICATE,
  GH_RELAY_OK_PREFIX_POW,
  GH_RELAY_OK_PREFIX_BLOCKED,
  GH_RELAY_OK_PREFIX_RATE_LIMITED,
  GH_RELAY_OK_PREFIX_INVALID,
  GH_RELAY_OK_PREFIX_RESTRICTED,
  GH_RELAY_OK_PREFIX_MUTE,
  GH_RELAY_OK_PREFIX_ERROR,
  GH_RELAY_OK_PREFIX_AUTH_REQUIRED
} GhRelayOkPrefix;

typedef struct {
  const gchar *url;
  GhRelayPublishOutcome outcome;
  GhRelayOkPrefix prefix;  /* GH_RELAY_OK_PREFIX_NONE unless an OK was seen */
  const gchar *message;    /* relay OK message or local failure detail;
                            * nullable, valid only during the callback */
} GhRelayPublishResult;

typedef struct {
  guint total;
  guint accepted;
  guint rejected;
  guint auth_required;
  guint connection_failed;
  gboolean any_accepted;   /* at least one relay-local OK true */
  gboolean all_failed;     /* no relay accepted the event */
} GhRelayPublishSummary;

typedef void (*GhRelayPublishUpdateFunc)(GhRelayPublish *publish,
                                         const GhRelayPublishResult *result,
                                         gpointer user_data);
typedef void (*GhRelayPublishDoneFunc)(GhRelayPublish *publish,
                                       const GhRelayPublishSummary *summary,
                                       gpointer user_data);

/* Internal transport seam. open must send the EVENT only to url, on a
 * connection not shared with any other publish or scope, and later report
 * back with gh_relay_publish_ok()/gh_relay_publish_failed() on the owning
 * context. close must stop all deliveries before returning; it is called once
 * per successful open, as soon as the URL is terminal or the publish is
 * cancelled. A publish never hands the transport a URL outside its set. */
typedef struct {
  gpointer (*open)(GhRelayPublish *publish, const gchar *url,
                   const gchar *event_json, gpointer transport_data,
                   GError **error);
  void (*close)(gpointer handle, gpointer transport_data);
} GhRelayPublishTransport;

/* Validates event_json up front: it must be a signed NIP-01 event whose id
 * matches its canonical hash and whose signature verifies. Unsigned or
 * invalid events are refused with G_IO_ERROR_INVALID_DATA. */
GhRelayPublish *gh_relay_publish_new(guint64 account_generation,
                                     const gchar *event_json,
                                     GhRelayPublishUpdateFunc update,
                                     GhRelayPublishDoneFunc done,
                                     gpointer user_data,
                                     GError **error);
GhRelayPublish *gh_relay_publish_new_with_transport(
    guint64 account_generation, const gchar *event_json,
    const GhRelayPublishTransport *transport, gpointer transport_data,
    GhRelayPublishUpdateFunc update, GhRelayPublishDoneFunc done,
    gpointer user_data, GError **error);
GhRelayPublish *gh_relay_publish_ref(GhRelayPublish *publish);
/* Dropping the last reference cancels an unfinished publish. */
void gh_relay_publish_unref(GhRelayPublish *publish);

/* At most 16 distinct URLs accepted by gh_relay_url_validate() (the relay
 * scope's rule), added before start. Re-adding a URL is a no-op. */
gboolean gh_relay_publish_add_url(GhRelayPublish *publish, const gchar *url,
                                  GError **error);
/* Per-relay failure deadline in seconds (default 30, clamped to 1..300),
 * measured from that relay's open. It only ever produces CONNECTION_FAILED:
 * it is a bound on waiting, never a success signal. Set before start. */
void gh_relay_publish_set_deadline(GhRelayPublish *publish, guint seconds);
/* Opens every URL. Fails without any URL. Callbacks may run before this
 * returns (e.g. a transport that cannot open a URL). */
gboolean gh_relay_publish_start(GhRelayPublish *publish, GError **error);
/* Revokes the account generation: no callback runs afterwards, late OKs are
 * discarded and every non-terminal URL becomes CANCELLED. An EVENT that has
 * already been written cannot be recalled. */
void gh_relay_publish_cancel(GhRelayPublish *publish);

guint64 gh_relay_publish_get_generation(const GhRelayPublish *publish);
const gchar *gh_relay_publish_get_event_id(const GhRelayPublish *publish);
GhRelayPublishOutcome gh_relay_publish_get_outcome(const GhRelayPublish *publish,
                                                   const gchar *url);
gboolean gh_relay_publish_is_complete(const GhRelayPublish *publish);

GhRelayOkPrefix gh_relay_ok_prefix_classify(const gchar *message);

/* Transport-to-publish delivery. Unknown URLs, mismatched event ids, any
 * delivery after a URL's terminal outcome, and deliveries after cancellation
 * are ignored, so a URL reports at most one outcome. */
void gh_relay_publish_ok(GhRelayPublish *publish, const gchar *url,
                         const gchar *event_id, gboolean accepted,
                         const gchar *message);
void gh_relay_publish_failed(GhRelayPublish *publish, const gchar *url,
                             const gchar *detail);

G_END_DECLS
#endif
