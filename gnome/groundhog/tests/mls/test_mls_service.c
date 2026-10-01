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
#include "gh-store-mls-identity.h"
#if GH_MLS_SERVICE_ACCOUNT_PROOF
#include "mls-forge.h"
#endif

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

/* MIP-00: each account's KeyPackage on its own kind-10002 write relays only
 * (never its 10050 inbox relays, nostrc-0bdg), signed by the account; a
 * rotation replaces it in the same `d` slot. */
static void
test_key_packages(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  g_autofree gchar *d_first = NULL, *d_second = NULL;
  g_assert_cmpuint(key_packages_by(&w.w, ALICE, &d_first), ==, 1);
  g_assert_cmpuint(key_packages_by(&w.x, ALICE, NULL), ==, 0);
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
group_ended(gpointer data)
{
  return gh_mls_group_get_end(data) != GH_MLS_GROUP_END_NONE;
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

  /* Remove Carol (nostrc-xrya): her group ends, says who removed her, stops
   * reading and sending, and counts nothing as "unreadable yet"; her
   * history stays. */
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_NONE);
  OpWait removed = { 0 };
  gh_mls_service_remove_members_async(alice->service, ga, carol_only, NULL, on_changed,
                                      &removed);
  change(alice, &removed);
  wait_members(gb, 2);
  g_auto(GStrv) after = gh_mls_group_dup_members(ga);
  g_assert_false(g_strv_contains((const gchar *const *)after, hex[CAROL]));
  spin_until(group_ended, gc, "Carol's group ending");
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_REMOVED);
  g_assert_cmpstr(gh_mls_group_get_removed_by(gc), ==, hex[ALICE]);
  g_assert_false(gh_mls_group_get_active(gc));
  g_assert_cmpint(gh_mls_group_get_read_state(gc), ==, GH_MLS_READ_IDLE);
  g_assert_cmpint(gh_mls_group_get_end(gb), ==, GH_MLS_GROUP_END_NONE);
  g_assert_nonnull(find_message(carol, room, "welcome carol"));
  g_autoptr(GError) send_error = NULL;
  g_assert_null(gh_mls_service_send(carol->service, gc, "still here?", &send_error));
  g_assert_error(send_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  send_text(alice, ga, "after removal");
  wait_message(bob, room, "after removal");
  send_text(bob, gb, "bob after removal");
  wait_message(alice, room, "bob after removal");
  drain();
  g_assert_cmpuint(gh_mls_group_get_unreadable(gc), ==, 0);
  g_assert_null(find_message(carol, room, "after removal"));
  g_assert_null(find_message(carol, room, "bob after removal"));
  /* A restart keeps the end and who caused it. */
  app_restart(carol);
  gc = gh_mls_service_lookup(carol->service, room);
  g_assert_nonnull(gc);
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_REMOVED);
  g_assert_cmpstr(gh_mls_group_get_removed_by(gc), ==, hex[ALICE]);
  g_assert_false(gh_mls_group_get_active(gc));
  g_assert_nonnull(find_message(carol, room, "welcome carol"));
  /* A damaged removal record says only that the group ended, never "You
   * left" (W22 review N3). */
  {
    g_autoptr(GError) serror = NULL;
    MarmotStorage *storage = gh_store_marmot_new(carol->store, &serror);
    g_assert_no_error(serror);
    const gchar *gid_hex = gh_mls_group_get_group_id(gc);
    gsize gid_len = strlen(gid_hex) / 2;
    g_autofree guint8 *gid = g_malloc(gid_len);
    g_assert_true(nostr_hex2bin(gid, gid_hex, gid_len));
    const guint8 junk[3] = { 9, 9, 9 };
    g_assert_true(gh_store_begin(carol->store, &serror));
    g_assert_cmpint(storage->mls_store(storage->ctx, "mls_group_removed", gid, gid_len, junk,
                                       sizeof junk), ==, MARMOT_OK);
    g_assert_true(gh_store_commit(carol->store, &serror));
    marmot_storage_free(storage);
  }
  app_restart(carol);
  gc = gh_mls_service_lookup(carol->service, room);
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_UNKNOWN);
  g_assert_null(gh_mls_group_get_removed_by(gc));
  g_assert_false(gh_mls_group_get_active(gc));

  /* Bob (not an admin) leaves for everyone (nostrc-2um6): his SelfRemove,
   * which Alice commits; then he stops reading; his room and history stay. */
  g_autoptr(GError) error = NULL;
  /* Groundhog makes its groups alone, so they do not require SelfRemove
   * (MDK's rule, review L1): the leave is a Remove request (review M1). */
  g_assert_cmpint(gh_mls_service_leave_kind(bob->service, gb), ==, GH_MLS_LEAVE_ADMINS);
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  g_assert_true(gh_mls_group_get_leaving(gb));
  wait_members(ga, 1);
  spin_until(group_ended, gb, "Bob's leave confirmed");
  g_assert_false(gh_mls_group_get_active(gb));
  g_assert_false(gh_mls_group_get_leaving(gb));
  g_assert_cmpint(gh_mls_group_get_end(gb), ==, GH_MLS_GROUP_END_LEFT);
  g_assert_null(gh_mls_group_get_removed_by(gb));
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
  gchar *id = event_id_dup(event);
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
    for (guint i = published_before; i < since_add->len; i++) {
      g_autofree gchar *id = event_id_dup(g_ptr_array_index(since_add, i));
      g_assert_cmpstr(id, ==, commit_id);
    }
  }
  send_text(carol, gc, "joined after the crash");
  wait_message(alice, room_id, "joined after the crash");
  wait_message(bob, room_id, "joined after the crash");
  /* Exactly one Add Commit was ever published: the one staged before. */
  g_autoptr(GPtrArray) commits = published(&w.g, 445);
  g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < commits->len; i++)
    g_hash_table_add(ids, event_id_dup(g_ptr_array_index(commits, i)));
  g_assert_true(g_hash_table_contains(ids, commit_id));
  world_down(&w);
}


/* ---- Read cursor and held events (review B1, M1, M3, M4) ------------------------------ */

static void
wait_text(App *app, const gchar *room, const gchar *text)
{
  MessageWait wait = { app, room, text };
  spin_until(message_listed, &wait, "a message listed");
}

static void
rename_group(App *alice, GhMlsGroup *ga, const gchar *name)
{
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, name, NULL, NULL, on_changed,
                                       &renamed);
  change(alice, &renamed);
}



static void
track_max(GObject *group, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  guint *max = data;
  *max = MAX(*max, gh_mls_group_get_unreadable(GH_MLS_GROUP(group)));
}

typedef struct {
  GhMlsGroup *group;
  gint64 at_least;
} CursorWait;

static gboolean
cursor_reached(gpointer data)
{
  CursorWait *wait = data;
  return gh_mls_group_get_cursor(wait->group) >= wait->at_least;
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
  /* Nothing held may mask the cursor (re-review N5): any Add Commit of
   * Bob's join second ages out over three Commits. */
  for (guint i = 0; i < GH_MLS_SERVICE_JUNK_AFTER_COMMITS; i++) {
    g_autofree gchar *name = g_strdup_printf("Clock %u", i);
    rename_group(alice, ga, name);
    NameWait seen = { gb, name };
    spin_until(name_is, &seen, "Bob applying the rename");
  }
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, 0);
  send_text(alice, ga, "first");
  wait_message(bob, room, "first");

  /* Ten years ahead, the same content under a fresh key. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *forged = resigned(last_stored_445(&w.g)->json, now + 10 * 365 * 24 * 3600);
  wire_relay_inject(&w.g, forged);
  send_text(alice, ga, "barrier");            /* delivered after the forgery */
  wait_message(bob, room, "barrier");
  g_assert_cmpint(gh_mls_group_get_cursor(gb), <=, real_now());   /* B1 */

  set_online(bob, FALSE);
  send_text(alice, ga, "while away");
  StatusWait away = { alice, room, "while away" };
  spin_until(sent, &away, "the message sent while Bob was away");
  set_online(bob, TRUE);
  wait_message(bob, room, "while away");      /* history */
  g_assert_cmpint(gh_mls_group_get_cursor(gb), <=, real_now());
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
 * comes live in one burst of 300: GNostrSubscription no longer drops events
 * past 200 queued (nostrc-dha5, nostrc-kzun). */
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
  for (guint i = 0; i < 300; i++) {
    g_autofree gchar *junk = junk_445(h, injected++, now);
    wire_relay_inject(&w.g, junk);
  }
  wait_unreadable(gb, (gint)MIN(base + injected, GH_MLS_SERVICE_MAX_HELD));
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

/* nostrc-oya4: what the UI shows is "decrypt-pending", not the number of
 * held events: a held event's type (message or Commit) is sealed until its
 * epoch opens. An undecryptable event dated in the join's own second, from
 * a stored answer -- where the joiner's own Add Commit lands -- is held but
 * not shown; a later one is. Junk dropped after the Commits is not held
 * again when a reconnect's overlap fetches it once more. */
static gboolean
decrypt_pending_is(gpointer data)
{
  GroupWait *wait = data;
  return gh_mls_group_get_decrypt_pending(wait->group) == (gboolean)wait->value;
}

static void
test_decrypt_pending_honest(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Pending", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(gb);
  gint64 joined = gh_mls_group_get_join_time(gb);
  g_assert_cmpint(joined, >, 0);
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  gint base = (gint)gh_mls_group_get_unreadable(gb);
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));

  /* In the join's second, from the stored answer: held, not shown. */
  set_online(bob, FALSE);
  g_autofree gchar *at_join = junk_445(h, 0, joined);
  wire_relay_inject(&w.g, at_join);
  set_online(bob, TRUE);
  wait_live(gb);
  wait_unreadable(gb, base + 1);
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));

  /* Dated later: something is waiting -- for a while (W22 review N4:
   * junk would otherwise keep it up for good in a quiet group). */
  gh_mls_service_set_pending_shown(bob->service, 1000);
  g_autofree gchar *later = junk_445(h, 1, real_now() + 30);
  wire_relay_inject(&w.g, later);
  wait_unreadable(gb, base + 2);
  GroupWait shown = { gb, TRUE };
  spin_until(decrypt_pending_is, &shown, "decrypt-pending");
  GroupWait aged = { gb, FALSE };
  spin_until(decrypt_pending_is, &aged, "decrypt-pending gone after its bound");
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, (guint)base + 2);   /* still held */
  gh_mls_service_set_pending_shown(bob->service, 0);

  /* Three Commits later both are junk, dropped. */
  for (guint i = 0; i < GH_MLS_SERVICE_JUNK_AFTER_COMMITS; i++) {
    g_autofree gchar *name = g_strdup_printf("Pending %u", i);
    rename_group(alice, ga, name);
    NameWait seen = { gb, name };
    spin_until(name_is, &seen, "Bob applying the next Commit");
  }
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, 0);
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));

  /* A reconnect's overlap fetches them again: judged already, not held. */
  set_online(bob, FALSE);
  set_online(bob, TRUE);
  wait_live(gb);
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, 0);
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));
  world_down(&w);
}

/* W22 review B1: a removal is judged by the Commit ordering, not by
 * arrival. Alice and Bob are admins, and Alice's key sorts below Bob's, so
 * her rename beats Bob's removal of Carol made in the same epoch. Carol gets
 * the removal first: she is removed, but keeps listening; the winning rename
 * then re-activates the group, which is read again, and the removed copy is
 * gone. */
static gboolean
group_end_is(gpointer data)
{
  GroupWait *wait = data;
  return (gint)gh_mls_group_get_end(wait->group) == wait->value;
}

static gboolean
became_admin(gpointer data)
{
  return gh_mls_group_get_is_admin(data);
}

static gboolean
epoch_is(gpointer data)
{
  GroupWait *wait = data;
  return gh_mls_group_get_epoch(wait->group) == (guint64)wait->value;
}

static void
test_losing_removal_reactivates(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  g_assert_cmpint(strcmp(hex[ALICE], hex[BOB]), <, 0);   /* the order this relies on */
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Race", (const guint[]){ BOB, CAROL }, 2);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  const gchar *admins[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(became_admin, gb, "Bob becoming an admin");
  GroupWait carol_epoch = { gc, (gint)gh_mls_group_get_epoch(ga) };
  spin_until(epoch_is, &carol_epoch, "Carol in Alice's epoch");
  wait_live(gc);

  /* One epoch, neither seeing the other's: Bob removes Carol, Alice renames. */
  w.g.withhold_new = TRUE;
  const gchar *carol_only[] = { hex[CAROL], NULL };
  OpWait removed = { 0 };
  gh_mls_service_remove_members_async(bob->service, gb, carol_only, NULL, on_changed, &removed);
  change(bob, &removed);
  g_autofree gchar *removal = g_strdup(last_stored_445(&w.g)->id);
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Race won", NULL, NULL, on_changed,
                                       &renamed);
  change(alice, &renamed);
  g_autofree gchar *rename = g_strdup(last_stored_445(&w.g)->id);
  w.g.withhold_new = FALSE;

  /* The losing removal first: removed, still listening. */
  wire_relay_release(&w.g, removal);
  GroupWait ended = { gc, GH_MLS_GROUP_END_REMOVED };
  spin_until(group_end_is, &ended, "Carol removed");
  g_assert_cmpstr(gh_mls_group_get_removed_by(gc), ==, hex[BOB]);
  g_assert_false(gh_mls_group_get_active(gc));
  g_assert_cmpint(gh_mls_group_get_read_state(gc), !=, GH_MLS_READ_IDLE);
  g_autoptr(GError) send_error = NULL;
  g_assert_null(gh_mls_service_send(carol->service, gc, "removed?", &send_error));
  /* Alice writes in her (winning) epoch meanwhile: Carol cannot read it now,
   * and must not skip it. */
  send_text(alice, ga, "sent during the race");
  StatusWait during = { alice, room, "sent during the race" };
  spin_until(sent, &during, "Alice's message sent");
  g_assert_null(find_message(carol, room, "sent during the race"));

  /* Then Alice's winning rename: Carol is a member of that epoch, and reads
   * it again from her cursor. */
  wire_relay_release(&w.g, rename);
  GroupWait back = { gc, GH_MLS_GROUP_END_NONE };
  spin_until(group_end_is, &back, "Carol back in the group");
  g_assert_true(gh_mls_group_get_active(gc));
  g_assert_null(gh_mls_group_get_removed_by(gc));
  NameWait carol_name = { gc, "Race won" }, bob_name = { gb, "Race won" };
  spin_until(name_is, &carol_name, "Carol in the winning epoch");
  spin_until(name_is, &bob_name, "Bob converging on the winning epoch");
  wait_live(gc);
  wait_message(carol, room, "sent during the race");
  send_text(alice, ga, "carol still here");
  wait_message(carol, room, "carol still here");
  send_text(carol, gc, "yes I am");
  wait_message(alice, room, "yes I am");
  wait_message(bob, room, "yes I am");
  g_auto(GStrv) members = gh_mls_group_dup_members(ga);
  g_assert_true(g_strv_contains((const gchar *const *)members, hex[CAROL]));
  world_down(&w);
}

/* W22 review B2: a removal that stays contested (Bob removes Carol; Alice's
 * key sorts lower, so she could still win) turns final once the group has
 * moved on without Carol -- MARMOT_REMOVAL_FINAL_AFTER events she cannot
 * open -- and then Carol stops listening: no subscription, and none after
 * a restart, so nothing since the removal is fetched again. */
static GArray *req_sinces(WireRelay *relay);

static gboolean
group_not_read(gpointer data)
{
  return gh_mls_group_get_read_state(data) == GH_MLS_READ_IDLE;
}

static void
test_contested_removal_stops_listening(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Moves on", (const guint[]){ BOB, CAROL }, 2);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  const gchar *admins[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(became_admin, gb, "Bob becoming an admin");
  GroupWait carol_epoch = { gc, (gint)gh_mls_group_get_epoch(ga) };
  spin_until(epoch_is, &carol_epoch, "Carol in Alice's epoch");
  wait_live(gc);

  const gchar *carol_only[] = { hex[CAROL], NULL };
  OpWait removed = { 0 };
  gh_mls_service_remove_members_async(bob->service, gb, carol_only, NULL, on_changed, &removed);
  change(bob, &removed);
  GroupWait ended = { gc, GH_MLS_GROUP_END_REMOVED };
  spin_until(group_end_is, &ended, "Carol removed");
  wait_members(ga, 2);
  g_assert_cmpint(gh_mls_group_get_read_state(gc), !=, GH_MLS_READ_IDLE);   /* contested */

  for (guint i = 0; i < MARMOT_REMOVAL_FINAL_AFTER; i++) {
    g_autofree gchar *text = g_strdup_printf("moving on %u", i);
    send_text(alice, ga, text);
    MessageWait listed = { bob, room, text };
    spin_until(message_listed, &listed, "Bob reading Alice");
  }
  spin_until(group_not_read, gc, "Carol no longer listening");
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_REMOVED);
  g_assert_cmpstr(gh_mls_group_get_removed_by(gc), ==, hex[BOB]);
  g_assert_cmpuint(gh_mls_group_get_unreadable(gc), ==, 0);

  /* A restart does not subscribe again. */
  g_autoptr(GArray) before = req_sinces(&w.g);
  app_restart(carol);
  spin_until(key_package_published, carol, "Carol's service running");
  drain();
  gc = gh_mls_service_lookup(carol->service, room);
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_REMOVED);
  g_assert_cmpint(gh_mls_group_get_read_state(gc), ==, GH_MLS_READ_IDLE);
  g_autoptr(GArray) after = req_sinces(&w.g);
  g_assert_cmpuint(after->len, ==, before->len);
  world_down(&w);
}

/* W22 review N5: a removal applied out of the held queue (Carol missed the
 * Commit before it) ends the group mid-pass; the rest of the pass is not
 * held again. */
static void
test_removal_from_held_queue(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Held", (const guint[]){ BOB, CAROL }, 2);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(&w.apps[BOB], ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  wait_live(gc);
  /* Carol's clock a day ahead: held times are bounded by her now, and the
   * message below must stay a minute after the removal. */
  gh_clock_unref(carol->clock);
  carol->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(carol);
  gc = gh_mls_service_lookup(carol->service, room);
  wait_live(gc);
  gint base = (gint)gh_mls_group_get_unreadable(gc);

  set_online(carol, FALSE);
  w.g.withhold_new = TRUE;
  rename_group(alice, ga, "Missed");
  g_autofree gchar *missed = g_strdup(last_stored_445(&w.g)->id);
  w.g.withhold_new = FALSE;
  const gchar *carol_only[] = { hex[CAROL], NULL };
  OpWait removed = { 0 };
  gh_mls_service_remove_members_async(alice->service, ga, carol_only, NULL, on_changed,
                                      &removed);
  change(alice, &removed);
  send_text(alice, ga, "after carol");
  StatusWait sent_wait = { alice, room, "after carol" };
  spin_until(sent, &sent_wait, "the message sent");
  /* Served dated a minute later: it is left for the retry pass after the
   * one that applies the removal (a pass ends with a Commit's second). */
  WireStored *message = last_stored_445(&w.g);
  g_autofree gchar *later = resigned(message->json, real_now() + 60);
  wire_relay_withhold(&w.g, message->id);
  wire_relay_inject(&w.g, later);
  set_online(carol, TRUE);
  wait_unreadable(gc, base + 2);              /* the removal and the message, held */

  wire_relay_release(&w.g, missed);
  GroupWait ended = { gc, GH_MLS_GROUP_END_REMOVED };
  spin_until(group_end_is, &ended, "the held removal applied");
  g_assert_cmpuint(gh_mls_group_get_unreadable(gc), ==, 0);
  g_assert_false(gh_mls_group_get_decrypt_pending(gc));
  g_assert_null(find_message(carol, room, "after carol"));
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
  g_autofree gchar *attempt = event_id_dup(g_ptr_array_index(attempts, attempts->len - 1));
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


/* nostrc-2lrz, review W24 M2: libmarmot dates a group's events strictly
 * after each other, within a bounded lead over the clock. A burst of
 * messages, then a Commit, through a group relay that refuses events dated
 * more than a minute ahead (relays do: strfry past 900 s, relayd past 600
 * s): every one is accepted, and Bob reads them all and follows the Commit. */
static void
test_burst_then_commit_accepted(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  w.g.max_future_seconds = 60;
  GhMlsGroup *ga = create_group(alice, "Burst", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  enum { BURST = 90 };
  for (guint i = 0; i < BURST; i++) {
    g_autofree gchar *text = g_strdup_printf("burst %u", i);
    send_text(alice, ga, text);
  }
  StatusWait last = { alice, room, "burst 89" };
  spin_until(sent, &last, "the last message of the burst sent");
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "After the burst", NULL, NULL,
                                       on_changed, &renamed);
  change(alice, &renamed);
  g_assert_cmpuint(w.g.future_refused, ==, 0);
  NameWait bob_name = { gb, "After the burst" };
  spin_until(name_is, &bob_name, "Bob follows the Commit");
  wait_message(bob, room, "burst 0");
  wait_message(bob, room, "burst 89");
  world_down(&w);
}

static gboolean
is_admin(gpointer data)
{
  return gh_mls_group_get_is_admin(data);
}

/* Review W24 N5: libmarmot refuses a Commit it cannot date within a minute of
 * our clock (MARMOT_ERR_EVENT_RATE): right after applying another member's
 * Commit dated that far ahead, for about a second. Bob applies Alice's
 * rename only as a copy dated a day ahead (the genuine event withheld), and
 * renames at once: the service stages the change again a second later and
 * it goes through, with no error. The refusal needs Bob's rename in the
 * second he applied the copy, so the round repeats until one retry was seen. */
static void
test_change_retried_after_event_rate(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Rate", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  const gchar *admins[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(is_admin, gb, "Bob becoming an admin");

  guint before = gh_mls_service_test_rate_retries();
  for (guint round = 0; round < 5 && gh_mls_service_test_rate_retries() == before; round++) {
    g_autofree gchar *ahead_name = g_strdup_printf("Ahead %u", round);
    g_autofree gchar *bob_name = g_strdup_printf("Bob's %u", round);
    set_online(bob, FALSE);
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(alice->service, ga, ahead_name, NULL, NULL,
                                         on_changed, &renamed);
    change(alice, &renamed);
    WireStored *genuine = last_stored_445(&w.g);
    wire_relay_withhold(&w.g, genuine->id);
    g_autofree gchar *copy = resigned(genuine->json, real_now() + 24 * 3600);
    wire_relay_inject(&w.g, copy);
    set_online(bob, TRUE);
    NameWait applied = { gb, ahead_name };
    spin_until(name_is, &applied, "Bob applying the copy dated a day ahead");
    OpWait bob_renamed = { 0 };
    gh_mls_service_update_metadata_async(bob->service, gb, bob_name, NULL, NULL, on_changed,
                                         &bob_renamed);
    change(bob, &bob_renamed);
    NameWait seen = { ga, bob_name };
    spin_until(name_is, &seen, "Alice following Bob's rename");
  }
  g_assert_cmpuint(gh_mls_service_test_rate_retries(), >, before);
  world_down(&w);
}

/* §7.10 owner/admin: the creator makes Bob an admin; Bob then invites Carol,
 * who joins and reads. (With libmarmot >= 0.10.0 Carol's join checks every
 * leaf's account proof, the creator's included: this needs the creator's
 * enrollment, review B2.) */
static void
test_second_admin_invites(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(bob, CAROL);
  GhMlsGroup *ga = create_group(alice, "Admins", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  g_assert_false(gh_mls_group_get_is_admin(gb));

  const gchar *admins[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(is_admin, gb, "Bob becoming an admin");
  g_auto(GStrv) listed = gh_mls_group_dup_admins(gb);
  g_assert_cmpuint(g_strv_length(listed), ==, 2);

  /* Only members can be admins. */
  const gchar *stranger[] = { hex[STRANGER], NULL };
  OpWait refused = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, stranger, NULL, on_changed, &refused);
  spin_until(op_done, &refused, "the refused admin change");
  g_assert_error(refused.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&refused.error);

  const gchar *carol_only[] = { hex[CAROL], NULL };
  OpWait added = { 0 };
  gh_mls_service_add_members_async(bob->service, gb, carol_only, NULL, on_changed, &added);
  change(bob, &added);
  GhMlsGroup *gc = join(carol, BOB);
  wait_members(ga, 3);
  send_text(carol, gc, "invited by bob");
  wait_message(alice, room, "invited by bob");
  wait_message(bob, room, "invited by bob");
  world_down(&w);
}



/* ---- Catch-up across missed Commits (re-review N1, N2) ------------------------------- */

/* Bob misses k Commits with a message in each new epoch; the relay then
 * gives him that backlog newest first (every event withheld and released in
 * that order, the first Commit last, so the order does not depend on
 * events sharing a second): one catch-up must read every message and end at
 * Alice's epoch. Before re-review N1 the nested retry crashed; before N2
 * events four or more epochs ahead were dropped as junk. */
static void
run_catch_up(guint k)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Catch-up", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(gb);
  gint base = (gint)gh_mls_group_get_unreadable(gb);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;
  g_autoptr(GPtrArray) backlog = g_ptr_array_new_with_free_func(g_free);  /* c1 m1 c2 m2 ... */
  g_autoptr(GPtrArray) texts = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 1; i <= k; i++) {
    g_autofree gchar *name = g_strdup_printf("Epoch %u", i);
    rename_group(alice, ga, name);
    g_ptr_array_add(backlog, g_strdup(last_stored_445(&w.g)->id));
    gchar *text = g_strdup_printf("in epoch %u", i);
    send_text(alice, ga, text);
    StatusWait done = { alice, room, text };
    spin_until(sent, &done, "a message of the next epoch sent");
    g_ptr_array_add(backlog, g_strdup(last_stored_445(&w.g)->id));
    g_ptr_array_add(texts, text);
  }
  w.g.withhold_new = FALSE;
  set_online(bob, TRUE);
  wait_live(gb);
  for (guint i = backlog->len; i > 1; i--)
    wire_relay_release(&w.g, g_ptr_array_index(backlog, i - 1));   /* newest first */
  wait_unreadable(gb, base + (gint)(2 * k - 1));   /* k-1 Commits, k messages */
  wire_relay_release(&w.g, g_ptr_array_index(backlog, 0));        /* the first Commit */
  for (guint i = 0; i < texts->len; i++)
    wait_text(bob, room, g_ptr_array_index(texts, i));
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_autofree gchar *last = g_strdup_printf("Epoch %u", k);
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, last);
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, base);
  send_text(alice, ga, "after the catch-up");
  wait_message(bob, room, "after the catch-up");
  world_down(&w);
}

static void test_catch_up_2(void) { run_catch_up(2); }
static void test_catch_up_4(void) { run_catch_up(4); }
static void test_catch_up_5(void) { run_catch_up(5); }

/* ---- Large backlogs (nostrc-cpwf, nostrc-kzun) ------------------------------------------ */

typedef struct {
  App *app;
  const gchar *room;
  GPtrArray *texts;
  guint next;            /* texts[0..next) are listed */
} TextsWait;

static gboolean
history_incomplete(gpointer data)
{
  return gh_mls_group_get_history_incomplete(data);
}

static gboolean
texts_listed(gpointer data)
{
  TextsWait *wait = data;
  while (wait->next < wait->texts->len &&
         find_message(wait->app, wait->room, g_ptr_array_index(wait->texts, wait->next)))
    wait->next++;
  return wait->next == wait->texts->len;
}

typedef struct {
  WireRelay *relay;
  const gchar *h;
  guint from;            /* relay->stored index the count starts at */
  guint count;
} StoredCount;

static guint
group_stored(WireRelay *relay, const gchar *h, guint from)
{
  guint n = 0;
  for (guint i = from; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(stored->event) == 445 && g_strcmp0(h_of(stored), h) == 0)
      n++;
  }
  return n;
}

static gboolean
stored_reached(gpointer data)
{
  StoredCount *wait = data;
  return group_stored(wait->relay, wait->h, wait->from) >= wait->count;
}

typedef struct {
  GPtrArray *texts;      /* every message of the backlog */
  GPtrArray *ids;        /* its kind 445s on the relay, in the order they were made */
  gchar *first_commit;
} Backlog;

static void
backlog_clear(Backlog *backlog)
{
  g_ptr_array_unref(backlog->texts);
  g_ptr_array_unref(backlog->ids);
  g_free(backlog->first_commit);
}

/* While Bob is offline, Alice sends @before messages in the current epoch,
 * then @commits Commits (renames) each followed by @per_epoch messages. Each
 * event is stored by the relay before the next is made, so ids is in the
 * order the events were made. */
static void
make_backlog(World *w, App *alice, GhMlsGroup *ga, const gchar *h, guint before,
             guint commits, guint per_epoch, Backlog *out)
{
  out->texts = g_ptr_array_new_with_free_func(g_free);
  out->ids = g_ptr_array_new_with_free_func(g_free);
  out->first_commit = NULL;
  StoredCount wait = { &w->g, h, w->g.stored->len, 0 };
  guint n = 0;
  for (guint c = 0; c <= commits; c++) {
    if (c > 0) {
      g_autofree gchar *name = g_strdup_printf("Backlog %u", c);
      rename_group(alice, ga, name);
      wait.count++;
      spin_until(stored_reached, &wait, "the Commit stored");
      if (c == 1)
        out->first_commit = g_strdup(last_stored_445(&w->g)->id);
    }
    for (guint i = 0; i < (c == 0 ? before : per_epoch); i++) {
      gchar *text = g_strdup_printf("backlog %u", n++);
      send_text(alice, ga, text);
      g_ptr_array_add(out->texts, text);
      /* One at a time: publishes are asynchronous, and the relay's arrival
       * order must be the sender's ratchet order (re-signed dates follow it). */
      wait.count++;
      spin_until(stored_reached, &wait, "the message stored");
    }
  }
  for (guint i = wait.from; i < w->g.stored->len; i++) {
    WireStored *stored = g_ptr_array_index(w->g.stored, i);
    if (nostr_event_get_kind(stored->event) == 445 && g_strcmp0(h_of(stored), h) == 0)
      g_ptr_array_add(out->ids, g_strdup(stored->id));
  }
  g_assert_cmpuint(out->ids->len, ==, before + commits * (per_epoch + 1));
}

static WireStored *
stored_by_id(WireRelay *relay, const gchar *id)
{
  for (guint i = 0; i < relay->stored->len; i++)
    if (g_str_equal(((WireStored *)g_ptr_array_index(relay->stored, i))->id, id))
      return g_ptr_array_index(relay->stored, i);
  g_assert_not_reached();
}

/* REQs for kind 445 that page backwards (carry until). */
static guint
paged_reqs(WireRelay *relay)
{
  guint n = 0;
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, "445") && strstr(frame->text, "\"until\":"))
      n++;
  }
  return n;
}

/* nostrc-cpwf: the group relay caps every REQ's stored answer at 50 (strfry
 * does at 500, whatever the REQ's limit) and Bob, offline, missed 303
 * events: 100 messages of his epoch, then 3 Commits spread over 200 more
 * messages. The relay answers newest first, so the Commit that opens the
 * next epoch is in the oldest part, which a single REQ never gets. Bob's
 * catch-up pages backwards with until, per relay, while the live REQ stays
 * open: every message read, Alice's epoch, nothing held, the cursor past the
 * backlog, and a live message after it. (The backlog is re-signed three
 * events per second, oldest first, so that a relay page never fits in one
 * second; Bob's clock runs a day ahead so those dates are in his past.) */
static void
test_catch_up_past_relay_cap(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  w.g.max_limit = 50;
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Capped", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  join(bob, ALICE);
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  gint base = (gint)gh_mls_group_get_unreadable(gb);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;                  /* served only as re-signed below */
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 100, 3, 66, &backlog);
  w.g.withhold_new = FALSE;
  gint64 start = real_now() + 5, newest = 0;
  for (guint i = 0; i < backlog.ids->len; i++) {
    newest = start + i / 3;
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, g_ptr_array_index(backlog.ids, i))->json,
                                      newest);
    wire_relay_inject(&w.g, copy);
  }
  guint paged_before = paged_reqs(&w.g);

  set_online(bob, TRUE);
  TextsWait all = { bob, room, backlog.texts, 0 };
  spin_until(texts_listed, &all, "every message of the backlog");
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, "Backlog 3");
  wait_live(gb);
  /* Nothing of the backlog is held; applied oldest first, its three Commits
   * are fresh ones, which also age out the join's own Add Commit. */
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, 0);
  g_assert_cmpint(base, <=, 1);
  CursorWait moved = { gb, newest };
  spin_until(cursor_reached, &moved, "the cursor past the backlog");
  /* 303 events at 50 a page: the live answer and at least five older pages. */
  g_assert_cmpuint(paged_reqs(&w.g) - paged_before, >=, 5);
  send_text(alice, ga, "after the capped catch-up");
  wait_message(bob, room, "after the capped catch-up");   /* the live REQ */
  backlog_clear(&backlog);
  world_down(&w);
}


/* Review B3: two group relays, each capped at 50 and each holding part of
 * a 303-event backlog: a third only on g, a third only on h, a third on
 * both. The scope deduplicates across relays, so neither relay's share is
 * the whole backlog nor a contiguous part of it. Applied oldest first as one
 * set once both relays have paged it (not one relay's share at a time),
 * every message is read, and the cursor passes the backlog. */
static void
test_catch_up_two_relays_partial(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  w.g.max_limit = 50;
  w.h.max_limit = 50;
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  const gchar *relays[] = { w.g.url, w.h.url, NULL };
  GhMlsGroup *ga = create_group_on(alice, "Two relays", relays, (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  /* g asks for AUTH before it stores a publish: the Add Commit may be on h first. */
  WireStored *any = last_stored_445(&w.g) ? last_stored_445(&w.g) : last_stored_445(&w.h);
  g_assert_nonnull(any);
  g_autofree gchar *h = g_strdup(h_of(any));
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;                  /* served only as re-signed below */
  w.h.withhold_new = TRUE;
  StoredCount on_h = { &w.h, h, w.h.stored->len, 0 };
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 100, 3, 66, &backlog);
  on_h.count = backlog.ids->len;            /* h has every original too, withheld */
  spin_until(stored_reached, &on_h, "the backlog stored on h");
  w.g.withhold_new = FALSE;
  w.h.withhold_new = FALSE;
  gint64 start = real_now() + 5, newest = 0;
  for (guint i = 0; i < backlog.ids->len; i++) {
    newest = start + i / 3;
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, g_ptr_array_index(backlog.ids, i))->json,
                                      newest);
    if (i % 3 != 1)
      wire_relay_inject(&w.g, copy);
    if (i % 3 != 0)
      wire_relay_inject(&w.h, copy);
  }

  set_online(bob, TRUE);
  TextsWait all = { bob, room, backlog.texts, 0 };
  spin_until(texts_listed, &all, "every message of the backlog");
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, "Backlog 3");
  wait_live(gb);
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, 0);
  CursorWait moved = { gb, newest };
  spin_until(cursor_reached, &moved, "the cursor past the backlog");
  g_assert_cmpuint(paged_reqs(&w.g), >=, 3);   /* each relay held about 200: paged */
  g_assert_cmpuint(paged_reqs(&w.h), >=, 3);
  send_text(alice, ga, "after the two-relay catch-up");
  wait_message(bob, room, "after the two-relay catch-up");
  backlog_clear(&backlog);
  world_down(&w);
}

/* Review B4: the stored backfill is bounded. Bob keeps at most 60 events
 * at once and comes back to 100: at the 60th his relay stops paging and
 * counts as answered-incomplete ("history-incomplete", reported honestly),
 * what is stored is applied, the rest of that connection is read as it
 * comes, the read cursor does not move past the gap, and the group keeps
 * reading live messages. The next subscription with room enough completes
 * and only then moves the cursor. (Messages older than the part applied may
 * stay unreadable: the sender's ratchet has moved past libmarmot's window.
 * The default bound is what one honest paging round can deliver.) Dates are
 * re-signed three per second, Bob's clock a day ahead, as in
 * catch-up-past-relay-cap. */
static void
test_backfill_store_bounded(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Bounded", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  join(bob, ALICE);
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  g_assert_false(gh_mls_group_get_history_incomplete(gb));
  gint64 before = gh_mls_group_get_cursor(gb);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;                  /* served only as re-signed below */
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 100, 0, 0, &backlog);
  w.g.withhold_new = FALSE;
  gint64 start = real_now() + 5, newest = 0;
  for (guint i = 0; i < backlog.ids->len; i++) {
    newest = start + i / 3;
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, g_ptr_array_index(backlog.ids, i))->json,
                                      newest);
    wire_relay_inject(&w.g, copy);
  }
  gh_mls_service_set_backfill_limit(bob->service, 60, 0);
  set_online(bob, TRUE);
  spin_until(history_incomplete, gb, "the backfill reported incomplete");
  wait_live(gb);
  send_text(alice, ga, "live after the bounded backfill");
  wait_message(bob, room, "live after the bounded backfill");
  g_assert_true(gh_mls_group_get_history_incomplete(gb));
  g_assert_cmpint(gh_mls_group_get_cursor(gb), ==, before);   /* the gap is asked again */
  g_assert_nonnull(find_message(bob, room, "backlog 99"));     /* the newest were applied */

  /* The next subscription, with room enough, completes, and only then does
   * the cursor move (past the backlog). */
  gh_mls_service_set_backfill_limit(bob->service, 0, 0);
  set_online(bob, FALSE);
  set_online(bob, TRUE);
  CursorWait moved = { gb, newest };
  spin_until(cursor_reached, &moved, "the cursor moving after a complete backfill");
  g_assert_false(gh_mls_group_get_history_incomplete(gb));
  backlog_clear(&backlog);
  world_down(&w);
}

/* Final review N1 (probe P4): a group on relays g and h; Bob misses 60
 * messages. h serves all but the newest; g, capped at 50, is the only one
 * serving the newest, and then never answers its until page. (Re-signed
 * three per second in the recent past, in the sender's order.) Once h has
 * finished and g has been silent for the quiet period (1 s here), g is
 * given up as incomplete: the stored backlog of both is applied, a live
 * message Alice sends while g's page is out is read, the cursor holds (g
 * never answered) and history-incomplete says so. */
static void
test_stalled_relay_given_up(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  w.g.max_limit = 50;
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  const gchar *relays[] = { w.g.url, w.h.url, NULL };
  GhMlsGroup *ga = create_group_on(alice, "Stalled", relays, (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  WireStored *any = last_stored_445(&w.g) ? last_stored_445(&w.g) : last_stored_445(&w.h);
  g_assert_nonnull(any);
  g_autofree gchar *h = g_strdup(h_of(any));
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  gint64 before = gh_mls_group_get_cursor(gb);
  gh_mls_service_set_backfill_quiet(bob->service, 1000);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;                  /* served only as re-signed below */
  w.h.withhold_new = TRUE;
  StoredCount on_h = { &w.h, h, w.h.stored->len, 0 };
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 60, 0, 0, &backlog);
  on_h.count = backlog.ids->len;
  spin_until(stored_reached, &on_h, "the backlog stored on h");
  w.g.withhold_new = FALSE;
  w.h.withhold_new = FALSE;
  /* In the recent past, inside Bob's overlap: the live message below must
   * not be dated before the backlog it follows. */
  gint64 start = real_now() - 25;
  for (guint i = 0; i < backlog.ids->len; i++) {
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, g_ptr_array_index(backlog.ids, i))->json,
                                      start + i / 3);
    wire_relay_inject(&w.g, copy);
    if (i + 1 < backlog.ids->len)
      wire_relay_inject(&w.h, copy);          /* the newest only on g */
  }
  w.g.stall_pages = TRUE;

  set_online(bob, TRUE);
  wait_for_count(&w.g.stalled_reqs, 1);      /* g's page is out, and never answered */
  send_text(alice, ga, "live while g stalls");
  TextsWait all = { bob, room, backlog.texts, 0 };
  spin_until(texts_listed, &all, "every message of the backlog");
  wait_message(bob, room, "live while g stalls");
  spin_until(history_incomplete, gb, "g given up as incomplete");
  wait_live(gb);
  g_assert_cmpint(gh_mls_group_get_cursor(gb), ==, before);   /* g never answered */
  send_text(alice, ga, "live after g was given up");
  wait_message(bob, room, "live after g was given up");
  backlog_clear(&backlog);
  world_down(&w);
}

/* nostrc-iihf (w21-mls-prereqs closing review M1, P5): a busy group whose
 * stalled relay carries the live traffic. g answers the live REQ, then never
 * answers its older page; h has the backlog but serves nothing new, so every
 * live message arrives through g first. Live events must not restart g's
 * quiet period: with a 1 s quiet period and one message every 250 ms for
 * 5 s, the first live message is read while the burst is still going (it
 * used to wait until 1 s after the burst ended). */
typedef struct {
  App *alice;
  GhMlsGroup *group;
  guint sent;
  guint total;
} Burst;

static gboolean
burst_tick(gpointer data)
{
  Burst *burst = data;
  g_autofree gchar *text = g_strdup_printf("burst %u", burst->sent);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_mls_service_send(burst->alice->service, burst->group, text,
                                                     &error);
  g_assert_no_error(error);
  burst->sent++;
  return burst->sent < burst->total ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
test_busy_group_stalled_relay(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  w.g.max_limit = 50;
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  const gchar *relays[] = { w.g.url, w.h.url, NULL };
  GhMlsGroup *ga = create_group_on(alice, "Busy", relays, (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  WireStored *any = last_stored_445(&w.g) ? last_stored_445(&w.g) : last_stored_445(&w.h);
  g_assert_nonnull(any);
  g_autofree gchar *h = g_strdup(h_of(any));
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  gh_mls_service_set_backfill_quiet(bob->service, 1000);

  /* A backlog past g's cap of 50, so g pages; h has all but the newest. */
  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;
  w.h.withhold_new = TRUE;
  StoredCount on_h = { &w.h, h, w.h.stored->len, 0 };
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 60, 0, 0, &backlog);
  on_h.count = backlog.ids->len;
  spin_until(stored_reached, &on_h, "the backlog stored on h");
  w.g.withhold_new = FALSE;
  w.h.withhold_new = FALSE;
  gint64 start = real_now() - 25;
  for (guint i = 0; i < backlog.ids->len; i++) {
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, g_ptr_array_index(backlog.ids, i))->json,
                                      start + i / 3);
    wire_relay_inject(&w.g, copy);
    if (i + 1 < backlog.ids->len)
      wire_relay_inject(&w.h, copy);          /* the newest only on g: g holds the flush */
  }
  w.g.stall_pages = TRUE;                    /* g's older page is never answered */
  w.h.withhold_new = TRUE;                   /* the live traffic comes through g */

  set_online(bob, TRUE);
  wait_for_count(&w.g.stalled_reqs, 1);
  Burst burst = { alice, ga, 0, 20 };
  guint source = g_timeout_add(250, burst_tick, &burst);
  wait_message(bob, room, "burst 0");
  g_assert_cmpuint(burst.sent, <, burst.total);   /* read while the group is still busy */
  if (burst.sent < burst.total)
    g_source_remove(source);
  TextsWait all = { bob, room, backlog.texts, 0 };
  spin_until(texts_listed, &all, "every message of the backlog");
  g_assert_true(gh_mls_group_get_history_incomplete(gb));   /* g given up, cursor held */
  backlog_clear(&backlog);
  world_down(&w);
}

/* nostrc-kzun (after nostrc-dha5): a backlog of more than 200 kind 445s in
 * one stored answer -- 249 messages across 3 Commits, the first Commit
 * withheld and released last, as run_catch_up() does -- reaches the service
 * complete: all 251 served events are held at once (GNostrSubscription's old
 * 200-event queue dropped the oldest of such a burst), then every message
 * is read, the epochs match, nothing is left unreadable and the cursor
 * passes the backlog. (Re-signed three per second, oldest first, with Bob's
 * clock a day ahead, as in catch-up-past-relay-cap: libmarmot keeps 32
 * skipped keys per sender, and a relay's order inside one second is not the
 * sender's, so more than that in one second cannot be read in any order.) */
static void
test_catch_up_over_200(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Over 200", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  join(bob, ALICE);
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  gint base = (gint)gh_mls_group_get_unreadable(gb);

  set_online(bob, FALSE);
  w.g.withhold_new = TRUE;                  /* served only as re-signed below */
  Backlog backlog;
  make_backlog(&w, alice, ga, h, 0, 3, 83, &backlog);
  gint64 start = real_now() + 5, newest = 0;
  g_autofree gchar *first_commit = NULL;
  for (guint i = 0; i < backlog.ids->len; i++) {
    const gchar *id = g_ptr_array_index(backlog.ids, i);
    gboolean first = g_str_equal(id, backlog.first_commit);
    newest = start + i / 3;
    g_autofree gchar *copy = resigned(stored_by_id(&w.g, id)->json, newest);
    w.g.withhold_new = first;               /* the first Commit comes last */
    wire_relay_inject(&w.g, copy);
    if (first)
      first_commit = g_strdup(last_stored_445(&w.g)->id);
  }
  w.g.withhold_new = FALSE;
  guint served = backlog.ids->len - 1;
  g_assert_cmpuint(served, >, 200);
  g_assert_cmpuint(base + served, <=, GH_MLS_SERVICE_MAX_HELD);

  set_online(bob, TRUE);
  wait_live(gb);
  wait_unreadable(gb, base + (gint)served);          /* all of it arrived */
  wire_relay_release(&w.g, first_commit);           /* the first Commit, last */
  TextsWait all = { bob, room, backlog.texts, 0 };
  spin_until(texts_listed, &all, "every message of the backlog");
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_assert_cmpstr(gh_mls_group_get_name(gb), ==, "Backlog 3");
  g_assert_cmpint(gh_mls_group_get_unreadable(gb), ==, base);   /* none of the backlog */
  send_text(alice, ga, "after the backlog");
  wait_message(bob, room, "after the backlog");
  CursorWait moved = { gb, newest };
  spin_until(cursor_reached, &moved, "the cursor past the backlog");
  backlog_clear(&backlog);
  world_down(&w);
}

/* A group relay that fails during a catch-up may hold what the others lack:
 * until every relay answered, nothing moves the cursor. (Bob's store clock
 * runs a day ahead so that the cursor would visibly move.) */
static void
test_failed_relay_holds_the_cursor(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  const gchar *relays[] = { w.g.url, w.h.url, NULL };
  GhMlsGroup *ga = create_group_on(alice, "Two relays", relays, (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);
  gint64 before = gh_mls_group_get_cursor(gb);

  set_online(bob, FALSE);
  w.h.close_on_connect = TRUE;
  send_text(alice, ga, "while h is down");
  StatusWait on_g = { alice, room, "while h is down" };
  spin_until(sent, &on_g, "the message accepted by g");
  /* The same message under a later envelope date (the outer created_at is
   * not covered by MLS): accepted, and it would move the cursor. It is the
   * copy g serves (a backfill is applied oldest first, so with the original
   * served too the original would be read and this copy be a duplicate). */
  WireStored *original = last_stored_445(&w.g);
  g_autofree gchar *later = resigned(original->json, real_now() + 200);
  wire_relay_withhold(&w.g, original->id);
  wire_relay_inject(&w.g, later);
  set_online(bob, TRUE);
  wait_text(bob, room, "while h is down");
  wait_live(gb);
  g_assert_cmpint(gh_mls_group_get_cursor(gb), ==, before);    /* h has not answered */

  w.h.close_on_connect = FALSE;
  set_online(bob, FALSE);
  set_online(bob, TRUE);
  CursorWait moved = { gb, real_now() + 190 };
  spin_until(cursor_reached, &moved, "the cursor moving once every relay answered");
  world_down(&w);
}

/* The Commit that added Bob is on the relay, but Bob joined after it and
 * can never apply it: held, it must not pin his cursor at the join (review
 * N4). Bob's store clock runs a day ahead so that the cursor can move. */
static void
test_join_commit_pins_no_cursor(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Joined", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  gint64 added = real_now();   /* the Add Commit is no later */
  join(bob, ALICE);
  gh_clock_unref(bob->clock);
  bob->clock = gh_clock_new_fake(g_get_real_time() + (gint64)24 * 3600 * G_USEC_PER_SEC);
  app_restart(bob);
  GhMlsGroup *gb = gh_mls_service_lookup(bob->service, room);
  wait_live(gb);

  /* The same message under a later envelope date (as in the test above),
   * the only copy the relay serves to the catch-up. */
  set_online(bob, FALSE);
  send_text(alice, ga, "after the join");
  StatusWait done = { alice, room, "after the join" };
  spin_until(sent, &done, "the message sent");
  WireStored *original = last_stored_445(&w.g);
  g_autofree gchar *later = resigned(original->json, real_now() + 200);
  wire_relay_withhold(&w.g, original->id);
  wire_relay_inject(&w.g, later);
  set_online(bob, TRUE);
  wait_text(bob, room, "after the join");
  CursorWait moved = { gb, real_now() + 190 };
  spin_until(cursor_reached, &moved, "the cursor moving past the join");
  g_assert_cmpint(gh_mls_group_get_cursor(gb), >, added);
  world_down(&w);
}

/* ---- Account proof (review B2) ---------------------------------------------------------- */

#if GH_MLS_SERVICE_ACCOUNT_PROOF
static gboolean
identity_is(gpointer data)
{
  GroupWait *wait = data;   /* group: the service, value: the state */
  return (gint)gh_mls_service_get_identity_state((GhMlsService *)wait->group) == wait->value;
}

#define wait_identity(service_, state_) \
  G_STMT_START { GroupWait iw_ = { (GhMlsGroup *)(service_), (state_) }; \
    spin_until(identity_is, &iw_, "the identity state"); } G_STMT_END

static void
create_attempt(App *app, guint invitee, GError **out_error)
{
  const gchar *relays[] = { app->world->g.url, NULL };
  const gchar *people[] = { hex[invitee], NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(app->service, "Try", NULL, relays, people, NULL, on_created,
                                    &wait);
  spin_until(op_done, &wait, "the creation attempt");
  if (wait.result)
    g_object_unref(wait.result);
  *out_error = wait.error;
}

static void
count_notify(guint *count)
{
  (*count)++;
}

/* Proof requests (the kind:450 template) the signer holds unanswered. */
static guint
held_proofs(GhTestSigner *signer)
{
  guint n = 0;
  for (guint i = 0; i < signer->held->len; i++) {
    GDBusMethodInvocation *call = g_ptr_array_index(signer->held, i);
    const gchar *input = NULL, *account = NULL, *app = NULL;
    if (!g_str_equal(g_dbus_method_invocation_get_method_name(call), "SignEvent"))
      continue;
    g_variant_get(g_dbus_method_invocation_get_parameters(call), "(&s&s&s)", &input, &account,
                  &app);
    NostrEvent *event = nostr_event_new();
    if (nostr_event_deserialize_compact(event, input, NULL) == 1 &&
        nostr_event_get_kind(event) == 450)
      n++;
    nostr_event_free(event);
  }
  return n;
}

typedef struct {
  GhTestSigner *signer;
  guint n;
} ProofWait;

static gboolean
proofs_held(gpointer data)
{
  ProofWait *wait = data;
  return held_proofs(wait->signer) == wait->n;
}

#define wait_proofs(signer_, n_) \
  G_STMT_START { ProofWait pw_ = { (signer_), (n_) }; \
    spin_until(proofs_held, &pw_, "the proof requests the signer holds"); } G_STMT_END

static GhMlsGroup *
only_group(App *app)
{
  GListModel *groups = G_LIST_MODEL(app->service);
  g_assert_cmpuint(g_list_model_get_n_items(groups), ==, 1);
  g_autoptr(GhMlsGroup) group = g_list_model_get_item(groups, 0);
  return group;   /* the service keeps it */
}

/* libmarmot 0.10.0: each start enrolls the account proof through the
 * signer (the kind:450 template, never published); until the signer
 * answers nothing that needs the proof is made; a decline is honest. */
static void
test_account_proof_enrollment(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  g_assert_cmpint(gh_mls_service_get_identity_state(alice->service), ==,
                  GH_MLS_IDENTITY_ENROLLED);
  accept_contact(alice, BOB);

  /* A restart: a new instance key, so a new proof; the signer waits. */
  w.signer.hold = TRUE;
  app_restart(alice);
  wait_identity(alice->service, GH_MLS_IDENTITY_WAITING);
  g_autoptr(GError) error = NULL;
  create_attempt(alice, BOB, &error);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NOT_ENROLLED);
  g_clear_error(&error);
  g_assert_cmpint(gh_mls_service_get_key_package_state(alice->service), !=,
                  GH_MLS_KEY_PACKAGE_PUBLISHING);
  w.signer.hold = FALSE;
  gh_test_signer_release_all(&w.signer);
  wait_identity(alice->service, GH_MLS_IDENTITY_ENROLLED);
  accept_contact(alice, BOB);   /* an empty room is not stored: the restart forgot it */
  create_group(alice, "Proven", (const guint[]){ BOB }, 1);
  join(&w.apps[BOB], ALICE);

  /* Declined: honest, nothing made, not asked again this start. */
  w.signer.deny = TRUE;
  app_restart(alice);
  wait_identity(alice->service, GH_MLS_IDENTITY_DECLINED);
  create_attempt(alice, BOB, &error);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NOT_ENROLLED);
  g_clear_error(&error);

  /* A reconnect is not a reason to ask again (review N7): two network flaps
   * leave the decline standing, with no new proof request (the signer holds
   * every call, so what was asked can be read; relays still ask for AUTH). */
  guint changes = 0;
  gulong watch = g_signal_connect_swapped(alice->service, "notify::identity-state",
                                          G_CALLBACK(count_notify), &changes);
  w.signer.hold = TRUE;
  GhMlsGroup *proven = only_group(alice);
  for (guint i = 0; i < 2; i++) {
    set_online(alice, FALSE);
    set_online(alice, TRUE);
    wait_live(proven);
  }
  drain();
  g_assert_cmpuint(changes, ==, 0);
  g_assert_cmpuint(held_proofs(&w.signer), ==, 0);
  g_assert_cmpint(gh_mls_service_get_identity_state(alice->service), ==,
                  GH_MLS_IDENTITY_DECLINED);
  /* An explicit retry asks again, once: declined once more, then allowed. */
  g_assert_true(gh_mls_service_retry_identity(alice->service, NULL));
  wait_identity(alice->service, GH_MLS_IDENTITY_WAITING);
  wait_proofs(&w.signer, 1);
  gh_test_signer_release_all(&w.signer);
  wait_identity(alice->service, GH_MLS_IDENTITY_DECLINED);
  w.signer.deny = FALSE;
  g_assert_true(gh_mls_service_retry_identity(alice->service, NULL));
  wait_identity(alice->service, GH_MLS_IDENTITY_WAITING);
  wait_proofs(&w.signer, 1);
  gh_test_signer_release_all(&w.signer);
  wait_identity(alice->service, GH_MLS_IDENTITY_ENROLLED);
  w.signer.hold = FALSE;
  g_signal_handler_disconnect(alice->service, watch);

  /* A switch while the signer waits: the request dies with the account's
   * generation; coming back asks once more for the new one. */
  w.signer.hold = TRUE;
  app_restart(alice);
  wait_identity(alice->service, GH_MLS_IDENTITY_WAITING);
  wait_proofs(&w.signer, 1);
  g_settings_set_string(alice->settings, "current-npub", npub[STRANGER]);
  gh_account_controller_refresh(alice->accounts);
  wait_identity(alice->service, GH_MLS_IDENTITY_NONE);
  g_settings_set_string(alice->settings, "current-npub", npub[ALICE]);
  gh_account_controller_refresh(alice->accounts);
  wait_identity(alice->service, GH_MLS_IDENTITY_WAITING);
  wait_proofs(&w.signer, 2);
  w.signer.hold = FALSE;
  gh_test_signer_release_all(&w.signer);
  wait_identity(alice->service, GH_MLS_IDENTITY_ENROLLED);

  /* The template is local-only: no relay ever saw a kind 450. */
  WireRelay *relays[] = { &w.e, &w.w, &w.x, &w.g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    g_autoptr(GPtrArray) templates = published(relays[i], 450);
    g_assert_cmpuint(templates->len, ==, 0);
  }
  world_down(&w);
}

#define VERIFIED_ONLY "only-join-verified-mls-groups"

/* `key` runs an older client (MDK 0.8, libmarmot <= 0.9.0): a KeyPackage
 * without the account proof on W. Its event id (transfer full). */
static gchar *
inject_legacy_key_package(World *w, guint key)
{
  MarmotConfig config = marmot_config_default();
  config.allow_unproven_self = true;
  Marmot *legacy = marmot_new_with_config(marmot_storage_memory_new(), &config);
  guint8 pubkey[32];
  g_assert_true(nostr_hex2bin(pubkey, hex[key], sizeof pubkey));
  const char *relays[] = { w->w.url };
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  g_assert_cmpint(marmot_create_key_package_unsigned(legacy, pubkey, relays, 1, &made), ==,
                  MARMOT_OK);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, made.event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *signed_json = nostr_event_serialize_compact(event);
  gchar *id = event_id_dup(event);
  nostr_event_free(event);
  wire_relay_inject(&w->w, signed_json);
  free(signed_json);
  marmot_key_package_result_free(&made);
  marmot_free(legacy);
  return id;
}

static void
add_member(App *app, GhMlsGroup *group, guint key)
{
  const gchar *people[] = { hex[key], NULL };
  OpWait added = { 0 };
  gh_mls_service_add_members_async(app->service, group, people, NULL, on_changed, &added);
  spin_until(op_done, &added, "the Add");
  g_assert_no_error(added.error);
  g_assert_true(added.ok);
}

typedef struct {
  GhMlsGroup *group;
  guint key;
} IdentityWait;

static gboolean
identity_settled(gpointer data)
{
  IdentityWait *wait = data;
  g_auto(GStrv) members = gh_mls_group_dup_members(wait->group);
  return g_strv_contains((const gchar *const *)members, hex[wait->key]) &&
         gh_mls_group_get_member_identity(wait->group, hex[wait->key], NULL) !=
           GH_MLS_MEMBER_CHECKING;
}

/* What the group says of `key` once it is not CHECKING: `want`, added by
 * `added_by` (0: not known). */
static void
assert_identity(GhMlsGroup *group, guint key, GhMlsMemberIdentity want, guint added_by)
{
  IdentityWait wait = { group, key };
  spin_until(identity_settled, &wait, "the member's identity check");
  g_autofree gchar *by = NULL;
  g_assert_cmpint(gh_mls_group_get_member_identity(group, hex[key], &by), ==, want);
  g_assert_cmpstr(by, ==, added_by ? hex[added_by] : NULL);
}

/* REQs a relay got for `key`'s KeyPackages. */
static guint
key_package_reqs(WireRelay *relay, guint key)
{
  guint n = 0;
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, "30443") && strstr(frame->text, hex[key]))
      n++;
  }
  return n;
}

static gboolean
change_refused(gpointer data)
{
  return gh_mls_group_get_change_refused(data);
}

static gboolean
change_not_refused(gpointer data)
{
  return !gh_mls_group_get_change_refused(data);
}

/* nostrc-6ukh. A KeyPackage without the account proof (MDK 0.8, libmarmot
 * <= 0.9.0) can be invited by default, and the inviter knows the device
 * from the KeyPackage it used. Only when the account requires proofs is
 * the invitation refused, honestly ("needs an update"), with nothing
 * changed. */
static void
test_unproven_invitee(void)
{
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  g_autofree gchar *carol_kp = inject_legacy_key_package(&w, CAROL);
  accept_contact(alice, CAROL);

  /* Proofs required: refused before anything is made (nostrc-7gx7). */
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, TRUE);
  g_autoptr(GError) error = NULL;
  guint g_events = w.g.events;
  create_attempt(alice, CAROL, &error);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NEEDS_UPDATE);
  g_clear_error(&error);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  g_assert_cmpuint(w.g.events, ==, g_events);

  /* The default: Carol is invited, and Alice confirmed her device from the
   * KeyPackage she added her with. */
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, FALSE);
  GhMlsGroup *ga = create_group(alice, "Older app", (const guint[]){ CAROL }, 1);
  wait_members(ga, 2);
  assert_identity(ga, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);
  g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[ALICE], NULL), ==,
                  GH_MLS_MEMBER_PROVEN);
  g_assert_cmpuint(gh_mls_group_get_unverified_members(ga), ==, 0);
  world_down(&w);
}

typedef struct {
  gboolean done;
  GhMlsMemberIdentity identity;
  GError *error;
} VerifyWait;

static gboolean
verify_done_p(gpointer data)
{
  return ((VerifyWait *)data)->done;
}

static void
on_verified(GObject *source, GAsyncResult *result, gpointer data)
{
  VerifyWait *wait = data;
  wait->identity = gh_mls_service_verify_member_finish(GH_MLS_SERVICE(source), result,
                                                       &wait->error);
  wait->done = TRUE;
}

/* The user's Verify of `key` in `group` (W24 review H1). */
static GhMlsMemberIdentity
verify(App *app, GhMlsGroup *group, guint key, GError **error)
{
  VerifyWait wait = { 0 };
  gh_mls_service_verify_member_async(app->service, group, hex[key], NULL, on_verified, &wait);
  spin_until(verify_done_p, &wait, "the Verify");
  if (wait.error)
    g_propagate_error(error, wait.error);
  return wait.identity;
}

/* KeyPackage REQs (kinds 30443/443) naming `key`, on any of the world's
 * relays but the group relay, and on the group relay. */
static guint
all_key_package_reqs(World *w, guint key)
{
  return key_package_reqs(&w->e, key) + key_package_reqs(&w->w, key) +
         key_package_reqs(&w->x, key) + key_package_reqs(&w->g, key);
}

/* nostrc-juhs: the account's own Add can come back from the group relay
 * before that relay's OK, and libmarmot merges it on that echo (reporting no
 * committer: it is ours). The device it added is still the account's
 * addition, as when the OK comes first. */
static void
test_own_commit_echo_before_ok(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Echo first", (const guint[]){ BOB }, 1);
  /* An older client's device: who added it is all the group knows of it. */
  g_autofree gchar *carol_kp = inject_legacy_key_package(&w, CAROL);
  accept_contact(alice, CAROL);

  w.g.hold_oks = TRUE;
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait added = { 0 };
  gh_mls_service_add_members_async(alice->service, ga, people, NULL, on_changed, &added);
  wait_members(ga, 3);          /* merged on the echo */
  g_assert_false(added.done);   /* the relay has not answered */
  assert_identity(ga, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);

  wire_relay_release_oks(&w.g);
  spin_until(op_done, &added, "the Add");
  g_assert_no_error(added.error);
  g_assert_true(added.ok);
  assert_identity(ga, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);
  world_down(&w);
}

/* nostrc-6ukh, W24 review H1/M1/N3. Bob (default mode) sees members without
 * the proof that Alice added. With no evidence in hand they are UNVERIFIED,
 * "Added by" Alice, and nothing is looked up by itself: no KeyPackage REQ
 * from Bob at all, and none ever on the group relay. Bob's Verify asks the
 * discovery relays and the person's write relays: Carol, whose KeyPackage
 * is there, is VERIFIED; the stranger's is gone (replaced, deleted), so he
 * stays UNVERIFIED. Messages flow throughout. After a restart the verdicts
 * hold from the store and nothing is asked again. An admin's Remove + Add
 * of Carol's slot is a new device Alice added: Carol's verdict does not
 * carry over to it (only the device's own renewal would). Kind 10051 is
 * never asked for. */
static void
test_unproven_member_identity(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Mixed", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  g_assert_cmpint(gh_mls_group_get_member_identity(gb, hex[ALICE], NULL), ==,
                  GH_MLS_MEMBER_PROVEN);

  /* Carol and the stranger (older apps) join through Alice's Adds. */
  g_autofree gchar *carol_kp = inject_legacy_key_package(&w, CAROL);
  g_autofree gchar *stranger_kp = inject_legacy_key_package(&w, STRANGER);
  accept_contact(alice, CAROL);
  accept_contact(alice, STRANGER);
  add_member(alice, ga, CAROL);
  add_member(alice, ga, STRANGER);
  guint carol_asked = all_key_package_reqs(&w, CAROL);
  guint stranger_asked = all_key_package_reqs(&w, STRANGER);
  wait_members(gb, 4);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  drain();
  assert_identity(gb, CAROL, GH_MLS_MEMBER_UNVERIFIED, ALICE);
  assert_identity(gb, STRANGER, GH_MLS_MEMBER_UNVERIFIED, ALICE);
  g_assert_cmpuint(gh_mls_group_get_unverified_members(gb), ==, 2);
  assert_identity(ga, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);   /* her own KeyPackages */
  assert_identity(ga, STRANGER, GH_MLS_MEMBER_VERIFIED, ALICE);
  g_assert_cmpuint(all_key_package_reqs(&w, CAROL), ==, carol_asked);   /* nothing by itself */
  g_assert_cmpuint(all_key_package_reqs(&w, STRANGER), ==, stranger_asked);
  send_text(bob, gb, "despite unverified members");
  wait_text(alice, gh_mls_group_get_room_id(ga), "despite unverified members");

  /* Verify: Carol's KeyPackage matches; the stranger's is gone. */
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(verify(bob, gb, CAROL, &error), ==, GH_MLS_MEMBER_VERIFIED);
  g_assert_no_error(error);
  assert_identity(gb, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);
  g_assert_cmpuint(key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL), >,
                   carol_asked);
  g_assert_true(client_frames_mention(&w.w, ",443"));      /* the older kind too */
  wire_relay_withhold(&w.w, stranger_kp);
  g_assert_cmpint(verify(bob, gb, STRANGER, &error), ==, GH_MLS_MEMBER_UNVERIFIED);
  g_assert_no_error(error);
  assert_identity(gb, STRANGER, GH_MLS_MEMBER_UNVERIFIED, ALICE);
  g_assert_cmpuint(gh_mls_group_get_unverified_members(gb), ==, 1);
  /* A proven member needs no Verify. */
  verify(bob, gb, ALICE, &error);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  /* After a restart: from the store, and nothing asked again. */
  guint asked = all_key_package_reqs(&w, STRANGER) + all_key_package_reqs(&w, CAROL);
  app_restart(bob);
  gb = only_group(bob);
  assert_identity(gb, STRANGER, GH_MLS_MEMBER_UNVERIFIED, ALICE);
  assert_identity(gb, CAROL, GH_MLS_MEMBER_VERIFIED, ALICE);
  wait_live(gb);
  drain();
  g_assert_cmpuint(all_key_package_reqs(&w, STRANGER) + all_key_package_reqs(&w, CAROL), ==,
                   asked);

  /* An admin's Remove + Add of Carol's slot: a new device, added by Alice;
   * Carol's verdict stays with her old key (W24 review B1, M1). */
  g_autofree gchar *replace = forge_replace(alice, ga, CAROL);
  wire_relay_inject(&w.g, replace);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga) + 1);
  assert_identity(gb, CAROL, GH_MLS_MEMBER_UNVERIFIED, ALICE);

  /* Never the group relay, never kind 10051. */
  g_assert_cmpuint(key_package_reqs(&w.g, CAROL) + key_package_reqs(&w.g, STRANGER), ==, 0);

  /* What Bob knew of the stranger's device goes when he leaves (N3). */
  {
    const gchar *gid_hex = gh_mls_group_get_group_id(gb);
    gsize gid_len = strlen(gid_hex) / 2;
    g_autofree guint8 *gid_bytes = g_malloc(gid_len);
    g_assert_true(nostr_hex2bin(gid_bytes, gid_hex, gid_len));
    MarmotGroupId gid = marmot_group_id_new(gid_bytes, gid_len);
    MarmotMemberIdentity *ids = NULL;
    size_t n_ids = 0;
    g_assert_cmpint(marmot_get_group_member_identities(gh_mls_service_get_marmot(bob->service),
                                                       &gid, &ids, &n_ids), ==, MARMOT_OK);
    g_autofree gchar *device = NULL;
    for (size_t i = 0; i < n_ids; i++) {
      g_autofree gchar *account = g_malloc0(65);
      for (guint j = 0; j < 32; j++)
        g_snprintf(account + 2 * j, 3, "%02x", ids[i].account_pubkey[j]);
      if (g_str_equal(account, hex[STRANGER])) {
        device = g_malloc0(65);
        for (guint j = 0; j < 32; j++)
          g_snprintf(device + 2 * j, 3, "%02x", ids[i].signature_key[j]);
      }
    }
    free(ids);
    marmot_group_id_free(&gid);
    g_assert_nonnull(device);
    GhStoreMlsMember record;
    gboolean found = FALSE;
    g_assert_true(gh_store_mls_member_load(bob->store, gid_hex, hex[STRANGER], device,
                                           &record, &found, NULL));
    g_assert_true(found);
    gh_store_mls_member_clear(&record);
    g_autofree gchar *kept_gid = g_strdup(gid_hex);
    /* Leaving (nostrc-2um6) is not leaving yet: the record stays while the
     * group waits for a member to commit Bob's departure. (Alice cannot
     * here: the forged Commit above, made from her state outside her
     * service, moved Bob to an epoch her service never applied.) Leaving
     * again gives up waiting and ends the group on this device. */
    g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
    g_assert_no_error(error);
    g_assert_true(gh_mls_group_get_leaving(gb));
    g_assert_true(gh_store_mls_member_load(bob->store, kept_gid, hex[STRANGER], device,
                                           &record, &found, NULL));
    g_assert_true(found);
    gh_store_mls_member_clear(&record);
    g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
    g_assert_no_error(error);
    g_assert_true(group_ended(gb));
    g_assert_true(gh_store_mls_member_load(bob->store, kept_gid, hex[STRANGER], device,
                                           &record, &found, NULL));
    g_assert_false(found);
  }
  WireRelay *relays[] = { &w.e, &w.w, &w.x, &w.g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    g_assert_false(client_frames_mention(relays[i], "10051"));
  world_down(&w);
}

typedef struct {
  WireRelay *relay;
  guint count;
} Kind445Count;

static guint
count_445(WireRelay *relay)
{
  guint n = 0;
  for (guint i = 0; i < relay->stored->len; i++)
    n += nostr_event_get_kind(((WireStored *)g_ptr_array_index(relay->stored, i))->event) == 445;
  return n;
}

static gboolean
more_445(gpointer data)
{
  Kind445Count *wait = data;
  return count_445(wait->relay) > wait->count;
}

/* nostrc-prrl, W24 review L2/L4. Bob and Carol require proofs; Alice
 * (default mode) adds the stranger, whose app can't prove his account. Bob
 * refuses the Commit for good and says so ("change-refused", the cause
 * recorded: proofs required), never that it waits for an earlier change,
 * although Alice's next message, of the new epoch, can't be read. Carol,
 * refusing too, writes on at the old epoch: what Bob reads of it never moves
 * his read cursor past the refused Commit (the store clock frozen here, so
 * only the event's own time could move it), and the refusal survives a
 * restart. Turning the preference off applies the change: the stranger is
 * listed and Alice's message read. */
static void
test_refused_change_honest(void)
{
  World w;
  world_fake_clock = TRUE;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Strict", (const guint[]){ BOB, CAROL }, 2);
  GhMlsGroup *gb = join(bob, ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(gb));
  g_settings_set_boolean(bob->settings, VERIFIED_ONLY, TRUE);
  g_settings_set_boolean(carol->settings, VERIFIED_ONLY, TRUE);
  guint64 epoch = gh_mls_group_get_epoch(gb);

  g_autofree gchar *stranger_kp = inject_legacy_key_package(&w, STRANGER);
  accept_contact(alice, STRANGER);
  add_member(alice, ga, STRANGER);
  gint64 refused_at = nostr_event_get_created_at(last_stored_445(&w.g)->event);
  spin_until(change_refused, gb, "Bob refusing the Add");
  spin_until(change_refused, gc, "Carol refusing the Add");
  g_assert_cmpint(gh_mls_group_get_refusal(gb), ==, GH_MLS_REFUSAL_UNPROVEN);
  g_assert_cmpuint(gh_mls_group_get_epoch(gb), ==, epoch);
  wait_members(gb, 3);

  /* Carol at the old epoch, read by Bob as if sent much later (her own
   * copy withheld; the store clocks moved on): the cursor holds. */
  Kind445Count stored = { &w.g, count_445(&w.g) };
  w.g.withhold_new = TRUE;
  send_text(carol, gc, "carol at the old epoch");
  spin_until(more_445, &stored, "Carol's message on the group relay");
  WireStored *original = last_stored_445(&w.g);
  g_assert_true(g_hash_table_contains(w.g.withheld, original->id));
  w.g.withhold_new = FALSE;
  gh_clock_fake_advance(bob->clock, (gint64)2000 * G_USEC_PER_SEC);
  g_autofree gchar *later = resigned(original->json, refused_at + 1000);
  wire_relay_inject(&w.g, later);
  wait_text(bob, room, "carol at the old epoch");
  g_assert_cmpint(gh_mls_group_get_cursor(gb), <=, refused_at);

  send_text(alice, ga, "after the stranger joined");
  wait_unreadable(gb, 1);
  drain();
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));
  g_assert_true(gh_mls_group_get_active(gb));

  /* Kept across a restart, cause and all. */
  app_restart(bob);
  gb = only_group(bob);
  g_assert_true(gh_mls_group_get_change_refused(gb));
  g_assert_cmpint(gh_mls_group_get_refusal(gb), ==, GH_MLS_REFUSAL_UNPROVEN);

  g_settings_set_boolean(bob->settings, VERIFIED_ONLY, FALSE);
  spin_until(change_not_refused, gb, "the refused change applying");
  wait_members(gb, 4);
  wait_text(bob, room, "after the stranger joined");
  assert_identity(gb, STRANGER, GH_MLS_MEMBER_UNVERIFIED, ALICE);
  world_down(&w);
}

/* A leaf whose account proof does not verify is refused in the default
 * mode too (nostrc-7vyi, we6g): Alice's modified client adds a leaf
 * claiming Carol with a proof Alice signed. Bob refuses it for good, says
 * so, and shows nothing as waiting (an unreadable event dated later would
 * otherwise); Alice's next honest change moves the group on and clears it. */
static void
test_forged_member_refused(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Forged", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(gb);
  g_autofree gchar *h = g_strdup(h_of(last_stored_445(&w.g)));
  guint64 epoch = gh_mls_group_get_epoch(gb);
  guint base = gh_mls_group_get_unreadable(gb);

  g_autofree gchar *forged = forge_add_with_bad_proof(alice, ga, CAROL, ALICE);
  wire_relay_inject(&w.g, forged);
  spin_until(change_refused, gb, "Bob refusing the forged Add");
  g_assert_cmpint(gh_mls_group_get_refusal(gb), ==, GH_MLS_REFUSAL_BROKEN_PROOF);
  g_assert_cmpuint(gh_mls_group_get_epoch(gb), ==, epoch);
  g_auto(GStrv) members = gh_mls_group_dup_members(gb);
  g_assert_cmpuint(g_strv_length(members), ==, 2);
  g_autofree gchar *later = junk_445(h, 1, real_now() + 30);
  wire_relay_inject(&w.g, later);
  wait_unreadable(gb, base + 1);
  drain();
  g_assert_false(gh_mls_group_get_decrypt_pending(gb));

  rename_group(alice, ga, "Moved on");
  NameWait moved = { gb, "Moved on" };
  spin_until(name_is, &moved, "Bob applying Alice's honest change");
  g_assert_false(gh_mls_group_get_change_refused(gb));
  world_down(&w);
}

/* W24b slice H review L2. In an adopted group (made with the test hook:
 * Groundhog offers none yet), an admin's Commit libmarmot refuses for good
 * -- here Alice's modified client restating the active lifecycle, a
 * "redundant lifecycle update" MDK refuses too -- is "change-refused",
 * cause "unfollowable", kept across a restart; a non-admin's junk Commit
 * marks nothing. Re-review R4: once every other member (Alice, whose own
 * client never applied it) is read at the same epoch after it, the refusal
 * and its cursor hold are released; the group then moves on as usual. */
static void
test_adopted_change_refused(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  /* Bob's adopted KeyPackage, made by libmarmot in his own store (the
   * producer is build-gated; its test entry point). */
  Marmot *mb = gh_mls_service_get_marmot(bob->service);
  guint8 pk[32], sk[32];
  g_assert_true(nostr_hex2bin(pk, hex[BOB], sizeof pk));
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[BOB], sizeof sk));
  MarmotKeyPackageResult kp;
  memset(&kp, 0, sizeof kp);
  g_assert_cmpint(marmot_create_key_package_adopted_internal(mb, pk, sk, NULL, NULL, &kp), ==,
                  MARMOT_OK);
  memset(sk, 0, sizeof sk);
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *kps[] = { kp.event_json, NULL };
  OpWait created = { 0 };
  gh_mls_service_test_create_adopted_group_async(alice->service, "Adopted", relays, kps, NULL,
                                                 on_created, &created);
  spin_until(op_done, &created, "the adopted group creation");
  g_assert_no_error(created.error);
  GhMlsGroup *ga = created.result;
  g_object_unref(ga);   /* the service keeps it */
  marmot_key_package_result_free(&kp);
  GhMlsGroup *gb = join(bob, ALICE);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  guint64 epoch = gh_mls_group_get_epoch(gb);

  /* Bob (no admin) renames the group from a modified client: Alice drops
   * it, and marks nothing (every member refuses it). */
  static const guint8 profile[] = { 0x04, 'j', 'u', 'n', 'k', 0x00 };
  g_autofree gchar *junk = forge_adopted_update(bob, gb, 0x8001, profile, sizeof profile);
  wire_relay_inject(&w.g, junk);
  send_text(bob, gb, "after bob's junk");
  wait_text(alice, room, "after bob's junk");
  g_assert_false(gh_mls_group_get_change_refused(ga));

  /* Alice's redundant lifecycle update: Bob refuses it for good. */
  static const guint8 active[] = { 0x00 };
  g_autofree gchar *redundant = forge_adopted_update(alice, ga, 0x800c, active, sizeof active);
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, redundant, NULL), ==, 1);
  gint64 refused_at = nostr_event_get_created_at(ev);
  nostr_event_free(ev);
  wire_relay_inject(&w.g, redundant);
  spin_until(change_refused, gb, "Bob refusing Alice's lifecycle update");
  g_assert_cmpint(gh_mls_group_get_refusal(gb), ==, GH_MLS_REFUSAL_UNFOLLOWABLE);
  g_assert_cmpuint(gh_mls_group_get_epoch(gb), ==, epoch);
  g_assert_true(gh_mls_group_get_active(gb));

  /* Kept across a restart, cause and all. */
  app_restart(bob);
  gb = only_group(bob);
  g_assert_true(gh_mls_group_get_change_refused(gb));
  g_assert_cmpint(gh_mls_group_get_refusal(gb), ==, GH_MLS_REFUSAL_UNFOLLOWABLE);
  g_assert_cmpint(gh_mls_group_get_cursor(gb), <=, refused_at);

  /* Alice writes at the same epoch, a second later: every member but Bob
   * refused it, so nothing waits behind it any more. */
  g_usleep(1100 * 1000);
  send_text(alice, ga, "alice after her refused change");
  wait_text(bob, room, "alice after her refused change");
  spin_until(change_not_refused, gb, "the refusal released");
  CursorWait past = { gb, refused_at + 1 };
  spin_until(cursor_reached, &past, "the cursor past the refused Commit");

  /* Alice's honest change moves the group on. */
  rename_group(alice, ga, "Moved on");
  NameWait moved = { gb, "Moved on" };
  spin_until(name_is, &moved, "Bob applying Alice's honest change");
  g_assert_false(gh_mls_group_get_change_refused(gb));
  world_down(&w);
}
#else
/* libmarmot < 0.10.0 has no account proof: nothing to enroll. */
static void
test_account_proof_enrollment(void)
{
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  g_assert_cmpint(gh_mls_service_get_identity_state(w.apps[ALICE].service), ==,
                  GH_MLS_IDENTITY_NOT_REQUIRED);
  world_down(&w);
}
#endif

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

/* ---- Leaving (nostrc-2um6) ------------------------------------------------------------- */

typedef struct {
  GPtrArray *left;   /* hex of members reported gone */
} LeftLog;

static void
on_member_left(GhMlsGroup *group, const gchar *pubkey, gpointer data)
{
  (void)group;
  g_ptr_array_add(((LeftLog *)data)->left, g_strdup(pubkey));
}

static gboolean
is_leaving(gpointer data)
{
  return gh_mls_group_get_leaving(data);
}

/* Carol (not an admin) leaves for everyone while Alice and Bob are offline:
 * her SelfRemove survives a restart and is published again; when they come
 * back one of them commits it after the jitter, both report "member-left",
 * Carol's group ends as LEFT for good, and the two go on. Alice, an admin,
 * can only leave on this device (admins step down first). */
static void
test_member_leaves(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Leavers", (const guint[]){ BOB, CAROL }, 2);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  wait_live(ga);
  LeftLog alice_log = { g_ptr_array_new_with_free_func(g_free) };
  LeftLog bob_log = { g_ptr_array_new_with_free_func(g_free) };
  g_signal_connect(ga, "member-left", G_CALLBACK(on_member_left), &alice_log);
  g_signal_connect(gb, "member-left", G_CALLBACK(on_member_left), &bob_log);

  g_assert_cmpint(gh_mls_service_leave_kind(alice->service, ga), ==, GH_MLS_LEAVE_DEVICE_ADMIN);
  g_assert_cmpint(gh_mls_service_leave_kind(carol->service, gc), ==, GH_MLS_LEAVE_ADMINS);

  set_online(alice, FALSE);
  set_online(bob, FALSE);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(carol->service, gc, &error));
  g_assert_no_error(error);
  g_assert_true(gh_mls_group_get_leaving(gc));
  g_assert_true(gh_mls_group_get_leave_via_admin(gc));
  g_assert_true(gh_mls_group_get_active(gc));
  g_assert_cmpint(gh_mls_service_leave_kind(carol->service, gc), ==,
                  GH_MLS_LEAVE_DEVICE_WAITING);
  g_assert_null(gh_mls_service_send(carol->service, gc, "still here?", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  /* The leave request is durable: after a restart it is still Leaving. */
  app_restart(carol);
  gc = gh_mls_service_lookup(carol->service, room);
  g_assert_nonnull(gc);
  spin_until(is_leaving, gc, "Carol still leaving after a restart");

  set_online(alice, TRUE);
  set_online(bob, TRUE);
  wait_members(ga, 2);
  wait_members(gb, 2);
  spin_until(group_ended, gc, "Carol's leave confirmed");
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_LEFT);
  g_assert_false(gh_mls_group_get_leaving(gc));
  g_assert_null(gh_mls_group_get_removed_by(gc));
  g_assert_cmpuint(alice_log.left->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(alice_log.left, 0), ==, hex[CAROL]);
  g_assert_cmpuint(bob_log.left->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(bob_log.left, 0), ==, hex[CAROL]);
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, gh_mls_group_get_epoch(gb));

  send_text(alice, ga, "after carol left");
  wait_message(bob, room, "after carol left");
  send_text(bob, gb, "bob after carol left");
  wait_message(alice, room, "bob after carol left");
  drain();
  g_assert_null(find_message(carol, room, "after carol left"));

  /* Carol's end survives a restart. */
  app_restart(carol);
  gc = gh_mls_service_lookup(carol->service, room);
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_LEFT);
  g_assert_false(gh_mls_group_get_active(gc));

  /* An admin's Leave is on this device only, and says so. */
  g_assert_true(gh_mls_service_leave(alice->service, ga, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_mls_group_get_end(ga), ==, GH_MLS_GROUP_END_LEFT_DEVICE);
  g_assert_false(gh_mls_group_get_leaving(ga));
  g_signal_handlers_disconnect_by_data(ga, &alice_log);
  g_signal_handlers_disconnect_by_data(gb, &bob_log);
  g_ptr_array_unref(alice_log.left);
  g_ptr_array_unref(bob_log.left);
  world_down(&w);
}

typedef struct {
  WireRelay *relay;
  guint n;
} StoredAtLeast;

static gboolean
stored_at_least(gpointer data)
{
  StoredAtLeast *count = data;
  return count->relay->stored->len >= count->n;
}

static gboolean
group_left(gpointer data)
{
  return gh_mls_group_get_end(data) == GH_MLS_GROUP_END_LEFT;
}

typedef struct {
  GhMlsGroup *group;
  guint n;
} HeldMore;

static gboolean
held_more(gpointer data)
{
  HeldMore *wait = data;
  return gh_mls_group_get_unreadable(wait->group) >= wait->n;
}

static gboolean
leave_failed(gpointer data)
{
  return gh_mls_group_get_leave_failed(data);
}

/* nostrc-2um6 review H1: Bob, an admin who could commit Carol's leave,
 * receives Alice's Commit of it before the leave itself (here: withheld on
 * G until Bob caught up). He keeps the Commit, applies it once the proposal
 * arrives, and publishes no competing Commit of his own. */
static void
test_commit_before_proposal(void)
{
  World w;
  world_up(&w, TRIO, G_N_ELEMENTS(TRIO));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_key_packages(&w, TRIO, G_N_ELEMENTS(TRIO));
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Reordered", (const guint[]){ BOB, CAROL }, 2);
  GhMlsGroup *gb = join(bob, ALICE);
  GhMlsGroup *gc = join(carol, ALICE);
  const gchar *admins[] = { hex[ALICE], hex[BOB], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(is_admin, gb, "Bob becoming an admin");
  wait_epoch(gc, (gint)gh_mls_group_get_epoch(ga));

  set_online(bob, FALSE);
  StoredAtLeast on_g = { &w.g, w.g.stored->len + 1 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(carol->service, gc, &error));
  g_assert_no_error(error);
  spin_until(stored_at_least, &on_g, "Carol's leave on G");
  g_autofree gchar *proposal_id = g_strdup(last_stored_445(&w.g)->id);
  wait_members(ga, 2);   /* Alice commits it */
  spin_until(group_left, gc, "Carol's leave committed");

  /* Bob comes back to the Commit alone, and catches up completely (within
   * one catch-up, Groundhog applies events oldest first anyway): the
   * Commit is held, waiting for its proposal. */
  wire_relay_withhold(&w.g, proposal_id);
  guint64 before = gh_mls_group_get_epoch(gb);
  HeldMore more = { gb, gh_mls_group_get_unreadable(gb) + 1 };
  set_online(bob, TRUE);
  wait_live(gb);
  spin_until(held_more, &more, "Bob holding the Commit");
  g_assert_cmpuint(gh_mls_group_get_epoch(gb), ==, before);
  guint stored = w.g.stored->len;
  guint held = gh_mls_group_get_unreadable(gb);

  wire_relay_release(&w.g, proposal_id);
  wait_members(gb, 2);
  g_assert_cmpuint(gh_mls_group_get_epoch(gb), ==, gh_mls_group_get_epoch(ga));
  g_assert_cmpuint(gh_mls_group_get_unreadable(gb), ==, held - 1);   /* the Commit applied */
  /* Past Bob's longest jitter: still no Commit of his. */
  gint64 until = g_get_monotonic_time() +
                 (GH_MLS_SERVICE_DEPARTURE_JITTER_MIN_MS + 4 * GH_MLS_SERVICE_DEPARTURE_JITTER_PER_MEMBER_MS +
                  1000) * 1000;
  while (g_get_monotonic_time() < until)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpuint(w.g.stored->len, ==, stored);
  send_text(bob, gb, "after the reorder");
  wait_message(alice, gh_mls_group_get_room_id(ga), "after the reorder");
  world_down(&w);
}

/* nostrc-2um6 review L3: Carol's leave cannot go on once Alice makes her an
 * admin (admins cannot leave this way): the leave is dropped, the group
 * says so, and Carol can send again. */
static void
test_leave_failed(void)
{
  World w;
  const guint keys[] = { ALICE, CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *carol = &w.apps[CAROL];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Promoted", (const guint[]){ CAROL }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gc = join(carol, ALICE);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(carol->service, gc, &error));
  g_assert_true(gh_mls_group_get_leaving(gc));
  const gchar *admins[] = { hex[ALICE], hex[CAROL], NULL };
  OpWait promoted = { 0 };
  gh_mls_service_set_admins_async(alice->service, ga, admins, NULL, on_changed, &promoted);
  change(alice, &promoted);
  spin_until(leave_failed, gc, "Carol's leave dropped");
  g_assert_false(gh_mls_group_get_leaving(gc));
  g_assert_true(gh_mls_group_get_active(gc));
  send_text(carol, gc, "staying after all");
  wait_message(alice, room, "staying after all");
  /* Leaving again clears it: an admin now, on this device only. */
  g_assert_cmpint(gh_mls_service_leave_kind(carol->service, gc), ==, GH_MLS_LEAVE_DEVICE_ADMIN);
  g_assert_true(gh_mls_service_leave(carol->service, gc, &error));
  g_assert_false(gh_mls_group_get_leave_failed(gc));
  g_assert_cmpint(gh_mls_group_get_end(gc), ==, GH_MLS_GROUP_END_LEFT_DEVICE);
  world_down(&w);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mls-service/key-packages", test_key_packages);
  g_test_add_func("/groundhog/mls-service/group-lifecycle", test_group_lifecycle);
  g_test_add_func("/groundhog/mls-service/member-leaves", test_member_leaves);
  g_test_add_func("/groundhog/mls-service/commit-before-proposal", test_commit_before_proposal);
  g_test_add_func("/groundhog/mls-service/leave-failed", test_leave_failed);
  g_test_add_func("/groundhog/mls-service/restart-mid-commit", test_restart_mid_commit);
  g_test_add_func("/groundhog/mls-service/account-switch", test_account_switch);
  g_test_add_func("/groundhog/mls-service/future-replay-moves-no-cursor",
                  test_future_replay_moves_no_cursor);
  g_test_add_func("/groundhog/mls-service/held-until-commit", test_held_until_commit);
  g_test_add_func("/groundhog/mls-service/junk-does-not-evict", test_junk_does_not_evict);
  g_test_add_func("/groundhog/mls-service/decrypt-pending-honest", test_decrypt_pending_honest);
  g_test_add_func("/groundhog/mls-service/losing-removal-reactivates",
                  test_losing_removal_reactivates);
  g_test_add_func("/groundhog/mls-service/removal-from-held-queue", test_removal_from_held_queue);
  g_test_add_func("/groundhog/mls-service/contested-removal-stops-listening",
                  test_contested_removal_stops_listening);
  g_test_add_func("/groundhog/mls-service/join-reads-from-welcome",
                  test_join_reads_from_welcome);
  g_test_add_func("/groundhog/mls-service/send-republished-after-restart",
                  test_send_republished_after_restart);
  g_test_add_func("/groundhog/mls-service/same-text-two-groups", test_same_text_two_groups);
  g_test_add_func("/groundhog/mls-service/burst-then-commit-accepted",
                  test_burst_then_commit_accepted);
  g_test_add_func("/groundhog/mls-service/change-retried-after-event-rate",
                  test_change_retried_after_event_rate);
  g_test_add_func("/groundhog/mls-service/second-admin-invites", test_second_admin_invites);
  g_test_add_func("/groundhog/mls-service/catch-up-2-commits", test_catch_up_2);
  g_test_add_func("/groundhog/mls-service/catch-up-4-commits", test_catch_up_4);
  g_test_add_func("/groundhog/mls-service/catch-up-5-commits", test_catch_up_5);
  g_test_add_func("/groundhog/mls-service/catch-up-past-relay-cap", test_catch_up_past_relay_cap);
  g_test_add_func("/groundhog/mls-service/catch-up-two-relays-partial",
                  test_catch_up_two_relays_partial);
  g_test_add_func("/groundhog/mls-service/backfill-store-bounded", test_backfill_store_bounded);
  g_test_add_func("/groundhog/mls-service/stalled-relay-given-up", test_stalled_relay_given_up);
  g_test_add_func("/groundhog/mls-service/busy-group-stalled-relay",
                  test_busy_group_stalled_relay);
  g_test_add_func("/groundhog/mls-service/catch-up-over-200", test_catch_up_over_200);
  g_test_add_func("/groundhog/mls-service/join-commit-pins-no-cursor",
                  test_join_commit_pins_no_cursor);
  g_test_add_func("/groundhog/mls-service/failed-relay-holds-the-cursor",
                  test_failed_relay_holds_the_cursor);
  g_test_add_func("/groundhog/mls-service/account-proof-enrollment",
                  test_account_proof_enrollment);
#if GH_MLS_SERVICE_ACCOUNT_PROOF
  g_test_add_func("/groundhog/mls-service/unproven-member-identity",
                  test_unproven_member_identity);
  g_test_add_func("/groundhog/mls-service/own-commit-echo-before-ok",
                  test_own_commit_echo_before_ok);
  g_test_add_func("/groundhog/mls-service/refused-change-honest", test_refused_change_honest);
  g_test_add_func("/groundhog/mls-service/forged-member-refused", test_forged_member_refused);
  g_test_add_func("/groundhog/mls-service/adopted-change-refused", test_adopted_change_refused);
  g_test_add_func("/groundhog/mls-service/unproven-invitee",
                  test_unproven_invitee);
#endif
  gint rc = g_test_run();
  mls_world_finish();
  return rc;
}
