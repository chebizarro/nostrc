/* Groundhog <-> MDK 0.11 interop matrix (nostrc-a5u5; the release gate of
 * nostrc-7gx7; docs/analysis/marmot-adopted-profile-gap-2026-09-30.md,
 * "Harness matrix and acceptance evidence"). The third-party side is MDK
 * v0.11.0 (946e0547: cgka-engine, the adopted Marmot profile with
 * app_data_dictionary components and the 0x8009 account proof) behind the
 * JSON-lines driver of tests/interop/mdk/driver-0.11 (mdk-peer.h). Groundhog
 * accounts are the real GhMlsService of mls-world.h on the test's local
 * relays: KeyPackages on W, Welcomes gift-wrapped (NIP-59) to the invitee's
 * inbox X, group traffic on G with NIP-42 AUTH.
 *
 * Every case runs alone (CTest groundhog-mdk011-interop-<case>, -p).
 *
 *  control: MDK 0.11 <-> MDK 0.11 through the same relays and harness. It
 *    must pass: it proves the driver, the relays and the adopted flows
 *    (KeyPackage, Welcome 1059 -> 13 -> 444, kind 9 both ways, rename, add,
 *    remove, self-update, SelfRemove leave) before any Groundhog verdict.
 *
 * The adopted cases. libmarmot cannot create or join an adopted group yet
 * (nostrc-qp24.5.1), so each asserts today's honest refusal precisely (the
 * failure class, nothing published, no group, no invitation, no stall, no
 * crash) and then exits 77: CTest reports it Skipped (XFAIL), never Passed.
 * A refusal of another shape fails; so does an unexpected success (XPASS),
 * which means the expectation must be updated.
 *  groundhog-invites-mdk: Groundhog reads an MDK 0.11 KeyPackage and is asked
 *    to invite its author, default and legacy mode.
 *  mdk-invites-groundhog: MDK 0.11 reads Groundhog's KeyPackage and is asked
 *    to invite Alice.
 *  adopted-welcome: an adopted Welcome reaches Groundhog (MDK 0.11 invites a
 *    second, MDK device of Alice's account; the gift wrap lands in Alice's
 *    inbox, where Groundhog opens it).
 *  white-noise-welcome: MDK 0.11 invites a libmarmot adopted KeyPackage
 *    (made in Groundhog's store) into a White Noise-shaped group; Groundhog
 *    joins and messages flow both ways (nostrc-qp24.5.2); XFAIL at the
 *    group's first Commit (nostrc-qp24.5.1). */
#include "mls-world.h"
#include "mdk-peer.h"

#include "gh-mls-copy.h"
#include "gh-mls-invitee.h"

enum { DAVE = STRANGER };

#define MDK011_REV "946e0547485c9a2c393c2048ec3a968fd50fb441"
#define ADOPTED "marmot-adopted"


static MdkDriver driver;

/* ---- XFAIL bookkeeping ------------------------------------------------------------- */

static guint xfails;
/* A case that could not run (no driver): never green either. */
static gboolean not_run;
/* Cases entered: a -p path that matches none must not pass vacuously. */
static guint cases_run;

/* Records the expected failure of an adopted case, by class. The case still
 * passed every assertion of the refusal: main() turns the run into a skip. */
static void
xfail(const gchar *class_, const gchar *what)
{
  g_test_message("XFAIL [%s]: %s", class_, what);
  g_printerr("XFAIL [%s]: %s\n", class_, what);
  xfails++;
}

/* ---- the MDK side -------------------------------------------------------------------- */

/* Starts the driver; FALSE (the test skipped) without one. Asserts the pin
 * and the profile negotiation: the driver speaks the adopted profile and
 * refuses the legacy one as unsupported. */
static gboolean
mdk_up(void)
{
  if (!mdk_driver_start(&driver)) {
    g_test_skip("GH_MDK_DRIVER is unset (configure with -DBUILD_MDK011_INTEROP=ON)");
    not_run = TRUE;
    return FALSE;
  }
  g_autoptr(JsonObject) hello = mdk_call(&driver, "\"cmd\":\"hello\",\"profiles\":[\"" ADOPTED "\"]");
  g_test_message("MDK peer: %s, rev %s, openmls %s, profile %s",
                 json_object_get_string_member(hello, "mdk"),
                 json_object_get_string_member(hello, "mdk_rev"),
                 json_object_get_string_member(hello, "openmls_rev"),
                 json_object_get_string_member(hello, "profile"));
  g_assert_cmpstr(json_object_get_string_member(hello, "mdk_rev"), ==, MDK011_REV);
  g_assert_cmpstr(json_object_get_string_member(hello, "profile"), ==, ADOPTED);
  g_autoptr(JsonObject) legacy = mdk_try(&driver,
                                         "\"cmd\":\"hello\",\"profiles\":[\"marmot-legacy-mip\"]");
  g_assert_false(json_object_get_boolean_member(legacy, "ok"));
  g_assert_cmpstr(json_object_get_string_member(legacy, "class"), ==, "unsupported");
  return TRUE;
}

static void
mdk_peer(const gchar *peer, guint key)
{
  g_autoptr(JsonObject) made = mdk_call(&driver,
                                        "\"cmd\":\"peer_new\",\"peer\":\"%s\",\"secret\":\"%s\"",
                                        peer, gh_test_secret[key]);
  g_assert_cmpstr(json_object_get_string_member(made, "pubkey"), ==, hex[key]);
}

/* The peer's KeyPackage published to `to` (a JSON array body, maybe empty:
 * made, not sent); its event JSON. */
static gchar *
mdk_publish_key_package(const gchar *peer, const gchar *to)
{
  g_autoptr(JsonObject) kp = mdk_call(&driver,
                                      "\"cmd\":\"publish_key_package\",\"peer\":\"%s\","
                                      "\"relays\":[],\"to\":[%s]",
                                      peer, to);
  JsonObject *view = json_object_get_object_member(kp, "mdk");
  g_assert_cmpstr(json_object_get_string_member(view, "profile"), ==, "Current");
  return g_strdup(json_object_get_string_member(kp, "event"));
}

/* A KeyPackage as White Noise 0.11 publishes it (marmot-app's session
 * configuration, nostrc-a5u5 review M1): the leaf advertises SelfRemove
 * (proposal 0x000a) and the three agent-text-stream-QUIC roles (private-use
 * extensions 0xF2D1, 0xF2D2, 0xF2D4) besides app_data_dictionary 0x0006, the
 * component set includes 0x8006 beside the 0x8009 proof, and the event carries
 * White Noise Android's `client` tag. */
static void
assert_white_noise_key_package(JsonObject *view, const gchar *event_json)
{
  JsonArray *extensions = json_object_get_array_member(view, "mls_extensions");
  static const gchar *const want_extensions[] = { "0x0006", "0xf2d1", "0xf2d2", "0xf2d4" };
  for (guint i = 0; i < G_N_ELEMENTS(want_extensions); i++)
    g_assert_true(mdk_has(extensions, want_extensions[i]));
  g_assert_true(mdk_has(json_object_get_array_member(view, "mls_proposals"), "0x000a"));
  JsonArray *components = json_object_get_array_member(view, "app_components");
  g_assert_true(mdk_has(components, "0x8006"));
  g_assert_true(mdk_has(components, "0x8009"));
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, event_json, -1, NULL));
  JsonArray *tags = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)),
                                                 "tags");
  guint clients = 0;
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (g_strcmp0(json_array_get_string_element(tag, 0), "client") == 0) {
      clients++;
      g_assert_cmpstr(json_array_get_string_element(tag, 1), ==, "White Noise Android");
    }
  }
  g_assert_cmpuint(clients, ==, 1);
}

/* The newest KeyPackage of account key on W, as the peer fetched it; *view:
 * what MDK 0.11's relay-fetch admission makes of it. */
static gchar *
mdk_fetch_key_package(World *w, const gchar *peer, guint key, JsonObject **view)
{
  g_autoptr(JsonObject) kp = mdk_call(&driver,
                                      "\"cmd\":\"fetch_key_package\",\"peer\":\"%s\","
                                      "\"author\":\"%s\",\"from\":[\"%s\"]",
                                      peer, hex[key], w->w.url);
  *view = json_object_ref(json_object_get_object_member(kp, "mdk"));
  g_autofree gchar *text = mdk_json(*view);
  g_test_message("MDK 0.11 %s admits %s's KeyPackage: %s", peer, hex[key], text);
  return g_strdup(json_object_get_string_member(kp, "event"));
}

static void
mdk_send(const gchar *peer, const gchar *group, const gchar *text)
{
  g_autoptr(JsonObject) sent = mdk_call(&driver,
                                        "\"cmd\":\"send\",\"peer\":\"%s\",\"group\":\"%s\","
                                        "\"text\":\"%s\"",
                                        peer, group, text);
}

static JsonObject *
mdk_sync(const gchar *peer, const gchar *group)
{
  JsonObject *synced = mdk_call(&driver, "\"cmd\":\"sync\",\"peer\":\"%s\",\"group\":\"%s\"",
                                peer, group);
  g_autofree gchar *text = mdk_json(synced);
  g_test_message("MDK %s sync: %s", peer, text);
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(synced, "failed")), ==, 0);
  return synced;
}

static JsonObject *
sync_state(JsonObject *synced)
{
  return json_object_get_object_member(synced, "state");
}

/* Whether a sync answer holds the kind 9 text by author. */
static gboolean
synced_message(JsonObject *synced, const gchar *author, const gchar *text)
{
  JsonArray *results = json_object_get_array_member(synced, "results");
  for (guint i = 0; i < json_array_get_length(results); i++) {
    JsonObject *r = json_array_get_object_element(results, i);
    if (g_strcmp0(json_object_get_string_member(r, "type"), "application") == 0 &&
        g_strcmp0(json_object_get_string_member(r, "author"), author) == 0 &&
        json_object_get_int_member_with_default(r, "kind", 0) == 9 &&
        g_strcmp0(json_object_get_string_member_with_default(r, "content", NULL), text) == 0)
      return TRUE;
  }
  return FALSE;
}

static guint64
state_epoch(JsonObject *state)
{
  return (guint64)json_object_get_int_member(state, "epoch");
}

static void
assert_strv_equal(GStrv a, GStrv b)
{
  g_autofree gchar *x = g_strjoinv(",", a), *y = g_strjoinv(",", b);
  g_assert_cmpstr(x, ==, y);
}

static void
assert_member_array(JsonObject *state, const gchar *field, const guint *keys, guint n)
{
  g_auto(GStrv) got = mdk_strv(json_object_get_array_member(state, field));
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; i < n; i++)
    g_strv_builder_add(builder, hex[keys[i]]);
  g_auto(GStrv) want = g_strv_builder_end(builder);
  qsort(want, g_strv_length(want), sizeof(gchar *), mdk_strcmp);
  assert_strv_equal(got, want);
}

/* Two MDK views of one group agree: group, routing, epoch, name, members,
 * admins, components. */
static void
assert_mdk_converged(JsonObject *a, JsonObject *b)
{
  static const gchar *const same[] = { "group", "nostr_group_id", "name", "description" };
  for (guint i = 0; i < G_N_ELEMENTS(same); i++)
    g_assert_cmpstr(json_object_get_string_member(a, same[i]), ==,
                    json_object_get_string_member(b, same[i]));
  g_assert_cmpuint(state_epoch(a), ==, state_epoch(b));
  static const gchar *const sets[] = { "members", "admins", "relays", "components" };
  for (guint i = 0; i < G_N_ELEMENTS(sets); i++) {
    g_auto(GStrv) x = mdk_strv(json_object_get_array_member(a, sets[i]));
    g_auto(GStrv) y = mdk_strv(json_object_get_array_member(b, sets[i]));
    assert_strv_equal(x, y);
  }
}

/* The one Welcome the peer finds for itself on X, joined: its group's state.
 * Asserts the adopted Welcome shape: a kind 444 rumor inside the NIP-59 wrap
 * whose `e` names the consumed KeyPackage event and whose `relays` the
 * group's. */
static JsonObject *
mdk_join(World *w, const gchar *peer, guint inviter, const gchar *kp_event_id)
{
  g_autoptr(JsonObject) found = mdk_call(&driver,
                                         "\"cmd\":\"fetch_welcomes\",\"peer\":\"%s\","
                                         "\"from\":[\"%s\"]",
                                         peer, w->x.url);
  g_autofree gchar *text = mdk_json(found);
  g_test_message("MDK %s Welcomes: %s", peer, text);
  JsonArray *welcomes = json_object_get_array_member(found, "welcomes");
  g_assert_cmpuint(json_array_get_length(welcomes), ==, 1);
  JsonObject *welcome = json_array_get_object_element(welcomes, 0);
  if (!json_object_get_boolean_member(welcome, "ok"))
    g_error("MDK %s refused the Welcome (%s): %s", peer,
            json_object_get_string_member(welcome, "class"),
            json_object_get_string_member(welcome, "error"));
  g_assert_cmpstr(json_object_get_string_member(welcome, "sender"), ==, hex[inviter]);
  g_assert_cmpint(json_object_get_int_member(welcome, "rumor_kind"), ==, 444);
  JsonArray *tags = json_object_get_array_member(welcome, "rumor_tags");
  gboolean e_ok = FALSE, relays_ok = FALSE;
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    const gchar *name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(name, "e") == 0)
      e_ok = g_strcmp0(json_array_get_string_element(tag, 1), kp_event_id) == 0;
    else if (g_strcmp0(name, "relays") == 0)
      relays_ok = json_array_get_length(tag) == 2 &&
                  g_strcmp0(json_array_get_string_element(tag, 1), w->g.url) == 0;
  }
  g_assert_true(e_ok);
  g_assert_true(relays_ok);
  return mdk_call(&driver, "\"cmd\":\"accept_welcome\",\"peer\":\"%s\",\"wrapper_id\":\"%s\"",
                  peer, json_object_get_string_member(welcome, "wrapper_id"));
}

static gchar *
event_id_of(const gchar *event_json)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, event_json, -1, NULL));
  return g_strdup(json_object_get_string_member(json_node_get_object(json_parser_get_root(parser)),
                                                "id"));
}

/* ---- control: MDK 0.11 <-> MDK 0.11 ------------------------------------------------- */

static void
test_control(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  world_up(&w, NULL, 0);
  mdk_peer("carol", CAROL);
  mdk_peer("dave", DAVE);
  mdk_peer("bob", BOB);
  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);

  /* Adopted KeyPackages (kind 30443, MLSMessage framing, 0x8009 proof),
   * published and admitted through the relay-fetch path. */
  g_autofree gchar *dave_published = mdk_publish_key_package("dave", on_w);
  g_autoptr(JsonObject) dave_view = NULL;
  g_autofree gchar *dave_kp = mdk_fetch_key_package(&w, "carol", DAVE, &dave_view);
  g_assert_true(json_object_get_boolean_member(dave_view, "parsed"));
  g_assert_cmpstr(json_object_get_string_member(dave_view, "profile"), ==, "Current");
  assert_white_noise_key_package(dave_view, dave_kp);
  g_autofree gchar *dave_kp_id = event_id_of(dave_kp);
  g_autofree gchar *published_id = event_id_of(dave_published);
  g_assert_cmpstr(dave_kp_id, ==, published_id);

  /* Carol makes the group with Dave; Dave joins from the Welcome on X. */
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Control\","
    "\"description\":\"mdk011 control\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], dave_kp, w.x.url);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));
  g_assert_cmpstr(json_object_get_string_member(made, "profile"), ==, "Current");
  JsonArray *components = json_object_get_array_member(made, "components");
  g_assert_true(mdk_has(components, "0x8001"));   /* profile */
  g_assert_true(mdk_has(components, "0x8003"));   /* admin policy */
  g_assert_true(mdk_has(components, "0x8004"));   /* Nostr routing */
  {
    g_autoptr(JsonObject) joined = mdk_join(&w, "dave", CAROL, dave_kp_id);
    assert_mdk_converged(made, joined);
    assert_member_array(joined, "members", (const guint[]){ CAROL, DAVE }, 2);
    assert_member_array(joined, "admins", (const guint[]){ CAROL }, 1);
    g_autoptr(JsonObject) ctx_carol = mdk_call(&driver,
      "\"cmd\":\"group_context\",\"peer\":\"carol\",\"group\":\"%s\"", group);
    g_autoptr(JsonObject) ctx_dave = mdk_call(&driver,
      "\"cmd\":\"group_context\",\"peer\":\"dave\",\"group\":\"%s\"", group);
    /* The answers' own envelope (id, ok) aside, the two views are equal. */
    for (guint i = 0; i < 2; i++) {
      json_object_remove_member(i ? ctx_dave : ctx_carol, "id");
      json_object_remove_member(i ? ctx_dave : ctx_carol, "ok");
    }
    g_autofree gchar *a = mdk_json(ctx_carol), *b = mdk_json(ctx_dave);
    g_test_message("GroupContext (carol): %s", a);
    g_assert_cmpstr(a, ==, b);
    g_assert_true(mdk_has(json_object_get_array_member(ctx_carol, "required_components"),
                          "0x8009"));
  }

  /* Kind 9 both ways. */
  mdk_send("carol", group, "hello dave");
  {
    g_autoptr(JsonObject) synced = mdk_sync("dave", group);
    g_assert_true(synced_message(synced, hex[CAROL], "hello dave"));
  }
  mdk_send("dave", group, "hello carol");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[DAVE], "hello carol"));
  }

  /* Rename (an AppDataUpdate of the profile component). */
  {
    g_autoptr(JsonObject) renamed = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"name\":\"Control renamed\"", group);
    g_assert_cmpstr(json_object_get_string_member(renamed, "name"), ==, "Control renamed");
    g_autoptr(JsonObject) synced = mdk_sync("dave", group);
    assert_mdk_converged(renamed, sync_state(synced));
  }

  /* Add Bob, who joins; Bob writes to both. */
  g_autofree gchar *bob_published = mdk_publish_key_package("bob", on_w);
  g_autoptr(JsonObject) bob_view = NULL;
  g_autofree gchar *bob_kp = mdk_fetch_key_package(&w, "carol", BOB, &bob_view);
  g_assert_true(json_object_get_boolean_member(bob_view, "parsed"));
  assert_white_noise_key_package(bob_view, bob_kp);
  g_autofree gchar *bob_kp_id = event_id_of(bob_kp);
  {
    g_autoptr(JsonObject) added = mdk_call(&driver,
      "\"cmd\":\"add_members\",\"peer\":\"carol\",\"group\":\"%s\",\"key_packages\":[%s],"
      "\"welcome_relays\":[\"%s\"]", group, bob_kp, w.x.url);
    assert_member_array(added, "members", (const guint[]){ CAROL, DAVE, BOB }, 3);
    g_autoptr(JsonObject) joined = mdk_join(&w, "bob", CAROL, bob_kp_id);
    assert_mdk_converged(added, joined);
    g_autoptr(JsonObject) synced = mdk_sync("dave", group);
    assert_mdk_converged(added, sync_state(synced));
  }
  mdk_send("bob", group, "three of us");
  {
    g_autoptr(JsonObject) carol_sync = mdk_sync("carol", group);
    g_assert_true(synced_message(carol_sync, hex[BOB], "three of us"));
    g_autoptr(JsonObject) dave_sync = mdk_sync("dave", group);
    g_assert_true(synced_message(dave_sync, hex[BOB], "three of us"));
  }

  /* Carol removes Bob. */
  {
    g_autoptr(JsonObject) removed = mdk_call(&driver,
      "\"cmd\":\"remove_members\",\"peer\":\"carol\",\"group\":\"%s\",\"members\":[\"%s\"]",
      group, hex[BOB]);
    assert_member_array(removed, "members", (const guint[]){ CAROL, DAVE }, 2);
    g_autoptr(JsonObject) dave_sync = mdk_sync("dave", group);
    assert_mdk_converged(removed, sync_state(dave_sync));
    g_autoptr(JsonObject) bob_sync = mdk_sync("bob", group);
    g_assert_true(json_object_get_boolean_member(sync_state(bob_sync), "removed"));
  }

  /* Dave rotates his leaf. */
  {
    g_autoptr(JsonObject) rotated = mdk_call(&driver,
      "\"cmd\":\"self_update\",\"peer\":\"dave\",\"group\":\"%s\"", group);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    assert_mdk_converged(rotated, sync_state(synced));
  }

  /* Dave leaves (SelfRemove): Carol, the admin, commits it on sync. */
  {
    g_autoptr(JsonObject) left = mdk_call(&driver,
      "\"cmd\":\"leave\",\"peer\":\"dave\",\"group\":\"%s\"", group);
    g_assert_true(json_object_get_boolean_member(left, "leave_in_progress"));
    g_autoptr(JsonObject) carol_sync = mdk_sync("carol", group);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(carol_sync, "published")),
                     >=, 1);
    assert_member_array(sync_state(carol_sync), "members", (const guint[]){ CAROL }, 1);
    g_autoptr(JsonObject) dave_sync = mdk_sync("dave", group);
    g_assert_true(json_object_get_boolean_member(sync_state(dave_sync), "removed"));
  }

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- adopted: Groundhog invites an MDK 0.11 user ------------------------------------ */

typedef struct {
  gboolean done;
  GhMlsInviteeState state;
} CheckWait;

static gboolean
check_done(gpointer data)
{
  return ((CheckWait *)data)->done;
}

static void
on_checked(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  CheckWait *wait = data;
  g_autoptr(GError) error = NULL;
  wait->state = gh_mls_invitee_check_finish(result, &error);
  wait->done = TRUE;
}

/* Alice asks to create a group with Carol (MDK 0.11): refused, and nothing
 * is created or published. The error (transfer full). */
static GError *
refused_creation(World *w, App *alice, const gchar *mode)
{
  guint g_events = w->g.events, x_events = w->x.events;
  const gchar *relays[] = { w->g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(alice->service, "Adopted?", NULL, relays, people, NULL,
                                    on_created, &wait);
  spin_until(op_done, &wait, "the refused creation");
  if (wait.result)
    g_error("XPASS (%s): Groundhog created a group with an MDK 0.11 user; update the "
            "expectation of this case", mode);
  g_autofree gchar *copy = gh_mls_error_copy(wait.error);
  g_test_message("Groundhog (%s) inviting Carol: %s %d \"%s\"; the UI says \"%s\"",
                 mode, g_quark_to_string(wait.error->domain), wait.error->code,
                 wait.error->message, copy);
  g_assert_cmpuint(w->g.events, ==, g_events);
  g_assert_cmpuint(w->x.events, ==, x_events);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  return wait.error;
}

static void
test_groundhog_invites_mdk(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  g_autofree gchar *to = g_strdup_printf("\"%s\",\"%s\"", w.w.url, w.x.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", to);
  accept_contact(alice, CAROL);

  /* libmarmot on the adopted KeyPackage event. */
  bool proven = true;
  MarmotError proof = marmot_key_package_event_has_account_proof(carol_kp, &proven);
  g_test_message("libmarmot account-proof check of an MDK 0.11 KeyPackage: %s, proven %d",
                 marmot_error_string(proof), proven);
  g_assert_cmpint(proof, ==, MARMOT_ERR_VALIDATION);

  /* New Group's check row. */
  CheckWait check = { 0 };
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, on_checked,
                             &check);
  spin_until(check_done, &check, "the KeyPackage check");
  g_test_message("New Group check row for an MDK 0.11 user: state %d, \"%s\", can invite %d",
                 check.state, gh_mls_invitee_copy(check.state),
                 gh_mls_invitee_can_invite(check.state));
  /* Today's row reads an adopted KeyPackage as none at all (nostrc-ncp0). */
  g_assert_cmpint(check.state, ==, GH_MLS_INVITEE_NOT_SET_UP);
  g_assert_false(gh_mls_invitee_can_invite(check.state));

  /* By default (members without the account proof admitted in legacy
   * groups, nostrc-6ukh) and with the preference that requires proofs. */
  g_autoptr(GError) by_default = refused_creation(&w, alice, "default");
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", TRUE);
  g_autoptr(GError) strict = refused_creation(&w, alice, "proofs required");
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", FALSE);
  /* Precise class: the adopted KeyPackage is one libmarmot cannot use. */
  g_assert_error(by_default, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE);
  g_assert_error(strict, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE);
  xfail("unsupported", "Groundhog cannot invite an MDK 0.11 user: libmarmot does not admit an "
                       "adopted (MLSMessage-framed, 0x8009) KeyPackage (nostrc-qp24.5.1)");

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- adopted: MDK 0.11 invites Groundhog -------------------------------------------- */

static void
test_mdk_invites_groundhog(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);

  /* MDK 0.11's relay-fetch admission refuses Groundhog's (legacy) KeyPackage
   * as unsupported... */
  g_autoptr(JsonObject) view = NULL;
  g_autofree gchar *alice_kp = mdk_fetch_key_package(&w, "carol", ALICE, &view);
  if (json_object_get_boolean_member(view, "parsed"))
    g_error("XPASS: MDK 0.11 admits Groundhog's KeyPackage; update the expectation");
  g_assert_cmpstr(json_object_get_string_member(view, "class"), ==, "unsupported");
  g_assert_nonnull(strstr(json_object_get_string_member(view, "error"),
                          "a legacy MIP-00 KeyPackage event"));

  /* ...and so does create_group: nothing reaches Alice's inbox. */
  guint g_events = w.g.events, x_events = w.x.events;
  g_autoptr(JsonObject) made = mdk_try(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Made by MDK 0.11\","
    "\"description\":\"\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_kp, w.x.url);
  g_autofree gchar *text = mdk_json(made);
  g_test_message("MDK 0.11 inviting Groundhog: %s", text);
  if (json_object_get_boolean_member(made, "ok"))
    g_error("XPASS: MDK 0.11 invited Groundhog; update the expectation");
  g_assert_cmpstr(json_object_get_string_member(made, "class"), ==, "unsupported");
  g_assert_cmpuint(w.g.events, ==, g_events);
  g_assert_cmpuint(w.x.events, ==, x_events);
  g_assert_cmpuint(alice->invites, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  xfail("unsupported", json_object_get_string_member(made, "error"));

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- adopted: an adopted Welcome reaches Groundhog ---------------------------------- */

typedef struct {
  App *app;
  const gchar *wrap_id;
} WelcomeWait;

/* libmarmot's record of a processed Welcome, if any: its state and reason. */
static gboolean
welcome_record_find(App *app, const gchar *wrap_id, gint *state, gchar **reason)
{
  g_autoptr(GError) error = NULL;
  MarmotStorage *storage = gh_store_marmot_new(app->store, &error);
  g_assert_no_error(error);
  guint8 wrapper[32];
  g_assert_true(nostr_hex2bin(wrapper, wrap_id, sizeof wrapper));
  bool found = false;
  int s = -1;
  char *why = NULL;
  g_assert_cmpint(storage->find_processed_welcome(storage->ctx, wrapper, &found, &s, &why),
                  ==, MARMOT_OK);
  marmot_storage_free(storage);
  if (found) {
    *state = s;
    *reason = g_strdup(why);
  }
  free(why);
  return found;
}

static gboolean
welcome_processed(gpointer data)
{
  WelcomeWait *wait = data;
  gint state;
  g_autofree gchar *reason = NULL;
  return welcome_record_find(wait->app, wait->wrap_id, &state, &reason);
}

static void
test_adopted_welcome(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);
  /* A second device of Alice's account, on MDK 0.11: its KeyPackage is made
   * and handed to Carol directly (never published, so Groundhog's own
   * KeyPackage stays the newest on W). */
  mdk_peer("alice-mdk", ALICE);
  g_autofree gchar *alice_mdk_kp = mdk_publish_key_package("alice-mdk", "");

  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Adopted for Alice\","
    "\"description\":\"\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_mdk_kp, w.x.url);
  JsonArray *welcomes = json_object_get_array_member(made, "welcomes");
  g_assert_cmpuint(json_array_get_length(welcomes), ==, 1);
  const gchar *wrap = json_object_get_string_member(json_array_get_object_element(welcomes, 0),
                                                    "wrapper_id");

  /* Groundhog opens the gift wrap and refuses the adopted Welcome: recorded
   * as failed with libmarmot's reason, no invitation, no group, no stall. */
  WelcomeWait wait = { alice, wrap };
  spin_until(welcome_processed, &wait, "Groundhog's verdict on the adopted Welcome");
  gint state = -1;
  g_autofree gchar *reason = NULL;
  g_assert_true(welcome_record_find(alice, wrap, &state, &reason));
  g_test_message("Groundhog on an adopted (MDK 0.11) Welcome: state %d, \"%s\"; invitations %u",
                 state, reason ? reason : "", alice->invites);
  if (state != MARMOT_WELCOME_STATE_FAILED)
    g_error("XPASS?: the adopted Welcome is in state %d, not failed; update the expectation",
            state);
  /* libmarmot 0.12.0 decodes and opens an adopted Welcome on arrival
   * (nostrc-qp24.5.1; before, "welcome content decode failed").  This one
   * is for the MDK device's KeyPackage, not Groundhog's -- Groundhog
   * publishes no adopted KeyPackage yet (producer OFF) -- so it is refused
   * there, final, with no invitation (nostrc-5yb3). */
  g_assert_cmpstr(reason, ==, "matching KeyPackage private key not found");
  g_assert_cmpuint(alice->invites, ==, 0);
  g_autoptr(GError) list_error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(alice->service, &list_error);
  g_assert_no_error(list_error);
  g_assert_cmpuint(invites->len, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  g_autofree gchar *what = g_strdup_printf("Groundhog refuses an adopted Welcome: \"%s\"",
                                           reason ? reason : "");
  xfail("unsupported", what);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- adopted: a White Noise group invites a libmarmot adopted KeyPackage ------------ */

/* libmarmot's adopted KeyPackage producer without its build gate
 * (MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER stays OFF, so Groundhog
 * publishes none): the test entry point of libmarmot/src/kp_profile.h. */
MarmotError marmot_create_key_package_adopted_internal(Marmot *m, const uint8_t nostr_pubkey[32],
                                                       const uint8_t nostr_sk[32],
                                                       MarmotAccountSignFunc account_sign,
                                                       void *sign_data,
                                                       MarmotKeyPackageResult *result);

static void
assert_has_all(JsonArray *array, const gchar *const *want, guint n)
{
  for (guint i = 0; i < n; i++)
    if (!mdk_has(array, want[i]))
      g_error("MDK does not see %s in the libmarmot KeyPackage", want[i]);
}

static gboolean
something_unreadable(gpointer data)
{
  return gh_mls_group_get_unreadable(data) > 0;
}

/* Alice's message with this text accepted by a group relay. */
static gboolean
sent_accepted(gpointer data)
{
  MessageWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_SENT;
}

static void
test_white_noise_welcome(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);

  /* Alice's adopted KeyPackage, made by libmarmot in Groundhog's own store
   * (its private keys stay there) and handed to Carol directly. */
  Marmot *marmot = gh_mls_service_get_marmot(alice->service);
  guint8 pk[32], sk[32];
  g_assert_true(nostr_hex2bin(pk, hex[ALICE], sizeof pk));
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[ALICE], sizeof sk));
  MarmotKeyPackageResult kp;
  memset(&kp, 0, sizeof kp);
  g_assert_cmpint(marmot_create_key_package_adopted_internal(marmot, pk, sk, NULL, NULL, &kp), ==,
                  MARMOT_OK);
  memset(sk, 0, sizeof sk);

  /* MDK 0.11's KeyPackage parser admits it as a current-profile KeyPackage
   * advertising what every White Noise group requires (nostrc-qp24.5.2). */
  g_autoptr(JsonObject) view = mdk_call(&driver,
    "\"cmd\":\"parse_key_package\",\"peer\":\"carol\",\"event\":%s", kp.event_json);
  g_autofree gchar *view_text = mdk_json(view);
  g_test_message("MDK 0.11 on a libmarmot adopted KeyPackage: %s", view_text);
  if (!json_object_get_boolean_member(view, "parsed"))
    g_error("MDK 0.11 refuses the libmarmot adopted KeyPackage (%s): %s",
            json_object_get_string_member(view, "class"),
            json_object_get_string_member(view, "error"));
  g_assert_cmpstr(json_object_get_string_member(view, "profile"), ==, "Current");
  static const gchar *const want_ext[] = { "0x0006", "0xf2d1" };
  static const gchar *const want_prop[] = { "0x0008", "0x000a" };
  static const gchar *const want_comp[] = { "0x8001", "0x8003", "0x8004", "0x8006",
                                            "0x8009", "0x800b", "0x800c" };
  assert_has_all(json_object_get_array_member(view, "mls_extensions"), want_ext,
                 G_N_ELEMENTS(want_ext));
  assert_has_all(json_object_get_array_member(view, "mls_proposals"), want_prop,
                 G_N_ELEMENTS(want_prop));
  assert_has_all(json_object_get_array_member(view, "app_components"), want_comp,
                 G_N_ELEMENTS(want_comp));
  /* Not the send or fanout role: libmarmot opens no QUIC stream. */
  g_assert_false(mdk_has(json_object_get_array_member(view, "mls_extensions"), "0xf2d2"));
  g_assert_false(mdk_has(json_object_get_array_member(view, "mls_extensions"), "0xf2d4"));

  /* Carol creates a group as White Noise does (marmot-app's components:
   * agent text stream user_to_agent_default, encrypted media v2), after
   * marmot-app's invite precheck of Alice's KeyPackage. */
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"White Noise group\","
    "\"description\":\"wn\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"white_noise\":true,\"media_endpoints\":[\"https://blossom.example.com\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], kp.event_json, w.x.url);
  marmot_key_package_result_free(&kp);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));
  g_auto(GStrv) components = mdk_strv(json_object_get_array_member(made, "components"));
  g_autofree gchar *component_text = g_strjoinv(",", components);
  g_test_message("White Noise group components: %s", component_text);
  g_assert_true(g_strv_contains((const gchar *const *)components, "0x8006"));
  g_assert_true(g_strv_contains((const gchar *const *)components, "0x800b"));

  /* Groundhog lists the invitation and joins: past admission. */
  GhMlsGroup *ga = join(alice, CAROL);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_assert_cmpstr(gh_mls_group_get_name(ga), ==, "White Noise group");
  guint8 gid_bytes[64];
  const gchar *gid_hex = gh_mls_group_get_group_id(ga);
  gsize gid_len = strlen(gid_hex) / 2;   /* MDK 0.11 group ids are 16 bytes */
  g_assert_cmpuint(gid_len, <=, sizeof gid_bytes);
  g_assert_true(nostr_hex2bin(gid_bytes, gid_hex, gid_len));
  MarmotGroupId gid = marmot_group_id_new(gid_bytes, gid_len);
  MarmotGroupProfile profile = MARMOT_GROUP_PROFILE_LEGACY;
  g_assert_cmpint(marmot_get_group_profile(marmot, &gid, &profile), ==, MARMOT_OK);
  g_assert_cmpint(profile, ==, MARMOT_GROUP_PROFILE_ADOPTED);
  MarmotGroupComponents parts;
  g_assert_cmpint(marmot_get_group_components(marmot, &gid, &parts), ==, MARMOT_OK);
  g_assert_true(parts.has_agent_text_stream);
  g_assert_cmpuint(parts.agent_text_stream.required_member_roles, ==,
                   MARMOT_AGENT_STREAM_ROLE_RECEIVE);
  g_assert_true(parts.has_media_policy);
  g_assert_cmpstr(parts.media_policy.default_blob_endpoints[0].base_url, ==,
                  "https://blossom.example.com/");
  marmot_group_components_clear(&parts);
  marmot_group_id_free(&gid);

  /* Application messages both ways. */
  mdk_send("carol", group, "hello from white noise");
  wait_message(alice, room, "hello from white noise");
  g_assert_cmpstr(gh_message_get_sender(find_message(alice, room, "hello from white noise")), ==,
                  hex[CAROL]);
  send_text(alice, ga, "hello from groundhog");
  {
    MessageWait sent = { alice, room, "hello from groundhog" };
    spin_until(sent_accepted, &sent, "Alice's message accepted by a group relay");
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "hello from groundhog"));
  }

  /* A Commit: libmarmot does not follow adopted Commits yet
   * (nostrc-qp24.5.1, W24 slice H). */
  guint64 joined_epoch = gh_mls_group_get_epoch(ga);
  g_autoptr(JsonObject) renamed = mdk_call(&driver,
    "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
    "\"name\":\"Renamed by White Noise\"", group);
  g_assert_cmpuint(state_epoch(renamed), ==, joined_epoch + 1);
  mdk_send("carol", group, "after the rename");
  /* Today: Groundhog stays in the joined epoch -- not renamed, the message
   * of the next epoch unreadable and waited for (decrypt-pending), not
   * refused for good (change-refused stays FALSE), the group still active:
   * no crash, no stall beyond the wait. */
  spin_until(something_unreadable, ga, "the next epoch's message held unreadable");
  if (gh_mls_group_get_epoch(ga) > joined_epoch)
    g_error("XPASS: Groundhog followed a White Noise Commit; update the expectation");
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, joined_epoch);
  g_assert_cmpstr(gh_mls_group_get_name(ga), ==, "White Noise group");
  g_assert_null(find_message(alice, room, "after the rename"));
  g_assert_true(gh_mls_group_get_decrypt_pending(ga));
  g_assert_false(gh_mls_group_get_change_refused(ga));
  g_assert_true(gh_mls_group_get_active(ga));
  xfail("unsupported", "a White Noise group is joined and its messages flow both ways, but "
                       "libmarmot does not follow adopted Commits yet (nostrc-qp24.5.1, W24 "
                       "slice H): Groundhog stays in the joined epoch");

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- MDK 0.9.0, expected incompatible ------------------------------------------------ */

static void
test_mdk09_probe(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  MdkDriver probe;
  if (!mdk_driver_start_env(&probe, "GH_MDK09_DRIVER")) {
    mdk_driver_stop(&driver);
    g_test_skip("GH_MDK09_DRIVER is unset");
    not_run = TRUE;
    return;
  }
  g_autoptr(JsonObject) hello = mdk_call(&probe,
    "\"cmd\":\"hello\",\"profiles\":[\"marmot-dictionary-proof-v1\"]");
  g_assert_cmpstr(json_object_get_string_member(hello, "mdk_rev"), ==,
                  "a102b1966267c5bfcbe3a822212c0e343ac109ef");
  g_assert_cmpstr(json_object_get_string_member(hello, "proof_extension"), ==, "0xf2f1");
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);

  /* Carol on MDK 0.9.0 publishes a KeyPackage (0xF2F1 v1 proof). */
  {
    g_autoptr(JsonObject) made = mdk_call(&probe,
      "\"cmd\":\"peer_new\",\"peer\":\"carol\",\"secret\":\"%s\"", gh_test_secret[CAROL]);
  }
  g_autoptr(JsonObject) kp = mdk_call(&probe,
    "\"cmd\":\"publish_key_package\",\"peer\":\"carol\",\"relays\":[],"
    "\"to\":[\"%s\",\"%s\"]", w.w.url, w.x.url);
  const gchar *carol_kp = json_object_get_string_member(kp, "event");

  /* MDK 0.11 refuses it: strict cutover admits current-profile KeyPackages
   * only, and a v1 proof is not one. */
  mdk_peer("dave", DAVE);
  g_autoptr(JsonObject) view = mdk_call(&driver,
    "\"cmd\":\"parse_key_package\",\"peer\":\"dave\",\"event\":%s", carol_kp);
  g_autofree gchar *view_text = mdk_json(view);
  g_test_message("MDK 0.11 on an MDK 0.9.0 KeyPackage: %s", view_text);
  if (json_object_get_boolean_member(view, "parsed"))
    g_error("XPASS: MDK 0.11 admits an MDK 0.9.0 KeyPackage; update the expectation");
  const gchar *mdk_class = json_object_get_string_member(view, "class");

  /* Groundhog refuses it too, and invites no one. */
  bool proven = true;
  MarmotError proof = marmot_key_package_event_has_account_proof(carol_kp, &proven);
  g_test_message("libmarmot account-proof check of an MDK 0.9.0 KeyPackage: %s, proven %d",
                 marmot_error_string(proof), proven);
  CheckWait check = { 0 };
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, on_checked,
                             &check);
  spin_until(check_done, &check, "the KeyPackage check");
  g_test_message("New Group check row for an MDK 0.9.0 user: state %d, \"%s\"", check.state,
                 gh_mls_invitee_copy(check.state));
  g_assert_false(gh_mls_invitee_can_invite(check.state));
  g_autoptr(GError) refused = refused_creation(&w, alice, "default; an MDK 0.9.0 user");
  /* Precise classes: MDK 0.11 refuses the v1 proof as another profile, not
   * as a failed check; libmarmot finds no usable proof. */
  g_assert_cmpstr(mdk_class, ==, "unsupported");
  g_assert_nonnull(strstr(json_object_get_string_member(view, "error"),
                          "unsupported proof version 1"));
  g_assert_cmpint(proof, ==, MARMOT_ERR_VALIDATION);
  g_assert_cmpint(check.state, ==, GH_MLS_INVITEE_NOT_SET_UP);
  g_assert_error(refused, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE);
  g_autofree gchar *what = g_strdup_printf("MDK 0.9.0 (0xF2F1 v1 proof) is incompatible: MDK 0.11 "
                                           "says [%s] %s; Groundhog: %s",
                                           mdk_class, json_object_get_string_member(view, "error"),
                                           refused->message);
  xfail(mdk_class, what);

  world_down(&w);
  mdk_driver_stop(&probe);
  mdk_driver_stop(&driver);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mdk011-interop/control", test_control);
  g_test_add_func("/groundhog/mdk011-interop/groundhog-invites-mdk", test_groundhog_invites_mdk);
  g_test_add_func("/groundhog/mdk011-interop/mdk-invites-groundhog", test_mdk_invites_groundhog);
  g_test_add_func("/groundhog/mdk011-interop/adopted-welcome", test_adopted_welcome);
  g_test_add_func("/groundhog/mdk011-interop/white-noise-welcome", test_white_noise_welcome);
  g_test_add_func("/groundhog/mdk011-interop/mdk09-probe", test_mdk09_probe);
  gint rc = g_test_run();
  mls_world_finish();
  if (cases_run == 0) {
    g_printerr("no case matched the requested -p path(s): nothing ran\n");
    return 1;
  }
  /* An expected failure is never green: a run that met its XFAIL exits 77,
   * which CTest reports as Skipped (SKIP_RETURN_CODE); so does a case that
   * could not run. */
  if (rc == 0 && (xfails > 0 || not_run))
    return 77;
  return rc;
}
