#include "gh-nip29-service.h"

#include "gh-auth-policy.h"
#include "gh-conversation-private.h"
#include "gh-identity.h"
#include "gh-nip11.h"
#include "gh-nip29-template.h"
#include "gh-store-nip29.h"
#include "gh-reaction.h"
#include "gh-reaction-store.h"

#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <string.h>

#define SNAPSHOTS        4       /* kinds 39000-39003 */
#define RECORD_VERSION   1
#define KEY_RETRY_S      600     /* a failed NIP-11 fetch is tried again after this */
#define MAX_SNAPSHOT     (2 * 1024 * 1024)  /* a larger snapshot is used, not kept */

typedef enum {
  HISTORY_INCOMPLETE,
  HISTORY_COMPLETE,
  HISTORY_PARTIAL
} HistoryState;

/* ---- Enums ----------------------------------------------------------------- */

GType
gh_nip29_join_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_NIP29_JOIN_NONE, "GH_NIP29_JOIN_NONE", "none" },
      { GH_NIP29_JOIN_REQUESTING, "GH_NIP29_JOIN_REQUESTING", "requesting" },
      { GH_NIP29_JOIN_PENDING, "GH_NIP29_JOIN_PENDING", "pending" },
      { GH_NIP29_JOIN_MEMBER, "GH_NIP29_JOIN_MEMBER", "member" },
      { GH_NIP29_JOIN_DENIED, "GH_NIP29_JOIN_DENIED", "denied" },
      { GH_NIP29_JOIN_CLOSED, "GH_NIP29_JOIN_CLOSED", "closed" },
      { GH_NIP29_JOIN_NOT_SENT, "GH_NIP29_JOIN_NOT_SENT", "not-sent" },
      { GH_NIP29_JOIN_LEAVING, "GH_NIP29_JOIN_LEAVING", "leaving" },
      { GH_NIP29_JOIN_LEFT, "GH_NIP29_JOIN_LEFT", "left" },
      { GH_NIP29_JOIN_REMOVED, "GH_NIP29_JOIN_REMOVED", "removed" },
      { GH_NIP29_JOIN_CREATING, "GH_NIP29_JOIN_CREATING", "creating" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(g_intern_static_string("GhNip29JoinState"),
                                                    values));
  }
  return type;
}

GType
gh_nip29_read_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_NIP29_READ_IDLE, "GH_NIP29_READ_IDLE", "idle" },
      { GH_NIP29_READ_SYNCING, "GH_NIP29_READ_SYNCING", "syncing" },
      { GH_NIP29_READ_LIVE, "GH_NIP29_READ_LIVE", "live" },
      { GH_NIP29_READ_AUTH_REQUIRED, "GH_NIP29_READ_AUTH_REQUIRED", "auth-required" },
      { GH_NIP29_READ_REFUSED, "GH_NIP29_READ_REFUSED", "refused" },
      { GH_NIP29_READ_DISCONNECTED, "GH_NIP29_READ_DISCONNECTED", "disconnected" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(g_intern_static_string("GhNip29ReadState"),
                                                    values));
  }
  return type;
}

GType
gh_nip29_relay_key_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_NIP29_RELAY_KEY_UNKNOWN, "GH_NIP29_RELAY_KEY_UNKNOWN", "unknown" },
      { GH_NIP29_RELAY_KEY_PINNED, "GH_NIP29_RELAY_KEY_PINNED", "pinned" },
      { GH_NIP29_RELAY_KEY_UNAVAILABLE, "GH_NIP29_RELAY_KEY_UNAVAILABLE", "unavailable" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhNip29RelayKeyState"), values));
  }
  return type;
}

/* ---- Types ----------------------------------------------------------------- */

typedef struct _Relay Relay;

struct _GhNip29Room {
  GObject parent_instance;
  GhNip29Service *service;     /* borrowed; NULL once the service is disposed */
  GhNip29GroupKey *key;
  gchar *room_id;
  gint64 conversation_id;
  gchar *relay_pubkey;         /* pinned relay key; "" while unknown */
  GhNip29Group *group;         /* relay-verified state; NULL while the key is unknown */
  gchar *snapshots[SNAPSHOTS]; /* the admitted events' JSON, kept for restart */
  gchar *held[SNAPSHOTS];      /* newest received while the key was unknown */
  gint64 held_at[SNAPSHOTS];
  GhNip29Timeline *timeline;
  gint64 cursor;               /* newest message created_at stored with nothing missing
                                * before it (bounded by now): the next REQ's since */
  gint64 sync_cursor;          /* newest stored by this REQ's backfill; committed at EOSE */
  gint64 reaction_cursor;      /* independent kind-5/7 backfill checkpoint */
  gint64 reaction_sync_cursor;
  gint64 history_oldest;       /* inclusive until of the next historical REQ */
  HistoryState history_state;
  GhRelayScope *history_scope; /* one per-group, one-page historical REQ */
  GPtrArray *history_events;   /* signed JSON buffered until EOSE */
  guint history_pages;         /* this Earlier Messages request's budget */
  guint history_idle;
  gint64 history_requested_until;
  gint64 history_page_oldest;
  gint64 history_page_newest;
  guint history_page_count;
  gboolean reaction_sync_failed;
  gboolean sync_failed;        /* an admission of this REQ failed, or its backfill came
                                * back incomplete: the cursor stays */
  gboolean backfilled;         /* the group's first EOSE ever came */
  gint64 joined_at;            /* local time of the last join request: older is history */
  gint64 left_at;              /* local time of the last leave request */
  GhNip29JoinState join;
  gint64 evidence_at;          /* relay time of the latest 9000/9001/39002 applied */
  gboolean join_with_code;
  gboolean awaiting_state;     /* refused before its REQ's answer ended: that REQ still
                                * brings the group state (a closed group?), so the
                                * room reads its 39000-39003 until the EOSE */
  gint64 join_op;              /* outbox ids; 0 = none */
  gint64 leave_op;
  gint64 create_op;            /* the create-group request (G20b); 0 = none */
  gchar *create_meta;          /* its metadata, sent (9002) once the relay created it */
  guint discard_idle;          /* a group the relay did not create leaves (G20b) */
  gchar *detail;
  GhNip29ReadState read;
  GhNip29RelayKeyState key_state;
  gchar *name;
  gboolean closed;
  gboolean private_;
  gboolean restricted;
  GhNip29MemberList members;
  guint save_idle;
};

struct _Relay {
  GhNip29Service *service;
  gchar *url;
  GhRelayScope *scope;         /* the live REQ, while subscribed */
  gchar *key;                  /* the NIP-11 key fetched or pinned; NULL unknown */
  GhNip29RelayKeyState key_state;
  GCancellable *key_fetch;
  gboolean key_refetched;      /* this session re-fetched after a foreign signature */
  guint key_retry;             /* GhClock timeout */
};

struct _GhNip29Service {
  GObject parent_instance;
  GhStore *store;
  GhClock *clock;
  gchar *account;
  GhAccountController *accounts;
  GhAuthPolicy *policy;
  GhConversationStore *conversations;
  GhReactionStore *reactions;          /* nullable; W26 slice B */
  GhStoreNip29 *rooms_store;
  GhNip29Outbox *outbox;
  gulong op_handler;
  GNetworkMonitor *network;
  GhRelayTransport scope_transport;
  GhRelayAuthTransport scope_auth;
  gboolean custom_scope;
  gboolean has_scope_auth;
  gpointer scope_data;
  GhNetHttp *http;             /* the NIP-11 fetches (gh-nip11.h) */
  guint64 generation;          /* the account generation it runs in; 0: paused */
  gboolean online;
  gulong accounts_handler;
  gulong network_handler;
  GPtrArray *rooms;            /* GhNip29Room, oldest first: the GListModel */
  GHashTable *by_key;          /* GhNip29GroupKey (the room's) -> room */
  GHashTable *by_room_id;      /* room id -> room */
  GHashTable *relays;          /* url -> Relay */
  GHashTable *role_policies;   /* url -> GhNip29RolePolicy */
};

enum {
  ROOM_PROP_0,
  ROOM_PROP_RELAY_URL,
  ROOM_PROP_GROUP_ID,
  ROOM_PROP_ROOM_ID,
  ROOM_PROP_NAME,
  ROOM_PROP_JOIN_STATE,
  ROOM_PROP_READ_STATE,
  ROOM_PROP_RELAY_KEY_STATE,
  ROOM_PROP_MEMBERS_STATE,
  ROOM_PROP_IS_CLOSED,
  ROOM_PROP_IS_PRIVATE,
  ROOM_PROP_IS_RESTRICTED,
  ROOM_PROP_DETAIL,
  ROOM_N_PROPS
};
static GParamSpec *room_props[ROOM_N_PROPS];

enum { ROOM_SIGNAL_GROUP_CHANGED, ROOM_N_SIGNALS };
static guint room_signals[ROOM_N_SIGNALS];

static void gh_nip29_service_list_model_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE(GhNip29Room, gh_nip29_room, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE_WITH_CODE(GhNip29Service, gh_nip29_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gh_nip29_service_list_model_init))

static void relay_resubscribe(Relay *relay);
static void relay_ensure_key(Relay *relay, gboolean force);
static void room_save(GhNip29Room *room);
static GhNip29Op *room_enqueue(GhNip29Room *room, const gchar *unsigned_json, GError **error);
static void room_context(GhNip29Room *room, GhNip29TemplateContext *context);

/* ---- Small helpers ------------------------------------------------------------ */

static gint64
now_unix(GhNip29Service *self)
{
  return gh_clock_get_unix(self->clock);
}

static gboolean
running(GhNip29Service *self)
{
  return self->generation != 0 && self->online;
}

/* Whether the account is (or is about to be) in the group: it reads it. */
static gboolean
join_subscribed(GhNip29JoinState join)
{
  switch (join) {
  case GH_NIP29_JOIN_REQUESTING:
  case GH_NIP29_JOIN_PENDING:
  case GH_NIP29_JOIN_MEMBER:
  case GH_NIP29_JOIN_LEAVING:
  case GH_NIP29_JOIN_CREATING: /* its state arrives once the relay created it */
    return TRUE;
  default:
    return FALSE;
  }
}

/* Whether the relay's REQ carries the room: all of a group the account is
 * (about to be) in; only the state of one whose refusal waits for it. */
static gboolean
room_reads(GhNip29Room *room)
{
  return join_subscribed(room->join) || room->awaiting_state;
}

static const gchar *
room_relay(GhNip29Room *room)
{
  return gh_nip29_group_key_get_relay_url(room->key);
}

static const gchar *
room_group_id(GhNip29Room *room)
{
  return gh_nip29_group_key_get_group_id(room->key);
}

static const NostrTag *
first_tag(NostrEvent *event, const gchar *name)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), name) == 0)
      return tag;
  }
  return NULL;
}

static const gchar *
first_tag_value(NostrEvent *event, const gchar *name)
{
  const NostrTag *tag = first_tag(event, name);
  return tag ? nostr_tag_get(tag, 1) : NULL;
}

/* ---- Room properties ------------------------------------------------------------ */

static void
room_notify(GhNip29Room *room, guint prop)
{
  g_object_notify_by_pspec(G_OBJECT(room), room_props[prop]);
}

static void
room_set_detail(GhNip29Room *room, const gchar *detail)
{
  if (g_strcmp0(room->detail, detail) == 0)
    return;
  g_free(room->detail);
  room->detail = g_strdup(detail);
  room_notify(room, ROOM_PROP_DETAIL);
}

static void history_close(GhNip29Room *room);
static void history_refresh_availability(GhNip29Room *room);

/* A group the account is in (again) is read in full: its REQ is rebuilt.
 * One it no longer is in stops being read, unless its refusal still awaits
 * the group state; then the REQ it is in stays (its answer is on the way)
 * until on_scope_update() drops the room at that REQ's EOSE. */
static void
room_set_join(GhNip29Room *room, GhNip29JoinState join, gint64 at, const gchar *detail)
{
  gboolean was_subscribed = join_subscribed(room->join);
  gboolean was_reading = room_reads(room);
  if (at > room->evidence_at)
    room->evidence_at = at;
  room_set_detail(room, detail);
  if (room->join != join) {
    room->join = join;
    room_notify(room, ROOM_PROP_JOIN_STATE);
  }
  if (join_subscribed(join))
    room->awaiting_state = FALSE;
  else
    history_close(room);
  room_save(room);
  history_refresh_availability(room);
  GhNip29Service *self = room->service;
  if (self && (was_reading != room_reads(room) || (!was_subscribed && join_subscribed(join)))) {
    Relay *relay = g_hash_table_lookup(self->relays, room_relay(room)); /* made with the room */
    if (relay)
      relay_resubscribe(relay);
  }
}

static void
room_set_read(GhNip29Room *room, GhNip29ReadState read, const gchar *detail)
{
  if (detail)
    room_set_detail(room, detail);
  if (room->read == read)
    return;
  room->read = read;
  room_notify(room, ROOM_PROP_READ_STATE);
}

static void
room_set_key_state(GhNip29Room *room, GhNip29RelayKeyState state)
{
  if (room->key_state == state)
    return;
  room->key_state = state;
  room_notify(room, ROOM_PROP_RELAY_KEY_STATE);
}

/* Name, flags and member-list state from the admitted snapshots; the
 * conversation takes the relay-signed name. */
static void
room_sync_metadata(GhNip29Room *room)
{
  g_autoptr(GhNip29Metadata) metadata = room->group ? gh_nip29_group_dup_metadata(room->group)
                                                    : NULL;
  const gchar *name = metadata && metadata->name && *metadata->name ? metadata->name : NULL;
  g_object_freeze_notify(G_OBJECT(room));
  if (g_strcmp0(room->name, name) != 0) {
    g_free(room->name);
    room->name = g_strdup(name);
    room_notify(room, ROOM_PROP_NAME);
  }
  gboolean closed = metadata && metadata->is_closed;
  gboolean private_ = metadata && metadata->is_private;
  gboolean restricted = metadata && metadata->is_restricted;
  if (room->closed != closed) {
    room->closed = closed;
    room_notify(room, ROOM_PROP_IS_CLOSED);
  }
  if (room->private_ != private_) {
    room->private_ = private_;
    room_notify(room, ROOM_PROP_IS_PRIVATE);
  }
  if (room->restricted != restricted) {
    room->restricted = restricted;
    room_notify(room, ROOM_PROP_IS_RESTRICTED);
  }
  GhNip29MemberList members = room->group ? gh_nip29_group_dup_members(room->group, NULL)
                                          : GH_NIP29_MEMBERS_UNAVAILABLE;
  if (room->members != members) {
    room->members = members;
    room_notify(room, ROOM_PROP_MEMBERS_STATE);
  }
  g_object_thaw_notify(G_OBJECT(room));
  /* A refusal that came before the metadata: a closed group needs a code. */
  if (room->join == GH_NIP29_JOIN_DENIED && closed && !room->join_with_code)
    room_set_join(room, GH_NIP29_JOIN_CLOSED, 0, room->detail);
  else if (room->join == GH_NIP29_JOIN_CLOSED && metadata && !closed)
    room_set_join(room, GH_NIP29_JOIN_DENIED, 0, room->detail);
  GhNip29Service *self = room->service;
  if (self && self->conversations && name) {
    GhConversation *conversation = gh_conversation_store_lookup(self->conversations,
                                                                room->room_id);
    if (conversation)
      gh_conversation_set_name(conversation, name);
  }
}

/* ---- Persistence ------------------------------------------------------------------ */

static gchar *
room_record(GhNip29Room *room)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "v");
  json_builder_add_int_value(builder, RECORD_VERSION);
  json_builder_set_member_name(builder, "join");
  GEnumClass *joins = g_type_class_ref(GH_TYPE_NIP29_JOIN_STATE);
  json_builder_add_string_value(builder, g_enum_get_value(joins, room->join)->value_nick);
  g_type_class_unref(joins);
  json_builder_set_member_name(builder, "evidence_at");
  json_builder_add_int_value(builder, room->evidence_at);
  json_builder_set_member_name(builder, "left_at");
  json_builder_add_int_value(builder, room->left_at);
  json_builder_set_member_name(builder, "join_code");
  json_builder_add_boolean_value(builder, room->join_with_code);
  json_builder_set_member_name(builder, "join_op");
  json_builder_add_int_value(builder, room->join_op);
  json_builder_set_member_name(builder, "leave_op");
  json_builder_add_int_value(builder, room->leave_op);
  if (room->create_op) {
    json_builder_set_member_name(builder, "create_op");
    json_builder_add_int_value(builder, room->create_op);
  }
  if (room->create_meta) {
    json_builder_set_member_name(builder, "create_meta");
    json_builder_add_string_value(builder, room->create_meta);
  }
  if (room->detail) {
    json_builder_set_member_name(builder, "detail");
    json_builder_add_string_value(builder, room->detail);
  }
  json_builder_set_member_name(builder, "cursor");
  json_builder_add_int_value(builder, room->cursor);
  json_builder_set_member_name(builder, "reaction_cursor");
  json_builder_add_int_value(builder, room->reaction_cursor);
  json_builder_set_member_name(builder, "oldest_until");
  json_builder_add_int_value(builder, room->history_oldest);
  json_builder_set_member_name(builder, "history_state");
  json_builder_add_string_value(builder, room->history_state == HISTORY_COMPLETE ? "complete"
                                       : room->history_state == HISTORY_PARTIAL ? "partial"
                                                                                : "incomplete");
  json_builder_set_member_name(builder, "backfilled");
  json_builder_add_boolean_value(builder, room->backfilled);
  json_builder_set_member_name(builder, "joined_at");
  json_builder_add_int_value(builder, room->joined_at);
  json_builder_set_member_name(builder, "snapshots");
  json_builder_begin_array(builder);
  for (guint i = 0; i < SNAPSHOTS; i++) {
    if (room->snapshots[i] && strlen(room->snapshots[i]) <= MAX_SNAPSHOT)
      json_builder_add_string_value(builder, room->snapshots[i]);
  }
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "timeline");
  json_builder_begin_array(builder);
  for (guint i = 0; i < gh_nip29_timeline_get_length(room->timeline); i++) {
    const GhNip29TimelineEntry *entry = gh_nip29_timeline_get_entry(room->timeline, i);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, entry->event_id);
    json_builder_add_string_value(builder, entry->pubkey);
    json_builder_add_int_value(builder, entry->created_at);
    json_builder_end_array(builder);
  }
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  return json_to_string(root, FALSE);
}

static gboolean
room_save_now(gpointer data)
{
  GhNip29Room *room = data;
  room->save_idle = 0;
  GhNip29Service *self = room->service;
  if (!self || !self->store)
    return G_SOURCE_REMOVE;
  g_autofree gchar *record = room_record(room);
  g_autoptr(GError) error = NULL;
  gint64 conversation_id = 0;
  if (!gh_store_nip29_save_group(self->store, room_relay(room), room_group_id(room),
                                 room->relay_pubkey, record, room->name, &conversation_id,
                                 &error))
    g_warning("Groundhog could not save a group: %s", error->message);
  else
    room->conversation_id = conversation_id;
  return G_SOURCE_REMOVE;
}

/* Coalesces the writes of a burst (a backfill) into one, after it. */
static void
room_save(GhNip29Room *room)
{
  if (room->save_idle || !room->service)
    return;
  room->save_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, room_save_now, g_object_ref(room),
                                    g_object_unref);
}

static void
room_save_flush(GhNip29Room *room)
{
  if (!room->save_idle)
    return;
  g_source_remove(room->save_idle); /* drops the source's reference, not the room */
  room->save_idle = 0;
  room_save_now(room);
}

static void
history_refresh_availability(GhNip29Room *room)
{
  if (!room->service)
    return;
  GhConversation *conversation =
    gh_conversation_store_lookup(room->service->conversations, room->room_id);
  if (conversation)
  {
    gh_conversation_set_remote_older(conversation,
      join_subscribed(room->join) && room->history_state == HISTORY_INCOMPLETE &&
      !room->history_scope && !room->history_idle);
    gh_conversation_set_history_partial(conversation,
      join_subscribed(room->join) && room->history_state == HISTORY_PARTIAL);
  }
}

static void
history_close(GhNip29Room *room)
{
  if (room->history_idle)
    g_clear_handle_id(&room->history_idle, g_source_remove);
  if (room->history_scope) {
    gh_relay_scope_cancel(room->history_scope);
    g_clear_pointer(&room->history_scope, gh_relay_scope_unref);
  }
  g_clear_pointer(&room->history_events, g_ptr_array_unref);
}

/* ---- Snapshots -------------------------------------------------------------------- */

static void room_membership_from_members(GhNip29Room *room, gint64 at);

/* Replaces the relay key: a new key is a new group state (qp24.12.2). */
static void
room_set_relay_key(GhNip29Room *room, const gchar *key)
{
  if (!key || g_strcmp0(room->relay_pubkey, key) == 0) {
    if (room->group)
      room_set_key_state(room, GH_NIP29_RELAY_KEY_PINNED);
    return;
  }
  g_autoptr(GError) error = NULL;
  GhNip29Group *group = gh_nip29_group_new(room->key, key, &error);
  if (!group) {
    g_warning("Groundhog ignored a malformed relay key: %s", error->message);
    return;
  }
  g_clear_pointer(&room->group, gh_nip29_group_free);
  room->group = group;
  g_free(room->relay_pubkey);
  room->relay_pubkey = g_strdup(key);
  for (guint i = 0; i < SNAPSHOTS; i++)
    g_clear_pointer(&room->snapshots[i], g_free);
  room_set_key_state(room, GH_NIP29_RELAY_KEY_PINNED);
  room_sync_metadata(room);
  room_save(room);
  g_signal_emit(room, room_signals[ROOM_SIGNAL_GROUP_CHANGED], 0);
}

typedef enum { ADMIT_NO_KEY, ADMIT_DONE, ADMIT_FOREIGN } AdmitOutcome;

/* One relay-signed 39000-39003 for room (already validated as signed JSON). */
static AdmitOutcome
room_admit_snapshot(GhNip29Room *room, NostrEvent *event, const gchar *json)
{
  gint slot = nostr_event_get_kind(event) - NOSTR_KIND_SIMPLE_GROUP_METADATA;
  if (slot < 0 || slot >= SNAPSHOTS)
    return ADMIT_DONE;
  gint64 created_at = nostr_event_get_created_at(event);
  if (!room->group) {
    /* Held back until the relay's key says who may sign group state. */
    if (!room->held[slot] || created_at >= room->held_at[slot]) {
      g_free(room->held[slot]);
      room->held[slot] = g_strdup(json);
      room->held_at[slot] = created_at;
    }
    return ADMIT_NO_KEY;
  }
  GhNip29Admission admission = gh_nip29_group_admit_at(room->group, event,
                                                       now_unix(room->service));
  switch (admission) {
  case GH_NIP29_ADMISSION_ACCEPTED:
    g_free(room->snapshots[slot]);
    room->snapshots[slot] = g_strdup(json);
    room_sync_metadata(room);
    if (slot == NOSTR_KIND_SIMPLE_GROUP_MEMBERS - NOSTR_KIND_SIMPLE_GROUP_METADATA)
      room_membership_from_members(room, created_at);
    room_save(room);
    g_signal_emit(room, room_signals[ROOM_SIGNAL_GROUP_CHANGED], 0);
    return ADMIT_DONE;
  case GH_NIP29_ADMISSION_WRONG_AUTHOR:
    /* Perhaps the relay's key rotated: held until the key is fetched again. */
    g_free(room->held[slot]);
    room->held[slot] = g_strdup(json);
    room->held_at[slot] = created_at;
    return ADMIT_FOREIGN;
  default:
    g_debug("Groundhog ignored a group snapshot: %s", gh_nip29_admission_to_string(admission));
    return ADMIT_DONE;
  }
}

static void
room_admit_held(GhNip29Room *room)
{
  for (guint i = 0; i < SNAPSHOTS && room->group; i++) {
    g_autofree gchar *json = g_steal_pointer(&room->held[i]);
    room->held_at[i] = 0;
    if (!json)
      continue;
    NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        room_admit_snapshot(room, event, json) == ADMIT_FOREIGN) {
      /* Signed by another key than the one just fetched: dropped. */
      g_clear_pointer(&room->held[i], g_free);
    }
    nostr_event_free(event);
  }
}

/* ---- Membership evidence ------------------------------------------------------------- */

/* Relay-dated evidence (9000, 9001, a 39002 listing the account) counts when
 * it is newer than the evidence already applied ("the latest of either"
 * decides) and not older than the account's current request, measured on
 * the local clock with the same skew allowance as snapshots: a relay clock
 * that runs somewhat behind still admits the account, while the 9000 of an
 * earlier membership does not. */
static gboolean
evidence_current(GhNip29Room *room, gint64 at)
{
  gint64 since = MAX(room->joined_at, room->left_at) - GH_NIP29_MAX_FUTURE_SKEW_SECONDS;
  return at > room->evidence_at && at >= since;
}

/* A 39002 that lists the account admits it; one that does not list it says
 * nothing (it may be partial). */
static void
room_membership_from_members(GhNip29Room *room, gint64 at)
{
  GhNip29Service *self = room->service;
  if (!self || gh_nip29_group_lookup_member(room->group, self->account) !=
                 GH_NIP29_MEMBERSHIP_LISTED)
    return;
  switch (room->join) {
  case GH_NIP29_JOIN_REQUESTING:
  case GH_NIP29_JOIN_PENDING:
  case GH_NIP29_JOIN_NOT_SENT:
  case GH_NIP29_JOIN_DENIED:
  case GH_NIP29_JOIN_CLOSED:
    if (evidence_current(room, at))
      room_set_join(room, GH_NIP29_JOIN_MEMBER, at, NULL);
    break;
  default:
    break;
  }
}

/* Whether author may speak for the group: its relay key, or an admin listed
 * in the relay-signed 39001. */
static gboolean
room_trusts(GhNip29Room *room, const gchar *author)
{
  if (!room->group)
    return FALSE;
  if (g_strcmp0(author, room->relay_pubkey) == 0)
    return TRUE;
  g_auto(GStrv) roles = gh_nip29_group_dup_admin_roles(room->group, author);
  return roles != NULL;
}

/* A 9000/9001 naming the account, signed by the relay or a listed admin. */
static void
room_membership_event(GhNip29Room *room, NostrEvent *event)
{
  GhNip29Service *self = room->service;
  if (!room_trusts(room, nostr_event_get_pubkey(event)) ||
      g_strcmp0(first_tag_value(event, "p"), self->account) != 0)
    return;
  gint64 at = nostr_event_get_created_at(event);
  if (!evidence_current(room, at))
    return; /* older than what the state already rests on */
  if (nostr_event_get_kind(event) == NOSTR_KIND_SIMPLE_GROUP_ADD_USER) {
    if (room->join != GH_NIP29_JOIN_LEAVING)
      room_set_join(room, GH_NIP29_JOIN_MEMBER, at, NULL);
  } else if (room->join == GH_NIP29_JOIN_LEAVING || room->join == GH_NIP29_JOIN_LEFT) {
    room_set_join(room, GH_NIP29_JOIN_LEFT, at, NULL);
  } else if (join_subscribed(room->join)) {
    room_set_join(room, GH_NIP29_JOIN_REMOVED, at, nostr_event_get_content(event));
  }
}

/* An event of the group was deleted (a 9005 the relay accepted): it leaves
 * the timeline ring (the relay would refuse a `previous` citing it), the
 * room and the stored history. */
static void
room_forget_event(GhNip29Room *room, const gchar *event_id)
{
  GhNip29Service *self = room->service;
  if (!gh_nip29_is_hex64(event_id))
    return;
  if (gh_nip29_timeline_remove(room->timeline, event_id))
    room_save(room);
  gh_conversation_store_remove_message(self->conversations, event_id);
  g_autoptr(GError) error = NULL;
  if (self->store && !gh_store_nip29_delete_message(self->store, room->room_id, event_id, &error))
    g_warning("Groundhog could not delete a removed group message: %s", error->message);
}

static void
room_deletion_event(GhNip29Room *room, NostrEvent *event)
{
  if (!room_trusts(room, nostr_event_get_pubkey(event)))
    return;
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "e") == 0)
      room_forget_event(room, nostr_tag_get(tag, 1));
  }
}

/* ---- Relay subscriptions ---------------------------------------------------------------- */

static void
relay_free(gpointer data)
{
  Relay *relay = data;
  if (relay->scope) {
    gh_relay_scope_cancel(relay->scope);
    g_clear_pointer(&relay->scope, gh_relay_scope_unref);
  }
  if (relay->key_fetch) {
    g_cancellable_cancel(relay->key_fetch);
    g_clear_object(&relay->key_fetch);
  }
  if (relay->key_retry && relay->service)
    gh_clock_source_remove(relay->service->clock, relay->key_retry);
  g_free(relay->url);
  g_free(relay->key);
  g_free(relay);
}

static Relay *
relay_get(GhNip29Service *self, const gchar *url)
{
  Relay *relay = g_hash_table_lookup(self->relays, url);
  if (relay)
    return relay;
  relay = g_new0(Relay, 1);
  relay->service = self;
  relay->url = g_strdup(url);
  relay->key_state = GH_NIP29_RELAY_KEY_UNKNOWN;
  g_hash_table_insert(self->relays, relay->url, relay);
  return relay;
}

/* The relay's rooms that read it now (room_reads()). */
static GPtrArray *
relay_rooms(Relay *relay)
{
  GPtrArray *rooms = g_ptr_array_new();
  GhNip29Service *self = relay->service;
  for (guint i = 0; i < self->rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(self->rooms, i);
    if (room_reads(room) && g_str_equal(room_relay(room), relay->url))
      g_ptr_array_add(rooms, room);
  }
  return rooms;
}

/* The room an event of group_id is for: one that reads its state, or with
 * everything, one that is (about to be) in the group. */
static GhNip29Room *
relay_room(Relay *relay, const gchar *group_id, gboolean state_only)
{
  if (!group_id)
    return NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(relay->url, group_id, NULL);
  GhNip29Room *room = key ? g_hash_table_lookup(relay->service->by_key, key) : NULL;
  if (!room)
    return NULL;
  return (state_only ? room_reads(room) : join_subscribed(room->join)) ? room : NULL;
}

static NostrFilters *
relay_filters(Relay *relay, GPtrArray *rooms)
{
  GhNip29Service *self = relay->service;
  NostrFilters *filters = nostr_filters_new();
  g_autoptr(GPtrArray) ids = g_ptr_array_new();
  for (guint i = 0; i < rooms->len; i++)
    g_ptr_array_add(ids, (gpointer)room_group_id(g_ptr_array_index(rooms, i)));
  g_ptr_array_add(ids, NULL);
  const gchar *const *groups = (const gchar *const *)ids->pdata;

  /* The relay-signed group state of every group, also of one whose refusal
   * awaits it (room_reads()); the rest only for the groups the account is
   * (about to be) in. */
  NostrFilter *meta = nostr_filter_new();
  int meta_kinds[] = { NOSTR_KIND_SIMPLE_GROUP_METADATA, NOSTR_KIND_SIMPLE_GROUP_ADMINS,
                       NOSTR_KIND_SIMPLE_GROUP_MEMBERS, NOSTR_KIND_SIMPLE_GROUP_ROLES };
  nostr_filter_set_kinds(meta, meta_kinds, G_N_ELEMENTS(meta_kinds));
  /* libnostr keeps a "#d": [a, b] filter as one ["d", value] entry each. */
  for (guint i = 0; groups[i]; i++)
    nostr_filter_tags_append(meta, "d", groups[i], NULL);
  nostr_filters_add(filters, meta);
  nostr_filter_free(meta);
  g_autoptr(GPtrArray) joined = g_ptr_array_new();
  for (guint i = 0; i < rooms->len; i++)
    if (join_subscribed(((GhNip29Room *)g_ptr_array_index(rooms, i))->join))
      g_ptr_array_add(joined, g_ptr_array_index(rooms, i));
  if (joined->len == 0)
    return filters;
  rooms = joined;

  /* Each group's messages, from its own cursor. */
  int message_kinds[] = { NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE,
                          NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY,
                          NOSTR_KIND_SIMPLE_GROUP_THREAD, NOSTR_KIND_SIMPLE_GROUP_REPLY };
  for (guint i = 0; i < rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(rooms, i);
    NostrFilter *messages = nostr_filter_new();
    nostr_filter_set_kinds(messages, message_kinds, G_N_ELEMENTS(message_kinds));
    nostr_filter_tags_append(messages, "h", room_group_id(room), NULL);
    if (room->cursor > 0)
      nostr_filter_set_since_i64(messages, MAX(room->cursor - GH_NIP29_SERVICE_CURSOR_OVERLAP, 1));
    /* A first read must be paged too. A filter-owned limit exempts it from
     * GhRelayScope's until/limit paging, leaving only the newest messages. */
    nostr_filters_add(filters, messages);
    nostr_filter_free(messages);

    if (relay->service->reactions) {
      /* Reactions and author deletions cannot use the chat cursor: a group
       * may have old reactions that predate its latest message. */
      int reaction_kinds[] = { NOSTR_KIND_DELETION, NOSTR_KIND_REACTION };
      NostrFilter *reactions = nostr_filter_new();
      nostr_filter_set_kinds(reactions, reaction_kinds, G_N_ELEMENTS(reaction_kinds));
      nostr_filter_tags_append(reactions, "h", room_group_id(room), NULL);
      if (room->reaction_cursor > 0)
        nostr_filter_set_since_i64(reactions,
          MAX(room->reaction_cursor - GH_NIP29_SERVICE_CURSOR_OVERLAP, 1));
      nostr_filters_add(filters, reactions);
      nostr_filter_free(reactions);
    }
  }

  /* Deletions (9005), so deleted events leave the rooms and the timeline:
   * from the oldest cursor, or the newest ones the first time. */
  NostrFilter *deletions = nostr_filter_new();
  int deletion_kind[] = { NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT };
  nostr_filter_set_kinds(deletions, deletion_kind, 1);
  gint64 oldest = G_MAXINT64;
  for (guint i = 0; i < rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(rooms, i);
    nostr_filter_tags_append(deletions, "h", room_group_id(room), NULL);
    oldest = MIN(oldest, room->cursor);
  }
  if (oldest > 0 && oldest != G_MAXINT64)
    nostr_filter_set_since_i64(deletions, MAX(oldest - GH_NIP29_SERVICE_CURSOR_OVERLAP, 1));
  else
    nostr_filter_set_limit(deletions, GH_NIP29_SERVICE_INITIAL_HISTORY);
  nostr_filters_add(filters, deletions);
  nostr_filter_free(deletions);

  /* The moderation events that say whether the account is in. */
  NostrFilter *mine = nostr_filter_new();
  int mine_kinds[] = { NOSTR_KIND_SIMPLE_GROUP_ADD_USER, NOSTR_KIND_SIMPLE_GROUP_REMOVE_USER };
  nostr_filter_set_kinds(mine, mine_kinds, G_N_ELEMENTS(mine_kinds));
  for (guint i = 0; i < rooms->len; i++)
    nostr_filter_tags_append(mine, "h", room_group_id(g_ptr_array_index(rooms, i)), NULL);
  nostr_filter_tags_append(mine, "p", self->account, NULL);
  nostr_filters_add(filters, mine);
  nostr_filter_free(mine);
  return filters;
}

static void on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data);

static void
relay_close(Relay *relay)
{
  if (!relay->scope)
    return;
  gh_relay_scope_cancel(relay->scope);
  g_clear_pointer(&relay->scope, gh_relay_scope_unref);
}

/* One live REQ for the relay's subscribed groups, on a fresh connection;
 * none when it has none or the service does not run. */
static void
relay_resubscribe(Relay *relay)
{
  GhNip29Service *self = relay->service;
  relay_close(relay);
  /* A group the account is no longer in is not read any more. */
  for (guint i = 0; i < self->rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(self->rooms, i);
    if (!join_subscribed(room->join) && g_str_equal(room_relay(room), relay->url))
      room_set_read(room, GH_NIP29_READ_IDLE, NULL);
  }
  g_autoptr(GPtrArray) rooms = relay_rooms(relay);
  if (rooms->len == 0 || !running(self)) {
    for (guint i = 0; i < rooms->len; i++)
      room_set_read(g_ptr_array_index(rooms, i), GH_NIP29_READ_IDLE, NULL);
    return;
  }
  relay_ensure_key(relay, FALSE);
  NostrFilters *filters = relay_filters(relay, rooms);
  GhRelayScope *scope = self->custom_scope
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->scope_transport,
                                        self->scope_data, on_scope_update, relay)
    : gh_relay_scope_new(self->generation, filters, on_scope_update, relay);
  if (self->custom_scope && self->has_scope_auth)
    gh_relay_scope_set_auth_transport(scope, &self->scope_auth);
  /* Past the relay's result cap (nostrc-cpwf, nostrc-x055). */
  gh_relay_scope_set_backfill_paging(scope, GH_NIP29_SERVICE_PAGE_LIMIT,
                                     GH_NIP29_SERVICE_MAX_PAGES);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_scope_add_url(scope, relay->url, &error)) {
    g_warning("Groundhog cannot read a group relay: %s", error->message);
    gh_relay_scope_unref(scope);
    for (guint i = 0; i < rooms->len; i++)
      room_set_read(g_ptr_array_index(rooms, i), GH_NIP29_READ_REFUSED, error->message);
    return;
  }
  /* §4.3/§4.4: a group relay may ask the account to sign in (on challenge). */
  if (!gh_auth_policy_apply_scope(self->policy, scope, GH_AUTH_PURPOSE_GROUP, relay->url,
                                  &error)) {
    g_debug("Groundhog will not sign in to a group relay: %s", error->message);
    g_clear_error(&error);
  }
  relay->scope = scope;
  for (guint i = 0; i < rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(rooms, i);
    if (!join_subscribed(room->join))
      continue; /* only its state is asked: nothing is read (IDLE) */
    /* A new REQ, from the committed cursor: nothing of it is stored yet. */
    room->sync_cursor = 0;
    room->sync_failed = FALSE;
    room->reaction_sync_cursor = 0;
    room->reaction_sync_failed = FALSE;
    room_set_read(room, GH_NIP29_READ_SYNCING, NULL);
  }
  gh_relay_scope_start(scope);
}

/* ---- Relay keys ----------------------------------------------------------------------- */

static gboolean
key_retry_fired(gpointer data)
{
  Relay *relay = data;
  relay->key_retry = 0;
  relay_ensure_key(relay, FALSE);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GhNip29Service *service;
  gchar *url;
  GCancellable *cancellable;
} KeyCall;

static void
on_relay_key(GObject *source, GAsyncResult *result, gpointer data)
{
  KeyCall *call = data;
  (void)source;
  g_autoptr(GError) error = NULL;
  GhNip29Service *self = call->service;
  g_autofree gchar *key = gh_nip11_fetch_relay_key_finish(result, &error);
  Relay *relay = g_cancellable_is_cancelled(call->cancellable)
                   ? NULL : g_hash_table_lookup(self->relays, call->url);
  if (relay && relay->key_fetch == call->cancellable) {
    g_clear_object(&relay->key_fetch);
    g_autofree gchar *lower = key ? g_ascii_strdown(key, -1) : NULL;
    g_autoptr(GPtrArray) rooms = g_ptr_array_new();
    for (guint i = 0; i < self->rooms->len; i++) {
      GhNip29Room *room = g_ptr_array_index(self->rooms, i);
      if (g_str_equal(room_relay(room), relay->url))
        g_ptr_array_add(rooms, g_object_ref(room));
    }
    if (lower && gh_nip29_is_hex64(lower)) {
      g_free(relay->key);
      relay->key = g_steal_pointer(&lower);
      relay->key_state = GH_NIP29_RELAY_KEY_PINNED;
      for (guint i = 0; i < rooms->len; i++) {
        GhNip29Room *room = g_ptr_array_index(rooms, i);
        room_set_relay_key(room, relay->key);
        room_admit_held(room);
      }
    } else {
      g_message("Groundhog cannot verify a group relay's state: %s",
                error ? error->message : "no key");
      relay->key_state = GH_NIP29_RELAY_KEY_UNAVAILABLE;
      for (guint i = 0; i < rooms->len; i++) {
        GhNip29Room *room = g_ptr_array_index(rooms, i);
        if (!room->group)
          room_set_key_state(room, GH_NIP29_RELAY_KEY_UNAVAILABLE);
      }
      /* A ws:// relay's document is never fetched (no plaintext): asking
       * again cannot help. */
      if (!relay->key_retry && !g_error_matches(error, GH_NIP11_ERROR, GH_NIP11_ERROR_PLAINTEXT))
        relay->key_retry = gh_clock_timeout_add(self->clock, KEY_RETRY_S * 1000, key_retry_fired,
                                                relay, NULL);
    }
    g_ptr_array_set_free_func(rooms, g_object_unref);
  }
  g_object_unref(call->cancellable);
  g_object_unref(call->service);
  g_free(call->url);
  g_free(call);
}

/* Fetches the relay's NIP-11 key when a room lacks one (or, forced, once per
 * session after a snapshot signed by another key). */
static void
relay_ensure_key(Relay *relay, gboolean force)
{
  GhNip29Service *self = relay->service;
  if (relay->key_fetch || !running(self))
    return;
  gboolean needed = force;
  for (guint i = 0; i < self->rooms->len && !needed; i++) {
    GhNip29Room *room = g_ptr_array_index(self->rooms, i);
    needed = room_reads(room) && !room->group && g_str_equal(room_relay(room), relay->url);
  }
  if (!needed)
    return;
  if (relay->key && !force) {
    /* Pinned by another room of this relay (this session or before). */
    for (guint i = 0; i < self->rooms->len; i++) {
      GhNip29Room *room = g_ptr_array_index(self->rooms, i);
      if (g_str_equal(room_relay(room), relay->url) && !room->group) {
        room_set_relay_key(room, relay->key);
        room_admit_held(room);
      }
    }
    return;
  }
  if (relay->key_retry) {
    gh_clock_source_remove(self->clock, relay->key_retry);
    relay->key_retry = 0;
  }
  KeyCall *call = g_new0(KeyCall, 1);
  call->service = g_object_ref(self);
  call->url = g_strdup(relay->url);
  call->cancellable = g_cancellable_new();
  relay->key_fetch = g_object_ref(call->cancellable);
  gh_nip11_fetch_relay_key_async(self->http, relay->url, call->cancellable, on_relay_key, call);
}

/* ---- Relay events ---------------------------------------------------------------------- */

/* The sync cursor after a message was dealt with for good (stored, a
 * duplicate, hidden, or unusable). The cursor is the since of the next REQ,
 * so it may only pass what is durably stored with nothing missing before
 * it: a backfill event only raises this REQ's pending cursor, committed at
 * EOSE (on_scope_update); a live one (after EOSE) moves it at once. Once an
 * admission of this REQ failed it moves no more, and the next REQ asks again
 * from where it was (W15 review non-blocking #1). */
static void
room_advance_cursor(GhNip29Room *room, gint64 created_at, gboolean backfill)
{
  GhNip29Service *self = room->service;
  gint64 bounded = MIN(created_at, now_unix(self) + GH_NIP29_MAX_FUTURE_SKEW_SECONDS);
  if (room->sync_failed)
    return;
  if (backfill) {
    room->sync_cursor = MAX(room->sync_cursor, bounded);
  } else if (bounded > room->cursor) {
    room->cursor = bounded;
    room_save(room);
  }
}

static void
room_advance_reaction_cursor(GhNip29Room *room, gint64 created_at, gboolean backfill)
{
  gint64 bounded = MIN(created_at, now_unix(room->service) + GH_NIP29_MAX_FUTURE_SKEW_SECONDS);
  if (room->reaction_sync_failed)
    return;
  if (backfill)
    room->reaction_sync_cursor = MAX(room->reaction_sync_cursor, bounded);
  else if (bounded > room->reaction_cursor) {
    room->reaction_cursor = bounded;
    room_save(room);
  }
}

static void
room_admit_message(GhNip29Room *room, NostrEvent *event, const gchar *json, gboolean backfill)
{
  GhNip29Service *self = room->service;
  gint64 created_at = nostr_event_get_created_at(event);
  gchar id[65] = { 0 };
  if (nostr_event_compute_id(event, id) != NOSTR_EVENT_VALIDATION_OK)
    return;
  if (gh_nip29_timeline_add(room->timeline, id, nostr_event_get_pubkey(event), created_at))
    room_save(room);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_nip29_event(self->account, room_relay(room),
                                                                  json, &error);
  if (!message) {
    /* Never storable: asking for it again would not help. */
    g_debug("Groundhog ignored a group message: %s", error->message);
    room_advance_cursor(room, created_at, backfill);
    return;
  }
  gh_message_add_relay(message, room_relay(room));
  /* The event is its own carrier: a delivered event, deduplicated on its id. */
  GhConversationAddResult added = gh_conversation_store_admit(self->conversations, message, id,
                                                              &error);
  if (added == GH_CONVERSATION_ADD_FAILED) {
    g_warning("Groundhog could not store a group message: %s", error->message);
    room->sync_failed = TRUE;
    return;
  }
  room_advance_cursor(room, created_at, backfill);
  /* The history a join brings in is not "unread": everything dated before
   * the join request is read while nothing newer is listed. (Not at EOSE:
   * the relay layer may report EOSE before the backfill it ends.) */
  GhConversation *conversation = added == GH_CONVERSATION_ADD_NEW
    ? gh_conversation_store_lookup(self->conversations, room->room_id) : NULL;
  guint n = conversation ? g_list_model_get_n_items(G_LIST_MODEL(conversation)) : 0;
  if (n > 0 && created_at <= room->joined_at) {
    g_autoptr(GhMessage) newest = g_list_model_get_item(G_LIST_MODEL(conversation), n - 1);
    if (gh_message_get_created_at(newest) <= room->joined_at)
      gh_conversation_mark_read(conversation);
  }
}

static void
relay_event(Relay *relay, const gchar *json, gboolean backfill)
{
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return;
  }
  gint kind = nostr_event_get_kind(event);
  if (kind >= NOSTR_KIND_SIMPLE_GROUP_METADATA && kind <= NOSTR_KIND_SIMPLE_GROUP_ROLES) {
    GhNip29Room *room = relay_room(relay, first_tag_value(event, "d"), TRUE);
    if (room) {
      AdmitOutcome outcome = room_admit_snapshot(room, event, json);
      if (outcome == ADMIT_NO_KEY) {
        relay_ensure_key(relay, FALSE);
      } else if (outcome == ADMIT_FOREIGN && !relay->key_refetched) {
        relay->key_refetched = TRUE;
        relay_ensure_key(relay, TRUE);
      }
    }
  } else {
    GhNip29Room *room = relay_room(relay, first_tag_value(event, "h"), FALSE);
    if (room && kind >= NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE &&
        kind <= NOSTR_KIND_SIMPLE_GROUP_REPLY) {
      room_admit_message(room, event, json, backfill);
    } else if (room && room->service->reactions && kind == NOSTR_KIND_REACTION) {
      /* W26 slice B (nostrc-191r): NIP-25 reaction in a NIP-29 group. */
      g_autoptr(GError) reaction_error = NULL;
      NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
      const gchar *target_id = NULL;
      if (tags) {
        for (size_t ti = 0; ti < nostr_tags_size(tags); ti++) {
          NostrTag *tag = nostr_tags_get(tags, ti);
          if (tag && g_strcmp0(nostr_tag_get_key(tag), "e") == 0 && nostr_tag_get_value(tag)) {
            target_id = nostr_tag_get_value(tag);
            break;
          }
        }
      }
      if (target_id) {
        const gchar *emoji = nostr_event_get_content(event);
        if (!emoji || !*emoji)
          emoji = "+";
        gchar id[65] = { 0 };
        if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
          g_autoptr(GhReaction) reaction =
            gh_reaction_new(target_id, id, nostr_event_get_pubkey(event),
                            emoji, nostr_event_get_created_at(event), room->room_id);
          if (reaction)
            gh_reaction_store_admit(room->service->reactions, reaction, &reaction_error);
        }
      }
      if (reaction_error) {
        room->reaction_sync_failed = TRUE;
        g_message("Groundhog could not store a group reaction: %s", reaction_error->message);
      } else {
        room_advance_reaction_cursor(room, nostr_event_get_created_at(event), backfill);
      }
    } else if (room && room->service->reactions && kind == NOSTR_KIND_DELETION) {
      /* W26 slice B: NIP-25 deletion in a NIP-29 group.
       * W26 slice B review fix (F3): only the reaction's author may delete
       * it (NIP-09). The event's pubkey is the deletion sender. */
      const gchar *deletion_sender = nostr_event_get_pubkey(event);
      g_autoptr(GError) deletion_error = NULL;
      NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
      if (tags && deletion_sender) {
        for (size_t ti = 0; ti < nostr_tags_size(tags); ti++) {
          NostrTag *tag = nostr_tags_get(tags, ti);
          if (tag && g_strcmp0(nostr_tag_get_key(tag), "e") == 0 && nostr_tag_get_value(tag)) {
            const gchar *rid = nostr_tag_get_value(tag);
            gh_reaction_store_delete_event(room->service->reactions, rid,
                                           deletion_sender, room->room_id, &deletion_error);
            if (deletion_error)
              break;
          }
        }
      }
      if (deletion_error) {
        room->reaction_sync_failed = TRUE;
        g_message("Groundhog could not store a group reaction deletion: %s",
                  deletion_error->message);
      } else {
        room_advance_reaction_cursor(room, nostr_event_get_created_at(event), backfill);
      }
    } else if (room && (kind == NOSTR_KIND_SIMPLE_GROUP_ADD_USER ||
                        kind == NOSTR_KIND_SIMPLE_GROUP_REMOVE_USER ||
                        kind == NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT)) {
      gchar id[65] = { 0 };
      if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK &&
          gh_nip29_timeline_add(room->timeline, id, nostr_event_get_pubkey(event),
                                nostr_event_get_created_at(event)))
        room_save(room);
      if (kind == NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT)
        room_deletion_event(room, event);
      else
        room_membership_event(room, event);
    }
  }
  nostr_event_free(event);
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Relay *relay = data;
  if (relay->scope != scope)
    return;
  g_autoptr(GhNip29Service) self = g_object_ref(relay->service);
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    relay_event(relay, update->event_json, update->backfill);
    return;
  default:
    break;
  }
  /* A handler of the rooms' notifications may change the room list. */
  g_autoptr(GPtrArray) rooms = relay_rooms(relay);
  for (guint i = 0; i < rooms->len; i++)
    g_object_ref(g_ptr_array_index(rooms, i));
  g_ptr_array_set_free_func(rooms, g_object_unref);
  gboolean settled = FALSE;
  for (guint i = 0; i < rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(rooms, i);
    if (!join_subscribed(room->join)) {
      /* A refusal awaited the group state: the REQ's answer has ended (the
       * state the relay has came before its EOSE), or the relay refused the
       * REQ. The room is no longer read; at an EOSE the REQ is rebuilt
       * without it (a refused REQ is not asked again). */
      if (update->notice == GH_RELAY_NOTICE_EOSE || update->notice == GH_RELAY_NOTICE_CLOSED) {
        room->awaiting_state = FALSE;
        settled |= update->notice == GH_RELAY_NOTICE_EOSE;
      }
      continue;
    }
    switch (update->notice) {
    case GH_RELAY_NOTICE_EOSE:
      room_set_read(room, GH_NIP29_READ_LIVE, NULL);
      room->backfilled = TRUE;
      if (!update->incomplete && !room->sync_failed && room->cursor == 0)
        room->history_state = HISTORY_COMPLETE;
      /* The backfill is complete (paged past the relay's cap): what it
       * stored has nothing missing before it. An incomplete one keeps the
       * cursor for as long as this REQ lives -- live messages included, or
       * the first one would move it past the unfetched stretch (review B1)
       * -- so the next REQ asks for that stretch again. */
      if (update->incomplete) {
        room->sync_failed = TRUE;
        room->reaction_sync_failed = TRUE;
        g_message("Groundhog could not fetch every older message of a group; its read "
                  "cursor stays where it was");
      } else {
        if (!room->sync_failed && room->sync_cursor > room->cursor)
          room->cursor = room->sync_cursor;
        if (self->reactions && !room->reaction_sync_failed)
          room->reaction_cursor = MAX(room->reaction_cursor,
            MAX(room->reaction_sync_cursor, now_unix(self)));
      }
      room_save(room);
      history_refresh_availability(room);
      break;
    case GH_RELAY_NOTICE_CLOSED:
      room_set_read(room, gh_relay_auth_is_required(update->detail)
                            ? GH_NIP29_READ_AUTH_REQUIRED : GH_NIP29_READ_REFUSED,
                    update->detail);
      break;
    case GH_RELAY_NOTICE_DISCONNECTED:
    case GH_RELAY_NOTICE_ERROR:
      if (room->read != GH_NIP29_READ_AUTH_REQUIRED && room->read != GH_NIP29_READ_REFUSED)
        room_set_read(room, GH_NIP29_READ_DISCONNECTED, NULL);
      break;
    default:
      break;
    }
  }
  if (settled)
    relay_resubscribe(relay);
}

/* ---- Operations -------------------------------------------------------------------------- */

static GhNip29Room *
room_for_op(GhNip29Service *self, GhNip29Op *op)
{
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(gh_nip29_op_get_relay_url(op),
                                                          gh_nip29_op_get_group_id(op), NULL);
  return key ? g_hash_table_lookup(self->by_key, key) : NULL;
}

static void
apply_join_result(GhNip29Room *room, GhNip29Op *op)
{
  GhNip29OpResult result = gh_nip29_op_get_result(op);
  const gchar *message = gh_nip29_op_get_relay_message(op);
  if (room->join != GH_NIP29_JOIN_REQUESTING && room->join != GH_NIP29_JOIN_PENDING &&
      room->join != GH_NIP29_JOIN_NOT_SENT)
    return; /* evidence (a 9000, a 39002) or a leave already decided */
  switch (result) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
    room_set_join(room, GH_NIP29_JOIN_MEMBER, 0, NULL);
    break;
  case GH_NIP29_OP_PENDING_APPROVAL:
    room_set_join(room, GH_NIP29_JOIN_PENDING, 0, message);
    break;
  case GH_NIP29_OP_REJECTED:
    /* A closed group refuses a request without a code: its 39000 tells
     * CLOSED from DENIED (room_sync_metadata() moves one to the other). The
     * answer comes on the publish connection and the group state on the
     * REQ's, so the refusal can come first, even when the relay sent the
     * state first (nostrc-kfso). While the REQ has not answered, the room
     * keeps reading the group state until its EOSE; nothing else of the
     * group is read. */
    if (!room->join_with_code &&
        (room->read == GH_NIP29_READ_SYNCING || room->read == GH_NIP29_READ_DISCONNECTED))
      room->awaiting_state = TRUE;
    room_set_join(room, room->closed && !room->join_with_code ? GH_NIP29_JOIN_CLOSED
                                                              : GH_NIP29_JOIN_DENIED,
                  0, message);
    /* Not reading messages, even while its state is still read. */
    room_set_read(room, GH_NIP29_READ_IDLE, NULL);
    break;
  case GH_NIP29_OP_NOT_SENT:
    room_set_join(room, GH_NIP29_JOIN_NOT_SENT, 0, message);
    break;
  case GH_NIP29_OP_CANCELLED:
    room_set_join(room, GH_NIP29_JOIN_NONE, 0, NULL);
    break;
  default:
    if (room->join == GH_NIP29_JOIN_NOT_SENT) /* a retry is under way */
      room_set_join(room, GH_NIP29_JOIN_REQUESTING, 0, NULL);
    break;
  }
}

static void
apply_leave_result(GhNip29Room *room, GhNip29Op *op)
{
  if (room->join != GH_NIP29_JOIN_LEAVING)
    return;
  switch (gh_nip29_op_get_result(op)) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
    room_set_join(room, GH_NIP29_JOIN_LEFT, 0, NULL);
    break;
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
  case GH_NIP29_OP_CANCELLED:
  case GH_NIP29_OP_PENDING_APPROVAL:
    /* Still in the group: the relay did not let the account out. */
    room_set_join(room, GH_NIP29_JOIN_MEMBER, 0, gh_nip29_op_get_relay_message(op));
    break;
  default:
    break;
  }
}

/* A group the relay did not create is not kept: its record, its (empty)
 * conversation and its room go, from an idle after the answer. */
static gboolean
room_discard_now(gpointer data)
{
  GhNip29Room *room = data;
  room->discard_idle = 0;
  GhNip29Service *self = room->service;
  if (!self || room->join != GH_NIP29_JOIN_NONE)
    return G_SOURCE_REMOVE;
  if (room->save_idle) {
    g_source_remove(room->save_idle);
    room->save_idle = 0;
  }
  g_autoptr(GError) error = NULL;
  if (room->conversation_id > 0 && self->store &&
      (!gh_store_nip29_delete_group(self->store, room->conversation_id, &error) ||
       !gh_store_forget_conversation(self->store, room->conversation_id, &error)))
    g_message("Groundhog could not drop a group the relay did not create: %s", error->message);
  gh_conversation_store_remove(self->conversations, room->room_id);
  guint position = 0;
  if (g_ptr_array_find(self->rooms, room, &position)) {
    g_hash_table_remove(self->by_key, room->key);
    g_hash_table_remove(self->by_room_id, room->room_id);
    room->service = NULL;
    g_ptr_array_remove_index(self->rooms, position);
    g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
  }
  return G_SOURCE_REMOVE;
}

/* The metadata chosen for a new group, sent as its first edit (9002). */
static void
room_send_create_meta(GhNip29Room *room)
{
  g_autofree gchar *meta = g_steal_pointer(&room->create_meta);
  if (!meta)
    return;
  room_save(room);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, meta, -1, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    return;
  JsonObject *object = json_node_get_object(json_parser_get_root(parser));
  g_autoptr(GhNip29Metadata) metadata = gh_nip29_metadata_new();
  metadata->name = g_strdup(json_object_get_string_member_with_default(object, "name", NULL));
  metadata->about = g_strdup(json_object_get_string_member_with_default(object, "about", NULL));
  metadata->is_private = json_object_get_boolean_member_with_default(object, "private", FALSE);
  metadata->is_closed = json_object_get_boolean_member_with_default(object, "closed", FALSE);
  GhNip29TemplateContext context;
  g_autoptr(GError) error = NULL;
  room_context(room, &context);
  g_autofree gchar *edit = gh_nip29_template_edit_metadata(room->key, &context, metadata, NULL,
                                                           &error);
  g_autoptr(GhNip29Op) op = edit ? room_enqueue(room, edit, &error) : NULL;
  if (!op)
    g_message("Groundhog could not name a new group: %s", error ? error->message : "unknown");
}

static void
apply_create_result(GhNip29Room *room, GhNip29Op *op)
{
  if (room->join != GH_NIP29_JOIN_CREATING)
    return;
  GhNip29Service *self = room->service;
  switch (gh_nip29_op_get_result(op)) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE: /* the relay already has this very request */
    /* The relay made the account the group's admin; its snapshots follow. */
    room_set_join(room, GH_NIP29_JOIN_MEMBER, 0, NULL);
    if (self)
      gh_conversation_store_ensure_group(self->conversations, room->room_id, room->name);
    room_send_create_meta(room);
    break;
  case GH_NIP29_OP_REJECTED:
  case GH_NIP29_OP_NOT_SENT:
  case GH_NIP29_OP_CANCELLED:
  case GH_NIP29_OP_PENDING_APPROVAL:
    g_clear_pointer(&room->create_meta, g_free);
    room_set_join(room, GH_NIP29_JOIN_NONE, 0, gh_nip29_op_get_relay_message(op));
    if (!room->discard_idle)
      room->discard_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, room_discard_now,
                                           g_object_ref(room), g_object_unref);
    break;
  default:
    break;
  }
}

/* The local echo of a chat message shows its honest status. */
static void
apply_message_status(GhNip29Service *self, GhNip29Op *op)
{
  GhMessage *message = gh_conversation_store_lookup_message(self->conversations,
                                                            gh_nip29_op_get_event_id(op));
  if (message)
    gh_message_set_status(message, gh_nip29_op_result_to_message_status(
                                     gh_nip29_op_get_result(op), running(self)));
}

static void
on_op_changed(GhNip29Outbox *outbox, GhNip29Op *op, gpointer data)
{
  GhNip29Service *self = data;
  (void)outbox;
  GhNip29Room *room = room_for_op(self, op);
  gint kind = gh_nip29_op_get_kind(op);
  if (kind >= NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE && kind <= NOSTR_KIND_SIMPLE_GROUP_REPLY)
    apply_message_status(self, op);
  if (!room)
    return;
  GhNip29OpResult result = gh_nip29_op_get_result(op);
  if (kind == NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT &&
      (result == GH_NIP29_OP_ACCEPTED || result == GH_NIP29_OP_DUPLICATE)) {
    /* Our own deletion, accepted: at once, before the relay echoes it. */
    NostrEvent *event = nostr_event_new();
    if (gh_nip29_op_get_signed_json(op) &&
        nostr_event_deserialize_signed(event, gh_nip29_op_get_signed_json(op), NULL) ==
          NOSTR_EVENT_VALIDATION_OK) {
      NostrTags *tags = nostr_event_get_tags(event);
      for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "e") == 0)
          room_forget_event(room, nostr_tag_get(tag, 1));
      }
    }
    nostr_event_free(event);
  }
  if (kind == NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP &&
      gh_nip29_op_get_outbox_id(op) == room->create_op)
    apply_create_result(room, op);
  else if (kind == NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST &&
      gh_nip29_op_get_outbox_id(op) == room->join_op)
    apply_join_result(room, op);
  else if (kind == NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST &&
           gh_nip29_op_get_outbox_id(op) == room->leave_op)
    apply_leave_result(room, op);
}

static gboolean
check_active(GhNip29Service *self, GError **error)
{
  if (self->generation)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                      "This account is not the active account");
  return FALSE;
}

/* The template context: the account, now, and the room's timeline. */
static void
room_context(GhNip29Room *room, GhNip29TemplateContext *context)
{
  gsize n = 0;
  const GhNip29TimelineRef *refs = gh_nip29_timeline_get_refs(room->timeline, &n);
  context->author_pubkey = room->service->account;
  context->created_at = now_unix(room->service);
  context->recent = refs;
  context->n_recent = n;
}

static GhNip29Op *
room_enqueue(GhNip29Room *room, const gchar *unsigned_json, GError **error)
{
  GhNip29Service *self = room->service;
  if (!unsigned_json)
    return NULL;
  if (room->conversation_id <= 0)
    room_save_flush(room);
  GhNip29Op *op = gh_nip29_outbox_enqueue(self->outbox, room->conversation_id, unsigned_json,
                                          error);
  if (!op)
    return NULL;
  /* Own events occupy slots of the timeline window (gh-nip29-template.h). */
  const gchar *id = gh_nip29_op_get_event_id(op);
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_unsigned(event, unsigned_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      gh_nip29_timeline_add(room->timeline, id, self->account, nostr_event_get_created_at(event)))
    room_save(room);
  nostr_event_free(event);
  return op;
}

/* ---- Public: rooms ------------------------------------------------------------------------ */

static GhNip29Room *
room_new(GhNip29Service *self, const GhNip29GroupKey *key)
{
  GhNip29Room *room = g_object_new(GH_TYPE_NIP29_ROOM, NULL);
  room->service = self;
  room->key = gh_nip29_group_key_copy(key);
  room->room_id = gh_message_nip29_room_id(gh_nip29_group_key_get_relay_url(key),
                                           gh_nip29_group_key_get_group_id(key));
  room->relay_pubkey = g_strdup("");
  room->timeline = gh_nip29_timeline_new();
  room->join = GH_NIP29_JOIN_NONE;
  room->read = GH_NIP29_READ_IDLE;
  room->key_state = GH_NIP29_RELAY_KEY_UNKNOWN;
  room->members = GH_NIP29_MEMBERS_UNAVAILABLE;
  g_ptr_array_add(self->rooms, room);
  g_hash_table_insert(self->by_key, room->key, room);
  g_hash_table_insert(self->by_room_id, room->room_id, room);
  relay_get(self, gh_nip29_group_key_get_relay_url(key));
  g_list_model_items_changed(G_LIST_MODEL(self), self->rooms->len - 1, 0, 1);
  return room;
}

static gboolean
parse_join(const gchar *nick, GhNip29JoinState *join)
{
  GEnumClass *joins = g_type_class_ref(GH_TYPE_NIP29_JOIN_STATE);
  GEnumValue *value = nick ? g_enum_get_value_by_nick(joins, nick) : NULL;
  if (value)
    *join = value->value;
  g_type_class_unref(joins);
  return value != NULL;
}

/* A stored group: its record re-read, its snapshots admitted again (their
 * signatures are checked again, against the pinned key). */
static void
room_restore(GhNip29Service *self, const GhStoreNip29Group *stored)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(stored->relay_url, stored->group_id,
                                                          &error);
  if (!key || g_hash_table_contains(self->by_key, key)) {
    if (error)
      g_warning("Groundhog skipped a stored group: %s", error->message);
    return;
  }
  GhNip29Room *room = room_new(self, key);
  room->conversation_id = stored->conversation_id;
  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *record = NULL;
  if (stored->state_json && json_parser_load_from_data(parser, stored->state_json, -1, NULL) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    record = json_node_get_object(json_parser_get_root(parser));
  if (record && json_object_get_int_member_with_default(record, "v", 0) == RECORD_VERSION) {
    GhNip29JoinState join = GH_NIP29_JOIN_NONE;
    if (parse_join(json_object_get_string_member_with_default(record, "join", NULL), &join))
      room->join = join;
    room->evidence_at = json_object_get_int_member_with_default(record, "evidence_at", 0);
    room->left_at = json_object_get_int_member_with_default(record, "left_at", 0);
    room->join_with_code = json_object_get_boolean_member_with_default(record, "join_code", FALSE);
    room->join_op = json_object_get_int_member_with_default(record, "join_op", 0);
    room->leave_op = json_object_get_int_member_with_default(record, "leave_op", 0);
    room->create_op = json_object_get_int_member_with_default(record, "create_op", 0);
    room->create_meta = g_strdup(json_object_get_string_member_with_default(record,
                                                                            "create_meta", NULL));
    room->detail = g_strdup(json_object_get_string_member_with_default(record, "detail", NULL));
    room->cursor = json_object_get_int_member_with_default(record, "cursor", 0);
    room->reaction_cursor = json_object_get_int_member_with_default(record, "reaction_cursor", 0);
    room->history_oldest = json_object_get_int_member_with_default(record, "oldest_until", 0);
    const gchar *history_state =
      json_object_get_string_member_with_default(record, "history_state", "incomplete");
    room->history_state = g_strcmp0(history_state, "complete") == 0 ? HISTORY_COMPLETE
                          : g_strcmp0(history_state, "partial") == 0 ? HISTORY_PARTIAL
                                                                    : HISTORY_INCOMPLETE;
    room->backfilled = json_object_get_boolean_member_with_default(record, "backfilled", FALSE);
    room->joined_at = json_object_get_int_member_with_default(record, "joined_at", 0);
    JsonArray *timeline = json_object_has_member(record, "timeline")
                            ? json_object_get_array_member(record, "timeline") : NULL;
    for (guint i = 0; timeline && i < json_array_get_length(timeline); i++) {
      JsonArray *entry = json_array_get_array_element(timeline, i);
      if (entry && json_array_get_length(entry) == 3)
        gh_nip29_timeline_add(room->timeline, json_array_get_string_element(entry, 0),
                              json_array_get_string_element(entry, 1),
                              json_array_get_int_element(entry, 2));
    }
  }
  if (stored->relay_pubkey && *stored->relay_pubkey) {
    Relay *relay = relay_get(self, stored->relay_url);
    room_set_relay_key(room, stored->relay_pubkey);
    if (!relay->key)
      relay->key = g_strdup(stored->relay_pubkey);
    JsonArray *snapshots = record && json_object_has_member(record, "snapshots")
                             ? json_object_get_array_member(record, "snapshots") : NULL;
    for (guint i = 0; snapshots && i < json_array_get_length(snapshots); i++) {
      const gchar *json = json_array_get_string_element(snapshots, i);
      NostrEvent *event = nostr_event_new();
      if (json && nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
          room_admit_snapshot(room, event, json) != ADMIT_DONE)
        g_debug("Groundhog dropped a stored group snapshot that no longer verifies");
      nostr_event_free(event);
    }
    for (guint i = 0; i < SNAPSHOTS; i++)
      g_clear_pointer(&room->held[i], g_free);
  }
  room_sync_metadata(room);
  /* Joined (or joining) groups are listed before their first message; one
   * the relay has not created yet only once it has. */
  if ((join_subscribed(room->join) && room->join != GH_NIP29_JOIN_CREATING) ||
      room->join == GH_NIP29_JOIN_DENIED ||
      room->join == GH_NIP29_JOIN_CLOSED || room->join == GH_NIP29_JOIN_NOT_SENT)
    gh_conversation_store_ensure_group(self->conversations, room->room_id, room->name);
  history_refresh_availability(room);
}

/* The ops of the restored groups may have moved on after their last save. */
static void
reconcile_ops(GhNip29Service *self)
{
  g_autoptr(GPtrArray) ops = gh_nip29_outbox_dup_ops(self->outbox);
  for (guint i = 0; i < ops->len; i++)
    on_op_changed(self->outbox, g_ptr_array_index(ops, i), self);
}

static void
sync_all(GhNip29Service *self)
{
  g_autoptr(GList) relays = g_hash_table_get_values(self->relays);
  for (GList *l = relays; l; l = l->next)
    relay_resubscribe(l->data);
}

static void
update_activity(GhNip29Service *self)
{
  guint64 generation = 0;
  if (self->accounts &&
      gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    const gchar *npub = gh_account_controller_get_active_npub(self->accounts);
    g_autofree gchar *active = npub ? gh_identity_pubkey_hex(npub) : NULL;
    if (g_strcmp0(active, self->account) == 0)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  gboolean online = self->network && g_network_monitor_get_network_available(self->network);
  if (generation == self->generation && online == self->online)
    return;
  if (generation != self->generation) {
    /* Nothing of the old generation may complete in the new one. */
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, self->relays);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Relay *relay = value;
      relay_close(relay);
      if (relay->key_fetch) {
        g_cancellable_cancel(relay->key_fetch);
        g_clear_object(&relay->key_fetch);
      }
    }
    for (guint i = 0; i < self->rooms->len; i++)
      history_close(g_ptr_array_index(self->rooms, i));
  } else if (!online) {
    for (guint i = 0; i < self->rooms->len; i++)
      history_close(g_ptr_array_index(self->rooms, i));
  }
  self->generation = generation;
  self->online = online;
  sync_all(self);
  for (guint i = 0; i < self->rooms->len; i++)
    history_refresh_availability(g_ptr_array_index(self->rooms, i));
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

GhNip29Service *
gh_nip29_service_new(const GhNip29ServiceConfig *config, GError **error)
{
  g_return_val_if_fail(config != NULL && config->store != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  g_return_val_if_fail(!config->scope_transport ||
                       (config->scope_transport->open && config->scope_transport->close), NULL);
  g_return_val_if_fail(!config->settings || G_IS_SETTINGS(config->settings), NULL);
  g_autoptr(GhNip29Service) self = g_object_new(GH_TYPE_NIP29_SERVICE, NULL);
  self->store = config->store;
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->account = g_strdup(gh_store_get_account_pubkey(config->store));
  self->accounts = g_object_ref(config->accounts);
  self->policy = g_object_ref(gh_auth_policy_get_for_accounts(config->accounts));
  self->conversations = g_object_ref(config->conversations);
  self->reactions = config->reactions ? g_object_ref(config->reactions) : NULL;
  self->network = g_object_ref(config->network ? config->network
                                               : g_network_monitor_get_default());
  if (config->scope_transport) {
    self->scope_transport = *config->scope_transport;
    self->custom_scope = TRUE;
    if (config->scope_auth_transport) {
      self->scope_auth = *config->scope_auth_transport;
      self->has_scope_auth = TRUE;
    }
    self->scope_data = config->scope_transport_data;
  }
  self->http = gh_net_http_new(config->settings);

  /* Stored group rooms first, so the restored groups find their rooms. */
  self->rooms_store = gh_store_nip29_new(config->store);
  if (!gh_store_nip29_attach(self->rooms_store, config->conversations, 0, error))
    return NULL;
  GhNip29OutboxConfig outbox = {
    .store = config->store,
    .accounts = config->accounts,
    .network = self->network,
    .transport = config->publish_transport,
    .auth_transport = config->publish_auth_transport,
    .transport_data = config->publish_transport_data,
    .publish_deadline = config->publish_deadline,
  };
  self->outbox = gh_nip29_outbox_new(&outbox, error);
  if (!self->outbox)
    return NULL;
  g_autoptr(GPtrArray) stored = gh_store_nip29_list_groups(config->store, error);
  if (!stored)
    return NULL;
  for (guint i = 0; i < stored->len; i++)
    room_restore(self, g_ptr_array_index(stored, i));
  self->op_handler = g_signal_connect(self->outbox, "op-changed", G_CALLBACK(on_op_changed),
                                      self);
  reconcile_ops(self);
  self->accounts_handler = g_signal_connect(self->accounts, "changed",
                                            G_CALLBACK(on_accounts_changed), self);
  self->network_handler = g_signal_connect(self->network, "network-changed",
                                           G_CALLBACK(on_network_changed), self);
  update_activity(self);
  return g_steal_pointer(&self);
}

GhNip29Outbox *
gh_nip29_service_get_outbox(GhNip29Service *self)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  return self->outbox;
}

const gchar *
gh_nip29_service_get_account(GhNip29Service *self)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  return self->account;
}

GhNip29Room *
gh_nip29_service_lookup(GhNip29Service *self, const gchar *relay_url, const gchar *group_id)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(relay_url, group_id, NULL);
  GhNip29Room *room = key ? g_hash_table_lookup(self->by_key, key) : NULL;
  return room ? g_object_ref(room) : NULL;
}

GhNip29Room *
gh_nip29_service_lookup_room(GhNip29Service *self, const gchar *room_id)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  GhNip29Room *room = room_id ? g_hash_table_lookup(self->by_room_id, room_id) : NULL;
  return room ? g_object_ref(room) : NULL;
}

GhNip29Room *
gh_nip29_service_join(GhNip29Service *self, const gchar *relay_url, const gchar *group_id,
                      const gchar *reason, const gchar *invite_code, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!check_active(self, error))
    return NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(relay_url, group_id, error);
  if (!key)
    return NULL;
  GhNip29Room *room = g_hash_table_lookup(self->by_key, key);
  if (room && (room->join == GH_NIP29_JOIN_MEMBER || room->join == GH_NIP29_JOIN_REQUESTING ||
               room->join == GH_NIP29_JOIN_PENDING))
    return g_object_ref(room);
  Relay *relay = g_hash_table_lookup(self->relays, gh_nip29_group_key_get_relay_url(key));
  if (relay) {
    g_autoptr(GPtrArray) rooms = relay_rooms(relay);
    if (rooms->len >= GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                  "Groundhog follows at most %d groups on one relay",
                  GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY);
      return NULL;
    }
  }
  gboolean created = room == NULL;
  if (created)
    room = room_new(self, key);
  /* The group and its conversation are stored before anything is sent. */
  g_autofree gchar *record = room_record(room);
  gint64 conversation_id = 0;
  if (!gh_store_nip29_save_group(self->store, room_relay(room), room_group_id(room),
                                 room->relay_pubkey, record, room->name, &conversation_id,
                                 error)) {
    if (created) {
      guint position = self->rooms->len - 1;
      g_hash_table_remove(self->by_key, room->key);
      g_hash_table_remove(self->by_room_id, room->room_id);
      g_ptr_array_remove_index(self->rooms, position);
      g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
    }
    return NULL;
  }
  room->conversation_id = conversation_id;
  GhNip29TemplateContext context;
  room_context(room, &context);
  room->joined_at = context.created_at;
  g_autofree gchar *request = gh_nip29_template_join_request(room->key, &context, reason,
                                                             invite_code, error);
  g_autoptr(GhNip29Op) op = request ? room_enqueue(room, request, error) : NULL;
  if (!op)
    return NULL;
  room->join_op = gh_nip29_op_get_outbox_id(op);
  room->join_with_code = invite_code && *invite_code;
  gh_conversation_store_ensure_group(self->conversations, room->room_id, room->name);
  /* Subscribes to the relay (its scope is rebuilt for the new group). */
  room_set_join(room, GH_NIP29_JOIN_REQUESTING, 0, NULL);
  apply_join_result(room, op); /* it may have been answered already */
  return g_object_ref(room);
}

/* Stores a group record and lists a room for it (not its conversation). */
static GhNip29Room *
room_create_stored(GhNip29Service *self, const GhNip29GroupKey *key, GError **error)
{
  GhNip29Room *room = room_new(self, key);
  g_autofree gchar *record = room_record(room);
  gint64 conversation_id = 0;
  if (!gh_store_nip29_save_group(self->store, room_relay(room), room_group_id(room),
                                 room->relay_pubkey, record, room->name, &conversation_id,
                                 error)) {
    guint position = self->rooms->len - 1;
    g_hash_table_remove(self->by_key, room->key);
    g_hash_table_remove(self->by_room_id, room->room_id);
    room->service = NULL;
    g_ptr_array_remove_index(self->rooms, position);
    g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
    return NULL;
  }
  room->conversation_id = conversation_id;
  return room;
}

gchar *
gh_nip29_new_group_id(void)
{
  g_autofree gchar *random = gh_store_new_op_id(); /* 32 lowercase hex, OS CSPRNG */
  return g_strndup(random, 16);
}

GhNip29Room *
gh_nip29_service_create_group(GhNip29Service *self, const gchar *relay_url,
                              const gchar *group_id, const GhNip29Metadata *metadata,
                              GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!check_active(self, error))
    return NULL;
  g_autofree gchar *chosen = group_id && *group_id ? g_strdup(group_id) : gh_nip29_new_group_id();
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(relay_url, chosen, error);
  if (!key)
    return NULL;
  if (g_hash_table_contains(self->by_key, key)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "This group is already in the list");
    return NULL;
  }
  Relay *relay = g_hash_table_lookup(self->relays, gh_nip29_group_key_get_relay_url(key));
  if (relay) {
    g_autoptr(GPtrArray) rooms = relay_rooms(relay);
    if (rooms->len >= GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                  "Groundhog follows at most %d groups on one relay",
                  GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY);
      return NULL;
    }
  }
  GhNip29Room *room = room_create_stored(self, key, error);
  if (!room)
    return NULL;
  if (metadata) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    if (metadata->name && *metadata->name) {
      json_builder_set_member_name(builder, "name");
      json_builder_add_string_value(builder, metadata->name);
    }
    if (metadata->about && *metadata->about) {
      json_builder_set_member_name(builder, "about");
      json_builder_add_string_value(builder, metadata->about);
    }
    json_builder_set_member_name(builder, "private");
    json_builder_add_boolean_value(builder, metadata->is_private);
    json_builder_set_member_name(builder, "closed");
    json_builder_add_boolean_value(builder, metadata->is_closed);
    json_builder_end_object(builder);
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    room->create_meta = json_to_string(root, FALSE);
  }
  GhNip29TemplateContext context;
  room_context(room, &context);
  room->joined_at = context.created_at;
  g_autofree gchar *request = gh_nip29_template_create_group(room->key, &context, NULL, error);
  g_autoptr(GhNip29Op) op = request ? room_enqueue(room, request, error) : NULL;
  if (!op) {
    room->join = GH_NIP29_JOIN_NONE;
    room->discard_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, room_discard_now,
                                         g_object_ref(room), g_object_unref);
    return NULL;
  }
  room->create_op = gh_nip29_op_get_outbox_id(op);
  /* Subscribes to the relay, which publishes the group's state once made. */
  room_set_join(room, GH_NIP29_JOIN_CREATING, 0, NULL);
  apply_create_result(room, op); /* it may have been answered already */
  return g_object_ref(room);
}

static gboolean
check_room(GhNip29Service *self, GhNip29Room *room, GError **error)
{
  if (GH_IS_NIP29_ROOM(room) && room->service == self)
    return check_active(self, error);
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "Not a group of this account");
  return FALSE;
}

GhNip29Op *
gh_nip29_service_leave(GhNip29Service *self, GhNip29Room *room, const gchar *reason,
                       GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!check_room(self, room, error))
    return NULL;
  if (!join_subscribed(room->join) || room->join == GH_NIP29_JOIN_LEAVING ||
      room->join == GH_NIP29_JOIN_CREATING) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The account is not in this group");
    return NULL;
  }
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *request = gh_nip29_template_leave_request(room->key, &context, reason, error);
  GhNip29Op *op = request ? room_enqueue(room, request, error) : NULL;
  if (!op)
    return NULL;
  room->leave_op = gh_nip29_op_get_outbox_id(op);
  room->left_at = context.created_at;
  room_set_join(room, GH_NIP29_JOIN_LEAVING, 0, NULL);
  apply_leave_result(room, op);
  return op;
}

static gboolean
can_send_chat(GhNip29Service *self, GhNip29Room *room, GError **error)
{
  if (!check_room(self, room, error))
    return FALSE;
  if (room->join == GH_NIP29_JOIN_MEMBER || room->join == GH_NIP29_JOIN_PENDING ||
      room->join == GH_NIP29_JOIN_REQUESTING)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                      "Join the group before writing in it");
  return FALSE;
}

static GhNip29Op *
enqueue_chat(GhNip29Service *self, GhNip29Room *room, const gchar *chat,
             GError **error)
{
  GhNip29Op *op = room_enqueue(room, chat, error);
  if (!op)
    return NULL;
  g_autoptr(GError) echo_error = NULL;
  g_autoptr(GhMessage) echo = gh_message_new_from_nip29_event(
    self->account, room_relay(room), chat, &echo_error);
  if (!echo || gh_conversation_store_admit(self->conversations, echo, NULL, &echo_error) ==
                 GH_CONVERSATION_ADD_FAILED)
    g_warning("Groundhog could not list a sent group message: %s",
              echo_error ? echo_error->message : "unknown error");
  apply_message_status(self, op);
  return op;
}

GhNip29Op *
gh_nip29_service_send(GhNip29Service *self, GhNip29Room *room, const gchar *text,
                      GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!can_send_chat(self, room, error))
    return NULL;
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *chat = gh_nip29_template_chat(room->key, &context, text, error);
  return chat ? enqueue_chat(self, room, chat, error) : NULL;
}

GhNip29Op *
gh_nip29_service_create_poll(GhNip29Service *self, GhNip29Room *room,
                             const gchar *question, const gchar *const *options,
                             guint n_options, gboolean multiple, gint64 ends_at,
                             GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!can_send_chat(self, room, error))
    return NULL;
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *chat = gh_nip29_template_poll(room->key, &context, question, options,
                                                   n_options, multiple, ends_at, error);
  return chat ? enqueue_chat(self, room, chat, error) : NULL;
}

GhNip29Op *
gh_nip29_service_cast_poll_vote(GhNip29Service *self, GhNip29Room *room,
                                const gchar *poll_event_id,
                                const gchar *const *option_ids, guint n_options,
                                GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!can_send_chat(self, room, error))
    return NULL;
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *chat = gh_nip29_template_poll_vote(room->key, &context, poll_event_id,
                                                        option_ids, n_options, error);
  return chat ? enqueue_chat(self, room, chat, error) : NULL;
}

GhNip29Op *
gh_nip29_service_send_reaction(GhNip29Service *self, GhNip29Room *room,
                               const gchar *target_event_id,
                               const gchar *target_pubkey,
                               const gchar *target_kind_str,
                               const gchar *emoji,
                               GhReactionStore *reactions,
                               GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!check_room(self, room, error))
    return NULL;
  if (room->join != GH_NIP29_JOIN_MEMBER && room->join != GH_NIP29_JOIN_PENDING &&
      room->join != GH_NIP29_JOIN_REQUESTING) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "Join the group before reacting in it");
    return NULL;
  }
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *reaction_json = gh_nip29_template_reaction(
    room->key, &context, target_event_id, target_pubkey, target_kind_str, emoji, error);
  GhNip29Op *op = reaction_json ? room_enqueue(room, reaction_json, error) : NULL;
  if (!op)
    return NULL;
  /* Local echo: compute the event id from the template and admit a
   * GhReaction so the chip shows immediately. */
  if (reactions) {
    NostrEvent *event = nostr_event_new();
    if (event && nostr_event_deserialize_compact(event, reaction_json, NULL) == 1) {
      gchar id[65] = { 0 };
      if (nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK) {
        g_autoptr(GhReaction) local =
          gh_reaction_new(target_event_id, id,
                          context.author_pubkey, emoji, context.created_at,
                          room->room_id);
        if (local)
          gh_reaction_store_admit(reactions, local, NULL);
      }
    }
    if (event)
      nostr_event_free(event);
  }
  return op;
}

/* W26 slice B review fix (F2): kind-5 NIP-09 author deletion for a NIP-29
 * group. A regular member can delete their own event (reaction) without admin
 * permission. Uses room_enqueue (member-level), not admin_enqueue. */
GhNip29Op *
gh_nip29_service_send_deletion(GhNip29Service *self, GhNip29Room *room,
                               const gchar *event_id, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  if (!check_room(self, room, error))
    return NULL;
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *deletion_json = gh_nip29_template_deletion(
    room->key, &context, event_id, error);
  return deletion_json ? room_enqueue(room, deletion_json, error) : NULL;
}

static GhNip29Op *
admin_enqueue(GhNip29Service *self, GhNip29Room *room, nostr_permission_t permission,
              gchar *(*build)(GhNip29Room *room, const GhNip29TemplateContext *context,
                              gconstpointer data, GError **error),
              gconstpointer data, GError **error)
{
  if (!check_room(self, room, error))
    return NULL;
  GhNip29Authz authz = gh_nip29_room_check_permission(room, permission);
  switch (authz) {
  case GH_NIP29_AUTHZ_DENIED_INVALID:
  case GH_NIP29_AUTHZ_DENIED_NOT_ADMIN:
  case GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES:
  case GH_NIP29_AUTHZ_DENIED_BY_POLICY:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Not allowed in this group: %s", gh_nip29_authz_to_string(authz));
    return NULL;
  default:
    break; /* ALLOWED, or UNKNOWN: the relay decides (§7.10) */
  }
  GhNip29TemplateContext context;
  room_context(room, &context);
  g_autofree gchar *json = build(room, &context, data, error);
  return json ? room_enqueue(room, json, error) : NULL;
}

typedef struct {
  const gchar *pubkey;
  const gchar *const *roles;
  const gchar *reason;
  const GhNip29Metadata *metadata;
  const gchar *text;
} AdminArgs;

static gchar *
build_put_user(GhNip29Room *room, const GhNip29TemplateContext *context, gconstpointer data,
               GError **error)
{
  const AdminArgs *args = data;
  return gh_nip29_template_put_user(room->key, context, args->pubkey, args->roles, args->reason,
                                    error);
}

static gchar *
build_remove_user(GhNip29Room *room, const GhNip29TemplateContext *context, gconstpointer data,
                  GError **error)
{
  const AdminArgs *args = data;
  return gh_nip29_template_remove_user(room->key, context, args->pubkey, args->reason, error);
}

static gchar *
build_edit_metadata(GhNip29Room *room, const GhNip29TemplateContext *context,
                    gconstpointer data, GError **error)
{
  const AdminArgs *args = data;
  return gh_nip29_template_edit_metadata(room->key, context, args->metadata, args->reason, error);
}

static gchar *
build_delete_event(GhNip29Room *room, const GhNip29TemplateContext *context, gconstpointer data,
                   GError **error)
{
  const AdminArgs *args = data;
  return gh_nip29_template_delete_event(room->key, context, args->text, args->reason, error);
}

static gchar *
build_create_invite(GhNip29Room *room, const GhNip29TemplateContext *context,
                    gconstpointer data, GError **error)
{
  const AdminArgs *args = data;
  return gh_nip29_template_create_invite(room->key, context, args->text, args->reason, error);
}

GhNip29Op *
gh_nip29_service_put_user(GhNip29Service *self, GhNip29Room *room, const gchar *pubkey,
                          const gchar *const *roles, const gchar *reason, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  AdminArgs args = { .pubkey = pubkey, .roles = roles, .reason = reason };
  return admin_enqueue(self, room, NOSTR_PERMISSION_PUT_USER, build_put_user, &args, error);
}

GhNip29Op *
gh_nip29_service_remove_user(GhNip29Service *self, GhNip29Room *room, const gchar *pubkey,
                             const gchar *reason, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  AdminArgs args = { .pubkey = pubkey, .reason = reason };
  return admin_enqueue(self, room, NOSTR_PERMISSION_REMOVE_USER, build_remove_user, &args,
                       error);
}

GhNip29Op *
gh_nip29_service_edit_metadata(GhNip29Service *self, GhNip29Room *room,
                               const GhNip29Metadata *metadata, const gchar *reason,
                               GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  AdminArgs args = { .metadata = metadata, .reason = reason };
  return admin_enqueue(self, room, NOSTR_PERMISSION_EDIT_METADATA, build_edit_metadata, &args,
                       error);
}

GhNip29Op *
gh_nip29_service_delete_event(GhNip29Service *self, GhNip29Room *room, const gchar *event_id,
                              const gchar *reason, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  AdminArgs args = { .text = event_id, .reason = reason };
  return admin_enqueue(self, room, NOSTR_PERMISSION_DELETE_EVENT, build_delete_event, &args,
                       error);
}

GhNip29Op *
gh_nip29_service_create_invite(GhNip29Service *self, GhNip29Room *room, const gchar *code,
                               const gchar *reason, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), NULL);
  AdminArgs args = { .text = code, .reason = reason };
  return admin_enqueue(self, room, NOSTR_PERMISSION_CREATE_INVITE, build_create_invite, &args,
                       error);
}

void
gh_nip29_service_set_role_policy(GhNip29Service *self, const gchar *relay_url,
                                 GhNip29RolePolicy *policy)
{
  g_return_if_fail(GH_IS_NIP29_SERVICE(self));
  g_autofree gchar *url = gh_nip29_normalize_relay_url(relay_url, NULL);
  if (!url) {
    gh_nip29_role_policy_free(policy);
    return;
  }
  if (policy)
    g_hash_table_replace(self->role_policies, g_steal_pointer(&url), policy);
  else
    g_hash_table_remove(self->role_policies, url);
}

/* Historical REQs are separate from the live multi-room REQ. A page is
 * buffered until EOSE; its position is saved only after every admission.
 * Partial writes are idempotent on retry, and remain visible after failure. */
static gboolean history_start_page(GhNip29Room *room);

static gboolean
history_next_page(gpointer data)
{
  GhNip29Room *room = data;
  room->history_idle = 0;
  if (room->service && room->history_state == HISTORY_INCOMPLETE &&
      room->history_pages < GH_NIP29_SERVICE_MAX_PAGES && !history_start_page(room))
    history_refresh_availability(room);
  return G_SOURCE_REMOVE;
}

static gboolean
history_commit_page(GhNip29Room *room, HistoryState next_state, gint64 next_oldest)
{
  GhNip29Service *self = room->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *record = NULL;
  gint64 conversation_id = 0;
  gint64 old_oldest = room->history_oldest;
  HistoryState old_state = room->history_state;
  room_save_flush(room);
  for (guint i = 0; i < room->history_events->len; i++) {
    GhMessage *message = g_ptr_array_index(room->history_events, i);
    GhConversationAddResult added = gh_conversation_store_admit(
      self->conversations, message, gh_message_get_rumor_id(message), &error);
    if (added == GH_CONVERSATION_ADD_FAILED || added == GH_CONVERSATION_ADD_REJECTED) {
      if (!error)
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Group history message was rejected");
      goto fail;
    }
  }
  room->history_oldest = next_oldest;
  room->history_state = next_state;
  record = room_record(room);
  if (!gh_store_nip29_save_group(self->store, room_relay(room), room_group_id(room),
                                 room->relay_pubkey, record, room->name, &conversation_id,
                                 &error)) {
    room->history_oldest = old_oldest;
    room->history_state = old_state;
    goto fail;
  }
  room->conversation_id = conversation_id;
  return TRUE;
fail:
  g_warning("Groundhog could not commit a group history page: %s",
            error ? error->message : "unknown error");
  return FALSE;
}

static void
on_history_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhNip29Room *room = data;
  if (room->history_scope != scope || !room->service)
    return;
  if (update->notice == GH_RELAY_NOTICE_EVENT && update->stored) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GhMessage) message = gh_message_new_from_nip29_event(
      room->service->account, room_relay(room), update->event_json, &error);
    if (message && g_strcmp0(gh_message_get_room_id(message), room->room_id) == 0) {
      gint64 at = gh_message_get_created_at(message);
      room->history_page_oldest = MIN(room->history_page_oldest, at);
      room->history_page_newest = MAX(room->history_page_newest, at);
      room->history_page_count++;
      g_ptr_array_add(room->history_events, g_steal_pointer(&message));
    }
    return;
  }
  if (update->notice == GH_RELAY_NOTICE_CLOSED ||
      update->notice == GH_RELAY_NOTICE_DISCONNECTED ||
      update->notice == GH_RELAY_NOTICE_ERROR) {
    history_close(room); /* no EOSE: retry from the prior committed position */
    history_refresh_availability(room);
    return;
  }
  if (update->notice != GH_RELAY_NOTICE_EOSE)
    return;
  if (update->incomplete) {
    history_close(room); /* a truncated answer cannot move the position */
    history_refresh_availability(room);
    return;
  }
  gint64 requested = room->history_requested_until;
  gint64 oldest = room->history_page_oldest;
  guint count = room->history_page_count;
  HistoryState next_state = HISTORY_INCOMPLETE;
  gint64 next_oldest = room->history_oldest;
  if (count == 0 || (oldest >= requested && count < gh_relay_page_threshold(
                                                       GH_NIP29_SERVICE_PAGE_LIMIT))) {
    next_state = HISTORY_COMPLETE;
  } else if (oldest >= requested ||
             (count >= GH_NIP29_SERVICE_PAGE_LIMIT && oldest == room->history_page_newest)) {
    next_state = HISTORY_PARTIAL; /* a capped same-second page cannot advance safely */
  } else {
    next_oldest = oldest;
  }
  gboolean committed = history_commit_page(room, next_state, next_oldest);
  history_close(room);
  if (!committed)
    return;
  if (room->history_state == HISTORY_INCOMPLETE &&
      room->history_pages < GH_NIP29_SERVICE_MAX_PAGES)
    room->history_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, history_next_page,
                                         g_object_ref(room), g_object_unref);
  history_refresh_availability(room);
}

static gboolean
history_start_page(GhNip29Room *room)
{
  GhNip29Service *self = room->service;
  if (!self || !running(self) || !join_subscribed(room->join) || room->history_scope)
    return FALSE;
  gint64 until = room->history_oldest;
  if (until <= 0) {
    GhConversation *conversation =
      gh_conversation_store_lookup(self->conversations, room->room_id);
    guint n = conversation ? g_list_model_get_n_items(G_LIST_MODEL(conversation)) : 0;
    if (n > 0) {
      g_autoptr(GhMessage) oldest = g_list_model_get_item(G_LIST_MODEL(conversation), 0);
      until = gh_message_get_created_at(oldest);
    }
  }
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE, NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY,
                  NOSTR_KIND_SIMPLE_GROUP_THREAD, NOSTR_KIND_SIMPLE_GROUP_REPLY };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_tags_append(filter, "h", room_group_id(room), NULL);
  if (until > 0)
    nostr_filter_set_until_i64(filter, until);
  nostr_filter_set_limit(filter, GH_NIP29_SERVICE_PAGE_LIMIT);
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  GhRelayScope *scope = self->custom_scope
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->scope_transport,
                                        self->scope_data, on_history_scope_update, room)
    : gh_relay_scope_new(self->generation, filters, on_history_scope_update, room);
  if (self->custom_scope && self->has_scope_auth)
    gh_relay_scope_set_auth_transport(scope, &self->scope_auth);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_scope_add_url(scope, room_relay(room), &error)) {
    gh_relay_scope_unref(scope);
    g_warning("Groundhog cannot read group history: %s", error->message);
    return FALSE;
  }
  if (!gh_auth_policy_apply_scope(self->policy, scope, GH_AUTH_PURPOSE_GROUP,
                                  room_relay(room), &error))
    g_clear_error(&error);
  room->history_scope = scope;
  room->history_events = g_ptr_array_new_with_free_func(g_object_unref);
  room->history_requested_until = until > 0 ? until : G_MAXINT64;
  room->history_page_oldest = G_MAXINT64;
  room->history_page_newest = 0;
  room->history_page_count = 0;
  room->history_pages++;
  history_refresh_availability(room);
  gh_relay_scope_start(scope);
  return TRUE;
}

gboolean
gh_nip29_service_load_older(GhNip29Service *self, GhConversation *conversation, guint limit,
                            guint *out_loaded, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), FALSE);
  guint loaded = 0;
  if (!gh_store_nip29_load_older(self->rooms_store, conversation, limit, &loaded, error))
    return FALSE;
  if (out_loaded)
    *out_loaded = loaded;
  if (loaded > 0 || gh_conversation_get_floor(conversation, NULL, NULL))
    return TRUE;
  GhNip29Room *room = g_hash_table_lookup(self->by_room_id,
                                          gh_conversation_get_room_id(conversation));
  if (!room || room->history_state != HISTORY_INCOMPLETE)
    return TRUE;
  if (room->history_scope || room->history_idle)
    return TRUE;
  if (!running(self)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "The group relay is offline");
    return FALSE;
  }
  room->history_pages = 0;
  return history_start_page(room);
}

gboolean
gh_nip29_service_forget(GhNip29Service *self, GhNip29Room *room, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(self), FALSE);
  if (!GH_IS_NIP29_ROOM(room) || room->service != self || join_subscribed(room->join)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Leave the group before forgetting it");
    return FALSE;
  }
  if (room->save_idle) {
    g_source_remove(room->save_idle);
    room->save_idle = 0;
  }
  gint64 id = room->conversation_id;
  if (id > 0 && (!gh_store_nip29_delete_group(self->store, id, error) ||
                 !gh_store_forget_conversation(self->store, id, error)))
    return FALSE;
  gh_conversation_store_remove(self->conversations, room->room_id);
  gboolean was_reading = room->awaiting_state;
  room->awaiting_state = FALSE;
  g_autofree gchar *url = g_strdup(room_relay(room));
  guint position = 0;
  if (g_ptr_array_find(self->rooms, room, &position)) {
    g_hash_table_remove(self->by_key, room->key);
    g_hash_table_remove(self->by_room_id, room->room_id);
    room->service = NULL;
    g_ptr_array_remove_index(self->rooms, position);
    g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
  }
  /* Its state is no longer asked for. */
  Relay *relay = was_reading ? g_hash_table_lookup(self->relays, url) : NULL;
  if (relay)
    relay_resubscribe(relay);
  return TRUE;
}

/* ---- GhNip29Room ----------------------------------------------------------------------- */

const gchar *
gh_nip29_room_get_relay_url(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return room_relay(self);
}

const gchar *
gh_nip29_room_get_group_id(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return room_group_id(self);
}

const gchar *
gh_nip29_room_get_room_id(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return self->room_id;
}

const gchar *
gh_nip29_room_get_name(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return self->name;
}

GhNip29JoinState
gh_nip29_room_get_join_state(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), GH_NIP29_JOIN_NONE);
  return self->join;
}

GhNip29ReadState
gh_nip29_room_get_read_state(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), GH_NIP29_READ_IDLE);
  return self->read;
}

GhNip29RelayKeyState
gh_nip29_room_get_relay_key_state(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), GH_NIP29_RELAY_KEY_UNKNOWN);
  return self->key_state;
}

GhNip29MemberList
gh_nip29_room_get_members_state(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), GH_NIP29_MEMBERS_UNAVAILABLE);
  return self->members;
}

gboolean
gh_nip29_room_get_is_closed(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), FALSE);
  return self->closed;
}

gboolean
gh_nip29_room_get_is_private(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), FALSE);
  return self->private_;
}

gboolean
gh_nip29_room_get_is_restricted(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), FALSE);
  return self->restricted;
}

const gchar *
gh_nip29_room_get_detail(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return self->detail;
}

const GhNip29Group *
gh_nip29_room_get_group(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  return self->group;
}

GhNip29Op *
gh_nip29_room_dup_request_op(GhNip29Room *self)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), NULL);
  gint64 id = self->join == GH_NIP29_JOIN_CREATING || (self->create_op && !self->join_op)
                ? self->create_op : self->join_op;
  if (!self->service || !self->service->outbox || id <= 0)
    return NULL;
  return gh_nip29_outbox_lookup(self->service->outbox, id);
}

GhNip29Authz
gh_nip29_room_check_permission(GhNip29Room *self, nostr_permission_t permission)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(self), GH_NIP29_AUTHZ_DENIED_INVALID);
  if (!self->group || !self->service)
    return GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS;
  const GhNip29RolePolicy *policy = g_hash_table_lookup(self->service->role_policies,
                                                        room_relay(self));
  return gh_nip29_group_check_permission(self->group, policy, self->service->account,
                                         permission);
}

static void
gh_nip29_room_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GhNip29Room *self = GH_NIP29_ROOM(object);
  switch (prop_id) {
  case ROOM_PROP_RELAY_URL:
    g_value_set_string(value, room_relay(self));
    break;
  case ROOM_PROP_GROUP_ID:
    g_value_set_string(value, room_group_id(self));
    break;
  case ROOM_PROP_ROOM_ID:
    g_value_set_string(value, self->room_id);
    break;
  case ROOM_PROP_NAME:
    g_value_set_string(value, self->name);
    break;
  case ROOM_PROP_JOIN_STATE:
    g_value_set_enum(value, self->join);
    break;
  case ROOM_PROP_READ_STATE:
    g_value_set_enum(value, self->read);
    break;
  case ROOM_PROP_RELAY_KEY_STATE:
    g_value_set_enum(value, self->key_state);
    break;
  case ROOM_PROP_MEMBERS_STATE:
    g_value_set_int(value, self->members);
    break;
  case ROOM_PROP_IS_CLOSED:
    g_value_set_boolean(value, self->closed);
    break;
  case ROOM_PROP_IS_PRIVATE:
    g_value_set_boolean(value, self->private_);
    break;
  case ROOM_PROP_IS_RESTRICTED:
    g_value_set_boolean(value, self->restricted);
    break;
  case ROOM_PROP_DETAIL:
    g_value_set_string(value, self->detail);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_nip29_room_finalize(GObject *object)
{
  GhNip29Room *self = GH_NIP29_ROOM(object);
  g_assert(self->save_idle == 0);
  gh_nip29_group_key_free(self->key);
  g_free(self->room_id);
  g_free(self->relay_pubkey);
  g_clear_pointer(&self->group, gh_nip29_group_free);
  for (guint i = 0; i < SNAPSHOTS; i++) {
    g_free(self->snapshots[i]);
    g_free(self->held[i]);
  }
  gh_nip29_timeline_free(self->timeline);
  g_free(self->detail);
  g_free(self->name);
  g_free(self->create_meta);
  G_OBJECT_CLASS(gh_nip29_room_parent_class)->finalize(object);
}

static void
gh_nip29_room_class_init(GhNip29RoomClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_nip29_room_get_property;
  object_class->finalize = gh_nip29_room_finalize;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
  const GParamFlags notified = ro | G_PARAM_EXPLICIT_NOTIFY;
  room_props[ROOM_PROP_RELAY_URL] = g_param_spec_string("relay-url", NULL, NULL, NULL, ro);
  room_props[ROOM_PROP_GROUP_ID] = g_param_spec_string("group-id", NULL, NULL, NULL, ro);
  room_props[ROOM_PROP_ROOM_ID] = g_param_spec_string("room-id", NULL, NULL, NULL, ro);
  room_props[ROOM_PROP_NAME] = g_param_spec_string("name", NULL, NULL, NULL, notified);
  room_props[ROOM_PROP_JOIN_STATE] = g_param_spec_enum("join-state", NULL, NULL,
    GH_TYPE_NIP29_JOIN_STATE, GH_NIP29_JOIN_NONE, notified);
  room_props[ROOM_PROP_READ_STATE] = g_param_spec_enum("read-state", NULL, NULL,
    GH_TYPE_NIP29_READ_STATE, GH_NIP29_READ_IDLE, notified);
  room_props[ROOM_PROP_RELAY_KEY_STATE] = g_param_spec_enum("relay-key-state", NULL, NULL,
    GH_TYPE_NIP29_RELAY_KEY_STATE, GH_NIP29_RELAY_KEY_UNKNOWN, notified);
  room_props[ROOM_PROP_MEMBERS_STATE] = g_param_spec_int("members-state", NULL, NULL,
    GH_NIP29_MEMBERS_UNAVAILABLE, GH_NIP29_MEMBERS_PARTIAL, GH_NIP29_MEMBERS_UNAVAILABLE,
    notified);
  room_props[ROOM_PROP_IS_CLOSED] = g_param_spec_boolean("is-closed", NULL, NULL, FALSE,
                                                         notified);
  room_props[ROOM_PROP_IS_PRIVATE] = g_param_spec_boolean("is-private", NULL, NULL, FALSE,
                                                          notified);
  room_props[ROOM_PROP_IS_RESTRICTED] = g_param_spec_boolean("is-restricted", NULL, NULL, FALSE,
                                                             notified);
  room_props[ROOM_PROP_DETAIL] = g_param_spec_string("detail", NULL, NULL, NULL, notified);
  g_object_class_install_properties(object_class, ROOM_N_PROPS, room_props);
  room_signals[ROOM_SIGNAL_GROUP_CHANGED] = g_signal_new("group-changed",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_nip29_room_init(GhNip29Room *self)
{
  (void)self;
}

/* ---- GhNip29Service: GListModel and GObject ------------------------------------------------ */

static GType
list_get_item_type(GListModel *model)
{
  (void)model;
  return GH_TYPE_NIP29_ROOM;
}

static guint
list_get_n_items(GListModel *model)
{
  return GH_NIP29_SERVICE(model)->rooms->len;
}

static gpointer
list_get_item(GListModel *model, guint position)
{
  GhNip29Service *self = GH_NIP29_SERVICE(model);
  return position < self->rooms->len ? g_object_ref(g_ptr_array_index(self->rooms, position))
                                     : NULL;
}

static void
gh_nip29_service_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_n_items = list_get_n_items;
  iface->get_item = list_get_item;
}

static void
gh_nip29_service_dispose(GObject *object)
{
  GhNip29Service *self = GH_NIP29_SERVICE(object);
  if (self->accounts_handler)
    g_clear_signal_handler(&self->accounts_handler, self->accounts);
  if (self->network_handler)
    g_clear_signal_handler(&self->network_handler, self->network);
  if (self->relays)
    g_hash_table_remove_all(self->relays); /* cancels every REQ and key fetch */
  for (guint i = 0; self->rooms && i < self->rooms->len; i++) {
    GhNip29Room *room = g_ptr_array_index(self->rooms, i);
    history_close(room);
    room_save_flush(room); /* the store is still open (see the header) */
    room->service = NULL;
  }
  if (self->outbox) {
    if (self->op_handler)
      g_clear_signal_handler(&self->op_handler, self->outbox);
    g_object_run_dispose(G_OBJECT(self->outbox));
    g_clear_object(&self->outbox);
  }
  if (self->rooms_store) {
    gh_store_nip29_close(self->rooms_store);
    g_clear_object(&self->rooms_store);
  }
  self->store = NULL;
  g_clear_object(&self->accounts);
  g_clear_object(&self->policy);
  g_clear_object(&self->network);
  g_clear_object(&self->http);
  g_clear_object(&self->conversations);
  g_clear_object(&self->reactions);
  G_OBJECT_CLASS(gh_nip29_service_parent_class)->dispose(object);
}

static void
gh_nip29_service_finalize(GObject *object)
{
  GhNip29Service *self = GH_NIP29_SERVICE(object);
  g_hash_table_unref(self->relays);
  g_hash_table_unref(self->by_key);
  g_hash_table_unref(self->by_room_id);
  g_hash_table_unref(self->role_policies);
  g_ptr_array_unref(self->rooms);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  G_OBJECT_CLASS(gh_nip29_service_parent_class)->finalize(object);
}

static void
gh_nip29_service_class_init(GhNip29ServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_nip29_service_dispose;
  object_class->finalize = gh_nip29_service_finalize;
}

static void
gh_nip29_service_init(GhNip29Service *self)
{
  self->rooms = g_ptr_array_new_with_free_func(g_object_unref);
  self->by_key = g_hash_table_new(gh_nip29_group_key_hash, gh_nip29_group_key_equal);
  self->by_room_id = g_hash_table_new(g_str_hash, g_str_equal);
  self->relays = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, relay_free);
  self->role_policies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify)gh_nip29_role_policy_free);
}
