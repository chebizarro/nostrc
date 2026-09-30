#include "gh-mls-service.h"

#include "gh-auth-policy.h"
#include "gh-conversation-private.h"
#include "gh-identity.h"
#include "gh-message-status.h"
#include "gh-mls-commits.h"
#include "gh-mls-key-packages.h"
#include "gh-nip17-envelope.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"
#include "gh-store-marmot.h"
#include "gh-store-mls.h"

#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(gh-mls-service-error-quark, gh_mls_service_error)

/* Store cursors (gh-store.h `cursors`): the KeyPackage's last publish time
 * and each group's read cursor ("mls/" + a hash of the group id: the scope
 * is bounded and names no group). */
#define KEY_PACKAGE_CURSOR "mls/key-package"
/* A new joiner reads back this far (its Welcome may arrive late). */
#define JOIN_BACKFILL ((gint64)2 * 24 * 3600)
/* Retry of unanswered Commits, Welcomes and sends: jittered, doubling. */
#define RETRY_MIN_S 15
#define RETRY_MAX_S 600
#define MAX_GROUP_RELAYS 16

/* GhRelayPublishOutcome values the store keeps (gh-mls-commits.c too). */
enum { OUTCOME_ACCEPTED = GH_RELAY_PUBLISH_ACCEPTED, OUTCOME_REJECTED = GH_RELAY_PUBLISH_REJECTED };

typedef struct _Round Round;
typedef struct _Delivery Delivery;

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
  GhMlsReadState read;
  gboolean is_admin;
  gboolean pending_commit;
  guint unsent_welcomes;
  GStrv members;
  GStrv admins;
  GStrv relays;
  /* Reading: one scope on the group relays. */
  GhRelayScope *scope;
  GHashTable *settled;       /* url -> GINT_TO_POINTER(1 eose / 2 failed / 3 never
                              * subscribed / 4 eose, older events missing) */
  gint64 cursor;             /* everything before it was processed */
  gint64 newest;             /* newest accepted created_at this session (bounded) */
  GQueue backfill;           /* Stored: backfill of every relay, applied oldest first */
  guint backfill_seq;
  GHashTable *backfilling;   /* urls delivering a backfill round that has not ended */
  gsize backfill_bytes;      /* JSON bytes in backfill (review B4 bound) */
  gboolean history_incomplete; /* a relay's backfill ended incomplete (settled 4) */
  GQueue held;               /* Held: kind 445 of a later epoch, oldest first */
  GHashTable *held_ids;      /* their event ids (owned by the Held records) */
  gint64 pinned;             /* oldest created_at dropped unread this session; 0: none */
  gint64 floor;              /* when the account joined (or made) the group; 0: unknown */
  gboolean retrying;         /* retry_held() runs: a nested Commit only asks again */
  gboolean retry_again;
  guint fresh_commits;       /* Commits applied not out of the queue, since the last aging */
  /* Changes: the Commit being published, and who waits for it. */
  Round *round;
  GPtrArray *waiters;        /* GTask */
};

enum {
  GROUP_PROP_0,
  GROUP_PROP_GROUP_ID,
  GROUP_PROP_ROOM_ID,
  GROUP_PROP_NAME,
  GROUP_PROP_DESCRIPTION,
  GROUP_PROP_EPOCH,
  GROUP_PROP_ACTIVE,
  GROUP_PROP_READ_STATE,
  GROUP_PROP_IS_ADMIN,
  GROUP_PROP_PENDING_COMMIT,
  GROUP_PROP_UNSENT_WELCOMES,
  GROUP_PROP_UNREADABLE,
  GROUP_PROP_HISTORY_INCOMPLETE,
  N_GROUP_PROPS
};
static GParamSpec *group_props[N_GROUP_PROPS];
enum { GROUP_SIGNAL_MEMBERS_CHANGED, N_GROUP_SIGNALS };
static guint group_signals[N_GROUP_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsGroup, gh_mls_group, G_TYPE_OBJECT)

struct _GhMlsService {
  GObject parent_instance;
  GhStore *store;                  /* borrowed */
  MarmotStorage *storage;          /* owned by marmot */
  Marmot *marmot;
  GhStoreMls *rooms;
  gchar *account;
  guint8 account_key[32];
  GhClock *clock;
  GhAccountController *accounts;
  GhAuthPolicy *policy;
  GhConversationStore *conversations;
  guint max_backfill_events;       /* per group (review B4) */
  gsize max_backfill_bytes;
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

  /* KeyPackage */
  GhMlsKeyPackageState key_package;
  gchar *key_package_id;
  gboolean key_package_busy;
  gboolean key_package_rotate;
  GhRelayPublish *key_package_publish;

  /* Welcomes and sends in flight */
  GHashTable *deliveries;          /* key (welcome id hex or outbox id) -> Delivery */
  guint retry;
  guint retry_s;
};

enum { PROP_0, PROP_KEY_PACKAGE_STATE, PROP_IDENTITY_STATE, N_PROPS };
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
  guint misses;        /* Commits applied since, without it becoming readable */
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
} Stored;

static void
stored_free(gpointer data)
{
  Stored *stored = data;
  g_free(stored->json);
  g_free(stored->url);
  g_free(stored);
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
  case MARMOT_ERR_OWN_COMMIT_PENDING: code = GH_MLS_SERVICE_ERROR_BUSY; break;
  case MARMOT_ERR_ADMIN_ONLY:
  case MARMOT_ERR_COMMIT_FROM_NON_ADMIN: code = GH_MLS_SERVICE_ERROR_NOT_ADMIN; break;
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  case MARMOT_ERR_KEY_PACKAGE_IDENTITY:
    /* An unproven leaf (MDK 0.8, libmarmot <= 0.9.0) in a KeyPackage, a
     * Commit or a Welcome's tree (nostrc-7vyi). */
    g_set_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NEEDS_UPDATE,
                "%s: someone uses an app that can't prove their account yet; they need to "
                "update it", what);
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

static void
group_set_read(GhMlsGroup *group, GhMlsReadState read)
{
  if (group->read == read)
    return;
  group->read = read;
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_READ_STATE]);
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
  case GROUP_PROP_READ_STATE: g_value_set_enum(value, self->read); break;
  case GROUP_PROP_IS_ADMIN: g_value_set_boolean(value, self->is_admin); break;
  case GROUP_PROP_PENDING_COMMIT: g_value_set_boolean(value, self->pending_commit); break;
  case GROUP_PROP_UNSENT_WELCOMES: g_value_set_uint(value, self->unsent_welcomes); break;
  case GROUP_PROP_UNREADABLE: g_value_set_uint(value, g_queue_get_length(&self->held)); break;
  case GROUP_PROP_HISTORY_INCOMPLETE: g_value_set_boolean(value, self->history_incomplete); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_mls_group_finalize(GObject *object)
{
  GhMlsGroup *self = GH_MLS_GROUP(object);
  g_warn_if_fail(self->scope == NULL && self->round == NULL);
  marmot_group_id_free(&self->gid);
  g_free(self->gid_hex);
  g_free(self->room_id);
  g_free(self->name);
  g_free(self->description);
  g_strfreev(self->members);
  g_strfreev(self->admins);
  g_strfreev(self->relays);
  g_hash_table_unref(self->settled);
  g_hash_table_unref(self->held_ids);
  g_queue_clear_full(&self->held, held_free);
  g_queue_clear_full(&self->backfill, stored_free);
  g_hash_table_unref(self->backfilling);
  g_ptr_array_unref(self->waiters);
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
  group_props[GROUP_PROP_HISTORY_INCOMPLETE] =
    g_param_spec_boolean("history-incomplete", NULL, NULL, FALSE, ro);
  g_object_class_install_properties(object_class, N_GROUP_PROPS, group_props);
  group_signals[GROUP_SIGNAL_MEMBERS_CHANGED] =
    g_signal_new("members-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                 NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_mls_group_init(GhMlsGroup *self)
{
  self->settled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->waiters = g_ptr_array_new_with_free_func(g_object_unref);
  g_queue_init(&self->held);
  g_queue_init(&self->backfill);
  self->backfilling = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->held_ids = g_hash_table_new(g_str_hash, g_str_equal);
  self->members = g_new0(gchar *, 1);
  self->admins = g_new0(gchar *, 1);
  self->relays = g_new0(gchar *, 1);
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

static gboolean
account_matches_model(GhMlsService *self)
{
  return g_strcmp0(gh_conversation_store_get_account(self->conversations), self->account) == 0;
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
  if (account_matches_model(self))
    gh_conversation_store_ensure_group(self->conversations, group->room_id, group->name);
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
    group->name = g_strdup(g->name && *g->name ? g->name : NULL);
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
  if (group->active != active) {
    group->active = active;
    g_object_notify_by_pspec(object, group_props[GROUP_PROP_ACTIVE]);
  }
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
  if (marmot_get_group_members(m, &group->gid, &keys, &n_keys) == MARMOT_OK) {
    GPtrArray *members = g_ptr_array_new();
    for (size_t i = 0; i < n_keys; i++)
      g_ptr_array_add(members, to_hex(keys[i], 32));
    GStrv list = sorted_strv(members);
    if (!strv_equal((const gchar *const *)list, (const gchar *const *)group->members)) {
      g_strfreev(group->members);
      group->members = list;
      members_changed = TRUE;
    } else {
      g_strfreev(list);
    }
  }
  free(keys);

  MarmotGroupRelay *relays = NULL;
  size_t n_relays = 0;
  if (marmot_get_group_relay_urls(m, &group->gid, &relays, &n_relays) == MARMOT_OK) {
    GPtrArray *urls = g_ptr_array_new();
    for (size_t i = 0; i < n_relays; i++) {
      if (relays[i].relay_url && !g_ptr_array_find_with_equal_func(urls, relays[i].relay_url,
                                                                   g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(relays[i].relay_url));
      free(relays[i].relay_url);
      marmot_group_id_free(&relays[i].mls_group_id);
    }
    free(relays);
    GStrv list = sorted_strv(urls);
    if (!strv_equal((const gchar *const *)list, (const gchar *const *)group->relays)) {
      g_strfreev(group->relays);
      group->relays = list;
      routing_changed = TRUE;
    } else {
      g_strfreev(list);
    }
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
  if (name_changed && group->active)
    group_list_room(group);
  if (!group->active)
    group_unsubscribe(group);
  else if (routing_changed && group->scope)
    group_subscribe(group);
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
  if (group->scope) {
    gh_relay_scope_cancel(group->scope);
    g_clear_pointer(&group->scope, gh_relay_scope_unref);
  }
  /* Not read yet, so nothing moved the cursor past them: the next
   * subscription fetches them again. */
  g_queue_clear_full(&group->backfill, stored_free);
  group->backfill_bytes = 0;
  g_hash_table_remove_all(group->backfilling);
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
                                  Held *retry);

/* Keeps a kind 445 for a later epoch: once per event id; when the queue is
 * full the oldest one goes (never the new arrival silently), and the cursor
 * stays behind it so the next subscription fetches it again (review M1). */
static void
hold_event(GhMlsGroup *group, const gchar *id, const gchar *json, gint64 created_at)
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
  g_queue_push_tail(&group->held, held);
  g_hash_table_add(group->held_ids, held->id);
  g_object_notify_by_pspec(G_OBJECT(group), group_props[GROUP_PROP_UNREADABLE]);
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
 * Junk: anyone can post a kind 445 with the group's public h. Once the
 * fixpoint ends, every event still unreadable counts one miss per Commit
 * that came from a relay (or was our own, merged) since the last count --
 * never per Commit applied out of this queue -- and one that reaches
 * GH_MLS_SERVICE_JUNK_AFTER_COMMITS misses is dropped (and no longer holds
 * the cursor back). */
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
      if (at <= pass_ends && group->active) {
        gboolean before = group->retry_again;
        if (process_event(group, held->json, NULL, held) != EVENT_HELD) {
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
  guint fresh = group->fresh_commits;
  group->fresh_commits = 0;
  for (GList *l = group->held.head; fresh && l;) {
    GList *next = l->next;
    Held *held = l->data;
    held->misses += fresh;
    if (held->misses >= GH_MLS_SERVICE_JUNK_AFTER_COMMITS) {
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
}

static void after_commit(GhMlsGroup *group, gboolean fresh);

/* Every relay of the subscription sent its EOSE, fully paged, and is still
 * connected (value 1), apart from URLs that could never be subscribed
 * (value 3). A relay that failed during the catch-up, or whose paging could
 * not fetch everything (value 4, nostrc-cpwf), may hold events the others
 * lack: the cursor must not pass them. */
static gboolean
all_relays_answered(GhMlsGroup *group)
{
  if (!group->relays || !group->relays[0])
    return FALSE;
  for (guint i = 0; group->relays[i]; i++) {
    gint state = GPOINTER_TO_INT(g_hash_table_lookup(group->settled, group->relays[i]));
    if (state != 1 && state != 3)
      return FALSE;
  }
  return TRUE;
}

/* One kind-445 envelope from a group relay: libmarmot's relay path (id and
 * signature first) and, for a chat message, its admission, in one
 * transaction. @retry: the held record when this is a retry (it is not held
 * again here; the caller decides). */
static EventOutcome
process_event(GhMlsGroup *group, const gchar *event_json, const gchar *url, Held *retry)
{
  GhMlsService *self = group->service;
  g_autoptr(GError) error = NULL;
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error)) {
    g_message("Groundhog could not read an encrypted group message: %s", error->message);
    return EVENT_OTHER;
  }
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  MarmotError err = marmot_process_message(self->marmot, event_json, &result);
  gboolean commit = FALSE, held = FALSE, accepted = FALSE;
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

  if (err == MARMOT_OK && result.type == MARMOT_RESULT_APPLICATION_MESSAGE) {
    g_autoptr(GError) bad = NULL;
    g_autoptr(GhMessage) message =
      gh_message_new_from_mls(self->account, group->gid_hex, result.app_msg.inner_event_json,
                              &bad);
    /* libmarmot authenticated the author; the model checks it again. */
    if (message && g_strcmp0(gh_message_get_sender(message),
                             result.app_msg.sender_pubkey_hex) != 0)
      g_clear_object(&message);
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
    /* Another kind (a reaction, a deletion) is read but not shown yet. */
    accepted = TRUE;
  } else if (err == MARMOT_OK && result.type == MARMOT_RESULT_COMMIT) {
    commit = accepted = TRUE;
  } else if (err == MARMOT_ERR_NIP44) {
    /* A later epoch's (or not for us): wait for the Commit that opens it. */
    held = TRUE;
  } else if (err != MARMOT_OK) {
    g_debug("Groundhog skipped an encrypted group event: %s", marmot_error_string(err));
  }
  marmot_message_result_free(&result);
  /* libmarmot rolled a failed operation back itself; its deliberate
   * outcomes (a deferred competing Commit) are kept. */
  if (!gh_store_commit(self->store, &error)) {
    g_message("Groundhog could not store an encrypted group event: %s", error->message);
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
    if (!retry)
      hold_event(group, envelope_id, event_json, created_at);
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
  if (commit)
    after_commit(group, retry == NULL);
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
 * ratchet past them. A relay that has sent nothing yet holds nothing up. */
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
    if (group->active)
      process_event(group, stored->json, stored->url, NULL);
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
backfill_full(GhMlsGroup *group)
{
  g_message("Groundhog stopped reading an encrypted group's history after %u events: it keeps "
            "no more at once; the group's read cursor stays where it was",
            g_queue_get_length(&group->backfill));
  GHashTableIter iter;
  gpointer url;
  g_hash_table_iter_init(&iter, group->backfilling);
  while (g_hash_table_iter_next(&iter, &url, NULL)) {
    if (group->scope)
      gh_relay_scope_end_backfill(group->scope, url);
    g_hash_table_insert(group->settled, g_strdup(url), GINT_TO_POINTER(4));
  }
  g_hash_table_remove_all(group->backfilling);
  update_history_incomplete(group);
  if (group->relays && g_hash_table_size(group->settled) >= g_strv_length(group->relays))
    group_set_read(group, GH_MLS_READ_LIVE);
  flush_backfill(group);
}

static void
keep_backfill(GhMlsGroup *group, const GhRelayUpdate *update)
{
  Stored *stored = g_new0(Stored, 1);
  stored->json = g_strdup(update->event_json);
  stored->url = g_strdup(update->url);
  stored->created_at = G_MAXINT64;
  stored->seq = group->backfill_seq++;
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

static void
on_group_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhMlsGroup *group = data;
  if (scope != group->scope)
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    /* A live event waits too while a backfill is pending: applied first it
     * would move the sender's ratchet past the stored older ones. */
    if (update->backfill)
      g_hash_table_add(group->backfilling, g_strdup(update->url));
    if (g_hash_table_size(group->backfilling) > 0 || !g_queue_is_empty(&group->backfill))
      keep_backfill(group, update);
    else
      process_event(group, update->event_json, update->url, NULL);
    break;
  case GH_RELAY_NOTICE_EOSE: {
    g_hash_table_remove(group->backfilling, update->url);
    if (g_hash_table_size(group->backfilling) == 0)
      flush_backfill(group);
    if (scope != group->scope)
      break;   /* a Commit in it re-subscribed the group */
    /* The scope reports it once the backfill has been paged (nostrc-cpwf). */
    if (update->incomplete)
      g_message("Groundhog could not fetch every older encrypted group event from a relay; "
                "the group's read cursor stays where it was");
    g_hash_table_insert(group->settled, g_strdup(update->url),
                        GINT_TO_POINTER(update->incomplete ? 4 : 1));
    update_history_incomplete(group);
    guint n = g_strv_length(group->relays);
    if (g_hash_table_size(group->settled) >= n) {
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
    if (scope != group->scope)
      break;
    g_hash_table_insert(group->settled, g_strdup(update->url), GINT_TO_POINTER(2));
    update_history_incomplete(group);
    gboolean any_live = FALSE;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, group->settled);
    while (g_hash_table_iter_next(&iter, NULL, &value))
      any_live |= GPOINTER_TO_INT(value) == 1 || GPOINTER_TO_INT(value) == 4;
    if (!any_live && g_hash_table_size(group->settled) >= g_strv_length(group->relays))
      group_set_read(group, GH_MLS_READ_DISCONNECTED);
    break;
  }
  default:
    break;
  }
}

/* One live REQ {kinds:[445], #h:[nostr group id], since, limit} on exactly
 * the group relays (§4.3 MLS routing: ephemeral AUTH only), each relay's
 * backfill paged past its result cap (nostrc-cpwf). */
static void
group_subscribe(GhMlsGroup *group)
{
  GhMlsService *self = group->service;
  group_unsubscribe(group);
  if (!running(self) || !group->active || !group->relays[0] || !group->nostr_hex[0])
    return;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { MARMOT_KIND_GROUP_MESSAGE };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_tags_append(filter, "h", group->nostr_hex, NULL);
  if (group->cursor > 0)
    nostr_filter_set_since_i64(filter, MAX(group->cursor - GH_MLS_SERVICE_CURSOR_OVERLAP, 1));
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  GhRelayScope *scope = gh_relay_scope_new(self->generation, filters, on_group_update, group);
  gh_relay_scope_set_backfill_paging(scope, GH_MLS_SERVICE_PAGE_LIMIT, GH_MLS_SERVICE_MAX_PAGES);
  g_autofree gchar *isolation = isolation_label(group);
  gh_relay_scope_set_isolation(scope, isolation);
  guint added = 0;
  for (guint i = 0; group->relays[i] && added < MAX_GROUP_RELAYS; i++) {
    g_autoptr(GError) error = NULL;
    if (!gh_relay_scope_add_url(scope, group->relays[i], &error)) {
      g_debug("Groundhog skips a group relay: %s", error->message);
      g_hash_table_insert(group->settled, g_strdup(group->relays[i]), GINT_TO_POINTER(3));
      continue;
    }
    if (!gh_auth_policy_apply_scope(self->policy, scope, GH_AUTH_PURPOSE_MLS_ROUTING,
                                    group->relays[i], &error))
      g_debug("Groundhog will not sign in to a group relay: %s", error->message);
    added++;
  }
  if (!added) {
    gh_relay_scope_unref(scope);
    group_set_read(group, GH_MLS_READ_DISCONNECTED);
    return;
  }
  group->scope = scope;
  group_set_read(group, GH_MLS_READ_SYNCING);
  gh_relay_scope_start(scope);
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

typedef enum { OP_CREATE, OP_ADD, OP_REMOVE, OP_METADATA } OpKind;

typedef struct {
  OpKind kind;
  GhMlsGroup *group;          /* ref; NULL until a created group exists */
  gchar *name;
  gchar *description;
  GStrv relays;               /* create */
  GStrv people;               /* invitees (create, add) or members (remove) */
  GPtrArray *key_packages;    /* the invitees' selected kind-30443 JSON */
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

/* @fresh: the Commit came from a relay or was our own merged, not out of
 * the held queue (it ages the queue's junk; review N2). */
static void
after_commit(GhMlsGroup *group, gboolean fresh)
{
  group_refresh(group);
  if (fresh)
    group->fresh_commits++;
  retry_held(group);
  welcomes_pump(group);
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
  if (round->state == GH_MLS_COMMIT_MERGED)
    retry_succeeded(group->service);
  after_commit(group, TRUE);
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
    /* Unanswered: republished later, byte for byte (the waiters wait). */
    group_refresh(group);
    schedule_retry(group->service);
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
  GArray *keys = data;
  return marmot_remove_members(marmot, gid, (const uint8_t (*)[32])keys->data, keys->len, out);
}

static MarmotError
produce_metadata(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  return marmot_update_group_metadata(marmot, gid, data, out);
}

/* Stages the change (T-mls) and publishes it; task completes with it. */
static void
stage_change(GTask *task, GhMlsGroup *group, GhMlsCommitProducer producer, gpointer data)
{
  GhMlsService *self = group->service;
  g_autoptr(GError) error = NULL;
  if (group->round || group->pending_commit) {
    g_task_return_new_error(task, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_BUSY,
                            "Another change of this group is still being sent");
    g_object_unref(task);
    return;
  }
  GhMlsCommitPublish *publish = gh_mls_commit_stage(self->store, self->marmot, self->storage,
                                                    &group->gid, self->account, producer, data,
                                                    &error);
  if (!publish) {
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
  if (op->kind == OP_CREATE) {
    create_group_now(task);
    return;
  }
  stage_change(task, op->group, produce_add, op->key_packages);
}

static void
lookup_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *task = data;
  Op *op = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsKeyPackage) kp = gh_mls_key_package_lookup_finish(result, &error);
  if (kp) {
    g_ptr_array_add(op->key_packages, g_strdup(kp->event_json));
  } else if (!op->error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      op->error = g_error_new(GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE,
                              "%s", error->message);
    else
      op->error = g_steal_pointer(&error);
  }
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
  op->lookups = n;
  for (guint i = 0; i < n; i++)
    gh_mls_key_package_lookup_async(self->accounts, (const gchar *const *)sources,
                                    op->people[i], self->lookup_deadline, self->cancellable,
                                    lookup_done, task);
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
  MarmotError err = marmot_create_group(self->marmot, self->account_key, NULL, 0, &config,
                                        &created);
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
  save_cursor(group, now_s(self));
  group_subscribe(group);
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

void
gh_mls_service_create_group_async(GhMlsService *self, const gchar *name,
                                  const gchar *description, const gchar *const *relays,
                                  const gchar *const *invitees, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_MLS_SERVICE(self));
  GTask *task = op_task(self, OP_CREATE, NULL, cancellable, callback, user_data,
                        gh_mls_service_create_group_async);
  Op *op = g_task_get_task_data(task);
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
  if (!name || !*name || !g_utf8_validate(name, -1, NULL) ||
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
  stage_change(task, group, produce_remove, keys);
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
  MarmotGroupConfig config = { .name = (char *)name, .description = (char *)description };
  stage_change(task, group, produce_metadata, &config);
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
  MarmotGroupConfig config = { .admin_pubkeys = keys, .admin_count = n };
  stage_change(task, group, produce_metadata, &config);
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
inner_event_new(GhMlsService *self, GhMlsGroup *group, const gchar *text, gchar **out_id)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, GH_MESSAGE_MLS_KIND);
  nostr_event_set_pubkey(event, self->account);
  nostr_event_set_created_at(event, now_s(self));
  nostr_event_set_content(event, text);
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("h", group->nostr_hex, NULL)));
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

GhMessage *
gh_mls_service_send(GhMlsService *self, GhMlsGroup *group, const gchar *text, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), NULL);
  if (!check_running(self, error))
    return NULL;
  if (!GH_IS_MLS_GROUP(group) || group->service != self || !group->active ||
      !text || !*text || !g_utf8_validate(text, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A message needs text and an active group");
    return NULL;
  }
  if (!group->relays[0]) {
    g_set_error_literal(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_RELAYS,
                        "The group has no relays");
    return NULL;
  }
  g_autofree gchar *inner_id = NULL;
  g_autofree gchar *inner = inner_event_new(self, group, text, &inner_id);
  g_autoptr(GhMessage) message = inner ? gh_message_new_from_mls(self->account, group->gid_hex,
                                                                 inner, error) : NULL;
  if (!message)
    return NULL;
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
  /* The sender ratchet step, in this transaction (libmarmot 0.8.0). */
  err = marmot_create_message(self->marmot, &group->gid, inner, &out);
  if (err != MARMOT_OK) {
    marmot_fail(self, err, "The message could not be encrypted", error);
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

static void key_package_rotate(GhMlsService *self);

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
    marmot_fail(self, err != MARMOT_OK ? err : MARMOT_ERR_WELCOME_NOT_FOUND,
                "The invitation could not be accepted", error);
    /* libmarmot keeps its deliberate outcomes (a retired duplicate). */
    if (!gh_store_commit(self->store, NULL))
      gh_store_rollback(self->store);
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
  group_subscribe(group);
  /* MIP-00: the KeyPackage the Welcome used is spent; publish a new one. */
  key_package_rotate(self);
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
  return TRUE;
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
  MarmotError err = marmot_leave_group(self->marmot, &group->gid);
  if (err != MARMOT_OK)
    return marmot_fail(self, err, "The group could not be left", error);
  group_unsubscribe(group);
  group_refresh(group);
  return TRUE;
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

static void
key_package_set_state(GhMlsService *self, GhMlsKeyPackageState state)
{
  if (self->key_package == state)
    return;
  self->key_package = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_KEY_PACKAGE_STATE]);
}

/* The account's own kind-10002 write and kind-10050 inbox relays (§4.3 own
 * list publish), sorted, unique, at most 16. */
static GStrv
own_relays(GhMlsService *self)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  if (self->account_relays &&
      gh_account_relays_get_generation(self->account_relays) == self->generation) {
    const gchar *const *lists[] = { gh_account_relays_get_write_relays(self->account_relays),
                                    gh_account_relays_get_inbox_relays(self->account_relays) };
    for (guint l = 0; l < G_N_ELEMENTS(lists); l++)
      for (guint i = 0; lists[l] && lists[l][i] && urls->len < MAX_GROUP_RELAYS; i++)
        if (gh_relay_url_validate(lists[l][i], NULL) &&
            !g_ptr_array_find_with_equal_func(urls, lists[l][i], g_str_equal, NULL))
          g_ptr_array_add(urls, g_strdup(lists[l][i]));
  }
  g_ptr_array_sort(urls, compare_strings);
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_steal(urls, NULL);
}

typedef struct {
  GWeakRef service;
  guint64 run;
  GStrv urls;
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
  GhMlsService *self = data;
  if (result->outcome != GH_RELAY_PUBLISH_ACCEPTED ||
      self->key_package == GH_MLS_KEY_PACKAGE_PUBLISHED)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR, "", now_s(self), &error))
    g_message("Groundhog could not record the KeyPackage publication: %s", error->message);
  g_free(self->key_package_id);
  self->key_package_id = g_strdup(gh_relay_publish_get_event_id(publish));
  self->key_package_rotate = FALSE;
  key_package_set_state(self, GH_MLS_KEY_PACKAGE_PUBLISHED);
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
  GhMlsService *self = data;
  if (self->key_package_publish == publish)
    self->key_package_publish = NULL;
  self->key_package_busy = FALSE;
  if (!summary->any_accepted) {
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_FAILED);
    schedule_retry(self);
  }
  g_idle_add(key_package_publish_free_idle, publish);
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
  /* The signer must have signed exactly a KeyPackage of this account. */
  NostrEvent *event = signed_json ? nostr_event_new() : NULL;
  gboolean ok = event && nostr_event_deserialize_compact(event, signed_json, NULL) == 1 &&
                nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == MARMOT_KIND_KEY_PACKAGE &&
                g_strcmp0(nostr_event_get_pubkey(event), self->account) == 0;
  if (event)
    nostr_event_free(event);
  GhRelayPublish *publish = ok ? gh_relay_publish_new(self->generation, signed_json,
                                                      key_package_update, key_package_done,
                                                      self, &error)
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
    self->key_package_busy = FALSE;
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_FAILED);
    key_package_job_free(job);
    return;
  }
  self->key_package_publish = publish;
  if (!gh_relay_publish_start(publish, &error)) {
    self->key_package_publish = NULL;
    gh_relay_publish_unref(publish);
    self->key_package_busy = FALSE;
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_FAILED);
  }
  key_package_job_free(job);
}

/* Publishes a new KeyPackage when none was published within the lifetime,
 * or the last one was spent. */
static void
key_package_maybe_publish(GhMlsService *self)
{
  if (!running(self) || self->key_package_busy || !identity_ready(self))
    return;
  g_auto(GStrv) urls = own_relays(self);
  if (!urls[0]) {
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_NO_RELAYS);
    return;
  }
  gint64 last = 0;
  g_autoptr(GError) error = NULL;
  if (!gh_store_get_cursor(self->store, KEY_PACKAGE_CURSOR, "", &last, &error)) {
    g_message("Groundhog could not read the KeyPackage state: %s", error->message);
    return;
  }
  if (!self->key_package_rotate && last > 0 && now_s(self) - last < self->key_package_lifetime) {
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_PUBLISHED);
    return;
  }
  /* The private init key is stored (encrypted) before anything is signed. */
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  drop_stale_error(self);
  if (!gh_store_begin(self->store, &error))
    return;
  MarmotError err = marmot_create_key_package_unsigned(self->marmot, self->account_key,
                                                       (const char **)urls, g_strv_length(urls),
                                                       &made);
  if (err != MARMOT_OK || !gh_store_commit(self->store, &error)) {
    if (err != MARMOT_OK) {
      marmot_fail(self, err, "Making a KeyPackage", &error);
      gh_store_rollback(self->store);
    }
    g_message("Groundhog could not make a KeyPackage: %s", error->message);
    marmot_key_package_result_free(&made);
    key_package_set_state(self, GH_MLS_KEY_PACKAGE_FAILED);
    return;
  }
  self->key_package_busy = TRUE;
  key_package_set_state(self, GH_MLS_KEY_PACKAGE_PUBLISHING);
  KeyPackageJob *job = g_new0(KeyPackageJob, 1);
  g_weak_ref_init(&job->service, self);
  job->run = self->run;
  job->urls = g_steal_pointer(&urls);
  gh_account_controller_sign_with_cancellable_async(self->accounts, made.event_json,
                                                    self->cancellable, key_package_signed,
                                                    job);
  marmot_key_package_result_free(&made);
}

/* MIP-00: a Welcome consumed the published KeyPackage (or the user asked):
 * publish a new one, now or at the next start. */
static void
key_package_rotate(GhMlsService *self)
{
  self->key_package_rotate = TRUE;
  g_autoptr(GError) error = NULL;
  if (!gh_store_set_cursor(self->store, KEY_PACKAGE_CURSOR, "", 0, &error))
    g_message("Groundhog could not record a spent KeyPackage: %s", error->message);
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
  return self->key_package_id;
}

gboolean
gh_mls_service_rotate_key_package(GhMlsService *self, GError **error)
{
  g_return_val_if_fail(GH_IS_MLS_SERVICE(self), FALSE);
  if (!check_running(self, error))
    return FALSE;
  key_package_rotate(self);
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
  }
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
  if (self->key_package_publish) {
    gh_relay_publish_cancel(self->key_package_publish);
    g_clear_pointer(&self->key_package_publish, gh_relay_publish_unref);
  }
  self->key_package_busy = FALSE;
  g_hash_table_remove_all(self->deliveries);
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
    /* Held events stay (review M1): a network flap must not lose them. */
    complete_waiters(group, cancelled);
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
  Marmot *marmot = marmot_new(storage);
  if (!marmot) {
    marmot_storage_free(storage);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "libmarmot did not start");
    return NULL;
  }
  g_autoptr(GhMlsService) self = g_object_new(GH_TYPE_MLS_SERVICE, NULL);
  self->store = config->store;
  self->storage = storage;
  self->marmot = marmot;
  self->account = g_strdup(account);
  nostr_hex2bin(self->account_key, account, sizeof self->account_key);
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->accounts = g_object_ref(config->accounts);
  self->policy = g_object_ref(gh_auth_policy_get_for_accounts(config->accounts));
  self->max_backfill_events = GH_MLS_SERVICE_MAX_BACKFILL_EVENTS;
  self->max_backfill_bytes = GH_MLS_SERVICE_MAX_BACKFILL_BYTES;
  self->conversations = g_object_ref(config->conversations);
  self->account_relays = config->account_relays ? g_object_ref(config->account_relays) : NULL;
  self->inboxes = config->inboxes ? g_object_ref(config->inboxes) : NULL;
  self->settings = config->settings ? g_object_ref(config->settings) : NULL;
  self->may_look_up = config->may_look_up;
  self->consent_data = config->consent_data;
  self->network = g_object_ref(config->network ? config->network
                                               : g_network_monitor_get_default());
  self->publish_deadline = config->publish_deadline;
  self->lookup_deadline = config->lookup_deadline;
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
  if (self->marmot)
    marmot_free(self->marmot);   /* and its storage */
  g_clear_object(&self->rooms);
  g_clear_object(&self->inbox);
  g_clear_object(&self->identity_cancellable);
  g_clear_object(&self->accounts);
  g_clear_object(&self->policy);
  g_clear_object(&self->conversations);
  g_clear_object(&self->account_relays);
  g_clear_object(&self->inboxes);
  g_clear_object(&self->settings);
  g_clear_object(&self->network);
  if (self->clock)
    gh_clock_unref(self->clock);
  g_free(self->account);
  g_free(self->key_package_id);
  G_OBJECT_CLASS(gh_mls_service_parent_class)->finalize(object);
}

static void
gh_mls_service_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMlsService *self = GH_MLS_SERVICE(object);
  switch (id) {
  case PROP_KEY_PACKAGE_STATE: g_value_set_enum(value, self->key_package); break;
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
}
