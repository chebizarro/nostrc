#ifndef GH_MLS_SERVICE_H
#define GH_MLS_SERVICE_H

#include <marmot/marmot.h>
#if defined(__has_include)
#if __has_include(<marmot/marmot-version.h>)
#include <marmot/marmot-version.h>
#endif
#endif

#include "gh-account-relays.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-inbox-resolver.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhMlsService: one account's Marmot (MLS) encrypted groups (nostrc-qp24.13
 * part 1; privacy charter §2.2, §3.9 D5, §4.3, §4.4, §7.9-§7.10; Marmot
 * MIP-00..03 and transports/nostr.md). GTK-free, main context only, over the
 * account's open GhStore: libmarmot runs on GhStoreMarmot (gh-store-marmot.h),
 * so every piece of MLS state -- group secrets, sender ratchets, KeyPackage
 * private keys, pending Commits, the Welcome outbox -- lives only in the
 * encrypted store, in the same transactions as the messages and the outbox.
 * An ephemeral store is refused (KC-4: MLS is disabled without durable state).
 *
 * KeyPackages (MIP-00, kind 30443). While the account is active and online
 * the service keeps one KeyPackage of the account published per format
 * (GhMlsKeyPackageFormat, nostrc-lf62): the adopted one (MDK 0.11, current
 * White Noise; built in with GH_MLS_ADOPTED_KEY_PACKAGES) and the MDK 0.8
 * one, so that older White Noise and Amethyst can still invite the account.
 * The price is two KeyPackage events, which anyone can link to each other
 * and to the account (same author, relays and timing), showing that its app
 * speaks both formats. Each is made by libmarmot (its private init key
 * stored first, in the store), signed by the account's signer, and
 * published to the account's kind-10002 write-capable relays only -- `r`
 * entries marked "write" or unmarked; never read-only ones, never the
 * kind-10050 inbox relays, never kind 10051 (Marmot transports/nostr.md
 * "KeyPackage publication"; charter §4.3 "own list publish": GhAuthPolicy
 * OWN_LIST_PUBLISH, account AUTH only on challenge). Each format reuses its
 * own addressable `d` slot, so a new one replaces its format's old one on
 * relays and never the other format's (each one's created_at strictly
 * newer); the adopted one is made first and the MDK 0.8 one with it, so the
 * MDK 0.8 one is the account's newest kind 30443 for a legacy reader that
 * ignores slots: an adopted replacement leaves the MDK 0.8 one due, recorded
 * in the store, until it goes out, so one held for an invitation or failed
 * goes at the hold's end, the retry or the next start (review M1). The MDK
 * 0.8 format follows the "mls-legacy-key-packages" setting ("Let people
 * using older Marmot apps invite me", on by default; privacy charter
 * amendment 2026-10-01): switched off, its KeyPackage is withdrawn with a
 * NIP-09 deletion request to the same write relays (its `a` address and an
 * `e` tag per MDK 0.8 KeyPackage event id this run published, dated from
 * libmarmot's KeyPackage clock so never before the newest of them: review
 * M4) and its private keys deleted once a relay accepted that request (a
 * slot that can't be read keeps them and retries: L3); switched on, a new
 * one is published in its slot. A build without the adopted producer keeps
 * it on (and Preferences doesn't show the switch).
 * A format's KeyPackage
 * is rotated when it is older than the lifetime (default
 * GH_MLS_KEY_PACKAGE_LIFETIME) and after a Welcome to it was joined -- once
 * no other received invitation is pending (they were most likely made with
 * the same KeyPackage), even across a restart -- never after a Welcome that
 * failed. A join rotates the format of the KeyPackage it spent (libmarmot
 * records it; review N1); the user's rotate, every format published.
 *
 * KeyPackage lifecycle (nostrc-0bdg; foundation/key-packages.md). A due
 * replacement -- lifetime, the user's rotate or a join -- is not published
 * while a received invitation is pending (it was most likely made with the
 * current KeyPackage, whose key the replacement's confirmation deletes), for
 * at most GH_MLS_KEY_PACKAGE_MAX_HOLD and never closer than
 * GH_MLS_KEY_PACKAGE_EXPIRY_MARGIN to the current one's Lifetime end; it is
 * looked at again on accept, decline, failed accept, start and a timer.
 * The replacement is acknowledgement-tied: the first relay OK for a new
 * KeyPackage confirms it (marmot_key_package_confirm_published()), and only
 * then does libmarmot delete the private material of the older ones. Until
 * then a delayed Welcome to the old (last-resort) KeyPackage still joins;
 * after it, it fails: the spec's deliberate trade-off. Confirmation, the
 * hold and the deletion are per format: a format's replacement retires only
 * that format's older keys, and a format with no KeyPackage left is not
 * held. Groundhog's KeyPackages are last-resort, so a join leaves the key
 * for further Welcomes until that confirmation; libmarmot deletes a
 * consumed single-use one at the join. An expired KeyPackage's private
 * material goes at every start and publish check
 * (marmot_key_package_sweep_expired()).
 *
 * Invitations need consent (charter PD-8, PT-8). A KeyPackage is looked up
 * (gh-mls-key-packages.h: discovery relays, then the person's 10002 write
 * relays; ephemeral AUTH only) only for someone the account accepted: a
 * peer of an accepted, non-request NIP-17 conversation, or whoever
 * GhMlsServiceConfig.may_look_up allows. Never for a message request.
 *
 * Groups. A new group is made with the account as its only member and
 * admin, then everyone invited joins through ONE Add Commit: staged with
 * gh-mls-commits (T-mls: the pending Commit and its sealed kind 445 commit
 * together), published byte for byte to the group relays, merged on the first
 * relay OK (MIP-03: never before), and only then are the Welcomes sent. The
 * same lifecycle carries adds, removals and metadata changes. A restart
 * republishes a pending Commit exactly as sealed (gh_mls_commit_resume()).
 *
 * Welcomes (MIP-02) leave through libmarmot's Welcome outbox: each is sealed
 * by the account's signer, gift-wrapped (NIP-59, a fresh ephemeral key) to
 * the invitee alone, stored signed (outbox role WELCOME_WRAP, T-seal) and
 * published to the invitee's kind-10050 inbox relays only (GhInboxResolver;
 * RECIPIENT_WRAP: ephemeral AUTH only), republished byte for byte until one
 * relay accepted it, and only then marked sent. Received Welcomes come from
 * the account's own inbox (GhDmInbox's Welcome sink): each is stored as a
 * pending invitation (with its wrap id seen, one transaction) and joined only
 * when the user accepts it (gh_mls_service_accept_invite()).
 *
 * Group messages (MIP-03, kind 445). Per joined group one live REQ
 * {kinds:[445], #h:[nostr group id], since} on its own connection to exactly
 * the group's relays (GhAuthPolicy MLS_ROUTING: ephemeral AUTH only; a stable
 * per-group Tor isolation label), event driven, never polled. Every event
 * goes through marmot_process_message() (the relay path: the envelope's id
 * and signature are verified first) inside one store transaction with its
 * admission to the conversation model (T-admit), so a ratchet step and its
 * message commit together. Events of a later epoch wait (bounded) for the
 * Commit that makes them readable; duplicates are dropped by the scope, by
 * libmarmot's processed markers and by the seen set. Relays cap a REQ's
 * stored answer (strfry 500, some 100) and answer newest first, so the
 * backfill is paged per relay (GhRelayScope backfill paging, nostrc-cpwf):
 * REQ limit GH_MLS_SERVICE_PAGE_LIMIT, then older pages with until = the
 * oldest received, down to since, at most GH_MLS_SERVICE_MAX_PAGES, while
 * the live REQ stays open; a relay has answered only once its paging ended
 * complete. Backfill (stored answers and older pages) is kept, from every
 * group relay together, until no relay is still delivering a backfill
 * round (each has sent its EOSE or failed, or has sent nothing), and then
 * applied oldest first as one set: libmarmot keeps only a few skipped
 * message keys per sender, so a long backlog applied newest first -- or in
 * one relay's share at a time, since events are deduplicated across relays
 * -- would leave older messages unreadable. Live events wait with it while
 * a backfill is pending, and are applied as they come otherwise. The wait is
 * bounded: relays that delivered part of a round and then deliver nothing
 * more of their stored answers for GH_MLS_SERVICE_BACKFILL_QUIET_S -- their
 * live traffic does not count (nostrc-iihf) -- (once every other relay has
 * finished) are
 * given up as answered-incomplete, and so is every relay still delivering
 * when the store reaches its bound; the store is then applied. The read
 * cursor (the
 * REQ's since, minus an overlap) moves only for events libmarmot accepted
 * and the store kept, only while every group relay has answered, never past
 * now and never past an event held or dropped unread. Held events are kept
 * once per id (oldest dropped first when full) across network flaps and
 * retried as a fixpoint after every Commit (a whole backlog at once); one
 * still unreadable after GH_MLS_SERVICE_JUNK_AFTER_COMMITS new Commits is
 * junk. Nothing from before the account joined is held. A joined group is
 * read from its Welcome's time. A sent message is one
 * transaction -- the outgoing message row, marmot_create_message() (the
 * sender ratchet step) and its sealed kind 445 -- committed before anything
 * is published (libmarmot 0.8.0 review N1), then published to the group
 * relays; its status is honest (SENT once one group relay accepted it) and a
 * restart republishes it byte for byte. libmarmot signs every kind 445 with a
 * fresh ephemeral key: the account key never appears on a group relay.
 *
 * Rooms. Joined groups are rooms of the GhConversationStore (backend MLS,
 * room id gh_message_mls_room_id(), durable through gh-store-mls.h), listed
 * even before their first message and titled with the group's name.
 *
 * Account proof (libmarmot >= 0.10.0, GH_MLS_SERVICE_ACCOUNT_PROOF). Every
 * member leaf carries the account's signature over the leaf's MLS key. At
 * each start (libmarmot's instance key is not stored) the service asks the
 * account's signer to sign libmarmot's local-only kind:450 template (never
 * published), checks it is exactly that event by the account, and hands it
 * to marmot_set_account_proof(). Until then the identity state is WAITING
 * ("Waiting for approval"), no KeyPackage is made and no group created
 * (GH_MLS_SERVICE_ERROR_NOT_ENROLLED). The request belongs to the account
 * generation: a switch cancels it, a network flap does not; a declined one
 * is asked again only for a new account generation, at the next start, or
 * through gh_mls_service_retry_identity(). An
 * invitee (or a group member) whose app cannot prove its account is
 * GH_MLS_SERVICE_ERROR_NEEDS_UPDATE only while the account chose to require
 * proofs (below).
 *
 * Members without the proof (nostrc-6ukh). Legacy-profile groups (MDK 0.8,
 * Amethyst/Quartz) hold members whose app cannot prove their account. By
 * default libmarmot admits them there (MarmotConfig.allow_unproven_members;
 * never in adopted-profile groups, never a proof that does not verify, and
 * a slot an Add fills is always a new claim an admin made), so they can be
 * invited and their groups joined. What the service knows of each such
 * device (gh_mls_group_get_member_identity()) comes from evidence already
 * in hand, without any network traffic (W24 review H1):
 *  - the KeyPackage the account itself added the device with;
 *  - the Welcome the account joined with: the device that signed it, when
 *    its account sent it (the NIP-59 seal; libmarmot's welcome_signer) --
 *    typically the creator of an MDK group who invited us (owkh);
 *  - a KeyPackage event the service already fetched (an invitation lookup,
 *    an earlier Verify), checked by libmarmot
 *    (marmot_key_package_event_matches_member());
 *  - the device's own renewal: a Commit by the device's leaf (its
 *    UpdatePath, signed in by its previous key) carries what was known of
 *    the old key over to the new one, so MDK's key-rotating self-update
 *    keeps a verified member verified (W24 review M1).
 * Anything else is UNVERIFIED until the user asks: Verify
 * (gh_mls_service_verify_member_async()) is one lookup, for that one
 * account, on the discovery relays and the person's kind-10002 write
 * relays only -- never the group's relays, which could tie the lookup to
 * the group -- on the normal transports (network mode, Tor, a fresh Tor
 * isolation per relay scope, ephemeral AUTH; kinds 30443 and 443, never
 * 10051). Nothing is looked up again by itself. The verdicts and who added
 * each device are kept in the encrypted store (gh-store-mls-identity.h) and
 * forgotten with the device or the group. Messaging never waits for any of
 * it. The settings key "only-join-verified-mls-groups" (default off;
 * gh_mls_requires_proofs()) brings the refusal back: invitations, Welcomes
 * and Commits with such a member fail (NEEDS_UPDATE), and an admin's Commit
 * refused for good is "change-refused" (gh_mls_group_get_refusal()), never
 * a wait (nostrc-prrl); it is kept across restarts, the read cursor stays
 * behind it, and turning the preference off applies it.
 *
 * Leaving (nostrc-2um6; MIP-03 "Leaving a group", Marmot
 * protocol-core/member-departure.md). Unless the account is an admin
 * (admins step down first), gh_mls_service_leave() leaves for everyone:
 * libmarmot makes the account's leave proposal -- a SelfRemove, which any
 * member commits, where the group's required_capabilities list it, else a
 * Remove request, which an admin commits (as MDK 0.8; review M1) --
 * which is published to the group relays (republished after a restart, and
 * made again for each new epoch), and the group is "leaving": it is still
 * read, nothing else is sent, and once another member commits the proposal
 * the group ends ("end" LEFT). If a new epoch's proposal cannot be made, or
 * a Remove request was re-made GH_MLS_SERVICE_LEAVE_REQUESTS times and an
 * admin's Commit kept the account each time (re-review R1), the leave is
 * dropped and the group says so ("leave-failed"). Otherwise
 * (gh_mls_service_leave_kind()) the
 * group ends on this device only ("end" LEFT_DEVICE): marmot_leave_group()
 * marks it inactive locally and the service stops reading it; the others
 * keep counting the account until an admin removes it. Leaving again while
 * "leaving" gives up waiting and leaves on this device.
 *
 * Others leaving. A member's own departure request -- its SelfRemove, or
 * the Remove of itself MDK 0.8 sends where SelfRemove is not required -- is
 * kept by libmarmot (marmot_process_message(): MARMOT_RESULT_PROPOSAL), and
 * the service commits it after a random delay
 * (GH_MLS_SERVICE_DEPARTURE_JITTER_*, growing with the group; member-departure.md: any
 * remaining member may commit a SelfRemove, as MDK 0.8 and 0.11 do; a Remove
 * needs an admin), through the same publish-then-merge lifecycle as every
 * Commit; another member's Commit consuming it first cancels ours. When a
 * Commit takes such a member out of the group -- whether or not this
 * account saw the proposal: libmarmot names them in the Commit's result --
 * the group emits "member-left". A Commit that arrives before the proposal
 * it cites is held and offered again when a proposal arrives (review H1);
 * meanwhile no departure Commit of ours is scheduled.
 *
 * Removal (nostrc-xrya). A member an admin removed cannot enter the next
 * epoch (the Commit's UpdatePath is encrypted to the others). libmarmot
 * recognises the authenticated admin Commit that removes the account's leaf
 * and turns the group inactive (marmot_get_group_removal() names who), and
 * the service shows it: "end" REMOVED with "removed-by"; nothing is held,
 * sends are refused and "unreadable" is 0; its room and history stay. The
 * removal is judged by the Commit ordering, not by arrival (W22 review B1):
 * while another admin could still publish a winning Commit of that epoch,
 * the group's subscription stays open and libmarmot judges that epoch's
 * Commits (nothing else is read; the cursor does not move); a winner
 * re-activates the group ("end" NONE) and it is read again from the
 * cursor. The removal turns final when nobody can beat it, or once the
 * group has visibly moved on without the account (MARMOT_REMOVAL_FINAL_AFTER
 * later-epoch events, review B2); a final removal closes the subscription,
 * for good. A removal record libmarmot cannot read
 * ends the group as UNKNOWN, never as LEFT (review N3).
 *
 * Generation. The service runs only while its store's account is the active
 * account: a switch cancels every subscription, lookup, signer request and
 * publish at once; nothing of the old generation is recorded afterwards.
 * Transports are the defaults (gh_relay_scope_new(), gh_relay_publish_new()),
 * so the network session's dispatcher (G09) routes everything, Tor included.
 * The store, model, relays, resolver and inbox are borrowed: dispose the
 * service before closing the store.
 */

/* libmarmot >= 0.10.0 binds every member leaf to its account with a proof
 * the account signs (nostrc-7vyi): the service enrolls it (see "Account
 * proof" above) before any KeyPackage or group. Older libmarmot has no
 * proof: the identity state is NOT_REQUIRED. */
#if defined(MARMOT_VERSION_MAJOR) && \
    (MARMOT_VERSION_MAJOR > 0 || MARMOT_VERSION_MINOR >= 10)
#define GH_MLS_SERVICE_ACCOUNT_PROOF 1
#else
#define GH_MLS_SERVICE_ACCOUNT_PROOF 0
#endif

/* The adopted-profile KeyPackage producer (nostrc-0bdg): on with libmarmot's
 * producer (the CMake option MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER sets both; on
 * by default since nostrc-lf62). Same relays and lifecycle as the MDK 0.8
 * one, its own `d` slot. Without it the account publishes MDK 0.8
 * KeyPackages only, and only MDK 0.8-format groups can add it. */
#ifndef GH_MLS_ADOPTED_KEY_PACKAGES
#define GH_MLS_ADOPTED_KEY_PACKAGES 0
#endif

/* Default KeyPackage rotation age (28 days). */
#define GH_MLS_KEY_PACKAGE_LIFETIME ((gint64)28 * 24 * 3600)
/* A due KeyPackage replacement waits for pending invitations (see "KeyPackage
 * lifecycle" above) at most this long, and never later than the margin
 * before the current KeyPackage's Lifetime ends; it is looked at again at
 * least this often. */
#define GH_MLS_KEY_PACKAGE_MAX_HOLD ((gint64)7 * 24 * 3600)
#define GH_MLS_KEY_PACKAGE_EXPIRY_MARGIN ((gint64)24 * 3600)
#define GH_MLS_KEY_PACKAGE_HOLD_RECHECK_S 600
/* Kind-445 events of a later epoch held for a Commit, per group. */
#define GH_MLS_SERVICE_MAX_HELD 256
/* Overlap subtracted from a group's read cursor (seconds). */
#define GH_MLS_SERVICE_CURSOR_OVERLAP 600
/* An earlier routing address of an adopted group (nostrc-ms4d) is read at
 * most this long after the Commit that left it, and only while the group is
 * not yet two epochs past it (libmarmot reads nothing older). */
#define GH_MLS_SERVICE_ROUTING_RETAIN_S ((gint64)7 * 24 * 3600)
/* A relay read only for an earlier address that fails this many times in a
 * row (no EOSE between) is read no more (nostrc-ms4d review L2). */
#define GH_MLS_SERVICE_EARLIER_RELAY_FAILURES 3
/* The one-time SelfRemove requirement of an existing group this device
 * created (nostrc-8ndz): a random delay in [MIN, MAX] once the group has
 * caught up, and at least STAGGER after the previous group's, so a first
 * launch never commits in many groups at once (review L1). */
#define GH_MLS_SERVICE_UPGRADE_DELAY_MIN_S (10 * 60)
#define GH_MLS_SERVICE_UPGRADE_DELAY_MAX_S (6 * 3600)
#define GH_MLS_SERVICE_UPGRADE_STAGGER_S (30 * 60)
/* A group relay's backfill: REQ limit, and older pages per subscription. */
#define GH_MLS_SERVICE_PAGE_LIMIT 500
#define GH_MLS_SERVICE_MAX_PAGES 64
/* Backfill kept per group until it is applied (review B4): what an honest
 * relay's paging can deliver in one round, and 64 MiB. Past either, the
 * relays still delivering stop paging and count as incomplete. */
#define GH_MLS_SERVICE_MAX_BACKFILL_EVENTS ((GH_MLS_SERVICE_MAX_PAGES + 1) * GH_MLS_SERVICE_PAGE_LIMIT)
#define GH_MLS_SERVICE_MAX_BACKFILL_BYTES ((gsize)64 * 1024 * 1024)
/* Relays that delivered part of a backfill round and have all been silent
 * this long stop holding the others' backfill (final review N1): given up
 * as incomplete, their subscriptions left open. Seconds. */
#define GH_MLS_SERVICE_BACKFILL_QUIET_S 30
/* A held event keeps decrypt-pending up this long at most (seconds): junk
 * anyone posts with the group's h would otherwise show it for good in a
 * quiet group (W22 review N4). It stays held and is retried. */
#define GH_MLS_SERVICE_PENDING_SHOWN_S (15 * 60)
/* A held event still unreadable after this many applied Commits is junk. */
#define GH_MLS_SERVICE_JUNK_AFTER_COMMITS 3
/* People invited at once (one Add Commit). */
#define GH_MLS_SERVICE_MAX_INVITEES 32
/* The random delay before committing another member's leave (milliseconds):
 * member-departure.md "short randomized jitter", so that members online
 * together rarely race. The window is MIN_MS..MIN_MS + PER_MEMBER_MS per
 * member, at most MAX_MS (review L5). Metadata: relays cannot read the
 * events, but they see a proposal followed by a burst of Commits from
 * fresh keys, which marks a departure and hints at how many members were
 * online; a wider window shortens the burst, it does not hide it. */
#define GH_MLS_SERVICE_DEPARTURE_JITTER_MIN_MS 1000
#define GH_MLS_SERVICE_DEPARTURE_JITTER_PER_MEMBER_MS 250
#define GH_MLS_SERVICE_DEPARTURE_JITTER_MAX_MS 15000
/* A held Commit that cites a proposal not received yet (review H1) is
 * retried whenever a proposal or Commit arrives, at most this many times
 * and for this long (seconds); then it is junk. */
/* A Remove request is made at most this many times per leave: each admin
 * Commit that keeps the account uses one (re-review R1: MDK 0.8's admin
 * auto-commit drops the request and commits nothing). Then the leave stops
 * with GH_MLS_LEAVE_FAILURE_NOT_PROCESSED. */
#define GH_MLS_SERVICE_LEAVE_REQUESTS 2
#define GH_MLS_SERVICE_PROPOSAL_WAIT_TRIES 16
#define GH_MLS_SERVICE_PROPOSAL_WAIT_S 600

typedef enum {
  GH_MLS_KEY_PACKAGE_NONE,        /* not published (inactive, offline or not yet) */
  GH_MLS_KEY_PACKAGE_NO_RELAYS,   /* the account's kind 10002 lists no write-capable relay
                                   * (or none is known): nobody can invite it */
  GH_MLS_KEY_PACKAGE_PUBLISHING,  /* made, being signed or published */
  GH_MLS_KEY_PACKAGE_PUBLISHED,   /* a relay accepted the current one */
  GH_MLS_KEY_PACKAGE_FAILED       /* the signer declined, or no relay accepted it */
} GhMlsKeyPackageState;

/* The KeyPackage formats (nostrc-lf62): an account publishes one KeyPackage
 * of each it produces, and a group can add an invitee only through the
 * format of its own profile. */
typedef enum {
  GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED, /* the adopted Marmot profile (MDK 0.11, White Noise) */
  GH_MLS_KEY_PACKAGE_FORMAT_LEGACY,  /* the MDK 0.8 profile ("the older format") */
  GH_MLS_KEY_PACKAGE_N_FORMATS
} GhMlsKeyPackageFormat;

GType gh_mls_key_package_state_get_type(void);
#define GH_TYPE_MLS_KEY_PACKAGE_STATE (gh_mls_key_package_state_get_type())

typedef enum {
  GH_MLS_READ_IDLE,         /* not read (inactive, offline or left) */
  GH_MLS_READ_SYNCING,      /* subscribed, history until EOSE */
  GH_MLS_READ_LIVE,         /* every group relay sent EOSE */
  GH_MLS_READ_DISCONNECTED  /* every group relay dropped or refused; it reconnects */
} GhMlsReadState;

GType gh_mls_read_state_get_type(void);
#define GH_TYPE_MLS_READ_STATE (gh_mls_read_state_get_type())

/* Why a group is no longer active for the account (nostrc-xrya). */
typedef enum {
  GH_MLS_GROUP_END_NONE,     /* active */
  GH_MLS_GROUP_END_LEFT,     /* the account left: a member committed its SelfRemove */
  GH_MLS_GROUP_END_REMOVED,  /* an admin's Commit removed the account's leaf */
  GH_MLS_GROUP_END_UNKNOWN,  /* inactive, and why can't be read (a damaged record) */
  GH_MLS_GROUP_END_LEFT_DEVICE /* the account left on this device only; the others still
                                * count it (gh_mls_service_leave() without SelfRemove) */
} GhMlsGroupEnd;

/* Why a leave was dropped (gh_mls_group_get_leave_failure()). */
typedef enum {
  GH_MLS_LEAVE_FAILURE_NONE,
  GH_MLS_LEAVE_FAILURE_CANNOT_CONTINUE, /* a new epoch's request could not be made */
  GH_MLS_LEAVE_FAILURE_NOT_PROCESSED    /* admins' Commits came and kept the account (R1) */
} GhMlsLeaveFailure;

/* What gh_mls_service_leave() would do for a group (nostrc-2um6). */
typedef enum {
  GH_MLS_LEAVE_EVERYONE,           /* SelfRemove: the others are told; any of them commits it */
  GH_MLS_LEAVE_ADMINS,             /* a Remove request: an admin commits it (review M1) */
  GH_MLS_LEAVE_DEVICE_ADMIN,       /* this device only: admins step down first */
  GH_MLS_LEAVE_DEVICE_UNSUPPORTED, /* this device only: someone's app can't process a leave */
  GH_MLS_LEAVE_DEVICE_WAITING,     /* this device only: the leave still waits for a member */
  GH_MLS_LEAVE_DEVICE              /* this device only (busy, offline, or an error) */
} GhMlsLeave;

GType gh_mls_group_end_get_type(void);
#define GH_TYPE_MLS_GROUP_END (gh_mls_group_end_get_type())

#define GH_MLS_SERVICE_ERROR (gh_mls_service_error_quark())
GQuark gh_mls_service_error_quark(void);
typedef enum {
  GH_MLS_SERVICE_ERROR_NO_CONSENT = 1, /* a message request, or not a contact (PT-8) */
  GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE, /* the person hasn't set up encrypted groups */
  GH_MLS_SERVICE_ERROR_NOT_ADMIN,      /* the change needs a group admin */
  GH_MLS_SERVICE_ERROR_BUSY,           /* another change of the group is pending */
  GH_MLS_SERVICE_ERROR_REFUSED,        /* every group relay refused the Commit */
  GH_MLS_SERVICE_ERROR_SUPERSEDED,     /* another member's change won the epoch */
  GH_MLS_SERVICE_ERROR_NO_RELAYS,      /* no group relay (or none usable) */
  GH_MLS_SERVICE_ERROR_INACTIVE,       /* the store's account is not the active one */
  GH_MLS_SERVICE_ERROR_NOT_ENROLLED,   /* the signer has not approved this device's proof */
  GH_MLS_SERVICE_ERROR_NEEDS_UPDATE,   /* someone's app cannot prove their account, and the
                                        * account requires proofs (nostrc-6ukh) */
  GH_MLS_SERVICE_ERROR_FORGED_IDENTITY, /* someone's account proof does not verify */
  GH_MLS_SERVICE_ERROR_MIXED_PROFILE, /* invitees with only the adopted and only the MDK 0.8
                                       * KeyPackage format cannot share a new group */
  GH_MLS_SERVICE_ERROR_PROFILE_MISMATCH, /* an invitee has no KeyPackage in the group's format */
  GH_MLS_SERVICE_ERROR_FORMAT_CHANGED, /* an invitee's KeyPackages no longer give the format the
                                       * user was shown (review M2): check again */
  GH_MLS_SERVICE_ERROR_ADDRESS_TAKEN,  /* an invitation names the address (h tag) of another
                                        * group of ours; refused for good (nostrc-scki) */
  GH_MLS_SERVICE_ERROR_INVITEE_UNSUPPORTED, /* an invitee's app cannot join what the group
                                            * requires (SelfRemove; nostrc-zbmb) */
  GH_MLS_SERVICE_ERROR_EPOCH_CHANGED,  /* the group moved on since the files were sealed:
                                        * seal and upload them again (W25) */
  GH_MLS_SERVICE_ERROR_UNSUPPORTED     /* this group's kind can't do that (a legacy group's
                                        * picture, W25) */
} GhMlsServiceError;

/* Whether settings asks for every member's account proof: the key
 * "only-join-verified-mls-groups", FALSE when settings is NULL or has no
 * such key (one rule for the service and the UI, W24 review N1). */
gboolean gh_mls_requires_proofs(GSettings *settings);

typedef enum {
  GH_MLS_IDENTITY_NOT_REQUIRED, /* libmarmot < 0.10.0: no account proof */
  GH_MLS_IDENTITY_NONE,         /* not asked yet (inactive, offline) */
  GH_MLS_IDENTITY_WAITING,      /* the signer is asking the user */
  GH_MLS_IDENTITY_ENROLLED,     /* the proof is set for this start */
  GH_MLS_IDENTITY_DECLINED,     /* the user declined; asked again at the next start */
  GH_MLS_IDENTITY_FAILED        /* the signer failed or returned something else */
} GhMlsIdentityState;

GType gh_mls_identity_state_get_type(void);
#define GH_TYPE_MLS_IDENTITY_STATE (gh_mls_identity_state_get_type())

/* What Groundhog knows about who a member is (nostrc-6ukh). An account with
 * several devices is as weak as its weakest one. */
typedef enum {
  GH_MLS_MEMBER_PROVEN,     /* the leaf carries the account's own proof */
  GH_MLS_MEMBER_VERIFIED,   /* no proof, but a KeyPackage the account signed matches
                             * (or matched the key this device renewed) */
  GH_MLS_MEMBER_CHECKING,   /* no proof; a Verify the user asked for runs */
  GH_MLS_MEMBER_UNVERIFIED  /* no proof, and no evidence in hand */
} GhMlsMemberIdentity;

/* Why a group's change was refused for good ("change-refused"). */
typedef enum {
  GH_MLS_REFUSAL_NONE,
  GH_MLS_REFUSAL_BROKEN_PROOF,  /* default mode: a proof that does not verify, or one a
                                 * member's own new leaf dropped */
  GH_MLS_REFUSAL_UNPROVEN,      /* proofs required: a member without one, or broken */
  GH_MLS_REFUSAL_UNFOLLOWABLE   /* an admin's change libmarmot cannot follow, or that breaks
                                 * the group's rules (adopted groups: MARMOT_ERR_COMMIT_REFUSED,
                                 * W24b slice H review L2) */
} GhMlsRefusal;

GType gh_mls_refusal_get_type(void);
#define GH_TYPE_MLS_REFUSAL (gh_mls_refusal_get_type())

GType gh_mls_member_identity_get_type(void);
#define GH_TYPE_MLS_MEMBER_IDENTITY (gh_mls_member_identity_get_type())

#define GH_TYPE_MLS_GROUP (gh_mls_group_get_type())
G_DECLARE_FINAL_TYPE(GhMlsGroup, gh_mls_group, GH, MLS_GROUP, GObject)

/* One joined (or left) group of the account. Read-only properties, notified
 * on change: "group-id" (hex MLS group id), "room-id", "name",
 * "description", "epoch", "active" (FALSE once left or removed), "end" (why
 * not: GhMlsGroupEnd), "removed-by" (hex: the admin whose Commit removed the
 * account, or NULL), "read-state",
 * "is-admin" (the account is a GroupData admin), "pending-commit" (a change
 * of the account's is published but not merged yet) and
 * "unsent-welcomes" (Welcomes of merged Adds not yet accepted by an
 * invitee's inbox relay) and "unreadable" (kind-445 events held this
 * session because they cannot be decrypted yet -- application messages
 * and Commits alike, whose type is sealed until their epoch opens, and
 * junk anyone posted with the group's h; 0 once the group ended; for
 * diagnostics and tests) and "decrypt-pending" (what the UI shows, charter
 * §7.15 state 13, nostrc-oya4: the active group holds an event it cannot
 * read yet -- not one dated in the join's own second from a stored answer,
 * where the joiner's own Add Commit lands, not one dropped as junk before,
 * and none held longer than GH_MLS_SERVICE_PENDING_SHOWN_S) and
 * "history-incomplete" (a group relay's backfill could
 * not be fetched completely this subscription: paging failed or ran out, or
 * more was delivered than the service keeps at once; the read cursor holds,
 * and the next subscription asks again), "leaving" (the account's
 * SelfRemove waits for a member's Commit), "unverified-members" (members
 * CHECKING or UNVERIFIED, nostrc-6ukh) and "change-refused" (an admin's
 * Commit was refused for good: it added someone whose identity can't be
 * verified while the account requires proofs, or whose proof is forged;
 * the group can't be read past it, and nothing is "waiting": decrypt-pending
 * stays FALSE; a later Commit that applies clears it; nostrc-prrl). Signal
 * "members-changed": the member list, the admins or a member's identity
 * may differ. Signal "member-left" (gchar *pubkey, hex): a Commit took out
 * a member who had asked to leave (nostrc-2um6); "leave-failed"
 * (gh_mls_group_get_leave_failed()). */
const gchar *gh_mls_group_get_group_id(GhMlsGroup *self);
const gchar *gh_mls_group_get_room_id(GhMlsGroup *self);
const gchar *gh_mls_group_get_name(GhMlsGroup *self);
const gchar *gh_mls_group_get_description(GhMlsGroup *self);
guint64 gh_mls_group_get_epoch(GhMlsGroup *self);
gboolean gh_mls_group_get_active(GhMlsGroup *self);
/* TRUE while the account's leave waits for a member's Commit (nostrc-2um6). */
gboolean gh_mls_group_get_leaving(GhMlsGroup *self);
/* While leaving: TRUE when an admin must commit it (a Remove request). */
gboolean gh_mls_group_get_leave_via_admin(GhMlsGroup *self);
/* The leave could not go on (a new epoch's proposal could not be made) and
 * was dropped: the account is still a member and may send (review L3).
 * Cleared by the next gh_mls_service_leave(). */
gboolean gh_mls_group_get_leave_failed(GhMlsGroup *self);
/* Why: a new epoch's request could not be made, or (re-review R1) an admin
 * moved the group on twice without acting on the Remove request. */
GhMlsLeaveFailure gh_mls_group_get_leave_failure(GhMlsGroup *self);
GhMlsGroupEnd gh_mls_group_get_end(GhMlsGroup *self);
const gchar *gh_mls_group_get_removed_by(GhMlsGroup *self);
GhMlsReadState gh_mls_group_get_read_state(GhMlsGroup *self);
gboolean gh_mls_group_get_is_admin(GhMlsGroup *self);
/* Whether the group is adopted-profile (MDK 0.11, White Noise), else MDK
 * 0.8-profile ("the older format"); fixed for the group's life. Only the
 * matching KeyPackage format can be added (nostrc-lf62). */
gboolean gh_mls_group_get_adopted(GhMlsGroup *self);
gboolean gh_mls_group_get_pending_commit(GhMlsGroup *self);
guint gh_mls_group_get_unsent_welcomes(GhMlsGroup *self);
guint gh_mls_group_get_unreadable(GhMlsGroup *self);
gboolean gh_mls_group_get_decrypt_pending(GhMlsGroup *self);
/* When the account joined (or made) the group, unix seconds; 0: unknown.
 * For diagnostics and tests. */
gint64 gh_mls_group_get_join_time(GhMlsGroup *self);
gboolean gh_mls_group_get_history_incomplete(GhMlsGroup *self);
/* The read cursor (unix seconds; 0: none): the group's next REQ asks from
 * it minus GH_MLS_SERVICE_CURSOR_OVERLAP. For diagnostics and tests. */
gint64 gh_mls_group_get_cursor(GhMlsGroup *self);
/* The members' account keys (lowercase hex, sorted; the account included):
 * what every member can see (charter §2.2). Transfer full. */
GStrv gh_mls_group_dup_members(GhMlsGroup *self);
/* What is known about member (hex): see GhMlsMemberIdentity; PROVEN for
 * one that is not a member, UNVERIFIED for a listed member whose devices
 * could not be read (W24 review N2). out_added_by (nullable, transfer
 * full): who added its weakest device, hex (the member itself when it
 * added that device), or NULL when not known (it was there before the
 * account joined). */
GhMlsMemberIdentity gh_mls_group_get_member_identity(GhMlsGroup *self, const gchar *member,
                                                     gchar **out_added_by);
guint gh_mls_group_get_unverified_members(GhMlsGroup *self);
gboolean gh_mls_group_get_change_refused(GhMlsGroup *self);
GhMlsRefusal gh_mls_group_get_refusal(GhMlsGroup *self);
/* The GroupData admins (lowercase hex, sorted). Transfer full. */
GStrv gh_mls_group_dup_admins(GhMlsGroup *self);
/* The group relays (sorted). Transfer full. */
GStrv gh_mls_group_dup_relays(GhMlsGroup *self);
/* Every relay the group is read at: its relays, and those of earlier routing
 * addresses still followed for late events (nostrc-ms4d); sorted. */
GStrv gh_mls_group_dup_read_relays(GhMlsGroup *self);

/* A pending invitation: a Welcome received and stored, not yet accepted. */
typedef struct {
  gchar *wrapper_id;   /* the gift wrap's id (hex): what accept/decline name */
  gchar *inviter;      /* hex: the seal's signer, who invited the account */
  gchar *group_name;   /* from the Welcome (the inviter's claim until joined) */
  guint member_count;  /* at invite time */
  GStrv relays;        /* the group relays the Welcome names */
} GhMlsInvite;

void gh_mls_invite_free(GhMlsInvite *invite);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsInvite, gh_mls_invite_free)

/* Whether the account may look pubkey (hex) up to invite it (PT-8). */
typedef gboolean (*GhMlsConsentFunc)(const gchar *pubkey, gpointer data);

typedef struct {
  GhStore *store;                      /* the account's open store; borrowed */
  GhAccountController *accounts;       /* signer, generation */
  GhConversationStore *conversations;  /* bound to the store's account; borrowed */
  GhAccountRelays *account_relays;     /* own 10002 write + 10050: KeyPackage publish */
  GhInboxResolver *inboxes;            /* invitees' 10050: Welcome delivery */
  GSettings *settings;                 /* discovery-relays: KeyPackage lookups;
                                        * only-join-verified-mls-groups */
  GhDmInbox *inbox;                    /* nullable: where Welcomes arrive (its sink) */
  /* NULL: an accepted peer of a non-request NIP-17 room of conversations. */
  GhMlsConsentFunc may_look_up;
  gpointer consent_data;
  GNetworkMonitor *network;            /* NULL: g_network_monitor_get_default() */
  guint publish_deadline;              /* per-relay seconds; 0: the publish default */
  guint lookup_deadline;               /* per-phase seconds; 0: 15 */
  gint64 key_package_lifetime;         /* seconds; 0: GH_MLS_KEY_PACKAGE_LIFETIME */
  gint64 key_package_max_hold;         /* seconds; 0: GH_MLS_KEY_PACKAGE_MAX_HOLD */
  /* Publish MDK 0.8 KeyPackages only, as a build without the adopted producer
   * does (nostrc-lf62): for tests (Groundhog accounts then make MDK 0.8
   * groups with each other) and diagnostics. */
  gboolean legacy_key_packages_only;
} GhMlsServiceConfig;

#define GH_TYPE_MLS_SERVICE (gh_mls_service_get_type())
G_DECLARE_FINAL_TYPE(GhMlsService, gh_mls_service, GH, MLS_SERVICE, GObject)

/* Opens libmarmot on the store, lists the stored rooms and joined groups,
 * resumes pending Commits, Welcomes and sends, and starts once the account
 * is active and online. The service is a GListModel of GhMlsGroup, oldest
 * first. Signals: "invite-received" (gchar *wrapper_id), "group-added"
 * (GhMlsGroup). */
GhMlsService *gh_mls_service_new(const GhMlsServiceConfig *config, GError **error);
#ifdef GH_MLS_TEST_HOOKS
/* Test hook (nostrc-0bdg re-review A3): the received-invitation list cannot
 * be read (as with a storage error) while fail is TRUE. */
void gh_mls_service_test_fail_invitation_listing(gboolean fail);
/* Test hook (W25 slice K re-review L3): reading the MDK 0.8 KeyPackage's
 * slot for its withdrawal fails with a storage error (not "not found")
 * while fail is TRUE; how many times it did since the last call. */
void gh_mls_service_test_fail_key_package_slot(gboolean fail);
guint gh_mls_service_test_key_package_slot_failures(void);
/* Test hook (nostrc-0bdg): whether libmarmot still holds the private init
 * key of the KeyPackage whose ref (`i` tag) is @ref_hex. */
gboolean gh_mls_service_test_has_init_key(GhMlsService *self, const gchar *ref_hex);
#if GH_MLS_ADOPTED_KEY_PACKAGES
/* Test hook (nostrc-0bdg): replaces the one libmarmot call that its own
 * build gate (CMake MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER) refuses when it is
 * off, e.g. with libmarmot's ungated internal producer; the rest of the
 * adopted producer path is the service's own. */
typedef MarmotError (*GhMlsTestAdoptedProducer)(Marmot *marmot, const guint8 account[32],
                                                MarmotKeyPackageResult *made);
void gh_mls_service_test_set_adopted_producer(GhMlsTestAdoptedProducer producer);
#endif
/* Test hook, compiled only into test executables: how many times a group
 * change was staged again because libmarmot refused its Commit with
 * MARMOT_ERR_EVENT_RATE (review W24 N5). */
guint gh_mls_service_test_rate_retries(void);
/* Test hooks (nostrc-2um6): the next `n` group changes staged are refused
 * with MARMOT_ERR_EVENT_RATE as libmarmot refuses them, nothing staged; and
 * how many Commits of members' leaves failed. */
void gh_mls_service_test_refuse_rate(guint n);
guint gh_mls_service_test_departure_failures(void);
/* Test hooks (nostrc-8ndz). permissive TRUE: services made from now on keep
 * a group created alone permissive at its first Add (libmarmot
 * MarmotConfig.keep_first_add_permissive) and make no background SelfRemove
 * upgrade -- the shape of groups made before 0.12, or with a first invitee
 * whose app lacks SelfRemove (default FALSE, as the app). And the background
 * upgrade's delay window and stagger in milliseconds (all 0: the defaults
 * above). */
void gh_mls_service_test_set_permissive_groups(gboolean permissive);
void gh_mls_service_test_set_upgrade_window(guint min_ms, guint max_ms, guint stagger_ms);
/* Test hook (W25): an admin's Commit of the adopted group's 0x800b media
 * policy, blossom-v1 with `endpoints` as its default blob endpoints
 * (marmot_update_group_media_policy()), as an MDK admin would set it.
 * Finish with gh_mls_service_change_finish(). */
void gh_mls_service_test_set_media_policy_async(GhMlsService *self, GhMlsGroup *group,
                                                const gchar *const *endpoints,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
#endif
const gchar *gh_mls_service_get_account(GhMlsService *self);
/* libmarmot, for tests and diagnostics (borrowed; one thread). */
Marmot *gh_mls_service_get_marmot(GhMlsService *self);

/* Every format the account publishes, together: PUBLISHED once each one
 * is; PUBLISHING or FAILED while one is. */
GhMlsKeyPackageState gh_mls_service_get_key_package_state(GhMlsService *self);
/* The account-proof enrollment ("identity-state", notified). */
GhMlsIdentityState gh_mls_service_get_identity_state(GhMlsService *self);
/* The user asks again after a decline or failure ("Try Again"): a
 * declined request is otherwise asked again only when the account's
 * generation changes, never on a network reconnect. FALSE with
 * GH_MLS_SERVICE_ERROR_INACTIVE when the service is not running. */
gboolean gh_mls_service_retry_identity(GhMlsService *self, GError **error);
/* The most backfill kept per group before it is applied (0: the default,
 * GH_MLS_SERVICE_MAX_BACKFILL_EVENTS / _BYTES); from the next event on.
 * For tests and tuning. */
void gh_mls_service_set_backfill_limit(GhMlsService *self, guint max_events, gsize max_bytes);
/* The quiet period after which silent relays stop holding the backfill
 * (milliseconds; 0: GH_MLS_SERVICE_BACKFILL_QUIET_S). For tests and tuning. */
void gh_mls_service_set_backfill_quiet(GhMlsService *self, guint quiet_ms);
/* How long a held event keeps "decrypt-pending" up (milliseconds; 0:
 * GH_MLS_SERVICE_PENDING_SHOWN_S). For tests and tuning. */
void gh_mls_service_set_pending_shown(GhMlsService *self, guint shown_ms);
/* The id of the KeyPackage event last accepted by a relay, or NULL: of the
 * adopted format when the account publishes it, else of the MDK 0.8 one. */
const gchar *gh_mls_service_get_key_package_id(GhMlsService *self);
/* The same for one format; NULL for a format the account does not publish. */
const gchar *gh_mls_service_get_key_package_id_for_format(GhMlsService *self,
                                                          GhMlsKeyPackageFormat format);
/* Asks for a new KeyPackage of every format (e.g. the user asked); FALSE with
 * GH_MLS_SERVICE_ERROR_INACTIVE when the service is not running. Published
 * now, or -- while an invitation is pending -- once none is or the hold ends
 * (gh_mls_service_get_key_package_held()). */
gboolean gh_mls_service_rotate_key_package(GhMlsService *self, GError **error);
/* Whether a due KeyPackage replacement is held back because an invitation
 * is pending (the current KeyPackage stays published meanwhile); the
 * "key-package-held" property, with notify. */
gboolean gh_mls_service_get_key_package_held(GhMlsService *self);

/* The group of a hex MLS group id or of a room id, or NULL. Transfer none. */
GhMlsGroup *gh_mls_service_lookup(GhMlsService *self, const gchar *group_id_or_room_id);

/* Creates a group named name (description nullable) on relays (1 to 16
 * ws(s) URLs) and invites invitees (1 to GH_MLS_SERVICE_MAX_INVITEES hex
 * pubkeys): consent, KeyPackage lookups, the group, one Add Commit
 * (published, merged), then the Welcomes. Completes once the Add was merged
 * (the group, with the Welcomes on their way) or failed. The group is
 * adopted-profile when every invitee has an adopted KeyPackage, and
 * MDK 0.8-profile when someone has only an MDK 0.8 one (nostrc-lf62);
 * invitees who have only one format each, different ones, cannot share a
 * group: GH_MLS_SERVICE_ERROR_MIXED_PROFILE, nothing made. */
void gh_mls_service_create_group_async(GhMlsService *self, const gchar *name,
                                       const gchar *description,
                                       const gchar *const *relays,
                                       const gchar *const *invitees,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback, gpointer user_data);
/* The same, in the group format the user was shown (New Group, review M2):
 * when the lookups at creation no longer give every invitee a KeyPackage of
 * `format`, nothing is made and the task fails with
 * GH_MLS_SERVICE_ERROR_FORMAT_CHANGED, never with a group of the other
 * format. Finish with gh_mls_service_create_group_finish(). */
void gh_mls_service_create_group_in_format_async(GhMlsService *self, const gchar *name,
                                                 const gchar *description,
                                                 const gchar *const *relays,
                                                 const gchar *const *invitees,
                                                 GhMlsKeyPackageFormat format,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data);
GhMlsGroup *gh_mls_service_create_group_finish(GhMlsService *self, GAsyncResult *result,
                                               GError **error);

/* Adds, removes, renames: one Commit each, completing once it was merged
 * (TRUE) or failed. Only admins (GH_MLS_SERVICE_ERROR_NOT_ADMIN). A Commit
 * still unanswered by every relay when the round ends stays pending and is
 * republished (it completes then). */
void gh_mls_service_add_members_async(GhMlsService *self, GhMlsGroup *group,
                                      const gchar *const *invitees,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data);
void gh_mls_service_remove_members_async(GhMlsService *self, GhMlsGroup *group,
                                         const gchar *const *members,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data);
/* name and description: NULL keeps each. */
void gh_mls_service_update_metadata_async(GhMlsService *self, GhMlsGroup *group,
                                          const gchar *name, const gchar *description,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);
/* Replaces the GroupData admins (charter §7.10 owner/admin) with admins:
 * 1 to 1000 hex pubkeys of current members. */
void gh_mls_service_set_admins_async(GhMlsService *self, GhMlsGroup *group,
                                     const gchar *const *admins, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_mls_service_change_finish(GhMlsService *self, GAsyncResult *result,
                                      GError **error);

/* Whether group is an adopted-profile group (MDK 0.11, White Noise), whose
 * settings are app components; FALSE for a legacy (MIP-01) group. */
gboolean gh_mls_service_get_adopted(GhMlsService *self, GhMlsGroup *group);
/* An adopted group's components as its current epoch says
 * (marmot_get_group_components(): name, the 0x8002 picture, the 0x8007
 * avatar URL and which one shows, the 0x800b media policy; out owned, free
 * with marmot_group_components_clear()). GH_MLS_SERVICE_ERROR_UNSUPPORTED
 * for a legacy group. The picture's image_key and upload key are secrets:
 * use them, never keep them. Reads nothing from the network. */
gboolean gh_mls_service_get_components(GhMlsService *self, GhMlsGroup *group,
                                       MarmotGroupComponents *out, GError **error);
/* The group picture (W25, nostrc-m6tp): image is the 0x8002 state of a
 * picture already encrypted (marmot_group_image_encrypt()) and uploaded
 * (copied, wiped after); NULL or !present removes the picture. One admin
 * Commit, published then merged, as update_metadata (finish:
 * gh_mls_service_change_finish()); GH_MLS_SERVICE_ERROR_UNSUPPORTED for a
 * legacy group, which libmarmot gives no picture. */
void gh_mls_service_set_image_async(GhMlsService *self, GhMlsGroup *group,
                                    const MarmotGroupBlossomImage *image,
                                    GCancellable *cancellable, GAsyncReadyCallback callback,
                                    gpointer user_data);
/* Removes the group's 0x8007 web-address picture (an admin, adopted groups;
 * Groundhog never sets one: it never loads web pictures). */
void gh_mls_service_clear_avatar_url_async(GhMlsService *self, GhMlsGroup *group,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data);

/* Verify (W24 review H1): the user asked to check member (hex), a member of
 * group without the account proof. One KeyPackage lookup for that account
 * on the discovery relays and its kind-10002 write relays (never the
 * group's relays); a KeyPackage it signed that matches a device of the
 * member makes it VERIFIED, and the verdict is kept. finish: the member's
 * identity afterwards (UNVERIFIED: relays answered, nothing matched), or
 * FALSE-ish with error: G_IO_ERROR_HOST_UNREACHABLE (no relay answered;
 * nothing recorded), G_IO_ERROR_INVALID_ARGUMENT (not such a member, no
 * discovery relay), G_IO_ERROR_PERMISSION_DENIED (every relay to ask is one
 * of the group's relays: asking would reveal the group; W24 review A1),
 * GH_MLS_SERVICE_ERROR_INACTIVE. Group relays are never asked, in either
 * phase. */
void gh_mls_service_verify_member_async(GhMlsService *self, GhMlsGroup *group,
                                        const gchar *member, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data);
GhMlsMemberIdentity gh_mls_service_verify_member_finish(GhMlsService *self,
                                                        GAsyncResult *result, GError **error);

/* Leaves the group (see "Leaving" above): for everyone where it can, then
 * the group is "leaving" until a member commits it; otherwise on this device
 * only, at once (the group turns inactive and is not read any more; its room
 * and history stay). */
gboolean gh_mls_service_leave(GhMlsService *self, GhMlsGroup *group, GError **error);
/* What gh_mls_service_leave() would do now (for the confirmation copy). */
GhMlsLeave gh_mls_service_leave_kind(GhMlsService *self, GhMlsGroup *group);

/* Sends a chat message (kind 9 inner event) to an active group: stored and
 * ratcheted in one transaction, listed at once, then published. The listed
 * message (transfer full) carries its GhMessageStatus. */
GhMessage *gh_mls_service_send(GhMlsService *self, GhMlsGroup *group, const gchar *text,
                               GError **error);
/* As send(), with encrypted attachments (W25, nostrc-q3a6): imeta_tags
 * holds 1 to GH_MLS_IMETA_MAX_ATTACHMENTS ordered MIP-04 v2 imeta tags
 * (GStrv, "imeta" first; gh_mls_attachment_dup_imeta()), each sealed for
 * source_epoch, and caption (possibly empty) is the content, as MDK sends
 * them: one kind-9 inner event. In the send's store transaction and right
 * before marmot_create_message(), libmarmot checks that a message sent now
 * is in source_epoch (marmot_media_check_epoch(), reconciling an interrupted
 * transition first); if the group moved on, nothing is stored or sent and
 * the error is GH_MLS_SERVICE_ERROR_EPOCH_CHANGED: seal and upload again.
 * The message carries its attachments (gh_message_get_attachment()) and
 * epoch, and the store binds their cache identities to it. */
GhMessage *gh_mls_service_send_with_imeta(GhMlsService *self, GhMlsGroup *group,
                                          const gchar *caption, GPtrArray *imeta_tags,
                                          guint64 source_epoch, GError **error);

/* Pending invitations, oldest first (GhMlsInvite). Transfer full. */
GPtrArray *gh_mls_service_list_invites(GhMlsService *self, GError **error);
/* Joins the group of a pending invitation: its room is listed and read.
 * Transfer none. */
GhMlsGroup *gh_mls_service_accept_invite(GhMlsService *self, const gchar *wrapper_id,
                                         GError **error);
gboolean gh_mls_service_decline_invite(GhMlsService *self, const gchar *wrapper_id,
                                       GError **error);

G_END_DECLS
#endif
