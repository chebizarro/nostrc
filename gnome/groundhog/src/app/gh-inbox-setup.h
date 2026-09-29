#ifndef GH_INBOX_SETUP_H
#define GH_INBOX_SETUP_H

#include "gh-account-relays.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/*
 * Setting up where the account's private messages arrive: its NIP-17
 * kind-10050 "DM relay list" (privacy charter §7.8 step 4, §8.2 G14, D4).
 * GTK-free; the onboarding view (src/ui/gh-onboarding-view.c) drives it.
 *
 * Nothing here contacts a relay on its own. Parsing suggestions, checking
 * addresses and planning a publication are local. Only
 * gh_inbox_setup_start() and gh_inbox_probe_start(), called after the user
 * confirmed (PD-13, PT-9), open connections, and only to the URLs they
 * were given.
 */

/* ---- relay addresses ------------------------------------------------------- */

/* A relay address typed by the user or read from a list, in the form
 * Groundhog stores and publishes: "wss://" (added when no scheme is typed),
 * a lower-case ASCII host (an IDN becomes its xn-- form, so look-alikes
 * show), an optional port and path, no trailing "/", no user name, query or
 * fragment. Plain "ws://" is accepted only for loopback hosts
 * (PD-5). NULL with G_IO_ERROR_INVALID_ARGUMENT and a message meant for the
 * user otherwise. */
gchar *gh_inbox_setup_normalize_url(const gchar *input, GError **error);

/* ---- reviewed suggestions (data/relay-suggestions.json, D4) ---------------- */

#define GH_INBOX_SETUP_SUGGESTIONS_RESOURCE "/org/nostr/Groundhog/relay-suggestions.json"
/* The list is short on purpose; a longer one is refused. */
#define GH_INBOX_SETUP_MAX_SUGGESTIONS 6

/* Whether a relay refuses to hand out kind-1059 gift wraps to a connection
 * that has not signed in (NIP-42) as their recipient. */
typedef enum {
  GH_INBOX_PRIVATE_READS_UNKNOWN,
  GH_INBOX_PRIVATE_READS_YES,
  GH_INBOX_PRIVATE_READS_NO
} GhInboxPrivateReads;

typedef struct {
  gchar *url;          /* normalized wss:// URL */
  gchar *name;
  gchar *description;  /* reviewed, plain-language */
  GhInboxPrivateReads private_reads; /* as reviewed, not as checked now */
} GhInboxSuggestion;

void gh_inbox_suggestion_free(GhInboxSuggestion *suggestion);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhInboxSuggestion, gh_inbox_suggestion_free)

/* Parses the suggestions JSON: {"relays": [{"url", "name", "description",
 * "private_reads": "yes"|"no"|"unknown"}, ...]}. Every URL must be a
 * distinct secure (wss://) relay address that normalizes to itself, and
 * there must be 1..GH_INBOX_SETUP_MAX_SUGGESTIONS of them; anything else is
 * G_IO_ERROR_INVALID_DATA. Returns GhInboxSuggestion items in file order. */
GPtrArray *gh_inbox_setup_parse_suggestions(GBytes *json, GError **error);
/* The same for the copy bundled in the Groundhog GResource. */
GPtrArray *gh_inbox_setup_load_suggestions(GError **error);

/* ---- the kind-10050 list ---------------------------------------------------- */

#define GH_INBOX_SETUP_KIND 10050
/* At most this many message relays; NIP-17 asks for a small list. */
#define GH_INBOX_SETUP_MAX_INBOX_RELAYS 8

/* The unsigned event for the account's signer: kind 10050, one ["relay",
 * url] tag per relay in order, empty content. */
gchar *gh_inbox_setup_build_unsigned(const gchar *pubkey_hex, const gchar *const *relays,
                                     gint64 created_at);

/* ---- the private-reads check ---------------------------------------------- */

typedef enum {
  GH_INBOX_PROBE_PENDING,     /* no answer yet */
  GH_INBOX_PROBE_PRIVATE,     /* refused the unauthenticated read (auth-required:, restricted:) */
  GH_INBOX_PROBE_OPEN,        /* answered it (EOSE) without any sign-in */
  GH_INBOX_PROBE_REFUSED,     /* closed it for another reason (the detail says which) */
  GH_INBOX_PROBE_UNREACHABLE  /* no answer: the connection failed or the deadline passed */
} GhInboxProbeResult;

/* The result a CLOSED reason means. Tolerates relays that wrap the NIP-01
 * machine-readable prefix as "ERROR: auth-required: ...". */
GhInboxProbeResult gh_inbox_probe_classify_closed(const gchar *reason);

/*
 * GhInboxProbe asks each of its relays, on its own connection and without
 * signing in (AUTH identity NONE, charter §4.4: it never authenticates, as
 * the account or otherwise), for kind-1059 events addressed to a random
 * public key: {"kinds":[1059],"#p":[<random>],"limit":1}. The random key
 * tells the relay nothing about the account. A relay that keeps messages
 * private refuses (CLOSED "auth-required:"); one that does not answers with
 * EOSE and no events. Each URL reports exactly one result through the
 * callback; once all have, the connections are closed. Bound to the owning
 * thread-default main context, like GhRelayScope.
 */
typedef struct _GhInboxProbe GhInboxProbe;
typedef void (*GhInboxProbeFunc)(GhInboxProbe *probe, const gchar *url,
                                 GhInboxProbeResult result, const gchar *detail,
                                 gpointer user_data);

/* transport NULL uses gnostr relays; auth_transport is only installed with
 * a custom transport (tests record that nothing is ever sent on it). */
GhInboxProbe *gh_inbox_probe_new(guint64 account_generation,
                                 const GhRelayTransport *transport,
                                 const GhRelayAuthTransport *auth_transport,
                                 gpointer transport_data,
                                 GhInboxProbeFunc callback, gpointer user_data);
GhInboxProbe *gh_inbox_probe_ref(GhInboxProbe *probe);
/* Dropping the last reference cancels an unfinished probe. */
void gh_inbox_probe_unref(GhInboxProbe *probe);
/* Before start; at most 16 URLs (the relay scope's limit). */
gboolean gh_inbox_probe_add_url(GhInboxProbe *probe, const gchar *url, GError **error);
/* Seconds to wait for an answer before UNREACHABLE (default 15, 1..120). */
void gh_inbox_probe_set_deadline(GhInboxProbe *probe, guint seconds);
gboolean gh_inbox_probe_start(GhInboxProbe *probe, GError **error);
/* No callback runs afterwards; the connections close. */
void gh_inbox_probe_cancel(GhInboxProbe *probe);
GhInboxProbeResult gh_inbox_probe_get_result(GhInboxProbe *probe, const gchar *url);
gboolean gh_inbox_probe_is_complete(GhInboxProbe *probe);

/* ---- publishing the list ---------------------------------------------------- */

typedef enum {
  GH_INBOX_SETUP_IDLE,       /* not started: nothing signed or contacted */
  GH_INBOX_SETUP_SIGNING,    /* waiting for Nostr Signer: still nothing contacted */
  GH_INBOX_SETUP_PUBLISHING, /* publishing, and checking the message relays */
  GH_INBOX_SETUP_DONE,       /* finished; at least one relay accepted the list */
  GH_INBOX_SETUP_FAILED      /* finished without any relay accepting it; see get_error */
} GhInboxSetupState;

/* Why a relay is a publication target (charter §4.3 "own list publish"). */
typedef enum {
  GH_INBOX_SETUP_ROLE_INBOX = 1 << 0,     /* a chosen message (10050) relay */
  GH_INBOX_SETUP_ROLE_WRITE = 1 << 1,     /* one of the account's own 10002 write relays */
  GH_INBOX_SETUP_ROLE_DISCOVERY = 1 << 2  /* a discovery-relays source (or adopted as one) */
} GhInboxSetupRole;

/* One publication target and what happened there. Strings are owned by the
 * setup and valid until the next "changed" emission. */
typedef struct {
  const gchar *url;
  GhInboxSetupRole roles;
  GhRelayPublishOutcome outcome;  /* PENDING until the relay's own answer */
  GhRelayOkPrefix prefix;
  const gchar *message;           /* the relay's OK message or a local failure; nullable */
  gboolean probed;                /* inbox relays are checked; others are not */
  GhInboxProbeResult probe;
  const gchar *probe_detail;      /* nullable */
} GhInboxSetupRelay;

typedef struct {
  GhAccountController *accounts;   /* required; must have an active account to start */
  GhAccountRelays *account_relays; /* nullable: supplies the account's 10002 write relays */
  GSettings *settings;             /* nullable: discovery-relays */
  /* NULL transports use gnostr relays; custom ones are for tests. */
  const GhRelayTransport *probe_transport;
  const GhRelayAuthTransport *probe_auth_transport;
  gpointer probe_transport_data;
  const GhRelayPublishTransport *publish_transport;
  const GhRelayPublishAuthTransport *publish_auth_transport;
  gpointer publish_transport_data;
} GhInboxSetupConfig;

/*
 * GhInboxSetup publishes one kind-10050 list for the account that is active
 * when it starts:
 *
 *  1. The list is signed through the account's signer
 *     (gh_account_controller_sign_with_cancellable_async). Nothing is
 *     contacted before the signer answers; a refusal ends the setup with
 *     the signer's error (GH_SIGNER_ERROR_DENIED, ...) and zero connections.
 *  2. The signed list goes, each on its own connection (GhRelayPublish), to
 *     the chosen message relays, the account's own 10002 write relays and
 *     the discovery-relays sources: the "own list publish" purpose of
 *     charter §4.3, the only one here allowed NIP-42 AUTH as the account
 *     (§4.4 R1). GhAuthPolicy (GH_AUTH_PURPOSE_OWN_LIST_PUBLISH) sets that
 *     identity, with the generation's one account signer that the inbox and
 *     the outbox share (R6). Each target reports its own outcome; a relay OK
 *     is relay-local acceptance only. Read-only 10002 relays are never used.
 *  3. At the same time the message relays are checked with a GhInboxProbe
 *     (separate connections, never authenticated).
 *
 * It finishes DONE when at least one relay accepted the list, else FAILED.
 * With adopt_discovery and an empty discovery-relays setting, the message
 * relays that accepted the list then become the discovery relays, so
 * Groundhog can find the list again (the user agreed on the confirm page).
 * An account switch cancels it (FAILED, G_IO_ERROR_CANCELLED). One use per
 * object. "changed" is emitted on the main context after every update.
 */
#define GH_TYPE_INBOX_SETUP (gh_inbox_setup_get_type())
G_DECLARE_FINAL_TYPE(GhInboxSetup, gh_inbox_setup, GH, INBOX_SETUP, GObject)

GhInboxSetup *gh_inbox_setup_new(const GhInboxSetupConfig *config);

/* The targets start() would use for inbox_relays, without contacting
 * anything: GhInboxSetupRelay items (outcome PENDING), inbox relays first.
 * Fails like start() for bad input (G_IO_ERROR_INVALID_ARGUMENT), too many
 * targets, or no active account (G_IO_ERROR_NOT_INITIALIZED). */
GPtrArray *gh_inbox_setup_plan(GhInboxSetup *self, const gchar *const *inbox_relays,
                               gboolean adopt_discovery, GError **error);
gboolean gh_inbox_setup_start(GhInboxSetup *self, const gchar *const *inbox_relays,
                              gboolean adopt_discovery, GError **error);
/* Stops signing, publishing and checking (FAILED, G_IO_ERROR_CANCELLED). */
void gh_inbox_setup_cancel(GhInboxSetup *self);

GhInboxSetupState gh_inbox_setup_get_state(GhInboxSetup *self);
/* Set once FAILED. */
const GError *gh_inbox_setup_get_error(GhInboxSetup *self);
guint gh_inbox_setup_get_n_relays(GhInboxSetup *self);
const GhInboxSetupRelay *gh_inbox_setup_get_relay(GhInboxSetup *self, guint index);
const GhInboxSetupRelay *gh_inbox_setup_lookup(GhInboxSetup *self, const gchar *url);
guint gh_inbox_setup_get_n_accepted(GhInboxSetup *self);
/* The signed list's id once signed; NULL before. */
const gchar *gh_inbox_setup_get_event_id(GhInboxSetup *self);
/* TRUE once the discovery-relays setting was set from this setup. */
gboolean gh_inbox_setup_get_adopted_discovery(GhInboxSetup *self);

G_END_DECLS
#endif
