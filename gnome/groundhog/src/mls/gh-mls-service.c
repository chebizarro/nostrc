#include "gh-mls-service.h"

#include "gh-auth-policy.h"
#include "gh-conversation-private.h"
#include "gh-identity.h"
#include "gh-message-status.h"
#include "gh-mls-commits.h"
#include "gh-mls-key-packages.h"
#include "gh-mls-imeta.h"
#include "gh-nip17-envelope.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"
#include "gh-store-marmot.h"
#include "gh-store-mls.h"
#include "gh-store-mls-identity.h"
#include "gh-reaction.h"
#include "gh-reaction-store.h"

#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(gh-mls-service-error-quark, gh_mls_service_error)

/* Store cursors (gh-store.h `cursors`): each KeyPackage format's last
 * publish time (key "" for the MDK 0.8 format, as before nostrc-lf62;
 * "adopted" for the adopted one) and each group's read cursor ("mls/" + a
 * hash of the group id: the scope is bounded and names no group). */
#define KEY_PACKAGE_CURSOR "mls/key-package"
/* When a due KeyPackage replacement was first held back for pending
 * invitations (0: not held); see key_package_hold(). */
#define KEY_PACKAGE_HELD_CURSOR "mls/key-package-held"
/* A new joiner reads back this far (its Welcome may arrive late). */
#define JOIN_BACKFILL ((gint64)2 * 24 * 3600)
/* Retry of unanswered Commits, Welcomes and sends: jittered, doubling. */
#define RETRY_MIN_S 15
#define RETRY_MAX_S 600
#define MAX_GROUP_RELAYS 16
/* A slot's accepted KeyPackage event ids remembered for a withdrawal. */
#define MAX_KEY_PACKAGE_EVENT_IDS 16
/* Earlier routing addresses read at once (libmarmot keeps 16 aliases). */
#define MAX_GROUP_ADDRESSES 16
/* The settings key that brings back the proof requirement (nostrc-6ukh). */
#define VERIFIED_ONLY_KEY "only-join-verified-mls-groups"
/* Whether the account also publishes the MDK 0.8 KeyPackage (nostrc-lf62
 * review L1; privacy charter amendment 2026-10-01). */
#define LEGACY_KEY_PACKAGES_KEY "mls-legacy-key-packages"
/* KeyPackages of the account's own Adds kept per group as evidence. */
#define MAX_OWN_EVIDENCE 64

/* GhRelayPublishOutcome values the store keeps (gh-mls-commits.c too). */
enum { OUTCOME_ACCEPTED = GH_RELAY_PUBLISH_ACCEPTED, OUTCOME_REJECTED = GH_RELAY_PUBLISH_REJECTED };

typedef struct _Round Round;
typedef struct _Delivery Delivery;

/* An earlier routing address of an adopted group (nostrc-ms4d): an h tag the
 * group left, at the relays it was read at then. A routing change leaves
 * the traffic of the epochs before it there (late messages, competing
 * Commits; nostr-routing-v1.md "Routing rotation"), which libmarmot still
 * routes to the group (an alias). Read until the group is two epochs past
 * it -- libmarmot then reads nothing of those epochs any more -- or
 * GH_MLS_SERVICE_ROUTING_RETAIN_S after it was left. */
typedef struct {
  gchar id[65];              /* the h tag, lowercase hex */
  GStrv relays;              /* sorted */
  guint64 epoch;             /* the epoch the Commit that left it entered */
  gint64 retired_at;         /* when that Commit was applied (unix seconds) */
  gint64 since;              /* the REQ's since for it (0: none) */
} Address;

static void
address_free(gpointer data)
{
  Address *address = data;
  g_strfreev(address->relays);
  g_free(address);
}

struct _GhMlsGroup {
  GObject parent_instance;
  GhMlsService *service;     /* owner; a group dies with it */
  MarmotGroupId gid;
  gchar *gid_hex;
  gchar *room_id;
  gchar nostr_hex[65];
  gchar *name;
  gchar *description;
  guint64 epoch;
  gboolean active;
  GhMlsGroupEnd end;
  gchar *removed_by;         /* hex, or NULL */
  gboolean removal_final;    /* REMOVED and no competing Commit can undo it */
  GhMlsReadState read;
  gboolean is_admin;
  gboolean adopted;          /* the adopted Marmot profile, else MDK 0.8 (fixed) */
  gboolean pending_commit;
  guint unsent_welcomes;
  GStrv members;
  GStrv admins;
  GStrv relays;              /* the current routing's relays (an adopted group's signed
                              * 0x8004 list): published to, and read */
  GPtrArray *addresses;      /* Address: earlier routing still read (nostrc-ms4d) */
  GStrv read_relays;         /* every relay read: relays and the addresses', sorted */
  /* Reading: one scope per set of addresses a relay serves (nostrc-ms4d), so
   * a relay is asked only for the h tags it carries. */
  GPtrArray *scopes;         /* GhRelayScope (owned) */
  GHashTable *url_scope;     /* url -> its scope (borrowed) */
  guint retain_source;       /* GhClock: the next address to age out */
  GHashTable *settled;       /* url -> GINT_TO_POINTER(1 eose / 2 failed / 3 never
                              * subscribed / 4 eose, older events missing) */
  gint64 cursor;             /* everything before it was processed */
  gint64 newest;             /* newest accepted created_at this session (bounded) */
  GQueue backfill;           /* Stored: backfill of every relay, applied oldest first */
  guint backfill_seq;
  GHashTable *backfilling;   /* url -> gint64*: relays delivering a backfill round that has
                              * not ended, and when each last delivered (monotonic) */
  guint quiet_source;        /* gives up relays silent too long (final review N1) */
  gsize backfill_bytes;      /* JSON bytes in backfill (review B4 bound) */
  gboolean history_incomplete; /* a relay's backfill ended incomplete (settled 4) */
  GQueue held;               /* Held: kind 445 of a later epoch, oldest first */
  GHashTable *held_ids;      /* their event ids (owned by the Held records) */
  gboolean decrypt_pending;  /* a shown Held is waiting (nostrc-oya4) */
  guint pending_source;      /* re-judges decrypt-pending when a Held ages out */
  GHashTable *junk_ids;      /* ids aged out unread: held again silently (bounded) */
  GQueue junk_order;         /* the same ids, oldest first (owned here) */
  gint64 pinned;             /* oldest created_at dropped unread this session; 0: none */
  gint64 floor;              /* when the account joined (or made) the group; 0: unknown */
  gboolean retrying;         /* retry_held() runs: a nested Commit only asks again */
  gboolean retry_again;
  gint64 retained_retry_us;  /* the last retry for a retained Commit (monotonic) */
  guint retained_source;     /* a retry for retained Commits, coalesced (W25 review M1) */
  /* Changes: the Commit being published, and who waits for it. */
  Round *round;
  GPtrArray *waiters;        /* GTask */
  /* Leaving (nostrc-2um6): our SelfRemove, while a member has to commit it. */
  gboolean leaving;
  gchar *leave_json;         /* the SelfRemove event of the current epoch */
  GhRelayPublish *leave_publish; /* publishing it; NULL once a relay took it */
  gboolean leave_sent;       /* a group relay accepted leave_json */
  gboolean leave_via_admin;  /* the leave is a Remove request an admin commits (review M1) */
  GhMlsLeaveFailure leave_failure; /* why the last leave was dropped (review L3, R1) */
  /* Re-review R1: admin Commits that moved the group on without our Remove
   * request. MDK 0.8's admin auto-commit drops such a request and commits
   * an empty Commit, so re-requesting each epoch would never end. */
  guint leave_misses;
  gboolean leave_admin_commit;   /* the Commit just applied was an admin's */
  /* Others leaving: members who asked to (hex), and the delayed Commit. */
  GHashTable *leavers;
  guint departures_source;
  /* nostrc-8ndz: our admin Commit requiring SelfRemove, delayed; tried once
   * per epoch (upgrade_epoch is the epoch tried, plus one; 0: none). */
  guint upgrade_source;
  guint64 upgrade_epoch;
  /* Review M1: an automatic Commit of ours (the upgrade, a departure) is out;
   * the admin's own changes made meanwhile wait behind it (QueuedChange). */
  gboolean auto_change;
  GQueue queued;
  /* Review L2: url -> failures in a row of a relay read only for an earlier
   * address (dropped at GH_MLS_SERVICE_EARLIER_RELAY_FAILURES). */
  GHashTable *earlier_failures;
  /* Member identities (nostrc-6ukh). */
  GHashTable *devices;       /* "account:signature key" (hex) -> Device */
  gboolean devices_loaded;   /* read once: a device new after that was just added */
  gchar *last_committer;     /* hex: whose Commit the next refresh follows, or NULL */
  guint32 last_committer_leaf; /* its leaf, the one leaf it renews (UINT32_MAX: none) */
  guint unverified;          /* accounts CHECKING or UNVERIFIED */
  GPtrArray *own_evidence;   /* KeyPackage JSON of the account's own Adds */
  gboolean records_forgotten; /* ended for good: its member records are gone (N3) */
  gchar *refused_json;       /* an admin's Commit refused for good (nostrc-prrl), or NULL */
  gint64 refused_at;         /* its created_at: the cursor stays behind it (L2) */
  GhMlsRefusal refusal;
  GHashTable *refusal_witnesses; /* hex: members read at this epoch after it (R4) */
};

enum {
  GROUP_PROP_0,
  GROUP_PROP_GROUP_ID,
  GROUP_PROP_ROOM_ID,
  GROUP_PROP_NAME,
  GROUP_PROP_DESCRIPTION,
  GROUP_PROP_EPOCH,
  GROUP_PROP_ACTIVE,
  GROUP_PROP_END,
  GROUP_PROP_REMOVED_BY,
  GROUP_PROP_READ_STATE,
  GROUP_PROP_IS_ADMIN,
  GROUP_PROP_PENDING_COMMIT,
  GROUP_PROP_UNSENT_WELCOMES,
  GROUP_PROP_UNREADABLE,
  GROUP_PROP_DECRYPT_PENDING,
  GROUP_PROP_HISTORY_INCOMPLETE,
  GROUP_PROP_LEAVING,
  GROUP_PROP_LEAVE_FAILED,
  GROUP_PROP_UNVERIFIED_MEMBERS,
  GROUP_PROP_CHANGE_REFUSED,
  N_GROUP_PROPS
};
static GParamSpec *group_props[N_GROUP_PROPS];
enum {
  GROUP_SIGNAL_MEMBERS_CHANGED,
  GROUP_SIGNAL_MEMBER_LEFT,
  GROUP_SIGNAL_CONFLICT_RESOLVED,
  N_GROUP_SIGNALS
};
static guint group_signals[N_GROUP_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsGroup, gh_mls_group, G_TYPE_OBJECT)

/* One KeyPackage format's publication (nostrc-lf62): its own `d` slot in
 * libmarmot, publish, relay ACK and cursor. */
typedef struct {
  GhMlsService *service;             /* owner (the slot is embedded in it) */
  GhMlsKeyPackageFormat format;
  GhMlsKeyPackageState state;
  gchar *id;                         /* the event last accepted by a relay */
  GPtrArray *event_ids;              /* every event of the slot accepted this run, oldest
                                      * first (bounded): a withdrawal's `e` tags (M4) */
  gboolean busy;                     /* made, being signed or published */
  gboolean rotate;                   /* a replacement was asked for */
  gboolean held;                     /* its due replacement waits for invitations */
  gboolean withdrawing;              /* publishing a deletion request for the slot (L1) */
  GhRelayPublish *withdrawal;        /* that request's publish (borrowed: slot->publish) */
  GhRelayPublish *publish;
  guint8 ref[32];                    /* KeyPackageRef of the one in flight */
  gboolean in_flight;                /* ...until its first relay OK confirms it */
} KeyPackageSlot;

struct _GhMlsService {
  GObject parent_instance;
  GhStore *store;                  /* borrowed */
  MarmotStorage *storage;          /* owned by marmot */
  Marmot *marmot;
  gboolean allow_unproven;         /* FALSE: the account requires proofs (VERIFIED_ONLY_KEY) */
  GhStoreMls *rooms;
  gchar *account;
  guint8 account_key[32];
  GhClock *clock;
  GhAccountController *accounts;
  GhAuthPolicy *policy;
  GhConversationStore *conversations;
  GhReactionStore *reactions;          /* nullable; W26 slice B */
  guint max_backfill_events;       /* per group (review B4) */
  gsize max_backfill_bytes;
  gint64 backfill_quiet_us;        /* a relay silent this long stops holding a flush */
  gint64 pending_shown_us;         /* a Held keeps decrypt-pending this long at most */
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
  GSettings *settings;
  GhDmInbox *inbox;
  GhMlsConsentFunc may_look_up;
  gpointer consent_data;
  GNetworkMonitor *network;
  guint publish_deadline;
  guint lookup_deadline;
  gint64 key_package_lifetime;
  gint64 key_package_max_hold;
  gboolean legacy_only;            /* MDK 0.8 KeyPackages only (config) */
  gboolean disposed;

  GPtrArray *groups;               /* GhMlsGroup, oldest first */

  guint64 generation;              /* 0: not running */
  guint64 run;                     /* bumped whenever a run stops */
  gboolean online;
  GCancellable *cancellable;       /* of the running generation */
  gulong accounts_handler;
  gulong network_handler;
  gulong relays_handler;

  /* Account proof (libmarmot >= 0.10.0) */
  GhMlsIdentityState identity;
  gboolean identity_busy;
  guint64 identity_generation;     /* the account generation the state belongs to */
  GCancellable *identity_cancellable; /* the signer request: per account generation */

  /* KeyPackages: one publication per format (nostrc-lf62) */
  GhMlsKeyPackageState key_package;  /* of every format the account publishes */
  KeyPackageSlot kp[GH_MLS_KEY_PACKAGE_N_FORMATS];
  gboolean key_package_held;         /* a due replacement waits for invitations */
  gboolean key_package_recheck;      /* the formats changed during a round: look again */
  guint key_package_hold_timer;      /* GhClock source: look at the hold again */
  guint key_package_sweep_timer;     /* GhClock source: the next not_after */

  /* Member identities (nostrc-6ukh, W24 review H1) */
  GHashTable *verifying;           /* account hex: a Verify the user asked for runs */
  GPtrArray *known_key_packages;   /* KeyPackage JSON already fetched (invitations, Verify) */

  /* Welcomes and sends in flight */
  GHashTable *deliveries;          /* key (welcome id hex or outbox id) -> Delivery */
  guint retry;
  guint retry_s;
  /* nostrc-8ndz: when the last background SelfRemove upgrade was scheduled
   * to fire (store clock, unix milliseconds): the next one is staggered
   * after it. */
  gint64 upgrade_slot;

  /* NIP-88 polls (nostrc-a36s): group_id_hex:poll_event_id → GhMlsPoll. */
  GHashTable *polls;
};

enum { PROP_0, PROP_KEY_PACKAGE_STATE, PROP_IDENTITY_STATE, PROP_KEY_PACKAGE_HELD, N_PROPS };
static GParamSpec *props[N_PROPS];
enum { SIGNAL_INVITE_RECEIVED, SIGNAL_GROUP_ADDED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void gh_mls_service_list_model_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhMlsService, gh_mls_service, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                                    gh_mls_service_list_model_init))

GType
gh_mls_key_package_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_KEY_PACKAGE_NONE, "GH_MLS_KEY_PACKAGE_NONE", "none" },
      { GH_MLS_KEY_PACKAGE_NO_RELAYS, "GH_MLS_KEY_PACKAGE_NO_RELAYS", "no-relays" },
      { GH_MLS_KEY_PACKAGE_PUBLISHING, "GH_MLS_KEY_PACKAGE_PUBLISHING", "publishing" },
      { GH_MLS_KEY_PACKAGE_PUBLISHED, "GH_MLS_KEY_PACKAGE_PUBLISHED", "published" },
      { GH_MLS_KEY_PACKAGE_FAILED, "GH_MLS_KEY_PACKAGE_FAILED", "failed" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhMlsKeyPackageState"), values));
  }
  return type;
}

GType
gh_mls_identity_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_IDENTITY_NOT_REQUIRED, "GH_MLS_IDENTITY_NOT_REQUIRED", "not-required" },
      { GH_MLS_IDENTITY_NONE, "GH_MLS_IDENTITY_NONE", "none" },
      { GH_MLS_IDENTITY_WAITING, "GH_MLS_IDENTITY_WAITING", "waiting" },
      { GH_MLS_IDENTITY_ENROLLED, "GH_MLS_IDENTITY_ENROLLED", "enrolled" },
      { GH_MLS_IDENTITY_DECLINED, "GH_MLS_IDENTITY_DECLINED", "declined" },
      { GH_MLS_IDENTITY_FAILED, "GH_MLS_IDENTITY_FAILED", "failed" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhMlsIdentityState"), values));
  }
  return type;
}

GType
gh_mls_member_identity_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_MEMBER_PROVEN, "GH_MLS_MEMBER_PROVEN", "proven" },
      { GH_MLS_MEMBER_VERIFIED, "GH_MLS_MEMBER_VERIFIED", "verified" },
      { GH_MLS_MEMBER_CHECKING, "GH_MLS_MEMBER_CHECKING", "checking" },
      { GH_MLS_MEMBER_UNVERIFIED, "GH_MLS_MEMBER_UNVERIFIED", "unverified" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static("GhMlsMemberIdentity", values));
  }
  return type;
}

GType
gh_mls_refusal_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_REFUSAL_NONE, "GH_MLS_REFUSAL_NONE", "none" },
      { GH_MLS_REFUSAL_BROKEN_PROOF, "GH_MLS_REFUSAL_BROKEN_PROOF", "broken-proof" },
      { GH_MLS_REFUSAL_UNPROVEN, "GH_MLS_REFUSAL_UNPROVEN", "unproven" },
      { GH_MLS_REFUSAL_UNFOLLOWABLE, "GH_MLS_REFUSAL_UNFOLLOWABLE", "unfollowable" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static("GhMlsRefusal", values));
  }
  return type;
}

gboolean
gh_mls_requires_proofs(GSettings *settings)
{
  if (!settings)
    return FALSE;
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key(schema, VERIFIED_ONLY_KEY) &&
         g_settings_get_boolean(settings, VERIFIED_ONLY_KEY);
}

GType
gh_mls_group_end_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_GROUP_END_NONE, "GH_MLS_GROUP_END_NONE", "none" },
      { GH_MLS_GROUP_END_LEFT, "GH_MLS_GROUP_END_LEFT", "left" },
      { GH_MLS_GROUP_END_REMOVED, "GH_MLS_GROUP_END_REMOVED", "removed" },
      { GH_MLS_GROUP_END_UNKNOWN, "GH_MLS_GROUP_END_UNKNOWN", "unknown" },
      { GH_MLS_GROUP_END_LEFT_DEVICE, "GH_MLS_GROUP_END_LEFT_DEVICE", "left-device" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static("GhMlsGroupEnd", values));
  }
  return type;
}

GType
gh_mls_read_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MLS_READ_IDLE, "GH_MLS_READ_IDLE", "idle" },
      { GH_MLS_READ_SYNCING, "GH_MLS_READ_SYNCING", "syncing" },
      { GH_MLS_READ_LIVE, "GH_MLS_READ_LIVE", "live" },
      { GH_MLS_READ_DISCONNECTED, "GH_MLS_READ_DISCONNECTED", "disconnected" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhMlsReadState"), values));
  }
  return type;
}

/* ---- Held events (a later epoch) -------------------------------------------------------- */

/* A kind 445 that could not be decrypted yet. */
typedef struct {
  gchar *id;
  gchar *json;
  gint64 created_at;   /* bounded to now + skew */
  guint64 epoch;       /* the group's epoch when it was held: it ages by tip advances */
  gboolean shown;      /* makes decrypt-pending (not the join second's stored answer) */
  gint64 held_us;      /* when it was first held (monotonic) */
  /* A Commit citing a proposal not received yet (nostrc-2um6 review H1):
   * offered again whenever a proposal arrives, and dropped after
   * GH_MLS_SERVICE_PROPOSAL_WAIT_TRIES retries or
   * GH_MLS_SERVICE_PROPOSAL_WAIT_S. */
  gboolean awaits_proposal;
  guint proposal_tries;
} Held;

static void
held_free(gpointer data)
{
  Held *held = data;
  g_free(held->id);
  g_free(held->json);
  g_free(held);
}

/* A kind 445 of a relay's stored answer (or an older page), waiting for that
 * relay's EOSE (nostrc-cpwf). */
typedef struct {
  gchar *json;
  gchar *url;
  gint64 created_at;
  guint seq;           /* arrival order */
  gboolean stored;     /* of a stored answer or a page, not live (GhRelayUpdate.backfill) */
} Stored;

static void
stored_free(gpointer data)
{
  Stored *stored = data;
  g_free(stored->json);
  g_free(stored->url);
  g_free(stored);
}

/* One member device (an MLS leaf) and what is known about who it is
 * (nostrc-6ukh). */
typedef struct {
  gchar *account;                 /* hex */
  gchar *signature_key;           /* hex */
  MarmotMemberIdentity leaf;
  GhMlsMemberIdentity state;
  gchar *added_by;                /* hex, or NULL */
} Device;

static void
device_free(gpointer data)
{
  Device *device = data;
  g_free(device->account);
  g_free(device->signature_key);
  g_free(device->added_by);
  g_free(device);
}

/* ---- Small helpers ------------------------------------------------------------------ */

static gboolean
lower_hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

static gchar *
to_hex(const guint8 *bytes, gsize length)
{
  gchar *out = g_malloc(length * 2 + 1);
  for (gsize i = 0; i < length; i++)
    g_snprintf(out + 2 * i, 3, "%02x", bytes[i]);
  out[length * 2] = '\0';
  return out;
}

/* A MarmotGroupId from hex (its data owned: marmot_group_id_free()). */
static gboolean
gid_from_hex(const gchar *hex, MarmotGroupId *out)
{
  gsize length = hex ? strlen(hex) : 0;
  if (length < 2 || length % 2 || length > 2 * GH_MESSAGE_MAX_MLS_GROUP_ID)
    return FALSE;
  g_autofree guint8 *bytes = g_malloc(length / 2);
  if (!nostr_hex2bin(bytes, hex, length / 2))
    return FALSE;
  *out = marmot_group_id_new(bytes, length / 2);
  return out->data != NULL;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static GStrv
sorted_strv(GPtrArray *items)
{
  g_ptr_array_sort(items, compare_strings);
  g_ptr_array_add(items, NULL);
  return (GStrv)g_ptr_array_free(items, FALSE);
}

static gboolean
strv_equal(const gchar *const *a, const gchar *const *b)
{
  if (!a || !b)
    return a == b;
  return g_strv_equal(a, b);
}

/* A libmarmot failure: the storage error behind it when there is one (so
 * GH_STORE_ERROR_FULL stays FULL), else the MarmotError. */
static gboolean
marmot_fail(GhMlsService *self, MarmotError err, const gchar *what, GError **error)
{
  GError *store_error = gh_store_marmot_take_error(self->storage);
  if (store_error) {
    g_propagate_prefixed_error(error, store_error, "%s: ", what);
    return FALSE;
  }
  GhMlsServiceError code = 0;
  switch (err) {
  case MARMOT_ERR_OWN_COMMIT_PENDING:
  /* The group's events cannot be dated within libmarmot's bound right now
   * (2lrz): transient, like a change still being sent. */
  case MARMOT_ERR_EVENT_RATE: code = GH_MLS_SERVICE_ERROR_BUSY; break;
  case MARMOT_ERR_ADMIN_ONLY:
  case MARMOT_ERR_COMMIT_FROM_NON_ADMIN: code = GH_MLS_SERVICE_ERROR_NOT_ADMIN; break;
  /* An invitee whose leaf lacks what the group requires (nostrc-zbmb). */
  case MARMOT_ERR_KEY_PACKAGE_CAPABILITIES: code = GH_MLS_SERVICE_ERROR_INVITEE_UNSUPPORTED; break;
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  case MARMOT_ERR_KEY_PACKAGE_IDENTITY:
    /* A leaf libmarmot refuses in a KeyPackage, a Commit or a Welcome's tree
     * (nostrc-7vyi). Requiring proofs: an unproven one (MDK 0.8, libmarmot
     * <= 0.9.0). Otherwise only a proof that does not verify is refused
     * (nostrc-6ukh). */
    if (!self->allow_unproven)
      g_set_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NEEDS_UPDATE,
                  "%s: someone uses an app that can't prove their account, and only groups "
                  "where every identity is verified are joined", what);
    else
      g_set_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_FORGED_IDENTITY,
                  "%s: someone's account proof does not verify", what);
    return FALSE;
#endif
  default: break;
  }
  if (code)
    g_set_error(error, GH_MLS_SERVICE_ERROR, code, "%s: %s", what, marmot_error_string(err));
  else
    g_set_error(error, GH_MLS_COMMIT_ERROR, err, "%s: %s", what, marmot_error_string(err));
  return FALSE;
}

static void
drop_stale_error(GhMlsService *self)
{
  GError *stale = gh_store_marmot_take_error(self->storage);
  g_clear_error(&stale);
}

static gboolean
running(GhMlsService *self)
{
  return !self->disposed && self->generation != 0 && self->online;
}

static gboolean identity_ready(GhMlsService *self);

static gint64
now_s(GhMlsService *self)
{
  return gh_clock_get_unix(self->clock);
}

/* The group's cursor scope: "mls/" + 32 hex of SHA-256(domain || group id). */
static gchar *
cursor_scope(GhMlsGroup *group)
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-cursor/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, group->gid.data, (gssize)group->gid.len);
  return g_strdup_printf("mls/%.32s", g_checksum_get_string(sum));
}

/* The cursor scope that keeps when a pending invitation's Welcome was
 * made (review M3): "mls/w/" + 32 hex of SHA-256(domain || wrapper id). */
static gchar *
welcome_time_scope(const guint8 wrapper[32])
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-welcome-time/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, wrapper, 32);
  return g_strdup_printf("mls/w/%.32s", g_checksum_get_string(sum));
}

/* Where the group's join floor is kept: "mls/j/" + 32 hex of
 * SHA-256(domain || group id). */
static gchar *
floor_scope(GhMlsGroup *group)
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-join-floor/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, group->gid.data, (gssize)group->gid.len);
  return g_strdup_printf("mls/j/%.32s", g_checksum_get_string(sum));
}

/* The per-group Tor isolation label (charter §4.3 acct/mls/<hash(group)>). */
static gchar *
isolation_label(GhMlsGroup *group)
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-isolation/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, group->gid.data, (gssize)group->gid.len);
  return g_strdup_printf("mls-%.16s", g_checksum_get_string(sum));
}

/* ---- GhMlsGroup --------------------------------------------------------------------- */

const gchar *
gh_mls_group_get_group_id(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return self->gid_hex;
}

const gchar *
gh_mls_group_get_room_id(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return self->room_id;
}

const gchar *
gh_mls_group_get_name(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return self->name;
}

const gchar *
gh_mls_group_get_description(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return self->description;
}

guint64
gh_mls_group_get_epoch(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return self->epoch;
}

gboolean
gh_mls_group_get_active(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->active;
}

gboolean
gh_mls_group_get_leaving(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->leaving;
}

gboolean
gh_mls_group_get_leave_via_admin(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->leaving && self->leave_via_admin;
}

gboolean
gh_mls_group_get_leave_failed(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->leave_failure != GH_MLS_LEAVE_FAILURE_NONE;
}

GhMlsLeaveFailure
gh_mls_group_get_leave_failure(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), GH_MLS_LEAVE_FAILURE_NONE);
  return self->leave_failure;
}

GhMlsGroupEnd
gh_mls_group_get_end(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), GH_MLS_GROUP_END_NONE);
  return self->end;
}

const gchar *
gh_mls_group_get_removed_by(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return self->removed_by;
}

GhMlsReadState
gh_mls_group_get_read_state(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), GH_MLS_READ_IDLE);
  return self->read;
}

gboolean
gh_mls_group_get_is_admin(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->is_admin;
}

gboolean
gh_mls_group_get_adopted(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->adopted;
}

gboolean
gh_mls_group_get_pending_commit(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->pending_commit;
}

guint
gh_mls_group_get_unsent_welcomes(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return self->unsent_welcomes;
}

gint64
gh_mls_group_get_cursor(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return self->cursor;
}

guint
gh_mls_group_get_unreadable(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return g_queue_get_length(&self->held);
}

gboolean
gh_mls_group_get_decrypt_pending(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->decrypt_pending;
}

gint64
gh_mls_group_get_join_time(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return self->floor;
}

gboolean
gh_mls_group_get_history_incomplete(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->history_incomplete;
}

GStrv
gh_mls_group_dup_members(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return g_strdupv(self->members);
}

/* How weak an identity is: the account shows its weakest device. */
static gint
identity_weakness(GhMlsMemberIdentity state)
{
  switch (state) {
  case GH_MLS_MEMBER_PROVEN: return 0;
  case GH_MLS_MEMBER_VERIFIED: return 1;
  case GH_MLS_MEMBER_CHECKING: return 2;
  case GH_MLS_MEMBER_UNVERIFIED:
  default: return 3;
  }
}

static Device *
weakest_device(GhMlsGroup *self, const gchar *member)
{
  Device *weakest = NULL;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->devices);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Device *device = value;
    if (g_strcmp0(device->account, member) == 0 &&
        (!weakest || identity_weakness(device->state) > identity_weakness(weakest->state)))
      weakest = device;
  }
  return weakest;
}

GhMlsMemberIdentity
gh_mls_group_get_member_identity(GhMlsGroup *self, const gchar *member, gchar **out_added_by)
{
  if (out_added_by)
    *out_added_by = NULL;
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), GH_MLS_MEMBER_PROVEN);
  Device *device = member ? weakest_device(self, member) : NULL;
  if (!device) {
    /* A listed member whose devices could not be read is not shown as
     * fine (W24 review N2); someone not in the group has nothing to show. */
    gboolean listed = member && self->members &&
                      g_strv_contains((const gchar *const *)self->members, member);
    return listed ? GH_MLS_MEMBER_UNVERIFIED : GH_MLS_MEMBER_PROVEN;
  }
  if (out_added_by)
    *out_added_by = g_strdup(device->added_by);
  return device->state;
}

guint
gh_mls_group_get_unverified_members(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), 0);
  return self->unverified;
}

gboolean
gh_mls_group_get_change_refused(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), FALSE);
  return self->refused_json != NULL;
}

GhMlsRefusal
gh_mls_group_get_refusal(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), GH_MLS_REFUSAL_NONE);
  return self->refused_json ? self->refusal : GH_MLS_REFUSAL_NONE;
}

GStrv
gh_mls_group_dup_admins(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return g_strdupv(self->admins);
}

GStrv
gh_mls_group_dup_relays(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return g_strdupv(self->relays);
}

GStrv
gh_mls_group_dup_read_relays(GhMlsGroup *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(self), NULL);
  return g_strdupv(self->read_relays);
}

static void upgrade_schedule(GhMlsGroup *group);

static void
group_set_read(GhMlsGroup *group, GhMlsReadState read)
{
  if (group->read == read)
    return;
  group->read = read;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_READ_STATE]);
  if (read == GH_MLS_READ_LIVE)
    upgrade_schedule(group);   /* caught up (nostrc-8ndz review L1) */
}

static void
gh_mls_group_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMlsGroup *self = GH_MLS_GROUP(object);
  switch (id) {
  case GROUP_PROP_GROUP_ID: g_value_set_string(value, self->gid_hex); break;
  case GROUP_PROP_ROOM_ID: g_value_set_string(value, self->room_id); break;
  case GROUP_PROP_NAME: g_value_set_string(value, self->name); break;
  case GROUP_PROP_DESCRIPTION: g_value_set_string(value, self->description); break;
  case GROUP_PROP_EPOCH: g_value_set_uint64(value, self->epoch); break;
  case GROUP_PROP_ACTIVE: g_value_set_boolean(value, self->active); break;
  case GROUP_PROP_END: g_value_set_enum(value, self->end); break;
  case GROUP_PROP_REMOVED_BY: g_value_set_string(value, self->removed_by); break;
  case GROUP_PROP_READ_STATE: g_value_set_enum(value, self->read); break;
  case GROUP_PROP_IS_ADMIN: g_value_set_boolean(value, self->is_admin); break;
  case GROUP_PROP_PENDING_COMMIT: g_value_set_boolean(value, self->pending_commit); break;
  case GROUP_PROP_UNSENT_WELCOMES: g_value_set_uint(value, self->unsent_welcomes); break;
  case GROUP_PROP_UNREADABLE: g_value_set_uint(value, g_queue_get_length(&self->held)); break;
  case GROUP_PROP_DECRYPT_PENDING: g_value_set_boolean(value, self->decrypt_pending); break;
  case GROUP_PROP_UNVERIFIED_MEMBERS: g_value_set_uint(value, self->unverified); break;
  case GROUP_PROP_CHANGE_REFUSED: g_value_set_boolean(value, self->refused_json != NULL); break;
  case GROUP_PROP_HISTORY_INCOMPLETE: g_value_set_boolean(value, self->history_incomplete); break;
  case GROUP_PROP_LEAVING: g_value_set_boolean(value, self->leaving); break;
  case GROUP_PROP_LEAVE_FAILED:
    g_value_set_boolean(value, self->leave_failure != GH_MLS_LEAVE_FAILURE_NONE);
    break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_mls_group_finalize(GObject *object)
{
  GhMlsGroup *self = GH_MLS_GROUP(object);
  g_warn_if_fail(self->scopes->len == 0 && self->retain_source == 0 && self->round == NULL &&
                 self->leave_publish == NULL && self->departures_source == 0 &&
                 self->upgrade_source == 0 && g_queue_is_empty(&self->queued));
  marmot_group_id_free(&self->gid);
  g_free(self->leave_json);
  g_hash_table_unref(self->leavers);
  g_hash_table_unref(self->earlier_failures);
  g_free(self->gid_hex);
  g_free(self->room_id);
  g_free(self->name);
  g_free(self->description);
  g_free(self->removed_by);
  g_strfreev(self->members);
  g_strfreev(self->admins);
  g_strfreev(self->relays);
  g_strfreev(self->read_relays);
  g_ptr_array_unref(self->addresses);
  g_ptr_array_unref(self->scopes);
  g_hash_table_unref(self->url_scope);
  g_hash_table_unref(self->settled);
  g_hash_table_unref(self->held_ids);
  g_queue_clear_full(&self->held, held_free);
  g_hash_table_unref(self->junk_ids);
  g_queue_clear_full(&self->junk_order, g_free);
  if (self->pending_source)
    g_source_remove(self->pending_source);
  if (self->retained_source)
    g_source_remove(self->retained_source);
  g_queue_clear_full(&self->backfill, stored_free);
  if (self->quiet_source)
    g_source_remove(self->quiet_source);
  g_hash_table_unref(self->backfilling);
  g_ptr_array_unref(self->waiters);
  g_hash_table_unref(self->devices);
  g_ptr_array_unref(self->own_evidence);
  g_free(self->last_committer);
  g_free(self->refused_json);
  g_hash_table_unref(self->refusal_witnesses);
  G_OBJECT_CLASS(gh_mls_group_parent_class)->finalize(object);
}

static void
gh_mls_group_class_init(GhMlsGroupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_mls_group_get_property;
  object_class->finalize = gh_mls_group_finalize;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;
  group_props[GROUP_PROP_GROUP_ID] = g_param_spec_string("group-id", NULL, NULL, NULL, ro);
  group_props[GROUP_PROP_ROOM_ID] = g_param_spec_string("room-id", NULL, NULL, NULL, ro);
  group_props[GROUP_PROP_NAME] = g_param_spec_string("name", NULL, NULL, NULL, ro);
  group_props[GROUP_PROP_DESCRIPTION] = g_param_spec_string("description", NULL, NULL, NULL, ro);
  group_props[GROUP_PROP_EPOCH] = g_param_spec_uint64("epoch", NULL, NULL, 0, G_MAXUINT64, 0, ro);
  group_props[GROUP_PROP_ACTIVE] = g_param_spec_boolean("active", NULL, NULL, FALSE, ro);
  group_props[GROUP_PROP_END] = g_param_spec_enum("end", NULL, NULL, GH_TYPE_MLS_GROUP_END,
                                                  GH_MLS_GROUP_END_NONE, ro);
  group_props[GROUP_PROP_REMOVED_BY] = g_param_spec_string("removed-by", NULL, NULL, NULL, ro);
  group_props[GROUP_PROP_READ_STATE] = g_param_spec_enum("read-state", NULL, NULL,
                                                         GH_TYPE_MLS_READ_STATE,
                                                         GH_MLS_READ_IDLE, ro);
  group_props[GROUP_PROP_IS_ADMIN] = g_param_spec_boolean("is-admin", NULL, NULL, FALSE, ro);
  group_props[GROUP_PROP_PENDING_COMMIT] = g_param_spec_boolean("pending-commit", NULL, NULL,
                                                                FALSE, ro);
  group_props[GROUP_PROP_UNSENT_WELCOMES] = g_param_spec_uint("unsent-welcomes", NULL, NULL, 0,
                                                              G_MAXUINT, 0, ro);
  group_props[GROUP_PROP_UNREADABLE] = g_param_spec_uint("unreadable", NULL, NULL, 0, G_MAXUINT, 0,
                                                         ro);
  group_props[GROUP_PROP_DECRYPT_PENDING] =
    g_param_spec_boolean("decrypt-pending", NULL, NULL, FALSE, ro);
  group_props[GROUP_PROP_HISTORY_INCOMPLETE] =
    g_param_spec_boolean("history-incomplete", NULL, NULL, FALSE, ro);
  group_props[GROUP_PROP_LEAVING] = g_param_spec_boolean("leaving", NULL, NULL, FALSE, ro);
  group_props[GROUP_PROP_LEAVE_FAILED] = g_param_spec_boolean("leave-failed", NULL, NULL, FALSE,
                                                              ro);
  group_props[GROUP_PROP_UNVERIFIED_MEMBERS] =
    g_param_spec_uint("unverified-members", NULL, NULL, 0, G_MAXUINT, 0, ro);
  group_props[GROUP_PROP_CHANGE_REFUSED] =
    g_param_spec_boolean("change-refused", NULL, NULL, FALSE, ro);
  g_object_class_install_properties(object_class, N_GROUP_PROPS, group_props);
  group_signals[GROUP_SIGNAL_MEMBERS_CHANGED] =
    g_signal_new("members-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                 NULL, NULL, G_TYPE_NONE, 0);
  group_signals[GROUP_SIGNAL_MEMBER_LEFT] =
    g_signal_new("member-left", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  group_signals[GROUP_SIGNAL_CONFLICT_RESOLVED] =
    g_signal_new("conflict-resolved", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                 NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void
gh_mls_group_init(GhMlsGroup *self)
{
  self->settled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->waiters = g_ptr_array_new_with_free_func(g_object_unref);
  g_queue_init(&self->held);
  g_queue_init(&self->backfill);
  self->backfilling = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->held_ids = g_hash_table_new(g_str_hash, g_str_equal);
  self->junk_ids = g_hash_table_new(g_str_hash, g_str_equal);
  g_queue_init(&self->junk_order);
  self->members = g_new0(gchar *, 1);
  self->admins = g_new0(gchar *, 1);
  self->relays = g_new0(gchar *, 1);
  self->read_relays = g_new0(gchar *, 1);
  self->addresses = g_ptr_array_new_with_free_func(address_free);
  self->scopes = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  self->url_scope = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->leavers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->earlier_failures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init(&self->queued);
  self->refusal_witnesses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, device_free);
  self->own_evidence = g_ptr_array_new_with_free_func(g_free);
  self->last_committer_leaf = G_MAXUINT32;
}

void
gh_mls_invite_free(GhMlsInvite *invite)
{
  if (!invite)
    return;
  g_free(invite->wrapper_id);
  g_free(invite->inviter);
  g_free(invite->group_name);
  g_strfreev(invite->relays);
  g_free(invite);
}

/* ---- Groups: state from libmarmot ------------------------------------------------------ */

static void group_subscribe(GhMlsGroup *group);
static void update_history_incomplete(GhMlsGroup *group);
static void group_unsubscribe(GhMlsGroup *group);
static void welcomes_pump(GhMlsGroup *group);

/* decrypt-pending (nostrc-oya4): an active group holds an event that is
 * really waiting for a Commit. A held event's type is sealed under an epoch
 * the account has not reached, so no count of them is a count of messages;
 * this says only that something can't be read yet. Not shown: an event
 * dated in the join's own second that came in a stored answer -- where the
 * joiner's own Add Commit, sealed in an epoch it never had, always lands
 * (W20 re-review N4) -- nothing of an ended group, and nothing held longer
 * than GH_MLS_SERVICE_PENDING_SHOWN_S: anyone can post undecryptable junk
 * with the group's h, which a quiet group would otherwise show for good
 * (W22 review N4); it stays held and is retried. */
static gboolean pending_expired(gpointer data);

static void
sync_decrypt_pending(GhMlsGroup *group)
{
  gint64 now = g_get_monotonic_time(), limit = group->service->pending_shown_us;
  gint64 next = G_MAXINT64;   /* when the next shown Held ages out */
  gboolean pending = FALSE;
  /* Nothing is "waiting" behind a Commit refused for good (nostrc-prrl). */
  for (GList *l = group->held.head; group->active && !group->refused_json && l; l = l->next) {
    Held *held = l->data;
    if (!held->shown || now - held->held_us >= limit)
      continue;
    pending = TRUE;
    next = MIN(next, held->held_us + limit);
  }
  if (group->pending_source) {
    g_source_remove(group->pending_source);
    group->pending_source = 0;
  }
  if (pending)
    group->pending_source = g_timeout_add(MAX((next - now) / 1000, 1) + 1, pending_expired,
                                          group);
  if (pending == group->decrypt_pending)
    return;
  group->decrypt_pending = pending;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_DECRYPT_PENDING]);
}

static gboolean
pending_expired(gpointer data)
{
  GhMlsGroup *group = data;
  group->pending_source = 0;
  sync_decrypt_pending(group);
  return G_SOURCE_REMOVE;
}

/* An ended group (left or removed) holds nothing: none of it can ever be
 * read, so none of it is "unreadable yet" (nostrc-xrya). */
static void
drop_held(GhMlsGroup *group)
{
  group->pinned = 0;
  if (!g_queue_is_empty(&group->held)) {
    g_hash_table_remove_all(group->held_ids);
    g_queue_clear_full(&group->held, held_free);
    g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_UNREADABLE]);
  }
  sync_decrypt_pending(group);
}

/* A group is read while active, and while a removal of the account may
 * still lose its epoch (W22 review B1): libmarmot then judges that epoch's
 * Commits, and a winner re-activates the group. Nothing else of it is read
 * or held. */
static gboolean
listening(GhMlsGroup *group)
{
  return group->active || ((group->end == GH_MLS_GROUP_END_REMOVED ||
                            group->end == GH_MLS_GROUP_END_LEFT) && !group->removal_final);
}

/* An event that aged out unread (retry_held()) is remembered: fetched again
 * (a later REQ's overlap, a restart), it is held again -- it may still be
 * convergence input whose parent comes later (W25 slice N re-review N1;
 * transports/nostr.md: deferred input is never recorded as seen) -- but
 * silently: it does not raise decrypt-pending again (nostrc-oya4). */
static void
remember_junk(GhMlsGroup *group, const gchar *id)
{
  if (g_hash_table_contains(group->junk_ids, id))
    return;
  if (g_queue_get_length(&group->junk_order) >= GH_MLS_SERVICE_MAX_HELD) {
    gchar *oldest = g_queue_pop_head(&group->junk_order);
    g_hash_table_remove(group->junk_ids, oldest);
    g_free(oldest);
  }
  gchar *copy = g_strdup(id);
  g_queue_push_tail(&group->junk_order, copy);
  g_hash_table_add(group->junk_ids, copy);
}

static gboolean
account_matches_model(GhMlsService *self)
{
  return g_strcmp0(gh_conversation_store_get_account(self->conversations), self->account) == 0;
}

/* Whether group is a DM in White Noise's shape: two members and no name
 * (MDK 0.11 groups.rs set_direct_member_ids_from_roster; W26 slice A). */
static gboolean
group_is_dm(GhMlsGroup *group)
{
  return !group->name && group->members && g_strv_length(group->members) == 2;
}

/* The group's room in the model and its stored name. */
static void
group_list_room(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_save_room(self->store, group->gid_hex, group->name ? group->name : "", NULL,
                              &error))
    g_message("Groundhog could not store an encrypted group's room: %s", error->message);
  if (account_matches_model(self)) {
    GhConversation *conv =
      gh_conversation_store_ensure_group(self->conversations, group->room_id, group->name);
    if (conv) {
      gboolean dm = group_is_dm(group);
      gh_conversation_set_is_direct(conv, dm);
      if (group->members)
        gh_conversation_set_mls_peers(conv, (const gchar *const *)group->members);
      /* A Marmot DM from a known contact is auto-accepted (not a request).
       * A DM from a stranger stays as a request (charter §7.9). Groups
       * we created ourselves get has_own_message when we send the first
       * message, so they are never requests. */
      if (dm && gh_conversation_get_is_request(conv)) {
        /* Check whether any peer is already an accepted contact (a peer
         * of another accepted DM). Walk the conversation store inline
         * to avoid a link dependency on groundhog-mls-ui. */
        const gchar *const *dm_peers = gh_conversation_get_peers(conv);
        gboolean from_contact = FALSE;
        guint n = g_list_model_get_n_items(G_LIST_MODEL(self->conversations));
        for (guint k = 0; !from_contact && dm_peers && dm_peers[0] && k < n; k++) {
          g_autoptr(GhConversation) other = g_list_model_get_item(G_LIST_MODEL(self->conversations), k);
          if (other == conv)
            continue;
          GhConversationBackend b = gh_conversation_get_backend(other);
          gboolean is_dm_source = b == GH_CONVERSATION_BACKEND_NIP17 ||
                                  (b == GH_CONVERSATION_BACKEND_MLS &&
                                   gh_conversation_get_is_direct(other));
          if (!is_dm_source || gh_conversation_get_is_request(other))
            continue;
          const gchar *const *other_peers = gh_conversation_get_peers(other);
          for (guint j = 0; other_peers && other_peers[j]; j++)
            if (g_ascii_strcasecmp(other_peers[j], dm_peers[0]) == 0)
              from_contact = TRUE;
        }
        if (from_contact)
          gh_conversation_accept(conv);
      }
    }
  }
}

/* ---- Member identities (nostrc-6ukh, W24 review H1/M1) ------------------------------------ */

static GStrv discovery_relays(GhMlsService *self);
static void verify_group(GhMlsGroup *group);

#define MAX_KNOWN_KEY_PACKAGES 256

static void
device_save(GhMlsGroup *group, Device *device, GhStoreMlsMemberCheck check)
{
  GhMlsService *self = group->service;
  GhStoreMlsMember record = {
    .check = check,
    .checked_at = check == GH_STORE_MLS_MEMBER_UNCHECKED ? 0 : MAX(now_s(self), 0),
    .added_by = device->added_by,
  };
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_member_save(self->store, group->gid_hex, device->account,
                                device->signature_key, &record, &error))
    g_message("Groundhog could not store what it knows about a group member: %s",
              error->message);
}

/* "unverified-members": accounts whose weakest device is not confirmed. */
static void
sync_unverified(GhMlsGroup *group)
{
  g_autoptr(GHashTable) weak = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->devices);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Device *device = value;
    if (device->state == GH_MLS_MEMBER_CHECKING || device->state == GH_MLS_MEMBER_UNVERIFIED)
      g_hash_table_add(weak, device->account);
  }
  guint n = g_hash_table_size(weak);
  if (n == group->unverified)
    return;
  group->unverified = n;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_UNVERIFIED_MEMBERS]);
}

/* The device of `account` in leaf `leaf` before the Commit just applied. */
static Device *
device_at(GhMlsGroup *group, const gchar *account, guint32 leaf)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->devices);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Device *device = value;
    if (device->leaf.leaf_index == leaf && g_strcmp0(device->account, account) == 0)
      return device;
  }
  return NULL;
}

/* A device new to us: PROVEN by libmarmot; else what the store says; else,
 * when `renewed_from` is the key this leaf had before its own Commit (its
 * UpdatePath, signed in by that key: libmarmot's committer_leaf), what was
 * known of that key (W24 review M1); else UNVERIFIED. A device that
 * appears after the group was first read, and is no renewal, was added by
 * the Commit just applied (group->last_committer). */
static Device *
device_new(GhMlsGroup *group, const MarmotMemberIdentity *leaf, const gchar *account,
           const gchar *signature_key, const Device *renewed_from)
{
  GhMlsService *self = group->service;
  Device *device = g_new0(Device, 1);
  device->account = g_strdup(account);
  device->signature_key = g_strdup(signature_key);
  device->leaf = *leaf;
  if (leaf->status == MARMOT_MEMBER_IDENTITY_PROVEN) {
    device->state = GH_MLS_MEMBER_PROVEN;
    return device;
  }
  GhStoreMlsMember record;
  gboolean found = FALSE;
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_member_load(self->store, group->gid_hex, account, signature_key, &record,
                                &found, &error))
    g_message("Groundhog could not read what it knows about a group member: %s",
              error->message);
  device->added_by = g_steal_pointer(&record.added_by);
  gboolean save = FALSE;
  GhStoreMlsMemberCheck check = found ? record.check : GH_STORE_MLS_MEMBER_UNCHECKED;
  if (leaf->status == MARMOT_MEMBER_IDENTITY_UNPROVEN && found &&
      record.check == GH_STORE_MLS_MEMBER_VERIFIED) {
    device->state = GH_MLS_MEMBER_VERIFIED;
  } else if (leaf->status == MARMOT_MEMBER_IDENTITY_UNPROVEN && leaf->welcome_signer) {
    /* Its account sent us the Welcome this device signed (the NIP-59
     * seal): the account vouched for it, as for a KeyPackage it published
     * -- typically an MDK group's creator who invited us (owkh). */
    device->state = GH_MLS_MEMBER_VERIFIED;
    check = GH_STORE_MLS_MEMBER_VERIFIED;
    save = TRUE;
  } else if (leaf->status == MARMOT_MEMBER_IDENTITY_UNPROVEN && renewed_from &&
             (renewed_from->state == GH_MLS_MEMBER_VERIFIED ||
              renewed_from->state == GH_MLS_MEMBER_PROVEN)) {
    device->state = GH_MLS_MEMBER_VERIFIED;   /* the verified key signed this one in */
    check = GH_STORE_MLS_MEMBER_VERIFIED;
    save = TRUE;
  } else {
    device->state = renewed_from && renewed_from->state == GH_MLS_MEMBER_CHECKING
                      ? GH_MLS_MEMBER_CHECKING : GH_MLS_MEMBER_UNVERIFIED;
  }
  if (!device->added_by && renewed_from && renewed_from->added_by) {
    device->added_by = g_strdup(renewed_from->added_by);   /* still whoever added it */
    save = TRUE;
  } else if (!device->added_by && !renewed_from && group->devices_loaded &&
             lower_hex64(group->last_committer)) {
    device->added_by = g_strdup(group->last_committer);
    save = TRUE;
  }
  if (save)
    device_save(group, device, check);
  gh_store_mls_member_clear(&record);
  return device;
}

/* Reads the group's devices from libmarmot; TRUE when what it shows changed.
 * Devices that left are forgotten in the store (W24 review N3). */
static gboolean
refresh_devices(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  MarmotMemberIdentity *leaves = NULL;
  size_t n = 0;
  if (marmot_get_group_member_identities(self->marmot, &group->gid, &leaves, &n) != MARMOT_OK)
    return FALSE;
  gboolean changed = FALSE;
  GHashTable *next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, device_free);
  for (size_t i = 0; i < n; i++) {
    g_autofree gchar *account = to_hex(leaves[i].account_pubkey, 32);
    g_autofree gchar *signature_key = to_hex(leaves[i].signature_key, 32);
    gchar *key = g_strconcat(account, ":", signature_key, NULL);
    gpointer old_key = NULL, value = NULL;
    Device *device = NULL;
    if (g_hash_table_steal_extended(group->devices, key, &old_key, &value)) {
      g_free(old_key);
      device = value;
      device->leaf = leaves[i];
      /* The same device proved itself (a self-update with the proof). */
      if (leaves[i].status == MARMOT_MEMBER_IDENTITY_PROVEN &&
          device->state != GH_MLS_MEMBER_PROVEN) {
        device->state = GH_MLS_MEMBER_PROVEN;
        changed = TRUE;
      }
    } else {
      /* Only the committer's own leaf is renewed in place; any other new
       * key is a device someone added (libmarmot, W24 review B1). */
      const Device *renewed_from =
        leaves[i].leaf_index == group->last_committer_leaf &&
            g_strcmp0(account, group->last_committer) == 0
          ? device_at(group, account, leaves[i].leaf_index) : NULL;
      device = device_new(group, &leaves[i], account, signature_key, renewed_from);
      changed = TRUE;
    }
    g_hash_table_replace(next, key, device);
  }
  free(leaves);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->devices);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Device *gone = value;
    changed = TRUE;
    if (gone->state != GH_MLS_MEMBER_PROVEN)
      gh_store_mls_member_delete(self->store, group->gid_hex, gone->account,
                                 gone->signature_key, NULL);
  }
  g_hash_table_unref(group->devices);
  group->devices = next;
  group->devices_loaded = TRUE;
  g_clear_pointer(&group->last_committer, g_free);
  group->last_committer_leaf = G_MAXUINT32;
  sync_unverified(group);
  return changed;
}

/* Whether one of `candidates` (KeyPackage event JSON) is the device's. */
static gboolean
device_matches(const Device *device, GPtrArray *candidates)
{
  for (guint i = 0; candidates && i < candidates->len; i++) {
    bool matches = false;
    if (marmot_key_package_event_matches_member(g_ptr_array_index(candidates, i),
                                                &device->leaf, &matches) == MARMOT_OK &&
        matches)
      return TRUE;
  }
  return FALSE;
}

/* A KeyPackage event fetched anyway (an invitation, a Verify): evidence
 * kept in memory for the run, bounded. */
static void
remember_key_package(GhMlsService *self, const gchar *json)
{
  if (!json)
    return;
  for (guint i = 0; i < self->known_key_packages->len; i++)
    if (g_str_equal(g_ptr_array_index(self->known_key_packages, i), json))
      return;
  if (self->known_key_packages->len >= MAX_KNOWN_KEY_PACKAGES)
    g_ptr_array_remove_index(self->known_key_packages, 0);
  g_ptr_array_add(self->known_key_packages, g_strdup(json));
}

/* Confirms the group's unconfirmed devices from evidence already in hand:
 * the KeyPackages the account added them with, and those it fetched anyway.
 * No network traffic (W24 review H1). */
static void
verify_group(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (!group->active)
    return;
  gboolean changed = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->devices);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Device *device = value;
    if (device->state != GH_MLS_MEMBER_UNVERIFIED)
      continue;
    if (device_matches(device, group->own_evidence) ||
        device_matches(device, self->known_key_packages)) {
      device->state = GH_MLS_MEMBER_VERIFIED;
      device_save(group, device, GH_STORE_MLS_MEMBER_VERIFIED);
      changed = TRUE;
    }
  }
  if (changed) {
    sync_unverified(group);
    g_signal_emit(group, group_signals[GROUP_SIGNAL_MEMBERS_CHANGED], 0);
  }
}

/* The KeyPackages the account adds people with: the strongest evidence of
 * who those devices are (it saw the signed event itself). */
static void
remember_own_evidence(GhMlsGroup *group, GPtrArray *key_packages)
{
  for (guint i = 0; key_packages && i < key_packages->len; i++) {
    if (group->own_evidence->len >= MAX_OWN_EVIDENCE)
      g_ptr_array_remove_index(group->own_evidence, 0);
    g_ptr_array_add(group->own_evidence, g_strdup(g_ptr_array_index(key_packages, i)));
  }
}

static gint64
event_created_at(const gchar *json)
{
  gint64 at = 0;
  NostrEvent *event = json ? nostr_event_new() : NULL;
  if (event && nostr_event_deserialize_compact(event, json, NULL) == 1)
    at = nostr_event_get_created_at(event);
  if (event)
    nostr_event_free(event);
  return at;
}

/* An admin's Commit libmarmot refused for good (nostrc-prrl): the group
 * can't follow it, and nothing is "waiting". Kept in the store with why
 * (`refusal`; for a proof, whether proofs are required decides) until a
 * Commit moves the group on, and the read cursor stays behind it (W24
 * review L2). event_json NULL clears it. */
static void
set_refused(GhMlsGroup *group, const gchar *event_json, GhMlsRefusal refusal)
{
  GhMlsService *self = group->service;
  gboolean was = group->refused_json != NULL;
  gboolean same = event_json && g_strcmp0(group->refused_json, event_json) == 0;
  if (!same) {
    g_hash_table_remove_all(group->refusal_witnesses);
    g_free(group->refused_json);
    group->refused_json = event_json ? g_strdup(event_json) : NULL;
    group->refused_at = event_created_at(event_json);
    group->refusal = !event_json ? GH_MLS_REFUSAL_NONE
                     : refusal == GH_MLS_REFUSAL_UNFOLLOWABLE ? GH_MLS_REFUSAL_UNFOLLOWABLE
                     : self->allow_unproven ? GH_MLS_REFUSAL_BROKEN_PROOF
                                            : GH_MLS_REFUSAL_UNPROVEN;
    guint cause = group->refusal == GH_MLS_REFUSAL_UNFOLLOWABLE ? 2
                  : group->refusal == GH_MLS_REFUSAL_UNPROVEN ? 1 : 0;
    g_autoptr(GError) error = NULL;
    if ((event_json || was) &&
        !gh_store_mls_refused_save(self->store, group->gid_hex, event_json, cause, &error))
      g_message("Groundhog could not store a group's refused change: %s", error->message);
  }
  if (was != (event_json != NULL) || !same)
    g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_CHANGE_REFUSED]);
  sync_decrypt_pending(group);
}

/* Whether every member but us has been read at the group's epoch after its
 * refused Commit (W24b slice H re-review R4): they all refused it. */
static gboolean
refused_by_everyone(GhMlsGroup *group)
{
  if (!group->members || g_hash_table_size(group->refusal_witnesses) == 0)
    return FALSE;
  for (guint i = 0; group->members[i]; i++)
    if (g_strcmp0(group->members[i], group->service->account) != 0 &&
        !g_hash_table_contains(group->refusal_witnesses, group->members[i]))
      return FALSE;
  return TRUE;
}

/* The refused change of a stored group, at start (W24 review L2). */
static void
load_refused(GhMlsGroup *group)
{
  g_autofree gchar *json = NULL;
  guint cause = 0;
  if (!gh_store_mls_refused_load(group->service->store, group->gid_hex, &json, &cause, NULL) ||
      !json)
    return;
  group->refused_json = g_steal_pointer(&json);
  group->refused_at = event_created_at(group->refused_json);
  group->refusal = cause == 2   ? GH_MLS_REFUSAL_UNFOLLOWABLE
                   : cause == 1 ? GH_MLS_REFUSAL_UNPROVEN
                                : GH_MLS_REFUSAL_BROKEN_PROOF;
}

/* A group ended for good keeps no record of its members (W24 review N3). */
static void
forget_group_records(GhMlsGroup *group)
{
  if (group->records_forgotten)
    return;
  group->records_forgotten = TRUE;
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_member_forget_group(group->service->store, group->gid_hex, &error))
    g_message("Groundhog could not forget an ended group's members: %s", error->message);
  if (group->refused_json) {
    g_clear_pointer(&group->refused_json, g_free);
    group->refusal = GH_MLS_REFUSAL_NONE;
    g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_CHANGE_REFUSED]);
  }
}

/* ---- Routing addresses (nostrc-ms4d) ----------------------------------------------------- */

/* "1", then one line per address: "<id> <epoch> <retired_at> <since> <url>...". */
static gchar *
addresses_serialize(GPtrArray *addresses)
{
  GString *out = g_string_new("1");
  for (guint i = 0; i < addresses->len; i++) {
    Address *a = g_ptr_array_index(addresses, i);
    g_string_append_printf(out, "\n%s %" G_GUINT64_FORMAT " %" G_GINT64_FORMAT " %" G_GINT64_FORMAT,
                           a->id, a->epoch, a->retired_at, a->since);
    for (guint k = 0; a->relays[k]; k++)
      g_string_append_printf(out, " %s", a->relays[k]);
  }
  return g_string_free(out, FALSE);
}

/* What addresses_serialize() wrote; anything malformed is skipped. */
static void
addresses_parse(GPtrArray *into, const gchar *record)
{
  g_auto(GStrv) lines = record ? g_strsplit(record, "\n", -1) : NULL;
  if (!lines || g_strcmp0(lines[0], "1") != 0)
    return;
  for (guint i = 1; lines[i] && into->len < MAX_GROUP_ADDRESSES; i++) {
    g_auto(GStrv) parts = g_strsplit(lines[i], " ", -1);
    guint n = g_strv_length(parts);
    if (n < 5 || n > 4 + MAX_GROUP_RELAYS || !lower_hex64(parts[0]))
      continue;
    gchar *end = NULL;
    guint64 epoch = g_ascii_strtoull(parts[1], &end, 10);
    if (*end)
      continue;
    gint64 retired_at = g_ascii_strtoll(parts[2], &end, 10);
    if (*end || retired_at <= 0)
      continue;
    gint64 since = g_ascii_strtoll(parts[3], &end, 10);
    if (*end || since < 0)
      continue;
    GPtrArray *urls = g_ptr_array_new();
    for (guint k = 4; k < n; k++)
      if (gh_relay_url_validate(parts[k], NULL) &&
          !g_ptr_array_find_with_equal_func(urls, parts[k], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(parts[k]));
    if (urls->len == 0) {
      g_ptr_array_unref(urls);
      continue;
    }
    Address *a = g_new0(Address, 1);
    g_strlcpy(a->id, parts[0], sizeof a->id);
    a->relays = sorted_strv(urls);
    a->epoch = epoch;
    a->retired_at = retired_at;
    a->since = since;
    g_ptr_array_add(into, a);
  }
}

static void
routing_save(GhMlsGroup *group)
{
  g_autofree gchar *record = group->addresses->len ? addresses_serialize(group->addresses) : NULL;
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_routing_save(group->service->store, group->gid_hex, record, &error))
    g_message("Groundhog could not keep an encrypted group's earlier addresses: %s",
              error->message);
}

static void
routing_load(GhMlsGroup *group)
{
  g_autofree gchar *record = NULL;
  g_autoptr(GError) error = NULL;
  if (!gh_store_mls_routing_load(group->service->store, group->gid_hex, &record, &error)) {
    g_message("Groundhog could not read an encrypted group's earlier addresses: %s",
              error->message);
    return;
  }
  g_ptr_array_set_size(group->addresses, 0);
  addresses_parse(group->addresses, record);
}

/* A Commit just changed the group's routing (MarmotMessageResult.commit.
 * routing_changed): what it left -- the address, or the relays the group no
 * longer lists at the same address -- is read a while longer. Called in the
 * Commit's store transaction, before the refresh moves group->relays on,
 * so the record lands with the Commit (a crash keeps both or neither). */
static void
routing_retire(GhMlsGroup *group, const MarmotMessageResult *result)
{
  GhMlsService *self = group->service;
  uint8_t current[32];
  char **signed_urls = NULL;
  size_t n_signed = 0;
  if (marmot_get_group_routing(self->marmot, &group->gid, current, &signed_urls, &n_signed, NULL,
                               NULL) != MARMOT_OK) {
    g_message("Groundhog could not read an encrypted group's new routing; its earlier address "
              "is not read any more");
    return;
  }
  g_autofree gchar *left_id = to_hex(result->commit.previous_nostr_group_id, 32);
  g_autofree gchar *now_id = to_hex(current, 32);
  gboolean same = g_str_equal(left_id, now_id);
  GPtrArray *left = g_ptr_array_new();
  for (guint i = 0; group->relays[i]; i++) {
    gboolean kept = FALSE;
    for (size_t k = 0; same && k < n_signed && !kept; k++)
      kept = g_strcmp0(signed_urls[k], group->relays[i]) == 0;
    if (!kept)
      g_ptr_array_add(left, g_strdup(group->relays[i]));
  }
  for (size_t k = 0; k < n_signed; k++)
    free(signed_urls[k]);
  free(signed_urls);
  if (left->len == 0) {
    g_ptr_array_unref(left);
    return;
  }
  Address *a = NULL;
  for (guint i = 0; i < group->addresses->len && !a; i++) {
    Address *b = g_ptr_array_index(group->addresses, i);
    if (g_str_equal(b->id, left_id))
      a = b;
  }
  if (a) {
    for (guint k = 0; a->relays[k]; k++)
      if (left->len < MAX_GROUP_RELAYS &&
          !g_ptr_array_find_with_equal_func(left, a->relays[k], g_str_equal, NULL))
        g_ptr_array_add(left, g_strdup(a->relays[k]));
    g_strfreev(a->relays);
  } else {
    if (group->addresses->len >= MAX_GROUP_ADDRESSES)
      g_ptr_array_remove_index(group->addresses, 0);   /* the oldest */
    a = g_new0(Address, 1);
    g_strlcpy(a->id, left_id, sizeof a->id);
    g_ptr_array_add(group->addresses, a);
    /* Late events there may be dated before the cursor: from it, less the
     * overlap, as a re-subscription reads. */
    gint64 since = group->cursor > 0 ? group->cursor - GH_MLS_SERVICE_CURSOR_OVERLAP : 0;
    a->since = group->floor > 0 ? MAX(since, group->floor) : MAX(since, 0);
  }
  a->relays = sorted_strv(left);
  MarmotGroup *updated = result->commit.updated_group;
  a->epoch = updated ? updated->epoch : group->epoch + 1;
  a->retired_at = now_s(self);
  routing_save(group);
}

static gboolean
strv_has(const gchar *const *strv, const gchar *s)
{
  return strv && g_strv_contains(strv, s);
}

/* Drops the addresses no longer read: the group is past their epochs'
 * reach, they aged out, libmarmot no longer routes them, or (a rotation
 * back) they are the current routing again. Returns whether any changed. */
static gboolean
routing_prune(GhMlsGroup *group, const uint8_t (*previous)[32], size_t n_previous)
{
  gint64 now = now_s(group->service);
  gboolean changed = FALSE;
  for (guint i = group->addresses->len; i-- > 0;) {
    Address *a = g_ptr_array_index(group->addresses, i);
    gboolean current = g_str_equal(a->id, group->nostr_hex);
    gboolean drop = group->epoch > a->epoch ||
                    now - a->retired_at >= GH_MLS_SERVICE_ROUTING_RETAIN_S;
    if (!drop && !current) {
      gboolean routed = FALSE;
      for (size_t k = 0; k < n_previous && !routed; k++) {
        g_autofree gchar *hex = to_hex(previous[k], 32);
        routed = g_str_equal(hex, a->id);
      }
      drop = !routed;
    }
    if (!drop && current) {
      GPtrArray *rest = g_ptr_array_new_with_free_func(g_free);
      for (guint k = 0; a->relays[k]; k++)
        if (!strv_has((const gchar *const *)group->relays, a->relays[k]))
          g_ptr_array_add(rest, g_strdup(a->relays[k]));
      if (rest->len == 0) {
        drop = TRUE;
      } else if (rest->len != g_strv_length(a->relays)) {
        g_ptr_array_set_free_func(rest, NULL);
        g_strfreev(a->relays);
        a->relays = sorted_strv(g_steal_pointer(&rest));
        changed = TRUE;
      }
      if (rest)
        g_ptr_array_unref(rest);
    }
    if (drop) {
      g_ptr_array_remove_index(group->addresses, i);
      changed = TRUE;
    }
  }
  if (changed)
    routing_save(group);
  return changed;
}

/* Every relay read: the current ones and the addresses'. */
static GStrv
read_relays_of(GhMlsGroup *group)
{
  GPtrArray *urls = g_ptr_array_new();
  for (guint i = 0; group->relays[i]; i++)
    g_ptr_array_add(urls, g_strdup(group->relays[i]));
  for (guint i = 0; i < group->addresses->len; i++) {
    Address *a = g_ptr_array_index(group->addresses, i);
    for (guint k = 0; a->relays[k]; k++)
      if (!g_ptr_array_find_with_equal_func(urls, a->relays[k], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(a->relays[k]));
  }
  return sorted_strv(urls);
}

/* Reads everything the group shows from libmarmot and notifies what changed.
 * A changed routing (relays or nostr group id) re-subscribes. */
static void
group_refresh(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  Marmot *m = self->marmot;
  MarmotGroup *g = NULL;
  if (marmot_get_group(m, &group->gid, &g) != MARMOT_OK || !g) {
    drop_stale_error(self);
    return;
  }
  GObject *object = G_OBJECT(group);
  g_object_freeze_notify(object);
  gboolean routing_changed = FALSE, members_changed = FALSE, name_changed = FALSE;
  g_autofree gchar *nostr_hex = to_hex(g->nostr_group_id, 32);
  if (g_strcmp0(nostr_hex, group->nostr_hex) != 0) {
    g_strlcpy(group->nostr_hex, nostr_hex, sizeof group->nostr_hex);
    routing_changed = TRUE;
  }
  if (g_strcmp0(group->name, g->name) != 0) {
    g_free(group->name);
    /* Normalise empty and whitespace-only names to NULL, matching WN's
     * name.trim().is_empty() in groups.rs (N1, review w26-wn-dms). */
    const gchar *trimmed = g->name;
    while (trimmed && g_ascii_isspace(*trimmed))
      trimmed++;
    group->name = g_strdup(trimmed && *trimmed ? g->name : NULL);
    name_changed = TRUE;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_NAME]);
  }
  if (g_strcmp0(group->description, g->description) != 0) {
    g_free(group->description);
    group->description = g_strdup(g->description);
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_DESCRIPTION]);
  }
  if (group->epoch != g->epoch) {
    group->epoch = g->epoch;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_EPOCH]);
  }
  gboolean active = g->state == MARMOT_GROUP_STATE_ACTIVE;
  GPtrArray *admins = g_ptr_array_new();
  gboolean admin = g->admin_count == 0; /* libmarmot: no admins, anyone */
  for (size_t i = 0; i < g->admin_count; i++) {
    gchar *hex = to_hex(g->admin_pubkeys[i], 32);
    admin |= g_str_equal(hex, self->account);
    g_ptr_array_add(admins, hex);
  }
  GStrv admin_list = sorted_strv(admins);
  if (!strv_equal((const gchar *const *)admin_list, (const gchar *const *)group->admins)) {
    g_strfreev(group->admins);
    group->admins = admin_list;
    members_changed = TRUE;
  } else {
    g_strfreev(admin_list);
  }
  if (group->is_admin != admin) {
    group->is_admin = admin;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_IS_ADMIN]);
  }
  marmot_group_free(g);

  uint8_t (*keys)[32] = NULL;
  size_t n_keys = 0;
  /* nostrc-2um6: members who asked to leave and are gone now. */
  g_autoptr(GPtrArray) left = g_ptr_array_new_with_free_func(g_free);
  if (marmot_get_group_members(m, &group->gid, &keys, &n_keys) == MARMOT_OK) {
    GPtrArray *members = g_ptr_array_new();
    for (size_t i = 0; i < n_keys; i++)
      g_ptr_array_add(members, to_hex(keys[i], 32));
    GStrv list = sorted_strv(members);
    if (!strv_equal((const gchar *const *)list, (const gchar *const *)group->members)) {
      g_strfreev(group->members);
      group->members = list;
      members_changed = TRUE;
      GHashTableIter it;
      gpointer key;
      g_hash_table_iter_init(&it, group->leavers);
      while (active && g_hash_table_iter_next(&it, &key, NULL)) {
        if (g_strv_contains((const gchar *const *)group->members, key))
          continue;
        g_ptr_array_add(left, g_strdup(key));
        g_hash_table_iter_remove(&it);
      }
    } else {
      g_strfreev(list);
    }
  }
  free(keys);
  members_changed |= refresh_devices(group);

  /* nostrc-xrya: why the group is not active. libmarmot keeps who removed
   * the account, and whether a competing Commit could still undo it; a
   * record it cannot read says only that the group ended (review N3). */
  GhMlsGroupEnd end = GH_MLS_GROUP_END_NONE;
  g_autofree gchar *removed_by = NULL;
  gboolean removal_final = FALSE;
  if (!active) {
    bool removed = false, final = false, left_group = false;
    uint8_t by[32];
    MarmotError rerr = marmot_get_group_removal(m, &group->gid, &removed, by, NULL, &final);
    if (rerr == MARMOT_OK && removed)
      rerr = marmot_get_group_left(m, &group->gid, &left_group);
    if (rerr != MARMOT_OK) {
      end = GH_MLS_GROUP_END_UNKNOWN;
    } else if (removed && left_group) {
      /* A member committed our SelfRemove (nostrc-2um6). */
      end = GH_MLS_GROUP_END_LEFT;
      removal_final = final;
    } else if (removed) {
      end = GH_MLS_GROUP_END_REMOVED;
      removed_by = to_hex(by, 32);
      removal_final = final;
    } else {
      end = GH_MLS_GROUP_END_LEFT_DEVICE;
    }
  }
  bool leave_waits = false;
  gboolean leaving = active && marmot_is_leaving(m, &group->gid, &leave_waits) == MARMOT_OK &&
                     leave_waits;
  MarmotLeaveKind leave_kind = MARMOT_LEAVE_SELF_REMOVE;
  if (leaving && marmot_can_self_remove(m, &group->gid, &leave_kind) == MARMOT_OK)
    group->leave_via_admin = leave_kind == MARMOT_LEAVE_REMOVE_REQUEST;
  if (group->leaving != leaving) {
    group->leaving = leaving;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_LEAVING]);
  }
  gboolean reactivated = active && !group->active &&
                         (group->end == GH_MLS_GROUP_END_REMOVED ||
                          group->end == GH_MLS_GROUP_END_LEFT);
  group->removal_final = removal_final;
  if (group->active != active) {
    group->active = active;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_ACTIVE]);
  }
  if (group->end != end) {
    group->end = end;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_END]);
  }
  if (g_strcmp0(group->removed_by, removed_by) != 0) {
    g_free(group->removed_by);
    group->removed_by = g_steal_pointer(&removed_by);
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_REMOVED_BY]);
  }

  /* Routing (nostrc-ms4d): an adopted group's address and relays are its
   * signed 0x8004 state (libmarmot does not rewrite the relay table on a
   * Commit); a legacy group's are its record's and its relay table. */
  GPtrArray *urls = g_ptr_array_new();
  uint8_t current_id[32];
  char **signed_urls = NULL;
  size_t n_signed = 0, n_previous = 0;
  uint8_t (*previous)[32] = NULL;
  MarmotError rerr = marmot_get_group_routing(m, &group->gid, current_id, &signed_urls,
                                              &n_signed, &previous, &n_previous);
  /* Known: an adopted group's routing, or a legacy group (UNSUPPORTED) or
   * one without MLS state, whose relays are the table's. Anything else (a
   * busy store) keeps what the group had: never the table's stale list,
   * and no earlier address dropped. */
  gboolean known = rerr == MARMOT_OK || rerr == MARMOT_ERR_UNSUPPORTED ||
                   rerr == MARMOT_ERR_GROUP_NOT_FOUND;
  if (rerr == MARMOT_OK) {
    for (size_t i = 0; i < n_signed; i++) {
      if (signed_urls[i] &&
          !g_ptr_array_find_with_equal_func(urls, signed_urls[i], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(signed_urls[i]));
      free(signed_urls[i]);
    }
    free(signed_urls);
  } else if (!known) {
    drop_stale_error(self);
    for (guint i = 0; group->relays[i]; i++)
      g_ptr_array_add(urls, g_strdup(group->relays[i]));
  } else {
    MarmotGroupRelay *relays = NULL;
    size_t n_relays = 0;
    if (marmot_get_group_relay_urls(m, &group->gid, &relays, &n_relays) == MARMOT_OK) {
      for (size_t i = 0; i < n_relays; i++) {
        if (relays[i].relay_url &&
            !g_ptr_array_find_with_equal_func(urls, relays[i].relay_url, g_str_equal, NULL))
          g_ptr_array_add(urls, g_strdup(relays[i].relay_url));
        free(relays[i].relay_url);
        marmot_group_id_free(&relays[i].mls_group_id);
      }
      free(relays);
    }
  }
  GStrv list = sorted_strv(urls);
  if (!strv_equal((const gchar *const *)list, (const gchar *const *)group->relays)) {
    g_strfreev(group->relays);
    group->relays = list;
    routing_changed = TRUE;
  } else {
    g_strfreev(list);
  }
  if (known)
    routing_changed |= routing_prune(group, (const uint8_t (*)[32])previous, n_previous);
  free(previous);
  GStrv read = read_relays_of(group);
  if (!strv_equal((const gchar *const *)read, (const gchar *const *)group->read_relays)) {
    g_strfreev(group->read_relays);
    group->read_relays = read;
    routing_changed = TRUE;
  } else {
    g_strfreev(read);
  }

  char *pending_json = NULL;
  gboolean pending = marmot_get_pending_commit(m, &group->gid, &pending_json, NULL) == MARMOT_OK &&
                     pending_json != NULL;
  free(pending_json);
  if (group->pending_commit != pending) {
    group->pending_commit = pending;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_PENDING_COMMIT]);
  }
  MarmotUnsentWelcome *unsent = NULL;
  size_t n_unsent = 0;
  if (marmot_get_unsent_welcomes(m, &group->gid, &unsent, &n_unsent) == MARMOT_OK) {
    marmot_unsent_welcomes_free(unsent, n_unsent);
    if (group->unsent_welcomes != n_unsent) {
      group->unsent_welcomes = (guint)n_unsent;
      g_object_notify_by_pspec(object, group_props[GROUP_PROP_UNSENT_WELCOMES]);
    }
  }
  drop_stale_error(self);
  g_object_thaw_notify(object);
  if (members_changed)
    g_signal_emit(group, group_signals[GROUP_SIGNAL_MEMBERS_CHANGED], 0);
  for (guint i = 0; i < left->len; i++)
    g_signal_emit(group, group_signals[GROUP_SIGNAL_MEMBER_LEFT], 0,
                  (const gchar *)g_ptr_array_index(left, i));
  verify_group(group);
  if (name_changed && group->active)
    group_list_room(group);
  if (!group->active) {
    drop_held(group);
    /* A removal that may still lose keeps listening for the winner. */
    if (!listening(group)) {
      group_unsubscribe(group);
      forget_group_records(group);
    }
  } else if (reactivated) {
    /* A winning Commit undid the removal (review B1): read again from the
     * cursor, which held while the group was ended. */
    group_subscribe(group);
  } else if (routing_changed && group->scopes->len > 0) {
    group_subscribe(group);
  }
}

static GhMlsGroup *
group_new(GhMlsService *self, const MarmotGroupId *gid)
{
  GhMlsGroup *group = g_object_new(GH_TYPE_MLS_GROUP, NULL);
  group->service = self;
  group->gid = marmot_group_id_new(gid->data, gid->len);
  group->gid_hex = to_hex(gid->data, gid->len);
  group->room_id = gh_message_mls_room_id(group->gid_hex);
  return group;
}

static GhMlsGroup *
find_group(GhMlsService *self, const MarmotGroupId *gid)
{
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    if (marmot_group_id_equal(&group->gid, gid))
      return group;
  }
  return NULL;
}

static GhMlsGroup *
find_group_hex(GhMlsService *self, const gchar *hex)
{
  for (guint i = 0; hex && i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    if (g_str_equal(group->gid_hex, hex))
      return group;
  }
  return NULL;
}

/* The group object of gid, made and listed when new. */
static GhMlsGroup *
ensure_group(GhMlsService *self, const MarmotGroupId *gid)
{
  GhMlsGroup *group = find_group(self, gid);
  if (group)
    return group;
  group = group_new(self, gid);
  MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_LEGACY;
  if (marmot_get_group_profile(self->marmot, gid, &profile) != MARMOT_OK)
    drop_stale_error(self);
  group->adopted = profile == MARMOT_GROUP_PROFILE_ADOPTED;
  load_refused(group);
  routing_load(group);
  group_refresh(group);
  g_ptr_array_add(self->groups, group);
  g_list_model_items_changed(G_LIST_MODEL(self), self->groups->len - 1, 0, 1);
  if (group->active)
    group_list_room(group);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *scope = cursor_scope(group);
  if (!gh_store_get_cursor(self->store, scope, "", &group->cursor, &error))
    g_message("Groundhog could not read an encrypted group's cursor: %s", error->message);
  g_autofree gchar *floor = floor_scope(group);
  gh_store_get_cursor(self->store, floor, "", &group->floor, NULL);
  g_signal_emit(self, signals[SIGNAL_GROUP_ADDED], 0, group);
  return group;
}

/* ---- Reading kind 445 ---------------------------------------------------------------- */

static void
group_unsubscribe(GhMlsGroup *group)
{
  for (guint i = 0; i < group->scopes->len; i++)
    gh_relay_scope_cancel(g_ptr_array_index(group->scopes, i));
  g_ptr_array_set_size(group->scopes, 0);
  g_hash_table_remove_all(group->url_scope);
  g_hash_table_remove_all(group->earlier_failures);
  if (group->retain_source) {
    gh_clock_source_remove(group->service->clock, group->retain_source);
    group->retain_source = 0;
  }
  /* Not read yet, so nothing moved the cursor past them: the next
   * subscription fetches them again. */
  g_queue_clear_full(&group->backfill, stored_free);
  group->backfill_bytes = 0;
  g_hash_table_remove_all(group->backfilling);
  if (group->quiet_source) {
    g_source_remove(group->quiet_source);
    group->quiet_source = 0;
  }
  g_hash_table_remove_all(group->settled);
  update_history_incomplete(group);
  group_set_read(group, GH_MLS_READ_IDLE);
}

/* Sets the read cursor to @cursor if that moves it forward, bounded
 * (review B1, M1, N6): never past now (a relay's or a member's clock is no
 * authority: a far-future cursor would put every later REQ's `since` past
 * everything, and any allowance ahead of now would eat into the overlap),
 * never past an event held unread or dropped unread this session (so a
 * re-subscribe fetches it again). */
static void
save_cursor(GhMlsGroup *group, gint64 cursor)
{
  gint64 now = now_s(group->service);
  cursor = MIN(cursor, now);
  for (GList *l = group->held.head; l; l = l->next) {
    gint64 at = ((Held *)l->data)->created_at;
    if (at > group->floor)   /* the join's own second is behind the cursor anyway */
      cursor = MIN(cursor, at);
  }
  if (group->pinned > 0)
    cursor = MIN(cursor, group->pinned);
  /* A refused Commit is fetched again after a restart (W24 review L2). */
  if (group->refused_json && group->refused_at > 0)
    cursor = MIN(cursor, group->refused_at);
  if (cursor <= group->cursor)
    return;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *scope = cursor_scope(group);
  if (gh_store_set_cursor(group->service->store, scope, "", cursor, &error))
    group->cursor = cursor;
  else
    g_message("Groundhog could not save an encrypted group's cursor: %s", error->message);
}

/* When the account joined (or made) the group: nothing before it can be of
 * a later epoch (review N4). Kept with the cursor. */
static void
set_floor(GhMlsGroup *group, gint64 floor)
{
  g_autofree gchar *scope = floor_scope(group);
  g_autoptr(GError) error = NULL;
  if (!gh_store_set_cursor(group->service->store, scope, "", MAX(floor, 1), &error))
    g_message("Groundhog could not save an encrypted group's join time: %s", error->message);
  group->floor = MAX(floor, 1);
}

typedef enum {
  EVENT_ACCEPTED,  /* a message admitted or a Commit applied: moves the cursor */
  EVENT_HELD,      /* not decryptable yet: a later epoch's, or not for us */
  EVENT_OTHER      /* rejected, a duplicate or our own echo: moves nothing */
} EventOutcome;

static EventOutcome process_event(GhMlsGroup *group, const gchar *event_json, const gchar *url,
                                  gboolean stored, Held *retry);

/* Keeps a kind 445 for a later epoch: once per event id; when the queue is
 * full the oldest one goes (never the new arrival silently), and the cursor
 * stays behind it so the next subscription fetches it again (review M1). */
static void
hold_event(GhMlsGroup *group, const gchar *id, const gchar *json, gint64 created_at,
           gboolean shown, gboolean awaits_proposal)
{
  if (!id || g_hash_table_contains(group->held_ids, id))
    return;
  if (g_queue_get_length(&group->held) >= GH_MLS_SERVICE_MAX_HELD) {
    Held *oldest = g_queue_pop_head(&group->held);
    g_hash_table_remove(group->held_ids, oldest->id);
    group->pinned = group->pinned ? MIN(group->pinned, oldest->created_at) : oldest->created_at;
    g_debug("Groundhog dropped the oldest unreadable group event (queue full)");
    held_free(oldest);
  }
  Held *held = g_new0(Held, 1);
  held->id = g_strdup(id);
  held->json = g_strdup(json);
  held->created_at = MIN(created_at, now_s(group->service));
  held->shown = shown;
  held->held_us = g_get_monotonic_time();
  held->awaits_proposal = awaits_proposal;
  held->epoch = group->epoch;
  g_queue_push_tail(&group->held, held);
  g_hash_table_add(group->held_ids, held->id);
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_UNREADABLE]);
  sync_decrypt_pending(group);
}

static gint
held_older_first(gconstpointer a, gconstpointer b)
{
  const Held *x = a, *y = b;
  return x->created_at < y->created_at ? -1 : x->created_at > y->created_at;
}

/* Held events, again after a Commit moved the group on (review N1, N2).
 *
 * A fixpoint, never recursion: a held Commit that applies during a pass
 * (process_event() -> after_commit() -> here) only asks for another pass,
 * and passes repeat until one applies no Commit. Each pass takes the held
 * events oldest first, so an epoch's messages are tried before the Commit
 * that closes it; a whole newest-first backlog resolves in one call. Once a
 * Commit applied, a pass ends with that Commit's own second (the rest waits
 * for the next pass, oldest first again). Inside one second the order is
 * the relay's, not the sender's: a message of the new epoch may have come
 * before the Commit, and reading the epoch's later seconds first would move
 * the sender's ratchet past libmarmot's window of skipped keys (32) before
 * it is tried again (nostrc-kzun); the rest of that second is still tried,
 * as it may hold the closing epoch's messages.
 *
 * Junk: anyone can post a kind 445 with the group's public h. An event
 * still unreadable once the group's epoch advanced more than Marmot's
 * max_rewind_commits past the one it was held at (GH_MLS_SERVICE_JUNK_AFTER_
 * EPOCHS) is dropped and no longer holds the cursor back: no branch it
 * could belong to can still be selected, nor its Commit staged (W25 slice N
 * re-review N1: counting Commits instead dropped a competing branch's events
 * that came a few Commits before their parent). Dropped, it stays eligible:
 * fetched again, it is held again (remember_junk()). A Commit refused for
 * capacity (MARMOT_ERR_RESOURCE_REFUSED) is held the same way: it is
 * offered again with every retry until it is retained or stale. */
static void
retry_held(GhMlsGroup *group)
{
  if (group->retrying) {
    group->retry_again = TRUE;
    return;
  }
  group->retrying = TRUE;
  gboolean touched = FALSE;
  do {
    group->retry_again = FALSE;
    if (g_queue_is_empty(&group->held) || !group->active)
      break;
    touched = TRUE;
    GList *pass = group->held.head;
    g_queue_init(&group->held);
    g_hash_table_remove_all(group->held_ids);
    pass = g_list_sort(pass, held_older_first);   /* stable: arrival order within a second */
    gint64 pass_ends = G_MAXINT64;                /* the second of a Commit applied */
    for (GList *l = pass; l; l = l->next) {
      Held *held = l->data;
      gint64 at = held->created_at;
      if (!group->active) {
        /* Ended mid-pass (a removal applied): nothing of it is held (N5). */
        held_free(held);
        continue;
      }
      if (at <= pass_ends) {
        gboolean before = group->retry_again;
        if (process_event(group, held->json, NULL, FALSE, held) != EVENT_HELD) {
          if (!before && group->retry_again)
            pass_ends = at;
          held_free(held);
          continue;
        }
      }
      /* Still held, or left for the next pass. */
      g_queue_push_tail(&group->held, held);
      g_hash_table_add(group->held_ids, held->id);
    }
    g_list_free(pass);
  } while (group->retry_again);
  gint64 now_us = g_get_monotonic_time();
  for (GList *l = group->held.head; l;) {
    GList *next = l->next;
    Held *held = l->data;
    /* A Commit still waiting for its proposal (review H1): one more try
     * counted per pass, and given up after a bounded count or time. */
    gboolean gave_up = FALSE;
    if (held->awaits_proposal && touched) {
      held->proposal_tries++;
      gave_up = held->proposal_tries >= GH_MLS_SERVICE_PROPOSAL_WAIT_TRIES ||
                now_us - held->held_us >= (gint64)GH_MLS_SERVICE_PROPOSAL_WAIT_S * G_USEC_PER_SEC;
    }
    gboolean aged = group->epoch > held->epoch + GH_MLS_SERVICE_JUNK_AFTER_EPOCHS - 1;
    if (gave_up || aged) {
      remember_junk(group, held->id);
      g_hash_table_remove(group->held_ids, held->id);
      g_queue_delete_link(&group->held, l);
      held_free(held);
      touched = TRUE;
    }
    l = next;
  }
  group->retrying = FALSE;
  if (touched)
    g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_UNREADABLE]);
  sync_decrypt_pending(group);
}

static gboolean
retained_retry_due(gpointer data)
{
  GhMlsGroup *group = data;
  group->retained_source = 0;
  group->retained_retry_us = g_get_monotonic_time();
  retry_held(group);
  return G_SOURCE_REMOVE;
}

/* A Commit was retained as a losing candidate (MARMOT_ERR_COMMIT_RETAINED,
 * W25 review H2): the decryption context changed -- its state's exporter
 * secret may open held events of its branch -- so they are offered again
 * (inbound-processing.md: a transport-deferred object is retried whenever
 * the candidate-key set changes). Inside a retry pass that is the fixpoint's
 * next pass; for Commits arriving from relays the retries are coalesced to
 * one per GH_MLS_SERVICE_RETAINED_RETRY_MS (review M1: a flood of losing
 * Commits does not buy a pass over the whole queue each). */
static void
retry_held_retained(GhMlsGroup *group, gboolean fresh)
{
  if (!fresh || group->retrying) {
    retry_held(group);
    return;
  }
  if (group->retained_source)
    return;   /* one is due already */
  gint64 now = g_get_monotonic_time();
  gint64 wait_us = group->retained_retry_us + (gint64)GH_MLS_SERVICE_RETAINED_RETRY_MS * 1000 - now;
  if (group->retained_retry_us == 0 || wait_us <= 0) {
    group->retained_retry_us = now;
    retry_held(group);
    return;
  }
  group->retained_source = g_timeout_add((guint)(wait_us / 1000) + 1, retained_retry_due, group);
}

static void after_commit(GhMlsGroup *group);
static void queued_flush(GhMlsGroup *group);
static void departures_schedule(GhMlsGroup *group);
static gchar *event_id_of(const gchar *json);

/* Whether @envelope_id is the account's own Commit that libmarmot holds
 * pending for a relay's OK. Its echo from a relay merges it, and libmarmot
 * then reports no committer (nostrc-juhs). Asked only while a Commit of ours
 * is out: the read loads the group's MLS state (W25 review L2), and
 * round_start() sets group->round before publishing, while group_refresh()
 * keeps pending_commit across an unanswered round and a restart. */
static gboolean
own_pending_commit(GhMlsGroup *group, const gchar *envelope_id)
{
  if (!envelope_id || !(group->round || group->pending_commit))
    return FALSE;
  char *pending_json = NULL;
  if (marmot_get_pending_commit(group->service->marmot, &group->gid, &pending_json, NULL) !=
      MARMOT_OK) {
    drop_stale_error(group->service);
    return FALSE;
  }
  g_autofree gchar *pending_id = pending_json ? event_id_of(pending_json) : NULL;
  free(pending_json);
  return pending_id && g_str_equal(pending_id, envelope_id);
}
static void upgrade_schedule(GhMlsGroup *group);
static void upgrade_cancel(GhMlsGroup *group);

/* Every relay of the subscription sent its EOSE, fully paged, and is still
 * connected (value 1), apart from URLs that could never be subscribed
 * (value 3). A relay that failed during the catch-up, or whose paging could
 * not fetch everything (value 4, nostrc-cpwf), may hold events the others
 * lack: the cursor must not pass them. */
static gboolean
all_relays_answered(GhMlsGroup *group)
{
  /* The current routing's relays only (review L2): an earlier address has
   * its own fixed `since`, and a dead relay the group dropped must not hold
   * the group's cursor back for the days it is still read. */
  if (!group->relays || !group->relays[0])
    return FALSE;
  for (guint i = 0; group->relays[i]; i++) {
    gint state = GPOINTER_TO_INT(g_hash_table_lookup(group->settled, group->relays[i]));
    if (state != 1 && state != 3)
      return FALSE;
  }
  return TRUE;
}

/* Every current relay has answered or failed (the group's read state). */
static gboolean
current_relays_settled(GhMlsGroup *group)
{
  if (!group->relays || !group->relays[0])
    return FALSE;
  for (guint i = 0; group->relays[i]; i++)
    if (!g_hash_table_contains(group->settled, group->relays[i]))
      return FALSE;
  return TRUE;
}

/* nostrc-xrza (W25 review M3): the messages a branch change withdrew, by
 * the inner event ids the conversation keys them by. libmarmot reports
 * their kind 445s; GhStoreMarmot keeps the inner event's id for each
 * (gh-store-marmot.h, "Messages keep no plaintext"). */
static GPtrArray *
withdrawn_messages(GhMlsGroup *group, const MarmotMessageResult *result)
{
  GhMlsService *self = group->service;
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  MarmotStorage *st = self->storage;
  for (size_t i = 0; st && st->find_message_by_id && i < result->convergence.invalidated_count;
       i++) {
    const gchar *hex = result->convergence.invalidated_message_ids[i];
    uint8_t id[32];
    if (!hex || strlen(hex) != 64)
      continue;
    gboolean ok = TRUE;
    for (guint b = 0; b < 32 && ok; b++) {
      gint hi = g_ascii_xdigit_value(hex[2 * b]), lo = g_ascii_xdigit_value(hex[2 * b + 1]);
      ok = hi >= 0 && lo >= 0;
      id[b] = (uint8_t)((hi << 4) | lo);
    }
    MarmotMessage *stored = NULL;
    if (!ok || st->find_message_by_id(st->ctx, id, &stored) != MARMOT_OK || !stored) {
      drop_stale_error(self);
      continue;
    }
    const gchar *inner = stored->content;
    gboolean is_id = inner && strlen(inner) == 64;
    for (guint b = 0; is_id && b < 64; b++)
      is_id = g_ascii_isdigit(inner[b]) || (inner[b] >= 'a' && inner[b] <= 'f');
    if (is_id)
      g_ptr_array_add(ids, g_strdup(inner));
    marmot_message_free(stored);
  }
  return ids;
}

static gboolean
strv_same(GStrv a, GStrv b)
{
  static const gchar *const none[] = { NULL };
  return g_strv_equal(a ? (const gchar *const *)a : none, b ? (const gchar *const *)b : none);
}

/* What the branch change undid of the group's state, from before it. */
static guint
undone_since(GhMlsGroup *group, const gchar *name, GStrv members, GStrv admins)
{
  guint undone = GH_MLS_UNDONE_NONE;
  if (g_strcmp0(name, group->name) != 0)
    undone |= GH_MLS_UNDONE_NAME;
  if (!strv_same(members, group->members))
    undone |= GH_MLS_UNDONE_MEMBERS;
  if (!strv_same(admins, group->admins))
    undone |= GH_MLS_UNDONE_ADMINS;
  return undone;
}

/* One kind-445 envelope from a group relay: libmarmot's relay path (id and
 * signature first) and, for a chat message, its admission, in one
 * transaction. @retry: the held record when this is a retry (it is not held
 * again here; the caller decides). */
static EventOutcome
process_event(GhMlsGroup *group, const gchar *event_json, const gchar *url, gboolean stored,
              Held *retry)
{
  GhMlsService *self = group->service;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not read an encrypted group message: %s", error->message);
    return EVENT_OTHER;
  }
  gint64 created_at = 0;
  NostrEvent *envelope = nostr_event_new();
  g_autofree gchar *envelope_id = NULL;
  if (envelope && nostr_event_deserialize_compact(envelope, event_json, NULL) == 1) {
    created_at = nostr_event_get_created_at(envelope);
    {
      char *raw_id = nostr_event_get_id(envelope); /* malloc'd; nostrc-kdxe */
      envelope_id = raw_id ? g_strdup(raw_id) : NULL;
      free(raw_id);
    }
  }
  if (envelope)
    nostr_event_free(envelope);
  gboolean own_commit = own_pending_commit(group, envelope_id);
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  MarmotError err = marmot_process_message(self->marmot, event_json, &result);
  gboolean commit = FALSE, held = FALSE, accepted = FALSE, check_final = FALSE, refused = FALSE;
  GhMlsRefusal refusal = GH_MLS_REFUSAL_BROKEN_PROOF;   /* or UNPROVEN: set_refused() */
  gboolean proposal = FALSE, awaits_proposal = FALSE;
  gboolean retained = FALSE;

  if (err == MARMOT_OK && result.type == MARMOT_RESULT_APPLICATION_MESSAGE) {
    g_autoptr(GError) bad = NULL;
    g_autoptr(GhMessage) message =
      gh_message_new_from_mls(self->account, group->gid_hex, result.app_msg.inner_event_json,
                              &bad);
    /* libmarmot authenticated the author; the model checks it again. */
    if (message && g_strcmp0(gh_message_get_sender(message),
                             result.app_msg.sender_pubkey_hex) != 0)
      g_clear_object(&message);
    /* Its files: display data, and the epoch libmarmot authenticated for it
     * (never a tag), which opening them needs. */
    if (message)
      gh_mls_imeta_describe(message, group->gid_hex, TRUE, result.app_msg.epoch);
    if (message && account_matches_model(self)) {
      if (url)
        gh_message_add_relay(message, url);
      GhConversationAddResult added =
        gh_conversation_store_admit(self->conversations, message, envelope_id, &error);
      if (added == GH_CONVERSATION_ADD_FAILED || added == GH_CONVERSATION_ADD_REJECTED) {
        /* Not stored: nothing of it may be (its ratchet step included). */
        g_message("Groundhog could not store an encrypted group message: %s",
                  error ? error->message : "refused");
        gh_store_rollback(self->store);
        marmot_message_result_free(&result);
        return EVENT_OTHER;
      }
    }
    /* NIP-88 polls (nostrc-a36s): track poll definitions and votes. */
    if (message) {
      gint inner_kind = gh_message_get_kind(message);
      if (inner_kind == GH_MLS_POLL_KIND) {
        g_autoptr(GError) poll_err = NULL;
        GhMlsPoll *poll = gh_mls_poll_new_from_event(
          gh_message_get_rumor_id(message), result.app_msg.sender_pubkey_hex,
          gh_message_get_created_at(message), result.app_msg.inner_event_json,
          &poll_err);
        if (poll) {
          gh_mls_poll_set_local_account(poll, self->account);
          g_autofree gchar *key = g_strdup_printf("%s:%s", group->gid_hex,
                                                  gh_message_get_rumor_id(message));
          g_hash_table_replace(self->polls, g_steal_pointer(&key), poll);
        } else {
          g_message("Groundhog: malformed poll in group %s: %s", group->gid_hex,
                    poll_err ? poll_err->message : "unknown");
        }
      } else if (inner_kind == GH_MLS_POLL_VOTE_KIND) {
        g_autoptr(GError) vote_err = NULL;
        g_autofree gchar *target_id = NULL;
        g_auto(GStrv) option_ids = NULL;
        if (gh_mls_poll_parse_vote(result.app_msg.inner_event_json,
                                   &target_id, &option_ids, &vote_err)) {
          g_autofree gchar *key = g_strdup_printf("%s:%s", group->gid_hex, target_id);
          GhMlsPoll *poll = g_hash_table_lookup(self->polls, key);
          if (poll) {
            /* F3: reject votes whose created_at is outside the poll's
             * lifetime, as MDK's validate_poll_response does. */
            gint64 vote_ts = gh_message_get_created_at(message);
            if (gh_mls_poll_is_open(poll, vote_ts) &&
                vote_ts >= gh_mls_poll_get_created_at(poll))
              gh_mls_poll_apply_vote(poll, result.app_msg.sender_pubkey_hex,
                                     (const gchar **) option_ids, vote_ts);
          }
        }
      }
    }
    /* W26 slice B (nostrc-191r): kind-7 reaction or kind-5 deletion.
     * The inner event is an unsigned rumor: parse it to check its kind,
     * and route to the reaction store when present. */
    if (!message && self->reactions && result.app_msg.inner_event_json) {
      NostrEvent *inner = nostr_event_new();
      if (inner && nostr_event_deserialize_unsigned(inner, result.app_msg.inner_event_json,
                                                     NULL) == NOSTR_EVENT_VALIDATION_OK) {
        int inner_kind = nostr_event_get_kind(inner);
        if (inner_kind == 7) {
          /* NIP-25 reaction: e-tag is the target message, content is emoji. */
          NostrTags *tags = (NostrTags *)nostr_event_get_tags(inner);
          const gchar *target_id = NULL;
          if (tags) {
            for (size_t ti = 0; ti < nostr_tags_size(tags); ti++) {
              NostrTag *tag = nostr_tags_get(tags, ti);
              if (tag && g_strcmp0(nostr_tag_get_key(tag), "e") == 0 &&
                  nostr_tag_get_value(tag)) {
                target_id = nostr_tag_get_value(tag);
                break;   /* MDK v0.11: one e-tag */
              }
            }
          }
          if (target_id && result.app_msg.sender_pubkey_hex) {
            const gchar *emoji = nostr_event_get_content(inner);
            if (!emoji || !*emoji)
              emoji = "+";
            gchar rumor_id[65] = { 0 };
            if ((inner->id ? nostr_event_validate_id(inner, rumor_id)
                           : nostr_event_compute_id(inner, rumor_id)) ==
                              NOSTR_EVENT_VALIDATION_OK) {
              g_autoptr(GhReaction) reaction =
                gh_reaction_new(target_id, rumor_id,
                                result.app_msg.sender_pubkey_hex,
                                emoji, nostr_event_get_created_at(inner),
                                group->room_id);
              if (reaction)
                gh_reaction_store_admit(self->reactions, reaction, NULL);
            }
          }
        } else if (inner_kind == 5) {
          /* NIP-25 deletion: each e-tag names a reaction to remove. */
          NostrTags *tags = (NostrTags *)nostr_event_get_tags(inner);
          if (tags) {
            for (size_t ti = 0; ti < nostr_tags_size(tags); ti++) {
              NostrTag *tag = nostr_tags_get(tags, ti);
              if (tag && g_strcmp0(nostr_tag_get_key(tag), "e") == 0 &&
                  nostr_tag_get_value(tag))
                gh_reaction_store_remove(self->reactions, nostr_tag_get_value(tag), NULL);
            }
          }
        }
      }
      if (inner)
        nostr_event_free(inner);
    }
    accepted = TRUE;
    /* W24b slice H re-review R4: a member's message of the epoch a refused
     * Commit would have ended, dated after that Commit, says the member did
     * not follow it either. */
    if (group->refused_json && result.app_msg.sender_pubkey_hex &&
        result.app_msg.epoch == group->epoch && created_at > group->refused_at &&
        g_strcmp0(result.app_msg.sender_pubkey_hex, self->account) != 0)
      g_hash_table_add(group->refusal_witnesses, g_strdup(result.app_msg.sender_pubkey_hex));
  } else if (err == MARMOT_OK && result.type == MARMOT_RESULT_COMMIT) {
    commit = accepted = TRUE;
    /* Who left on their own request, proposal seen or not (review L4):
     * "member-left" once the refresh finds them gone. */
    for (size_t i = 0; i < result.commit.departed_count; i++)
      if (g_strcmp0(result.commit.departed_pubkey_hexes[i], self->account) != 0)
        g_hash_table_add(group->leavers, g_strdup(result.commit.departed_pubkey_hexes[i]));
    /* Re-review R1: an admin's Commit while our Remove request waits
     * (group->admins still lists the admins it was judged against). */
    if (group->leaving && group->leave_via_admin && result.commit.committer_pubkey_hex &&
        g_strv_contains((const gchar *const *)group->admins, result.commit.committer_pubkey_hex))
      group->leave_admin_commit = TRUE;
    /* Who added the devices the Commit brings (nostrc-6ukh). Our own,
     * merged on its relay echo before the relay's OK, is ours, as when that
     * OK merges it (round_report(); nostrc-juhs). */
    g_free(group->last_committer);
    if (!result.commit.committer_pubkey_hex && own_commit) {
      group->last_committer = g_strdup(self->account);
      group->last_committer_leaf = G_MAXUINT32;   /* our own leaf is proven */
    } else {
      group->last_committer = g_strdup(result.commit.committer_pubkey_hex);
      group->last_committer_leaf = result.commit.committer_pubkey_hex
                                     ? result.commit.committer_leaf : G_MAXUINT32;
    }
    /* nostrc-ms4d: what a routing change left is still read a while. */
    if (result.commit.routing_changed)
      routing_retire(group, &result);
  } else if (err == MARMOT_OK && result.type == MARMOT_RESULT_PROPOSAL) {
    /* nostrc-2um6: a member's standalone proposal, which libmarmot keeps for
     * the Commit that references it; a leave is committed after a delay. */
    proposal = accepted = TRUE;
    if (result.proposal.leave && result.proposal.sender_pubkey_hex)
      g_hash_table_add(group->leavers, g_strdup(result.proposal.sender_pubkey_hex));
  } else if (err == MARMOT_ERR_KEY_PACKAGE_IDENTITY && group->active) {
    /* An authenticated admin Commit libmarmot will never apply: it adds a
     * member whose proof is forged, or (the account requiring proofs) has
     * none (nostrc-prrl). It is not something to wait for. */
    refused = TRUE;
  } else if (err == MARMOT_ERR_COMMIT_REFUSED && group->active) {
    /* An adopted group's admin's Commit libmarmot refuses for good: one it
     * cannot follow as MDK does (a disband, say) or one breaking the
     * group's rules (W24b slice H review L2). A non-admin's junk keeps its
     * own error and marks nothing. */
    refused = TRUE;
    refusal = GH_MLS_REFUSAL_UNFOLLOWABLE;
  } else if (err == MARMOT_ERR_COMMIT_RETAINED) {
    /* nostrc-w1m0 (W25 review H2): a valid Commit retained as a losing
     * candidate. It is processed (the cursor may pass it) and the group is
     * unchanged, but its state's exporter secret may open held events. */
    accepted = retained = TRUE;
  } else if (err == MARMOT_ERR_RESOURCE_REFUSED) {
    /* W25 review H1: a losing Commit libmarmot has no room to retain now.
     * Not invalid: held and offered again with every retry, until it is
     * retained or stale (transports/nostr.md "resource_refused"). */
    held = group->active;
  } else if (err == MARMOT_ERR_NIP44) {
    /* A later epoch's (or not for us): wait for the Commit that opens it. */
    held = group->active;   /* an ended group holds nothing (review N5) */
  } else if (err == MARMOT_ERR_PROPOSAL_UNKNOWN) {
    /* nostrc-2um6 review H1: a Commit citing a proposal not received yet
     * (one second, relay order): kept, and offered again when a proposal
     * arrives; meanwhile no departure Commit of ours competes with it. */
    held = awaits_proposal = group->active;
  } else if (err == MARMOT_ERR_USE_AFTER_EVICTION && listening(group)) {
    /* Removed by a Commit that may still lose: nothing but that epoch's
     * Commits is read. libmarmot counts the later-epoch events it cannot
     * open and makes the removal final after a few (W22 review B2): then
     * the group stops listening. The cursor does not move while the group
     * is ended (nothing is accepted), and is not pinned either: once
     * final, nothing is fetched again. */
    check_final = TRUE;
  } else if (err != MARMOT_OK) {
    g_debug("Groundhog skipped an encrypted group event: %s", marmot_error_string(err));
  }
  /* nostrc-w1m0: libmarmot made another branch of the group canonical while
   * processing this event -- a competing Commit, or a message witnessing a
   * competing branch: the group changed as after a Commit. */
  g_autoptr(GPtrArray) withdrawn = NULL;
  gboolean recovered = err == MARMOT_OK && result.convergence.branch_recovered;
  if (recovered) {
    commit = TRUE;
    /* nostrc-xrza: its messages that the group withdrew are marked, in
     * the same transaction (convergence.md: a change that lost branch
     * selection must not stay visible as completed). */
    withdrawn = withdrawn_messages(group, &result);
    for (guint i = 0; i < withdrawn->len; i++)
      if (!gh_store_mls_mark_withdrawn(self->store, group->gid_hex, withdrawn->pdata[i],
                                       &error)) {
        g_message("Groundhog could not mark a withdrawn group message: %s", error->message);
        gh_store_rollback(self->store);
        marmot_message_result_free(&result);
        return EVENT_OTHER;
      }
  }
  marmot_message_result_free(&result);
  /* libmarmot rolled a failed operation back itself; its deliberate
   * outcomes (a deferred competing Commit) are kept. */
  if (!gh_store_commit(self->store, &error)) {
    g_message("Groundhog could not store an encrypted group event: %s", error->message);
    return EVENT_OTHER;
  }
  if (refused) {
    set_refused(group, event_json, refusal);
    return EVENT_OTHER;
  }
  /* Every other member refused it too: the group never moved past it, and
   * nothing is left to fetch again behind it (re-review R4). */
  if (group->refused_json && refused_by_everyone(group))
    set_refused(group, NULL, GH_MLS_REFUSAL_NONE);
  if (check_final) {
    bool removed = false, final = false;
    if (marmot_get_group_removal(self->marmot, &group->gid, &removed, NULL, NULL, &final) ==
          MARMOT_OK && removed && final)
      group_refresh(group);   /* final: the subscription closes */
    return EVENT_OTHER;
  }
  if (held) {
    /* Before the account joined, nothing can be of a later epoch: it is of
     * one the account never had (the Add Commit that admitted it, say), and
     * is not kept (review N4). One in the join's own second may be either
     * (the Add Commit, or a later epoch's in a fast group): it is kept, but
     * never holds the cursor back (save_cursor()). */
    if (group->floor > 0 && created_at < group->floor)
      return EVENT_OTHER;
    /* Aged out before: held again, silently (remember_junk()). */
    gboolean aged = envelope_id && g_hash_table_contains(group->junk_ids, envelope_id);
    if (!retry)
      hold_event(group, envelope_id, event_json, created_at,
                 !aged && !(stored && group->floor > 0 && created_at <= group->floor),
                 awaits_proposal);
    else if (awaits_proposal)
      retry->awaits_proposal = TRUE;
    return EVENT_HELD;
  }
  if (!accepted)
    return EVENT_OTHER;   /* rejected, duplicate, own echo: no clock of ours */
  /* Only what libmarmot accepted and the store kept moves the cursor, and
   * never past now (review B1, N6), and only while every group relay has
   * answered (a relay that failed may hold what the others lack). */
  gint64 bounded = MIN(created_at, now_s(self));
  group->newest = MAX(group->newest, bounded);
  if (group->read == GH_MLS_READ_LIVE && all_relays_answered(group))
    save_cursor(group, bounded);
  if (withdrawn && account_matches_model(self))
    for (guint i = 0; i < withdrawn->len; i++) {
      GhMessage *shown = gh_conversation_store_lookup_message(self->conversations,
                                                              withdrawn->pdata[i]);
      if (shown && gh_message_is_mls(shown) &&
          g_strcmp0(gh_message_get_group_id(shown), group->gid_hex) == 0)
        gh_message_set_withdrawn(shown, TRUE);
    }
  /* F5: clear in-memory poll state for withdrawn messages so tallies stay
   * consistent within the session (on restart, rebuild_polls rescans and
   * the withdrawn messages are absent). */
  if (withdrawn)
    for (guint i = 0; i < withdrawn->len; i++) {
      const gchar *rumor_id = withdrawn->pdata[i];
      g_autofree gchar *poll_key = g_strdup_printf("%s:%s", group->gid_hex, rumor_id);
      /* A withdrawn poll: remove the whole projection. */
      if (g_hash_table_remove(self->polls, poll_key))
        continue;
      /* A withdrawn vote: find the poll it targeted and remove the voter.
       * The voter's pubkey is not in the rumor id, so scan each poll in
       * this group for a voter whose record references this rumor. This is
       * O(polls * voters) but withdrawals are rare (convergence events). */
      GHashTableIter poll_iter;
      g_hash_table_iter_init(&poll_iter, self->polls);
      const gchar *pk;
      GhMlsPoll *p;
      while (g_hash_table_iter_next(&poll_iter, (gpointer *) &pk, (gpointer *) &p)) {
        if (!g_str_has_prefix(pk, group->gid_hex))
          continue;
        /* Rebuild is simpler: just mark dirty and let the next rebuild
         * pass fix it.  But we can do better: the store still has all
         * non-withdrawn messages, and rebuild_polls runs on restart
         * anyway.  For now, just signal that tallies may have changed
         * so the UI refreshes. */
        g_signal_emit_by_name(p, "tallies-changed");
      }
    }
  if (commit) {
    g_autofree gchar *name_before = recovered ? g_strdup(group->name) : NULL;
    g_auto(GStrv) members_before = recovered ? g_strdupv(group->members) : NULL;
    g_auto(GStrv) admins_before = recovered ? g_strdupv(group->admins) : NULL;
    g_object_ref(group);
    after_commit(group);
    if (recovered)
      g_signal_emit(group, group_signals[GROUP_SIGNAL_CONFLICT_RESOLVED], 0,
                    withdrawn ? withdrawn->len : 0u,
                    undone_since(group, name_before, members_before, admins_before));
    g_object_unref(group);
  } else if (retained) {
    retry_held_retained(group, retry == NULL);
  } else if (proposal) {
    /* A held Commit may have waited for this proposal (review H1). */
    retry_held(group);
    departures_schedule(group);
  }
  return EVENT_ACCEPTED;
}

static gint
stored_older_first(gconstpointer a, gconstpointer b)
{
  const Stored *x = a, *y = b;
  if (x->created_at != y->created_at)
    return x->created_at < y->created_at ? -1 : 1;
  /* Within a second the relay sent newest first: undo its order. */
  return x->seq > y->seq ? -1 : x->seq < y->seq;
}

/* Relays answer newest first, and libmarmot keeps only a few skipped
 * message keys per sender (MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS): a
 * backlog of more than that from one sender, applied newest first, would
 * leave the older messages undecryptable for good. Backfill is therefore
 * stored and applied oldest first, as the held queue is (nostrc-cpwf).
 *
 * Across relays (review B3): the scope deduplicates by id for all group
 * relays at once, so each event arrives under whichever relay sent it
 * first, and one relay's share of a backlog has arbitrary gaps. What is
 * stored is applied as one set, from every relay, and only once no relay is
 * still delivering a backfill round (group->backfilling): a relay may hold
 * older events the others lack, and applying the rest first would push the
 * ratchet past them. A relay that has sent nothing yet holds nothing up,
 * and one that went silent holds it only for the quiet period
 * (backfill_quiet()). */
static void
flush_backfill(GhMlsGroup *group)
{
  GList *mine = group->backfill.head;
  g_queue_init(&group->backfill);
  group->backfill_bytes = 0;
  mine = g_list_sort(mine, stored_older_first);
  g_object_ref(group);   /* a Commit may change the subscription meanwhile */
  for (GList *l = mine; l; l = l->next) {
    Stored *stored = l->data;
    if (listening(group))
      process_event(group, stored->json, stored->url, stored->stored, NULL);
  }
  g_list_free_full(mine, stored_free);
  g_object_unref(group);
}

/* history-incomplete: some relay of this subscription ended its backfill
 * incomplete (settled 4): paging failed, ran out, or the store was full. */
static void
update_history_incomplete(GhMlsGroup *group)
{
  gboolean incomplete = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->settled);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    incomplete |= GPOINTER_TO_INT(value) == 4;
  if (incomplete == group->history_incomplete)
    return;
  group->history_incomplete = incomplete;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_HISTORY_INCOMPLETE]);
}

/* The store is full (review B4): a relay ignoring limit, a page that never
 * ends, or live traffic during a long backfill. Every relay still delivering
 * a backfill round stops paging and counts as answered-incomplete (the
 * cursor holds, history-incomplete), and what is stored is applied oldest
 * first; later events of those connections are applied as they come. What
 * is stored is the newest part of the backlog (relays answer newest first),
 * so a sender's older messages may then lie outside libmarmot's window and
 * stay unreadable although they are fetched again. The bound is what one
 * honest paging round can deliver: reaching it means a relay ignoring its
 * limits, or a backlog beyond the page budget. */
static void
give_up_backfill(GhMlsGroup *group)
{
  GHashTableIter iter;
  gpointer url;
  g_hash_table_iter_init(&iter, group->backfilling);
  while (g_hash_table_iter_next(&iter, &url, NULL)) {
    GhRelayScope *scope = g_hash_table_lookup(group->url_scope, url);
    if (scope)
      gh_relay_scope_end_backfill(scope, url);
    g_hash_table_insert(group->settled, g_strdup(url), GINT_TO_POINTER(4));
  }
  g_hash_table_remove_all(group->backfilling);
  update_history_incomplete(group);
  if (current_relays_settled(group))
    group_set_read(group, GH_MLS_READ_LIVE);
  flush_backfill(group);
}

static void
backfill_full(GhMlsGroup *group)
{
  g_message("Groundhog stopped reading an encrypted group's history after %u events: it keeps "
            "no more at once; the group's read cursor stays where it was",
            g_queue_get_length(&group->backfill));
  give_up_backfill(group);
}

/* Final review N1: a relay that delivered some of a backfill round and then
 * went silent (a page or the live REQ never answered: a relay dropping REQs
 * over a limit without CLOSED, a hung query, on purpose) would hold every
 * stored and live event of the group for good, since the scope has no
 * timeouts. Once every relay still holding the flush has been silent for
 * the quiet period -- so any relay still delivering is waited for, and a
 * silent one is given up only when the others have finished -- they are
 * given up as backfill_full() does: end_backfill, answered-incomplete
 * (cursor held, history-incomplete), the store applied. Nothing is closed:
 * their subscriptions stay open and later events are applied live. Only the
 * local wait for ordering is bounded. */
static gboolean
backfill_quiet(gpointer data)
{
  GhMlsGroup *group = data;
  group->quiet_source = 0;
  if (g_hash_table_size(group->backfilling) == 0)
    return G_SOURCE_REMOVE;
  gint64 now = g_get_monotonic_time(), last = G_MININT64;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, group->backfilling);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    last = MAX(last, *(gint64 *)value);
  gint64 quiet = group->service->backfill_quiet_us;
  if (now - last < quiet) {    /* someone is still delivering: wait for them */
    group->quiet_source = g_timeout_add(MAX((quiet - (now - last)) / 1000, 1), backfill_quiet,
                                        group);
    return G_SOURCE_REMOVE;
  }
  g_message("Groundhog stopped waiting for a group relay that went silent during the group's "
            "history; the group's read cursor stays where it was");
  g_object_ref(group);
  give_up_backfill(group);
  g_object_unref(group);
  return G_SOURCE_REMOVE;
}

/* @url delivered a backfill event now: it holds the flush, for a while. */
static void
touch_backfilling(GhMlsGroup *group, const gchar *url)
{
  gint64 *last = g_hash_table_lookup(group->backfilling, url);
  if (!last) {
    last = g_new(gint64, 1);
    g_hash_table_insert(group->backfilling, g_strdup(url), last);
  }
  *last = g_get_monotonic_time();
  if (!group->quiet_source)
    group->quiet_source = g_timeout_add(MAX(group->service->backfill_quiet_us / 1000, 1),
                                        backfill_quiet, group);
}

static void
keep_backfill(GhMlsGroup *group, const GhRelayUpdate *update)
{
  Stored *stored = g_new0(Stored, 1);
  stored->json = g_strdup(update->event_json);
  stored->url = g_strdup(update->url);
  stored->created_at = G_MAXINT64;
  stored->seq = group->backfill_seq++;
  stored->stored = update->stored;   /* a stored answer, not live traffic (review N4) */
  NostrEvent *event = nostr_event_new();
  if (event && nostr_event_deserialize_compact(event, update->event_json, NULL) == 1)
    stored->created_at = nostr_event_get_created_at(event);
  if (event)
    nostr_event_free(event);
  g_queue_push_tail(&group->backfill, stored);
  group->backfill_bytes += strlen(stored->json) + 1;
  GhMlsService *self = group->service;
  if (g_queue_get_length(&group->backfill) >= self->max_backfill_events ||
      group->backfill_bytes >= self->max_backfill_bytes)
    backfill_full(group);
}

static gboolean
resubscribe_idle(gpointer data)
{
  GhMlsGroup *group = data;
  if (group->scopes->len > 0)
    group_subscribe(group);
  g_object_unref(group);
  return G_SOURCE_REMOVE;
}

/* Review L2: a relay read only for an earlier address failed. One that
 * fails GH_MLS_SERVICE_EARLIER_RELAY_FAILURES times in a row -- an admin
 * usually drops a relay because it is dead -- is read no more; its late
 * events are lost with it. Re-subscribed from an idle: we are inside its
 * scope's callback. */
static void
earlier_relay_failed(GhMlsGroup *group, const gchar *url)
{
  guint n = GPOINTER_TO_UINT(g_hash_table_lookup(group->earlier_failures, url)) + 1;
  g_hash_table_insert(group->earlier_failures, g_strdup(url), GUINT_TO_POINTER(n));
  if (n < GH_MLS_SERVICE_EARLIER_RELAY_FAILURES)
    return;
  g_hash_table_remove(group->earlier_failures, url);
  gboolean changed = FALSE;
  for (guint i = group->addresses->len; i-- > 0;) {
    Address *a = g_ptr_array_index(group->addresses, i);
    if (!strv_has((const gchar *const *)a->relays, url))
      continue;
    GPtrArray *rest = g_ptr_array_new();
    for (guint k = 0; a->relays[k]; k++)
      if (!g_str_equal(a->relays[k], url))
        g_ptr_array_add(rest, g_strdup(a->relays[k]));
    g_strfreev(a->relays);
    a->relays = sorted_strv(rest);
    if (!a->relays[0])
      g_ptr_array_remove_index(group->addresses, i);
    changed = TRUE;
  }
  if (!changed)
    return;
  g_message("Groundhog stopped reading a relay an encrypted group left: it kept failing");
  routing_save(group);
  g_strfreev(group->read_relays);
  group->read_relays = read_relays_of(group);
  g_idle_add(resubscribe_idle, g_object_ref(group));
}

static void
on_group_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhMlsGroup *group = data;
  if (!g_ptr_array_find(group->scopes, scope, NULL))
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    /* A live event waits too while a backfill is pending: applied first it
     * would move the sender's ratchet past the stored older ones. */
    /* Only stored-answer progress holds the flush for @url: live traffic of a
     * relay whose older page stalled must not restart its quiet period, or a
     * busy group would show nothing until that relay went quiet (nostrc-iihf,
     * w21-mls-prereqs closing review M1). It still waits in the store. */
    if (update->backfill && update->stored)
      touch_backfilling(group, update->url);
    if (g_hash_table_size(group->backfilling) > 0 || !g_queue_is_empty(&group->backfill))
      keep_backfill(group, update);
    else
      process_event(group, update->event_json, update->url, update->stored, NULL);
    break;
  case GH_RELAY_NOTICE_EOSE: {
    g_hash_table_remove(group->backfilling, update->url);
    if (g_hash_table_size(group->backfilling) == 0)
      flush_backfill(group);
    if (!g_ptr_array_find(group->scopes, scope, NULL))
      break;   /* a Commit in it re-subscribed the group */
    /* The scope reports it once the backfill has been paged (nostrc-cpwf). */
    if (update->incomplete)
      g_message("Groundhog could not fetch every older encrypted group event from a relay; "
                "the group's read cursor stays where it was");
    g_hash_table_insert(group->settled, g_strdup(update->url),
                        GINT_TO_POINTER(update->incomplete ? 4 : 1));
    g_hash_table_remove(group->earlier_failures, update->url);
    update_history_incomplete(group);
    if (current_relays_settled(group)) {
      group_set_read(group, GH_MLS_READ_LIVE);
      if (all_relays_answered(group))
        save_cursor(group, group->newest);
    }
    break;
  }
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_DISCONNECTED:
  case GH_RELAY_NOTICE_ERROR: {
    /* What the relay sent before failing is still worth reading (it moves
     * no cursor: the relay has not answered). */
    g_hash_table_remove(group->backfilling, update->url);
    if (g_hash_table_size(group->backfilling) == 0)
      flush_backfill(group);
    if (!g_ptr_array_find(group->scopes, scope, NULL))
      break;
    g_hash_table_insert(group->settled, g_strdup(update->url), GINT_TO_POINTER(2));
    update_history_incomplete(group);
    if (!strv_has((const gchar *const *)group->relays, update->url))
      earlier_relay_failed(group, update->url);
    gboolean any_live = FALSE;
    for (guint i = 0; group->relays[i]; i++) {
      gint state = GPOINTER_TO_INT(g_hash_table_lookup(group->settled, group->relays[i]));
      any_live |= state == 1 || state == 4;
    }
    if (!any_live && current_relays_settled(group))
      group_set_read(group, GH_MLS_READ_DISCONNECTED);
    break;
  }
  default:
    break;
  }
}

static void group_refresh(GhMlsGroup *group);

static gboolean retain_fired(gpointer data);

/* A wake-up when the next earlier address ages out (it is read no more). */
static void
retain_schedule(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (group->retain_source || group->scopes->len == 0)
    return;
  gint64 next = G_MAXINT64;
  for (guint i = 0; i < group->addresses->len; i++) {
    Address *a = g_ptr_array_index(group->addresses, i);
    next = MIN(next, a->retired_at + GH_MLS_SERVICE_ROUTING_RETAIN_S);
  }
  if (next == G_MAXINT64)
    return;
  gint64 wait_s = MAX(next - now_s(self), 0) + 1;
  group->retain_source = gh_clock_timeout_add(self->clock, (guint64)wait_s * 1000, retain_fired,
                                              group, NULL);
}

static gboolean
retain_fired(gpointer data)
{
  GhMlsGroup *group = data;
  group->retain_source = 0;
  group_refresh(group);   /* drops what aged out, and re-subscribes */
  retain_schedule(group);  /* none aged out yet (or no re-subscription) */
  return G_SOURCE_REMOVE;
}

/* What @url is asked for: h tag -> since (0: none), the current routing's
 * and the earlier addresses' it served (nostrc-ms4d). */
static GHashTable *
url_wants(GhMlsGroup *group, const gchar *url)
{
  GHashTable *wants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (strv_has((const gchar *const *)group->relays, url)) {
    gint64 since = group->cursor > 0 ? MAX(group->cursor - GH_MLS_SERVICE_CURSOR_OVERLAP, 1) : 0;
    g_hash_table_insert(wants, g_strdup(group->nostr_hex), (gpointer)(gintptr)since);
  }
  for (guint i = 0; i < group->addresses->len; i++) {
    Address *a = g_ptr_array_index(group->addresses, i);
    if (!strv_has((const gchar *const *)a->relays, url))
      continue;
    gpointer old = NULL;
    if (g_hash_table_lookup_extended(wants, a->id, NULL, &old)) {
      gint64 was = (gint64)(gintptr)old;
      /* The earlier of the two; none at all is earliest. */
      gint64 since = was == 0 || a->since == 0 ? 0 : MIN(was, a->since);
      g_hash_table_insert(wants, g_strdup(a->id), (gpointer)(gintptr)since);
    } else {
      g_hash_table_insert(wants, g_strdup(a->id), (gpointer)(gintptr)a->since);
    }
  }
  return wants;
}

/* "<h>:<since>,..." sorted: relays with the same key share a scope. */
static gchar *
wants_key(GHashTable *wants)
{
  g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, wants);
  while (g_hash_table_iter_next(&iter, &key, &value))
    g_ptr_array_add(parts, g_strdup_printf("%s:%" G_GINT64_FORMAT, (const gchar *)key,
                                           (gint64)(gintptr)value));
  g_ptr_array_sort(parts, compare_strings);
  g_ptr_array_add(parts, NULL);
  return g_strjoinv(",", (gchar **)parts->pdata);
}

/* Live REQs {kinds:[445], #h:[an address of the group], since, limit} on
 * exactly the group relays (§4.3 MLS routing: ephemeral AUTH only), each
 * relay's backfill paged past its result cap (nostrc-cpwf). A relay is
 * asked only for the addresses it carried (nostrc-ms4d): the current one on
 * the current relays, an earlier one on the relays it was read at then.
 * Relays asked for the same set share a scope (each relay is in exactly
 * one, so `settled` and `backfilling` stay per relay); a relay the group
 * no longer lists never learns the address the group moved to (rotating
 * after a removal hides it from whoever runs the old relays). An earlier
 * address is read from the cursor it had when it was left, less the
 * overlap: its late events can be dated before the group's cursor now. */
static void
group_subscribe(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  group_unsubscribe(group);
  if (!running(self) || !listening(group) || !group->read_relays[0] || !group->nostr_hex[0])
    return;
  /* key -> GPtrArray of URLs (borrowed), in read_relays order; key -> wants. */
  g_autoptr(GHashTable) classes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                        (GDestroyNotify)g_ptr_array_unref);
  g_autoptr(GHashTable) class_wants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                            (GDestroyNotify)g_hash_table_unref);
  g_autoptr(GPtrArray) order = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; group->read_relays[i]; i++) {
    GHashTable *wants = url_wants(group, group->read_relays[i]);
    gchar *key = wants_key(wants);
    GPtrArray *urls = g_hash_table_lookup(classes, key);
    if (!urls) {
      urls = g_ptr_array_new();
      g_hash_table_insert(classes, g_strdup(key), urls);
      g_hash_table_insert(class_wants, g_strdup(key), wants);
      g_ptr_array_add(order, key);
    } else {
      g_hash_table_unref(wants);
      g_free(key);
    }
    g_ptr_array_add(urls, group->read_relays[i]);
  }
  g_autofree gchar *isolation = isolation_label(group);
  guint added = 0;
  for (guint c = 0; c < order->len; c++) {
    const gchar *key = g_ptr_array_index(order, c);
    GPtrArray *urls = g_hash_table_lookup(classes, key);
    GHashTable *wants = g_hash_table_lookup(class_wants, key);
    if (g_hash_table_size(wants) == 0) {
      /* Never: every relay read carries an address. A REQ without filters
       * is not sent, and the relay does not hold the group back. */
      for (guint i = 0; i < urls->len; i++)
        g_hash_table_insert(group->settled, g_strdup(g_ptr_array_index(urls, i)),
                            GINT_TO_POINTER(3));
      continue;
    }
    NostrFilters *filters = nostr_filters_new();
    int kinds[] = { MARMOT_KIND_GROUP_MESSAGE };
    gboolean current = FALSE;
    GHashTableIter iter;
    gpointer h, value;
    g_hash_table_iter_init(&iter, wants);
    while (g_hash_table_iter_next(&iter, &h, &value)) {
      current |= g_str_equal(h, group->nostr_hex);
      NostrFilter *filter = nostr_filter_new();
      nostr_filter_set_kinds(filter, kinds, 1);
      nostr_filter_tags_append(filter, "h", (const gchar *)h, NULL);
      gint64 since = (gint64)(gintptr)value;
      if (since > 0)
        nostr_filter_set_since_i64(filter, since);
      nostr_filters_add(filters, filter);
      nostr_filter_free(filter);
    }
    GhRelayScope *scope = gh_relay_scope_new(self->generation, filters, on_group_update, group);
    gh_relay_scope_set_backfill_paging(scope, GH_MLS_SERVICE_PAGE_LIMIT,
                                       GH_MLS_SERVICE_MAX_PAGES);
    /* The current routing's scope keeps the group's Tor circuits; one only
     * for earlier addresses gets its own. */
    g_autofree gchar *label = current ? g_strdup(isolation)
                                      : g_strdup_printf("%s-%u", isolation, c);
    gh_relay_scope_set_isolation(scope, label);
    guint in_scope = 0;
    for (guint i = 0; i < urls->len; i++) {
      const gchar *url = g_ptr_array_index(urls, i);
      g_autoptr(GError) error = NULL;
      /* A scope takes 16 URLs; an address set's relays are one routing
       * state's (at most 16), so this holds -- but a relay never asked must
       * not keep the group from going live. */
      if (in_scope >= MAX_GROUP_RELAYS || !gh_relay_scope_add_url(scope, url, &error)) {
        g_debug("Groundhog skips a group relay: %s", error ? error->message : "too many");
        g_hash_table_insert(group->settled, g_strdup(url), GINT_TO_POINTER(3));
        continue;
      }
      if (!gh_auth_policy_apply_scope(self->policy, scope, GH_AUTH_PURPOSE_MLS_ROUTING, url,
                                      &error))
        g_debug("Groundhog will not sign in to a group relay: %s", error->message);
      g_hash_table_insert(group->url_scope, g_strdup(url), scope);
      in_scope++;
    }
    if (!in_scope) {
      gh_relay_scope_unref(scope);
      continue;
    }
    g_ptr_array_add(group->scopes, scope);
    added += in_scope;
  }
  if (!added) {
    group_set_read(group, GH_MLS_READ_DISCONNECTED);
    return;
  }
  group_set_read(group, GH_MLS_READ_SYNCING);
  for (guint i = 0; i < group->scopes->len; i++)
    gh_relay_scope_start(g_ptr_array_index(group->scopes, i));
  retain_schedule(group);
}

/* ---- Retry ------------------------------------------------------------------------------ */

static void resume_all(GhMlsService *self);

static gboolean
retry_fired(gpointer data)
{
  GhMlsService *self = data;
  self->retry = 0;
  resume_all(self);
  return G_SOURCE_REMOVE;
}

/* A Commit, Welcome or send that no relay answered is republished later:
 * jittered (x U(0.5, 1.5)), doubling from RETRY_MIN_S to RETRY_MAX_S. */
static void
schedule_retry(GhMlsService *self)
{
  if (self->retry || !running(self))
    return;
  guint base = self->retry_s ? self->retry_s : RETRY_MIN_S;
  gint64 ms = gh_clock_random_range(self->clock, (gint64)base * 500, (gint64)base * 1500);
  self->retry = gh_clock_timeout_add(self->clock, (guint64)MAX(ms, 1000), retry_fired, self,
                                     NULL);
  self->retry_s = MIN(base * 2, RETRY_MAX_S);
}

static void
retry_succeeded(GhMlsService *self)
{
  self->retry_s = 0;
}

/* ---- Commits: publish, then merge (gh-mls-commits.h) ---------------------------------- */

typedef enum { OP_CREATE, OP_ADD, OP_REMOVE, OP_METADATA, OP_DEPARTURES } OpKind;

typedef struct {
  OpKind kind;
  GhMlsGroup *group;          /* ref; NULL until a created group exists */
  gchar *name;
  gchar *description;
  GStrv relays;               /* create */
  GStrv people;               /* invitees (create, add) or members (remove) */
  GPtrArray *key_packages;    /* the invitees' kind-30443 JSON in the group's format */
  GPtrArray *adopted_packages; /* per invitee: their adopted KeyPackage, or NULL */
  GPtrArray *legacy_packages;  /* per invitee: their MDK 0.8 KeyPackage, or NULL */
  GArray *keys;               /* 32-byte accounts: members (remove) or admins (metadata) */
  gboolean adopted;           /* create, add: the group's profile is the adopted one */
  gint expected;              /* create: the format the user was shown (-1: any) */
  MarmotGroupBlossomImage image; /* picture: the new 0x8002 state (present FALSE: clear) */
  gboolean clear_avatar_url;  /* picture: clear 0x8007 instead */
  guint rate_retries;         /* stagings libmarmot refused with MARMOT_ERR_EVENT_RATE */
  guint lookups;              /* in flight */
  GError *error;              /* the first lookup failure */
} Op;

static void
op_free(gpointer data)
{
  Op *op = data;
  g_clear_object(&op->group);
  g_free(op->name);
  g_free(op->description);
  g_strfreev(op->relays);
  g_strfreev(op->people);
  g_ptr_array_unref(op->key_packages);
  if (op->adopted_packages)
    g_ptr_array_unref(op->adopted_packages);
  if (op->legacy_packages)
    g_ptr_array_unref(op->legacy_packages);
  if (op->keys)
    g_array_unref(op->keys);
  marmot_group_blossom_image_clear(&op->image);   /* wipes the image and upload keys */
  g_clear_error(&op->error);
  g_free(op);
}

struct _Round {
  GhMlsGroup *group;
  GhMlsCommitPublish *publish;
  GhRelayPublish *relay;
  GhMlsCommitState state;
  gboolean reported;
  gboolean any_answer;
};

static void
round_free(Round *round)
{
  if (round->relay) {
    gh_relay_publish_cancel(round->relay);
    gh_relay_publish_unref(round->relay);
  }
  gh_mls_commit_publish_free(round->publish);
  g_free(round);
}

static gboolean
round_free_idle(gpointer data)
{
  round_free(data);
  return G_SOURCE_REMOVE;
}

static void
complete_waiters(GhMlsGroup *group, const GError *error)
{
  g_autoptr(GPtrArray) waiters = g_steal_pointer(&group->waiters);
  group->waiters = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < waiters->len; i++) {
    GTask *task = g_ptr_array_index(waiters, i);
    Op *op = g_task_get_task_data(task);
    if (error)
      g_task_return_error(task, g_error_copy(error));
    else if (op->kind == OP_CREATE)
      g_task_return_pointer(task, g_object_ref(group), g_object_unref);
    else
      g_task_return_boolean(task, TRUE);
  }
}

static void leave_continue(GhMlsGroup *group);

static void
after_commit(GhMlsGroup *group)
{
  /* The group moved on: a refused Commit lost its epoch (nostrc-prrl). */
  if (group->refused_json)
    set_refused(group, NULL, GH_MLS_REFUSAL_NONE);
  group_refresh(group);
  retry_held(group);
  welcomes_pump(group);
  /* nostrc-2um6: a new epoch makes a leave proposal stale (ours is made
   * again); others' leaves may be committable now. */
  leave_continue(group);
  departures_schedule(group);
  upgrade_schedule(group);
}

/* The Commit reached a final state for the account's change. */
static void
round_report(Round *round)
{
  GhMlsGroup *group = round->group;
  round->reported = TRUE;
  g_autoptr(GError) error = NULL;
  if (round->state == GH_MLS_COMMIT_SUPERSEDED)
    error = g_error_new_literal(GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_SUPERSEDED,
                                "Another member changed the group first");
  else if (round->state == GH_MLS_COMMIT_CLEARED)
    error = g_error_new_literal(GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_REFUSED,
                                "The group relays refused the change");
  if (round->state == GH_MLS_COMMIT_MERGED) {
    retry_succeeded(group->service);
    g_free(group->last_committer);
    group->last_committer = g_strdup(group->service->account);
    group->last_committer_leaf = G_MAXUINT32;   /* our own leaf is proven */
  }
  after_commit(group);
  complete_waiters(group, error);
}

static void
round_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  Round *round = data;
  (void)publish;
  GhMlsService *self = round->group->service;
  gboolean accepted = result->outcome == GH_RELAY_PUBLISH_ACCEPTED;
  gboolean refused = result->outcome == GH_RELAY_PUBLISH_REJECTED ||
                     result->outcome == GH_RELAY_PUBLISH_AUTH_REQUIRED;
  if (!accepted && !refused)
    return; /* no answer: the relay may still have it; the Commit stays pending */
  round->any_answer = TRUE;
  g_autoptr(GError) error = NULL;
  GhMlsCommitState state = GH_MLS_COMMIT_PENDING;
  if (!gh_mls_commit_record_answer(self->store, self->marmot, self->storage, round->publish,
                                   result->url, accepted, result->message, &state, &error)) {
    g_message("Groundhog could not record a group relay's answer: %s", error->message);
    return;
  }
  round->state = state;
  if (state != GH_MLS_COMMIT_PENDING && !round->reported)
    round_report(round);
}

static void
round_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  Round *round = data;
  (void)publish;
  (void)summary;
  GhMlsGroup *group = round->group;
  if (group->round == round)
    group->round = NULL;
  if (!round->reported) {
    /* Unanswered: republished later, byte for byte (the waiters wait, and
     * so do changes queued behind it). */
    group_refresh(group);
    schedule_retry(group->service);
  } else if (!group->pending_commit) {
    group->auto_change = FALSE;
    queued_flush(group);
  }
  g_idle_add(round_free_idle, round);
}

/* Publishes the staged Commit to its relays; takes publish. */
static void
round_start(GhMlsGroup *group, GhMlsCommitPublish *publish)
{
  GhMlsService *self = group->service;
  if (group->round || !running(self) || !publish->relay_urls || !publish->relay_urls[0]) {
    gh_mls_commit_publish_free(publish);
    if (!group->round)
      schedule_retry(self);
    return;
  }
  Round *round = g_new0(Round, 1);
  round->group = group;
  round->publish = publish;
  g_autoptr(GError) error = NULL;
  round->relay = gh_relay_publish_new(self->generation, publish->event_json, round_update,
                                      round_done, round, &error);
  if (!round->relay) {
    g_warning("Groundhog cannot publish a group change: %s", error->message);
    round_free(round);
    return;
  }
  if (self->publish_deadline)
    gh_relay_publish_set_deadline(round->relay, self->publish_deadline);
  guint added = 0;
  for (guint i = 0; publish->relay_urls[i]; i++) {
    g_autoptr(GError) url_error = NULL;
    if (!gh_relay_publish_add_url(round->relay, publish->relay_urls[i], &url_error))
      continue;
    /* §4.3: MLS routing relays never learn the account. */
    gh_auth_policy_apply_publish(self->policy, round->relay, GH_AUTH_PURPOSE_MLS_ROUTING,
                                 publish->relay_urls[i], NULL);
    added++;
  }
  if (!added) {
    round_free(round);
    return;
  }
  group->round = round;
  if (!gh_relay_publish_start(round->relay, &error)) {
    group->round = NULL;
    g_warning("Groundhog cannot publish a group change: %s", error->message);
    round_free(round);
  }
}

static MarmotError
produce_add(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  GPtrArray *kps = data;
  char **welcomes = NULL;
  size_t n = 0;
  MarmotError err = marmot_add_members(marmot, gid, (const char **)kps->pdata, kps->len,
                                       &welcomes, &n, out);
  /* The same Welcomes reach the outbox on merge: send that copy only. */
  for (size_t i = 0; i < n; i++)
    free(welcomes[i]);
  free(welcomes);
  return err;
}

static MarmotError
produce_remove(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  Op *op = data;
  return marmot_remove_members(marmot, gid, (const uint8_t (*)[32])op->keys->data,
                               op->keys->len, out);
}

/* The Op's name, description or admins (whichever it sets). */
static MarmotError
produce_metadata(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  Op *op = data;
  MarmotGroupConfig config = {
    .name = op->name,
    .description = op->description,
    .admin_pubkeys = op->keys ? (uint8_t (*)[32])op->keys->data : NULL,
    .admin_count = op->keys ? op->keys->len : 0,
  };
  return marmot_update_group_metadata(marmot, gid, &config, out);
}

/* The group picture (W25, nostrc-m6tp): one AppDataUpdate of 0x8002, the
 * canonical empty state to clear. */
static MarmotError
produce_image(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  Op *op = data;
  if (op->clear_avatar_url)
    return marmot_update_group_avatar_url(marmot, gid, NULL, out);
  return marmot_update_group_blossom_image(marmot, gid, op->image.present ? &op->image : NULL,
                                           out);
}

/* libmarmot refuses a Commit it cannot date within a minute of our clock
 * (MARMOT_ERR_EVENT_RATE, nostrc-2lrz): right after another member's Commit
 * dated that far ahead, or Commits back to back. It fits a second later, so
 * the change is staged again after RATE_RETRY_MS, up to RATE_RETRIES times,
 * before the caller sees GH_MLS_SERVICE_ERROR_BUSY (review W24 N5). The
 * producer's data lives in the task's Op. */
#define RATE_RETRY_MS 1000
#define RATE_RETRIES 5

#ifdef GH_MLS_TEST_HOOKS
static guint test_rate_retries;
static guint test_refuse_rate;
static guint test_departure_failures;
static gboolean test_permissive_groups;
static guint test_upgrade_min_ms, test_upgrade_max_ms, test_upgrade_stagger_ms;

void
gh_mls_service_test_set_permissive_groups(gboolean permissive)
{
  test_permissive_groups = permissive;
}

void
gh_mls_service_test_set_upgrade_window(guint min_ms, guint max_ms, guint stagger_ms)
{
  test_upgrade_min_ms = min_ms;
  test_upgrade_max_ms = max_ms;
  test_upgrade_stagger_ms = stagger_ms;
}

guint
gh_mls_service_test_rate_retries(void)
{
  return test_rate_retries;
}

void
gh_mls_service_test_refuse_rate(guint n)
{
  test_refuse_rate = n;
}

guint
gh_mls_service_test_departure_failures(void)
{
  return test_departure_failures;
}
#endif

static gboolean check_change(GhMlsService *self, GhMlsGroup *group, GError **error);
static gboolean check_departures(GhMlsService *self, GhMlsGroup *group, GError **error);
static gboolean check_running(GhMlsService *self, GError **error);
static void stage_change(GTask *task, GhMlsGroup *group, GhMlsCommitProducer producer,
                         gpointer data);

typedef struct {
  GTask *task;                /* owned */
  GhMlsCommitProducer producer;
  gpointer data;              /* the task's */
} RateRetry;

static gboolean
rate_retry_fired(gpointer data)
{
  RateRetry *retry = data;
  GTask *task = retry->task;
  GhMlsService *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
  } else if (op->kind == OP_DEPARTURES ? !check_departures(self, op->group, &error)
                                       : !check_change(self, op->group, &error)) {
    /* A member's leave is any member's to commit, not an admin's only
     * (nostrc-2um6): re-checked as departures_fired() checks it. */
    g_task_return_error(task, error);
    g_object_unref(task);
  } else {
    stage_change(task, op->group, retry->producer, retry->data);
  }
  g_free(retry);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GTask *task;                /* owned */
  GhMlsCommitProducer producer;
  gpointer data;              /* the task's */
} QueuedChange;

/* The next change queued behind an automatic Commit, now that it is through
 * (staged now; the rest wait for that one). */
static void
queued_flush(GhMlsGroup *group)
{
  QueuedChange *queued = g_queue_pop_head(&group->queued);
  if (!queued)
    return;
  GError *error = NULL;
  if (g_task_return_error_if_cancelled(queued->task)) {
    g_object_unref(queued->task);
  } else if (!check_change(group->service, group, &error)) {
    g_task_return_error(queued->task, error);
    g_object_unref(queued->task);
  } else {
    stage_change(queued->task, group, queued->producer, queued->data);
  }
  g_free(queued);
  if (!group->round && !group->pending_commit)
    queued_flush(group);   /* that one failed at once: the next */
}

static void
queued_cancel(GhMlsGroup *group, const GError *error)
{
  QueuedChange *queued;
  while ((queued = g_queue_pop_head(&group->queued))) {
    g_task_return_error(queued->task, g_error_copy(error));
    g_object_unref(queued->task);
    g_free(queued);
  }
  group->auto_change = FALSE;
}

/* Stages the change (T-mls) and publishes it; task completes with it. */
static void
stage_change(GTask *task, GhMlsGroup *group, GhMlsCommitProducer producer, gpointer data)
{
  GhMlsService *self = group->service;
  g_autoptr(GError) error = NULL;
  if (group->round || group->pending_commit) {
    /* Review M1: behind a Commit Groundhog made on its own (the SelfRemove
     * upgrade, a member's leave) the admin's change waits and is made once
     * that one is through, instead of failing for a change they never made. */
    if (group->auto_change) {
      QueuedChange *queued = g_new0(QueuedChange, 1);
      queued->task = task;
      queued->producer = producer;
      queued->data = data;
      g_queue_push_tail(&group->queued, queued);
      return;
    }
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_BUSY,
                            "Another change of this group is still being sent");
    g_object_unref(task);
    return;
  }
  GhMlsCommitPublish *publish = NULL;
#ifdef GH_MLS_TEST_HOOKS
  if (test_refuse_rate > 0) {   /* as libmarmot refuses it; nothing is staged */
    test_refuse_rate--;
    g_set_error(&error, GH_MLS_COMMIT_ERROR, MARMOT_ERR_EVENT_RATE, "%s: %s",
                "Could not stage the change", marmot_error_string(MARMOT_ERR_EVENT_RATE));
  } else
#endif
    publish = gh_mls_commit_stage(self->store, self->marmot, self->storage, &group->gid,
                                  self->account, producer, data, &error);
  if (!publish) {
    Op *op = g_task_get_task_data(task);
    if (error->domain == GH_MLS_COMMIT_ERROR && error->code == MARMOT_ERR_EVENT_RATE &&
        op->rate_retries < RATE_RETRIES) {
      op->rate_retries++;
#ifdef GH_MLS_TEST_HOOKS
      test_rate_retries++;
#endif
      drop_stale_error(self);
      RateRetry *retry = g_new0(RateRetry, 1);
      retry->task = task;                 /* our reference */
      retry->producer = producer;
      retry->data = data;
      gh_clock_timeout_add(self->clock, RATE_RETRY_MS, rate_retry_fired, retry, NULL);
      return;
    }
    if (error->domain == GH_MLS_COMMIT_ERROR) {
      GError *mapped = NULL;
      marmot_fail(self, (MarmotError)error->code, "The change could not be made", &mapped);
      g_task_return_error(task, mapped);
    } else {
      g_task_return_error(task, g_steal_pointer(&error));
    }
    g_object_unref(task);
    return;
  }
  g_ptr_array_add(group->waiters, task);   /* takes the reference */
  group_refresh(group);
  round_start(group, publish);
}

/* ---- Leaving and others' leaves (nostrc-2um6) ------------------------------------------ */

static gboolean held_awaits_proposal(GhMlsGroup *group);
static GTask *op_task(GhMlsService *self, OpKind kind, GhMlsGroup *group,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data, gpointer tag);

static MarmotError
produce_departures(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  (void)data;
  return marmot_commit_pending_proposals(marmot, gid, out);
}

/* Whether libmarmot would commit a member's leave now; also learns who asked
 * to leave (after a restart, from the kept proposals). */
static gboolean
departures_committable(GhMlsGroup *group)
{
  MarmotPendingProposal *p = NULL;
  size_t n = 0;
  gboolean any = FALSE;
  if (marmot_get_pending_proposals(group->service->marmot, &group->gid, &p, &n) != MARMOT_OK)
    return FALSE;
  for (size_t i = 0; i < n; i++) {
    if (p[i].leave && !p[i].own)
      g_hash_table_add(group->leavers, to_hex(p[i].sender, 32));
    any |= p[i].committable;
  }
  marmot_pending_proposals_free(p);
  return any;
}

static void
departures_cancel(GhMlsGroup *group)
{
  if (group->departures_source) {
    gh_clock_source_remove(group->service->clock, group->departures_source);
    group->departures_source = 0;
  }
}

static void
departures_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)data;
  g_autoptr(GError) error = NULL;
  /* Superseded (another member committed it first), refused or busy: the
   * next Commit, or the leaver's fresh proposal, schedules it again. */
  if (!gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error)) {
#ifdef GH_MLS_TEST_HOOKS
    test_departure_failures++;
#endif
    g_debug("Groundhog did not commit a member's leave: %s", error->message);
  }
}

static gboolean
departures_fired(gpointer data)
{
  GhMlsGroup *group = data;
  GhMlsService *self = group->service;
  group->departures_source = 0;
  /* Re-checked now: another member's Commit may have consumed it, or a
   * change of ours is still out (after_commit() schedules again). */
  if (!running(self) || !group->active || group->leaving || group->round ||
      group->pending_commit || held_awaits_proposal(group) || !departures_committable(group))
    return G_SOURCE_REMOVE;
  GTask *task = op_task(self, OP_DEPARTURES, group, NULL, departures_done, NULL,
                        gh_mls_service_change_finish);
  group->auto_change = TRUE;   /* the admin's own changes queue behind it (M1) */
  stage_change(task, group, produce_departures, NULL);
  if (!group->round && !group->pending_commit)
    group->auto_change = FALSE;   /* not staged */
  return G_SOURCE_REMOVE;
}

/* Whether a member's leave may still be committed by us: the group change
 * check for departures, which any member may commit (unlike check_change(),
 * no admin is needed). */
static gboolean
check_departures(GhMlsService *self, GhMlsGroup *group, GError **error)
{
  if (!check_running(self, error))
    return FALSE;
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active || group->leaving) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not an active group of this account");
    return FALSE;
  }
  if (!departures_committable(group)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "No member's leave is left to commit");
    return FALSE;
  }
  return TRUE;
}

/* A member asked to leave: commit it after a random delay (member-departure.md:
 * any remaining member may; the jitter keeps members online together from
 * racing, and never enters ordering). */
/* A held Commit waits for a proposal (review H1): it may consume the leave
 * we would commit, so ours waits too. */
static gboolean
held_awaits_proposal(GhMlsGroup *group)
{
  for (GList *l = group->held.head; l; l = l->next)
    if (((Held *)l->data)->awaits_proposal)
      return TRUE;
  return FALSE;
}

/* The jitter window grows with the group (review L5): with N members
 * online, about 1 + (N-1)*latency/window of them fire before the first
 * Commit is seen. */
static gint64
departures_window_ms(GhMlsGroup *group)
{
  guint n = group->members ? g_strv_length(group->members) : 1;
  gint64 max = GH_MLS_SERVICE_DEPARTURE_JITTER_MIN_MS +
               (gint64)n * GH_MLS_SERVICE_DEPARTURE_JITTER_PER_MEMBER_MS;
  return MIN(max, GH_MLS_SERVICE_DEPARTURE_JITTER_MAX_MS);
}

static void
departures_schedule(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (group->departures_source || !running(self) || !group->active || group->leaving ||
      group->retrying || held_awaits_proposal(group) || !departures_committable(group))
    return;
  gint64 ms = gh_clock_random_range(self->clock, GH_MLS_SERVICE_DEPARTURE_JITTER_MIN_MS,
                                    departures_window_ms(group));
  group->departures_source = gh_clock_timeout_add(self->clock, (guint64)ms, departures_fired,
                                                  group, NULL);
}

/* ---- Requiring SelfRemove (nostrc-8ndz) ------------------------------------------------- *
 *
 * Groundhog makes every group alone and then Adds. libmarmot's Add that
 * brings such a group its second member also requires SelfRemove when our
 * leaf and every invitee advertise it -- MDK's creation rule (LCD over the
 * invitees), in that same Commit (MarmotConfig.keep_first_add_permissive
 * stays false) -- so any member, not only an admin, commits a member's
 * leave, and a later invitee whose app lacks it is refused with honest copy
 * (nostrc-zbmb). Nothing else is committed for new groups.
 *
 * A group this device created (its mls/o/ record says so) without the
 * requirement -- made permissive, e.g. its first invitee lacked SelfRemove
 * -- gets it once in the background, when every
 * member supports it, through libmarmot marmot_require_self_remove() (a
 * GroupContextExtensions Commit, MDK 0.11's upgrade_group_capabilities()):
 * only once the group has caught up (read live, every current relay
 * answered), after a long random delay, and staggered across groups so a
 * first launch never commits in many groups at once (review L1). Groups
 * others made are left as their creator chose (MDK keeps a group created
 * with no invitee permissive on purpose). The admin's own changes made
 * while it is out wait behind it (review M1). */

#define ORIGIN_CREATED 1
#define ORIGIN_JOINED 2

/* Where the group's origin is kept: "mls/o/" + 32 hex of SHA-256(domain ||
 * group id); 1 created on this device, 2 joined by a Welcome. */
static gchar *
origin_scope(GhMlsGroup *group)
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-origin/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, group->gid.data, (gssize)group->gid.len);
  return g_strdup_printf("mls/o/%.32s", g_checksum_get_string(sum));
}

static void
set_origin(GhMlsGroup *group, gint64 origin)
{
  g_autofree gchar *scope = origin_scope(group);
  g_autoptr(GError) error = NULL;
  if (!gh_store_set_cursor(group->service->store, scope, "", origin, &error))
    g_message("Groundhog could not record where an encrypted group came from: %s",
              error->message);
}

/* Whether this device created the group: only by its record (set on create
 * and on join). A group without one -- from before the record, so only on
 * development installs -- counts as joined: nothing about its tree says for
 * sure that this device made it (W25 slice M re-review R1), and a group
 * someone else made is left as its creator chose. */
static gboolean
created_here(GhMlsGroup *group)
{
  g_autofree gchar *scope = origin_scope(group);
  gint64 origin = 0;
  return gh_store_get_cursor(group->service->store, scope, "", &origin, NULL) &&
         origin == ORIGIN_CREATED;
}

static gboolean
upgrade_permissive(void)
{
#ifdef GH_MLS_TEST_HOOKS
  return test_permissive_groups;
#else
  return FALSE;
#endif
}

static MarmotError
produce_require_self_remove(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  (void)data;
  return marmot_require_self_remove(marmot, gid, out);
}

/* The group could take the requirement from us now, caught up or not. */
static gboolean
upgrade_wanted(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (upgrade_permissive() || !running(self) || !group->active || !group->is_admin ||
      group->leaving || group->upgrade_epoch == group->epoch + 1 || !group->members ||
      g_strv_length(group->members) < 2)
    return FALSE;
  bool required = false, upgradable = false;
  return marmot_get_self_remove_requirement(self->marmot, &group->gid, &required, &upgradable) ==
           MARMOT_OK && upgradable && created_here(group);
}

static gboolean
caught_up(GhMlsGroup *group)
{
  return group->read == GH_MLS_READ_LIVE && all_relays_answered(group) &&
         g_hash_table_size(group->backfilling) == 0 && g_queue_is_empty(&group->backfill);
}

static void
upgrade_cancel(GhMlsGroup *group)
{
  if (group->upgrade_source) {
    gh_clock_source_remove(group->service->clock, group->upgrade_source);
    group->upgrade_source = 0;
  }
}

static void
upgrade_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)data;
  g_autoptr(GError) error = NULL;
  /* Superseded, refused or busy: a later epoch tries again. */
  if (!gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error))
    g_debug("Groundhog did not require SelfRemove in a group: %s", error->message);
}

static gboolean
upgrade_fired(gpointer data)
{
  GhMlsGroup *group = data;
  group->upgrade_source = 0;
  /* Not caught up (any more): going live schedules it again; a change of
   * ours still out: its outcome does (after_commit()). */
  if (!upgrade_wanted(group) || !caught_up(group) || group->round || group->pending_commit)
    return G_SOURCE_REMOVE;
  group->upgrade_epoch = group->epoch + 1;
  GhMlsService *self = group->service;
  GTask *task = op_task(self, OP_METADATA, group, NULL, upgrade_done, NULL,
                        gh_mls_service_change_finish);
  group->auto_change = TRUE;
  stage_change(task, group, produce_require_self_remove, NULL);
  if (!group->round && !group->pending_commit)
    group->auto_change = FALSE;   /* not staged */
  return G_SOURCE_REMOVE;
}

static void
upgrade_window(guint64 *min_ms, guint64 *max_ms, guint64 *stagger_ms)
{
  *min_ms = (guint64)GH_MLS_SERVICE_UPGRADE_DELAY_MIN_S * 1000;
  *max_ms = (guint64)GH_MLS_SERVICE_UPGRADE_DELAY_MAX_S * 1000;
  *stagger_ms = (guint64)GH_MLS_SERVICE_UPGRADE_STAGGER_S * 1000;
#ifdef GH_MLS_TEST_HOOKS
  if (test_upgrade_max_ms) {
    *min_ms = test_upgrade_min_ms;
    *max_ms = test_upgrade_max_ms;
    *stagger_ms = test_upgrade_stagger_ms;
  }
#endif
}

static void
upgrade_schedule(GhMlsGroup *group)
{
  if (group->upgrade_source || !caught_up(group) || !upgrade_wanted(group))
    return;
  GhMlsService *self = group->service;
  guint64 min_ms, max_ms, stagger_ms;
  upgrade_window(&min_ms, &max_ms, &stagger_ms);
  gint64 now_ms = now_s(self) * 1000;
  gint64 at_ms = now_ms + gh_clock_random_range(self->clock, (gint64)min_ms, (gint64)max_ms);
  /* Staggered: never within `stagger` of another group's. */
  if (self->upgrade_slot > 0)
    at_ms = MAX(at_ms, self->upgrade_slot + (gint64)stagger_ms);
  self->upgrade_slot = at_ms;
  group->upgrade_source = gh_clock_timeout_add(self->clock, (guint64)(at_ms - now_ms),
                                               upgrade_fired, group, NULL);
}

static void
leave_cancel(GhMlsGroup *group)
{
  if (group->leave_publish) {
    gh_relay_publish_cancel(group->leave_publish);
    g_clear_pointer(&group->leave_publish, gh_relay_publish_unref);
  }
}

static gboolean
leave_publish_free_idle(gpointer data)
{
  gh_relay_publish_unref(data);
  return G_SOURCE_REMOVE;
}

static void
leave_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  GhMlsGroup *group = data;
  if (publish == group->leave_publish && result->outcome == GH_RELAY_PUBLISH_ACCEPTED)
    group->leave_sent = TRUE;
}

static void
leave_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GhMlsGroup *group = data;
  (void)summary;
  if (publish != group->leave_publish)
    return;
  group->leave_publish = NULL;
  g_idle_add(leave_publish_free_idle, publish);
  if (!group->leave_sent)
    schedule_retry(group->service);   /* resume_all() publishes it again */
}

/* Publishes our SelfRemove event `json` to the group relays until one takes
 * it (MIP-03: like any kind 445, ephemeral AUTH only). */
static void
leave_start(GhMlsGroup *group, const gchar *json)
{
  GhMlsService *self = group->service;
  if (g_strcmp0(group->leave_json, json) != 0) {
    leave_cancel(group);
    g_free(group->leave_json);
    group->leave_json = g_strdup(json);
    group->leave_sent = FALSE;
  }
  if (group->leave_sent || group->leave_publish || !running(self) || !group->relays ||
      !group->relays[0])
    return;
  g_autoptr(GError) error = NULL;
  GhRelayPublish *relay = gh_relay_publish_new(self->generation, json, leave_update, leave_done,
                                               group, &error);
  if (!relay) {
    g_warning("Groundhog cannot publish its leave: %s", error->message);
    return;
  }
  if (self->publish_deadline)
    gh_relay_publish_set_deadline(relay, self->publish_deadline);
  guint added = 0;
  for (guint i = 0; group->relays[i]; i++) {
    g_autoptr(GError) url_error = NULL;
    if (!gh_relay_publish_add_url(relay, group->relays[i], &url_error))
      continue;
    gh_auth_policy_apply_publish(self->policy, relay, GH_AUTH_PURPOSE_MLS_ROUTING,
                                 group->relays[i], NULL);
    added++;
  }
  if (!added) {
    gh_relay_publish_unref(relay);
    return;
  }
  group->leave_publish = relay;
  if (!gh_relay_publish_start(relay, &error)) {
    group->leave_publish = NULL;
    gh_relay_publish_unref(relay);
    g_warning("Groundhog cannot publish its leave: %s", error->message);
  }
}

/* While leaving: (re)publish the SelfRemove of the current epoch. libmarmot
 * returns the same bytes within an epoch and a fresh proposal for a new one
 * (member-departure.md); its leave request is durable, so this also resumes
 * a leave after a restart. */
/* Drop the leave and say why; sending works again (review L3). */
static void
leave_give_up(GhMlsGroup *group, GhMlsLeaveFailure why)
{
  GhMlsService *self = group->service;
  leave_cancel(group);
  if (marmot_cancel_leave(self->marmot, &group->gid) != MARMOT_OK)
    return;   /* still Leaving; the next pass tries again */
  group->leave_failure = why;
  group->leave_misses = 0;
  group->leave_admin_commit = FALSE;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_LEAVE_FAILED]);
  group_refresh(group);
}

static void
leave_continue(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (!running(self) || !group->active || !group->leaving)
    return;
  /* Re-review R1: an admin's Commit moved the group on and kept us -- our
   * Remove request was not acted on (MDK 0.8's admin auto-commit drops it
   * and commits nothing). Request once more; after a second such Commit,
   * stop: the admin's app does not process it. */
  if (group->leave_admin_commit && group->leave_via_admin) {
    group->leave_admin_commit = FALSE;
    if (++group->leave_misses >= GH_MLS_SERVICE_LEAVE_REQUESTS) {
      g_message("Groundhog stopped asking the admins to remove the account: no admin acted on it");
      leave_give_up(group, GH_MLS_LEAVE_FAILURE_NOT_PROCESSED);
      return;
    }
  }
  char *json = NULL;
  MarmotError err = marmot_self_remove(self->marmot, &group->gid, &json);
  if (err == MARMOT_OK && json) {
    leave_start(group, json);
  } else if (err == MARMOT_ERR_ADMIN_CANNOT_LEAVE || err == MARMOT_ERR_UNSUPPORTED ||
             err == MARMOT_ERR_USE_AFTER_EVICTION) {
    /* Definitive (review L3, N4): this leave cannot go on (e.g. the
     * account was made an admin). Say so and stop blocking sends, rather
     * than "waiting" for a Commit that cannot come. */
    g_message("Groundhog stopped leaving an encrypted group: %s", marmot_error_string(err));
    leave_give_up(group, GH_MLS_LEAVE_FAILURE_CANNOT_CONTINUE);
  } else {
    /* Transient (storage, a pending change of ours): keep the leave and
     * try again on the next pass (review N4). */
    g_debug("Groundhog will retry its leave: %s", marmot_error_string(err));
    schedule_retry(self);
  }
  free(json);
}

static gboolean
check_running(GhMlsService *self, GError **error)
{
  if (running(self))
    return TRUE;
  g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_INACTIVE,
                      "Encrypted groups work only for the active account, online");
  return FALSE;
}

/* PD-8/PT-8: the default consent is an accepted, non-request NIP-17 room
 * with the person. */
static gboolean
may_look_up(GhMlsService *self, const gchar *pubkey)
{
  if (self->may_look_up)
    return self->may_look_up(pubkey, self->consent_data);
  if (!account_matches_model(self))
    return FALSE;
  GListModel *model = G_LIST_MODEL(self->conversations);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhConversation) room = g_list_model_get_item(model, i);
    if (gh_conversation_get_backend(room) == GH_CONVERSATION_BACKEND_NIP17 &&
        !gh_conversation_get_is_request(room) &&
        g_strv_contains(gh_conversation_get_peers(room), pubkey))
      return TRUE;
  }
  return FALSE;
}

static GStrv
discovery_relays(GhMlsService *self)
{
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  g_auto(GStrv) urls = self->settings ? g_settings_get_strv(self->settings, "discovery-relays")
                                      : NULL;
  for (guint i = 0; urls && urls[i]; i++)
    if (gh_relay_url_validate(urls[i], NULL))
      g_strv_builder_add(out, urls[i]);
  return g_strv_builder_end(out);
}

static void create_group_now(GTask *task);

static void
invitees_ready(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  if (op->error) {
    g_task_return_error(task, g_steal_pointer(&op->error));
    g_object_unref(task);
    return;
  }
  GhMlsService *self = g_task_get_source_object(task);
  if (!running(self)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The account changed before the invitation was sent");
    g_object_unref(task);
    return;
  }
  /* The group's format (nostrc-lf62): a new group is adopted when every
   * invitee has an adopted KeyPackage, and falls back to the MDK 0.8 one only
   * when someone has no adopted one; an existing group keeps its own. The
   * two cannot be mixed: an MLS group has one profile, and an adopted-only
   * and a legacy-only invitee have no format in common. */
  guint n = op->people ? g_strv_length(op->people) : 0;
  guint adopted_count = 0, legacy_count = 0;
  for (guint i = 0; i < n; i++) {
    adopted_count += g_ptr_array_index(op->adopted_packages, i) != NULL;
    legacy_count += g_ptr_array_index(op->legacy_packages, i) != NULL;
  }
  if (op->kind == OP_CREATE && op->expected >= 0) {
    /* The format New Group showed (review M2): the lookups at creation must
     * agree. A KeyPackage gone missing since the check (a relay withholding
     * it, a lookup timing out) never turns the group into an older-format
     * one unseen; the user reviews the choice instead. */
    op->adopted = op->expected == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED;
    if ((op->adopted ? adopted_count : legacy_count) != n) {
      g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_FORMAT_CHANGED,
                              "An invitee's KeyPackages changed since they were checked");
      g_object_unref(task);
      return;
    }
  } else if (op->kind == OP_CREATE) {
    op->adopted = adopted_count == n;
    if (!op->adopted && legacy_count != n) {
      g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_MIXED_PROFILE,
                              "Some invitees use only the newer group format and others only "
                              "the older one");
      g_object_unref(task);
      return;
    }
  } else {
    op->adopted = op->group->adopted;
    if ((op->adopted ? adopted_count : legacy_count) != n) {
      g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_PROFILE_MISMATCH,
                              "An invitee has no KeyPackage in this group's format");
      g_object_unref(task);
      return;
    }
  }
  GPtrArray *chosen = op->adopted ? op->adopted_packages : op->legacy_packages;
  for (guint i = 0; i < n; i++)
    g_ptr_array_add(op->key_packages, g_strdup(g_ptr_array_index(chosen, i)));
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  /* While the account requires proofs (VERIFIED_ONLY_KEY), an invitee whose
   * app cannot prove their account (MDK 0.8, libmarmot <= 0.9.0) is refused
   * before anything is made: libmarmot refuses the Add too, but only after a
   * creation stored the account's group of one, which then stayed listed
   * although the UI says nothing was changed (nostrc-7gx7). By default such
   * an invitee is added (nostrc-6ukh); a proof that does not verify is
   * refused either way. libmarmot still judges the whole tree of an Add.
   * Only MDK 0.8 KeyPackages can lack the proof: the adopted profile's
   * validation requires and verifies it. */
  if (!op->adopted) {
    for (guint i = 0; i < op->key_packages->len; i++) {
      bool proven = false;
      MarmotError err =
        marmot_key_package_event_has_account_proof(op->key_packages->pdata[i], &proven);
      if (err != MARMOT_OK || (!proven && !self->allow_unproven)) {
        GError *error = NULL;
        marmot_fail(self, err == MARMOT_OK ? MARMOT_ERR_KEY_PACKAGE_IDENTITY : err,
                    "Nobody was invited", &error);
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
      }
    }
  }
#endif
  if (op->kind == OP_CREATE) {
    create_group_now(task);
    return;
  }
  remember_own_evidence(op->group, op->key_packages);
  stage_change(task, op->group, produce_add, op->key_packages);
}

typedef struct {
  GTask *task; /* borrowed; the operation waits for every lookup */
  guint index;
} InviteeLookup;

static void
lookup_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  InviteeLookup *lookup = data;
  GTask *task = lookup->task;
  Op *op = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsKeyPackage) kp = gh_mls_key_package_lookup_finish(result, &error);
  if (kp) {
    /* Both formats, whichever the group uses: evidence fetched anyway. */
    GhMlsService *self = g_task_get_source_object(task);
    if (kp->adopted) {
      g_ptr_array_index(op->adopted_packages, lookup->index) = g_strdup(kp->event_json);
      remember_key_package(self, kp->event_json);
    }
    if (kp->legacy_event_json) {
      g_ptr_array_index(op->legacy_packages, lookup->index) = g_strdup(kp->legacy_event_json);
      remember_key_package(self, kp->legacy_event_json);
    }
  } else if (!op->error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      op->error = g_error_new(GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE,
                              "%s", error->message);
    else
      op->error = g_steal_pointer(&error);
  }
  g_free(lookup);
  if (--op->lookups == 0)
    invitees_ready(task);
}

/* Consent first (nothing is looked up for anyone when one invitee is not
 * accepted), then one KeyPackage lookup per invitee. */
static void
look_up_invitees(GTask *task)
{
  GhMlsService *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  guint n = op->people ? g_strv_length(op->people) : 0;
  if (n == 0 || n > GH_MLS_SERVICE_MAX_INVITEES) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invite 1 to %d people at a time", GH_MLS_SERVICE_MAX_INVITEES);
    g_object_unref(task);
    return;
  }
  for (guint i = 0; i < n; i++) {
    if (!lower_hex64(op->people[i]) || g_str_equal(op->people[i], self->account)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "An invitee is not another person's hex public key");
      g_object_unref(task);
      return;
    }
    if (!may_look_up(self, op->people[i])) {
      g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_CONSENT,
                              "Only accepted contacts can be invited");
      g_object_unref(task);
      return;
    }
  }
  g_auto(GStrv) sources = discovery_relays(self);
  /* Never on the group's relays (nostrc-0bdg): they would learn whom the
   * group is about to add. The new group's relays, or every relay the group
   * is read at -- an earlier address's too (nostrc-ms4d review L3). */
  const gchar *const *group_relays = op->relays ? (const gchar *const *)op->relays
                                   : op->group  ? (const gchar *const *)op->group->read_relays
                                                : NULL;
  op->adopted_packages = g_ptr_array_new_with_free_func(g_free);
  op->legacy_packages = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_set_size(op->adopted_packages, n);
  g_ptr_array_set_size(op->legacy_packages, n);
  op->lookups = n;
  for (guint i = 0; i < n; i++) {
    InviteeLookup *lookup = g_new0(InviteeLookup, 1);
    lookup->task = task;
    lookup->index = i;
    gh_mls_key_package_lookup_async(self->accounts, (const gchar *const *)sources, group_relays,
                                    op->people[i], self->lookup_deadline, self->cancellable,
                                    lookup_done, lookup);
  }
}

static GStrv
lowercase_unique(const gchar *const *items)
{
  g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; items && items[i]; i++) {
    gchar *lower = g_ascii_strdown(items[i], -1);
    if (g_ptr_array_find_with_equal_func(out, lower, g_str_equal, NULL))
      g_free(lower);
    else
      g_ptr_array_add(out, lower);
  }
  g_ptr_array_add(out, NULL);
  return (GStrv)g_ptr_array_steal(out, NULL);
}

static GTask *
op_task(GhMlsService *self, OpKind kind, GhMlsGroup *group, GCancellable *cancellable,
        GAsyncReadyCallback callback, gpointer user_data, gpointer tag)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, tag);
  Op *op = g_new0(Op, 1);
  op->kind = kind;
  op->expected = -1;
  op->group = group ? g_object_ref(group) : NULL;
  op->key_packages = g_ptr_array_new_with_free_func(g_free);
  g_task_set_task_data(task, op, op_free);
  return task;
}

static void
create_group_now(GTask *task)
{
  GhMlsService *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  guint n_relays = g_strv_length(op->relays);
  uint8_t admins[1][32];
  memcpy(admins[0], self->account_key, 32);
  MarmotGroupConfig config = {
    .name = op->name,
    .description = op->description,
    .admin_pubkeys = admins,
    .admin_count = 1,
    .relay_urls = op->relays,
    .relay_count = n_relays,
  };
  MarmotCreateGroupResult created;
  memset(&created, 0, sizeof created);
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  /* The account alone first (applied at once: nobody else sees it); the
   * invitees join through one published Add Commit. */
  MarmotError err = op->adopted
    ? marmot_create_group_for_profile(self->marmot, MARMOT_GROUP_PROFILE_ADOPTED,
                                      self->account_key, NULL, NULL, NULL, NULL, 0, &config,
                                      &created)
    : marmot_create_group(self->marmot, self->account_key, NULL, 0, &config, &created);
  if (err == MARMOT_OK)
    err = marmot_merge_pending_commit(self->marmot, &created.group->mls_group_id);
  g_autofree gchar *gid_hex = err == MARMOT_OK
    ? to_hex(created.group->mls_group_id.data, created.group->mls_group_id.len) : NULL;
  if (err != MARMOT_OK ||
      !gh_store_mls_save_room(self->store, gid_hex, op->name ? op->name : "", NULL, &error) ||
      !gh_store_commit(self->store, &error)) {
    if (err != MARMOT_OK)
      marmot_fail(self, err, "The group could not be created", &error);
    gh_store_rollback(self->store);
    marmot_create_group_result_free(&created);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  GhMlsGroup *group = ensure_group(self, &created.group->mls_group_id);
  marmot_create_group_result_free(&created);
  op->group = g_object_ref(group);
  set_floor(group, now_s(self));
  set_origin(group, ORIGIN_CREATED);   /* nostrc-8ndz */
  save_cursor(group, now_s(self));
  group_subscribe(group);
  remember_own_evidence(group, op->key_packages);
  stage_change(task, group, produce_add, op->key_packages);
}

static gboolean
relays_valid(const gchar *const *relays, GError **error)
{
  guint n = relays ? g_strv_length((gchar **)relays) : 0;
  if (n == 0 || n > MAX_GROUP_RELAYS) {
    g_set_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_RELAYS,
                "A group needs 1 to %d relays", MAX_GROUP_RELAYS);
    return FALSE;
  }
  for (guint i = 0; i < n; i++)
    if (!gh_relay_url_validate(relays[i], error))
      return FALSE;
  return TRUE;
}

static void
create_group(GhMlsService *self, const gchar *name, const gchar *description,
             const gchar *const *relays, const gchar *const *invitees, gint expected,
             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = op_task(self, OP_CREATE, NULL, cancellable, callback, user_data,
                        gh_mls_service_create_group_async);
  Op *op = g_task_get_task_data(task);
  op->expected = expected;
  GError *error = NULL;
  if (!check_running(self, &error) || !relays_valid(relays, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (!identity_ready(self)) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NOT_ENROLLED,
                            "Encrypted groups on this device wait for your signer to approve "
                            "them");
    g_object_unref(task);
    return;
  }
  /* A DM (Marmot DM, WN's shape) has no name (empty string → stored as
   * NULL, checked by group_is_dm()); a group must have one. NULL name is
   * always rejected; empty is allowed (the caller decides). */
  if (!name || (*name && !g_utf8_validate(name, -1, NULL)) ||
      (description && !g_utf8_validate(description, -1, NULL))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A group needs a name");
    g_object_unref(task);
    return;
  }
  op->name = g_strdup(name);
  op->description = g_strdup(description);
  op->relays = g_strdupv((gchar **)relays);
  op->people = lowercase_unique(invitees);
  look_up_invitees(task);
}

void
gh_mls_service_create_group_async(GhMlsService *self, const gchar *name,
                                  const gchar *description, const gchar *const *relays,
                                  const gchar *const *invitees, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  create_group(self, name, description, relays, invitees, -1, cancellable, callback, user_data);
}

void
gh_mls_service_create_group_in_format_async(GhMlsService *self, const gchar *name,
                                            const gchar *description,
                                            const gchar *const *relays,
                                            const gchar *const *invitees,
                                            GhMlsKeyPackageFormat format,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  g_return_if_fail(format < GH_MLS_KEY_PACKAGE_N_FORMATS);
  create_group(self, name, description, relays, invitees, (gint)format, cancellable, callback,
               user_data);
}


GhMlsGroup *
gh_mls_service_create_group_finish(GhMlsService *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static gboolean
check_change(GhMlsService *self, GhMlsGroup *group, GError **error)
{
  if (!check_running(self, error))
    return FALSE;
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not an active group of this account");
    return FALSE;
  }
  if (!group->is_admin) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NOT_ADMIN,
                        "Only a group admin can change the group");
    return FALSE;
  }
  return TRUE;
}

void
gh_mls_service_add_members_async(GhMlsService *self, GhMlsGroup *group,
                                 const gchar *const *invitees, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_ADD, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->people = lowercase_unique(invitees);
  for (guint i = 0; op->people[i]; i++)
    if (g_strv_contains((const gchar *const *)group->members, op->people[i])) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                              "Someone invited is already a member");
      g_object_unref(task);
      return;
    }
  look_up_invitees(task);
}

void
gh_mls_service_remove_members_async(GhMlsService *self, GhMlsGroup *group,
                                    const gchar *const *members, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_REMOVE, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->people = lowercase_unique(members);
  guint n = g_strv_length(op->people);
  g_autoptr(GArray) keys = g_array_sized_new(FALSE, TRUE, 32, n);
  for (guint i = 0; i < n; i++) {
    guint8 key[32];
    if (!lower_hex64(op->people[i]) || g_str_equal(op->people[i], self->account) ||
        !g_strv_contains((const gchar *const *)group->members, op->people[i]) ||
        !nostr_hex2bin(key, op->people[i], sizeof key)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Only other current members can be removed");
      g_object_unref(task);
      return;
    }
    g_array_append_vals(keys, key, 1);
  }
  if (n == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Nobody to remove");
    g_object_unref(task);
    return;
  }
  op->keys = g_steal_pointer(&keys);
  stage_change(task, group, produce_remove, op);
}

void
gh_mls_service_update_metadata_async(GhMlsService *self, GhMlsGroup *group, const gchar *name,
                                     const gchar *description, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_METADATA, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if ((!name && !description) || (name && (!*name || !g_utf8_validate(name, -1, NULL))) ||
      (description && !g_utf8_validate(description, -1, NULL))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Nothing valid to change");
    g_object_unref(task);
    return;
  }
  Op *op = g_task_get_task_data(task);
  op->name = g_strdup(name);
  op->description = g_strdup(description);
  stage_change(task, group, produce_metadata, op);
}

void
gh_mls_service_set_admins_async(GhMlsService *self, GhMlsGroup *group,
                                const gchar *const *admins, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_METADATA, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->people = lowercase_unique(admins);
  guint n = g_strv_length(op->people);
  g_autofree guint8 *key_bytes = g_malloc0(MAX(n, 1) * 32);
  uint8_t (*keys)[32] = (uint8_t (*)[32])key_bytes;
  for (guint i = 0; i < n; i++) {
    if (!lower_hex64(op->people[i]) ||
        !g_strv_contains((const gchar *const *)group->members, op->people[i]) ||
        !nostr_hex2bin(keys[i], op->people[i], 32)) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "Admins must be current members");
      g_object_unref(task);
      return;
    }
  }
  if (n == 0 || n > 1000) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A group has 1 to 1000 admins");
    g_object_unref(task);
    return;
  }
  op->keys = g_array_sized_new(FALSE, FALSE, 32, n);
  g_array_append_vals(op->keys, key_bytes, n);
  stage_change(task, group, produce_metadata, op);
}

gboolean
gh_mls_service_get_adopted(GhMlsService *self, GhMlsGroup *group)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  if (!running(self) || !GH_IS_MLS_GROUP(group) || group->service != self)
    return FALSE;
  MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_LEGACY;
  return marmot_get_group_profile(self->marmot, &group->gid, &profile) == MARMOT_OK &&
         profile == MARMOT_GROUP_PROFILE_ADOPTED;
}

gboolean
gh_mls_service_get_components(GhMlsService *self, GhMlsGroup *group,
                              MarmotGroupComponents *out, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  g_return_val_if_fail(out != NULL, FALSE);
  memset(out, 0, sizeof *out);
  if (!check_running(self, error))
    return FALSE;
  if (!GH_IS_MLS_GROUP(group) || group->service != self) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not a group of this account");
    return FALSE;
  }
  drop_stale_error(self);
  MarmotError err = marmot_get_group_components(self->marmot, &group->gid, out);
  if (err == MARMOT_ERR_UNSUPPORTED) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED,
                        "Groundhog reads no app components of this older kind of group");
    return FALSE;
  }
  return err == MARMOT_OK || marmot_fail(self, err, "The group's settings can't be read", error);
}

void
gh_mls_service_set_image_async(GhMlsService *self, GhMlsGroup *group,
                               const MarmotGroupBlossomImage *image, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_METADATA, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  /* A legacy (0xF2EE) group: libmarmot writes no MIP-01 image fields (its
   * group may still have a picture others set; nostrc-g5zw). */
  if (!gh_mls_service_get_adopted(self, group)) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED,
                            "Groundhog can't change the picture of this older kind of group");
    g_object_unref(task);
    return;
  }
  if (image && image->present) {
    op->image = *image;
    /* libmarmot's clear free()s it (W25 review N9). */
    op->image.media_type = image->media_type ? strdup(image->media_type) : NULL;
    if (!op->image.media_type) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              "A picture needs its type");
      g_object_unref(task);
      return;
    }
  }
  stage_change(task, group, produce_image, op);
}

#ifdef GH_MLS_TEST_HOOKS
static MarmotError
produce_media_policy(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  Op *op = data;
  char *kinds[] = { (char *)MARMOT_MEDIA_LOCATOR_BLOSSOM_V1, NULL };
  guint n = g_strv_length(op->relays);
  g_autofree MarmotMediaBlobEndpoint *endpoints = g_new0(MarmotMediaBlobEndpoint, MAX(n, 1));
  for (guint i = 0; i < n; i++) {
    endpoints[i].locator_kind = (char *)MARMOT_MEDIA_LOCATOR_BLOSSOM_V1;
    endpoints[i].base_url = op->relays[i];
  }
  MarmotGroupMediaPolicy policy = { kinds, 1, endpoints, n };
  return marmot_update_group_media_policy(marmot, gid, &policy, out);
}

void
gh_mls_service_test_set_media_policy_async(GhMlsService *self, GhMlsGroup *group,
                                           const gchar *const *endpoints,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_METADATA, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  op->relays = g_strdupv((gchar **)endpoints);
  stage_change(task, group, produce_media_policy, op);
}
#endif

void
gh_mls_service_clear_avatar_url_async(GhMlsService *self, GhMlsGroup *group,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_METADATA, group, cancellable, callback, user_data,
                        gh_mls_service_change_finish);
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!check_change(self, group, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (!gh_mls_service_get_adopted(self, group)) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED,
                            "Groundhog can't change the picture of this older kind of group");
    g_object_unref(task);
    return;
  }
  op->clear_avatar_url = TRUE;
  stage_change(task, group, produce_image, op);
}

gboolean
gh_mls_service_change_finish(GhMlsService *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- Deliveries: sends and Welcome wraps ----------------------------------------------- */

struct _Delivery {
  GhMlsService *service;
  gchar *key;                /* in self->deliveries */
  gboolean welcome;
  GhMlsGroup *group;         /* ref */
  guint8 welcome_id[32];
  gchar *recipient;          /* a Welcome's invitee */
  gint64 outbox_id;
  gint64 outbox_event_id;
  GhMessage *message;        /* a send's listed message, when there is one */
  GhRelayPublish *relay;
  gboolean accepted;
  gboolean terminal_only;    /* every answer so far was a final refusal */
};

static void
delivery_free(gpointer data)
{
  Delivery *delivery = data;
  if (delivery->relay) {
    gh_relay_publish_cancel(delivery->relay);
    gh_relay_publish_unref(delivery->relay);
  }
  g_clear_object(&delivery->group);
  g_clear_object(&delivery->message);
  g_free(delivery->recipient);
  g_free(delivery->key);
  g_free(delivery);
}

static gboolean
delivery_free_idle(gpointer data)
{
  delivery_free(data);
  return G_SOURCE_REMOVE;
}

/* From the delivery's own publish callback: out of the table now (a new
 * delivery may take its key), freed once the callback returned. */
static void
delivery_drop(Delivery *delivery)
{
  GhMlsService *self = delivery->service;
  gpointer key = NULL, value = NULL;
  if (g_hash_table_lookup_extended(self->deliveries, delivery->key, &key, &value) &&
      value == delivery) {
    g_hash_table_steal(self->deliveries, delivery->key);
    g_free(key);
  }
  g_idle_add(delivery_free_idle, delivery);
}

static void
set_status(Delivery *delivery, GhMessageStatus status)
{
  if (delivery->message)
    gh_message_set_status(delivery->message, status);
}

/* One transition; count: this ends a publish round (one attempt). */
static void
outbox_set(GhMlsService *self, gint64 outbox_id, GhStoreOutboxState state, const gchar *reason,
           gboolean count)
{
  GhStoreOutboxUpdate update = { .state = state, .last_error = reason, .count_attempt = count };
  g_autoptr(GError) error = NULL;
  if (!gh_store_outbox_update(self->store, outbox_id, &update, &error))
    g_debug("Groundhog could not update an encrypted-group outbox entry: %s", error->message);
}

/* The invitee's inbox relay accepted the Welcome: marked sent in libmarmot
 * and settled in the outbox, together. */
static void
welcome_delivered(Delivery *delivery)
{
  GhMlsService *self = delivery->service;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not record a sent invitation: %s", error->message);
    return;
  }
  MarmotError err = marmot_mark_welcomes_sent(self->marmot, &delivery->group->gid,
                                              (const uint8_t (*)[32])delivery->welcome_id, 1);
  GhStoreOutboxUpdate update = { .state = GH_STORE_OUTBOX_SETTLED, .count_attempt = TRUE };
  if (err != MARMOT_OK || !gh_store_outbox_update(self->store, delivery->outbox_id, &update,
                                                  &error) ||
      !gh_store_commit(self->store, &error)) {
    if (err != MARMOT_OK)
      marmot_fail(self, err, "Marking an invitation sent", &error);
    gh_store_rollback(self->store);
    g_message("Groundhog could not record a sent invitation: %s", error->message);
    return;
  }
  group_refresh(delivery->group);
}

static void
delivery_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  Delivery *delivery = data;
  (void)publish;
  GhMlsService *self = delivery->service;
  GhStoreTargetOutcome outcome = {
    .relay_url = result->url,
    .outcome = result->outcome,
    .ok_prefix = result->prefix == GH_RELAY_OK_PREFIX_NONE ? -1 : (gint)result->prefix,
    .ok_message = result->message,
    .count_attempt = result->outcome != GH_RELAY_PUBLISH_CANCELLED,
  };
  g_autoptr(GError) error = NULL;
  if (!gh_store_record_outcome(self->store, delivery->outbox_event_id, &outcome, &error))
    g_debug("Groundhog could not record a relay outcome: %s", error->message);
  if (result->outcome != GH_RELAY_PUBLISH_ACCEPTED) {
    if (result->outcome != GH_RELAY_PUBLISH_REJECTED &&
        result->outcome != GH_RELAY_PUBLISH_AUTH_REQUIRED)
      delivery->terminal_only = FALSE;
    return;
  }
  if (delivery->accepted)
    return;
  delivery->accepted = TRUE;
  if (delivery->welcome) {
    welcome_delivered(delivery);
  } else {
    /* One group relay has it: every member can fetch it there. */
    set_status(delivery, GH_MESSAGE_STATUS_SENT);
    outbox_set(self, delivery->outbox_id, GH_STORE_OUTBOX_PUBLISHING, NULL, FALSE);
  }
  retry_succeeded(self);
}

static void
delivery_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  Delivery *delivery = data;
  (void)publish;
  (void)summary;
  GhMlsService *self = delivery->service;
  if (delivery->accepted) {
    if (!delivery->welcome)
      outbox_set(self, delivery->outbox_id, GH_STORE_OUTBOX_SETTLED, NULL, TRUE);
  } else if (delivery->terminal_only && !delivery->welcome) {
    set_status(delivery, GH_MESSAGE_STATUS_NOT_SENT);
    outbox_set(self, delivery->outbox_id, GH_STORE_OUTBOX_NEEDS_ATTENTION, "mls-refused", TRUE);
  } else {
    set_status(delivery, GH_MESSAGE_STATUS_RETRYING);
    outbox_set(self, delivery->outbox_id, GH_STORE_OUTBOX_WAITING_RETRY, NULL, TRUE);
    schedule_retry(self);
  }
  delivery_drop(delivery);
}

/* Publishes a stored event to urls (purpose: the AUTH identity). FALSE when
 * nothing could start (the delivery is then dropped). */
static gboolean
delivery_publish(Delivery *delivery, const gchar *event_json, const gchar *const *urls,
                 GhAuthPurpose purpose)
{
  GhMlsService *self = delivery->service;
  g_autoptr(GError) error = NULL;
  delivery->terminal_only = TRUE;
  delivery->relay = gh_relay_publish_new(self->generation, event_json, delivery_update,
                                         delivery_done, delivery, &error);
  if (!delivery->relay) {
    g_warning("Groundhog cannot publish an encrypted-group event: %s", error->message);
    return FALSE;
  }
  if (self->publish_deadline)
    gh_relay_publish_set_deadline(delivery->relay, self->publish_deadline);
  guint added = 0;
  for (guint i = 0; urls && urls[i]; i++) {
    if (!gh_relay_publish_add_url(delivery->relay, urls[i], NULL))
      continue;
    gh_auth_policy_apply_publish(self->policy, delivery->relay, purpose, urls[i], NULL);
    added++;
  }
  if (!added || !gh_relay_publish_start(delivery->relay, &error)) {
    gh_relay_publish_unref(g_steal_pointer(&delivery->relay));
    return FALSE;
  }
  set_status(delivery, GH_MESSAGE_STATUS_SENDING);
  outbox_set(self, delivery->outbox_id, GH_STORE_OUTBOX_PUBLISHING, NULL, FALSE);
  return TRUE;
}

static Delivery *
delivery_new(GhMlsService *self, const gchar *key, GhMlsGroup *group, gboolean welcome)
{
  Delivery *delivery = g_new0(Delivery, 1);
  delivery->service = self;
  delivery->key = g_strdup(key);
  delivery->group = g_object_ref(group);
  delivery->welcome = welcome;
  g_hash_table_replace(self->deliveries, g_strdup(key), delivery);
  return delivery;
}

static void
delivery_abandon(Delivery *delivery)
{
  g_hash_table_remove(delivery->service->deliveries, delivery->key);
}

/* The targets of a stored event that did not accept it yet. */
static GStrv
unaccepted_targets(GhStoreOutboxEvent *event)
{
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  for (guint i = 0; i < event->targets->len; i++) {
    GhStoreOutboxTarget *target = g_ptr_array_index(event->targets, i);
    if (target->outcome != OUTCOME_ACCEPTED && target->outcome != OUTCOME_REJECTED)
      g_strv_builder_add(out, target->relay_url);
  }
  return g_strv_builder_end(out);
}

static gchar *
outbox_key(gint64 outbox_id)
{
  return g_strdup_printf("send/%" G_GINT64_FORMAT, outbox_id);
}

/* ---- Sending ------------------------------------------------------------------------------ */

/* The canonical unsigned kind-9 inner event of a chat message, tagged
 * ["h", <the group's nostr group id>] (review M2: Marmot leaves inner tags
 * to the application; this one makes the same text sent to two groups in
 * the same second two events, which libmarmot, the store and the model all
 * tell apart by id). */
static gchar *
inner_event_new(GhMlsService *self, GhMlsGroup *group, const gchar *text,
                GPtrArray *imeta_tags, gchar **out_id)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, GH_MESSAGE_MLS_KIND);
  nostr_event_set_pubkey(event, self->account);
  nostr_event_set_created_at(event, now_s(self));
  nostr_event_set_content(event, text);
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("h", group->nostr_hex, NULL));
  /* One ordered imeta per file, as MDK's kind-9 media messages. */
  for (guint i = 0; imeta_tags && i < imeta_tags->len; i++) {
    const gchar *const *fields = g_ptr_array_index(imeta_tags, i);
    NostrTag *tag = nostr_tag_new(fields[0], NULL);
    for (guint j = 1; fields[j]; j++)
      nostr_tag_append(tag, fields[j]);
    nostr_tags_append(tags, tag);
  }
  nostr_event_set_tags(event, tags);
  gchar id[65] = { 0 };
  gchar *json = NULL;
  if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
    free(event->id);
    event->id = strdup(id);
    char *serialized = nostr_event_serialize_compact(event);
    json = g_strdup(serialized);
    free(serialized);
    *out_id = g_strdup(id);
  }
  nostr_event_free(event);
  return json;
}

static gchar *
event_id_of(const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  gboolean ok = event && nostr_event_deserialize_compact(event, json, NULL) == 1 &&
                nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  if (event)
    nostr_event_free(event);
  return ok ? g_strdup(id) : NULL;
}

/* A committed send: listed with its status, then published. */
static void
send_listed(GhMlsService *self, GhMlsGroup *group, GhMessage *message, gint64 outbox_id,
            GhStoreOutboxEntry *entry, const gchar *event_json)
{
  gh_message_set_status(message, GH_MESSAGE_STATUS_SENDING);
  if (account_matches_model(self)) {
    g_autoptr(GError) listed = NULL;
    if (gh_conversation_store_add_message(self->conversations, message, &listed) ==
        GH_CONVERSATION_ADD_FAILED)
      g_message("Groundhog could not list a sent group message: %s", listed->message);
  }
  g_autofree gchar *key = outbox_key(outbox_id);
  Delivery *delivery = delivery_new(self, key, group, FALSE);
  delivery->outbox_id = outbox_id;
  delivery->outbox_event_id = entry && entry->events->len
    ? ((GhStoreOutboxEvent *)g_ptr_array_index(entry->events, 0))->id : 0;
  delivery->message = g_object_ref(message);
  if (!delivery->outbox_event_id ||
      !delivery_publish(delivery, event_json, (const gchar *const *)group->relays,
                        GH_AUTH_PURPOSE_MLS_ROUTING)) {
    set_status(delivery, GH_MESSAGE_STATUS_RETRYING);
    delivery_abandon(delivery);
    schedule_retry(self);
  }
}

/* Every imeta tag a well-formed list of fields: "imeta" first, then 1 to
 * 64 non-empty UTF-8 fields (libmarmot built them; this guards the API). */
static gboolean
imeta_tags_valid(GPtrArray *tags)
{
  if (!tags)
    return TRUE;
  if (tags->len > GH_MLS_IMETA_MAX_ATTACHMENTS)
    return FALSE;
  for (guint i = 0; i < tags->len; i++) {
    const gchar *const *fields = g_ptr_array_index(tags, i);
    guint n = fields ? g_strv_length((gchar **)fields) : 0;
    if (n < 2 || n > 64 || g_strcmp0(fields[0], "imeta") != 0)
      return FALSE;
    for (guint k = 1; k < n; k++)
      if (!*fields[k] || !g_utf8_validate(fields[k], -1, NULL))
        return FALSE;
  }
  return TRUE;
}

static GhMessage *
send_inner(GhMlsService *self, GhMlsGroup *group, const gchar *text, GPtrArray *imeta_tags,
           guint64 source_epoch, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!check_running(self, error))
    return NULL;
  gboolean files = imeta_tags && imeta_tags->len > 0;
  /* Leaving (nostrc-2um6): nothing but the leave is sent any more. */
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active ||
      group->leaving || !text || (!*text && !files) || !g_utf8_validate(text, -1, NULL) ||
      !imeta_tags_valid(imeta_tags)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A message needs text or files and an active group it is not leaving");
    return NULL;
  }
  if (!group->relays[0]) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_RELAYS,
                        "The group has no relays");
    return NULL;
  }
  g_autofree gchar *inner_id = NULL;
  g_autofree gchar *inner = inner_event_new(self, group, text, imeta_tags, &inner_id);
  g_autoptr(GhMessage) message = inner ? gh_message_new_from_mls(self->account, group->gid_hex,
                                                                 inner, error) : NULL;
  if (!message)
    return NULL;
  /* The sender's own cards, and the cache identities bound to the row. */
  if (files)
    gh_mls_imeta_describe(message, group->gid_hex, TRUE, source_epoch);
  g_auto(GStrv) media_ids = files ? gh_mls_imeta_dup_file_ids(message) : NULL;
  if (files && gh_message_get_rejected_attachments(message) > 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A file reference is not valid encrypted media");
    return NULL;
  }
  g_autofree gchar *op_id = gh_store_new_op_id();
  gint64 conversation_id = 0, outbox_id = 0, message_id = 0;
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  g_autofree gchar *envelope_id = NULL;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, error))
    return NULL;
  GhStoreOutgoing outgoing = {
    .op_id = op_id,
    .backend_msg_id = inner_id,
    .sender_pubkey = self->account,
    .kind = GH_MESSAGE_MLS_KIND,
    .created_at = gh_message_get_created_at(message),
    .body = text,
    .rumor_json = inner,
    .has_mls_epoch = files,
    .mls_epoch = files ? source_epoch : 0,
    .media_ids = (const gchar *const *)media_ids,
  };
  GhStoreSealedEvent sealed = {
    .role = GH_STORE_OUTBOX_ROLE_MLS_MESSAGE,
    .relay_urls = (const gchar *const *)group->relays,
  };
  MarmotError err = MARMOT_OK;
  if (!gh_store_mls_save_room(self->store, group->gid_hex, NULL, &conversation_id, error))
    goto fail;
  outgoing.conversation_id = conversation_id;
  if (!gh_store_enqueue(self->store, &outgoing, &outbox_id, &message_id, error))
    goto fail;
  /* Media is bound to the epoch it was sealed in (MIP-04 v2): the check and
   * the ratchet step below are one transaction in one main-loop turn, so a
   * Commit that arrived during the upload is caught here and nothing stale
   * is stored or sent (gh-mls-media.h step 4; review L3). */
  if (files) {
    err = marmot_media_check_epoch(self->marmot, &group->gid, source_epoch);
    if (err == MARMOT_ERR_MEDIA_EPOCH_CHANGED) {
      g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_EPOCH_CHANGED,
                          "The group changed while the files were uploading");
      goto fail;
    }
    if (err != MARMOT_OK) {
      marmot_fail(self, err, "The files could not be sent", error);
      goto fail;
    }
  }
  /* The sender ratchet step, in this transaction (libmarmot 0.8.0). */
  err = marmot_create_message(self->marmot, &group->gid, inner, &out);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "The message could not be encrypted", error);
    goto fail;
  }
  /* Kept with its epoch in libmarmot's store, as a received message is, so
   * a branch change that withdraws it reports it (nostrc-xrza). */
  err = marmot_save_created_message(self->marmot, &group->gid, out.event_json,
                                    out.message && out.message->content ? out.message->content
                                                                        : inner);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "The message could not be stored", error);
    goto fail;
  }
  envelope_id = event_id_of(out.event_json);
  sealed.event_id = envelope_id;
  sealed.event_json = out.event_json;
  if (!envelope_id || !gh_store_seal(self->store, outbox_id, &sealed, 1, error))
    goto fail;
  /* Durable before anything is published: never roll back a step that is
   * out (gh-store-marmot.h "Durability when nested"). */
  if (!gh_store_commit(self->store, error)) {
    marmot_outgoing_message_free(&out);
    return NULL;
  }
  entry = gh_store_outbox_load(self->store, outbox_id, NULL);
  send_listed(self, group, message, outbox_id, entry, out.event_json);
  marmot_outgoing_message_free(&out);
  return g_steal_pointer(&message);

fail:
  gh_store_rollback(self->store);
  marmot_outgoing_message_free(&out);
  return NULL;
}

GhMessage *
gh_mls_service_send(GhMlsService *self, GhMlsGroup *group, const gchar *text,
                    GError **error)
{
  return send_inner(self, group, text, NULL, 0, error);
}

GhMessage *
gh_mls_service_send_with_imeta(GhMlsService *self, GhMlsGroup *group, const gchar *caption,
                               GPtrArray *imeta_tags, guint64 source_epoch, GError **error)
{
  if (!imeta_tags || imeta_tags->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No files to send");
    return NULL;
  }
  return send_inner(self, group, caption ? caption : "", imeta_tags, source_epoch, error);
}

/* The id and kind of a stored unsigned event (a rumor or inner event). */
static gchar *
unsigned_id_of(const gchar *json, gint *out_kind)
{
  NostrEvent *event = json ? nostr_event_new() : NULL;
  gchar id[65] = { 0 };
  gboolean ok = event &&
                nostr_event_deserialize_unsigned(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                (event->id ? nostr_event_validate_id(event, id)
                           : nostr_event_compute_id(event, id)) == NOSTR_EVENT_VALIDATION_OK;
  if (ok && out_kind)
    *out_kind = nostr_event_get_kind(event);
  if (event)
    nostr_event_free(event);
  return ok ? g_strdup(id) : NULL;
}

/* After a restart or a failed round: the stored kind 445, byte for byte. */
static void
send_resume(GhMlsService *self, GhStoreOutboxEntry *entry, GhMlsGroup *group)
{
  g_autofree gchar *key = outbox_key(entry->id);
  if (!group || !group->active || g_hash_table_contains(self->deliveries, key) ||
      entry->events->len != 1)
    return;
  GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, 0);
  if (event->role != GH_STORE_OUTBOX_ROLE_MLS_MESSAGE || !event->event_json)
    return;
  g_auto(GStrv) urls = unaccepted_targets(event);
  if (!urls[0])
    return;
  Delivery *delivery = delivery_new(self, key, group, FALSE);
  delivery->outbox_id = entry->id;
  delivery->outbox_event_id = event->id;
  if (account_matches_model(self)) {
    g_autofree gchar *inner_id = unsigned_id_of(entry->rumor_json, NULL);
    GhMessage *listed = inner_id ? gh_conversation_store_lookup_message(self->conversations,
                                                                        inner_id) : NULL;
    if (listed)
      delivery->message = g_object_ref(listed);
  }
  if (!delivery_publish(delivery, event->event_json, (const gchar *const *)urls,
                        GH_AUTH_PURPOSE_MLS_ROUTING))
    delivery_abandon(delivery);
}

/* ---- Welcomes out (MIP-02) ------------------------------------------------------------- */

/* libmarmot's kind-444 rumor with the account as its author (the seal binds
 * the rumor's author, NIP-59) and its canonical id. */
static gchar *
welcome_rumor(GhMlsService *self, const gchar *marmot_rumor, gchar **out_id)
{
  NostrEvent *event = nostr_event_new();
  gchar *json = NULL;
  gchar id[65] = { 0 };
  if (event && nostr_event_deserialize_compact(event, marmot_rumor, NULL) == 1 &&
      nostr_event_get_kind(event) == GH_NIP17_WELCOME_KIND && !event->sig) {
    nostr_event_set_pubkey(event, self->account);
    free(event->id);
    event->id = NULL;
    if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
      event->id = strdup(id);
      char *serialized = nostr_event_serialize_compact(event);
      json = g_strdup(serialized);
      free(serialized);
      *out_id = g_strdup(id);
    }
  }
  if (event)
    nostr_event_free(event);
  return json;
}

/* The outbox op_id of a Welcome (stable: a repeat is idempotent). */
static gchar *
welcome_op_id(const guint8 id[32])
{
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  static const guchar domain[] = "groundhog/mls-welcome/v1";
  g_checksum_update(sum, domain, sizeof domain);
  g_checksum_update(sum, id, 32);
  return g_strndup(g_checksum_get_string(sum), 32);
}

typedef struct {
  Delivery *delivery;        /* in self->deliveries while this runs */
  gchar *key;                /* its key */
  GWeakRef service;
  guint64 run;
  gchar *rumor_json;
  gchar *rumor_id;
  GStrv inbox;
} WelcomeJob;

static void
welcome_job_free(WelcomeJob *job)
{
  g_weak_ref_clear(&job->service);
  g_free(job->key);
  g_free(job->rumor_json);
  g_free(job->rumor_id);
  g_strfreev(job->inbox);
  g_free(job);
}

/* The job's service while its generation still runs; NULL otherwise (the
 * delivery then died with the generation). */
static GhMlsService *
welcome_job_service(WelcomeJob *job)
{
  GhMlsService *self = g_weak_ref_get(&job->service);
  if (self && (!running(self) || self->run != job->run ||
               g_hash_table_lookup(self->deliveries, job->key) != job->delivery))
    g_clear_object(&self);
  return self;
}

static void
welcome_sealed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  WelcomeJob *job = data;
  g_autoptr(GError) error = NULL;
  GhNip17Envelope *envelope = gh_nip17_envelope_build_finish(result, &error);
  g_autoptr(GhMlsService) self = welcome_job_service(job);
  if (!self) {
    gh_nip17_envelope_free(envelope);
    welcome_job_free(job);
    return;
  }
  Delivery *delivery = job->delivery;
  if (!envelope) {
    g_message("Groundhog could not seal an invitation: %s", error->message);
    delivery_abandon(delivery);
    welcome_job_free(job);
    schedule_retry(self);
    return;
  }
  const gchar *wrap = envelope->recipient_wrap_json;
  g_autofree gchar *wrap_id = wrap ? event_id_of(wrap) : NULL;
  g_autofree gchar *op_id = welcome_op_id(delivery->welcome_id);
  gint64 conversation_id = 0, message_id = 0;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  gboolean stored = wrap_id && gh_store_begin(self->store, &error);
  if (stored) {
    GhStoreOutgoing outgoing = {
      .op_id = op_id,
      .backend_msg_id = job->rumor_id,
      .sender_pubkey = self->account,
      .kind = GH_NIP17_WELCOME_KIND,
      .created_at = now_s(self),
      .rumor_json = job->rumor_json,
    };
    GhStoreSealedEvent sealed = {
      .role = GH_STORE_OUTBOX_ROLE_WELCOME_WRAP,
      .target_pubkey = delivery->recipient,
      .event_id = wrap_id,
      .event_json = wrap,
      .relay_urls = (const gchar *const *)job->inbox,
    };
    stored = gh_store_mls_save_room(self->store, delivery->group->gid_hex, NULL,
                                    &conversation_id, &error);
    outgoing.conversation_id = conversation_id;
    stored = stored &&
             gh_store_enqueue(self->store, &outgoing, &delivery->outbox_id, &message_id,
                              &error) &&
             gh_store_seal(self->store, delivery->outbox_id, &sealed, 1, &error) &&
             gh_store_commit(self->store, &error);
    if (!stored)
      gh_store_rollback(self->store);
  }
  if (stored)
    entry = gh_store_outbox_load(self->store, delivery->outbox_id, &error);
  if (!entry || entry->events->len != 1) {
    g_message("Groundhog could not store an invitation: %s",
              error ? error->message : "not sealed");
    delivery_abandon(delivery);
  } else {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, 0);
    delivery->outbox_event_id = event->id;
    /* §4.3: a recipient's inbox never learns the sender (ephemeral AUTH). */
    if (!delivery_publish(delivery, event->event_json, (const gchar *const *)job->inbox,
                          GH_AUTH_PURPOSE_RECIPIENT_WRAP))
      delivery_abandon(delivery);
  }
  gh_nip17_envelope_free(envelope);
  welcome_job_free(job);
}

static void
welcome_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  WelcomeJob *job = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhInboxResult) inbox = gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source),
                                                                   result, &error);
  g_autoptr(GhMlsService) self = welcome_job_service(job);
  if (!self) {
    welcome_job_free(job);
    return;
  }
  if (!inbox || inbox->status != GH_INBOX_FOUND || !inbox->relays || !inbox->relays[0]) {
    /* Charter §4.3: only the invitee's 10050, never a fallback. It stays
     * unsent; an unreachable lookup is asked again later. */
    g_message("Groundhog cannot deliver an invitation yet: %s",
              error ? error->message
                    : "the invitee has no inbox relays (kind 10050)");
    if (!inbox || inbox->status == GH_INBOX_UNREACHABLE)
      schedule_retry(self);
    delivery_abandon(job->delivery);
    welcome_job_free(job);
    return;
  }
  job->inbox = g_strdupv(inbox->relays);
  gh_nip17_envelope_seal_welcome_async(self->accounts, job->rumor_json,
                                       job->delivery->recipient, self->cancellable,
                                       welcome_sealed, job);
}

static void
welcome_start(GhMlsGroup *group, const MarmotUnsentWelcome *welcome)
{
  GhMlsService *self = group->service;
  g_autofree gchar *key = to_hex(welcome->id, 32);
  if (g_hash_table_contains(self->deliveries, key))
    return;
  g_autofree gchar *rumor_id = NULL;
  g_autofree gchar *rumor = welcome_rumor(self, welcome->rumor_json, &rumor_id);
  if (!rumor)
    return;
  Delivery *delivery = delivery_new(self, key, group, TRUE);
  memcpy(delivery->welcome_id, welcome->id, 32);
  delivery->recipient = to_hex(welcome->recipient, 32);
  /* Sealed before (a restart): the stored wrap, byte for byte. */
  gint64 conversation_id = 0, outbox_id = 0;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  if (gh_store_find_conversation(self->store, GH_STORE_BACKEND_MLS, group->gid_hex,
                                 &conversation_id, NULL) &&
      gh_store_outbox_find_by_rumor(self->store, conversation_id, rumor_id, &outbox_id, NULL))
    entry = gh_store_outbox_load(self->store, outbox_id, NULL);
  if (entry && entry->events->len == 1) {
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, 0);
    delivery->outbox_id = entry->id;
    delivery->outbox_event_id = event->id;
    g_auto(GStrv) urls = unaccepted_targets(event);
    if (entry->state == GH_STORE_OUTBOX_SETTLED || !urls[0] || !event->event_json) {
      delivery_abandon(delivery);   /* delivered, or nowhere left to try */
      return;
    }
    if (!delivery_publish(delivery, event->event_json, (const gchar *const *)urls,
                          GH_AUTH_PURPOSE_RECIPIENT_WRAP))
      delivery_abandon(delivery);
    return;
  }
  if (!self->inboxes) {
    delivery_abandon(delivery);
    return;
  }
  WelcomeJob *job = g_new0(WelcomeJob, 1);
  job->delivery = delivery;
  job->key = g_strdup(delivery->key);
  g_weak_ref_init(&job->service, self);
  job->run = self->run;
  job->rumor_json = g_steal_pointer(&rumor);
  job->rumor_id = g_steal_pointer(&rumor_id);
  gh_inbox_resolver_resolve_async(self->inboxes, delivery->recipient, self->cancellable,
                                  welcome_resolved, job);
}

/* Every Welcome of a merged Add not yet delivered (libmarmot's outbox). */
static void
welcomes_pump(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  if (!running(self) || !group->active)
    return;
  MarmotUnsentWelcome *welcomes = NULL;
  size_t n = 0;
  if (marmot_get_unsent_welcomes(self->marmot, &group->gid, &welcomes, &n) != MARMOT_OK) {
    drop_stale_error(self);
    return;
  }
  for (size_t i = 0; i < n; i++)
    welcome_start(group, &welcomes[i]);
  marmot_unsent_welcomes_free(welcomes, n);
}

/* ---- Welcomes in: the inbox sink and invitations -------------------------------------- */

static gboolean
welcome_sink(gpointer data, const GhNip17Message *welcome, const gchar *relay_url,
             GError **error)
{
  GhMlsService *self = data;
  (void)relay_url;
  guint8 wrapper[32];
  if (!lower_hex64(welcome->wrap_id) || !nostr_hex2bin(wrapper, welcome->wrap_id, 32)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bad wrap id");
    return FALSE;
  }
  drop_stale_error(self);
  if (!gh_store_begin(self->store, error))
    return FALSE;
  MarmotWelcome *stored = NULL;
  MarmotError err = marmot_process_welcome(self->marmot, wrapper, welcome->rumor_json, &stored);
  marmot_welcome_free(stored);
  /* A Welcome libmarmot refuses is a verdict too: it is recorded (as failed)
   * and its wrap is seen, so no signer is asked about it again. A storage
   * failure is not: nothing is recorded, a later session retries. */
  GError *store_error = gh_store_marmot_take_error(self->storage);
  if (store_error) {
    g_propagate_error(error, store_error);
    gh_store_rollback(self->store);
    return FALSE;
  }
  /* When the Welcome was made (its seal-authenticated rumor's created_at,
   * bounded): the joined group is read from then (review M3). */
  g_autofree gchar *time_scope = welcome_time_scope(wrapper);
  gint64 made = CLAMP(welcome->created_at, 1, now_s(self));
  if ((err == MARMOT_OK &&
       !gh_store_set_cursor(self->store, time_scope, "", made, error)) ||
      !gh_store_seen_add(self->store, GH_STORE_SEEN_WRAP, welcome->wrap_id, error) ||
      !gh_store_commit(self->store, error)) {
    gh_store_rollback(self->store);
    return FALSE;
  }
  if (err == MARMOT_OK)
    g_signal_emit(self, signals[SIGNAL_INVITE_RECEIVED], 0, welcome->wrap_id);
  else
    g_debug("Groundhog refused an encrypted-group invitation: %s", marmot_error_string(err));
  return TRUE;
}

static GhMlsInvite *
invite_of(const MarmotWelcome *welcome)
{
  GhMlsInvite *invite = g_new0(GhMlsInvite, 1);
  invite->wrapper_id = to_hex(welcome->wrapper_event_id, 32);
  invite->inviter = to_hex(welcome->welcomer, 32);
  invite->group_name = g_strdup(welcome->group_name);
  invite->member_count = welcome->member_count;
  g_autoptr(GStrvBuilder) relays = g_strv_builder_new();
  for (size_t i = 0; i < welcome->group_relay_count; i++)
    if (welcome->group_relays[i])
      g_strv_builder_add(relays, welcome->group_relays[i]);
  invite->relays = g_strv_builder_end(relays);
  /* The DM shape: no name and exactly two members (the inviter and this
   * account). White Noise creates DMs as 2-member groups with an empty name. */
  invite->is_dm = (!welcome->group_name || !*welcome->group_name) &&
                  welcome->member_count == 2;
  return invite;
}

GPtrArray *
gh_mls_service_list_invites(GhMlsService *self, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  MarmotWelcome **welcomes = NULL;
  size_t n = 0;
  MarmotPagination page = marmot_pagination_default();
  page.limit = 1000;
  drop_stale_error(self);
  MarmotError err = marmot_get_pending_welcomes(self->marmot, &page, &welcomes, &n);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Reading the invitations", error);
    return NULL;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_mls_invite_free);
  for (size_t i = 0; i < n; i++) {
    if (welcomes[i]->state == MARMOT_WELCOME_STATE_PENDING)
      g_ptr_array_add(out, invite_of(welcomes[i]));
    marmot_welcome_free(welcomes[i]);
  }
  free(welcomes);
  return out;
}

static MarmotWelcome *
pending_welcome(GhMlsService *self, const gchar *wrapper_id, GError **error)
{
  MarmotWelcome **welcomes = NULL;
  size_t n = 0;
  MarmotPagination page = marmot_pagination_default();
  page.limit = 1000;
  MarmotError err = marmot_get_pending_welcomes(self->marmot, &page, &welcomes, &n);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Reading the invitations", error);
    return NULL;
  }
  MarmotWelcome *found = NULL;
  for (size_t i = 0; i < n; i++) {
    g_autofree gchar *hex = to_hex(welcomes[i]->wrapper_event_id, 32);
    if (!found && welcomes[i]->state == MARMOT_WELCOME_STATE_PENDING &&
        g_strcmp0(hex, wrapper_id) == 0)
      found = welcomes[i];
    else
      marmot_welcome_free(welcomes[i]);
  }
  free(welcomes);
  if (!found)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such invitation");
  return found;
}

static void key_package_rotate(GhMlsService *self, gint format);
static void key_package_maybe_publish(GhMlsService *self);

typedef enum { INVITES_NONE, INVITES_PENDING, INVITES_UNKNOWN } InvitesPending;

/* Whether a received invitation is still pending (neither accepted nor
 * declined nor refused). A listing that fails is UNKNOWN: callers treat it
 * as pending (review L1), never as "none". */
#ifdef GH_MLS_TEST_HOOKS
static gboolean test_fail_invitation_listing;

void
gh_mls_service_test_fail_invitation_listing(gboolean fail)
{
  test_fail_invitation_listing = fail;
}
#endif

static InvitesPending
invitations_pending(GhMlsService *self)
{
  MarmotWelcome **welcomes = NULL;
  size_t n = 0;
  MarmotPagination page = marmot_pagination_default();
  page.limit = 1000;
  drop_stale_error(self);
  MarmotError listed = marmot_get_pending_welcomes(self->marmot, &page, &welcomes, &n);
#ifdef GH_MLS_TEST_HOOKS
  if (test_fail_invitation_listing && listed == MARMOT_OK) {
    for (size_t i = 0; i < n; i++)
      marmot_welcome_free(welcomes[i]);
    free(welcomes);
    welcomes = NULL;
    n = 0;
    listed = MARMOT_ERR_STORAGE;   /* as a storage that cannot be read */
  }
#endif
  if (listed != MARMOT_OK) {
    drop_stale_error(self);
    return INVITES_UNKNOWN;
  }
  gboolean pending = FALSE;
  for (size_t i = 0; i < n; i++) {
    pending |= welcomes[i]->state == MARMOT_WELCOME_STATE_PENDING;
    marmot_welcome_free(welcomes[i]);
  }
  free(welcomes);
  return pending ? INVITES_PENDING : INVITES_NONE;
}

GhMlsGroup *
gh_mls_service_accept_invite(GhMlsService *self, const gchar *wrapper_id, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!check_running(self, error))
    return NULL;
  guint8 wrapper[32];
  if (!lower_hex64(wrapper_id) || !nostr_hex2bin(wrapper, wrapper_id, 32)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Bad invitation id");
    return NULL;
  }
  drop_stale_error(self);
  MarmotGroup *joined = NULL;
  if (!gh_store_begin(self->store, error))
    return NULL;
  MarmotError err = marmot_accept_welcome_by_wrapper_id(self->marmot, wrapper, &joined);
  if (err != MARMOT_OK || !joined) {
    marmot_group_free(joined);
    /* A Welcome whose group address another group of ours has or had
     * (nostrc-scki): libmarmot refused it for good. Only here does this
     * error mean that (review N2). */
    if (err == MARMOT_ERR_PROTOCOL_GROUP_MISMATCH) {
      drop_stale_error(self);
      g_set_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_ADDRESS_TAKEN,
                  "The invitation could not be accepted: %s", marmot_error_string(err));
    } else {
      marmot_fail(self, err != MARMOT_OK ? err : MARMOT_ERR_WELCOME_NOT_FOUND,
                  "The invitation could not be accepted", error);
    }
    /* libmarmot keeps its deliberate outcomes (a retired duplicate). */
    if (!gh_store_commit(self->store, NULL))
      gh_store_rollback(self->store);
    /* The invitation may have left the pending state: a replacement held
     * for it is looked at again (review M1). A failed Welcome never asks
     * for a rotation itself. */
    key_package_maybe_publish(self);
    return NULL;
  }
  g_autofree gchar *gid_hex = to_hex(joined->mls_group_id.data, joined->mls_group_id.len);
  if (!gh_store_mls_save_room(self->store, gid_hex, joined->name ? joined->name : "", NULL,
                              error) ||
      !gh_store_commit(self->store, error)) {
    gh_store_rollback(self->store);
    marmot_group_free(joined);
    return NULL;
  }
  GhMlsGroup *group = ensure_group(self, &joined->mls_group_id);
  marmot_group_free(joined);
  group_refresh(group);
  if (group->active)
    group_list_room(group);
  /* Read from when the Welcome was made, however late it is accepted
   * (review M3); without that record, a little back from now. The record
   * goes: the invitation is used. */
  g_autofree gchar *time_scope = welcome_time_scope(wrapper);
  gint64 made = 0;
  if (!gh_store_get_cursor(self->store, time_scope, "", &made, NULL) || made <= 0)
    made = now_s(self) - JOIN_BACKFILL;
  gh_store_set_cursor(self->store, time_scope, "", 0, NULL);
  if (group->cursor == 0)
    save_cursor(group, MAX(made, 1));
  if (group->floor == 0)
    set_floor(group, made);
  set_origin(group, ORIGIN_JOINED);   /* never "created here" (nostrc-8ndz) */
  group_subscribe(group);
  /* MIP-00: the KeyPackage the Welcome used is spent; publish a new one of
   * its format (held while other invitations are pending:
   * key_package_hold_verdict()). libmarmot recorded which one it opened the
   * Welcome with (review N1); unknown (it predates the record), the group's
   * profile says. */
  MarmotKeyPackageProfile used = group->adopted ? MARMOT_KEY_PACKAGE_PROFILE_ADOPTED
                                                : MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8;
  drop_stale_error(self);
  if (marmot_key_package_last_used_profile(self->marmot, self->account_key, &used) != MARMOT_OK)
    drop_stale_error(self);
  key_package_rotate(self, used == MARMOT_KEY_PACKAGE_PROFILE_ADOPTED
                             ? GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED
                             : GH_MLS_KEY_PACKAGE_FORMAT_LEGACY);
  return group;
}

gboolean
gh_mls_service_decline_invite(GhMlsService *self, const gchar *wrapper_id, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  drop_stale_error(self);
  MarmotWelcome *welcome = pending_welcome(self, wrapper_id, error);
  if (!welcome)
    return FALSE;
  MarmotError err = marmot_decline_welcome(self->marmot, welcome);
  marmot_welcome_free(welcome);
  if (err != MARMOT_OK)
    return marmot_fail(self, err, "The invitation could not be declined", error);
  /* A replacement held for pending invitations, looked at again. */
  key_package_maybe_publish(self);
  return TRUE;
}

/* ---- Verify (W24 review H1) ----------------------------------------------------------- */

typedef struct {
  GhMlsGroup *group;   /* a reference */
  gchar *account;
  guint64 run;
} VerifyOp;

static void
verify_op_free(gpointer data)
{
  VerifyOp *op = data;
  g_clear_object(&op->group);
  g_free(op->account);
  g_free(op);
}

/* The account's devices in every group whose state is `from` become `to`. */
static void
verify_mark(GhMlsService *self, const gchar *account, GhMlsMemberIdentity from,
            GhMlsMemberIdentity to)
{
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    gboolean changed = FALSE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, group->devices);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Device *device = value;
      if (device->state == from && g_strcmp0(device->account, account) == 0) {
        device->state = to;
        changed = TRUE;
      }
    }
    if (changed) {
      sync_unverified(group);
      g_signal_emit(group, group_signals[GROUP_SIGNAL_MEMBERS_CHANGED], 0);
    }
  }
}

static void
verify_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *task = data;
  GhMlsService *self = g_task_get_source_object(task);
  VerifyOp *op = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) found = gh_mls_key_package_evidence_lookup_finish(result, &error);
  if (self->disposed || op->run != self->run) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The account changed before the check finished");
    g_object_unref(task);
    return;
  }
  g_hash_table_remove(self->verifying, op->account);
  for (guint i = 0; found && i < found->len; i++)
    remember_key_package(self, g_ptr_array_index(found, i));
  /* Every device of the account the answer confirms, in every group; the
   * asked group's others are recorded as checked (relays answered). */
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    gboolean changed = FALSE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, group->devices);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Device *device = value;
      if ((device->state != GH_MLS_MEMBER_CHECKING &&
           device->state != GH_MLS_MEMBER_UNVERIFIED) ||
          g_strcmp0(device->account, op->account) != 0)
        continue;
      if (device_matches(device, found)) {
        device->state = GH_MLS_MEMBER_VERIFIED;
        device_save(group, device, GH_STORE_MLS_MEMBER_VERIFIED);
      } else {
        device->state = GH_MLS_MEMBER_UNVERIFIED;
        if (found && group == op->group)
          device_save(group, device, GH_STORE_MLS_MEMBER_NOT_FOUND);
      }
      changed = TRUE;
    }
    if (changed) {
      sync_unverified(group);
      g_signal_emit(group, group_signals[GROUP_SIGNAL_MEMBERS_CHANGED], 0);
    }
  }
  if (!found) {
    g_task_return_error(task, g_steal_pointer(&error));
  } else {
    g_task_return_int(task, gh_mls_group_get_member_identity(op->group, op->account, NULL));
  }
  g_object_unref(task);
}

void
gh_mls_service_verify_member_async(GhMlsService *self, GhMlsGroup *group, const gchar *member,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_service_verify_member_async);
  GError *error = NULL;
  g_autofree gchar *account = member ? g_ascii_strdown(member, -1) : NULL;
  if (!check_running(self, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  Device *device = GH_IS_MLS_GROUP(group) && group->service == self && lower_hex64(account)
                     ? weakest_device(group, account) : NULL;
  if (!device || device->state == GH_MLS_MEMBER_PROVEN) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Not a member of this group whose identity needs checking");
    g_object_unref(task);
    return;
  }
  if (g_hash_table_contains(self->verifying, account)) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_BUSY,
                            "This person is being checked already");
    g_object_unref(task);
    return;
  }
  /* The discovery relays, then the person's own write relays: never the
   * group's relays, the one place a lookup could be tied to the group --
   * not even when a discovery or write relay is one of them (W24 review
   * A1; the lookup drops them in both phases). */
  g_auto(GStrv) all = discovery_relays(self);
  if (!all[0]) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "No discovery relay to look KeyPackages up on");
    g_object_unref(task);
    return;
  }
  g_autoptr(GHashTable) group_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                             NULL);
  /* Earlier routing's relays are group relays too (nostrc-ms4d). */
  for (guint i = 0; group->read_relays && group->read_relays[i]; i++)
    g_hash_table_add(group_relays, gh_mls_relay_key(group->read_relays[i]));
  g_autoptr(GStrvBuilder) usable = g_strv_builder_new();
  for (guint i = 0; all[i]; i++) {
    g_autofree gchar *key = gh_mls_relay_key(all[i]);
    if (!g_hash_table_contains(group_relays, key))
      g_strv_builder_add(usable, all[i]);
  }
  g_auto(GStrv) relays = g_strv_builder_end(usable);
  if (!relays[0]) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Every relay to ask is one of this group's relays");
    g_object_unref(task);
    return;
  }
  VerifyOp *op = g_new0(VerifyOp, 1);
  op->group = g_object_ref(group);
  op->account = g_strdup(account);
  op->run = self->run;
  g_task_set_task_data(task, op, verify_op_free);
  g_hash_table_add(self->verifying, g_strdup(account));
  verify_mark(self, account, GH_MLS_MEMBER_UNVERIFIED, GH_MLS_MEMBER_CHECKING);
  gh_mls_key_package_evidence_lookup_async(self->accounts, (const gchar *const *)relays,
                                           (const gchar *const *)group->read_relays, account,
                                           self->lookup_deadline, self->cancellable,
                                           verify_done, task);
}

GhMlsMemberIdentity
gh_mls_service_verify_member_finish(GhMlsService *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), GH_MLS_MEMBER_UNVERIFIED);
  GError *local = NULL;
  gssize identity = g_task_propagate_int(G_TASK(result), &local);
  if (local) {
    g_propagate_error(error, local);
    return GH_MLS_MEMBER_UNVERIFIED;
  }
  return (GhMlsMemberIdentity)identity;
}

gboolean
gh_mls_service_leave(GhMlsService *self, GhMlsGroup *group, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not an active group of this account");
    return FALSE;
  }
  drop_stale_error(self);
  /* For everyone where the group supports it (nostrc-2um6): our SelfRemove,
   * durable in libmarmot, published now or once online; the group is
   * "leaving" until a member commits it. Leaving again gives up waiting. */
  group->leave_misses = 0;
  group->leave_admin_commit = FALSE;
  if (group->leave_failure != GH_MLS_LEAVE_FAILURE_NONE) {
    group->leave_failure = GH_MLS_LEAVE_FAILURE_NONE;
    g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_LEAVE_FAILED]);
  }
  if (!group->leaving) {
    MarmotError check = marmot_can_self_remove(self->marmot, &group->gid, NULL);
    if (check == MARMOT_ERR_OWN_COMMIT_PENDING) {
      g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_BUSY,
                          "Another change of this group is still being sent");
      return FALSE;
    }
    if (check == MARMOT_OK) {
      char *json = NULL;
      MarmotError err = marmot_self_remove(self->marmot, &group->gid, &json);
      if (err != MARMOT_OK)
        return marmot_fail(self, err, "The group could not be left", error);
      departures_cancel(group);
      upgrade_cancel(group);
      group_refresh(group);
      leave_start(group, json);
      free(json);
      return TRUE;
    }
  }
  /* On this device only: an admin, a member whose app cannot process a
   * leave, or giving up waiting. */
  leave_cancel(group);
  departures_cancel(group);
  upgrade_cancel(group);
  MarmotError err = marmot_leave_group(self->marmot, &group->gid);
  if (err != MARMOT_OK)
    return marmot_fail(self, err, "The group could not be left", error);
  group_unsubscribe(group);
  group_refresh(group);
  return TRUE;
}

GhMlsLeave
gh_mls_service_leave_kind(GhMlsService *self, GhMlsGroup *group)
{
  if (!GH_IS_MLS_SERVICE(self) || !GH_IS_MLS_GROUP(group) || group->service != self ||
      !group->active)
    return GH_MLS_LEAVE_DEVICE;
  if (group->leaving)
    return GH_MLS_LEAVE_DEVICE_WAITING;
  MarmotLeaveKind kind = MARMOT_LEAVE_SELF_REMOVE;
  switch (marmot_can_self_remove(self->marmot, &group->gid, &kind)) {
  case MARMOT_OK:
  case MARMOT_ERR_OWN_COMMIT_PENDING:   /* the same, once our change is out */
    return kind == MARMOT_LEAVE_REMOVE_REQUEST ? GH_MLS_LEAVE_ADMINS : GH_MLS_LEAVE_EVERYONE;
  case MARMOT_ERR_ADMIN_CANNOT_LEAVE:
    return GH_MLS_LEAVE_DEVICE_ADMIN;
  case MARMOT_ERR_UNSUPPORTED:
    return GH_MLS_LEAVE_DEVICE_UNSUPPORTED;
  default:
    return GH_MLS_LEAVE_DEVICE;
  }
}

/* ---- Account proof (libmarmot >= 0.10.0; review B2) --------------------------------------- */

static void key_package_maybe_publish(GhMlsService *self);

static void
identity_set_state(GhMlsService *self, GhMlsIdentityState state)
{
  if (self->identity == state)
    return;
  self->identity = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IDENTITY_STATE]);
}

/* KeyPackages and new groups need the proof (libmarmot refuses them). */
static gboolean
identity_ready(GhMlsService *self)
{
  return self->identity == GH_MLS_IDENTITY_NOT_REQUIRED ||
         self->identity == GH_MLS_IDENTITY_ENROLLED;
}

#if GH_MLS_SERVICE_ACCOUNT_PROOF
typedef struct {
  GWeakRef service;
  guint64 generation;
  gchar *template_json;
} IdentityJob;

static void
identity_job_free(IdentityJob *job)
{
  g_weak_ref_clear(&job->service);
  g_free(job->template_json);
  g_free(job);
}

/* The signer must return exactly the template, signed by the account. */
static gboolean
proof_matches(const gchar *template_json, const gchar *signed_json, const gchar *account)
{
  NostrEvent *want = nostr_event_new(), *got = nostr_event_new();
  gboolean ok = want && got &&
                nostr_event_deserialize_compact(want, template_json, NULL) == 1 &&
                nostr_event_deserialize_compact(got, signed_json, NULL) == 1 &&
                nostr_event_validate(got, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(got) == nostr_event_get_kind(want) &&
                g_strcmp0(nostr_event_get_pubkey(got), account) == 0 &&
                g_strcmp0(nostr_event_get_content(got), nostr_event_get_content(want)) == 0;
  if (want)
    nostr_event_free(want);
  if (got)
    nostr_event_free(got);
  return ok;
}

static void
identity_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  IdentityJob *job = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  g_autoptr(GhMlsService) self = g_weak_ref_get(&job->service);
  /* The request belongs to the account generation, not to a connection: a
   * network flap neither cancels it nor asks again (review N7). */
  if (!self || self->identity_generation != job->generation) {
    identity_job_free(job);
    return;
  }
  self->identity_busy = FALSE;
  if (!signed_json) {
    /* Declined (or the signer failed): not asked again until the account's
     * next generation or gh_mls_service_retry_identity(), so a refusal is
     * not a prompt per reconnect. */
    g_message("Groundhog's encrypted groups wait for the signer: %s", error->message);
    identity_set_state(self, g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                               ? GH_MLS_IDENTITY_NONE : GH_MLS_IDENTITY_DECLINED);
    identity_job_free(job);
    return;
  }
  /* libmarmot checks it again; the template is local-only and never
   * published. */
  if (!proof_matches(job->template_json, signed_json, self->account) ||
      marmot_set_account_proof(self->marmot, self->account_key, signed_json) != MARMOT_OK) {
    drop_stale_error(self);
    g_message("Groundhog's signer returned something other than the account proof");
    identity_set_state(self, GH_MLS_IDENTITY_FAILED);
    identity_job_free(job);
    return;
  }
  identity_set_state(self, GH_MLS_IDENTITY_ENROLLED);
  identity_job_free(job);
  key_package_maybe_publish(self);
}
#endif

/* Asks the signer for this start's account proof when it is missing. */
static void
identity_enroll(GhMlsService *self)
{
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  if (!running(self) || self->identity_busy || self->identity != GH_MLS_IDENTITY_NONE)
    return;
  if (marmot_has_account_proof(self->marmot, self->account_key)) {
    identity_set_state(self, GH_MLS_IDENTITY_ENROLLED);
    return;
  }
  char *template_json = NULL;
  if (marmot_account_proof_template(self->marmot, self->account_key, &template_json) !=
        MARMOT_OK || !template_json) {
    free(template_json);
    drop_stale_error(self);
    identity_set_state(self, GH_MLS_IDENTITY_FAILED);
    return;
  }
  IdentityJob *job = g_new0(IdentityJob, 1);
  g_weak_ref_init(&job->service, self);
  job->generation = self->identity_generation;
  job->template_json = g_strdup(template_json);
  free(template_json);
  self->identity_busy = TRUE;
  identity_set_state(self, GH_MLS_IDENTITY_WAITING);
  gh_account_controller_sign_with_cancellable_async(self->accounts, job->template_json,
                                                    self->identity_cancellable,
                                                    identity_signed, job);
#else
  (void)self;
#endif
}

GhMlsIdentityState
gh_mls_service_get_identity_state(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), GH_MLS_IDENTITY_NONE);
  return self->identity;
}

void
gh_mls_service_set_backfill_quiet(GhMlsService *self, guint quiet_ms)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  self->backfill_quiet_us = quiet_ms ? (gint64)quiet_ms * 1000
                                     : (gint64)GH_MLS_SERVICE_BACKFILL_QUIET_S * G_USEC_PER_SEC;
}

void
gh_mls_service_set_pending_shown(GhMlsService *self, guint shown_ms)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  self->pending_shown_us = shown_ms ? (gint64)shown_ms * 1000
                                    : (gint64)GH_MLS_SERVICE_PENDING_SHOWN_S * G_USEC_PER_SEC;
  for (guint i = 0; i < self->groups->len; i++)
    sync_decrypt_pending(g_ptr_array_index(self->groups, i));
}

void
gh_mls_service_set_backfill_limit(GhMlsService *self, guint max_events, gsize max_bytes)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  self->max_backfill_events = max_events ? max_events : GH_MLS_SERVICE_MAX_BACKFILL_EVENTS;
  self->max_backfill_bytes = max_bytes ? max_bytes : GH_MLS_SERVICE_MAX_BACKFILL_BYTES;
}

gboolean
gh_mls_service_retry_identity(GhMlsService *self, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  if (!check_running(self, error))
    return FALSE;
  if (self->identity == GH_MLS_IDENTITY_DECLINED || self->identity == GH_MLS_IDENTITY_FAILED)
    identity_set_state(self, GH_MLS_IDENTITY_NONE);
  identity_enroll(self);
  return TRUE;
}

/* The account generation changed (a switch, or the account activated
 * again): a pending request dies with the old one, and a decline is asked
 * again once, for the new one. */
static void
identity_new_generation(GhMlsService *self, guint64 generation)
{
  if (self->identity_generation == generation)
    return;
  if (self->identity_cancellable) {
    g_cancellable_cancel(self->identity_cancellable);
    g_clear_object(&self->identity_cancellable);
  }
  self->identity_generation = generation;
  self->identity_busy = FALSE;
  if (generation)
    self->identity_cancellable = g_cancellable_new();
  if (self->identity == GH_MLS_IDENTITY_WAITING || self->identity == GH_MLS_IDENTITY_DECLINED ||
      self->identity == GH_MLS_IDENTITY_FAILED)
    identity_set_state(self, GH_MLS_IDENTITY_NONE);
}

/* ---- KeyPackage (MIP-00) --------------------------------------------------------------- */

/* The formats the account publishes (nostrc-lf62): the adopted one (MDK 0.11,
 * current White Noise) when the producer is built in, and the MDK 0.8 one
 * (older White Noise, Amethyst's MIP-era Quartz), so that legacy peers can
 * still invite the account. Both are kind 30443 on the same write relays,
 * each in its own `d` slot with its own ACK-tied lifecycle; an inviter picks
 * the one its group's profile needs. The price: two KeyPackage events, and
 * a relay or observer can link the two slots to one account (they share
 * the author, relays and timing) and learn that it runs a client speaking
 * both formats. */
/* Whether the user lets people on older Marmot apps invite them (the
 * "mls-legacy-key-packages" setting; on unless switched off). */
static gboolean
legacy_wanted(GhMlsService *self)
{
  if (!self->settings)
    return TRUE;
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(self->settings, "settings-schema", &schema, NULL);
  return !schema || !g_settings_schema_has_key(schema, LEGACY_KEY_PACKAGES_KEY) ||
         g_settings_get_boolean(self->settings, LEGACY_KEY_PACKAGES_KEY);
}

static gboolean
key_package_produced(GhMlsService *self, GhMlsKeyPackageFormat format)
{
  gboolean adopted = GH_MLS_ADOPTED_KEY_PACKAGES && !self->legacy_only;
  if (format == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED)
    return adopted;
  /* Without the adopted one, the MDK 0.8 one is the only way in: kept. */
  return !adopted || legacy_wanted(self);
}

static const gchar *
key_package_cursor_key(GhMlsKeyPackageFormat format)
{
  return format == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED ? "adopted" : "";
}

static MarmotKeyPackageProfile
key_package_profile(GhMlsKeyPackageFormat format)
{
  return format == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED ? MARMOT_KEY_PACKAGE_PROFILE_ADOPTED
                                                     : MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8;
}

/* The account's state is its formats' together: PUBLISHED once every one
 * is, PUBLISHING or FAILED as soon as one is. */
static void
key_package_update_state(GhMlsService *self)
{
  GhMlsKeyPackageState state = GH_MLS_KEY_PACKAGE_PUBLISHED;
  gboolean publishing = FALSE, failed = FALSE, no_relays = FALSE, none = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    if (!key_package_produced(self, f))
      continue;
    switch (self->kp[f].state) {
    case GH_MLS_KEY_PACKAGE_PUBLISHING: publishing = TRUE; break;
    case GH_MLS_KEY_PACKAGE_FAILED: failed = TRUE; break;
    case GH_MLS_KEY_PACKAGE_NO_RELAYS: no_relays = TRUE; break;
    case GH_MLS_KEY_PACKAGE_PUBLISHED: break;
    default: none = TRUE; break;
    }
  }
  if (publishing)
    state = GH_MLS_KEY_PACKAGE_PUBLISHING;
  else if (failed)
    state = GH_MLS_KEY_PACKAGE_FAILED;
  else if (no_relays)
    state = GH_MLS_KEY_PACKAGE_NO_RELAYS;
  else if (none)
    state = GH_MLS_KEY_PACKAGE_NONE;
  if (self->key_package == state)
    return;
  self->key_package = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_KEY_PACKAGE_STATE]);
}

static void
key_package_slot_set_state(KeyPackageSlot *slot, GhMlsKeyPackageState state)
{
  slot->state = state;
  key_package_update_state(slot->service);
}

/* Every format's state (the service stops, or has no relays). */
static void
key_package_set_state(GhMlsService *self, GhMlsKeyPackageState state)
{
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    self->kp[f].state = state;
  key_package_update_state(self);
}

static gboolean
key_package_any_busy(GhMlsService *self)
{
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    if (self->kp[f].busy)
      return TRUE;
  return FALSE;
}

/* Where the account's KeyPackages go (§4.3 own list publish; Marmot
 * transports/nostr.md "KeyPackage publication", nostrc-0bdg): its kind-10002
 * write-capable set only -- `r` entries marked "write" or unmarked, never
 * read-only ones, never the kind-10050 inbox relays -- the one set inviters
 * look them up on. Sorted, unique, at most 16. Both formats alike. */
static GStrv
key_package_relays(GhMlsService *self)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  if (self->account_relays &&
      gh_account_relays_get_generation(self->account_relays) == self->generation) {
    const gchar *const *write = gh_account_relays_get_write_relays(self->account_relays);
    for (guint i = 0; write && write[i] && urls->len < MAX_GROUP_RELAYS; i++)
      if (gh_relay_url_validate(write[i], NULL) &&
          !g_ptr_array_find_with_equal_func(urls, write[i], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(write[i]));
  }
  g_ptr_array_sort(urls, compare_strings);
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_steal(urls, NULL);
}

/* The KeyPackage producers. The adopted profile (kind 30443 of the adopted
 * Marmot spec: MLSMessage framing, app_components, no encoding or relays
 * tag) is built in with libmarmot's producer (CMake option
 * MARMOT_ADOPTED_KEY_PACKAGE_PRODUCER: GH_MLS_ADOPTED_KEY_PACKAGES, on by
 * default since nostrc-lf62); the MDK
 * 0.8 one always. Both use the enrolled account proof (the signer is
 * asynchronous). */
#if GH_MLS_ADOPTED_KEY_PACKAGES && defined(GH_MLS_TEST_HOOKS)
static GhMlsTestAdoptedProducer test_adopted_producer;

void
gh_mls_service_test_set_adopted_producer(GhMlsTestAdoptedProducer producer)
{
  test_adopted_producer = producer;
}
#endif

static MarmotError
key_package_make(GhMlsService *self, GhMlsKeyPackageFormat format, const gchar *const *urls,
                 MarmotKeyPackageResult *made)
{
#if GH_MLS_ADOPTED_KEY_PACKAGES
  if (format == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED) {
    /* The adopted profile repeats no relays. */
#ifdef GH_MLS_TEST_HOOKS
    if (test_adopted_producer)
      return test_adopted_producer(self->marmot, self->account_key, made);
#endif
    return marmot_create_key_package_for_profile(self->marmot, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 self->account_key, NULL, NULL, NULL, NULL, 0,
                                                 made);
  }
#else
  (void)format;
#endif
  return marmot_create_key_package_unsigned(self->marmot, self->account_key,
                                            (const char **)urls,
                                            g_strv_length((gchar **)urls), made);
}

#ifdef GH_MLS_TEST_HOOKS
gboolean
gh_mls_service_test_has_init_key(GhMlsService *self, const gchar *ref_hex)
{
  guint8 ref[32];
  bool present = false;
  g_assert_true(lower_hex64(ref_hex) && nostr_hex2bin(ref, ref_hex, sizeof ref));
  g_assert_cmpint(marmot_key_package_has_private_key(self->marmot, ref, &present), ==,
                  MARMOT_OK);
  return present;
}
#endif

static void key_package_sweep(GhMlsService *self);
static void key_package_withdrawn(KeyPackageSlot *slot);

static gboolean
key_package_sweep_fired(gpointer data)
{
  GhMlsService *self = data;
  self->key_package_sweep_timer = 0;
  key_package_sweep(self);
  key_package_maybe_publish(self);
  return G_SOURCE_REMOVE;
}

/* The sweep runs again at the earliest not_after left, whether or not a
 * publish check comes (review L3: e.g. an account without write relays). */
static void
key_package_sweep_schedule(GhMlsService *self)
{
  if (self->key_package_sweep_timer) {
    gh_clock_source_remove(self->clock, self->key_package_sweep_timer);
    self->key_package_sweep_timer = 0;
  }
  if (self->disposed || self->generation == 0)
    return;
  int64_t not_after = 0;
  drop_stale_error(self);
  if (marmot_key_package_next_expiry(self->marmot, self->account_key, &not_after) !=
        MARMOT_OK) {
    drop_stale_error(self);
    return;
  }
  if (not_after <= 0)
    return;
  gint64 wait_s = MAX((gint64)not_after - now_s(self), 0) + 1;
  self->key_package_sweep_timer =
    gh_clock_timeout_add(self->clock, (guint64)wait_s * 1000, key_package_sweep_fired, self,
                         NULL);
}

/* Lifetime bound (foundation/key-packages.md): an expired KeyPackage's
 * private material goes, whatever its state. Runs at every start, every
 * publish check and at the earliest not_after (key_package_sweep_schedule). */
static void
key_package_sweep(GhMlsService *self)
{
  if (self->disposed || self->generation == 0)
    return;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not check its KeyPackages' lifetime: %s", error->message);
    return;
  }
  size_t deleted = 0;
  MarmotError err = marmot_key_package_sweep_expired(self->marmot, self->account_key, 0,
                                                     &deleted);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Retiring expired KeyPackages", &error);
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its expired KeyPackages: %s", error->message);
    return;
  }
  if (!gh_store_commit(self->store, &error)) {
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its expired KeyPackages: %s", error->message);
  } else if (deleted > 0) {
    g_debug("Groundhog retired %" G_GSIZE_FORMAT " expired KeyPackage(s)", deleted);
  }
  key_package_sweep_schedule(self);
}

static gboolean
key_package_hold_fired(gpointer data)
{
  GhMlsService *self = data;
  self->key_package_hold_timer = 0;
  key_package_maybe_publish(self);
  return G_SOURCE_REMOVE;
}

static void
key_package_set_held(GhMlsService *self, gboolean held)
{
  if (self->key_package_held == held)
    return;
  self->key_package_held = held;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_KEY_PACKAGE_HELD]);
}

static void
key_package_hold_stop(GhMlsService *self, gboolean forget)
{
  if (self->key_package_hold_timer) {
    gh_clock_source_remove(self->clock, self->key_package_hold_timer);
    self->key_package_hold_timer = 0;
  }
  key_package_set_held(self, FALSE);
  gint64 since = 0;
  if (forget && self->store &&
      gh_store_get_cursor(self->store, KEY_PACKAGE_HELD_CURSOR, "", &since, NULL) && since > 0)
    gh_store_set_cursor(self->store, KEY_PACKAGE_HELD_CURSOR, "", 0, NULL);
}

/* A due replacement (lifetime, the user's rotate, or a join) of a KeyPackage
 * that pending invitations were most likely made with (nostrc-0bdg, review
 * H1/M1). Its first relay OK makes libmarmot delete the old private key of
 * its format (foundation/key-packages.md: no later than the confirmed
 * replacement; "Local retention policy MUST NOT extend" that), and a
 * pending invitation could no longer be accepted. So the replacement is not
 * *published* while an invitation is pending -- the old KeyPackage stays
 * published and usable (last-resort) -- for at most
 * GhMlsServiceConfig.key_package_max_hold (GH_MLS_KEY_PACKAGE_MAX_HOLD) from
 * when the first was held (one start for every format), and never later
 * than GH_MLS_KEY_PACKAGE_EXPIRY_MARGIN before its format's current
 * KeyPackages' Lifetime ends. A format with no KeyPackage left (its first,
 * e.g. the adopted one after an upgrade) has nothing to protect and is not
 * held. Recorded in the store (a restart keeps the start); looked at again
 * on every accept, decline and failed accept, at every start and on a timer
 * (at most GH_MLS_KEY_PACKAGE_HOLD_RECHECK_S). An invitation list that
 * cannot be read counts as pending. */
typedef enum { HOLD_NO, HOLD_YES, HOLD_ENDED } HoldVerdict;

static HoldVerdict
key_package_hold_verdict(GhMlsService *self, GhMlsKeyPackageFormat format, gboolean pending,
                         gint64 *deadline)
{
  if (!pending)
    return HOLD_NO;
  int64_t expires = 0;
  drop_stale_error(self);
  if (marmot_key_package_next_expiry_for_profile(self->marmot, self->account_key,
                                                 key_package_profile(format), &expires) !=
      MARMOT_OK) {
    drop_stale_error(self);
    expires = -1;   /* unknown: hold, bounded by the cap alone */
  }
  if (expires == 0)   /* nothing of this format published to protect */
    return HOLD_NO;
  gint64 now = now_s(self), since = 0;
  if (!gh_store_get_cursor(self->store, KEY_PACKAGE_HELD_CURSOR, "", &since, NULL) ||
      since <= 0 || since > now) {
    since = now;
    gh_store_set_cursor(self->store, KEY_PACKAGE_HELD_CURSOR, "", since, NULL);
  }
  *deadline = since + self->key_package_max_hold;
  if (expires > 0)
    *deadline = MIN(*deadline, (gint64)expires - GH_MLS_KEY_PACKAGE_EXPIRY_MARGIN);
  return now >= *deadline ? HOLD_ENDED : HOLD_YES;
}

/* Whether any format's replacement is held; the timer looks again by the
 * earliest deadline. */
static void
key_package_hold_apply(GhMlsService *self, gboolean ended, gint64 wake)
{
  gboolean held = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    held |= self->kp[f].held;
  if (ended)
    g_message("Groundhog publishes a KeyPackage replacement although an invitation is still "
              "pending: it waited as long as it may");
  if (!held) {
    /* An ended hold keeps its start until a replacement is accepted. */
    key_package_hold_stop(self, !ended);
    return;
  }
  key_package_set_held(self, TRUE);
  if (!self->key_package_hold_timer) {
    gint64 wait_s = MIN(wake - now_s(self), (gint64)GH_MLS_KEY_PACKAGE_HOLD_RECHECK_S);
    self->key_package_hold_timer =
      gh_clock_timeout_add(self->clock, (guint64)MAX(wait_s, 1) * 1000, key_package_hold_fired,
                           self, NULL);
  }
}

/* The relay's OK for the KeyPackage in flight: the replacement is
 * confirmed, and libmarmot deletes the private material of every older
 * KeyPackage of its format's slot -- never before (ACK-tied replacement),
 * never the other format's. */
static void
key_package_confirm(KeyPackageSlot *slot)
{
  GhMlsService *self = slot->service;
  if (!slot->in_flight)
    return;
  slot->in_flight = FALSE;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not retire its old KeyPackage: %s", error->message);
    return;
  }
  MarmotError err = marmot_key_package_confirm_published(self->marmot, self->account_key,
                                                         slot->ref);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Retiring the old KeyPackage", &error);
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its old KeyPackage: %s", error->message);
    return;
  }
  if (!gh_store_commit(self->store, &error)) {
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its old KeyPackage: %s", error->message);
  }
}

/* The `i` tag (KeyPackageRef) of a kind-30443 event, when it is exactly
 * @ref. */
static gboolean
key_package_event_has_ref(NostrEvent *event, const guint8 ref[32])
{
  g_autofree gchar *want = to_hex(ref, 32);
  NostrTags *tags = nostr_event_get_tags(event);
  guint found = 0;
  gboolean same = FALSE;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || g_strcmp0(nostr_tag_get(tag, 0), "i") != 0)
      continue;
    found++;
    same = nostr_tag_size(tag) == 2 && g_strcmp0(nostr_tag_get(tag, 1), want) == 0;
  }
  return found == 1 && same;
}

typedef struct {
  GWeakRef service;
  guint64 run;
  GhMlsKeyPackageFormat format;
  GStrv urls;
  guint8 ref[32];      /* the KeyPackageRef being published */
} KeyPackageJob;

static void
key_package_job_free(KeyPackageJob *job)
{
  g_weak_ref_clear(&job->service);
  g_strfreev(job->urls);
  g_free(job);
}

static void
key_package_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  KeyPackageSlot *slot = data;
  GhMlsService *self = slot->service;
  if (result->outcome != GH_RELAY_PUBLISH_ACCEPTED)
    return;
  /* A withdrawal: its first OK retires the keys, the others say nothing. */
  if (publish == slot->withdrawal) {
    if (slot->withdrawing)
      key_package_withdrawn(slot);
    return;
  }
  if (slot->state == GH_MLS_KEY_PACKAGE_PUBLISHED)
    return;
  /* The first relay OK (transports/nostr.md "Publish targets and
   * acknowledgements"): the replacement is confirmed. */
  key_package_confirm(slot);
  slot->held = FALSE;
  gboolean other_held = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    other_held |= self->kp[f].held;
  if (!other_held)
    key_package_hold_stop(self, TRUE);
  key_package_sweep_schedule(self);
  g_autoptr(GError) error = NULL;
  /* A rotation asked for while this one was in flight (a Welcome joined
   * meanwhile) still stands: the publication time is not recorded, so the
   * next check publishes again (key_package_done() runs it). */
  if (!slot->rotate &&
      !gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR, key_package_cursor_key(slot->format),
                           now_s(self), &error))
    g_message("Groundhog could not record the KeyPackage publication: %s", error->message);
  g_free(slot->id);
  slot->id = g_strdup(gh_relay_publish_get_event_id(publish));
  if (slot->id) {
    if (slot->event_ids->len >= MAX_KEY_PACKAGE_EVENT_IDS)
      g_ptr_array_remove_index(slot->event_ids, 0);
    g_ptr_array_add(slot->event_ids, g_strdup(slot->id));
  }
  key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_PUBLISHED);
}

static gboolean
key_package_publish_free_idle(gpointer data)
{
  gh_relay_publish_unref(data);
  return G_SOURCE_REMOVE;
}

static void
key_package_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  KeyPackageSlot *slot = data;
  GhMlsService *self = slot->service;
  if (slot->publish == publish)
    slot->publish = NULL;
  if (slot->withdrawal == publish)
    slot->withdrawal = NULL;
  slot->busy = FALSE;
  /* No relay accepted it: the older KeyPackages keep their keys (it may
   * still have reached a relay; the next confirmed one retires it). A
   * withdrawal no relay accepted keeps them too, and is asked again. */
  slot->in_flight = FALSE;
  gboolean withdrawal = slot->withdrawing;
  slot->withdrawing = FALSE;
  if (!summary->any_accepted) {
    if (!withdrawal)
      key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_FAILED);
    schedule_retry(self);
  }
  g_idle_add(key_package_publish_free_idle, publish);
  /* A rotation asked for, or the MDK 0.8 format switched on or off, while
   * this round was in flight. */
  gboolean rotate = self->key_package_recheck;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    rotate |= self->kp[f].rotate;
  if (summary->any_accepted && rotate && !key_package_any_busy(self))
    key_package_maybe_publish(self);
}

static void
key_package_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  KeyPackageJob *job = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  g_autoptr(GhMlsService) self = g_weak_ref_get(&job->service);
  if (!self || self->run != job->run || !running(self)) {
    key_package_job_free(job);
    return;
  }
  KeyPackageSlot *slot = &self->kp[job->format];
  /* The signer must have signed exactly a KeyPackage of this account, the
   * one just made (its KeyPackageRef is what a relay OK confirms). */
  NostrEvent *event = signed_json ? nostr_event_new() : NULL;
  gboolean ok = event && nostr_event_deserialize_compact(event, signed_json, NULL) == 1 &&
                nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == MARMOT_KIND_KEY_PACKAGE &&
                g_strcmp0(nostr_event_get_pubkey(event), self->account) == 0 &&
                key_package_event_has_ref(event, job->ref);
  if (event)
    nostr_event_free(event);
  GhRelayPublish *publish = ok ? gh_relay_publish_new(self->generation, signed_json,
                                                      key_package_update, key_package_done,
                                                      slot, &error)
                               : NULL;
  guint added = 0;
  for (guint i = 0; publish && job->urls[i]; i++) {
    if (!gh_relay_publish_add_url(publish, job->urls[i], NULL))
      continue;
    /* §4.3 own list publish: the account signs in only on challenge. */
    gh_auth_policy_apply_publish(self->policy, publish, GH_AUTH_PURPOSE_OWN_LIST_PUBLISH,
                                 job->urls[i], NULL);
    added++;
  }
  if (publish && self->publish_deadline)
    gh_relay_publish_set_deadline(publish, self->publish_deadline);
  if (!publish || !added) {
    g_message("Groundhog could not publish its KeyPackage: %s",
              error ? error->message : "the signer returned something else");
    if (publish)
      gh_relay_publish_unref(publish);
    slot->busy = FALSE;
    key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_FAILED);
    key_package_job_free(job);
    return;
  }
  slot->publish = publish;
  memcpy(slot->ref, job->ref, sizeof slot->ref);
  slot->in_flight = TRUE;
  if (!gh_relay_publish_start(publish, &error)) {
    slot->publish = NULL;
    slot->in_flight = FALSE;
    gh_relay_publish_unref(publish);
    slot->busy = FALSE;
    key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_FAILED);
  }
  key_package_job_free(job);
}

/* Makes the format's KeyPackage (its private init key stored, encrypted,
 * before anything is signed) and has it signed and published. */
static void
key_package_publish(GhMlsService *self, GhMlsKeyPackageFormat format, const gchar *const *urls)
{
  KeyPackageSlot *slot = &self->kp[format];
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error))
    return;
  MarmotError err = key_package_make(self, format, urls, &made);
  if (err != MARMOT_OK || !gh_store_commit(self->store, &error)) {
    if (err != MARMOT_OK) {
      marmot_fail(self, err, "Making a KeyPackage", &error);
      gh_store_rollback(self->store);
    }
    g_message("Groundhog could not make a KeyPackage: %s", error->message);
    marmot_key_package_result_free(&made);
    key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_FAILED);
    return;
  }
  slot->busy = TRUE;
  /* This KeyPackage serves any rotation asked for so far; until a relay
   * accepts it the publication time stays unrecorded (0 after a rotation),
   * so a failure or a restart publishes again. */
  slot->rotate = FALSE;
  key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_PUBLISHING);
  KeyPackageJob *job = g_new0(KeyPackageJob, 1);
  g_weak_ref_init(&job->service, self);
  job->run = self->run;
  job->format = format;
  job->urls = g_strdupv((gchar **)urls);
  memcpy(job->ref, made.key_package_ref, sizeof job->ref);
  gh_account_controller_sign_with_cancellable_async(self->accounts, made.event_json,
                                                    self->cancellable, key_package_signed,
                                                    job);
  marmot_key_package_result_free(&made);
}

/* The withdrawal's first relay OK (review L1): the relays were asked to
 * delete the MDK 0.8 KeyPackage, so its keys go now -- never before, as for
 * a replacement: until then a Welcome to it still opens. Its cursor goes to
 * 0, so switching the format back on publishes a new one at once. */
static void
key_package_withdrawn(KeyPackageSlot *slot)
{
  GhMlsService *self = slot->service;
  slot->withdrawing = FALSE;
  g_autoptr(GError) error = NULL;
  size_t retired = 0;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not retire its older-format KeyPackage: %s", error->message);
    return;
  }
  MarmotError err = marmot_key_package_retire_profile(self->marmot, self->account_key,
                                                      MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                      &retired);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Retiring the older-format KeyPackage", &error);
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its older-format KeyPackage: %s", error->message);
    return;
  }
  if (!gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR,
                           key_package_cursor_key(GH_MLS_KEY_PACKAGE_FORMAT_LEGACY), 0, &error) ||
      !gh_store_commit(self->store, &error)) {
    gh_store_rollback(self->store);
    g_message("Groundhog could not retire its older-format KeyPackage: %s", error->message);
    return;
  }
  g_debug("Groundhog withdrew its older-format KeyPackage (%" G_GSIZE_FORMAT " key(s))",
          retired);
  g_clear_pointer(&slot->id, g_free);
  g_ptr_array_set_size(slot->event_ids, 0);
  slot->rotate = FALSE;
  slot->held = FALSE;
  gboolean other_held = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    other_held |= self->kp[f].held;
  if (!other_held)
    key_package_hold_stop(self, TRUE);
  key_package_slot_set_state(slot, GH_MLS_KEY_PACKAGE_NONE);
  key_package_sweep_schedule(self);
}

/* The deletion request (NIP-09: `a` = the slot's address, `k` = 30443)
 * signed: checked, then published where the KeyPackages go. */
static void
key_package_withdraw_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  KeyPackageJob *job = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  g_autoptr(GhMlsService) self = g_weak_ref_get(&job->service);
  if (!self || self->run != job->run || !running(self)) {
    key_package_job_free(job);
    return;
  }
  KeyPackageSlot *slot = &self->kp[GH_MLS_KEY_PACKAGE_FORMAT_LEGACY];
  NostrEvent *event = signed_json ? nostr_event_new() : NULL;
  gboolean ok = event && nostr_event_deserialize_compact(event, signed_json, NULL) == 1 &&
                nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == 5 &&
                g_strcmp0(nostr_event_get_pubkey(event), self->account) == 0;
  if (event)
    nostr_event_free(event);
  GhRelayPublish *publish = ok ? gh_relay_publish_new(self->generation, signed_json,
                                                      key_package_update, key_package_done,
                                                      slot, &error)
                               : NULL;
  guint added = 0;
  for (guint i = 0; publish && job->urls[i]; i++) {
    if (!gh_relay_publish_add_url(publish, job->urls[i], NULL))
      continue;
    gh_auth_policy_apply_publish(self->policy, publish, GH_AUTH_PURPOSE_OWN_LIST_PUBLISH,
                                 job->urls[i], NULL);
    added++;
  }
  if (publish && self->publish_deadline)
    gh_relay_publish_set_deadline(publish, self->publish_deadline);
  /* Recorded first: a publish's callbacks may run before it starts. */
  slot->publish = publish;
  slot->withdrawal = publish;
  if (!publish || !added || !gh_relay_publish_start(publish, &error)) {
    g_message("Groundhog could not withdraw its older-format KeyPackage: %s",
              error ? error->message : "the signer returned something else");
    slot->publish = NULL;
    slot->withdrawal = NULL;
    if (publish)
      gh_relay_publish_unref(publish);
    slot->busy = FALSE;
    slot->withdrawing = FALSE;
    key_package_job_free(job);
    return;
  }
  key_package_job_free(job);
}

/* Asks the account's write relays to delete its MDK 0.8 KeyPackage (the
 * user switched that format off); its keys are retired on the first OK. */
#ifdef GH_MLS_TEST_HOOKS
static gboolean test_fail_key_package_slot;
static guint test_key_package_slot_failures;

void
gh_mls_service_test_fail_key_package_slot(gboolean fail)
{
  test_fail_key_package_slot = fail;
  test_key_package_slot_failures = 0;
}

guint
gh_mls_service_test_key_package_slot_failures(void)
{
  return test_key_package_slot_failures;
}
#endif

static void
key_package_withdraw(GhMlsService *self, const gchar *const *urls)
{
  KeyPackageSlot *slot = &self->kp[GH_MLS_KEY_PACKAGE_FORMAT_LEGACY];
  char d[65];
  drop_stale_error(self);
  MarmotError err = marmot_key_package_slot(self->marmot, self->account_key,
                                            MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8, d);
#ifdef GH_MLS_TEST_HOOKS
  if (test_fail_key_package_slot && err == MARMOT_OK) {
    err = MARMOT_ERR_STORAGE;
    test_key_package_slot_failures++;
  }
#endif
  if (err == MARMOT_ERR_STORAGE_NOT_FOUND) {
    /* Definitely never published: nothing to ask the relays; retire. */
    key_package_withdrawn(slot);
    return;
  }
  if (err != MARMOT_OK) {
    /* Unknown (review L3): the keys stay until the request can be made. */
    g_autoptr(GError) error = NULL;
    marmot_fail(self, err, "Reading the older-format KeyPackage's slot", &error);
    g_message("Groundhog could not withdraw its older-format KeyPackage: %s", error->message);
    schedule_retry(self);
    return;
  }
  /* Dated after every KeyPackage libmarmot made (it may date them ahead of
   * the clock): a relay deletes an address's versions only up to the
   * request's created_at (review M4). Recorded, so a later KeyPackage of
   * the slot is newer than the request. */
  int64_t created_at = 0;
  g_autoptr(GError) error = NULL;
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not withdraw its older-format KeyPackage: %s", error->message);
    schedule_retry(self);
    return;
  }
  err = marmot_key_package_reserve_created_at(self->marmot, self->account_key, now_s(self),
                                              &created_at);
  if (err != MARMOT_OK || !gh_store_commit(self->store, &error)) {
    if (err != MARMOT_OK)
      marmot_fail(self, err, "Dating the older-format KeyPackage's withdrawal", &error);
    gh_store_rollback(self->store);
    g_message("Groundhog could not withdraw its older-format KeyPackage: %s", error->message);
    schedule_retry(self);
    return;
  }
  g_autofree gchar *address = g_strdup_printf("%d:%s:%s", MARMOT_KIND_KEY_PACKAGE, self->account,
                                              d);
  g_autofree gchar *kind = g_strdup_printf("%d", MARMOT_KIND_KEY_PACKAGE);
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  /* Every event of the slot known to be published, by id too: an `e`
   * deletion is not bounded by created_at. */
  for (guint i = 0; i < slot->event_ids->len; i++)
    nostr_tags_append(tags, nostr_tag_new("e", g_ptr_array_index(slot->event_ids, i), NULL));
  nostr_tags_append(tags, nostr_tag_new("a", address, NULL));
  nostr_tags_append(tags, nostr_tag_new("k", kind, NULL));
  nostr_event_set_kind(event, 5);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_pubkey(event, self->account);
  nostr_event_set_tags(event, tags);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (!raw) {
    g_message("Groundhog could not withdraw its older-format KeyPackage");
    return;
  }
  slot->busy = TRUE;
  slot->withdrawing = TRUE;
  KeyPackageJob *job = g_new0(KeyPackageJob, 1);
  g_weak_ref_init(&job->service, self);
  job->run = self->run;
  job->format = GH_MLS_KEY_PACKAGE_FORMAT_LEGACY;
  job->urls = g_strdupv((gchar **)urls);
  gh_account_controller_sign_with_cancellable_async(self->accounts, raw, self->cancellable,
                                                    key_package_withdraw_signed, job);
  free(raw);
}

/* Publishes a format's new KeyPackage when none was published within the
 * lifetime, or the last one was spent. One round at a time: while either
 * format is being made or published, the next check waits for it. The
 * adopted one is made first and the MDK 0.8 one right after, which is then
 * the newer (libmarmot's created_at strictly increases per account): an
 * adopted replacement brings an MDK 0.8 one along, so a legacy reader that
 * takes the account's newest kind 30443 whatever its slot (the MDK 0.8
 * harness driver does) still finds one it can parse, while adopted readers
 * look per slot (MDK 0.11 marmot-app, libmarmot). */
static void
key_package_maybe_publish(GhMlsService *self)
{
  if (!running(self) || key_package_any_busy(self) || !identity_ready(self))
    return;
  self->key_package_recheck = FALSE;
  key_package_sweep(self);
  g_auto(GStrv) urls = key_package_relays(self);
  if (!urls[0]) {
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_NO_RELAYS);
    return;
  }
  gboolean due[GH_MLS_KEY_PACKAGE_N_FORMATS] = { FALSE };
  gboolean any_due = FALSE;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    if (!key_package_produced(self, f))
      continue;
    gint64 last = 0;
    g_autoptr(GError) error = NULL;
    if (!gh_store_get_cursor(self->store, KEY_PACKAGE_CURSOR, key_package_cursor_key(f), &last,
                             &error)) {
      g_message("Groundhog could not read the KeyPackage state: %s", error->message);
      return;
    }
    due[f] = self->kp[f].rotate || last <= 0 || now_s(self) - last >= self->key_package_lifetime;
    any_due |= due[f];
  }
  /* The MDK 0.8 format switched off (L1) while its keys are still kept: its
   * KeyPackage is withdrawn (a deletion request), then they are retired. */
  gboolean withdraw = FALSE;
  if (!key_package_produced(self, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY)) {
    int64_t legacy_expires = 0;
    drop_stale_error(self);
    if (marmot_key_package_next_expiry_for_profile(self->marmot, self->account_key,
                                                   MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                   &legacy_expires) != MARMOT_OK)
      drop_stale_error(self);
    withdraw = legacy_expires != 0;
    any_due |= withdraw;
  }
  if (!any_due) {
    for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
      self->kp[f].held = FALSE;
    key_package_hold_stop(self, TRUE);
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_PUBLISHED);
    return;
  }
  /* Due. Pending invitations hold a replacement back (review H1); the
   * current KeyPackage stays published meanwhile. */
  gboolean pending = invitations_pending(self) != INVITES_NONE;
  gboolean go[GH_MLS_KEY_PACKAGE_N_FORMATS] = { FALSE };
  gboolean ended = FALSE;
  gint64 wake = G_MAXINT64;
  static const GhMlsKeyPackageFormat order[] = { GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED,
                                                 GH_MLS_KEY_PACKAGE_FORMAT_LEGACY };
  for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
    GhMlsKeyPackageFormat f = order[i];
    KeyPackageSlot *slot = &self->kp[f];
    gboolean legacy = f == GH_MLS_KEY_PACKAGE_FORMAT_LEGACY;
    gboolean want = due[f] || (legacy && withdraw);
    slot->held = FALSE;
    if (!want || (!key_package_produced(self, f) && !(legacy && withdraw)))
      continue;
    gint64 deadline = 0;
    switch (key_package_hold_verdict(self, f, pending, &deadline)) {
    case HOLD_YES:
      slot->held = TRUE;
      wake = MIN(wake, deadline);
      break;
    case HOLD_ENDED:
      ended = TRUE;
      go[f] = TRUE;
      break;
    case HOLD_NO:
      go[f] = TRUE;
      break;
    }
    /* An adopted replacement brings an MDK 0.8 one along (it stays the
     * newest kind 30443), and stays owed until it goes out (review M1): held
     * or failed, it is still due at the hold's end, the retry or a restart. */
    if (f == GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED && go[f] &&
        key_package_produced(self, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY) &&
        !due[GH_MLS_KEY_PACKAGE_FORMAT_LEGACY]) {
      due[GH_MLS_KEY_PACKAGE_FORMAT_LEGACY] = TRUE;
      self->kp[GH_MLS_KEY_PACKAGE_FORMAT_LEGACY].rotate = TRUE;
      g_autoptr(GError) error = NULL;
      if (!gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR,
                               key_package_cursor_key(GH_MLS_KEY_PACKAGE_FORMAT_LEGACY), 0, &error))
        g_message("Groundhog could not record a due KeyPackage: %s", error->message);
    }
  }
  key_package_hold_apply(self, ended, wake);
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++)
    if (key_package_produced(self, f) && !go[f])
      key_package_slot_set_state(&self->kp[f], GH_MLS_KEY_PACKAGE_PUBLISHED);
  for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
    if (!go[order[i]])
      continue;
    if (order[i] == GH_MLS_KEY_PACKAGE_FORMAT_LEGACY && withdraw)
      key_package_withdraw(self, (const gchar *const *)urls);
    else
      key_package_publish(self, order[i], (const gchar *const *)urls);
  }
  key_package_sweep_schedule(self);
}

/* MIP-00: a Welcome consumed a published KeyPackage of `format` (or the
 * user asked, for every format: -1): publish a new one, now or at the next
 * start. Only a successful join rotates (foundation/key-packages.md
 * "Failure behavior"). */
static void
key_package_rotate(GhMlsService *self, gint format)
{
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    if (!key_package_produced(self, f) || (format >= 0 && (guint)format != f))
      continue;
    self->kp[f].rotate = TRUE;
    g_autoptr(GError) error = NULL;
    if (!gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR, key_package_cursor_key(f), 0,
                             &error))
      g_message("Groundhog could not record a spent KeyPackage: %s", error->message);
  }
  key_package_maybe_publish(self);
}

GhMlsKeyPackageState
gh_mls_service_get_key_package_state(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), GH_MLS_KEY_PACKAGE_NONE);
  return self->key_package;
}

const gchar *
gh_mls_service_get_key_package_id(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  return self->kp[key_package_produced(self, GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED)
                    ? GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED
                    : GH_MLS_KEY_PACKAGE_FORMAT_LEGACY].id;
}

const gchar *
gh_mls_service_get_key_package_id_for_format(GhMlsService *self, GhMlsKeyPackageFormat format)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  g_return_val_if_fail(format < GH_MLS_KEY_PACKAGE_N_FORMATS, NULL);
  return self->kp[format].id;
}

gboolean
gh_mls_service_get_key_package_held(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  return self->key_package_held;
}

gboolean
gh_mls_service_rotate_key_package(GhMlsService *self, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  if (!check_running(self, error))
    return FALSE;
  key_package_rotate(self, -1);
  return TRUE;
}

/* ---- Resume --------------------------------------------------------------------------- */

/* The group a stored kind 445 routes to (its h tag), or NULL. */
static GhMlsGroup *
group_of_envelope(GhMlsService *self, const gchar *event_json)
{
  NostrEvent *event = event_json ? nostr_event_new() : NULL;
  const gchar *h = NULL;
  g_autofree gchar *routing = NULL;
  if (event && nostr_event_deserialize_compact(event, event_json, NULL) == 1) {
    NostrTags *tags = nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags) && !h; i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (tag && nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "h") == 0)
        h = nostr_tag_get(tag, 1);
    }
    routing = g_strdup(h);
  }
  if (event)
    nostr_event_free(event);
  for (guint i = 0; routing && i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    if (g_str_equal(group->nostr_hex, routing))
      return group;
  }
  return NULL;
}

/* After a start, a reconnect or a retry: pending Commits, unsent Welcomes and
 * unfinished sends, each republished exactly as stored. */
static void
resume_all(GhMlsService *self)
{
  if (!running(self))
    return;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  g_autoptr(GPtrArray) commits = gh_mls_commit_resume(self->store, self->marmot, self->storage,
                                                      self->account, &error);
  if (!commits) {
    g_message("Groundhog could not resume its group changes: %s", error->message);
    g_clear_error(&error);
  }
  while (commits && commits->len) {
    GhMlsCommitPublish *publish = g_ptr_array_steal_index(commits, 0);
    MarmotGroupId gid = { 0 };
    GhMlsGroup *group = gid_from_hex(publish->group_id_hex, &gid) ? ensure_group(self, &gid)
                                                                  : NULL;
    marmot_group_id_free(&gid);
    if (group)
      round_start(group, publish);
    else
      gh_mls_commit_publish_free(publish);
  }
  g_autoptr(GArray) ids = gh_store_outbox_list_unfinished(self->store, GH_STORE_BACKEND_MLS,
                                                          &error);
  for (guint i = 0; ids && i < ids->len; i++) {
    g_autoptr(GhStoreOutboxEntry) entry =
      gh_store_outbox_load(self->store, g_array_index(ids, gint64, i), NULL);
    gint kind = 0;
    g_autofree gchar *id = entry ? unsigned_id_of(entry->rumor_json, &kind) : NULL;
    if (!id || kind != GH_MESSAGE_MLS_KIND || entry->events->len != 1)
      continue;   /* Commits: gh-mls-commits; Welcome wraps: welcomes_pump */
    GhStoreOutboxEvent *event = g_ptr_array_index(entry->events, 0);
    send_resume(self, entry, group_of_envelope(self, event->event_json));
  }
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    group_refresh(group);
    welcomes_pump(group);
    leave_continue(group);
    departures_schedule(group);
    upgrade_schedule(group);
  }
  key_package_sweep(self);
  key_package_maybe_publish(self);
}

/* ---- Generation ----------------------------------------------------------------------- */

static void
stop_generation(GhMlsService *self)
{
  self->run++;
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  if (self->retry) {
    gh_clock_source_remove(self->clock, self->retry);
    self->retry = 0;
  }
  self->retry_s = 0;
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    KeyPackageSlot *slot = &self->kp[f];
    if (slot->publish) {
      gh_relay_publish_cancel(slot->publish);
      g_clear_pointer(&slot->publish, gh_relay_publish_unref);
    }
    slot->busy = FALSE;
    slot->in_flight = FALSE;
    slot->held = FALSE;
    slot->withdrawing = FALSE;
    slot->withdrawal = NULL;
  }
  key_package_hold_stop(self, FALSE);
  if (self->key_package_sweep_timer) {
    gh_clock_source_remove(self->clock, self->key_package_sweep_timer);
    self->key_package_sweep_timer = 0;
  }
  g_hash_table_remove_all(self->deliveries);
  /* Verifies in flight were cancelled: nothing is "checking" any more. */
  {
    GHashTableIter iter;
    gpointer account;
    g_hash_table_iter_init(&iter, self->verifying);
    while (g_hash_table_iter_next(&iter, &account, NULL))
      verify_mark(self, account, GH_MLS_MEMBER_CHECKING, GH_MLS_MEMBER_UNVERIFIED);
    g_hash_table_remove_all(self->verifying);
  }
  g_autoptr(GError) cancelled = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "The account changed; the change stays "
                                                    "pending and is sent again later");
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    group_unsubscribe(group);
    if (group->round) {
      Round *round = g_steal_pointer(&group->round);
      round_free(round);
    }
    leave_cancel(group);
    departures_cancel(group);
    upgrade_cancel(group);
    /* Held events stay (review M1): a network flap must not lose them. */
    complete_waiters(group, cancelled);
    queued_cancel(group, cancelled);
  }
}

static void
update_activity(GhMlsService *self)
{
  guint64 generation = 0;
  if (!self->disposed && self->accounts &&
      gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
    g_autofree gchar *active = npub ? gh_identity_pubkey_hex(npub) : NULL;
    if (g_strcmp0(active, self->account) == 0)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  gboolean online = self->network && g_network_monitor_get_network_available(self->network);
  if (generation == self->generation && online == self->online)
    return;
  /* Nothing of the old generation (or connection) may complete in the new. */
  stop_generation(self);
  identity_new_generation(self, generation);
  self->generation = generation;
  self->online = online;
  if (!running(self)) {
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_NONE);
    return;
  }
  self->cancellable = g_cancellable_new();
  identity_enroll(self);
  for (guint i = 0; i < self->groups->len; i++)
    group_subscribe(g_ptr_array_index(self->groups, i));
  resume_all(self);
}

static void
on_accounts_changed(GhAccountController *accounts, gpointer data)
{
  (void)accounts;
  update_activity(data);
}

static void
on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer data)
{
  (void)monitor;
  (void)available;
  update_activity(data);
}

static void
on_relays_changed(GhAccountRelays *relays, gpointer data)
{
  (void)relays;
  key_package_maybe_publish(data);
}

/* ---- Object --------------------------------------------------------------------------- */

/* "Let people using older Marmot apps invite me" (review L1): on, the MDK
 * 0.8 KeyPackage is published again; off, it is withdrawn and its keys
 * retired once a relay accepted the request. */
static void
on_legacy_key_packages_changed(GhMlsService *self, gchar *key, GSettings *settings)
{
  (void)key;
  (void)settings;
  key_package_update_state(self);
  self->key_package_recheck = TRUE;   /* a round in flight looks again when it ends */
  key_package_maybe_publish(self);
}

static void
on_verified_only_changed(GSettings *settings, gchar *key, gpointer data)
{
  (void)key;
  GhMlsService *self = data;
  self->allow_unproven = !gh_mls_requires_proofs(settings);
  marmot_set_allow_unproven_members(self->marmot, self->allow_unproven);
  if (!self->allow_unproven)
    return;
  /* A Commit refused only because proofs were required applies now. */
  for (guint i = 0; i < self->groups->len; i++) {
    GhMlsGroup *group = g_ptr_array_index(self->groups, i);
    if (!group->refused_json || !group->active ||
        group->refusal == GH_MLS_REFUSAL_UNFOLLOWABLE)
      continue;
    g_autofree gchar *json = g_strdup(group->refused_json);
    process_event(group, json, NULL, FALSE, NULL);
  }
}

GhMlsService *
gh_mls_service_new(const GhMlsServiceConfig *config, GError **error)
{
  g_return_val_if_fail(config != NULL && config->store != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  g_return_val_if_fail(!config->account_relays || GH_IS_ACCOUNT_RELAYS(config->account_relays),
                       NULL);
  g_return_val_if_fail(!config->inboxes || GH_IS_INBOX_RESOLVER(config->inboxes), NULL);
  g_return_val_if_fail(!config->settings || G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(!config->inbox || GH_IS_DM_INBOX(config->inbox), NULL);
  const gchar *account = gh_store_get_account_pubkey(config->store);
  if (!lower_hex64(account)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No store account");
    return NULL;
  }
  MarmotStorage *storage = gh_store_marmot_new(config->store, error);
  if (!storage)
    return NULL;   /* an ephemeral store: MLS needs durable state (KC-4) */
  /* libmarmot's default admits members without the proof in legacy-profile
   * groups only (nostrc-6ukh); the account may require proofs instead. */
  MarmotConfig marmot_config = marmot_config_default();
  marmot_config.allow_unproven_members = !gh_mls_requires_proofs(config->settings);
  /* nostrc-8ndz: the first Add requires SelfRemove when everyone supports it
   * (libmarmot's default); a test may make permissive groups instead. */
  marmot_config.keep_first_add_permissive = upgrade_permissive();
  Marmot *marmot = marmot_new_with_config(storage, &marmot_config);
  if (!marmot) {
    marmot_storage_free(storage);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "libmarmot did not start");
    return NULL;
  }
  g_autoptr(GhMlsService) self = g_object_new(GH_TYPE_MLS_SERVICE, NULL);
  self->store = config->store;
  self->storage = storage;
  self->marmot = marmot;
  self->allow_unproven = marmot_config.allow_unproven_members;
  self->account = g_strdup(account);
  nostr_hex2bin(self->account_key, account, sizeof self->account_key);
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->accounts = g_object_ref(config->accounts);
  self->policy = g_object_ref(gh_auth_policy_get_for_accounts(config->accounts));
  self->max_backfill_events = GH_MLS_SERVICE_MAX_BACKFILL_EVENTS;
  self->max_backfill_bytes = GH_MLS_SERVICE_MAX_BACKFILL_BYTES;
  self->backfill_quiet_us = (gint64)GH_MLS_SERVICE_BACKFILL_QUIET_S * G_USEC_PER_SEC;
  self->pending_shown_us = (gint64)GH_MLS_SERVICE_PENDING_SHOWN_S * G_USEC_PER_SEC;
  self->conversations = g_object_ref(config->conversations);
  self->reactions = config->reactions ? g_object_ref(config->reactions) : NULL;
  self->account_relays = config->account_relays ? g_object_ref(config->account_relays) : NULL;
  self->inboxes = config->inboxes ? g_object_ref(config->inboxes) : NULL;
  self->settings = config->settings ? g_object_ref(config->settings) : NULL;
  if (self->settings) {
    g_signal_connect_object(self->settings, "changed::" VERIFIED_ONLY_KEY,
                            G_CALLBACK(on_verified_only_changed), self, 0);
    g_signal_connect_object(self->settings, "changed::" LEGACY_KEY_PACKAGES_KEY,
                            G_CALLBACK(on_legacy_key_packages_changed), self,
                            G_CONNECT_SWAPPED);
  }
  self->may_look_up = config->may_look_up;
  self->consent_data = config->consent_data;
  self->network = g_object_ref(config->network ? config->network
                                               : g_network_monitor_get_default());
  self->publish_deadline = config->publish_deadline;
  self->lookup_deadline = config->lookup_deadline;
  self->key_package_max_hold = config->key_package_max_hold > 0 ? config->key_package_max_hold
                                                                : GH_MLS_KEY_PACKAGE_MAX_HOLD;
  self->legacy_only = config->legacy_key_packages_only;
  self->key_package_lifetime = config->key_package_lifetime > 0 ? config->key_package_lifetime
                                                                : GH_MLS_KEY_PACKAGE_LIFETIME;

  /* Stored rooms first, then every group libmarmot holds. */
  self->rooms = gh_store_mls_new(config->store);
  if (!gh_store_mls_attach(self->rooms, config->conversations, 0, error))
    return NULL;
  MarmotGroup **groups = NULL;
  size_t n = 0;
  MarmotError err = marmot_get_all_groups(marmot, &groups, &n);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "Listing the encrypted groups", error);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    ensure_group(self, &groups[i]->mls_group_id);
    marmot_group_free(groups[i]);
  }
  free(groups);

  if (config->inbox) {
    self->inbox = g_object_ref(config->inbox);
    gh_dm_inbox_set_welcome_sink(self->inbox, welcome_sink, self);
  }
  self->accounts_handler = g_signal_connect(self->accounts, "changed",
                                            G_CALLBACK(on_accounts_changed), self);
  self->network_handler = g_signal_connect(self->network, "network-changed",
                                           G_CALLBACK(on_network_changed), self);
  if (self->account_relays)
    self->relays_handler = g_signal_connect(self->account_relays, "changed",
                                            G_CALLBACK(on_relays_changed), self);
  update_activity(self);
  return g_steal_pointer(&self);
}

const gchar *
gh_mls_service_get_account(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  return self->account;
}

Marmot *
gh_mls_service_get_marmot(GhMlsService *self)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  return self->marmot;
}

GhMlsGroup *
gh_mls_service_lookup(GhMlsService *self, const gchar *group_id_or_room_id)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  g_autofree gchar *hex = NULL;
  if (!gh_message_mls_room_split(group_id_or_room_id, &hex))
    hex = g_strdup(group_id_or_room_id);
  return find_group_hex(self, hex);
}

/* ---- NIP-88 polls (nostrc-a36s) ----------------------------------------- */

GhMlsPoll *
gh_mls_service_lookup_poll(GhMlsService *self, const gchar *group_id_hex,
                           const gchar *poll_event_id)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!group_id_hex || !poll_event_id) return NULL;
  g_autofree gchar *key = g_strdup_printf("%s:%s", group_id_hex, poll_event_id);
  return g_hash_table_lookup(self->polls, key);
}

GhMessage *
gh_mls_service_create_poll(GhMlsService *self, GhMlsGroup *group,
                           const gchar *question,
                           const gchar **option_labels, guint n_options,
                           GhMlsPollType poll_type, gint64 ends_at,
                           GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!check_running(self, error))
    return NULL;
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active ||
      group->leaving) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A poll needs an active group it is not leaving");
    return NULL;
  }
  if (!group->relays[0]) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_RELAYS,
                        "The group has no relays");
    return NULL;
  }

  gint64 now = now_s(self);
  g_autofree gchar *inner = gh_mls_poll_build_event(
    self->account, group->nostr_hex, now, question, option_labels, n_options,
    poll_type, ends_at, error);
  if (!inner)
    return NULL;

  g_autofree gchar *inner_id = NULL;
  {
    NostrEvent *ev = nostr_event_new();
    gchar id_buf[65] = { 0 };
    if (ev && nostr_event_deserialize_unsigned(ev, inner, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_compute_id(ev, id_buf) == NOSTR_EVENT_VALIDATION_OK)
      inner_id = g_strdup(id_buf);
    if (ev) nostr_event_free(ev);
  }
  if (!inner_id) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not compute poll event id");
    return NULL;
  }

  g_autoptr(GhMessage) message = gh_message_new_from_mls(self->account, group->gid_hex,
                                                          inner, error);
  if (!message) return NULL;

  /* Store+send via the same outbox path as text messages. */
  g_autofree gchar *op_id = gh_store_new_op_id();
  gint64 conversation_id = 0, outbox_id = 0, message_id = 0;
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  g_autofree gchar *envelope_id = NULL;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, error))
    return NULL;
  GhStoreOutgoing outgoing = {
    .op_id = op_id,
    .backend_msg_id = inner_id,
    .sender_pubkey = self->account,
    .kind = GH_MLS_POLL_KIND,
    .created_at = gh_message_get_created_at(message),
    .body = question,
    .rumor_json = inner,
  };
  GhStoreSealedEvent sealed = {
    .role = GH_STORE_OUTBOX_ROLE_MLS_MESSAGE,
    .relay_urls = (const gchar *const *) group->relays,
  };
  MarmotError err = MARMOT_OK;
  if (!gh_store_mls_save_room(self->store, group->gid_hex, NULL, &conversation_id, error))
    goto poll_fail;
  outgoing.conversation_id = conversation_id;
  if (!gh_store_enqueue(self->store, &outgoing, &outbox_id, &message_id, error))
    goto poll_fail;
  err = marmot_create_message(self->marmot, &group->gid, inner, &out);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "The poll could not be encrypted", error);
    goto poll_fail;
  }
  err = marmot_save_created_message(self->marmot, &group->gid, out.event_json,
                                    out.message && out.message->content ? out.message->content : inner);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "The poll could not be stored", error);
    goto poll_fail;
  }
  envelope_id = event_id_of(out.event_json);
  sealed.event_id = envelope_id;
  sealed.event_json = out.event_json;
  if (!envelope_id || !gh_store_seal(self->store, outbox_id, &sealed, 1, error))
    goto poll_fail;
  if (!gh_store_commit(self->store, error)) {
    marmot_outgoing_message_free(&out);
    return NULL;
  }
  entry = gh_store_outbox_load(self->store, outbox_id, NULL);
  send_listed(self, group, message, outbox_id, entry, out.event_json);
  marmot_outgoing_message_free(&out);

  /* Track the poll locally. */
  {
    GhMlsPoll *poll = gh_mls_poll_new_from_event(inner_id, self->account,
                                                  gh_message_get_created_at(message),
                                                  inner, NULL);
    if (poll) {
      gh_mls_poll_set_local_account(poll, self->account);
      gchar *pk = g_strdup_printf("%s:%s", group->gid_hex, inner_id);
      g_hash_table_replace(self->polls, pk, poll);
    }
  }

  return g_steal_pointer(&message);

poll_fail:
  gh_store_rollback(self->store);
  marmot_outgoing_message_free(&out);
  return NULL;
}

GhMessage *
gh_mls_service_cast_vote(GhMlsService *self, GhMlsGroup *group,
                         const gchar *poll_event_id,
                         const gchar **option_ids, guint n_options,
                         GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!check_running(self, error))
    return NULL;
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active ||
      group->leaving) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A vote needs an active group it is not leaving");
    return NULL;
  }
  if (!group->relays[0]) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_RELAYS,
                        "The group has no relays");
    return NULL;
  }

  /* Validate the poll exists. */
  g_autofree gchar *poll_key = g_strdup_printf("%s:%s", group->gid_hex, poll_event_id);
  GhMlsPoll *poll = g_hash_table_lookup(self->polls, poll_key);
  if (!poll) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Poll not found");
    return NULL;
  }

  gint64 now = now_s(self);
  if (!gh_mls_poll_is_open(poll, now)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Poll is closed");
    return NULL;
  }

  g_autofree gchar *inner = gh_mls_poll_build_vote_event(
    self->account, group->nostr_hex, now, poll_event_id,
    option_ids, n_options, error);
  if (!inner)
    return NULL;

  g_autofree gchar *inner_id = NULL;
  {
    NostrEvent *ev = nostr_event_new();
    gchar id_buf[65] = { 0 };
    if (ev && nostr_event_deserialize_unsigned(ev, inner, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_compute_id(ev, id_buf) == NOSTR_EVENT_VALIDATION_OK)
      inner_id = g_strdup(id_buf);
    if (ev) nostr_event_free(ev);
  }
  if (!inner_id) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not compute vote event id");
    return NULL;
  }

  g_autoptr(GhMessage) message = gh_message_new_from_mls(self->account, group->gid_hex,
                                                          inner, error);
  if (!message) return NULL;

  g_autofree gchar *op_id = gh_store_new_op_id();
  gint64 conversation_id = 0, outbox_id = 0, message_id = 0;
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  g_autofree gchar *envelope_id = NULL;
  g_autoptr(GhStoreOutboxEntry) entry = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, error))
    return NULL;
  GhStoreOutgoing outgoing = {
    .op_id = op_id,
    .backend_msg_id = inner_id,
    .sender_pubkey = self->account,
    .kind = GH_MLS_POLL_VOTE_KIND,
    .created_at = gh_message_get_created_at(message),
    .body = "",
    .rumor_json = inner,
  };
  GhStoreSealedEvent sealed = {
    .role = GH_STORE_OUTBOX_ROLE_MLS_MESSAGE,
    .relay_urls = (const gchar *const *) group->relays,
  };
  MarmotError merr = MARMOT_OK;
  if (!gh_store_mls_save_room(self->store, group->gid_hex, NULL, &conversation_id, error))
    goto vote_fail;
  outgoing.conversation_id = conversation_id;
  if (!gh_store_enqueue(self->store, &outgoing, &outbox_id, &message_id, error))
    goto vote_fail;
  merr = marmot_create_message(self->marmot, &group->gid, inner, &out);
  if (merr != MARMOT_OK) {
    marmot_fail(self, merr, "The vote could not be encrypted", error);
    goto vote_fail;
  }
  merr = marmot_save_created_message(self->marmot, &group->gid, out.event_json,
                                     out.message && out.message->content ? out.message->content : inner);
  if (merr != MARMOT_OK) {
    marmot_fail(self, merr, "The vote could not be stored", error);
    goto vote_fail;
  }
  envelope_id = event_id_of(out.event_json);
  sealed.event_id = envelope_id;
  sealed.event_json = out.event_json;
  if (!envelope_id || !gh_store_seal(self->store, outbox_id, &sealed, 1, error))
    goto vote_fail;
  if (!gh_store_commit(self->store, error)) {
    marmot_outgoing_message_free(&out);
    return NULL;
  }
  entry = gh_store_outbox_load(self->store, outbox_id, NULL);
  send_listed(self, group, message, outbox_id, entry, out.event_json);
  marmot_outgoing_message_free(&out);

  /* Apply the vote to the local poll. */
  gh_mls_poll_apply_vote(poll, self->account, option_ids, now);

  return g_steal_pointer(&message);

vote_fail:
  gh_store_rollback(self->store);
  marmot_outgoing_message_free(&out);
  return NULL;
}

void
gh_mls_service_rebuild_poll_from_stored(GhMlsService *self,
                                        const gchar *group_id_hex,
                                        GhMessage *message)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  g_return_if_fail(group_id_hex != NULL);
  g_return_if_fail(GH_IS_MESSAGE(message));

  gint kind = gh_message_get_kind(message);
  const gchar *raw = gh_message_get_rumor_json(message);
  if (!raw) return;

  if (kind == GH_MLS_POLL_KIND) {
    const gchar *rumor_id = gh_message_get_rumor_id(message);
    if (!rumor_id) return;
    g_autofree gchar *key = g_strdup_printf("%s:%s", group_id_hex, rumor_id);
    /* Skip if already tracked. */
    if (g_hash_table_contains(self->polls, key)) return;
    g_autoptr(GError) err = NULL;
    GhMlsPoll *poll = gh_mls_poll_new_from_event(
      rumor_id, gh_message_get_sender(message),
      gh_message_get_created_at(message), raw, &err);
    if (poll) {
      gh_mls_poll_set_local_account(poll, self->account);
      g_hash_table_replace(self->polls, g_steal_pointer(&key), poll);
    }
  } else if (kind == GH_MLS_POLL_VOTE_KIND) {
    g_autoptr(GError) err = NULL;
    g_autofree gchar *target_id = NULL;
    g_auto(GStrv) option_ids = NULL;
    if (gh_mls_poll_parse_vote(raw, &target_id, &option_ids, &err)) {
      g_autofree gchar *key = g_strdup_printf("%s:%s", group_id_hex, target_id);
      GhMlsPoll *poll = g_hash_table_lookup(self->polls, key);
      if (poll) {
        /* F3: reject votes outside the poll's lifetime (as MDK does). */
        gint64 vote_ts = gh_message_get_created_at(message);
        if (gh_mls_poll_is_open(poll, vote_ts) &&
            vote_ts >= gh_mls_poll_get_created_at(poll))
          gh_mls_poll_apply_vote(poll, gh_message_get_sender(message),
                                 (const gchar **) option_ids, vote_ts);
      }
    }
  }
}

static GType
list_get_item_type(GListModel *model)
{
  (void)model;
  return GH_TYPE_MLS_GROUP;
}

static guint
list_get_n_items(GListModel *model)
{
  return GH_MLS_SERVICE(model)->groups->len;
}

static gpointer
list_get_item(GListModel *model, guint position)
{
  GhMlsService *self = GH_MLS_SERVICE(model);
  return position < self->groups->len ? g_object_ref(g_ptr_array_index(self->groups, position))
                                      : NULL;
}

static void
gh_mls_service_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_n_items = list_get_n_items;
  iface->get_item = list_get_item;
}

static void
gh_mls_service_dispose(GObject *object)
{
  GhMlsService *self = GH_MLS_SERVICE(object);
  if (!self->disposed) {
    self->disposed = TRUE;
    if (self->inbox)
      gh_dm_inbox_set_welcome_sink(self->inbox, NULL, NULL);
    if (self->accounts_handler)
      g_clear_signal_handler(&self->accounts_handler, self->accounts);
    if (self->network_handler)
      g_clear_signal_handler(&self->network_handler, self->network);
    if (self->relays_handler)
      g_clear_signal_handler(&self->relays_handler, self->account_relays);
    stop_generation(self);
    identity_new_generation(self, 0);
    self->generation = 0;
    if (self->rooms)
      gh_store_mls_close(self->rooms);
  }
  G_OBJECT_CLASS(gh_mls_service_parent_class)->dispose(object);
}

static void
gh_mls_service_finalize(GObject *object)
{
  GhMlsService *self = GH_MLS_SERVICE(object);
  for (guint i = 0; i < self->groups->len; i++)
    ((GhMlsGroup *)g_ptr_array_index(self->groups, i))->service = NULL;
  g_ptr_array_unref(self->groups);
  g_hash_table_unref(self->deliveries);
  g_hash_table_unref(self->verifying);
  g_clear_pointer(&self->polls, g_hash_table_unref);
  g_ptr_array_unref(self->known_key_packages);
  if (self->marmot)
    marmot_free(self->marmot);   /* and its storage */
  g_clear_object(&self->rooms);
  g_clear_object(&self->inbox);
  g_clear_object(&self->identity_cancellable);
  g_clear_object(&self->accounts);
  g_clear_object(&self->policy);
  g_clear_object(&self->conversations);
  g_clear_object(&self->reactions);
  g_clear_object(&self->account_relays);
  g_clear_object(&self->inboxes);
  g_clear_object(&self->settings);
  g_clear_object(&self->network);
  if (self->clock)
    gh_clock_unref(self->clock);
  g_free(self->account);
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    g_free(self->kp[f].id);
    g_ptr_array_unref(self->kp[f].event_ids);
  }
  G_OBJECT_CLASS(gh_mls_service_parent_class)->finalize(object);
}

static void
gh_mls_service_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMlsService *self = GH_MLS_SERVICE(object);
  switch (id) {
  case PROP_KEY_PACKAGE_STATE: g_value_set_enum(value, self->key_package); break;
  case PROP_KEY_PACKAGE_HELD: g_value_set_boolean(value, self->key_package_held); break;
  case PROP_IDENTITY_STATE: g_value_set_enum(value, self->identity); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_mls_service_class_init(GhMlsServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_mls_service_dispose;
  object_class->finalize = gh_mls_service_finalize;
  object_class->get_property = gh_mls_service_get_property;
  props[PROP_KEY_PACKAGE_STATE] =
    g_param_spec_enum("key-package-state", NULL, NULL, GH_TYPE_MLS_KEY_PACKAGE_STATE,
                      GH_MLS_KEY_PACKAGE_NONE,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  /* gh_mls_service_get_key_package_held() (nostrc-0bdg re-review A5). */
  props[PROP_KEY_PACKAGE_HELD] =
    g_param_spec_boolean("key-package-held", NULL, NULL, FALSE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_IDENTITY_STATE] =
    g_param_spec_enum("identity-state", NULL, NULL, GH_TYPE_MLS_IDENTITY_STATE,
                      GH_MLS_IDENTITY_NONE,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
  signals[SIGNAL_INVITE_RECEIVED] =
    g_signal_new("invite-received", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_GROUP_ADDED] =
    g_signal_new("group-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, GH_TYPE_MLS_GROUP);
}

static void
gh_mls_service_init(GhMlsService *self)
{
  self->identity = GH_MLS_SERVICE_ACCOUNT_PROOF ? GH_MLS_IDENTITY_NONE
                                                : GH_MLS_IDENTITY_NOT_REQUIRED;
  self->groups = g_ptr_array_new_with_free_func(g_object_unref);
  self->deliveries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, delivery_free);
  self->verifying = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->known_key_packages = g_ptr_array_new_with_free_func(g_free);
  self->polls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  for (guint f = 0; f < GH_MLS_KEY_PACKAGE_N_FORMATS; f++) {
    self->kp[f].service = self;
    self->kp[f].format = (GhMlsKeyPackageFormat)f;
    self->kp[f].event_ids = g_ptr_array_new_with_free_func(g_free);
  }
}
