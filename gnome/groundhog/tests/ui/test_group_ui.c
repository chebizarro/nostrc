/* NIP-29 relay-group UI (charter §7.5, §7.7, §7.9, §7.10, §7.15 #18; §8.2
 * G20b; qp24.12 acceptance in the UI).
 *
 * Default mode (no display): the group UI's words and decisions
 * (gh-group-copy.h) against a local NIP-29 relay (tests/nip29/nip29-relay.h)
 * with the real GhNip29Service, its durable outbox, a real SQLCipher store
 * and a real account controller with the mock org.nostr.Signer on the
 * private test bus: group references; join (pending review then admission,
 * declined, invite only then an invite code, already a member both ways);
 * sending into a group, its honest status and delivery outcome, the relay's
 * refusal, Retry, and the "closed to you" reasons; leave; admin gating from
 * the relay-signed 39001/39003 (hidden, allowed, "the relay decides") and
 * the service refusing what is denied; creating a group (accepted and named,
 * or refused and dropped); older history from the store after a restart.
 *
 * --gui: the dialogs on a real window (Join Group through each answer, Group
 * Info for an admin and a member with Leave and Remove, New Group accepted
 * and refused), the group row's glyph and accessible label, and the window
 * glue (win.join-group, the composer delegate, Group Info). It exits 77
 * without a display. With GROUNDHOG_TEST_SCREENSHOTS=<dir> it also renders
 * the dialogs to <dir>/groundhog-g20b-*.png.
 *
 * Waits iterate the main context; their deadlines are failure bounds only. */
#include "gh-conversation-list.h"
#include "gh-conversation-row.h"
#include "gh-group-copy.h"
#include "gh-group-info-dialog.h"
#include "gh-group-join-dialog.h"
#include "gh-group-ui.h"
#include "gh-new-group-dialog.h"
#include "gh-shell.h"
#include "gh-store-nip29.h"
#include "gh-test-signer.h"
#include "group-send-stub.h"
#include "nip29-relay.h"

#include <glib/gstdio.h>
#include <nostr/nip19/nip19.h>

extern void groundhog_register_resource(void);

#define STORE_ID "5b1d7f00-29b0-4c1a-9e2b-6a7c8d9e0f1a"
#define KEY_ALICE 1
#define KEY_BOB   2
#define KEY_CAROL 3

static gchar *hex_alice, *hex_bob, *hex_carol, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

/* ---- helpers --------------------------------------------------------------- */

static void
drain(void)
{
  for (int i = 0; i < 500 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static void
rm_rf(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

/* ---- fake GNetworkMonitor ------------------------------------------------------ */

#define FAKE_TYPE_MONITOR (fake_monitor_get_type())
G_DECLARE_FINAL_TYPE(FakeMonitor, fake_monitor, FAKE, MONITOR, GObject)

struct _FakeMonitor {
  GObject parent_instance;
};

enum { MONITOR_PROP_0, MONITOR_PROP_AVAILABLE, MONITOR_PROP_METERED, MONITOR_PROP_CONNECTIVITY };

static gboolean
fake_monitor_initable_init(GInitable *initable, GCancellable *cancellable, GError **error)
{
  (void)initable; (void)cancellable; (void)error;
  return TRUE;
}

static void
fake_monitor_initable_iface_init(GInitableIface *iface)
{
  iface->init = fake_monitor_initable_init;
}

static gboolean
fake_monitor_can_reach(GNetworkMonitor *monitor, GSocketConnectable *connectable,
                       GCancellable *cancellable, GError **error)
{
  (void)monitor; (void)connectable; (void)cancellable; (void)error;
  return TRUE;
}

static void
fake_monitor_iface_init(GNetworkMonitorInterface *iface)
{
  iface->can_reach = fake_monitor_can_reach;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(FakeMonitor, fake_monitor, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE, fake_monitor_initable_iface_init)
  G_IMPLEMENT_INTERFACE(G_TYPE_NETWORK_MONITOR, fake_monitor_iface_init))

static void
fake_monitor_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  switch (prop_id) {
  case MONITOR_PROP_AVAILABLE: g_value_set_boolean(value, TRUE); break;
  case MONITOR_PROP_METERED: g_value_set_boolean(value, FALSE); break;
  case MONITOR_PROP_CONNECTIVITY: g_value_set_enum(value, G_NETWORK_CONNECTIVITY_FULL); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
fake_monitor_class_init(FakeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = fake_monitor_get_property;
  g_object_class_override_property(object_class, MONITOR_PROP_AVAILABLE, "network-available");
  g_object_class_override_property(object_class, MONITOR_PROP_METERED, "network-metered");
  g_object_class_override_property(object_class, MONITOR_PROP_CONNECTIVITY, "connectivity");
}

static void
fake_monitor_init(FakeMonitor *self)
{
  (void)self;
}

/* ---- fixture ------------------------------------------------------------------ */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhClock *clock;
  gchar *data_dir;
  GhStore *store;
  GhConversationStore *model;
  FakeMonitor *network;
  GhNip29Service *service;
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  const gchar *npubs[] = { npub_alice, npub_bob };
  for (guint i = 0; i < G_N_ELEMENTS(npubs); i++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npubs[i]);
    info->label = g_strdup(i ? "Bob" : "Alice");
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GhStore *
store_open(const gchar *data_dir, GhClock *clock)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8)(0x2b ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { data_dir, hex_alice, NULL, NULL, clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

static void
service_up(Fixture *f)
{
  GhNip29ServiceConfig config = {
    .store = f->store,
    .accounts = f->accounts,
    .conversations = f->model,
    .network = G_NETWORK_MONITOR(f->network),
    .publish_deadline = 10,
    .settings = f->settings,
  };
  g_autoptr(GError) error = NULL;
  f->service = gh_nip29_service_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->service);
}

static void
model_up(Fixture *f)
{
  f->model = gh_conversation_store_new();
  gh_conversation_store_set_account(f->model, hex_alice, NULL, NULL, NULL);
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->clock = gh_clock_new_fake(g_get_real_time());
  f->data_dir = g_dir_make_tmp("groundhog-group-ui-XXXXXX", NULL);
  f->store = store_open(f->data_dir, f->clock);
  f->network = g_object_new(FAKE_TYPE_MONITOR, NULL);
  model_up(f);
  service_up(f);
}

static void
service_down(Fixture *f)
{
  if (f->service)
    gh_test_release(g_steal_pointer(&f->service));
}

static void
restart(Fixture *f)
{
  service_down(f);
  drain();
  gh_store_close(f->store);
  g_clear_object(&f->model);
  f->store = store_open(f->data_dir, f->clock);
  model_up(f);
  service_up(f);
}

static void
fixture_down(Fixture *f)
{
  service_down(f);
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_clear_object(&f->model);
  g_object_unref(f->network);
  g_object_unref(f->settings);
  gh_store_close(f->store);
  gh_clock_unref(f->clock);
  gh_test_signer_down(&shared_bus, &f->mock);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
}

/* ---- waits ---------------------------------------------------------------------- */

typedef struct {
  GhNip29Room *room;
  gint value;
} RoomWait;

static gboolean
join_is(gpointer data)
{
  RoomWait *wait = data;
  return (gint)gh_nip29_room_get_join_state(wait->room) == wait->value;
}

static gboolean
read_is(gpointer data)
{
  RoomWait *wait = data;
  return (gint)gh_nip29_room_get_read_state(wait->room) == wait->value;
}

#define wait_join(room, state) \
  G_STMT_START { RoomWait w_ = { (room), (state) }; gh_test_spin_until(join_is, &w_); } G_STMT_END
#define wait_read(room, state) \
  G_STMT_START { RoomWait w_ = { (room), (state) }; gh_test_spin_until(read_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Room *room;
  const gchar *name;
} NameWait;

static gboolean
name_is(gpointer data)
{
  NameWait *wait = data;
  return g_strcmp0(gh_nip29_room_get_name(wait->room), wait->name) == 0;
}

#define wait_name(room, text) \
  G_STMT_START { NameWait w_ = { (room), (text) }; gh_test_spin_until(name_is, &w_); } G_STMT_END

static gboolean
restricted_known(gpointer data)
{
  return gh_nip29_room_get_is_restricted(data);
}

static gboolean
admins_known(gpointer data)
{
  const GhNip29Group *group = gh_nip29_room_get_group(data);
  return group && gh_nip29_group_get_snapshot_id(group, 39001, NULL) &&
         gh_nip29_group_get_snapshot_id(group, 39003, NULL);
}

typedef struct {
  GhNip29Op *op;
  GhNip29OpResult result;
} OpWait;

static gboolean
op_is(gpointer data)
{
  OpWait *wait = data;
  return gh_nip29_op_get_result(wait->op) == wait->result;
}

#define wait_op(op, value) \
  G_STMT_START { OpWait w_ = { (op), (value) }; gh_test_spin_until(op_is, &w_); } G_STMT_END

typedef struct {
  GhNip29Room *room;
  const gchar *pubkey;
} MemberWait;

static gboolean
member_listed(gpointer data)
{
  MemberWait *wait = data;
  const GhNip29Group *group = gh_nip29_room_get_group(wait->room);
  return group && gh_nip29_group_lookup_member(group, wait->pubkey) == GH_NIP29_MEMBERSHIP_LISTED;
}

typedef struct {
  GhConversationStore *model;
  const gchar *room_id;
  guint n;
} MessagesWait;

static gboolean
messages_reached(gpointer data)
{
  MessagesWait *wait = data;
  GhConversation *conversation = gh_conversation_store_lookup(wait->model, wait->room_id);
  return conversation && g_list_model_get_n_items(G_LIST_MODEL(conversation)) >= wait->n;
}

#define wait_messages(model, room_id, count) \
  G_STMT_START { MessagesWait w_ = { (model), (room_id), (count) }; \
                 gh_test_spin_until(messages_reached, &w_); } G_STMT_END

typedef struct {
  GhNip29Service *service;
  const gchar *relay;
  const gchar *group;
} GoneWait;

static gboolean
room_gone(gpointer data)
{
  GoneWait *wait = data;
  g_autoptr(GhNip29Room) room = gh_nip29_service_lookup(wait->service, wait->relay, wait->group);
  return room == NULL;
}

/* ---- relay setup ---------------------------------------------------------------- */

static gchar *
reference(Nip29Relay *relay, const gchar *group_id, const gchar *suffix)
{
  return g_strdup_printf("%s'%s%s", relay->url, group_id, suffix ? suffix : "");
}

/* Joins through what the Join dialog does: the pasted reference, parsed. */
static GhNip29Room *
join_ref(Fixture *f, const gchar *text, const gchar *typed_code)
{
  g_autofree gchar *relay = NULL, *group_id = NULL, *code = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_group_parse_reference(text, &relay, &group_id, &code, &error));
  g_assert_no_error(error);
  GhNip29Room *room = gh_nip29_service_join(f->service, relay, group_id, NULL,
                                            typed_code ? typed_code : code, &error);
  g_assert_no_error(error);
  g_assert_nonnull(room);
  return room;
}

static GhGroupStateCopy *
join_copy(GhNip29Room *room, gboolean already_member)
{
  g_autoptr(GhNip29Op) request = gh_nip29_room_dup_request_op(room);
  g_autofree gchar *host = gh_group_relay_host(gh_nip29_room_get_relay_url(room));
  return gh_group_join_copy(gh_nip29_room_get_join_state(room), request, already_member,
                            gh_nip29_room_get_detail(room), host);
}

static NostrEvent *
post_at(Nip29Relay *relay, guint key, const gchar *group_id, const gchar *text, gint64 at)
{
  NostrEvent *event = nip29_member_event(gh_test_secret[key], 9, at, group_id, text);
  nip29_store(relay, event);
  return event; /* owned by the relay */
}

/* ==== default mode: the decisions against a local relay ===================================== */

/* What people paste resolves offline to (relay, group, code), wss:// only. */
static void
test_references(void)
{
  g_autofree gchar *relay = NULL, *group = NULL, *code = NULL;
  g_autoptr(GError) error = NULL;

  /* NIP-29's own form: a bare host means wss://. */
  g_assert_true(gh_group_parse_reference("  groups.example.com'book-club  ", &relay, &group,
                                         &code, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(relay, ==, "wss://groups.example.com");
  g_assert_cmpstr(group, ==, "book-club");
  g_assert_null(code);
  g_clear_pointer(&relay, g_free);
  g_clear_pointer(&group, g_free);

  /* A shared invite link carries its code; the default port is dropped. */
  g_assert_true(gh_group_parse_reference("wss://Groups.Example.com:443'vip?invite=let%20me",
                                         &relay, &group, &code, &error));
  g_assert_cmpstr(relay, ==, "wss://groups.example.com");
  g_assert_cmpstr(group, ==, "vip");
  g_assert_cmpstr(code, ==, "let me");
  g_autofree gchar *shared = gh_group_format_address(relay, group, code);
  g_assert_cmpstr(shared, ==, "groups.example.com'vip?invite=let%20me");
  g_clear_pointer(&relay, g_free);
  g_clear_pointer(&group, g_free);
  g_clear_pointer(&code, g_free);

  /* A NIP-29 naddr (kind 39000 with the relay hint), as nostr: link too. */
  NostrEntityPointer pointer = { 0 };
  gchar *relays[] = { "wss://groups.example.com", NULL };
  pointer.public_key = hex_carol;
  pointer.kind = 39000;
  pointer.identifier = "naddr-group";
  pointer.relays = relays;
  pointer.relays_count = 1;
  char *naddr = NULL;
  g_assert_cmpint(nostr_nip19_encode_naddr(&pointer, &naddr), ==, 0);
  g_autofree gchar *link = g_strconcat("nostr:", naddr, NULL);
  free(naddr);
  g_assert_true(gh_group_parse_reference(link, &relay, &group, &code, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(relay, ==, "wss://groups.example.com");
  g_assert_cmpstr(group, ==, "naddr-group");
  g_clear_pointer(&relay, g_free);
  g_clear_pointer(&group, g_free);

  /* Never plaintext off loopback; loopback test relays are fine. */
  g_assert_false(gh_group_parse_reference("ws://groups.example.com'abc", &relay, &group, &code,
                                          &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull(strstr(error->message, "secure connection"));
  g_clear_error(&error);
  g_assert_true(gh_group_parse_reference("ws://127.0.0.1:7447'abc", &relay, &group, &code,
                                         &error));
  g_assert_cmpstr(relay, ==, "ws://127.0.0.1:7447");
  g_autofree gchar *loopback = gh_group_format_address(relay, group, NULL);
  g_assert_cmpstr(loopback, ==, "ws://127.0.0.1:7447'abc");
  g_clear_pointer(&relay, g_free);
  g_clear_pointer(&group, g_free);

  /* A .onion service over ws:// parses (Tor encrypts it); the transport
   * allows it only in Tor mode (gh_net_relay_url_allowed). */
  g_assert_true(gh_group_parse_reference("ws://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvw.onion'abc", &relay, &group,
                                         &code, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(relay, ==, "ws://abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvw.onion");
  g_clear_pointer(&relay, g_free);
  g_clear_pointer(&group, g_free);

  /* Not group addresses: said in words, nothing parsed. */
  const gchar *bad[] = { "", "   ", "npub1xyz", "groups.example.com", "host'Bad Id",
                         "https://example.com/'x", "host'a'b", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_assert_false(gh_group_parse_reference(bad[i], &relay, &group, &code, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null(relay);
    g_assert_null(group);
    g_clear_error(&error);
  }

  g_autofree gchar *host = gh_group_relay_host("wss://groups.example.com:8443/nip29");
  g_assert_cmpstr(host, ==, "groups.example.com:8443");
  g_autofree gchar *reason = gh_group_relay_reason("restricted: your join request is pending");
  g_assert_cmpstr(reason, ==, "Your join request is pending");
  g_assert_null(gh_group_relay_reason("blocked: "));
  g_assert_null(gh_group_relay_reason(NULL));
}

/* §7.10 "join states have distinct copy": pending review then admission,
 * declined, invite only then a code, already a member (both ways). */
static void
test_join_states(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *review = nip29_add_group(&relay, "review", "Reviewed");
  review->join_policy = NIP29_JOIN_PENDING;
  review->restricted = TRUE;
  nip29_publish_state(&relay, review);
  Nip29TestGroup *nope = nip29_add_group(&relay, "nope", "No Entry");
  nope->join_policy = NIP29_JOIN_DENY;
  Nip29TestGroup *vip = nip29_add_group(&relay, "vip", "VIP");
  vip->closed = TRUE;
  g_ptr_array_add(vip->invites, g_strdup("letmein"));
  nip29_publish_state(&relay, vip);
  Nip29TestGroup *known = nip29_add_group(&relay, "known", "Known");
  nip29_set_member(&relay, known, hex_alice, NULL);
  nip29_add_group(&relay, "open", "Open House");

  /* Pending review: said so, with the relay's words; a restricted group
   * is closed to writing until an admin admits the account. */
  g_autofree gchar *review_ref = reference(&relay, "review", NULL);
  g_autoptr(GhNip29Room) pending = join_ref(&f, review_ref, NULL);
  wait_join(pending, GH_NIP29_JOIN_PENDING);
  g_autoptr(GhGroupStateCopy) copy = join_copy(pending, FALSE);
  g_assert_cmpstr(copy->title, ==, "Waiting for an Admin");
  g_assert_cmpint(copy->tone, ==, GH_GROUP_TONE_WARNING);
  g_assert_nonnull(strstr(copy->description, "“Your join request is pending approval”"));
  g_assert_true(copy->can_open);
  gh_test_spin_until(restricted_known, pending);
  g_autofree gchar *reason = gh_group_room_send_reason(f.service,
                                                       gh_nip29_room_get_room_id(pending));
  g_assert_nonnull(strstr(reason, "Only members can write"));
  g_clear_pointer(&reason, g_free);
  /* Joined only on the relay's evidence: its 9000 for the account. */
  nip29_admit(&relay, review, hex_alice);
  wait_join(pending, GH_NIP29_JOIN_MEMBER);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(pending, FALSE);
  g_assert_cmpstr(copy->title, ==, "You're In");
  g_assert_nonnull(strstr(copy->description, "operators can read"));
  g_assert_null(gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(pending)));

  /* Declined for good: the relay's reason quoted; closed to writing. */
  g_autofree gchar *nope_ref = reference(&relay, "nope", NULL);
  g_autoptr(GhNip29Room) declined = join_ref(&f, nope_ref, NULL);
  wait_join(declined, GH_NIP29_JOIN_DENIED);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(declined, FALSE);
  g_assert_cmpstr(copy->title, ==, "Request Declined");
  g_assert_cmpint(copy->tone, ==, GH_GROUP_TONE_ERROR);
  g_assert_nonnull(strstr(copy->description, "“You may not join this group”"));
  reason = gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(declined));
  g_assert_nonnull(strstr(reason, "closed to you"));
  g_clear_pointer(&reason, g_free);

  /* Invite only: needs a code; with the link's code the relay admits. */
  g_autofree gchar *vip_ref = reference(&relay, "vip", NULL);
  g_autoptr(GhNip29Room) closed = join_ref(&f, vip_ref, NULL);
  wait_join(closed, GH_NIP29_JOIN_CLOSED);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(closed, FALSE);
  g_assert_cmpstr(copy->title, ==, "Invite Only");
  g_assert_true(copy->needs_code);
  reason = gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(closed));
  g_assert_nonnull(strstr(reason, "closed to you"));
  g_assert_nonnull(strstr(reason, "invite code"));
  g_clear_pointer(&reason, g_free);
  g_autofree gchar *invite_ref = reference(&relay, "vip", "?invite=letmein");
  g_autoptr(GhNip29Room) invited = join_ref(&f, invite_ref, NULL);
  g_assert_true(invited == closed); /* the same group, asked again */
  wait_join(invited, GH_NIP29_JOIN_MEMBER);
  NostrEvent *request = nip29_last_received(&relay, 9021);
  g_assert_cmpstr(nip29_tag_value(request, "code"), ==, "letmein");
  nostr_event_free(request);

  /* Already a member on the relay: its "duplicate:" answer says so. */
  g_autofree gchar *known_ref = reference(&relay, "known", NULL);
  g_autoptr(GhNip29Room) duplicate = join_ref(&f, known_ref, NULL);
  wait_join(duplicate, GH_NIP29_JOIN_MEMBER);
  /* Its 39002 may say "member" first; the answer to the request follows. */
  g_autoptr(GhNip29Op) answered = gh_nip29_room_dup_request_op(duplicate);
  wait_op(answered, GH_NIP29_OP_DUPLICATE);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(duplicate, FALSE);
  g_assert_cmpstr(copy->title, ==, "Already a Member");

  /* Already a member here: asking again sends nothing and says so. */
  g_autofree gchar *open_ref = reference(&relay, "open", NULL);
  g_autoptr(GhNip29Room) open = join_ref(&f, open_ref, NULL);
  wait_join(open, GH_NIP29_JOIN_MEMBER);
  guint seen = relay.events_seen;
  g_autoptr(GhNip29Room) again = join_ref(&f, open_ref, NULL);
  g_assert_true(again == open);
  drain();
  g_assert_cmpuint(relay.events_seen, ==, seen);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(again, TRUE);
  g_assert_cmpstr(copy->title, ==, "Already a Member");

  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* The composer's path into a group: listed at once, honest status and
 * outcome, the relay's refusal quoted, Retry, and the reasons. */
static void
test_send_and_retry(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  nip29_add_group(&relay, "open", "Open House");
  Nip29TestGroup *strict = nip29_add_group(&relay, "strict", "Members Only");
  strict->join_policy = NIP29_JOIN_PENDING;
  nip29_publish_state(&relay, strict);

  g_autofree gchar *open_ref = reference(&relay, "open", NULL);
  g_autoptr(GhNip29Room) room = join_ref(&f, open_ref, NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_read(room, GH_NIP29_READ_LIVE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) op = gh_nip29_service_send(f.service, room, "hello group", &error);
  g_assert_no_error(error);
  const gchar *event_id = gh_nip29_op_get_event_id(op);
  GhMessage *echo = gh_conversation_store_lookup_message(f.model, event_id);
  g_assert_nonnull(echo); /* listed before the relay answered */
  g_assert_true(gh_message_is_self(echo));
  wait_op(op, GH_NIP29_OP_ACCEPTED);
  g_assert_cmpint(gh_message_get_status(echo), ==, GH_MESSAGE_STATUS_SENT);
  g_autoptr(GhNip29Op) found = gh_group_find_message_op(f.service, event_id);
  g_assert_true(found == op);
  g_autofree gchar *outcome = gh_group_op_outcome(op);
  g_assert_nonnull(strstr(outcome, "accepted"));
  g_assert_nonnull(strstr(outcome, "can’t tell whether anyone read it"));
  NostrEvent *sent = nip29_last_received(&relay, 9);
  g_assert_cmpstr(nostr_event_get_content(sent), ==, "hello group");
  g_assert_cmpstr(nip29_tag_value(sent, "h"), ==, "open");
  nostr_event_free(sent);

  /* A pending member writing into a group that is not restricted: the
   * relay decides, and here refuses; its reason is quoted, and once the
   * account is admitted Retry sends the same event. */
  g_autofree gchar *strict_ref = reference(&relay, "strict", NULL);
  g_autoptr(GhNip29Room) waiting = join_ref(&f, strict_ref, NULL);
  wait_join(waiting, GH_NIP29_JOIN_PENDING);
  g_assert_null(gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(waiting)));
  g_autoptr(GhNip29Op) refused = gh_nip29_service_send(f.service, waiting, "let me in", &error);
  g_assert_no_error(error);
  wait_op(refused, GH_NIP29_OP_REJECTED);
  GhMessage *refused_echo = gh_conversation_store_lookup_message(
    f.model, gh_nip29_op_get_event_id(refused));
  g_assert_cmpint(gh_message_get_status(refused_echo), ==, GH_MESSAGE_STATUS_NOT_SENT);
  g_clear_pointer(&outcome, g_free);
  outcome = gh_group_op_outcome(refused);
  g_assert_nonnull(strstr(outcome, "refused it: “Only members can write”"));
  nip29_admit(&relay, strict, hex_alice);
  wait_join(waiting, GH_NIP29_JOIN_MEMBER);
  g_assert_true(gh_nip29_op_get_can_retry(refused));
  g_autoptr(GhNip29Op) retried = gh_group_find_message_op(f.service,
                                                          gh_nip29_op_get_event_id(refused));
  g_assert_true(gh_nip29_outbox_retry(gh_nip29_service_get_outbox(f.service), retried, &error));
  g_assert_no_error(error);
  wait_op(refused, GH_NIP29_OP_ACCEPTED);
  g_assert_cmpint(gh_message_get_status(refused_echo), ==, GH_MESSAGE_STATUS_SENT);

  /* Nothing to send to: no service, or a group not in the list. */
  g_autofree gchar *none = gh_group_room_send_reason(NULL, gh_nip29_room_get_room_id(room));
  g_assert_nonnull(strstr(none, "aren’t available"));
  g_autofree gchar *unknown_room = gh_message_nip29_room_id(relay.url, "elsewhere");
  g_autofree gchar *gone = gh_group_room_send_reason(f.service, unknown_room);
  g_assert_nonnull(strstr(gone, "isn’t in your list"));

  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* Leave (9022): the relay lets the account out; the copy and the composer
 * say so; the group can then be removed from the list. */
static void
test_leave(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *bye = nip29_add_group(&relay, "bye", "Goodbyes");
  Nip29TestGroup *strict = nip29_add_group(&relay, "strict", "Strict");
  g_autofree gchar *bye_ref = reference(&relay, "bye", NULL);
  g_autofree gchar *strict_ref = reference(&relay, "strict", NULL);
  g_autoptr(GhNip29Room) room = join_ref(&f, bye_ref, NULL);
  g_autoptr(GhNip29Room) kicked = join_ref(&f, strict_ref, NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_join(kicked, GH_NIP29_JOIN_MEMBER);
  wait_read(kicked, GH_NIP29_READ_LIVE);

  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) leave = gh_nip29_service_leave(f.service, room, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhGroupStateCopy) copy = join_copy(room, FALSE);
  g_assert_cmpstr(copy->title, ==, "Leaving…");
  wait_join(room, GH_NIP29_JOIN_LEFT);
  g_assert_false(g_hash_table_contains(bye->members, hex_alice));
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(room, FALSE);
  g_assert_cmpstr(copy->title, ==, "You Left This Group");
  g_autofree gchar *reason = gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(room));
  g_assert_nonnull(strstr(reason, "You left this group"));

  /* Removed by an admin: closed to the account, with the admin's reason. */
  nip29_remove(&relay, strict, hex_alice, "spam");
  wait_join(kicked, GH_NIP29_JOIN_REMOVED);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = join_copy(kicked, FALSE);
  g_assert_cmpstr(copy->title, ==, "Removed from Group");
  g_assert_nonnull(strstr(copy->description, "“Spam”"));
  g_clear_pointer(&reason, g_free);
  reason = gh_group_room_send_reason(f.service, gh_nip29_room_get_room_id(kicked));
  g_assert_nonnull(strstr(reason, "closed to you: a group admin removed you"));

  /* Remove from Conversations: the room and its conversation go. */
  g_autofree gchar *room_id = g_strdup(gh_nip29_room_get_room_id(room));
  g_assert_true(gh_nip29_service_forget(f.service, room, &error));
  g_assert_no_error(error);
  g_assert_null(gh_conversation_store_lookup(f.model, room_id));
  GoneWait gone = { f.service, relay.url, "bye" };
  g_assert_true(room_gone(&gone));

  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* §7.10 gating from the relay's 39001/39003: hidden for a member, "the
 * relay decides" for an admin whose role's rights are unknown, allowed
 * with a policy, denied by it; the service refuses what is denied locally
 * and the relay decides the rest. */
static void
test_admin_gating(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "Club");
  nip29_set_member(&relay, club, hex_alice, "admin");
  Nip29TestGroup *plain = nip29_add_group(&relay, "plain", "Plain");
  nip29_set_member(&relay, plain, hex_carol, "admin");
  Nip29TestGroup *mods = nip29_add_group(&relay, "mods", "Moderated");
  nip29_set_member(&relay, mods, hex_alice, "moderator");
  Nip29TestGroup *review = nip29_add_group(&relay, "review", "Reviewed");
  review->join_policy = NIP29_JOIN_PENDING;

  g_autofree gchar *club_ref = reference(&relay, "club", NULL);
  g_autofree gchar *plain_ref = reference(&relay, "plain", NULL);
  g_autofree gchar *mods_ref = reference(&relay, "mods", NULL);
  g_autofree gchar *review_ref = reference(&relay, "review", NULL);
  g_autoptr(GhNip29Room) admin = join_ref(&f, club_ref, NULL);
  g_autoptr(GhNip29Room) member = join_ref(&f, plain_ref, NULL);
  g_autoptr(GhNip29Room) moderator = join_ref(&f, mods_ref, NULL);
  g_autoptr(GhNip29Room) pending = join_ref(&f, review_ref, NULL);
  wait_join(admin, GH_NIP29_JOIN_MEMBER);
  wait_join(member, GH_NIP29_JOIN_MEMBER);
  wait_join(moderator, GH_NIP29_JOIN_MEMBER);
  wait_join(pending, GH_NIP29_JOIN_PENDING);
  gh_test_spin_until(admins_known, admin);
  gh_test_spin_until(admins_known, member);
  gh_test_spin_until(admins_known, moderator);
  gh_test_spin_until(admins_known, pending);

  /* No role policy: an admin's rights are the relay's to decide. */
  g_assert_cmpint(gh_group_room_gate(admin, NOSTR_PERMISSION_EDIT_METADATA), ==,
                  GH_GROUP_GATE_RELAY_DECIDES);
  g_assert_cmpint(gh_group_room_gate(admin, NOSTR_PERMISSION_PUT_USER), ==,
                  GH_GROUP_GATE_RELAY_DECIDES);
  /* Not in the relay-signed admin list: hidden. */
  g_assert_cmpint(gh_group_room_gate(member, NOSTR_PERMISSION_EDIT_METADATA), ==,
                  GH_GROUP_GATE_HIDDEN);
  g_assert_cmpint(gh_group_room_gate(member, NOSTR_PERMISSION_REMOVE_USER), ==,
                  GH_GROUP_GATE_HIDDEN);
  /* Not (yet) a member: nothing to manage. */
  g_assert_cmpint(gh_group_room_gate(pending, NOSTR_PERMISSION_EDIT_METADATA), ==,
                  GH_GROUP_GATE_HIDDEN);
  g_assert_cmpint(gh_group_gate(GH_NIP29_JOIN_MEMBER, GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS), ==,
                  GH_GROUP_GATE_HIDDEN);

  /* The relay's roles: "admin" may do everything, "moderator" delete only. */
  GhNip29RolePolicy *policy = gh_nip29_role_policy_new();
  const nostr_permission_t all[] = { NOSTR_PERMISSION_PUT_USER, NOSTR_PERMISSION_REMOVE_USER,
                                     NOSTR_PERMISSION_EDIT_METADATA,
                                     NOSTR_PERMISSION_DELETE_EVENT,
                                     NOSTR_PERMISSION_CREATE_INVITE };
  const nostr_permission_t delete_only[] = { NOSTR_PERMISSION_DELETE_EVENT };
  g_assert_true(gh_nip29_role_policy_set_role(policy, "admin", all, G_N_ELEMENTS(all), NULL));
  g_assert_true(gh_nip29_role_policy_set_role(policy, "moderator", delete_only, 1, NULL));
  gh_nip29_service_set_role_policy(f.service, relay.url, policy);
  g_assert_cmpint(gh_group_room_gate(admin, NOSTR_PERMISSION_EDIT_METADATA), ==,
                  GH_GROUP_GATE_ALLOWED);
  g_assert_cmpint(gh_group_room_gate(moderator, NOSTR_PERMISSION_DELETE_EVENT), ==,
                  GH_GROUP_GATE_ALLOWED);
  g_assert_cmpint(gh_group_room_gate(moderator, NOSTR_PERMISSION_EDIT_METADATA), ==,
                  GH_GROUP_GATE_HIDDEN);

  /* What is hidden is refused locally too: nothing reaches the relay. */
  g_autoptr(GError) error = NULL;
  guint seen = relay.events_seen;
  g_assert_null(gh_nip29_service_put_user(f.service, member, hex_bob, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  g_assert_null(gh_nip29_service_create_invite(f.service, moderator, "nope", NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  drain();
  g_assert_cmpuint(relay.events_seen, ==, seen);

  /* What is allowed goes to the relay, and the relay's next snapshot is the
   * truth: Bob appears in the member list only once it says so. */
  g_autoptr(GhNip29Op) add = gh_nip29_service_put_user(f.service, admin, hex_bob, NULL, NULL,
                                                       &error);
  g_assert_no_error(error);
  wait_op(add, GH_NIP29_OP_ACCEPTED);
  MemberWait bob = { admin, hex_bob };
  gh_test_spin_until(member_listed, &bob);

  fixture_down(&f);
  nip29_relay_clear(&relay);
}

typedef struct {
  guint changes;
} Counter;

static void
count_change(GhNip29Room *room, Counter *counter)
{
  (void)room;
  counter->changes++;
}

/* New Group (relay part): a relay that lets people create groups makes the
 * account the admin and takes the chosen name; one that doesn't says so,
 * and the group is dropped. */
static void
test_create_group(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  relay.allow_create = TRUE;
  Nip29Relay strict;
  nip29_relay_init(&strict);

  g_autoptr(GhNip29Metadata) metadata = gh_nip29_metadata_new();
  metadata->name = g_strdup("Book Club");
  metadata->about = g_strdup("One chapter a week");
  metadata->is_closed = TRUE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Room) room = gh_nip29_service_create_group(f.service, relay.url, NULL,
                                                              metadata, &error);
  g_assert_no_error(error);
  Counter counter = { 0 };
  g_signal_connect(room, "group-changed", G_CALLBACK(count_change), &counter);
  g_autofree gchar *group_id = g_strdup(gh_nip29_room_get_group_id(room));
  g_assert_cmpuint(strlen(group_id), ==, 16);
  g_assert_cmpint(gh_nip29_room_get_join_state(room), ==, GH_NIP29_JOIN_CREATING);
  g_autoptr(GhNip29Op) request = gh_nip29_room_dup_request_op(room);
  g_assert_nonnull(request);
  g_assert_cmpint(gh_nip29_op_get_kind(request), ==, 9007);
  /* Not listed before the relay created it. */
  g_assert_null(gh_conversation_store_lookup(f.model, gh_nip29_room_get_room_id(room)));
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_name(room, "Book Club");
  g_assert_true(gh_nip29_room_get_is_closed(room));
  g_assert_cmpuint(counter.changes, >, 0);
  Nip29TestGroup *made = g_hash_table_lookup(relay.groups, group_id);
  g_assert_nonnull(made);
  g_assert_cmpstr(g_hash_table_lookup(made->admins, hex_alice), ==, "admin");
  g_assert_cmpstr(made->about, ==, "One chapter a week");
  GhConversation *conversation = gh_conversation_store_lookup(f.model,
                                                              gh_nip29_room_get_room_id(room));
  g_assert_nonnull(conversation);
  g_assert_cmpstr(gh_conversation_get_title(conversation), ==, "Book Club");
  g_autofree gchar *host = gh_group_relay_host(relay.url);
  g_autoptr(GhGroupStateCopy) copy = gh_group_create_copy(GH_NIP29_JOIN_MEMBER, NULL, TRUE, NULL,
                                                          host);
  g_assert_cmpstr(copy->title, ==, "Group Created");
  /* The creator is its admin: 39001 lists the account. */
  gh_test_spin_until(admins_known, room);
  g_assert_cmpint(gh_group_room_gate(room, NOSTR_PERMISSION_CREATE_INVITE), !=,
                  GH_GROUP_GATE_HIDDEN);

  /* A relay that doesn't let people create groups: its reason, nothing kept. */
  g_autoptr(GhNip29Room) refused = gh_nip29_service_create_group(f.service, strict.url,
                                                                 "wanted", metadata, &error);
  g_assert_no_error(error);
  g_autofree gchar *refused_id = g_strdup(gh_nip29_room_get_room_id(refused));
  wait_join(refused, GH_NIP29_JOIN_NONE);
  g_autofree gchar *strict_host = gh_group_relay_host(strict.url);
  g_clear_pointer(&copy, gh_group_state_copy_free);
  copy = gh_group_create_copy(GH_NIP29_JOIN_NONE, NULL, FALSE,
                              gh_nip29_room_get_detail(refused), strict_host);
  g_assert_cmpstr(copy->title, ==, "Group Not Created");
  g_assert_nonnull(strstr(copy->description, "“Group creation is not allowed here”"));
  GoneWait gone = { f.service, strict.url, "wanted" };
  gh_test_spin_until(room_gone, &gone);
  g_assert_null(gh_conversation_store_lookup(f.model, refused_id));
  /* ... and after a restart it stays gone; the created group is restored. */
  g_signal_handlers_disconnect_by_data(room, &counter);
  g_clear_object(&room);
  g_clear_object(&refused);
  g_clear_object(&request);
  restart(&f);
  g_autoptr(GhNip29Room) restored = gh_nip29_service_lookup(f.service, relay.url, group_id);
  g_assert_nonnull(restored);
  g_assert_cmpint(gh_nip29_room_get_join_state(restored), ==, GH_NIP29_JOIN_MEMBER);
  gone.service = f.service;
  g_assert_true(room_gone(&gone));
  /* An existing group of the list is not created again. */
  g_assert_null(gh_nip29_service_create_group(f.service, relay.url, group_id, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);

  fixture_down(&f);
  nip29_relay_clear(&strict);
  nip29_relay_clear(&relay);
}

/* Older history from the store (charter §7.6): after a restart the newest
 * page is listed and the rest comes on request (gh-group-ui.c's source). */
static void
test_older_history(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *group = nip29_add_group(&relay, "hist", "History");
  nip29_set_member(&relay, group, hex_bob, NULL);
  gint64 start = nip29_past(&relay) - 1000;
  for (guint i = 0; i < 70; i++) {
    g_autofree gchar *text = g_strdup_printf("message %u", i);
    post_at(&relay, KEY_BOB, "hist", text, start + i);
  }
  g_autofree gchar *hist_ref = reference(&relay, "hist", NULL);
  g_autoptr(GhNip29Room) room = join_ref(&f, hist_ref, NULL);
  g_autofree gchar *room_id = g_strdup(gh_nip29_room_get_room_id(room));
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_messages(f.model, room_id, 70);
  wait_read(room, GH_NIP29_READ_LIVE);
  g_clear_object(&room);
  relay.hold_eose = TRUE; /* the relay sends nothing new after the restart */
  restart(&f);
  GhConversation *conversation = gh_conversation_store_lookup(f.model, room_id);
  g_assert_nonnull(conversation);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==,
                   GH_STORE_NIP29_PAGE_SIZE);
  g_assert_true(gh_conversation_get_has_older(conversation));
  guint loaded = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_nip29_service_load_older(f.service, conversation, GH_STORE_NIP29_PAGE_SIZE,
                                            &loaded, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(loaded, ==, 70 - GH_STORE_NIP29_PAGE_SIZE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 70);
  g_assert_false(gh_conversation_get_has_older(conversation));

  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* ==== --gui: the dialogs and the window glue ================================================= */

static gboolean
is_mapped(gpointer widget)
{
  return gtk_widget_get_mapped(GTK_WIDGET(widget));
}

typedef struct {
  GtkWidget *window;
  int width, height;
} SizeWait;

static gboolean
has_size(gpointer data)
{
  SizeWait *wait = data;
  return gtk_widget_get_width(wait->window) == wait->width &&
         gtk_widget_get_height(wait->window) == wait->height;
}

/* As test_conversation_info.c: a width x height content area. */
static void
present_window(GtkWindow *window, int width, int height)
{
  gtk_window_set_default_size(window, width, height);
  gtk_window_present(window);
  gh_test_spin_until(is_mapped, window);
  drain();
  int border_x = width - gtk_widget_get_width(GTK_WIDGET(window));
  int border_y = height - gtk_widget_get_height(GTK_WIDGET(window));
  if (border_x > 0 || border_y > 0) {
    SizeWait wait = { GTK_WIDGET(window), width, height };
    gtk_window_set_default_size(window, width + MAX(border_x, 0), height + MAX(border_y, 0));
    gh_test_spin_until(has_size, &wait);
  }
}

static GtkWidget *
find_button(GtkWidget *widget, const char *label)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_get_mapped(widget) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button(c, label);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
find_type(GtkWidget *widget, GType type, const char *title)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type) &&
      (!title || (ADW_IS_PREFERENCES_ROW(widget) &&
                  g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)),
                            title) == 0)))
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_type(c, type, title);
    if (found)
      return found;
  }
  return NULL;
}

static gpointer
child_of(gpointer widget, GType type, const char *name)
{
  GObject *object = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(object);
  return object;
}

static gboolean
visible_of(gpointer widget, GType type, const char *name)
{
  return gtk_widget_get_visible(GTK_WIDGET(child_of(widget, type, name)));
}

typedef struct {
  const gchar *(*get)(gpointer);
  gpointer dialog;
  const gchar *text;
} TitleWait;

static gboolean
title_is(gpointer data)
{
  TitleWait *wait = data;
  return g_strcmp0(wait->get(wait->dialog), wait->text) == 0;
}

static const gchar *
join_title(gpointer dialog)
{
  return gh_group_join_dialog_get_status_title(dialog);
}

static const gchar *
new_title(gpointer dialog)
{
  return gh_new_group_dialog_get_status_title(dialog);
}

#define wait_join_title(dialog, value) \
  G_STMT_START { TitleWait w_ = { join_title, (dialog), (value) }; \
                 gh_test_spin_until(title_is, &w_); } G_STMT_END
#define wait_new_title(dialog, value) \
  G_STMT_START { TitleWait w_ = { new_title, (dialog), (value) }; \
                 gh_test_spin_until(title_is, &w_); } G_STMT_END

typedef struct {
  GtkWidget *widget;
  GtkWindow *window;
} RootWait;

static gboolean
alert_shown(gpointer data)
{
  RootWait *wait = data;
  return gtk_widget_get_root(wait->widget) == GTK_ROOT(wait->window) &&
         gtk_widget_get_mapped(wait->widget);
}

static gboolean
alert_gone(gpointer data)
{
  return gtk_widget_get_root(((RootWait *)data)->widget) == NULL;
}

typedef struct {
  GtkWidget *root;
  const char *label;
  GtkWidget *button;
} ButtonWait;

static gboolean
button_shown(gpointer data)
{
  ButtonWait *wait = data;
  wait->button = find_button(wait->root, wait->label);
  return wait->button != NULL;
}

/* Runs action (with parameter) on the Group Info dialog and answers its
 * confirmation alert_name with the button labelled label. */
static void
confirm(GtkWindow *window, GhGroupInfoDialog *dialog, const char *action, GVariant *parameter,
        const char *alert_name, const char *label)
{
  RootWait wait = { child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, alert_name), window };
  g_assert_true(gtk_widget_activate_action_variant(GTK_WIDGET(dialog), action, parameter));
  gh_test_spin_until(alert_shown, &wait);
  ButtonWait shown = { wait.widget, label, NULL };
  gh_test_spin_until(button_shown, &shown);
  g_signal_emit_by_name(shown.button, "clicked");
  gh_test_spin_until(alert_gone, &wait);
  drain();
}

static void
on_closed(AdwDialog *dialog, gboolean *closed)
{
  (void)dialog;
  *closed = TRUE;
}

static gboolean
flag_set(gpointer data)
{
  return *(gboolean *)data;
}

static void
on_open_group(GObject *dialog, GhNip29Room *room, GhNip29Room **out)
{
  (void)dialog;
  g_set_object(out, room);
}

static GtkWindow *
test_window(void)
{
  GtkWindow *window = GTK_WINDOW(gh_window_new(NULL));
  present_window(window, 900, 720);
  return window;
}

/* Join Group: nothing is sent for a bad address; each answer is said in
 * its own words; Open Group hands the room over; a code opens a closed
 * group. */
static void
test_gui_join(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *review = nip29_add_group(&relay, "review", "Reviewed");
  review->join_policy = NIP29_JOIN_PENDING;
  Nip29TestGroup *vip = nip29_add_group(&relay, "vip", "VIP");
  vip->closed = TRUE;
  g_ptr_array_add(vip->invites, g_strdup("letmein"));
  nip29_publish_state(&relay, vip);
  Nip29TestGroup *nope = nip29_add_group(&relay, "nope", "No Entry");
  nope->join_policy = NIP29_JOIN_DENY;
  GtkWindow *window = test_window();

  GhGroupJoinDialog *dialog = g_object_ref_sink(gh_group_join_dialog_new(f.service));
  GhNip29Room *opened = NULL;
  gboolean closed = FALSE;
  g_signal_connect(dialog, "open-group", G_CALLBACK(on_open_group), &opened);
  g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), &closed);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  /* What joining means is on screen before anything is sent. */
  g_assert_nonnull(find_type(GTK_WIDGET(dialog), ADW_TYPE_ACTION_ROW,
                             "Not end-to-end encrypted"));

  /* A bad address: said in place, nothing contacts the relay. */
  gh_group_join_dialog_set_address(dialog, "groups.example.com");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.join", NULL));
  drain();
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG, "error_label"));
  g_assert_null(gh_group_join_dialog_get_room(dialog));
  g_assert_cmpuint(relay.reqs, ==, 0);
  g_assert_cmpuint(relay.events_seen, ==, 0);

  /* Pending review, then admitted: Open Group opens it. */
  g_autofree gchar *review_ref = reference(&relay, "review", NULL);
  gh_group_join_dialog_set_address(dialog, review_ref);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.join", NULL));
  g_assert_nonnull(gh_group_join_dialog_get_room(dialog));
  g_assert_false(visible_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG, "error_label"));
  wait_join_title(dialog, "Waiting for an Admin");
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG, "open_button"));
  nip29_admit(&relay, review, hex_alice);
  wait_join_title(dialog, "You're In");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.open", NULL));
  gh_test_spin_until(flag_set, &closed);
  g_assert_true(opened == gh_group_join_dialog_get_room(dialog));
  g_clear_object(&opened);
  g_object_unref(dialog);

  /* Invite only: a code field, then in. */
  dialog = g_object_ref_sink(gh_group_join_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  g_autofree gchar *vip_ref = reference(&relay, "vip", NULL);
  gh_group_join_dialog_set_address(dialog, vip_ref);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.join", NULL));
  wait_join_title(dialog, "Invite Only");
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG, "code_list"));
  gtk_editable_set_text(GTK_EDITABLE(child_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG,
                                              "retry_code_row")), "letmein");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.retry", NULL));
  wait_join_title(dialog, "You're In");
  adw_dialog_force_close(ADW_DIALOG(dialog));
  g_object_unref(dialog);

  /* Declined, with the relay's words; already a member when asked again. */
  dialog = g_object_ref_sink(gh_group_join_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  g_autofree gchar *nope_ref = reference(&relay, "nope", NULL);
  gh_group_join_dialog_set_address(dialog, nope_ref);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.join", NULL));
  wait_join_title(dialog, "Request Declined");
  GtkLabel *description = child_of(dialog, GH_TYPE_GROUP_JOIN_DIALOG, "status_description");
  g_assert_nonnull(strstr(gtk_label_get_text(description), "“You may not join this group”"));
  g_assert_false(gtk_label_get_use_markup(description));
  gh_group_join_dialog_set_address(dialog, vip_ref);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "join.join", NULL));
  wait_join_title(dialog, "Already a Member");
  adw_dialog_force_close(ADW_DIALOG(dialog));
  g_object_unref(dialog);

  gtk_window_destroy(window);
  drain();
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

static const gchar *
bob_name(const gchar *pubkey, gpointer data)
{
  (void)data;
  /* A cached name is text, never markup. */
  return g_strcmp0(pubkey, hex_bob) == 0 ? "<b>Bob</b>" : NULL;
}

static GtkWidget *
member_row(GtkWidget *dialog, const gchar *title)
{
  GType type = g_type_from_name("GhGroupMemberRow");
  g_assert_true(type != 0);
  return find_type(dialog, type, title);
}

static gboolean
member_row_shown(gpointer data)
{
  return member_row(data, "<b>Bob</b>") != NULL;
}

static gboolean
invite_shown(gpointer data)
{
  return visible_of(data, GH_TYPE_GROUP_INFO_DIALOG, "invite_code_row");
}

typedef struct {
  GHashTable *members;
  const gchar *pubkey;
} RelayMemberWait;

static gboolean
relay_dropped(gpointer data)
{
  RelayMemberWait *wait = data;
  return !g_hash_table_contains(wait->members, wait->pubkey);
}

/* Group Info: an admin's view and a member's; the member list may be
 * incomplete; an invite code and a removal through the relay; Leave and
 * Remove from Conversations, each confirmed. */
static void
test_gui_group_info(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "Club");
  nip29_set_member(&relay, club, hex_alice, "admin");
  Nip29TestGroup *plain = nip29_add_group(&relay, "plain", "Plain");
  nip29_set_member(&relay, plain, hex_carol, "admin");
  nip29_set_member(&relay, plain, hex_bob, NULL);
  g_autofree gchar *club_ref = reference(&relay, "club", NULL);
  g_autofree gchar *plain_ref = reference(&relay, "plain", NULL);
  g_autoptr(GhNip29Room) admin = join_ref(&f, club_ref, NULL);
  g_autoptr(GhNip29Room) member = join_ref(&f, plain_ref, NULL);
  wait_join(admin, GH_NIP29_JOIN_MEMBER);
  wait_join(member, GH_NIP29_JOIN_MEMBER);
  gh_test_spin_until(admins_known, admin);
  gh_test_spin_until(admins_known, member);
  GtkWindow *window = test_window();
  GhGroupInfoConfig config = { .service = f.service, .display_name = bob_name };

  /* The admin (no role policy: the relay decides). */
  GhGroupInfoDialog *dialog = g_object_ref_sink(gh_group_info_dialog_new(admin, &config));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  g_assert_cmpstr(gtk_label_get_text(child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "title_label")),
                  ==, "Club");
  AdwActionRow *privacy = child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "privacy_row");
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(privacy)), ==,
                  "Not end-to-end encrypted");
  AdwPreferencesGroup *members = child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "members_group");
  g_assert_nonnull(strstr(adw_preferences_group_get_description(members), "may be incomplete"));
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "admin_group"));
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "add_member_button"));
  g_assert_cmpstr(adw_preferences_group_get_description(
                    child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "admin_group")), ==,
                  "The relay decides whether you’re allowed.");
  /* Bob joins on the relay: the list follows its snapshot; his row has the
   * admin's menu, the account's own row has none. */
  nip29_set_member(&relay, club, hex_bob, NULL);
  gh_test_spin_until(member_row_shown, dialog);
  GtkWidget *bob = member_row(GTK_WIDGET(dialog), "<b>Bob</b>");
  g_assert_true(visible_of(bob, g_type_from_name("GhGroupMemberRow"), "menu_button"));
  g_assert_true(gtk_widget_is_sensitive(bob));

  /* Invite code: sent, accepted by the relay, shown as a link. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "group.create-invite", NULL));
  gh_test_spin_until(invite_shown, dialog);
  AdwActionRow *invite = child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "invite_code_row");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(invite), "'club?invite="));
  g_assert_cmpuint(club->invites->len, ==, 1);

  /* Remove Bob, confirmed: the relay removes him. */
  confirm(window, dialog, "group.remove", g_variant_new_string(hex_bob), "remove_dialog",
          "_Remove");
  RelayMemberWait dropped = { club->members, hex_bob };
  gh_test_spin_until(relay_dropped, &dropped);
  adw_dialog_force_close(ADW_DIALOG(dialog));
  g_object_unref(dialog);

  /* A member: nothing to manage, no member menus. */
  dialog = g_object_ref_sink(gh_group_info_dialog_new(member, &config));
  gboolean closed = FALSE;
  g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), &closed);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  g_assert_false(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "admin_group"));
  g_assert_false(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "add_member_button"));
  bob = member_row(GTK_WIDGET(dialog), "<b>Bob</b>");
  g_assert_nonnull(bob);
  g_assert_false(visible_of(bob, g_type_from_name("GhGroupMemberRow"), "menu_button"));
  g_assert_false(gtk_widget_activate_action(GTK_WIDGET(dialog), "group.edit", NULL) &&
                 gtk_widget_get_visible(GTK_WIDGET(child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG,
                                                            "edit_row"))));
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "leave_row"));
  g_assert_false(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "forget_row"));

  /* Leave, confirmed; then Remove from Conversations closes the dialog. */
  confirm(window, dialog, "group.leave", NULL, "leave_dialog", "_Leave");
  wait_join(member, GH_NIP29_JOIN_LEFT);
  drain();
  g_assert_false(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "leave_row"));
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "join_row"));
  g_assert_true(visible_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "forget_row"));
  AdwActionRow *membership = child_of(dialog, GH_TYPE_GROUP_INFO_DIALOG, "membership_row");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(membership), "You Left This Group"));
  confirm(window, dialog, "group.forget", NULL, "forget_dialog", "_Remove");
  gh_test_spin_until(flag_set, &closed);
  g_object_unref(dialog);

  gtk_window_destroy(window);
  drain();
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* New Group: a bad relay is said in place; a relay that allows it creates
 * the group ("Group Created" once its details arrive); one that doesn't
 * says so. */
static void
test_gui_new_group(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  relay.allow_create = TRUE;
  Nip29Relay strict;
  nip29_relay_init(&strict);
  GtkWindow *window = test_window();

  GhNewGroupDialog *dialog = g_object_ref_sink(gh_new_group_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  AdwEntryRow *relay_row = child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "relay_row");
  AdwEntryRow *name_row = child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "name_row");
  gtk_editable_set_text(GTK_EDITABLE(relay_row), "ws://groups.example.com");
  gtk_editable_set_text(GTK_EDITABLE(name_row), "Book Club");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.create", NULL));
  g_assert_true(visible_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "error_label"));
  g_assert_null(gh_new_group_dialog_get_room(dialog));

  gtk_editable_set_text(GTK_EDITABLE(relay_row), relay.url);
  adw_switch_row_set_active(child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "private_switch"), TRUE);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.create", NULL));
  g_assert_false(visible_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "error_label"));
  GhNip29Room *room = gh_new_group_dialog_get_room(dialog);
  g_assert_nonnull(room);
  wait_new_title(dialog, "Group Created");
  wait_name(room, "Book Club");
  g_assert_true(gh_nip29_room_get_is_private(room));
  g_assert_true(visible_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "open_button"));
  adw_dialog_force_close(ADW_DIALOG(dialog));
  g_object_unref(dialog);

  dialog = g_object_ref_sink(gh_new_group_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, dialog);
  gtk_editable_set_text(GTK_EDITABLE(child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "relay_row")),
                        strict.url);
  gtk_editable_set_text(GTK_EDITABLE(child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "name_row")),
                        "Nope");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "new-group.create", NULL));
  wait_new_title(dialog, "Group Not Created");
  GtkLabel *description = child_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "status_description");
  g_assert_nonnull(strstr(gtk_label_get_text(description),
                          "“Group creation is not allowed here”"));
  g_assert_true(visible_of(dialog, GH_TYPE_NEW_GROUP_DIALOG, "back_button"));
  adw_dialog_force_close(ADW_DIALOG(dialog));
  g_object_unref(dialog);

  gtk_window_destroy(window);
  drain();
  fixture_down(&f);
  nip29_relay_clear(&strict);
  nip29_relay_clear(&relay);
}

static GhNip29Service *
fixture_service(gpointer data)
{
  return ((Fixture *)data)->service;
}

static GtkWidget *
row_for(GtkWidget *widget, GhConversation *conversation)
{
  if (GH_IS_CONVERSATION_ROW(widget) &&
      gh_conversation_row_get_conversation(GH_CONVERSATION_ROW(widget)) == conversation)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = row_for(c, conversation);
    if (found)
      return found;
  }
  return NULL;
}

typedef struct {
  guint at_least;
} RefreshWait;

static gboolean
refreshed(gpointer data)
{
  return group_send_stub_refreshes() >= ((RefreshWait *)data)->at_least;
}

/* The window: the group row's glyph and label (charter §7.5), the menu
 * actions, the composer delegate (reason, send, delivery details, and the
 * reason following the join state), and Group Info for the shown group. */
static void
test_gui_window(void)
{
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  nip29_add_group(&relay, "open", "Open House");
  group_send_stub_reset();
  GhWindow *window = GH_WINDOW(test_window());
  gh_conversation_list_attach(window, f.model, NULL);
  GhGroupUiConfig config = { .conversations = f.model, .service = fixture_service,
                             .service_data = &f };
  gh_group_ui_attach(window, &config);
  g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(window), "join-group"));
  g_assert_true(g_action_group_get_action_enabled(G_ACTION_GROUP(window), "new-group"));

  g_autofree gchar *open_ref = reference(&relay, "open", NULL);
  g_autoptr(GhNip29Room) room = join_ref(&f, open_ref, NULL);
  wait_join(room, GH_NIP29_JOIN_MEMBER);
  wait_name(room, "Open House");
  wait_read(room, GH_NIP29_READ_LIVE);
  GhConversation *conversation = gh_conversation_store_lookup(f.model,
                                                              gh_nip29_room_get_room_id(room));
  g_assert_nonnull(conversation);
  drain();
  GtkWidget *row = row_for(GTK_WIDGET(window), conversation);
  g_assert_nonnull(row);
  GtkWidget *kind = child_of(row, GH_TYPE_CONVERSATION_ROW, "kind_icon");
  g_assert_true(gtk_widget_get_visible(kind));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(kind), ==, "Relay group, not end-to-end encrypted");
  /* The accessible label, where there is an AT context (not with the
   * GTK_A11Y=none main() sets on macOS). */
  g_autoptr(GtkATContext) at = gtk_accessible_get_at_context(GTK_ACCESSIBLE(kind));
  if (at)
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(kind), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        "Relay group, not end-to-end encrypted");
  g_assert_nonnull(strstr(gh_conversation_row_get_summary(GH_CONVERSATION_ROW(row)),
                          "Open House. Relay group, not end-to-end encrypted"));

  /* The composer's delegate: the member may write; a message is sent
   * through the group's outbox and its details name the relay. */
  gpointer data = NULL;
  const GhSendUiDelegate *delegate = group_send_stub_delegate(&data);
  g_assert_nonnull(delegate);
  g_assert_true(delegate->handles(conversation, data));
  g_assert_null(delegate->reason(conversation, data));
  g_autoptr(GError) error = NULL;
  g_assert_true(delegate->send(conversation, "from the composer", data, &error));
  g_assert_no_error(error);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(conversation));
  g_autoptr(GhMessage) sent = g_list_model_get_item(G_LIST_MODEL(conversation), n - 1);
  g_assert_true(gh_message_is_self(sent));
  g_autoptr(GhNip29Op) op = gh_group_find_message_op(f.service, gh_message_get_rumor_id(sent));
  wait_op(op, GH_NIP29_OP_ACCEPTED);
  g_autoptr(GhDeliveryReport) report = delegate->report(sent, data);
  g_assert_nonnull(report);
  g_assert_cmpuint(report->targets->len, ==, 1);
  GhDeliveryTarget *target = g_ptr_array_index(report->targets, 0);
  g_assert_cmpstr(target->relay_url, ==, relay.url);
  g_assert_true(target->accepted);

  /* Group Info for the shown group (the conversation-info group handler). */
  g_assert_true(gh_window_open_item(window, conversation));
  g_assert_true(gh_group_ui_show_info(window, conversation, NULL));
  AdwDialog *visible = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(GH_IS_GROUP_INFO_DIALOG(visible));
  g_assert_true(gh_group_info_dialog_get_room(GH_GROUP_INFO_DIALOG(visible)) == room);
  adw_dialog_force_close(visible);
  drain();

  /* The composer's reason follows the room: leaving closes it. */
  RefreshWait more = { group_send_stub_refreshes() + 1 };
  g_autoptr(GhNip29Op) leave = gh_nip29_service_leave(f.service, room, NULL, &error);
  g_assert_no_error(error);
  wait_join(room, GH_NIP29_JOIN_LEFT);
  gh_test_spin_until(refreshed, &more);
  g_autofree gchar *reason = delegate->reason(conversation, data);
  g_assert_nonnull(strstr(reason, "You left this group"));

  /* The menu's Join Group presents the dialog. */
  g_action_group_activate_action(G_ACTION_GROUP(window), "join-group", NULL);
  visible = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(GH_IS_GROUP_JOIN_DIALOG(visible));
  adw_dialog_force_close(visible);

  gtk_window_destroy(GTK_WINDOW(window));
  drain();
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* ---- screenshots (opt-in evidence) ----------------------------------------------------- */

static void
save_png(GtkWidget *window, const char *dir, const char *name)
{
  drain();
  gtk_test_widget_wait_for_draw(window);
  drain();
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree char *path = g_strdup_printf("%s/groundhog-g20b-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

static void
test_gui_screenshots(void)
{
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  Fixture f;
  fixture_up(&f);
  Nip29Relay relay;
  nip29_relay_init(&relay);
  Nip29TestGroup *club = nip29_add_group(&relay, "club", "Book Club");
  nip29_set_member(&relay, club, hex_alice, "admin");
  nip29_set_member(&relay, club, hex_bob, NULL);
  Nip29TestGroup *review = nip29_add_group(&relay, "review", "Reviewed");
  review->join_policy = NIP29_JOIN_PENDING;
  g_autofree gchar *club_ref = reference(&relay, "club", NULL);
  g_autoptr(GhNip29Room) admin = join_ref(&f, club_ref, NULL);
  wait_join(admin, GH_NIP29_JOIN_MEMBER);
  gh_test_spin_until(admins_known, admin);
  GtkWindow *window = test_window();

  GhGroupInfoConfig config = { .service = f.service, .display_name = bob_name };
  GhGroupInfoDialog *info = g_object_ref_sink(gh_group_info_dialog_new(admin, &config));
  adw_dialog_present(ADW_DIALOG(info), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, info);
  save_png(GTK_WIDGET(window), dir, "group-info");
  adw_dialog_force_close(ADW_DIALOG(info));
  g_object_unref(info);

  GhGroupJoinDialog *join = g_object_ref_sink(gh_group_join_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(join), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, join);
  save_png(GTK_WIDGET(window), dir, "join");
  g_autofree gchar *review_ref = reference(&relay, "review", NULL);
  gh_group_join_dialog_set_address(join, review_ref);
  gtk_widget_activate_action(GTK_WIDGET(join), "join.join", NULL);
  wait_join_title(join, "Waiting for an Admin");
  save_png(GTK_WIDGET(window), dir, "join-pending");
  adw_dialog_force_close(ADW_DIALOG(join));
  g_object_unref(join);

  GhNewGroupDialog *create = g_object_ref_sink(gh_new_group_dialog_new(f.service));
  adw_dialog_present(ADW_DIALOG(create), GTK_WIDGET(window));
  gh_test_spin_until(is_mapped, create);
  save_png(GTK_WIDGET(window), dir, "new-group");
  adw_dialog_force_close(ADW_DIALOG(create));
  g_object_unref(create);

  gtk_window_destroy(window);
  drain();
  fixture_down(&f);
  nip29_relay_clear(&relay);
}

/* ---- main ------------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  gboolean gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
  }
#ifdef __APPLE__
  /* As send-stack.h: GTK's macOS accessibility backend has no announce, which
   * the dialogs use. Linux (AT-SPI, the CI job) keeps it. */
  g_setenv("GTK_A11Y", "none", FALSE);
#endif
  hex_alice = gh_test_pub(KEY_ALICE);
  hex_bob = gh_test_pub(KEY_BOB);
  hex_carol = gh_test_pub(KEY_CAROL);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
  int status;
  if (gui_mode) {
    /* Before g_test_init(), as the other GUI tests. */
    if (!gtk_init_check()) {
      g_printerr("groundhog-group-ui GUI test skipped: no graphical display\n");
      status = 77;
      goto out;
    }
    adw_init();
    groundhog_register_resource();
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024, "gtk-enable-animations",
                 FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
    g_test_init(&argc, &argv, NULL);
    gh_test_bus_up(&shared_bus);
    g_test_add_func("/groundhog/group-ui-gui/join", test_gui_join);
    g_test_add_func("/groundhog/group-ui-gui/group-info", test_gui_group_info);
    g_test_add_func("/groundhog/group-ui-gui/new-group", test_gui_new_group);
    g_test_add_func("/groundhog/group-ui-gui/window", test_gui_window);
    g_test_add_func("/groundhog/group-ui-gui/screenshots", test_gui_screenshots);
  } else {
    g_test_init(&argc, &argv, NULL);
    gh_test_bus_up(&shared_bus);
    g_test_add_func("/groundhog/group-ui/references", test_references);
    g_test_add_func("/groundhog/group-ui/join-states", test_join_states);
    g_test_add_func("/groundhog/group-ui/send-and-retry", test_send_and_retry);
    g_test_add_func("/groundhog/group-ui/leave", test_leave);
    g_test_add_func("/groundhog/group-ui/admin-gating", test_admin_gating);
    g_test_add_func("/groundhog/group-ui/create-group", test_create_group);
    g_test_add_func("/groundhog/group-ui/older-history", test_older_history);
  }
  status = g_test_run();
  gh_test_bus_down(&shared_bus);
out:
  g_free(hex_alice);
  g_free(hex_bob);
  g_free(hex_carol);
  g_free(npub_alice);
  g_free(npub_bob);
  return status;
}
