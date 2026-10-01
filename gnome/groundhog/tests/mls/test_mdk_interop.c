/* Groundhog <-> MDK 0.8 interop (nostrc-7gx7 release gate, nostrc-77pa;
 * W23). Groundhog accounts are the real GhMlsService of mls-world.h (real
 * stores, transports and signer bus, local store-and-serve relays); the
 * third-party side is MDK v0.8.0 (mdk-core, the legacy 0xF2EE Marmot profile
 * White Noise 0.8 builds on) behind the JSON-lines driver of
 * tests/interop/mdk/driver (mdk-peer.h), which talks to the same local
 * relays: KeyPackages on W, Welcomes gift-wrapped (NIP-59) to the invitee's
 * inbox X, group traffic on G with NIP-42 AUTH. MDK peers use the test keys
 * of Carol (3) and of the Stranger (4, "dave" here).
 *
 *  1. MDK reads Groundhog's KeyPackage (0x8009 proof in the leaf's 0x0006
 *     app_data_dictionary), creates a group with it, Groundhog joins from
 *     the Welcome, messages and Commits (rename, self-update, removal) flow
 *     both ways.
 * Since nostrc-6ukh Groundhog's default admits MDK 0.8 members (no account
 * proof) in these legacy-profile groups, and marks the ones it can't
 * confirm from a KeyPackage they published; the "strict" cases turn on
 * the preference that requires proofs (only-join-verified-mls-groups).
 *  1b. An MDK group holding a second MDK member: by default Groundhog joins
 *     and talks with both; Dave is confirmed by Verify from his KeyPackage
 *     and stays confirmed across his own key-rotating self-update; strict, the
 *     invitation is listed and accepting it fails with NEEDS_UPDATE (the
 *     unproven leaf is not the sender's; listing it at all is nostrc-ho1z).
 *  1c. An MDK admin adds a second MDK member to a group Groundhog is in: by
 *     default Groundhog follows the Commit and reads what comes after;
 *     strict, it refuses the Commit and says so ("change-refused"), never
 *     that it waits for an earlier change (nostrc-prrl), and turning the
 *     preference off applies it.
 *  2a. Strict: Groundhog invites an MDK user: the New Group check row and
 *     the service say NEEDS_UPDATE, and nothing is created or published.
 *  2b. By default Groundhog invites MDK (the check row says so, ready), both
 *     sides commit (rename, admins, add, remove) and converge; a second
 *     Groundhog account (Bob) joins through MDK's Add.
 *  3a. An MDK member leaves (nostrc-2um6; MIP-03 "Leaving a group"): a
 *     Groundhog group (default mode admits the unproven MDK leaf) is made alone
 *     and does not require SelfRemove (MDK's rule), so MDK leaves with a
 *     Remove of itself, a PrivateMessage, which Groundhog (the admin)
 *     commits after its jitter; MDK follows and is out.
 *  3b. Groundhog leaves (default mode): in an MDK group, Groundhog's
 *     SelfRemove is auto-committed by MDK by reference (a PrivateMessage);
 *     Groundhog ends the group as LEFT.
 *  3c. Groundhog leaves a group whose only admin is MDK 0.8 (re-review R1):
 *     MDK's admin auto-commit drops a Remove request and commits nothing,
 *     so Groundhog asks at most GH_MLS_SERVICE_LEAVE_REQUESTS times, then
 *     stops with honest copy and can send again -- no endless loop.
 * Two devices on one account are out of scope (nostrc-yaa1).
 * Results: docs/analysis/marmot-mdk-interop-2026-09-30.md. */
#include "mls-world.h"
#include "mdk-peer.h"

#include "gh-mls-copy.h"
#include "gh-mls-invitee.h"

enum { DAVE = STRANGER };

#define VERIFIED_ONLY "only-join-verified-mls-groups"

typedef struct {
  gboolean done;
  GhMlsMemberIdentity identity;
} VerifyWait;

static gboolean
verify_done(gpointer data)
{
  return ((VerifyWait *)data)->done;
}

static void
on_verified(GObject *source, GAsyncResult *result, gpointer data)
{
  VerifyWait *wait = data;
  g_autoptr(GError) error = NULL;
  wait->identity = gh_mls_service_verify_member_finish(GH_MLS_SERVICE(source), result, &error);
  g_assert_no_error(error);
  wait->done = TRUE;
}

/* The user's Verify of `key` (W24 review H1: nothing is looked up by itself). */
static GhMlsMemberIdentity
verify_member(App *app, GhMlsGroup *group, guint key)
{
  VerifyWait wait = { 0 };
  gh_mls_service_verify_member_async(app->service, group, hex[key], NULL, on_verified, &wait);
  spin_until(verify_done, &wait, "the Verify");
  return wait.identity;
}

static MdkDriver driver;

/* Starts the driver for one test; FALSE (the test skipped) without one. */
static gboolean
mdk_up(void)
{
  if (!mdk_driver_start(&driver)) {
    g_test_skip("GH_MDK_DRIVER is unset (configure with -DBUILD_MDK_INTEROP=ON)");
    return FALSE;
  }
  g_autoptr(JsonObject) hello = mdk_call(&driver, "\"cmd\":\"hello\"");
  g_test_message("MDK peer: %s, mdk %s, openmls %s",
                 json_object_get_string_member(hello, "mdk"),
                 json_object_get_string_member(hello, "mdk_rev"),
                 json_object_get_string_member(hello, "openmls_rev"));
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

/* The peer's KeyPackage, on the write relay W and the inbox relay X (where
 * Groundhog's lookup finds it); its event JSON. */
static gchar *
mdk_publish_key_package(World *w, const gchar *peer)
{
  g_autoptr(JsonObject) kp = mdk_call(&driver,
                                      "\"cmd\":\"publish_key_package\",\"peer\":\"%s\","
                                      "\"relays\":[\"%s\"],\"to\":[\"%s\",\"%s\"]",
                                      peer, w->w.url, w->w.url, w->x.url);
  return g_strdup(json_object_get_string_member(kp, "event"));
}

/* The newest KeyPackage of account key on W, as the peer fetched it (event
 * JSON); *view: what MDK's parse_key_package made of it. */
static gchar *
mdk_fetch_key_package(World *w, const gchar *peer, guint key, JsonObject **view)
{
  g_autoptr(JsonObject) kp = mdk_call(&driver,
                                      "\"cmd\":\"fetch_key_package\",\"peer\":\"%s\","
                                      "\"author\":\"%s\",\"from\":[\"%s\"]",
                                      peer, hex[key], w->w.url);
  *view = json_object_ref(json_object_get_object_member(kp, "mdk"));
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

/* The peer reads the group's kind 445 from its relays (one REQ to EOSE per
 * relay: the test calls it once what it waits for was accepted by G). */
static JsonObject *
mdk_sync(const gchar *peer, const gchar *group)
{
  JsonObject *synced = mdk_call(&driver, "\"cmd\":\"sync\",\"peer\":\"%s\",\"group\":\"%s\"",
                                peer, group);
  g_autofree gchar *text = mdk_json(synced);
  g_test_message("MDK %s sync: %s", peer, text);
  return synced;
}

/* Whether a sync answer holds the application message text by author. */
static gboolean
synced_message(JsonObject *synced, const gchar *author, const gchar *text)
{
  JsonArray *results = json_object_get_array_member(synced, "results");
  for (guint i = 0; i < json_array_get_length(results); i++) {
    JsonObject *r = json_array_get_object_element(results, i);
    if (g_strcmp0(json_object_get_string_member(r, "type"), "application") == 0 &&
        g_strcmp0(json_object_get_string_member(r, "author"), author) == 0 &&
        g_strcmp0(json_object_get_string_member(r, "content"), text) == 0)
      return TRUE;
  }
  return FALSE;
}

static guint
synced_count(JsonObject *synced, const gchar *type)
{
  JsonArray *results = json_object_get_array_member(synced, "results");
  guint n = 0;
  for (guint i = 0; i < json_array_get_length(results); i++)
    if (g_strcmp0(json_object_get_string_member(json_array_get_object_element(results, i),
                                                "type"), type) == 0)
      n++;
  return n;
}

static void
assert_strv_equal(GStrv a, GStrv b)
{
  g_autofree gchar *x = g_strjoinv(",", a), *y = g_strjoinv(",", b);
  g_assert_cmpstr(x, ==, y);
}

/* The MDK peer's view (a "state" object) equals the Groundhog account's:
 * group, epoch, name, members, admins. */
static void
assert_converged(GhMlsGroup *group, JsonObject *state)
{
  g_assert_cmpstr(json_object_get_string_member(state, "group"), ==,
                  gh_mls_group_get_group_id(group));
  g_assert_cmpuint((guint64)json_object_get_int_member(state, "epoch"), ==,
                   gh_mls_group_get_epoch(group));
  g_assert_cmpstr(json_object_get_string_member(state, "name"), ==, gh_mls_group_get_name(group));
  g_auto(GStrv) mdk_members = mdk_strv(json_object_get_array_member(state, "members"));
  g_auto(GStrv) gh_members = gh_mls_group_dup_members(group);
  assert_strv_equal(mdk_members, gh_members);
  g_auto(GStrv) mdk_admins = mdk_strv(json_object_get_array_member(state, "admins"));
  g_auto(GStrv) gh_admins = gh_mls_group_dup_admins(group);
  assert_strv_equal(mdk_admins, gh_admins);
}

static guint64
state_epoch(JsonObject *state)
{
  return (guint64)json_object_get_int_member(state, "epoch");
}

/* ---- Groundhog-side waits ------------------------------------------------------------ */

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

#define wait_name(group_, name_) \
  G_STMT_START { NameWait nw_ = { (group_), (name_) }; \
    spin_until(name_is, &nw_, "the group's name " name_); } G_STMT_END

static gboolean
group_ended(gpointer data)
{
  return gh_mls_group_get_end(data) != GH_MLS_GROUP_END_NONE;
}

static gboolean
welcomes_sent(gpointer data)
{
  return gh_mls_group_get_unsent_welcomes(data) == 0;
}

typedef struct {
  App *app;
  const gchar *room_id;
  const gchar *text;
} StatusWait;

static gboolean
message_sent(gpointer data)
{
  StatusWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_SENT;
}

/* Sends from Groundhog and waits until a group relay accepted it. */
static void
send_accepted(App *app, GhMlsGroup *group, const gchar *text)
{
  send_text(app, group, text);
  StatusWait wait = { app, gh_mls_group_get_room_id(group), text };
  spin_until(message_sent, &wait, "the message accepted by a group relay");
}

static void
change_done(OpWait *wait)
{
  spin_until(op_done, wait, "the group change");
  g_assert_no_error(wait->error);
  g_assert_true(wait->ok);
}

/* libmarmot's record of a processed Welcome: its state (MarmotWelcomeState)
 * and failure reason (transfer full, or NULL). */
static gint
welcome_record(App *app, const gchar *wrap_id, gchar **reason)
{
  g_autoptr(GError) error = NULL;
  MarmotStorage *storage = gh_store_marmot_new(app->store, &error);
  g_assert_no_error(error);
  guint8 wrapper[32];
  g_assert_true(nostr_hex2bin(wrapper, wrap_id, sizeof wrapper));
  bool found = false;
  int state = -1;
  char *why = NULL;
  g_assert_cmpint(storage->find_processed_welcome(storage->ctx, wrapper, &found, &state, &why),
                  ==, MARMOT_OK);
  marmot_storage_free(storage);
  g_assert_true(found);
  *reason = g_strdup(why);
  free(why);
  return state;
}

static const gchar *
welcome_to(JsonObject *made, const gchar *pubkey)
{
  JsonArray *welcomes = json_object_get_array_member(made, "welcomes");
  for (guint i = 0; i < json_array_get_length(welcomes); i++) {
    JsonObject *welcome = json_array_get_object_element(welcomes, i);
    if (g_strcmp0(json_object_get_string_member(welcome, "to"), pubkey) == 0)
      return json_object_get_string_member(welcome, "wrapper_id");
  }
  g_assert_not_reached();
}

/* The one Welcome the MDK peer finds for itself on X, accepted: its group's
 * state. */
static JsonObject *
mdk_join(World *w, const gchar *peer, const gchar *inviter, gchar **out_group)
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
    g_error("MDK %s refused the Welcome: %s", peer,
            json_object_get_string_member(welcome, "error"));
  g_assert_cmpstr(json_object_get_string_member(welcome, "sender"), ==, inviter);
  JsonObject *state = mdk_call(&driver,
                               "\"cmd\":\"accept_welcome\",\"peer\":\"%s\",\"wrapper_id\":\"%s\"",
                               peer, json_object_get_string_member(welcome, "wrapper_id"));
  *out_group = g_strdup(json_object_get_string_member(state, "group"));
  return state;
}

/* ---- 1. MDK invites Groundhog -------------------------------------------------------- */

static void
check_mdk_reads_key_package(World *w, const gchar *peer, guint key, gchar **out_event)
{
  g_autoptr(JsonObject) view = NULL;
  *out_event = mdk_fetch_key_package(w, peer, key, &view);
  g_autofree gchar *text = mdk_json(view);
  g_test_message("MDK 0.8 parse_key_package on Groundhog's KeyPackage: %s", text);
  if (!json_object_get_boolean_member(view, "parsed"))
    g_error("MDK 0.8 cannot read a libmarmot 0.10.0 KeyPackage: %s",
            json_object_get_string_member(view, "error"));
  JsonArray *capabilities = json_object_get_array_member(view, "capability_extensions");
  g_assert_true(mdk_has(capabilities, "0x0006"));
  g_assert_true(mdk_has(capabilities, "0x000a"));
  g_assert_true(mdk_has(capabilities, "0xf2ee"));
  g_assert_true(mdk_has(json_object_get_array_member(view, "leaf_extensions"), "0x0006"));
}

static void
test_mdk_invites_groundhog(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  accept_contact(alice, CAROL);

  /* nostrc-77pa: MDK 0.8 reads a libmarmot 0.10.0 KeyPackage. */
  g_autofree gchar *alice_kp = NULL;
  check_mdk_reads_key_package(&w, "carol", ALICE, &alice_kp);

  /* MDK makes a group with Alice in it (both admins), and sends her the
   * Welcome as a NIP-59 gift wrap to her inbox X. */
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Made by MDK\","
    "\"description\":\"interop\",\"relays\":[\"%s\"],\"admins\":[\"%s\",\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], hex[ALICE], alice_kp, w.x.url);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));

  /* MIP-02: Groundhog lists the invitation from Carol and joins. */
  GhMlsGroup *ga = join(alice, CAROL);
  g_auto(GStrv) group_relays = gh_mls_group_dup_relays(ga);
  g_assert_cmpuint(g_strv_length(group_relays), ==, 1);
  g_assert_cmpstr(group_relays[0], ==, w.g.url);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  assert_converged(ga, made);

  /* MIP-03, both ways. */
  mdk_send("carol", group, "hello from mdk");
  wait_message(alice, room, "hello from mdk");
  g_assert_cmpstr(gh_message_get_sender(find_message(alice, room, "hello from mdk")), ==,
                  hex[CAROL]);
  send_accepted(alice, ga, "hello from groundhog");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "hello from groundhog"));
  }

  /* Commits both ways: MDK renames; Groundhog renames; MDK rotates its leaf. */
  {
    g_autoptr(JsonObject) renamed = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"name\":\"Renamed by MDK\"", group);
    wait_name(ga, "Renamed by MDK");
    assert_converged(ga, renamed);
  }
  {
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(alice->service, ga, "Renamed by Groundhog", NULL, NULL,
                                         on_changed, &renamed);
    change_done(&renamed);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_cmpuint(synced_count(synced, "commit"), ==, 1);
    assert_converged(ga, json_object_get_object_member(synced, "state"));
  }
  {
    g_autoptr(JsonObject) rotated = mdk_call(&driver,
      "\"cmd\":\"self_update\",\"peer\":\"carol\",\"group\":\"%s\"", group);
    wait_epoch(ga, (gint)state_epoch(rotated));
    assert_converged(ga, rotated);
  }
  mdk_send("carol", group, "mdk after the commits");
  wait_message(alice, room, "mdk after the commits");
  send_accepted(alice, ga, "groundhog after the commits");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "groundhog after the commits"));
  }

  /* An MDK admin removes Alice: Groundhog ends the group, removed by Carol
   * (nostrc-xrya). */
  g_autoptr(JsonObject) removed = mdk_call(&driver,
    "\"cmd\":\"remove_members\",\"peer\":\"carol\",\"group\":\"%s\",\"members\":[\"%s\"]",
    group, hex[ALICE]);
  spin_until(group_ended, ga, "Alice's group ending");
  g_assert_cmpint(gh_mls_group_get_end(ga), ==, GH_MLS_GROUP_END_REMOVED);
  g_assert_cmpstr(gh_mls_group_get_removed_by(ga), ==, hex[CAROL]);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- 1b. An MDK group with a second MDK member ------------------------------------- */

/* Carol (MDK) makes a group with Alice and Dave (MDK): the tree Alice's
 * Welcome carries holds Dave's leaf, which has no account proof and is not
 * the Welcome's sender. */
static JsonObject *
mdk_group_with_dave(World *w, gchar **out_alice_wrap)
{
  mdk_peer("carol", CAROL);
  mdk_peer("dave", DAVE);
  g_autofree gchar *dave_kp_published = mdk_publish_key_package(w, "dave");
  g_autofree gchar *alice_kp = NULL, *dave_kp = NULL;
  check_mdk_reads_key_package(w, "carol", ALICE, &alice_kp);
  g_autoptr(JsonObject) dave_view = NULL;
  dave_kp = mdk_fetch_key_package(w, "carol", DAVE, &dave_view);
  JsonObject *made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Three with MDK\","
    "\"description\":\"interop\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s,%s],\"welcome_relays\":[\"%s\"]",
    w->g.url, hex[CAROL], alice_kp, dave_kp, w->x.url);
  *out_alice_wrap = g_strdup(welcome_to(made, hex[ALICE]));
  return made;
}

static void
test_mdk_group_unproven_member_strict(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, TRUE);
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  g_autofree gchar *wrap = NULL;
  g_autoptr(JsonObject) made = mdk_group_with_dave(&w, &wrap);

  /* Groundhog lists the invitation (libmarmot's preview does not open the
   * tree), and accepting it fails: the tree holds Dave's leaf, which has no
   * account proof and is not the Welcome's sender (joining.md step 5 with
   * the 0.10.0 proof rule). The Welcome is recorded as failed, nothing is
   * joined, and the UI says why. */
  spin_until(has_invite, alice, "the invitation from Carol");
  g_autofree gchar *listed = the_invite(alice, CAROL);
  g_assert_cmpstr(listed, ==, wrap);
  g_autoptr(GError) error = NULL;
  GhMlsGroup *refused = gh_mls_service_accept_invite(alice->service, wrap, &error);
  g_assert_null(refused);
  g_autofree gchar *copy = gh_mls_error_copy(error);
  g_autofree gchar *reason = NULL;
  gint state = welcome_record(alice, wrap, &reason);
  g_test_message("Groundhog (strict) accepting an MDK Welcome with a second MDK member: %s %d "
                 "\"%s\"; the UI says \"%s\"; Welcome state %d, \"%s\"",
                 g_quark_to_string(error->domain), error->code, error->message, copy, state,
                 reason ? reason : "");
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NEEDS_UPDATE);
  g_assert_cmpint(state, ==, MARMOT_WELCOME_STATE_FAILED);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  g_autoptr(GError) list_error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(alice->service, &list_error);
  g_assert_no_error(list_error);
  g_assert_cmpuint(invites->len, ==, 0);   /* a final refusal is not an invitation */

  /* Dave, on MDK, joins the same group without trouble. */
  g_autofree gchar *dave_group = NULL;
  g_autoptr(JsonObject) dave_state = mdk_join(&w, "dave", hex[CAROL], &dave_group);
  g_assert_cmpstr(dave_group, ==, json_object_get_string_member(made, "group"));

  world_down(&w);
  mdk_driver_stop(&driver);
}

static void
test_mdk_group_unproven_member_default(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  g_autofree gchar *wrap = NULL;
  g_autoptr(JsonObject) made = mdk_group_with_dave(&w, &wrap);
  const gchar *group = json_object_get_string_member(made, "group");

  GhMlsGroup *ga = join(alice, CAROL);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  g_autofree gchar *dave_group = NULL;
  g_autoptr(JsonObject) dave_state = mdk_join(&w, "dave", hex[CAROL], &dave_group);
  assert_converged(ga, made);
  assert_converged(ga, dave_state);

  mdk_send("dave", group, "dave to everyone");
  wait_message(alice, room, "dave to everyone");
  send_accepted(alice, ga, "alice to everyone");
  g_autoptr(JsonObject) carol_sync = mdk_sync("carol", group);
  g_assert_true(synced_message(carol_sync, hex[DAVE], "dave to everyone"));
  g_assert_true(synced_message(carol_sync, hex[ALICE], "alice to everyone"));
  g_autoptr(JsonObject) dave_sync = mdk_sync("dave", group);
  g_assert_true(synced_message(dave_sync, hex[ALICE], "alice to everyone"));

  /* Dave (no proof) was in the group before Alice: nothing in hand, so
   * not verified, who added him not known, and nothing looked up until
   * Alice asks; her Verify finds the KeyPackage he published (nostrc-6ukh,
   * W24 review H1). */
  g_autofree gchar *by = NULL;
  g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[DAVE], &by), ==,
                  GH_MLS_MEMBER_UNVERIFIED);
  g_assert_null(by);
  g_assert_cmpint(verify_member(alice, ga, DAVE), ==, GH_MLS_MEMBER_VERIFIED);

  /* Dave self-updates: MDK rotates his leaf's signature key. The new key
   * was signed in by the verified one, so he stays verified, and nobody
   * "added" him (W24 review M1). */
  {
    g_autoptr(JsonObject) rotated = mdk_call(&driver,
      "\"cmd\":\"self_update\",\"peer\":\"dave\",\"group\":\"%s\"", group);
    wait_epoch(ga, (gint)state_epoch(rotated));
    assert_converged(ga, rotated);
  }
  g_autofree gchar *by_after = NULL;
  g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[DAVE], &by_after), ==,
                  GH_MLS_MEMBER_VERIFIED);
  g_assert_null(by_after);
  /* Carol's creator leaf is in no KeyPackage she published, but it signed
   * the Welcome she sent Alice (the NIP-59 seal): she vouched for it
   * (W24 review owkh). */
  g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[CAROL], NULL), ==,
                  GH_MLS_MEMBER_VERIFIED);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- 1c. An MDK admin adds a second MDK member later ----------------------------------- */

static gboolean
unreadable_positive(gpointer data)
{
  return gh_mls_group_get_unreadable(data) > 0;
}

static gboolean
change_refused(gpointer data)
{
  return gh_mls_group_get_change_refused(data);
}

/* strict: Alice requires proofs (nostrc-prrl); else the default. */
static void
mdk_adds_unproven_member(gboolean strict)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, strict);
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);
  mdk_peer("dave", DAVE);
  g_autofree gchar *alice_kp = NULL;
  check_mdk_reads_key_package(&w, "carol", ALICE, &alice_kp);
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Grows on MDK\","
    "\"description\":\"interop\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_kp, w.x.url);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));
  GhMlsGroup *ga = join(alice, CAROL);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  guint64 epoch = gh_mls_group_get_epoch(ga);

  /* Carol adds Dave (MDK, no proof) in one Commit, then writes. */
  g_autofree gchar *dave_kp_published = mdk_publish_key_package(&w, "dave");
  g_autoptr(JsonObject) dave_view = NULL;
  g_autofree gchar *dave_kp = mdk_fetch_key_package(&w, "carol", DAVE, &dave_view);
  g_autoptr(JsonObject) added = mdk_call(&driver,
    "\"cmd\":\"add_members\",\"peer\":\"carol\",\"group\":\"%s\",\"key_packages\":[%s],"
    "\"welcome_relays\":[\"%s\"]", group, dave_kp, w.x.url);
  mdk_send("carol", group, "carol after adding dave");

  if (!strict) {
    /* Default (nostrc-6ukh): Groundhog follows Carol's Add and reads on;
     * Dave, added by Carol, is not verified until Alice asks (H1). */
    wait_message(alice, room, "carol after adding dave");
    assert_converged(ga, added);
    g_autofree gchar *by = NULL;
    g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[DAVE], &by), ==,
                    GH_MLS_MEMBER_UNVERIFIED);
    g_assert_cmpstr(by, ==, hex[CAROL]);
    g_assert_cmpint(verify_member(alice, ga, DAVE), ==, GH_MLS_MEMBER_VERIFIED);
    g_assert_cmpint(gh_mls_group_get_member_identity(ga, hex[CAROL], NULL), ==,
                    GH_MLS_MEMBER_VERIFIED);   /* she sent the Welcome (owkh) */
    g_assert_false(gh_mls_group_get_change_refused(ga));
    world_down(&w);
    mdk_driver_stop(&driver);
    return;
  }

  /* Strict: Groundhog refuses that Commit for good, says so, and never
   * that it waits for an earlier change (nostrc-prrl). */
  spin_until(change_refused, ga, "Groundhog refusing the MDK Add");
  spin_until(unreadable_positive, ga, "Groundhog holding the MDK group's traffic");
  drain();
  g_auto(GStrv) members = gh_mls_group_dup_members(ga);
  g_test_message("Groundhog (strict) after an MDK Add of an unproven member: epoch %" G_GUINT64_FORMAT
                 " (MDK %" G_GINT64_FORMAT "), members %u, unreadable %u, decrypt-pending %d, "
                 "change-refused %d, active %d, end %d; the view says \"%s\"",
                 gh_mls_group_get_epoch(ga), json_object_get_int_member(added, "epoch"),
                 g_strv_length(members), gh_mls_group_get_unreadable(ga),
                 gh_mls_group_get_decrypt_pending(ga), gh_mls_group_get_change_refused(ga),
                 gh_mls_group_get_active(ga), gh_mls_group_get_end(ga),
                 gh_mls_refused_copy(TRUE));
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, epoch);
  g_assert_null(find_message(alice, room, "carol after adding dave"));
  g_assert_false(gh_mls_group_get_decrypt_pending(ga));
  g_assert_true(gh_mls_group_get_active(ga));

  /* The preference off: the change applies and the message is read. */
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, FALSE);
  wait_message(alice, room, "carol after adding dave");
  g_assert_false(gh_mls_group_get_change_refused(ga));
  assert_converged(ga, added);

  world_down(&w);
  mdk_driver_stop(&driver);
}

static void
test_mdk_adds_unproven_member(void)
{
  mdk_adds_unproven_member(FALSE);
}

static void
test_mdk_adds_unproven_member_strict(void)
{
  mdk_adds_unproven_member(TRUE);
}

/* ---- 2a. Groundhog invites an MDK user, strict ---------------------------------------- */

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

static void
test_groundhog_invites_mdk_strict(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  g_settings_set_boolean(alice->settings, VERIFIED_ONLY, TRUE);
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  g_autofree gchar *carol_kp = mdk_publish_key_package(&w, "carol");
  accept_contact(alice, CAROL);

  /* libmarmot: the KeyPackage is valid and carries no account proof. */
  bool proven = true;
  g_assert_cmpint(marmot_key_package_event_has_account_proof(carol_kp, &proven), ==, MARMOT_OK);
  g_assert_false(proven);

  /* New Group's KeyPackage check row (charter §7.9). */
  CheckWait check = { 0 };
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, on_checked,
                             &check);
  spin_until(check_done, &check, "the KeyPackage check");
  g_test_message("New Group check row for an MDK 0.8 user: state %d, \"%s\", can invite %d",
                 check.state, gh_mls_invitee_copy(check.state),
                 gh_mls_invitee_can_invite(check.state));
  g_assert_cmpint(check.state, ==, GH_MLS_INVITEE_NEEDS_UPDATE);
  g_assert_false(gh_mls_invitee_can_invite(check.state));

  /* The service refuses the invitation and changes nothing. */
  guint g_events = w.g.events, x_events = w.x.events;
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(alice->service, "Refused", NULL, relays, people, NULL,
                                    on_created, &wait);
  spin_until(op_done, &wait, "the refused creation");
  g_assert_null(wait.result);
  g_autofree gchar *copy = gh_mls_error_copy(wait.error);
  g_test_message("Groundhog (strict) inviting an MDK 0.8 user: %s %d \"%s\"; the UI says \"%s\"",
                 g_quark_to_string(wait.error->domain), wait.error->code, wait.error->message,
                 copy);
  g_assert_error(wait.error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NEEDS_UPDATE);
  g_clear_error(&wait.error);
  g_assert_cmpuint(w.g.events, ==, g_events);
  g_assert_cmpuint(w.x.events, ==, x_events);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- 2b. Groundhog invites MDK, default ------------------------------------------------- */

static void
test_groundhog_invites_mdk_default(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  mdk_peer("carol", CAROL);
  g_autofree gchar *carol_kp = mdk_publish_key_package(&w, "carol");
  accept_contact(alice, CAROL);

  /* The New Group check row: she can be invited (nostrc-6ukh). */
  CheckWait check = { 0 };
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, on_checked,
                             &check);
  spin_until(check_done, &check, "the KeyPackage check");
  g_assert_cmpint(check.state, ==, GH_MLS_INVITEE_READY_UNPROVEN);
  g_assert_true(gh_mls_invitee_can_invite(check.state));

  /* Groundhog creates the group and invites Carol (MDK). */
  GhMlsGroup *ga = create_group(alice, "Made by Groundhog", (const guint[]){ CAROL }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autofree gchar *group = NULL;
  {
    g_autoptr(JsonObject) joined = mdk_join(&w, "carol", hex[ALICE], &group);
    g_assert_cmpstr(group, ==, gh_mls_group_get_group_id(ga));
    assert_converged(ga, joined);
  }

  /* Messages both ways. */
  send_accepted(alice, ga, "hello mdk");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "hello mdk"));
  }
  mdk_send("carol", group, "hello groundhog");
  wait_message(alice, room, "hello groundhog");

  /* Groundhog commits: a rename, then Carol becomes an admin. */
  {
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(alice->service, ga, "Renamed by Groundhog", NULL, NULL,
                                         on_changed, &renamed);
    change_done(&renamed);
    OpWait admins = { 0 };
    const gchar *both[] = { hex[ALICE], hex[CAROL], NULL };
    gh_mls_service_set_admins_async(alice->service, ga, both, NULL, on_changed, &admins);
    change_done(&admins);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_cmpuint(synced_count(synced, "commit"), ==, 2);
    assert_converged(ga, json_object_get_object_member(synced, "state"));
  }

  /* MDK commits: a rename, then it adds Bob (Groundhog). */
  {
    g_autoptr(JsonObject) renamed = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"name\":\"Renamed by MDK\"", group);
    wait_name(ga, "Renamed by MDK");
    assert_converged(ga, renamed);
  }
  accept_contact(bob, CAROL);
  g_autoptr(JsonObject) bob_view = NULL;
  g_autofree gchar *bob_kp = mdk_fetch_key_package(&w, "carol", BOB, &bob_view);
  g_assert_true(json_object_get_boolean_member(bob_view, "parsed"));
  g_autoptr(JsonObject) added = mdk_call(&driver,
    "\"cmd\":\"add_members\",\"peer\":\"carol\",\"group\":\"%s\",\"key_packages\":[%s],"
    "\"welcome_relays\":[\"%s\"]", group, bob_kp, w.x.url);
  wait_members(ga, 3);
  assert_converged(ga, added);
  GhMlsGroup *gb = join(bob, CAROL);
  assert_converged(gb, added);

  mdk_send("carol", group, "three of us");
  wait_message(alice, room, "three of us");
  wait_message(bob, room, "three of us");
  send_accepted(bob, gb, "bob here");
  wait_message(alice, room, "bob here");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[BOB], "bob here"));
  }

  /* MDK removes Bob; then Groundhog removes Carol. */
  {
    g_autoptr(JsonObject) removed = mdk_call(&driver,
      "\"cmd\":\"remove_members\",\"peer\":\"carol\",\"group\":\"%s\",\"members\":[\"%s\"]",
      group, hex[BOB]);
    wait_members(ga, 2);
    assert_converged(ga, removed);
    spin_until(group_ended, gb, "Bob's group ending");
    g_assert_cmpint(gh_mls_group_get_end(gb), ==, GH_MLS_GROUP_END_REMOVED);
    g_assert_cmpstr(gh_mls_group_get_removed_by(gb), ==, hex[CAROL]);
  }
  {
    OpWait removed = { 0 };
    const gchar *carol_only[] = { hex[CAROL], NULL };
    gh_mls_service_remove_members_async(alice->service, ga, carol_only, NULL, on_changed,
                                        &removed);
    change_done(&removed);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    JsonObject *state = json_object_get_object_member(synced, "state");
    g_test_message("MDK after Groundhog removed it: state %s, epoch %" G_GINT64_FORMAT
                   " (Groundhog %" G_GUINT64_FORMAT ")",
                   json_object_get_string_member(state, "state"),
                   json_object_get_int_member(state, "epoch"), gh_mls_group_get_epoch(ga));
    g_assert_false(mdk_has(json_object_get_array_member(state, "members"), hex[CAROL]));
  }

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- 3. Leaving (nostrc-2um6) ------------------------------------------------------------ */

typedef struct {
  WireRelay *relay;
  guint n;
} StoredCount;

static gboolean
stored_reached(gpointer data)
{
  StoredCount *count = data;
  return count->relay->stored->len >= count->n;
}

static void
on_member_left(GhMlsGroup *group, const gchar *pubkey, gpointer data)
{
  (void)group;
  g_ptr_array_add(data, g_strdup(pubkey));
}

/* 3a: Groundhog creates the group with Carol (MDK; her leaf has no proof,
 * which the default admits in a legacy group, nostrc-6ukh). Groundhog
 * makes its groups alone, so they do not require SelfRemove (MDK's rule for
 * an empty invitee list, review L1), and MDK 0.8's leave_group() is then a
 * Remove of itself sent as a PrivateMessage. Groundhog, the admin, keeps it,
 * commits it by reference after the jitter, and reports "member-left";
 * MDK follows its own removal. (The SelfRemove form is case 3b, and MDK's
 * SelfRemove bytes are the MDK_SELF_REMOVE_* vector.) */
static void
test_mdk_member_leaves(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  g_autofree gchar *carol_kp = mdk_publish_key_package(&w, "carol");
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "Carol leaves", (const guint[]){ CAROL }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autofree gchar *group = NULL;
  {
    g_autoptr(JsonObject) joined = mdk_join(&w, "carol", hex[ALICE], &group);
    assert_converged(ga, joined);
  }
  g_autoptr(GPtrArray) gone = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(ga, "member-left", G_CALLBACK(on_member_left), gone);

  g_autoptr(JsonObject) left = mdk_call(&driver,
    "\"cmd\":\"leave_group\",\"peer\":\"carol\",\"group\":\"%s\"", group);
  const gchar *message = json_object_get_string_member(left, "mls_message");
  g_test_message("MDK leave MLSMessage: %s", message);
  g_autofree gchar *refs = mdk_json(left);
  g_test_message("MDK leave_group: %s", refs);
  /* A PrivateMessage (00 01 00 02): MDK's Remove of itself (review M1). */
  g_assert_true(g_str_has_prefix(message, "00010002"));

  wait_members(ga, 1);
  g_assert_cmpuint(gone->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(gone, 0), ==, hex[CAROL]);
  g_autoptr(JsonObject) synced = mdk_sync("carol", group);
  JsonObject *state = json_object_get_object_member(synced, "state");
  g_assert_cmpuint(synced_count(synced, "commit"), ==, 1);
  g_assert_false(mdk_has(json_object_get_array_member(state, "members"), hex[CAROL]));
  g_test_message("MDK after its leave was committed: %s",
                 json_object_get_string_member(state, "state"));
  send_accepted(alice, ga, "after carol left");
  g_signal_handlers_disconnect_by_data(ga, gone);
  world_down(&w);
  mdk_driver_stop(&driver);
}

/* 3b: MDK (Carol, the only admin) makes a group with Alice; Groundhog's
 * KeyPackage advertises SelfRemove, so MDK requires it. Alice leaves for
 * everyone: her SelfRemove reaches G, MDK auto-commits it by reference
 * (MDK 0.8 messages/proposal.rs: any member commits a SelfRemove), and
 * Groundhog ends the group as LEFT -- default mode, no test hook. */
static void
test_groundhog_leaves(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  accept_contact(alice, CAROL);
  g_autofree gchar *alice_kp = NULL;
  {
    g_autoptr(JsonObject) view = NULL;
    alice_kp = mdk_fetch_key_package(&w, "carol", ALICE, &view);
    g_assert_true(json_object_get_boolean_member(view, "parsed"));
    g_assert_true(mdk_has(json_object_get_array_member(view, "capability_proposals"), "0x000a"));
  }
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Alice leaves\","
    "\"description\":\"interop\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_kp, w.x.url);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));
  GhMlsGroup *ga = join(alice, CAROL);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  assert_converged(ga, made);
  mdk_send("carol", group, "before you go");
  wait_message(alice, room, "before you go");

  g_assert_cmpint(gh_mls_service_leave_kind(alice->service, ga), ==, GH_MLS_LEAVE_EVERYONE);
  StoredCount on_g = { &w.g, w.g.stored->len + 1 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(alice->service, ga, &error));
  g_assert_no_error(error);
  g_assert_true(gh_mls_group_get_leaving(ga));
  spin_until(stored_reached, &on_g, "Alice's SelfRemove on G");

  g_autoptr(JsonObject) synced = mdk_sync("carol", group);
  JsonArray *results = json_object_get_array_member(synced, "results");
  gboolean committed = FALSE;
  for (guint i = 0; i < json_array_get_length(results); i++) {
    JsonObject *r = json_array_get_object_element(results, i);
    if (g_strcmp0(json_object_get_string_member(r, "type"), "proposal") == 0 &&
        json_object_has_member(r, "auto_commit") &&
        json_object_get_null_member(r, "auto_commit_error"))
      committed = TRUE;
  }
  g_assert_true(committed);
  JsonObject *state = json_object_get_object_member(synced, "state");
  g_assert_false(mdk_has(json_object_get_array_member(state, "members"), hex[ALICE]));

  spin_until(group_ended, ga, "Alice's leave committed by MDK");
  g_assert_cmpint(gh_mls_group_get_end(ga), ==, GH_MLS_GROUP_END_LEFT);
  g_assert_false(gh_mls_group_get_leaving(ga));
  g_assert_null(gh_mls_group_get_removed_by(ga));
  world_down(&w);
  mdk_driver_stop(&driver);
}

/* 3c (re-review R1): Groundhog (Alice) makes the group with Carol (MDK) and
 * makes Carol its only admin, then leaves. The group does not require
 * SelfRemove, so Alice's leave is a Remove request; MDK 0.8's admin
 * auto-commit (messages/proposal.rs auto_commit_proposal) keeps only
 * SelfRemoves and commits an empty Commit, which keeps Alice. Alice asks
 * once more, then stops: "not processed", sending again, no more requests
 * and no more Commits however often MDK syncs. */
static void
test_groundhog_leaves_mdk_admin(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  mdk_peer("carol", CAROL);
  g_autofree gchar *carol_kp = mdk_publish_key_package(&w, "carol");
  accept_contact(alice, CAROL);
  GhMlsGroup *ga = create_group(alice, "MDK admin", (const guint[]){ CAROL }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autofree gchar *group = NULL;
  {
    g_autoptr(JsonObject) joined = mdk_join(&w, "carol", hex[ALICE], &group);
    assert_converged(ga, joined);
  }
  {
    OpWait admins = { 0 };
    const gchar *carol_only[] = { hex[CAROL], NULL };
    gh_mls_service_set_admins_async(alice->service, ga, carol_only, NULL, on_changed, &admins);
    change_done(&admins);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    assert_converged(ga, json_object_get_object_member(synced, "state"));
  }
  g_assert_false(gh_mls_group_get_is_admin(ga));
  g_assert_cmpint(gh_mls_service_leave_kind(alice->service, ga), ==, GH_MLS_LEAVE_ADMINS);

  StoredCount on_g = { &w.g, w.g.stored->len + 1 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(alice->service, ga, &error));
  g_assert_no_error(error);
  guint rounds = 0;
  while (gh_mls_group_get_leaving(ga)) {
    g_assert_cmpuint(rounds, <, GH_MLS_SERVICE_LEAVE_REQUESTS);   /* bounded */
    spin_until(stored_reached, &on_g, "Alice's Remove request on G");
    guint64 epoch = gh_mls_group_get_epoch(ga);
    /* After the sync: MDK's Commit, then (if Alice goes on) her next
     * request, which she makes as soon as she applies that Commit. */
    on_g.n = w.g.stored->len + 2;
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_cmpuint(synced_count(synced, "proposal"), ==, 1);   /* MDK auto-commits... */
    JsonObject *state = json_object_get_object_member(synced, "state");
    g_assert_true(mdk_has(json_object_get_array_member(state, "members"), hex[ALICE]));
    wait_epoch(ga, (gint)(epoch + 1));                            /* ...an empty Commit */
    g_test_message("round %u: Alice at epoch %" G_GUINT64_FORMAT ", leaving %d", rounds,
                   gh_mls_group_get_epoch(ga), gh_mls_group_get_leaving(ga));
    rounds++;
  }
  g_assert_cmpuint(rounds, ==, GH_MLS_SERVICE_LEAVE_REQUESTS);
  g_assert_cmpint(gh_mls_group_get_leave_failure(ga), ==, GH_MLS_LEAVE_FAILURE_NOT_PROCESSED);
  g_assert_true(gh_mls_group_get_active(ga));

  /* Nothing more: no request, so MDK commits nothing. */
  guint stored = w.g.stored->len;
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_cmpuint(synced_count(synced, "proposal"), ==, 0);
    g_assert_cmpuint(synced_count(synced, "commit"), ==, 0);
  }
  g_assert_cmpuint(w.g.stored->len, ==, stored);
  /* Sending works again. */
  send_accepted(alice, ga, "still here");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "still here"));
  }
  world_down(&w);
  mdk_driver_stop(&driver);
}

/* 3d (rebase onto nostrc-2lrz): MDK (Carol, the only admin) makes a group
 * with Alice and Bob (Groundhog); their KeyPackages advertise SelfRemove, so
 * MDK requires it and a leave is any member's to commit. Bob leaves; Alice,
 * not an admin, commits his SelfRemove after the jitter. Her first staging
 * is refused with MARMOT_ERR_EVENT_RATE (forced by a test hook, as libmarmot
 * refuses a Commit it cannot date within a minute): the retry a second later
 * must not ask for an admin, so it goes through with no failed attempt.
 * Carol is not synced: MDK auto-commits every SelfRemove it reads, which
 * would race Alice's Commit (that is case 3b). */
static void
test_groundhog_member_commits_leave(void)
{
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  mdk_peer("carol", CAROL);
  accept_contact(alice, CAROL);
  accept_contact(bob, CAROL);
  g_autoptr(JsonObject) alice_view = NULL;
  g_autoptr(JsonObject) bob_view = NULL;
  g_autofree gchar *alice_kp = mdk_fetch_key_package(&w, "carol", ALICE, &alice_view);
  g_autofree gchar *bob_kp = mdk_fetch_key_package(&w, "carol", BOB, &bob_view);
  g_assert_true(mdk_has(json_object_get_array_member(bob_view, "capability_proposals"),
                        "0x000a"));
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Bob leaves\","
    "\"description\":\"interop\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s,%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_kp, bob_kp, w.x.url);
  GhMlsGroup *ga = join(alice, CAROL);
  GhMlsGroup *gb = join(bob, CAROL);
  assert_converged(ga, made);
  assert_converged(gb, made);
  g_assert_false(gh_mls_group_get_is_admin(ga));
  g_assert_cmpint(gh_mls_service_leave_kind(bob->service, gb), ==, GH_MLS_LEAVE_EVERYONE);
  g_autoptr(GPtrArray) gone = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(ga, "member-left", G_CALLBACK(on_member_left), gone);

  guint retries = gh_mls_service_test_rate_retries();
  guint failures = gh_mls_service_test_departure_failures();
  gh_mls_service_test_refuse_rate(1);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  spin_until(group_ended, gb, "Bob's leave committed by Alice");
  g_assert_cmpint(gh_mls_group_get_end(gb), ==, GH_MLS_GROUP_END_LEFT);
  wait_members(ga, 2);
  g_assert_cmpuint(gh_mls_service_test_rate_retries(), ==, retries + 1);
  g_assert_cmpuint(gh_mls_service_test_departure_failures(), ==, failures);
  g_assert_cmpuint(gone->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(gone, 0), ==, hex[BOB]);
  gh_mls_service_test_refuse_rate(0);
  g_signal_handlers_disconnect_by_data(ga, gone);
  world_down(&w);
  mdk_driver_stop(&driver);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mdk-interop/mdk-invites-groundhog", test_mdk_invites_groundhog);
  g_test_add_func("/groundhog/mdk-interop/mdk-group-unproven-member-default",
                  test_mdk_group_unproven_member_default);
  g_test_add_func("/groundhog/mdk-interop/mdk-group-unproven-member-strict",
                  test_mdk_group_unproven_member_strict);
  g_test_add_func("/groundhog/mdk-interop/mdk-adds-unproven-member",
                  test_mdk_adds_unproven_member);
  g_test_add_func("/groundhog/mdk-interop/mdk-adds-unproven-member-strict",
                  test_mdk_adds_unproven_member_strict);
  g_test_add_func("/groundhog/mdk-interop/groundhog-invites-mdk-default",
                  test_groundhog_invites_mdk_default);
  g_test_add_func("/groundhog/mdk-interop/groundhog-invites-mdk-strict",
                  test_groundhog_invites_mdk_strict);
  g_test_add_func("/groundhog/mdk-interop/mdk-member-leaves", test_mdk_member_leaves);
  g_test_add_func("/groundhog/mdk-interop/groundhog-leaves", test_groundhog_leaves);
  g_test_add_func("/groundhog/mdk-interop/groundhog-leaves-mdk-admin",
                  test_groundhog_leaves_mdk_admin);
  g_test_add_func("/groundhog/mdk-interop/groundhog-member-commits-leave",
                  test_groundhog_member_commits_leave);
  gint rc = g_test_run();
  mls_world_finish();
  return rc;
}
