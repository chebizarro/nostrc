/* GhMlsService KeyPackage transport lifecycle (nostrc-0bdg; Marmot
 * foundation/key-packages.md, transports/nostr.md "KeyPackage publication"),
 * on the mls-world.h accounts and local relays. Unless a case says
 * otherwise, every account's kind 10002 splits its relays: W marked "write",
 * H unmarked, R marked "read" (world_split_lists).
 *
 *  - write-relays-only: the KeyPackage goes to the write-capable set, W
 *    ("write") and H (unmarked), never R ("read"), the 10050 inbox X, the
 *    group relay G or the discovery relay, in the strict shape of the
 *    producer's profile.
 *  - lookup-privacy: an invitation asks the discovery relay for the 10002
 *    only and the invitee's write-capable set for the KeyPackage -- but not
 *    H, the new group's relay -- never R, never another group relay.
 *  - rotation-ack-tied: a rotation reuses the `d` slot; a replacement no
 *    relay accepted keeps the old private init key; the first OK of a newer
 *    one deletes it.
 *  - delayed-welcome (MDK 0.8 producer): a Welcome to the old last-resort
 *    KeyPackage joins while its replacement is unconfirmed; once a relay
 *    accepted the replacement, a Welcome delayed in transit fails -- and
 *    that failure rotates nothing.
 *  - pending-invitations-defer-rotation (MDK 0.8 producer): a join's
 *    replacement is held while other received invitations are pending (made
 *    with the same last-resort KeyPackage), across a restart; the last one
 *    joins, then the replacement retires the old key.
 *  - rotation-held-for-pending-invitation, lifetime-rotation-held (review
 *    H1): the user's rotate and the lifetime rotation are held the same way.
 *  - hold-cap-expires (review H1/M1): the hold ends at its cap, by its timer.
 *  - failed-welcome-preserves (MDK 0.8 producer): a Welcome that fails keeps
 *    the key and rotates nothing; the inviter's next Welcome to the same
 *    KeyPackage joins.
 *  - fresh-accounts-invite-each-other, relay-list-never-replaced (review H2):
 *    two accounts with no lists at all set them up as onboarding does (the
 *    consented kind 10002 included) and invite each other; an existing 10002
 *    is never offered for replacement nor replaced.
 *
 * Built twice: with the MDK 0.8 producer, and with the adopted producer
 * (GH_MLS_ADOPTED_KEY_PACKAGES=1, the compile gate a release flips, with
 * libmarmot's ungated internal producer standing in for its own build gate).
 * Groundhog's groups are legacy-profile, which cannot add an adopted
 * KeyPackage yet (adopted Commits, nostrc-lf62): the Welcome cases run with
 * the MDK 0.8 producer only. */
#include "mls-world.h"

#include <nostr-keys.h>
#if GH_MLS_ADOPTED_KEY_PACKAGES
#include "kp_profile.h"   /* libmarmot's ungated adopted producer */
#endif

#define VERIFIED_ONLY "only-join-verified-mls-groups"

/* ---- helpers ------------------------------------------------------------------- */

typedef struct {
  WireRelay *relay;
  guint key;
  guint count;
} CountWait;

static guint n_key_packages(WireRelay *relay, guint key);

static gboolean
key_packages_reached(gpointer data)
{
  CountWait *wait = data;
  return n_key_packages(wait->relay, wait->key) >= wait->count;
}

static const gchar *
tag_value(NostrEvent *event, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

static guint
tag_count(NostrEvent *event, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(event);
  guint n = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++)
    if (g_strcmp0(nostr_tag_get(nostr_tags_get(tags, i), 0), key) == 0)
      n++;
  return n;
}

/* `key`'s kind-30443 events the relay kept, oldest first (borrowed). */
static GPtrArray *
stored_key_packages(WireRelay *relay, guint key)
{
  GPtrArray *out = g_ptr_array_new();
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(stored->event) == 30443 &&
        g_strcmp0(nostr_event_get_pubkey(stored->event), hex[key]) == 0)
      g_ptr_array_add(out, stored);
  }
  return out;
}

static guint
n_key_packages(WireRelay *relay, guint key)
{
  g_autoptr(GPtrArray) all = stored_key_packages(relay, key);
  return all->len;
}

/* The newest kept KeyPackage of `key` on the relay: its `i` (ref) and `d`. */
static void
newest_key_package(WireRelay *relay, guint key, gchar **ref, gchar **d, gchar **json)
{
  g_autoptr(GPtrArray) all = stored_key_packages(relay, key);
  g_assert_cmpuint(all->len, >, 0);
  WireStored *newest = g_ptr_array_index(all, all->len - 1);
  if (ref)
    *ref = g_strdup(tag_value(newest->event, "i"));
  if (d)
    *d = g_strdup(tag_value(newest->event, "d"));
  if (json)
    *json = g_strdup(newest->json);
}

/* Whether any frame a client sent the relay mentions text. */
static gboolean
inbound_mentions(WireRelay *relay, const gchar *text)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && strstr(frame->text, text))
      return TRUE;
  }
  return FALSE;
}

/* REQs the relay got for `key`'s KeyPackages. */
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

typedef struct {
  App *app;
  GhMlsKeyPackageState state;
} StateWait;

static gboolean
state_is(gpointer data)
{
  StateWait *wait = data;
  return gh_mls_service_get_key_package_state(wait->app->service) == wait->state;
}

#define wait_state(app_, state_) \
  G_STMT_START { StateWait sw_ = { (app_), (state_) }; \
    spin_until(state_is, &sw_, "the KeyPackage state " #state_); } G_STMT_END

typedef struct {
  App *app;
  const gchar *old_id;
} RotatedWait;

static G_GNUC_UNUSED gboolean
rotated(gpointer data)
{
  RotatedWait *wait = data;
  const gchar *id = gh_mls_service_get_key_package_id(wait->app->service);
  return id && g_strcmp0(id, wait->old_id) != 0 &&
         gh_mls_service_get_key_package_state(wait->app->service) ==
           GH_MLS_KEY_PACKAGE_PUBLISHED;
}

static void
rotate(App *app)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_rotate_key_package(app->service, &error));
  g_assert_no_error(error);
}

/* The account's KeyPackage is published and kept on W. */
static void
wait_published(App *app)
{
  spin_until(key_package_published, app, "a published KeyPackage");
}

static gboolean
has_init_key(App *app, const gchar *ref)
{
  return gh_mls_service_test_has_init_key(app->service, ref);
}

/* When the service last recorded its KeyPackage published (store cursor
 * "mls/key-package"); a rotation request sets it to 0 at once. */
static G_GNUC_UNUSED gint64
key_package_cursor(App *app)
{
  gint64 at = -1;
  g_assert_true(gh_store_get_cursor(app->store, "mls/key-package", "", &at, NULL));
  return at;
}

/* Starts a group creation of `app` on `relay` with `invitees`; waits for the
 * end. */
static GhMlsGroup *
create_attempt_on(App *app, const gchar *relay, const gchar *name, const guint *invitees,
                  guint n, GError **error)
{
  g_autoptr(GPtrArray) people = g_ptr_array_new();
  for (guint i = 0; i < n; i++)
    g_ptr_array_add(people, hex[invitees[i]]);
  g_ptr_array_add(people, NULL);
  const gchar *relays[] = { relay, NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(app->service, name, "a test group", relays,
                                    (const gchar *const *)people->pdata, NULL, on_created,
                                    &wait);
  spin_until(op_done, &wait, "the group creation");
  if (wait.error)
    g_propagate_error(error, wait.error);
  if (wait.result)
    g_object_unref(wait.result);   /* the service keeps it */
  return wait.result;
}

/* The same on the group relay G. */
static G_GNUC_UNUSED GhMlsGroup *
create_attempt(App *app, const gchar *name, const guint *invitees, guint n, GError **error)
{
  return create_attempt_on(app, app->world->g.url, name, invitees, n, error);
}

typedef struct {
  App *app;
  guint n;
} InvitesWait;

static G_GNUC_UNUSED gboolean
invites_at_least(gpointer data)
{
  InvitesWait *wait = data;
  return wait->app->invites >= wait->n;
}

/* The account's pending invitation from `inviter` (asserted), its wrapper. */
static G_GNUC_UNUSED gchar *
invite_from(App *app, guint inviter)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(app->service, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < invites->len; i++) {
    GhMlsInvite *invite = g_ptr_array_index(invites, i);
    if (g_strcmp0(invite->inviter, hex[inviter]) == 0)
      return g_strdup(invite->wrapper_id);
  }
  g_assert_not_reached();
  return NULL;
}

/* The account's KeyPackage replacement settled: a new one (not old_id)
 * published, no rotation pending or in flight (the publication time is
 * recorded only then), and it is the newest on W. A retry timer armed by an
 * earlier refused publish can add a rotation of its own: this waits it out. */
typedef struct {
  App *app;
  const gchar *old_id;
} SettledWait;

static gboolean
settled(gpointer data)
{
  SettledWait *wait = data;
  App *app = wait->app;
  const gchar *id = gh_mls_service_get_key_package_id(app->service);
  if (!id || g_strcmp0(id, wait->old_id) == 0 ||
      gh_mls_service_get_key_package_state(app->service) != GH_MLS_KEY_PACKAGE_PUBLISHED ||
      key_package_cursor(app) <= 0)
    return FALSE;
  g_autoptr(GPtrArray) all = stored_key_packages(&app->world->w, app->key);
  return all->len > 0 &&
         g_str_equal(((WireStored *)g_ptr_array_index(all, all->len - 1))->id, id);
}

#define wait_settled(app_, old_id_) \
  G_STMT_START { SettledWait stw_ = { (app_), (old_id_) }; \
    spin_until(settled, &stw_, "the KeyPackage replacement settled"); } G_STMT_END

/* A due replacement held back: still the first KeyPackage, published and
 * kept, and nothing new on W. */
static G_GNUC_UNUSED void
assert_held(World *w, App *app, const gchar *ref, const gchar *id)
{
  g_assert_true(gh_mls_service_get_key_package_held(app->service));
  g_assert_cmpint(gh_mls_service_get_key_package_state(app->service), ==,
                  GH_MLS_KEY_PACKAGE_PUBLISHED);
  /* The id is known again after a restart only once one is accepted. */
  const gchar *now_id = gh_mls_service_get_key_package_id(app->service);
  g_assert_true(now_id == NULL || g_str_equal(now_id, id));
  g_assert_true(has_init_key(app, ref));
  drain();
  g_assert_cmpuint(n_key_packages(&w->w, app->key), ==, 1);
}

typedef struct {
  App *app;
  gint64 age;
} AgedWait;

/* The account's last recorded publication is at least `age` seconds old. */
static G_GNUC_UNUSED gboolean
key_package_aged(gpointer data)
{
  AgedWait *wait = data;
  gint64 at = key_package_cursor(wait->app);
  return at > 0 && g_get_real_time() / G_USEC_PER_SEC - at >= wait->age;
}

/* ---- the producer's shape -------------------------------------------------------- */

/* The published KeyPackage in the producer profile's strict form; for the
 * MDK 0.8 producer its `relays` tag names only the write-capable relays. */
static void
assert_strict_shape(World *w, const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpuint(tag_count(event, "d"), ==, 1);
  g_assert_cmpuint(tag_count(event, "i"), ==, 1);
  g_assert_cmpuint(tag_count(event, "mls_protocol_version"), ==, 1);
  g_assert_cmpuint(tag_count(event, "mls_ciphersuite"), ==, 1);
#if GH_MLS_ADOPTED_KEY_PACKAGES
  g_assert_cmpint(marmot_validate_key_package_event_json(json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                         0, NULL, NULL), ==, MARMOT_OK);
  g_assert_cmpuint(tag_count(event, "encoding"), ==, 0);
  g_assert_cmpuint(tag_count(event, "relays"), ==, 0);
  g_assert_cmpuint(tag_count(event, "app_components"), ==, 1);
  (void)w;
#else
  g_assert_cmpint(marmot_validate_key_package_event_json(json, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                         0, NULL, NULL), ==, MARMOT_OK);
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get(tag, 0), "relays") != 0)
      continue;
    g_assert_cmpuint(nostr_tag_size(tag), ==, 3);
    for (guint j = 1; j < nostr_tag_size(tag); j++)
      g_assert_true(g_str_equal(nostr_tag_get(tag, j), w->w.url) ||
                    g_str_equal(nostr_tag_get(tag, j), w->h.url));
  }
#endif
  nostr_event_free(event);
}

/* ---- tests ------------------------------------------------------------------------ */

/* Publishing: the account's write-capable set only. */
static void
test_write_relays_only(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(alice);
  g_assert_cmpuint(n_key_packages(&w.w, ALICE), ==, 1);
  CountWait unmarked = { &w.h, ALICE, 1 };
  spin_until(key_packages_reached, &unmarked, "the KeyPackage on the unmarked relay");
  WireRelay *never[] = { &w.r, &w.x, &w.g, &w.e };
  for (guint i = 0; i < G_N_ELEMENTS(never); i++) {
    g_assert_cmpuint(n_key_packages(never[i], ALICE), ==, 0);
    g_assert_false(inbound_mentions(never[i], "30443"));
    g_assert_false(inbound_mentions(never[i], "10051"));
  }
  g_autofree gchar *json = NULL;
  newest_key_package(&w.w, ALICE, NULL, NULL, &json);
  assert_strict_shape(&w, json);
  world_down(&w);
}

/* Discovery: the 10002 from the discovery relay, the KeyPackage from the
 * invitee's write relay only; never the read relay, never the group relay. */
static void
test_lookup_privacy(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(alice);
  wait_published(&w.apps[BOB]);
  accept_contact(alice, BOB);
  /* Bob's unmarked relay H is the new group's relay too. */
  CountWait on_h = { &w.h, BOB, 1 };
  spin_until(key_packages_reached, &on_h, "Bob's KeyPackage on H");
  g_autoptr(GError) error = NULL;
  GhMlsGroup *group = create_attempt_on(alice, w.h.url, "Private", (const guint[]){ BOB }, 1,
                                        &error);
#if GH_MLS_ADOPTED_KEY_PACKAGES
  /* A legacy-profile group cannot add an adopted KeyPackage (yet). */
  g_assert_null(group);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE);
#else
  g_assert_no_error(error);
  g_assert_nonnull(group);
#endif
  g_assert_cmpuint(key_package_reqs(&w.w, BOB), >=, 1);
  g_assert_true(client_frames_mention(&w.e, hex[BOB]));   /* the 10002 */
  g_assert_cmpuint(key_package_reqs(&w.e, BOB), ==, 0);
  g_assert_cmpuint(key_package_reqs(&w.h, BOB), ==, 0);
  WireRelay *never[] = { &w.r, &w.g, &w.x };
  for (guint i = 0; i < G_N_ELEMENTS(never); i++) {
    g_assert_cmpuint(key_package_reqs(never[i], BOB), ==, 0);
    g_assert_false(inbound_mentions(never[i], "30443"));
  }
  g_assert_false(inbound_mentions(&w.e, "10051"));
  world_down(&w);
}

/* Rotation keeps the slot; a replacement no relay accepted keeps the old
 * init key; the first OK of a newer one deletes it. */
static void
test_rotation_ack_tied(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(alice);
  g_autofree gchar *ref1 = NULL, *d1 = NULL;
  newest_key_package(&w.w, ALICE, &ref1, &d1, NULL);
  g_assert_true(has_init_key(alice, ref1));
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(alice->service));

  /* The relay rejects the replacement: nothing confirmed, nothing retired. */
  w.w.refuse_events = w.h.refuse_events = TRUE;
  rotate(alice);
  wait_state(alice, GH_MLS_KEY_PACKAGE_FAILED);
  g_assert_cmpuint(w.w.refused_events, >=, 1);
  g_assert_cmpuint(n_key_packages(&w.w, ALICE), ==, 1);
  g_assert_true(has_init_key(alice, ref1));

  /* Accepted now: the old init key goes with the first OK. */
  w.w.refuse_events = w.h.refuse_events = FALSE;
  rotate(alice);
  wait_settled(alice, id1);
  g_autofree gchar *ref2 = NULL, *d2 = NULL, *json = NULL;
  newest_key_package(&w.w, ALICE, &ref2, &d2, &json);
  g_assert_cmpstr(d1, ==, d2);
  g_assert_cmpstr(ref1, !=, ref2);
  g_assert_false(has_init_key(alice, ref1));
  g_assert_true(has_init_key(alice, ref2));
  assert_strict_shape(&w, json);

  /* A restart keeps the current one and retires nothing more. */
  app_restart(alice);
  wait_published(alice);
  g_assert_true(has_init_key(alice, ref2));
  world_down(&w);
}

#if !GH_MLS_ADOPTED_KEY_PACKAGES
/* The relay's withheld events (wire_relay_withhold()), all released. */
static void
release_withheld(WireRelay *relay)
{
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer id;
  g_hash_table_iter_init(&iter, relay->withheld);
  while (g_hash_table_iter_next(&iter, &id, NULL))
    g_ptr_array_add(ids, g_strdup(id));
  for (guint i = 0; i < ids->len; i++)
    wire_relay_release(relay, g_ptr_array_index(ids, i));
}

static gboolean
withheld_some(gpointer data)
{
  WireRelay *relay = data;
  return relay->withheld && g_hash_table_size(relay->withheld) > 0;
}

/* The delayed-Welcome trade-off, both sides. */
static void
test_delayed_welcome(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE, BOB, CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_published(alice);
  wait_published(bob);
  wait_published(carol);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  accept_contact(alice, BOB);
  accept_contact(carol, BOB);

  /* No relay accepts Bob's KeyPackages for now. */
  w.w.refuse_events = w.h.refuse_events = TRUE;

  /* Alice invites Bob with KeyPackage 1. */
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "First", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  InvitesWait one = { bob, 1 };
  spin_until(invites_at_least, &one, "Alice's invitation");
  /* Carol invites Bob with KeyPackage 1 too; her Welcome is delayed in
   * transit (the inbox relay withholds it). */
  w.x.withhold_new = TRUE;
  g_assert_nonnull(create_attempt(carol, "Delayed", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  spin_until(withheld_some, &w.x, "Carol's Welcome on the inbox relay");
  w.x.withhold_new = FALSE;

  /* Bob joins Alice's group; the join's replacement is refused: within its
   * window the old last-resort key stays. */
  g_autofree gchar *first = invite_from(bob, ALICE);
  GhMlsGroup *joined = gh_mls_service_accept_invite(bob->service, first, &error);
  g_assert_no_error(error);
  g_assert_nonnull(joined);
  wait_state(bob, GH_MLS_KEY_PACKAGE_FAILED);
  g_assert_true(has_init_key(bob, ref1));

  /* A relay accepts a replacement: the old key goes. */
  w.w.refuse_events = w.h.refuse_events = FALSE;
  g_autofree gchar *id_before = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  rotate(bob);
  wait_settled(bob, id_before);
  g_assert_false(has_init_key(bob, ref1));
  g_autofree gchar *ref_now = NULL;
  newest_key_package(&w.w, BOB, &ref_now, NULL, NULL);
  g_assert_true(has_init_key(bob, ref_now));

  /* Carol's Welcome to KeyPackage 1 arrives now: it fails, and rotates
   * nothing (the spec's deliberate trade-off; Carol retries with the
   * current KeyPackage). */
  g_autofree gchar *id_now = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  gint64 cursor_now = key_package_cursor(bob);
  g_assert_cmpint(cursor_now, >, 0);
  guint published_before = n_key_packages(&w.w, BOB);
  bob->invites = 0;
  release_withheld(&w.x);
  spin_until(invites_at_least, &one, "Carol's delayed invitation");
  g_autofree gchar *delayed = invite_from(bob, CAROL);
  GhMlsGroup *late = gh_mls_service_accept_invite(bob->service, delayed, &error);
  g_assert_null(late);
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_cmpint(gh_mls_service_get_key_package_state(bob->service), ==,
                  GH_MLS_KEY_PACKAGE_PUBLISHED);
  g_assert_cmpstr(gh_mls_service_get_key_package_id(bob->service), ==, id_now);
  g_assert_cmpint(key_package_cursor(bob), ==, cursor_now);   /* no rotation asked */
  g_assert_true(has_init_key(bob, ref_now));
  drain();
  g_assert_cmpuint(n_key_packages(&w.w, BOB), ==, published_before);
  (void)carol;
  world_down(&w);
}

/* Invitations already received were made with the same last-resort
 * KeyPackage: the join's replacement is held while one is pending, so each
 * of them still joins; then the replacement retires the old key. */
static void
test_pending_invitations_defer_rotation(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE, BOB, CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB], *carol = &w.apps[CAROL];
  wait_published(alice);
  wait_published(bob);
  wait_published(carol);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  accept_contact(alice, BOB);
  accept_contact(carol, BOB);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  g_assert_nonnull(create_attempt(carol, "Two", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  InvitesWait both = { bob, 2 };
  spin_until(invites_at_least, &both, "both invitations");

  g_autofree gchar *one = invite_from(bob, ALICE);
  g_assert_nonnull(gh_mls_service_accept_invite(bob->service, one, &error));
  g_assert_no_error(error);
  /* Carol's invitation is pending: the replacement is held. */
  assert_held(&w, bob, ref1, id1);

  /* A restart keeps it held. */
  app_restart(bob);
  wait_published(bob);
  assert_held(&w, bob, ref1, id1);

  g_autofree gchar *two = invite_from(bob, CAROL);
  g_assert_nonnull(gh_mls_service_accept_invite(bob->service, two, &error));
  g_assert_no_error(error);
  /* None pending: the replacement now, and its OK retires KeyPackage 1. */
  wait_settled(bob, id1);
  g_assert_false(gh_mls_service_get_key_package_held(bob->service));
  g_assert_false(has_init_key(bob, ref1));
  world_down(&w);
}

/* Review H1, the reviewer's probe: a rotation the user asks for while an
 * invitation is pending is held; the invitation still joins; then the
 * replacement is published and retires the old key. */
static void
test_rotation_held_for_pending_invitation(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(alice);
  wait_published(bob);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  accept_contact(alice, BOB);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  InvitesWait one_wait = { bob, 1 };
  spin_until(invites_at_least, &one_wait, "the invitation");
  g_assert_true(gh_mls_service_rotate_key_package(bob->service, &error));
  g_assert_no_error(error);
  assert_held(&w, bob, ref1, id1);
  g_autofree gchar *one = invite_from(bob, ALICE);
  GhMlsGroup *joined = gh_mls_service_accept_invite(bob->service, one, &error);
  g_assert_no_error(error);
  g_assert_nonnull(joined);
  wait_settled(bob, id1);
  g_assert_false(has_init_key(bob, ref1));
  world_down(&w);
}

/* Review H1, the lifetime path: a KeyPackage older than the rotation age,
 * looked at again (here at a restart) while an invitation is pending, is
 * not replaced; the invitation joins. */
static void
test_lifetime_rotation_held(void)
{
  World w;
  world_split_lists = TRUE;
  world_key_package_lifetime = 2;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(alice);
  wait_published(bob);
  accept_contact(alice, BOB);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  InvitesWait one_wait = { bob, 1 };
  spin_until(invites_at_least, &one_wait, "the invitation");
  /* The KeyPackage Alice used (a short rotation age: read it now). */
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  /* Past the rotation age; the restart's check finds it due. */
  AgedWait aged = { bob, 3 };
  spin_until(key_package_aged, &aged, "Bob's KeyPackage past its rotation age");
  app_restart(bob);
  StateWait published = { bob, GH_MLS_KEY_PACKAGE_PUBLISHED };
  spin_until(state_is, &published, "the KeyPackage state after a restart");
  assert_held(&w, bob, ref1, id1);
  g_autofree gchar *one = invite_from(bob, ALICE);
  g_assert_nonnull(gh_mls_service_accept_invite(bob->service, one, &error));
  g_assert_no_error(error);
  wait_settled(bob, id1);
  g_assert_false(has_init_key(bob, ref1));
  world_down(&w);
}

/* Review H1/M1, the cap: a replacement is held at most
 * key_package_max_hold; then it is published (its timer, no other
 * trigger) and the still-pending invitation can no longer join -- the
 * spec's bound wins over the wait. */
static void
test_hold_cap_expires(void)
{
  World w;
  world_split_lists = TRUE;
  world_key_package_max_hold = 2;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(alice);
  wait_published(bob);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  accept_contact(alice, BOB);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "One", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  InvitesWait one_wait = { bob, 1 };
  spin_until(invites_at_least, &one_wait, "the invitation");
  g_assert_true(gh_mls_service_rotate_key_package(bob->service, &error));
  g_assert_no_error(error);
  assert_held(&w, bob, ref1, id1);
  wait_settled(bob, id1);
  g_assert_false(gh_mls_service_get_key_package_held(bob->service));
  g_assert_false(has_init_key(bob, ref1));
  g_autofree gchar *one = invite_from(bob, ALICE);
  g_assert_null(gh_mls_service_accept_invite(bob->service, one, &error));
  g_assert_nonnull(error);
  world_down(&w);
}

typedef struct {
  App *app;
  const gchar *old_id;
} HeldOrNew;

static gboolean
held_or_new(gpointer data)
{
  HeldOrNew *wait = data;
  const gchar *id = gh_mls_service_get_key_package_id(wait->app->service);
  return gh_mls_service_get_key_package_held(wait->app->service) ||
         (id && g_strcmp0(id, wait->old_id) != 0);
}

/* Re-review A3 (review L1): an invitation list that cannot be read counts
 * as pending, never as "none": the rotation is held; once it can be read
 * again (and none is pending) the replacement goes out. */
static void
test_invitation_listing_error_holds(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(alice);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, ALICE, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(alice->service));
  gh_mls_service_test_fail_invitation_listing(TRUE);
  rotate(alice);
  /* The first publish may still be in flight (its other relay): the
   * rotation is looked at when it ends. */
  HeldOrNew outcome = { alice, id1 };
  spin_until(held_or_new, &outcome, "the rotation held or published");
  assert_held(&w, alice, ref1, id1);
  gh_mls_service_test_fail_invitation_listing(FALSE);
  rotate(alice);   /* looked at again */
  wait_settled(alice, id1);
  g_assert_false(gh_mls_service_get_key_package_held(alice->service));
  world_down(&w);
}

/* `key` runs an older client: a KeyPackage without the account proof on W. */
static void
inject_unproven_key_package(World *w, guint key)
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
  nostr_event_free(event);
  wire_relay_inject(&w->w, signed_json);
  free(signed_json);
  marmot_key_package_result_free(&made);
  marmot_free(legacy);
}

/* A Welcome whose processing fails keeps the key and rotates nothing; the
 * inviter's next Welcome to the same KeyPackage joins. */
static void
test_failed_welcome_preserves(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_published(alice);
  wait_published(bob);
  g_autofree gchar *ref1 = NULL;
  newest_key_package(&w.w, BOB, &ref1, NULL, NULL);
  g_autofree gchar *id1 = g_strdup(gh_mls_service_get_key_package_id(bob->service));
  gint64 cursor1 = key_package_cursor(bob);
  g_assert_cmpint(cursor1, >, 0);
  inject_unproven_key_package(&w, CAROL);
  accept_contact(alice, BOB);
  accept_contact(alice, CAROL);

  /* Bob only joins groups whose members all prove their account: Alice's
   * group with Carol's older app fails to join. */
  g_settings_set_boolean(bob->settings, VERIFIED_ONLY, TRUE);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "Mixed", (const guint[]){ BOB, CAROL }, 2, &error));
  g_assert_no_error(error);
  InvitesWait one = { bob, 1 };
  spin_until(invites_at_least, &one, "the first invitation");
  g_autofree gchar *mixed = invite_from(bob, ALICE);
  g_assert_null(gh_mls_service_accept_invite(bob->service, mixed, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_true(has_init_key(bob, ref1));
  g_assert_cmpint(gh_mls_service_get_key_package_state(bob->service), ==,
                  GH_MLS_KEY_PACKAGE_PUBLISHED);
  g_assert_cmpstr(gh_mls_service_get_key_package_id(bob->service), ==, id1);
  g_assert_cmpint(key_package_cursor(bob), ==, cursor1);   /* no rotation asked */
  drain();
  g_assert_cmpuint(n_key_packages(&w.w, BOB), ==, 1);

  /* Alice retries with the same KeyPackage: Bob joins. */
  bob->invites = 0;
  g_assert_nonnull(create_attempt(alice, "Again", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  spin_until(invites_at_least, &one, "the second invitation");
  g_autofree gchar *again = invite_from(bob, ALICE);
  GhMlsGroup *joined = gh_mls_service_accept_invite(bob->service, again, &error);
  g_assert_no_error(error);
  g_assert_nonnull(joined);
  /* The successful join rotates; the replacement's OK retires KeyPackage 1. */
  RotatedWait done = { bob, id1 };
  spin_until(rotated, &done, "Bob's replacement after the join");
  g_assert_false(has_init_key(bob, ref1));
  world_down(&w);
}
#endif

#if GH_TEST_HAVE_INBOX_SETUP && !GH_MLS_ADOPTED_KEY_PACKAGES
#include "gh-inbox-setup.h"
#include "gh-relay-list-setup.h"

static gboolean
setup_finished(gpointer data)
{
  GhInboxSetupState state = gh_inbox_setup_get_state(data);
  return state == GH_INBOX_SETUP_DONE || state == GH_INBOX_SETUP_FAILED;
}

/* What onboarding's Publish does (GhOnboardingView): the message relays and,
 * when offered and agreed to, the relay list. The setup (transfer full). */
static GhInboxSetup *
onboard_full(App *app, const gchar *const *chosen, gboolean adopt, gboolean relay_list)
{
  GhInboxSetupConfig config = {
    .accounts = app->accounts,
    .account_relays = app->relays,
    .settings = app->settings,
    .offer_relay_list = TRUE,
  };
  GhInboxSetup *setup = gh_inbox_setup_new(&config);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_inbox_setup_start_full(setup, chosen, adopt, relay_list, &error));
  g_assert_no_error(error);
  spin_until(setup_finished, setup, "the onboarding publication");
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_DONE);
  return setup;
}

static GhInboxSetup *
onboard_on(App *app, const gchar *const *chosen, gboolean relay_list)
{
  return onboard_full(app, chosen, FALSE, relay_list);
}

static GhInboxSetup *
onboard(App *app, const gchar *relay, gboolean relay_list)
{
  const gchar *chosen[] = { relay, NULL };
  return onboard_on(app, chosen, relay_list);
}

static GhRelayListOffer
offer_of(App *app)
{
  GhInboxSetupConfig config = { .accounts = app->accounts, .account_relays = app->relays,
                                .settings = app->settings, .offer_relay_list = TRUE };
  return gh_relay_list_offer(&config);
}

/* A signed kind 10002 by `key` with the given ["r", url(, marker)] pairs. */
static void
seed_relay_list(WireRelay *relay, guint key, const gchar *url, const gchar *marker)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, marker ? nostr_tag_new("r", url, marker, NULL)
                                 : nostr_tag_new("r", url, NULL));
  nostr_tags_append(tags, nostr_tag_new("client", "another-app", NULL));
  g_autofree gchar *json = sign_event(key, 10002, g_get_real_time() / G_USEC_PER_SEC - 3600,
                                      "kept as it is", tags);
  wire_relay_inject(relay, json);
}

static gboolean
offer_create(gpointer data)
{
  return offer_of(data) == GH_RELAY_LIST_OFFER_CREATE;
}

static gboolean
list_setup_finished(gpointer data)
{
  GhRelayListSetupState state = gh_relay_list_setup_get_state(data);
  return state == GH_RELAY_LIST_SETUP_DONE || state == GH_RELAY_LIST_SETUP_FAILED ||
         state == GH_RELAY_LIST_SETUP_SKIPPED;
}

/* Re-review R2, end to end: a real first run has no discovery relay. The
 * message list is published to the chosen relay, which becomes the
 * discovery relay; once Groundhog looked there and found no relay list, the
 * result page's offer publishes one (after checking the relay); then the
 * two accounts invite each other. */
static void
test_first_run_accounts_invite_each_other(void)
{
  World w;
  world_fresh_lists = TRUE;
  world_no_discovery = TRUE;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  App *both[] = { alice, bob };
  for (guint i = 0; i < G_N_ELEMENTS(both); i++) {
    const gchar *chosen[] = { w.x.url, NULL };
    g_autoptr(GhInboxSetup) setup = onboard_full(both[i], chosen, TRUE, TRUE);
    /* Nothing was known about a relay list when Publish was pressed. */
    g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                    GH_INBOX_SETUP_RELAY_LIST_NONE);
    g_assert_true(gh_inbox_setup_get_adopted_discovery(setup));
    g_object_run_dispose(G_OBJECT(setup));
    spin_until(offer_create, both[i], "the relay-list offer after the adopted discovery");
    GhInboxSetupConfig config = { .accounts = both[i]->accounts,
                                  .account_relays = both[i]->relays,
                                  .settings = both[i]->settings, .offer_relay_list = TRUE };
    g_autoptr(GhRelayListSetup) list = gh_relay_list_setup_new(&config);
    g_autoptr(GError) error = NULL;
    g_assert_true(gh_relay_list_setup_start(list, chosen, chosen, &error));
    g_assert_no_error(error);
    spin_until(list_setup_finished, list, "the relay list");
    g_assert_cmpint(gh_relay_list_setup_get_state(list), ==, GH_RELAY_LIST_SETUP_DONE);
    g_object_run_dispose(G_OBJECT(list));
  }
  CountWait alice_kp = { &w.x, ALICE, 1 }, bob_kp = { &w.x, BOB, 1 };
  spin_until(key_packages_reached, &alice_kp, "Alice's KeyPackage on X");
  spin_until(key_packages_reached, &bob_kp, "Bob's KeyPackage on X");
  wait_state(alice, GH_MLS_KEY_PACKAGE_PUBLISHED);
  wait_state(bob, GH_MLS_KEY_PACKAGE_PUBLISHED);
  accept_contact(alice, BOB);
  accept_contact(bob, ALICE);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "From Alice", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  g_assert_nonnull(join(bob, ALICE));
  g_assert_nonnull(create_attempt(bob, "From Bob", (const guint[]){ ALICE }, 1, &error));
  g_assert_no_error(error);
  g_assert_nonnull(join(alice, BOB));
  world_down(&w);
}

/* Review R1, the re-review's probe P3: the user's real 10002 is only on the
 * chosen message relay X, not on the discovery relay E. Discovery finds
 * none, but the check of every target finds it there: nothing is
 * published over it. */
static void
test_relay_list_on_a_message_relay_kept(void)
{
  World w;
  world_fresh_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  seed_relay_list(&w.x, ALICE, "wss://my-real-outbox.example", NULL);
  g_assert_cmpint(offer_of(alice), ==, GH_RELAY_LIST_OFFER_CREATE);   /* E has none */
  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_SKIPPED);
  g_object_run_dispose(G_OBJECT(setup));
  WireRelay *relays[] = { &w.e, &w.x, &w.w, &w.h, &w.r, &w.g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    g_autoptr(GPtrArray) lists = published(relays[i], 10002);
    g_assert_cmpuint(lists->len, ==, 0);   /* no client published one */
  }
  world_down(&w);
}

/* Review R1: a target that does not answer is "unknown", never "none":
 * nothing is published. */
static void
test_relay_list_unconfirmed_not_published(void)
{
  World w;
  world_fresh_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  w.h.close_on_connect = TRUE;   /* a chosen relay that is down */
  const gchar *chosen[] = { w.x.url, w.h.url, NULL };
  g_autoptr(GhInboxSetup) setup = onboard_on(alice, chosen, TRUE);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_FAILED);
  GhRelayListSetup *list = gh_inbox_setup_get_relay_list(setup);
  g_assert_nonnull(list);
  g_assert_nonnull(gh_relay_list_setup_get_error(list));
  g_object_run_dispose(G_OBJECT(setup));
  WireRelay *relays[] = { &w.e, &w.x };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    g_autoptr(GPtrArray) lists = published(relays[i], 10002);
    g_assert_cmpuint(lists->len, ==, 0);
  }
  world_down(&w);
}

static gboolean
discovery_settled(gpointer data)
{
  App *app = data;
  GhAccountRelaysState state = gh_account_relays_get_state(app->relays);
  return state == GH_ACCOUNT_RELAYS_COMPLETE || state == GH_ACCOUNT_RELAYS_UNREACHABLE;
}

/* Review R1: discovery that heard "none" from one relay while another
 * failed has not shown there is none: nothing is offered. */
static void
test_relay_list_not_offered_on_partial_discovery(void)
{
  World w;
  world_fresh_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  g_assert_cmpint(offer_of(alice), ==, GH_RELAY_LIST_OFFER_CREATE);
  /* H closes the REQ (it demands a sign-in it then refuses): failed. */
  w.h.require_auth = TRUE;
  w.h.refuse_auth = TRUE;
  const gchar *discovery[] = { w.e.url, w.h.url, NULL };
  g_settings_set_strv(alice->settings, "discovery-relays", discovery);
  spin_until(discovery_settled, alice, "discovery on E and the failing H");
  g_assert_cmpint(gh_account_relays_get_state(alice->relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
  g_assert_false(gh_account_relays_get_all_answered(alice->relays));
  g_assert_cmpint(offer_of(alice), ==, GH_RELAY_LIST_OFFER_NONE);
  world_down(&w);
}

static gboolean
has_relay_list(gpointer data)
{
  return gh_account_relays_has_relay_list(((App *)data)->relays);
}

/* Review R3: a relay list naming only a read relay leaves the account
 * uninvitable; the offer adds the chosen relay as a write relay and keeps
 * every other tag and the content; then the KeyPackage is published. */
static void
test_relay_list_write_relay_added(void)
{
  World w;
  world_fresh_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  seed_relay_list(&w.e, ALICE, w.r.url, "read");
  spin_until(has_relay_list, alice, "Alice's read-only relay list");
  wait_state(alice, GH_MLS_KEY_PACKAGE_NO_RELAYS);
  g_assert_cmpint(offer_of(alice), ==, GH_RELAY_LIST_OFFER_ADD_WRITE);
  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_DONE);
  g_object_run_dispose(G_OBJECT(setup));
  g_autoptr(GPtrArray) lists = published(&w.e, 10002);
  g_assert_cmpuint(lists->len, ==, 1);
  NostrEvent *list = g_ptr_array_index(lists, 0);
  g_assert_cmpstr(nostr_event_get_content(list), ==, "kept as it is");
  NostrTags *tags = nostr_event_get_tags(list);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 3);
  NostrTag *read = nostr_tags_get(tags, 0), *client = nostr_tags_get(tags, 1),
           *added = nostr_tags_get(tags, 2);
  g_assert_cmpstr(nostr_tag_get(read, 1), ==, w.r.url);
  g_assert_cmpstr(nostr_tag_get(read, 2), ==, "read");
  g_assert_cmpstr(nostr_tag_get(client, 0), ==, "client");
  g_assert_cmpstr(nostr_tag_get(added, 1), ==, w.x.url);
  g_assert_cmpstr(nostr_tag_get(added, 2), ==, "write");
  /* Invitable now: the KeyPackage goes to X. */
  CountWait on_x = { &w.x, ALICE, 1 };
  spin_until(key_packages_reached, &on_x, "Alice's KeyPackage on X");
  wait_state(alice, GH_MLS_KEY_PACKAGE_PUBLISHED);
  world_down(&w);
}

/* Review H2: two Groundhog users onboarded from nothing (no kind 10002 or
 * 10050 anywhere) can invite each other: onboarding's consented relay list
 * names their message relay as their write relay, their KeyPackages go
 * there, and each finds the other's through it. */
static void
test_fresh_accounts_invite_each_other(void)
{
  World w;
  world_fresh_lists = TRUE;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  /* Nobody can invite them yet, and the service says so. */
  wait_state(alice, GH_MLS_KEY_PACKAGE_NO_RELAYS);
  wait_state(bob, GH_MLS_KEY_PACKAGE_NO_RELAYS);
  App *both[] = { alice, bob };
  for (guint i = 0; i < G_N_ELEMENTS(both); i++) {
    GhInboxSetupConfig config = { .accounts = both[i]->accounts,
                                  .account_relays = both[i]->relays,
                                  .settings = both[i]->settings, .offer_relay_list = TRUE };
    g_autoptr(GhInboxSetup) probe = gh_inbox_setup_new(&config);
    g_assert_true(gh_inbox_setup_relay_list_needed(probe));
    g_object_run_dispose(G_OBJECT(probe));
    g_autoptr(GhInboxSetup) setup = onboard(both[i], w.x.url, TRUE);
    g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                    GH_INBOX_SETUP_RELAY_LIST_DONE);
    g_assert_cmpuint(gh_inbox_setup_get_relay_list_n_accepted(setup), >=, 1);
    g_object_run_dispose(G_OBJECT(setup));
  }
  /* The relay list is on the discovery relay: X marked "write". */
  g_autoptr(GPtrArray) lists = published(&w.e, 10002);
  g_assert_cmpuint(lists->len, ==, 2);
  for (guint i = 0; i < lists->len; i++) {
    NostrEvent *list = g_ptr_array_index(lists, i);
    g_assert_cmpuint(tag_count(list, "r"), ==, 1);
    NostrTag *r = nostr_tags_get(nostr_event_get_tags(list), 0);
    g_assert_cmpuint(nostr_tag_size(r), ==, 3);
    g_assert_cmpstr(nostr_tag_get(r, 1), ==, w.x.url);
    g_assert_cmpstr(nostr_tag_get(r, 2), ==, "write");
  }
  /* Each account's KeyPackage goes to its write relay X. */
  CountWait alice_kp = { &w.x, ALICE, 1 }, bob_kp = { &w.x, BOB, 1 };
  spin_until(key_packages_reached, &alice_kp, "Alice's KeyPackage on X");
  spin_until(key_packages_reached, &bob_kp, "Bob's KeyPackage on X");
  wait_state(alice, GH_MLS_KEY_PACKAGE_PUBLISHED);
  wait_state(bob, GH_MLS_KEY_PACKAGE_PUBLISHED);

  /* And they invite each other. */
  accept_contact(alice, BOB);
  accept_contact(bob, ALICE);
  g_autoptr(GError) error = NULL;
  g_assert_nonnull(create_attempt(alice, "From Alice", (const guint[]){ BOB }, 1, &error));
  g_assert_no_error(error);
  GhMlsGroup *gb = join(bob, ALICE);
  g_assert_nonnull(gb);
  g_assert_nonnull(create_attempt(bob, "From Bob", (const guint[]){ ALICE }, 1, &error));
  g_assert_no_error(error);
  GhMlsGroup *ga = join(alice, BOB);
  g_assert_nonnull(ga);
  world_down(&w);
}

/* Review H2: an account that already has a kind 10002 is never offered one,
 * and asking anyway publishes none (it would replace the user's list). */
static void
test_relay_list_never_replaced(void)
{
  World w;
  world_split_lists = TRUE;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_published(alice);
  GhInboxSetupConfig config = { .accounts = alice->accounts, .account_relays = alice->relays,
                                .settings = alice->settings, .offer_relay_list = TRUE };
  g_autoptr(GhInboxSetup) probe = gh_inbox_setup_new(&config);
  g_assert_false(gh_inbox_setup_relay_list_needed(probe));
  g_object_run_dispose(G_OBJECT(probe));
  g_autoptr(GhInboxSetup) setup = onboard(alice, w.x.url, TRUE);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_NONE);
  g_object_run_dispose(G_OBJECT(setup));
  WireRelay *relays[] = { &w.e, &w.w, &w.x, &w.h, &w.r, &w.g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    g_autoptr(GPtrArray) lists = published(relays[i], 10002);
    g_assert_cmpuint(lists->len, ==, 0);
  }
  world_down(&w);
}
#endif

#if GH_MLS_ADOPTED_KEY_PACKAGES
/* libmarmot's own build gate refuses the public ADOPTED producer in a
 * default build; its ungated internal producer, signer-only (the enrolled
 * proof), stands in for that one call. */
static MarmotError
adopted_producer(Marmot *marmot, const guint8 account[32], MarmotKeyPackageResult *made)
{
  return marmot_create_key_package_adopted_internal(marmot, account, NULL, NULL, NULL, made);
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
#if GH_MLS_ADOPTED_KEY_PACKAGES
  gh_mls_service_test_set_adopted_producer(adopted_producer);
#define KP_TEST(name) "/groundhog/mls-kp-lifecycle-adopted/" name
#else
#define KP_TEST(name) "/groundhog/mls-kp-lifecycle/" name
#endif
  g_test_add_func(KP_TEST("write-relays-only"), test_write_relays_only);
  g_test_add_func(KP_TEST("lookup-privacy"), test_lookup_privacy);
  g_test_add_func(KP_TEST("rotation-ack-tied"), test_rotation_ack_tied);
#if !GH_MLS_ADOPTED_KEY_PACKAGES
  g_test_add_func(KP_TEST("delayed-welcome"), test_delayed_welcome);
  g_test_add_func(KP_TEST("pending-invitations-defer-rotation"),
                  test_pending_invitations_defer_rotation);
  g_test_add_func(KP_TEST("rotation-held-for-pending-invitation"),
                  test_rotation_held_for_pending_invitation);
  g_test_add_func(KP_TEST("lifetime-rotation-held"), test_lifetime_rotation_held);
  g_test_add_func(KP_TEST("hold-cap-expires"), test_hold_cap_expires);
  g_test_add_func(KP_TEST("invitation-listing-error-holds"),
                  test_invitation_listing_error_holds);
  g_test_add_func(KP_TEST("failed-welcome-preserves"), test_failed_welcome_preserves);
#endif
#if GH_TEST_HAVE_INBOX_SETUP && !GH_MLS_ADOPTED_KEY_PACKAGES
  g_test_add_func(KP_TEST("fresh-accounts-invite-each-other"),
                  test_fresh_accounts_invite_each_other);
  g_test_add_func(KP_TEST("relay-list-never-replaced"), test_relay_list_never_replaced);
  g_test_add_func(KP_TEST("relay-list-on-a-message-relay-kept"),
                  test_relay_list_on_a_message_relay_kept);
  g_test_add_func(KP_TEST("relay-list-unconfirmed-not-published"),
                  test_relay_list_unconfirmed_not_published);
  g_test_add_func(KP_TEST("relay-list-not-offered-on-partial-discovery"),
                  test_relay_list_not_offered_on_partial_discovery);
  g_test_add_func(KP_TEST("relay-list-write-relay-added"), test_relay_list_write_relay_added);
  g_test_add_func(KP_TEST("first-run-accounts-invite-each-other"),
                  test_first_run_accounts_invite_each_other);
#endif
  gint rc = g_test_run();
  mls_world_finish();
  return rc;
}
