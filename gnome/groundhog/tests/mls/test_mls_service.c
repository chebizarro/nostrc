/* GhMlsService wire tests (nostrc-qp24.13 part 1): three Groundhog accounts
 * (mls-world.h) against local store-and-serve relays through the real gnostr
 * transports. KeyPackages are published and rotated; Alice creates a group
 * and invites Bob (one Add Commit, merged on the relay's OK; the Welcome as a
 * gift wrap to Bob's inbox), Bob joins, messages flow both ways, Carol is
 * added and joins, the group is renamed, Carol is removed and can read
 * nothing afterwards, Bob leaves; a Commit whose relay answer was lost is
 * republished byte for byte after a restart and Carol joins through it; an
 * account switch closes every group connection and reselecting reopens
 * them. */
#include "mls-world.h"

#include <nostr-keys.h>

static const guint TRIO[] = { ALICE, BOB, CAROL };

static void
wait_key_packages(World *w, const guint *keys, guint n)
{
  for (guint i = 0; i < n; i++)
    spin_until(key_package_published, &w->apps[keys[i]], "a published KeyPackage");
}

/* The d tag of a kind-30443 event. */
static gchar *
d_tag(NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "d") == 0)
      return g_strdup(nostr_tag_get(tag, 1));
  }
  return NULL;
}

static guint
key_packages_by(WireRelay *relay, guint key, gchar **out_d)
{
  g_autoptr(GPtrArray) events = published(relay, 30443);
  guint n = 0;
  for (guint i = 0; i < events->len; i++) {
    NostrEvent *event = g_ptr_array_index(events, i);
    if (g_strcmp0(nostr_event_get_pubkey(event), hex[key]) != 0)
      continue;
    n++;
    if (out_d) {
      g_free(*out_d);
      *out_d = d_tag(event);
    }
  }
  return n;
}

typedef struct {
  WireRelay *relay;
  guint key;
  guint count;
} CountWait;

static gboolean
key_packages_reached(gpointer data)
{
  CountWait *wait = data;
  return key_packages_by(wait->relay, wait->key, NULL) >= wait->count;
}

typedef struct {
  App *app;
  const gchar *old_id;
} RotatedWait;

static gboolean
key_package_rotated(gpointer data)
{
  RotatedWait *wait = data;
  const gchar *id = gh_mls_service_get_key_package_id(wait->app->service);
  return id && g_strcmp0(id, wait->old_id) != 0 &&
         gh_mls_service_get_key_package_state(wait->app->service) ==
           GH_MLS_KEY_PACKAGE_PUBLISHED;
}

/* MIP-00: each account's KeyPackage on its own write and inbox relays,
 * signed by the account; a rotation replaces it in the same `d` slot. */
static void
test_key_packages(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  CountWait on_inbox = { &w.x, ALICE, 1 };
  spin_until(key_packages_reached, &on_inbox, "Alice's KeyPackage on her inbox relay");
  g_autofree gchar *d_first = NULL, *d_second = NULL;
  g_assert_cmpuint(key_packages_by(&w.w, ALICE, &d_first), ==, 1);
  g_assert_cmpuint(key_packages_by(&w.x, ALICE, NULL), ==, 1);
  g_assert_cmpuint(key_packages_by(&w.w, BOB, NULL), ==, 1);
  g_assert_nonnull(d_first);
  const gchar *first_id = gh_mls_service_get_key_package_id(w.apps[ALICE].service);
  g_assert_nonnull(first_id);
  g_autofree gchar *first = g_strdup(first_id);

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_rotate_key_package(w.apps[ALICE].service, &error));
  g_assert_no_error(error);
  CountWait again = { &w.w, ALICE, 2 };
  spin_until(key_packages_reached, &again, "the rotated KeyPackage");
  RotatedWait rotated = { &w.apps[ALICE], first };
  spin_until(key_package_rotated, &rotated, "the rotated KeyPackage accepted");
  g_assert_cmpuint(key_packages_by(&w.w, ALICE, &d_second), ==, 2);
  g_assert_cmpstr(d_first, ==, d_second);

  /* A restart within the lifetime publishes nothing new. */
  app_restart(&w.apps[ALICE]);
  spin_until(key_package_published, &w.apps[ALICE], "the KeyPackage state after a restart");
  g_assert_cmpuint(key_packages_by(&w.w, ALICE, NULL), ==, 2);
  world_down(&w);
}

typedef struct {
  GhMlsGroup *group;
  const gchar *name;
} NameWait;

static gboolean
name_is(gpointer data)
{
  NameWait *wait = data;
  return g_strcmp0(gh_mls_group_get_name(wait->group), wait->name) == 0;
}

static gboolean
unreadable_above(gpointer data)
{
  GroupWait *wait = data;
  return gh_mls_group_get_unreadable(wait->group) > (guint)wait->value;
}

typedef struct {
  App *app;
  const gchar *room_id;
  const gchar *text;
} StatusWait;

static gboolean
sent(gpointer data)
{
  StatusWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_SENT;
}

static gboolean
retrying_status(gpointer data)
{
  StatusWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_RETRYING;
}

static void
change(App *app, OpWait *wait)
{
  spin_until(op_done, wait, "the group change");
  g_assert_no_error(wait->error);
  g_assert_true(wait->ok);
  (void)app;
}

/* The whole life of a group among three accounts. */
static void
test_group_lifecycle(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);

  /* Create and invite: one Add, merged on the relay's OK. */
  GhMlsGroup *ga = create_group(alice, "Trio", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_assert_true(gh_mls_group_get_is_admin(ga));
  g_assert_false(gh_mls_group_get_pending_commit(ga));
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, 1);
  g_auto(GStrv) members = gh_mls_group_dup_members(ga);
  g_assert_cmpuint(g_strv_length(members), ==, 2);
  g_assert_true(g_strv_contains((const gchar *const *)members, hex[BOB]));
  GhConversation *alice_room = gh_conversation_store_lookup(alice->model, room);
  g_assert_nonnull(alice_room);
  g_assert_cmpint(gh_conversation_get_backend(alice_room), ==, GH_CONVERSATION_BACKEND_MLS);
  g_assert_cmpstr(gh_conversation_get_title(alice_room), ==, "Trio");
  g_assert_false(gh_conversation_get_is_request(alice_room));

  /* Join through the Welcome. */
  GhMlsGroup *gb = join(bob, ALICE);
  /* MIP-00: the Welcome spent Bob's KeyPackage; a new one replaces it. */
  CountWait rotated = { &w.w, BOB, 2 };
  spin_until(key_packages_reached, &rotated, "Bob's KeyPackage rotated after the join");
  g_assert_cmpstr(gh_mls_group_get_room_id(gb), ==, room);
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, "Trio");
  g_assert_false(gh_mls_group_get_is_admin(gb));
  GhConversation *bob_room = gh_conversation_store_lookup(bob->model, room);
  g_assert_nonnull(bob_room);
  g_assert_cmpstr(gh_conversation_get_title(bob_room), ==, "Trio");

  /* Messages both ways; the sender's status is honest. */
  send_text(alice, ga, "hello bob");
  wait_message(bob, room, "hello bob");
  StatusWait sent_wait = { alice, room, "hello bob" };
  spin_until(sent, &sent_wait, "hello bob sent");
  g_assert_cmpstr(gh_message_get_sender(find_message(bob, room, "hello bob")), ==, hex[ALICE]);
  send_text(bob, gb, "hi alice");
  wait_message(alice, room, "hi alice");
  g_assert_cmpstr(gh_message_get_sender(find_message(alice, room, "hi alice")), ==, hex[BOB]);

  /* Only an admin changes the group. */
  OpWait refused = { 0 };
  const gchar *carol_only[] = { hex[CAROL], NULL };
  gh_mls_service_add_members_async(bob->service, gb, carol_only, NULL, on_changed, &refused);
  spin_until(op_done, &refused, "the refused add");
  g_assert_error(refused.error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NOT_ADMIN);
  g_clear_error(&refused.error);

  /* Add Carol. */
  OpWait added = { 0 };
  gh_mls_service_add_members_async(alice->service, ga, carol_only, NULL, on_changed, &added);
  change(alice, &added);
  wait_members(gb, 3);
  GhMlsGroup *gc = join(carol, ALICE);
  g_assert_cmpuint(gh_mls_group_get_epoch(gc), ==, gh_mls_group_get_epoch(ga));
  send_text(carol, gc, "carol here");
  wait_message(alice, room, "carol here");
  wait_message(bob, room, "carol here");
  send_text(bob, gb, "welcome carol");
  wait_message(carol, room, "welcome carol");

  /* Metadata. */
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Renamed", NULL, NULL, on_changed,
                                       &renamed);
  change(alice, &renamed);
  NameWait bob_name = { gb, "Renamed" }, carol_name = { gc, "Renamed" };
  spin_until(name_is, &bob_name, "Bob sees the new name");
  spin_until(name_is, &carol_name, "Carol sees the new name");
  g_assert_cmpstr(gh_conversation_get_title(bob_room), ==, "Renamed");
  g_assert_cmpstr(gh_mls_group_get_description(gb), ==, "a test group");

  /* Remove Carol: she can read nothing of the next epoch. */
  OpWait removed = { 0 };
  gh_mls_service_remove_members_async(alice->service, ga, carol_only, NULL, on_changed,
                                      &removed);
  change(alice, &removed);
  wait_members(gb, 2);
  g_auto(GStrv) after = gh_mls_group_dup_members(ga);
  g_assert_false(g_strv_contains((const gchar *const *)after, hex[CAROL]));
  GroupWait carol_unreadable = { gc, (gint)gh_mls_group_get_unreadable(gc) };
  send_text(alice, ga, "after removal");
  wait_message(bob, room, "after removal");
  spin_until(unreadable_above, &carol_unreadable, "Carol failing to read the next epoch");
  g_assert_null(find_message(carol, room, "after removal"));
  send_text(bob, gb, "bob after removal");
  wait_message(alice, room, "bob after removal");
  carol_unreadable.value = (gint)gh_mls_group_get_unreadable(gc);
  g_assert_cmpint(carol_unreadable.value, >=, 1);
  g_assert_null(find_message(carol, room, "bob after removal"));

  /* Bob leaves: he stops reading; his room and history stay. */
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  g_assert_false(gh_mls_group_get_active(gb));
  g_assert_cmpint(gh_mls_group_get_read_state(gb), ==, GH_MLS_READ_IDLE);
  g_assert_nonnull(find_message(bob, room, "hello bob"));
  g_assert_null(gh_mls_service_send(bob->service, gb, "after leaving", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  send_text(alice, ga, "after bob left");
  StatusWait left_wait = { alice, room, "after bob left" };
  spin_until(sent, &left_wait, "the message after Bob left sent");
  g_assert_null(find_message(bob, room, "after bob left"));

  /* A restart restores the room, its messages and the group. */
  app_restart(alice);
  GhMlsGroup *ga2 = gh_mls_service_lookup(alice->service, room);
  g_assert_nonnull(ga2);
  g_assert_true(gh_mls_group_get_active(ga2));
  g_assert_cmpstr(gh_mls_group_get_name(ga2), ==, "Renamed");
  g_assert_nonnull(find_message(alice, room, "carol here"));
  wait_live(ga2);
  world_down(&w);
}

typedef struct {
  WireRelay *relay;
  const gchar *id;
} StoredWait;

static gboolean
relay_stored(gpointer data)
{
  StoredWait *wait = data;
  for (guint i = 0; i < wait->relay->stored->len; i++)
    if (g_strcmp0(((WireStored *)g_ptr_array_index(wait->relay->stored, i))->id, wait->id) == 0)
      return TRUE;
  return FALSE;
}

typedef struct {
  GhMlsGroup *group;
  WireRelay *relay;
  guint events;
} PendingWait;

static gboolean
pending_and_tried(gpointer data)
{
  PendingWait *wait = data;
  return gh_mls_group_get_pending_commit(wait->group) && wait->relay->events > wait->events;
}

static gchar *
pending_commit_id(App *app, GhMlsGroup *group)
{
  MarmotGroupId gid = { 0 };
  g_autofree guint8 *bytes = g_malloc(strlen(gh_mls_group_get_group_id(group)) / 2);
  g_assert_true(nostr_hex2bin(bytes, gh_mls_group_get_group_id(group),
                              strlen(gh_mls_group_get_group_id(group)) / 2));
  gid.data = bytes;
  gid.len = strlen(gh_mls_group_get_group_id(group)) / 2;
  char *json = NULL;
  g_assert_cmpint(marmot_get_pending_commit(gh_mls_service_get_marmot(app->service), &gid,
                                            &json, NULL), ==, MARMOT_OK);
  g_assert_nonnull(json);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  gchar *id = g_strdup(nostr_event_get_id(event));
  nostr_event_free(event);
  free(json);
  return id;
}

/* A Commit whose relay answer was lost stays pending; after a restart the
 * same signed event is republished (never rebuilt), merged on the relay's OK,
 * and only then does the invitee get its Welcome. */
static void
test_restart_mid_commit(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Crash", (const guint[]){ BOB }, 1);
  g_autofree gchar *room_id = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);

  /* The group relay drops the connection on the Commit: no answer. */
  w.g.close_on_event = TRUE;
  guint published_before = 0;
  {
    g_autoptr(GPtrArray) before = published(&w.g, 445);
    published_before = before->len;
  }
  PendingWait tried = { ga, &w.g, w.g.events };
  OpWait added = { 0 };
  const gchar *carol_only[] = { hex[CAROL], NULL };
  gh_mls_service_add_members_async(alice->service, ga, carol_only, NULL, on_changed, &added);
  spin_until(pending_and_tried, &tried, "the Commit tried and left pending");
  g_autofree gchar *commit_id = pending_commit_id(alice, ga);
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, 1);   /* not merged */
  g_assert_cmpuint(carol->invites, ==, 0);                /* no Welcome before the merge */

  /* "Crash": the process goes (the waiting change is cancelled, the
   * Commit stays pending in the store), the relay recovers, it restarts. */
  w.g.close_on_event = FALSE;
  app_restart(alice);
  g_assert_true(added.done);
  g_assert_error(added.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&added.error);
  StoredWait stored = { &w.g, commit_id };
  spin_until(relay_stored, &stored, "the same Commit republished and stored");
  GhMlsGroup *ga2 = gh_mls_service_lookup(alice->service, room_id);
  g_assert_nonnull(ga2);
  wait_epoch(ga2, 2);
  wait_members(gb, 3);
  GhMlsGroup *gc = join(carol, ALICE);
  g_assert_cmpuint(gh_mls_group_get_epoch(gc), ==, 2);
  {
    /* Every kind 445 published since the add is that one Commit. */
    g_autoptr(GPtrArray) since_add = published(&w.g, 445);
    g_assert_cmpuint(since_add->len, >, published_before);
    for (guint i = published_before; i < since_add->len; i++)
      g_assert_cmpstr(nostr_event_get_id(g_ptr_array_index(since_add, i)), ==, commit_id);
  }
  send_text(carol, gc, "joined after the crash");
  wait_message(alice, room_id, "joined after the crash");
  wait_message(bob, room_id, "joined after the crash");
  /* Exactly one Add Commit was ever published: the one staged before. */
  g_autoptr(GPtrArray) commits = published(&w.g, 445);
  g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < commits->len; i++)
    g_hash_table_add(ids, g_strdup(nostr_event_get_id(g_ptr_array_index(commits, i))));
  g_assert_true(g_hash_table_contains(ids, commit_id));
  world_down(&w);
}


/* ---- Read cursor and held events (review B1, M1, M3, M4) ------------------------------ */

/* The envelope json re-signed under a fresh key with another created_at:
 * what a relay replay, or anyone copying a group event, can publish. */
static gchar *
resigned(const gchar *json, gint64 created_at)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  nostr_event_set_created_at(event, created_at);
  char *key = nostr_key_generate_private();
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  free(key);
  char *out = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(out);
  free(out);
  return copy;
}

/* A validly signed kind 445 with the group's routing h that is nothing:
 * anyone can publish one. */
static gchar *
junk_445(const gchar *h, guint n, gint64 created_at)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 445);
  nostr_event_set_created_at(event, created_at);
  g_autofree gchar *content = g_strdup_printf("AjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunkA%u", n);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", h, NULL));
  nostr_event_set_tags(event, tags);
  char *key = nostr_key_generate_private();
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  free(key);
  char *out = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(out);
  free(out);
  return copy;
}

static void
track_max(GObject *group, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  guint *max = data;
  *max = MAX(*max, gh_mls_group_get_unreadable(GH_MLS_GROUP(group)));
}

static const gchar *
h_of(WireStored *stored)
{
  NostrTags *tags = nostr_event_get_tags(stored->event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get(tag, 0), "h") == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

/* B1: a far-future replay is refused and moves no cursor: after the group
 * is subscribed again (a network flap, a restart) both what was posted
 * meanwhile and what is posted live still arrive. */
static void
test_future_replay_moves_no_cursor(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Clock", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  send_text(alice, ga, "first");
  wait_message(bob, room, "first");

  /* Ten years ahead, the same content under a fresh key. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *forged = resigned(last_stored_445(&w.g)->json, now + 10 * 365 * 24 * 3600);
  wire_relay_inject(&w.g, forged);
  send_text(alice, ga, "barrier");            /* delivered after the forgery */
  wait_message(bob, room, "barrier");

  set_online(bob, FALSE);
  send_text(alice, ga, "while away");
  StatusWait away = { alice, room, "while away" };
  spin_until(sent, &away, "the message sent while Bob was away");
  set_online(bob, TRUE);
  wait_message(bob, room, "while away");      /* history */
  send_text(alice, ga, "live after");
  wait_message(bob, room, "live after");      /* live */

  app_restart(bob);
  gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  send_text(alice, ga, "after restart");
  wait_message(bob, room, "after restart");
  world_down(&w);
}

/* M1/M4: a later epoch's message that arrives before its Commit is held
 * once (dedup across re-subscriptions), kept across a network flap, not
 * passed by the cursor (a restart fetches it again), and read when the
 * Commit arrives. */
static void
test_held_until_commit(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Epochs", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  /* What Bob holds already: the Add Commit that admitted him, of an epoch
   * he never had (it ages out as junk after a few Commits). */
  gint base = (gint)gh_mls_group_get_unreadable(gb);

  set_online(bob, FALSE);
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Next", NULL, NULL, on_changed,
                                       &renamed);
  change(alice, &renamed);
  g_autofree gchar *commit = g_strdup(last_stored_445(&w.g)->id);
  wire_relay_withhold(&w.g, commit);
  send_text(alice, ga, "next epoch");
  StatusWait next = { alice, room, "next epoch" };
  spin_until(sent, &next, "the next epoch's message sent");

  set_online(bob, TRUE);
  wait_unreadable(gb, base + 1);
  set_online(bob, FALSE);
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, base + 1);   /* not lost offline */
  set_online(bob, TRUE);
  wait_live(gb);
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, base + 1);   /* fetched again, once */

  app_restart(bob);
  gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  wait_unreadable(gb, base + 1);                   /* the cursor stayed behind them */
  g_assert_null(find_message(bob, room, "next epoch"));
  wire_relay_release(&w.g, commit);
  wait_message(bob, room, "next epoch");
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, "Next");
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, base);
  world_down(&w);
}

/* M1: a flood of junk with the group's public h cannot push out a genuine
 * later-epoch message that arrives after it (the oldest held goes first),
 * the queue never grows past its cap, and junk that stays unreadable
 * through GH_MLS_SERVICE_JUNK_AFTER_COMMITS Commits is dropped. The junk
 * comes live in batches: GNostrSubscription keeps at most 200 queued events
 * per subscription (nostrc-75o3), so one larger burst would lose events
 * before Groundhog saw them. */
static void
test_junk_does_not_evict(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Flood", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(gb);
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  guint max = 0;
  g_signal_connect(gb, "notify::unreadable", G_CALLBACK(track_max), &max);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  guint base = gh_mls_group_get_unreadable(gb), injected = 0;
  for (guint batch = 0; batch < 3; batch++) {
    for (guint i = 0; i < 100; i++) {
      g_autofree gchar *junk = junk_445(h, injected++, now);
      wire_relay_inject(&w.g, junk);
    }
    wait_unreadable(gb, (gint)MIN(base + injected, GH_MLS_SERVICE_MAX_HELD));
  }
  /* The next epoch's Commit, withheld; then its message, live. */
  w.g.withhold_new = TRUE;
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Flooded", NULL, NULL, on_changed,
                                       &renamed);
  change(alice, &renamed);
  w.g.withhold_new = FALSE;
  g_autofree gchar *commit = g_strdup(last_stored_445(&w.g)->id);
  send_text(alice, ga, "genuine");
  StatusWait genuine = { alice, room, "genuine" };
  spin_until(sent, &genuine, "the genuine message sent");
  wire_relay_release(&w.g, commit);
  wait_message(bob, room, "genuine");      /* it survived the flood */
  for (guint i = 0; i < GH_MLS_SERVICE_JUNK_AFTER_COMMITS; i++) {
    OpWait again = { 0 };
    g_autofree gchar *name = g_strdup_printf("Flooded %u", i);
    gh_mls_service_update_metadata_async(alice->service, ga, name, NULL, NULL, on_changed,
                                         &again);
    change(alice, &again);
    NameWait seen = { gb, name };
    spin_until(name_is, &seen, "Bob applying the next Commit");
  }
  g_assert_cmpuint(max, ==, GH_MLS_SERVICE_MAX_HELD);          /* full, never over */
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, 0);    /* the junk is gone */
  g_signal_handlers_disconnect_by_data(gb, &max);
  world_down(&w);
}

/* The since of every kind-445 REQ the relay received. */
static GArray *
req_sinces(WireRelay *relay)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(gint64));
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    const gchar *at = frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"")
                        ? strstr(frame->text, "\"since\":") : NULL;
    if (!at || !strstr(frame->text, "445"))
      continue;
    gint64 since = g_ascii_strtoll(at + strlen("\"since\":"), NULL, 10);
    g_array_append_val(out, since);
  }
  return out;
}

/* M3: a joined group is read from its Welcome's time (not from a fixed
 * window before the accept). */
static void
test_join_reads_from_welcome(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  create_group(alice, "Since", (const guint[]){ BOB }, 1);
  join(bob, ALICE);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  g_autoptr(GArray) sinces = req_sinces(&w.g);
  g_assert_cmpuint(sinces->len, >=, 2);                        /* Alice's and Bob's */
  for (guint i = 0; i < sinces->len; i++) {
    gint64 since = g_array_index(sinces, gint64, i);
    g_assert_cmpint(since, >=, before - GH_MLS_SERVICE_CURSOR_OVERLAP - 5);
    g_assert_cmpint(since, <=, after);
  }
  world_down(&w);
}

/* M4: a send whose relay answer was lost is republished after a restart,
 * byte for byte (the same kind 445), and delivered. */
static void
test_send_republished_after_restart(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Resend", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);

  w.g.close_on_event = TRUE;
  guint tried = w.g.events;
  send_text(alice, ga, "lost answer");
  StatusWait retrying = { alice, room, "lost answer" };
  spin_until(retrying_status, &retrying, "the send left for a retry");
  g_assert_cmpuint(w.g.events, >, tried);
  g_autoptr(GPtrArray) attempts = published(&w.g, 445);
  g_autofree gchar *attempt = g_strdup(nostr_event_get_id(g_ptr_array_index(attempts,
                                                                            attempts->len - 1)));
  w.g.close_on_event = FALSE;
  app_restart(alice);
  StoredWait stored = { &w.g, attempt };
  spin_until(relay_stored, &stored, "the same kind 445 republished");
  wait_message(bob, room, "lost answer");
  world_down(&w);
}


/* M2: the same text sent to two groups within one second (the store clock
 * frozen here) is two messages: the inner events are tagged with their
 * group, and the store dedups within a group only. */
static void
test_same_text_two_groups(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_fake_clock = TRUE;
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *one = create_group(alice, "One", (const guint[]){ BOB }, 1);
  join(bob, ALICE);
  GhMlsGroup *two = create_group(alice, "Two", (const guint[]){ BOB }, 1);
  join(bob, ALICE);
  g_autofree gchar *room_one = g_strdup(gh_mls_group_get_room_id(one));
  g_autofree gchar *room_two = g_strdup(gh_mls_group_get_room_id(two));
  send_text(alice, one, "ok");
  send_text(alice, two, "ok");
  wait_message(bob, room_one, "ok");
  wait_message(bob, room_two, "ok");
  GhMessage *a = find_message(bob, room_one, "ok"), *b = find_message(bob, room_two, "ok");
  g_assert_cmpint(gh_message_get_created_at(a), ==, gh_message_get_created_at(b));
  g_assert_cmpstr(gh_message_get_rumor_id(a), !=, gh_message_get_rumor_id(b));
  /* Both are Alice's own messages in her two rooms too. */
  g_assert_nonnull(find_message(alice, room_one, "ok"));
  g_assert_nonnull(find_message(alice, room_two, "ok"));
  world_down(&w);
}

/* An account switch closes every group connection at once; the account
 * coming back reopens them. */
static gboolean
read_idle(gpointer data)
{
  return gh_mls_group_get_read_state(data) == GH_MLS_READ_IDLE;
}

static void
test_account_switch(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Switch", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(ga);
  guint closed = w.g.closed_sockets;
  /* Offline is the same stop as a switch (a generation that does not run). */
  alice->network->available = FALSE;
  g_signal_emit_by_name(alice->network, "network-changed", FALSE);
  g_assert_cmpint(gh_mls_group_get_read_state(ga), ==, GH_MLS_READ_IDLE);
  g_assert_cmpint(gh_mls_service_get_key_package_state(alice->service), ==,
                  GH_MLS_KEY_PACKAGE_NONE);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_mls_service_send(alice->service, ga, "offline", &error));
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_INACTIVE);
  g_clear_error(&error);
  /* The account switch proper: another identity becomes active. */
  alice->network->available = TRUE;
  g_signal_emit_by_name(alice->network, "network-changed", TRUE);
  wait_live(ga);
  g_settings_set_string(alice->settings, "current-npub", npub[STRANGER]);
  gh_account_controller_refresh(alice->accounts);
  spin_until(read_idle, ga, "Alice's group closed by the switch");
  g_assert_cmpuint(w.g.closed_sockets, >, closed);
  g_settings_set_string(alice->settings, "current-npub", npub[ALICE]);
  gh_account_controller_refresh(alice->accounts);
  wait_live(ga);
  send_text(alice, ga, "back again");
  wait_message(bob, gh_mls_group_get_room_id(gb), "back again");
  world_down(&w);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mls-service/key-packages", test_key_packages);
  g_test_add_func("/groundhog/mls-service/group-lifecycle", test_group_lifecycle);
  g_test_add_func("/groundhog/mls-service/restart-mid-commit", test_restart_mid_commit);
  g_test_add_func("/groundhog/mls-service/account-switch", test_account_switch);
  g_test_add_func("/groundhog/mls-service/future-replay-moves-no-cursor",
                  test_future_replay_moves_no_cursor);
  g_test_add_func("/groundhog/mls-service/held-until-commit", test_held_until_commit);
  g_test_add_func("/groundhog/mls-service/junk-does-not-evict", test_junk_does_not_evict);
  g_test_add_func("/groundhog/mls-service/join-reads-from-welcome",
                  test_join_reads_from_welcome);
  g_test_add_func("/groundhog/mls-service/send-republished-after-restart",
                  test_send_republished_after_restart);
  g_test_add_func("/groundhog/mls-service/same-text-two-groups", test_same_text_two_groups);
  gint rc = g_test_run();
  mls_world_finish();
  return rc;
}
