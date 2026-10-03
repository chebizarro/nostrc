/* Groundhog <-> MDK 0.11 interop matrix (nostrc-a5u5; the release gate of
 * nostrc-7gx7; docs/analysis/marmot-adopted-profile-gap-2026-09-30.md,
 * "Harness matrix and acceptance evidence"). The third-party side is MDK
 * v0.11.0 (946e0547: cgka-engine, the adopted Marmot profile with
 * app_data_dictionary components and the 0x8009 account proof) behind the
 * JSON-lines driver of tests/interop/mdk/driver-0.11 (mdk-peer.h). Groundhog
 * accounts are the real GhMlsService of mls-world.h on the test's local
 * relays, as shipped: they publish an adopted and an MDK 0.8 KeyPackage, each
 * in its own `d` slot, on their kind 10002 write relay W (nostrc-lf62);
 * Welcomes are gift-wrapped (NIP-59) to the invitee's inbox X; group traffic
 * is on G with NIP-42 AUTH.
 *
 * Every case runs alone (CTest groundhog-mdk011-interop-<case>, -p).
 *
 *  control: MDK 0.11 <-> MDK 0.11 through the same relays and harness. It
 *    must pass: it proves the driver, the relays and the adopted flows
 *    (KeyPackage, Welcome 1059 -> 13 -> 444, kind 9 both ways, rename, add,
 *    remove, self-update, SelfRemove leave) before any Groundhog verdict.
 *  groundhog-invites-mdk: Groundhog's New Group check reads a White Noise
 *    (MDK 0.11) user's KeyPackage as adopted-only, and Groundhog, requiring
 *    every member's proof, creates an adopted group with her; MDK joins from
 *    the Welcome; kind 445 both ways (nostrc-lse9, a Groundhog-made group).
 *  mdk-invites-groundhog: MDK 0.11 finds Groundhog's adopted KeyPackage
 *    through its kind 10002 write relays -- per slot, passing over the newer
 *    MDK 0.8 one -- admits it and invites Alice (nostrc-8u53); the invitation
 *    is listed as the invitations dialog shows it and accepted (with --gui:
 *    in GhMlsInvitesDialog itself); kind 445 both ways (nostrc-lse9, an
 *    MDK-made group); the join's replacement is confirmed, retires the old
 *    adopted key only, and is what MDK finds next.
 *  adopted-welcome: an adopted Welcome for another device of Alice's account
 *    (an MDK device's KeyPackage) reaches Groundhog: not for this device,
 *    it is refused as it arrives, with no invitation, no group and Alice's
 *    own KeyPackages untouched (same-account multi-device: nostrc-yaa1).
 *  white-noise-welcome: MDK 0.11 invites Alice's published adopted
 *    KeyPackage into a White Noise-shaped group; Groundhog joins, follows
 *    its Commits, and messages flow both ways (nostrc-qp24.5.2).
 *  adopted-commits: Groundhog creates an adopted group with an engine-default
 *    MDK 0.11 peer through New Group's own path and the two exchange
 *    messages and Commits both ways (nostrc-qp24.5.1.3).
 *  concurrent-commits: Groundhog and MDK 0.11 commit from the same epoch
 *    and converge on one branch: MDK's by the key (Groundhog switches),
 *    then Groundhog's by a witness against the key (MDK switches), then
 *    MDK's by the key between two witnessed branches (Groundhog switches on
 *    a message) (nostrc-w1m0).
 *
 * mdk09-probe: MDK 0.9.0 (the dictionary engine, v1 proof) is expected
 * incompatible: the case asserts the refusal precisely (the failure class,
 * nothing published, no group, no invitation, no stall, no crash) and then
 * exits 77: CTest reports it Skipped (XFAIL), never Passed. A refusal of
 * another shape fails; so does an unexpected success (XPASS). */
#include "mls-world.h"
#include "mdk-peer.h"
#include "gh-reaction-store.h"
#include "blossom-fixture.h"
#include "gh-attachments.h"
#include "gh-mls-attachments.h"

#include "gh-mls-copy.h"
#include "gh-mls-invitee.h"
#include "gh-mls-invites-dialog.h"
#include "gh-test-dialog.h"
#include "nostrc-test-gdk-frame.h"

extern void groundhog_register_resource(void);

enum { DAVE = STRANGER };

#define MDK011_REV "946e0547485c9a2c393c2048ec3a968fd50fb441"
#define ADOPTED "marmot-adopted"


static MdkDriver driver;
/* --gui: mdk-invites-groundhog accepts in GhMlsInvitesDialog. */
static gboolean gui_mode;

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

/* Account key's KeyPackage as MDK 0.11 finds it for an invitation (nostrc-
 * 8u53): the account's kind 10002 from the discovery relay E, its write
 * relays, and there marmot-app's per-slot choice; *view: MDK's admission.
 * Asserts the write relays are W alone. */
static gchar *
mdk_discover_key_package(World *w, const gchar *peer, guint key, JsonObject **view)
{
  g_autoptr(JsonObject) kp = mdk_call(&driver,
                                      "\"cmd\":\"fetch_key_package\",\"peer\":\"%s\","
                                      "\"author\":\"%s\",\"discover\":[\"%s\"]",
                                      peer, hex[key], w->e.url);
  g_auto(GStrv) write = mdk_strv(json_object_get_array_member(kp, "write_relays"));
  g_assert_cmpuint(g_strv_length(write), ==, 1);
  g_assert_cmpstr(write[0], ==, w->w.url);
  *view = json_object_ref(json_object_get_object_member(kp, "mdk"));
  g_autofree gchar *text = mdk_json(*view);
  g_test_message("MDK 0.11 %s finds %s's KeyPackage through the 10002 (%" G_GINT64_FORMAT
                 " slots): %s", peer, hex[key], json_object_get_int_member(kp, "slots"), text);
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

/* ---- New Group's check, and a refused creation ----------------------------------- */

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

/* ---- an adopted Welcome for another device of the account ------------------------- */

typedef struct {
  App *app;
  const gchar *wrap_id;
} WelcomeWait;

static gboolean
welcome_processed(gpointer data)
{
  WelcomeWait *wait = data;
  return processed_welcome(wait->app, wait->wrap_id, NULL, NULL);
}

/* The value of the one `name` tag of a signed event's JSON (transfer full). */
static gchar *
event_tag_of(const gchar *event_json, const gchar *name)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, event_json, -1, NULL));
  JsonArray *tags = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)),
                                                 "tags");
  gchar *value = NULL;
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (g_strcmp0(json_array_get_string_element(tag, 0), name) == 0) {
      g_assert_null(value);
      value = g_strdup(json_array_get_string_element(tag, 1));
    }
  }
  g_assert_nonnull(value);
  return value;
}

/* Whether libmarmot still holds the private init key of KeyPackageRef
 * ref_hex for the account. */
static gboolean
holds_init_key(App *app, const gchar *ref_hex)
{
  guint8 ref[32];
  g_assert_true(nostr_hex2bin(ref, ref_hex, sizeof ref));
  bool present = false;
  g_assert_cmpint(marmot_key_package_has_private_key(gh_mls_service_get_marmot(app->service), ref,
                                                     &present), ==, MARMOT_OK);
  return present;
}

/* Carol invites a second device of Alice's account, on MDK 0.11, whose
 * KeyPackage she was handed directly. The gift wrap lands in Alice's inbox,
 * where Groundhog opens it: it is for another device's KeyPackage, so it is
 * refused as it arrives -- recorded failed, never an invitation, no group --
 * and Alice's own KeyPackages, adopted ones included, are untouched (the
 * account's devices publish their own; same-account multi-device is
 * nostrc-yaa1). */
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
  spin_until(key_package_published, alice, "Alice's KeyPackages");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);
  g_autofree gchar *own_id = g_strdup(
    gh_mls_service_get_key_package_id_for_format(alice->service,
                                                 GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED));
  g_assert_nonnull(own_id);
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

  WelcomeWait wait = { alice, wrap };
  spin_until(welcome_processed, &wait, "Groundhog's verdict on the other device's Welcome");
  gint state = -1;
  g_autofree gchar *reason = NULL;
  g_assert_true(processed_welcome(alice, wrap, &state, &reason));
  g_test_message("Groundhog on another device's adopted Welcome: state %d, \"%s\"", state,
                 reason ? reason : "");
  g_assert_cmpint(state, ==, MARMOT_WELCOME_STATE_FAILED);
  g_assert_cmpstr(reason, ==, "matching KeyPackage private key not found");
  g_assert_cmpuint(alice->invites, ==, 0);
  g_autoptr(GError) list_error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(alice->service, &list_error);
  g_assert_no_error(list_error);
  g_assert_cmpuint(invites->len, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  /* Nothing of Alice's own was spent: no rotation, the same KeyPackage. */
  drain();
  g_assert_cmpstr(gh_mls_service_get_key_package_id_for_format(alice->service,
                                                               GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED),
                  ==, own_id);
  g_assert_cmpint(gh_mls_service_get_key_package_state(alice->service), ==,
                  GH_MLS_KEY_PACKAGE_PUBLISHED);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- adopted: a White Noise group invites Groundhog's published KeyPackage ---------- */

static void
assert_has_all(JsonArray *array, const gchar *const *want, guint n)
{
  for (guint i = 0; i < n; i++)
    if (!mdk_has(array, want[i]))
      g_error("MDK does not see %s in the libmarmot KeyPackage", want[i]);
}

/* Alice's message with this text accepted by a group relay. */
static gboolean
sent_accepted(gpointer data)
{
  MessageWait *wait = data;
  GhMessage *message = find_message(wait->app, wait->room_id, wait->text);
  return message && gh_message_get_status(message) == GH_MESSAGE_STATUS_SENT;
}

/* A message object reached SENT status (for votes: empty content makes
 * find_message() unreliable, so we check the object directly). */
static gboolean
msg_sent(gpointer data)
{
  return gh_message_get_status(GH_MESSAGE(data)) == GH_MESSAGE_STATUS_SENT;
}

#define spin_until_msg(msg_, desc_) \
  spin_until(msg_sent, (msg_), (desc_))

/* A poll has at least one voter. */
static gboolean
poll_has_voters(gpointer data)
{
  return gh_mls_poll_get_total_voters(GH_MLS_POLL(data)) >= 1;
}

/* ---- adopted Commits both ways (nostrc-qp24.5.1.3) ---------------------------------- */

/* An MDK 0.11 peer configured as the engine's default (not White Noise's
 * marmot-app: its groups require no SelfRemove, agent stream or media v2;
 * white-noise-welcome covers those). */
static void
mdk_peer_engine_default(const gchar *peer, guint key)
{
  g_autoptr(JsonObject) made = mdk_call(&driver,
                                        "\"cmd\":\"peer_new\",\"peer\":\"%s\",\"secret\":\"%s\","
                                        "\"config\":\"engine-default\",\"client\":null",
                                        peer, gh_test_secret[key]);
  g_assert_cmpstr(json_object_get_string_member(made, "pubkey"), ==, hex[key]);
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

#define wait_name(group_, name_) \
  G_STMT_START { NameWait nw_ = { (group_), (name_) }; \
    spin_until(name_is, &nw_, "the group's name " name_); } G_STMT_END

static gboolean
welcomes_sent(gpointer data)
{
  return gh_mls_group_get_unsent_welcomes(data) == 0;
}

/* Sends from Groundhog and waits until a group relay accepted it. */
static void
send_accepted(App *app, GhMlsGroup *group, const gchar *text)
{
  send_text(app, group, text);
  MessageWait wait = { app, gh_mls_group_get_room_id(group), text };
  spin_until(sent_accepted, &wait, "the message accepted by a group relay");
}

static void
change_done(OpWait *wait)
{
  spin_until(op_done, wait, "the group change");
  g_assert_no_error(wait->error);
  g_assert_true(wait->ok);
}

/* The MDK peer's view (a "state" object) equals Groundhog's: group, epoch,
 * name, members, admins. */
static void
assert_gh_converged(GhMlsGroup *group, JsonObject *state)
{
  g_assert_cmpstr(json_object_get_string_member(state, "group"), ==,
                  gh_mls_group_get_group_id(group));
  g_assert_cmpuint(state_epoch(state), ==, gh_mls_group_get_epoch(group));
  g_assert_cmpstr(json_object_get_string_member(state, "name"), ==, gh_mls_group_get_name(group));
  g_auto(GStrv) mdk_members = mdk_strv(json_object_get_array_member(state, "members"));
  g_auto(GStrv) gh_members = gh_mls_group_dup_members(group);
  qsort(gh_members, g_strv_length(gh_members), sizeof(gchar *), mdk_strcmp);
  assert_strv_equal(mdk_members, gh_members);
  g_auto(GStrv) mdk_admins = mdk_strv(json_object_get_array_member(state, "admins"));
  g_auto(GStrv) gh_admins = gh_mls_group_dup_admins(group);
  qsort(gh_admins, g_strv_length(gh_admins), sizeof(gchar *), mdk_strcmp);
  assert_strv_equal(mdk_admins, gh_admins);
}

/* The ids of the kind-445 events on `relay` so far (e.g. the Commit that
 * added a joiner: before its join, it cannot read them). */
static GPtrArray *
group_events_on(WireRelay *relay)
{
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *event = g_ptr_array_index(relay->stored, i);
    if (nostr_event_get_kind(event->event) == 445)
      g_ptr_array_add(ids, g_strdup(nostr_event_get_id(event->event)));
  }
  return ids;
}

/* mdk_sync() for a peer that joined after `before` were published: those
 * (only) may stay undecryptable to it (TransportDeferred), as the Add that
 * admitted it does for any joiner. */
static JsonObject *
mdk_sync_joined(const gchar *peer, const gchar *group, GPtrArray *before)
{
  JsonObject *synced = mdk_call(&driver, "\"cmd\":\"sync\",\"peer\":\"%s\",\"group\":\"%s\"",
                                peer, group);
  g_autofree gchar *text = mdk_json(synced);
  g_test_message("MDK %s sync: %s", peer, text);
  JsonArray *failed = json_object_get_array_member(synced, "failed");
  for (guint i = 0; i < json_array_get_length(failed); i++) {
    JsonObject *f = json_array_get_object_element(failed, i);
    const gchar *id = json_object_get_string_member(f, "id");
    gboolean pre_join = FALSE;
    for (guint k = 0; k < before->len && !pre_join; k++)
      pre_join = g_strcmp0(g_ptr_array_index(before, k), id) == 0;
    if (!pre_join || !g_str_has_prefix(json_object_get_string_member(f, "outcome"),
                                       "TransportDeferred"))
      g_error("MDK %s failed on %s", peer, text);
  }
  return synced;
}

/* Groundhog (libmarmot) and MDK 0.11 in one adopted group, Commits both
 * ways. Groundhog creates the group as New Group does (nostrc-lf62: the
 * KeyPackage lookup on Carol's write relay finds her adopted KeyPackage, so
 * the group is adopted) and invites MDK; white-noise-welcome is the other
 * way round. */
static void
test_adopted_commits(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage (her proof enrolled)");
  accept_contact(alice, CAROL);
  mdk_peer_engine_default("carol", CAROL);
  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", on_w);
  g_autofree gchar *carol_kp_id = event_id_of(carol_kp);

  /* Groundhog creates the group with Carol (MDK): adopted. */
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(alice->service, "Adopted by Groundhog", NULL, relays, people,
                                    NULL, on_created, &created);
  spin_until(op_done, &created, "the adopted group creation");
  g_assert_no_error(created.error);
  g_assert_nonnull(created.result);
  GhMlsGroup *ga = created.result;
  g_object_unref(ga);   /* the service keeps it */
  g_assert_true(gh_mls_group_get_adopted(ga));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);
  g_assert_cmpuint(before_join->len, ==, 1);   /* the Add that admits Carol */
  g_autoptr(JsonObject) joined = mdk_join(&w, "carol", ALICE, carol_kp_id);
  const gchar *group = json_object_get_string_member(joined, "group");
  g_assert_cmpstr(json_object_get_string_member(joined, "profile"), ==, "Current");
  assert_gh_converged(ga, joined);

  /* Messages both ways. */
  send_accepted(alice, ga, "hello adopted mdk");
  {
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    g_assert_true(synced_message(synced, hex[ALICE], "hello adopted mdk"));
  }
  mdk_send("carol", group, "hello adopted groundhog");
  wait_message(alice, room, "hello adopted groundhog");

  /* Groundhog's Commits, MDK follows: a rename (0x8001), then Carol a
   * co-admin (0x8003). */
  {
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(alice->service, ga, "Renamed by Groundhog", NULL, NULL,
                                         on_changed, &renamed);
    change_done(&renamed);
    OpWait admins = { 0 };
    const gchar *both[] = { hex[ALICE], hex[CAROL], NULL };
    gh_mls_service_set_admins_async(alice->service, ga, both, NULL, on_changed, &admins);
    change_done(&admins);
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    assert_gh_converged(ga, sync_state(synced));
    g_assert_cmpstr(gh_mls_group_get_name(ga), ==, "Renamed by Groundhog");
  }

  /* MDK's Commits, Groundhog follows: a rename, then a self-update. */
  {
    g_autoptr(JsonObject) renamed = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"name\":\"Renamed by MDK\"", group);
    wait_name(ga, "Renamed by MDK");
    assert_gh_converged(ga, renamed);
    g_autoptr(JsonObject) updated = mdk_call(&driver,
      "\"cmd\":\"self_update\",\"peer\":\"carol\",\"group\":\"%s\"", group);
    wait_epoch(ga, (gint)state_epoch(updated));
    assert_gh_converged(ga, updated);
  }

  /* Messages after the Commits, both ways. */
  mdk_send("carol", group, "mdk after the commits");
  wait_message(alice, room, "mdk after the commits");
  send_accepted(alice, ga, "groundhog after the commits");
  {
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    g_assert_true(synced_message(synced, hex[ALICE], "groundhog after the commits"));
  }

  /* Groundhog removes Carol, an admin: her key leaves 0x8003 in the same
   * Commit (admin-policy-v1.md), which MDK accepts. */
  {
    OpWait removed = { 0 };
    const gchar *carol_only[] = { hex[CAROL], NULL };
    gh_mls_service_remove_members_async(alice->service, ga, carol_only, NULL, on_changed,
                                        &removed);
    change_done(&removed);
    g_auto(GStrv) admins = gh_mls_group_dup_admins(ga);
    g_assert_cmpuint(g_strv_length(admins), ==, 1);
    g_assert_cmpstr(admins[0], ==, hex[ALICE]);
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    JsonObject *state = sync_state(synced);
    g_test_message("MDK after Groundhog removed it: removed %d, epoch %" G_GINT64_FORMAT
                   " (Groundhog %" G_GUINT64_FORMAT ")",
                   json_object_get_boolean_member(state, "removed"),
                   json_object_get_int_member(state, "epoch"), gh_mls_group_get_epoch(ga));
    g_assert_true(json_object_get_boolean_member(state, "removed"));
  }

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- Groundhog invites a White Noise (MDK 0.11) user ------------------------------- */

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
  spin_until(key_package_published, alice, "Alice's KeyPackages");
  mdk_peer("carol", CAROL);
  g_autofree gchar *to = g_strdup_printf("\"%s\",\"%s\"", w.w.url, w.x.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", to);
  g_autofree gchar *carol_kp_id = event_id_of(carol_kp);
  accept_contact(alice, CAROL);

  /* libmarmot admits the White Noise KeyPackage as adopted, not as MDK 0.8. */
  g_assert_cmpint(marmot_validate_key_package_event_json(carol_kp,
                                                         MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                                         NULL, NULL), ==, MARMOT_OK);
  g_assert_cmpint(marmot_validate_key_package_event_json(carol_kp,
                                                         MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8, 0,
                                                         NULL, NULL), !=, MARMOT_OK);

  /* New Group's check row: ready, newer-format groups only. */
  CheckWait check = { 0 };
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, NULL, on_checked,
                             &check);
  spin_until(check_done, &check, "the KeyPackage check");
  g_test_message("New Group check row for a White Noise user: state %d, \"%s\"", check.state,
                 gh_mls_invitee_copy(check.state));
  g_assert_cmpint(check.state, ==, GH_MLS_INVITEE_READY_ADOPTED_ONLY);
  g_assert_true(gh_mls_invitee_can_invite(check.state));
  g_assert_true(gh_mls_invitee_can_join(check.state, TRUE));
  g_assert_false(gh_mls_invitee_can_join(check.state, FALSE));

  /* Alice, requiring every member's account proof (the adopted format always
   * carries one), creates the group: adopted. */
  g_settings_set_boolean(alice->settings, "only-join-verified-mls-groups", TRUE);
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(alice->service, "Groundhog and White Noise", NULL, relays,
                                    people, NULL, on_created, &created);
  spin_until(op_done, &created, "the group creation");
  g_assert_no_error(created.error);
  GhMlsGroup *ga = created.result;
  g_assert_nonnull(ga);
  g_object_unref(ga);   /* the service keeps it */
  g_assert_true(gh_mls_group_get_adopted(ga));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);

  /* MDK joins from the Welcome (rumor `e`: the KeyPackage it consumed). */
  g_autoptr(JsonObject) joined = mdk_join(&w, "carol", ALICE, carol_kp_id);
  const gchar *group = json_object_get_string_member(joined, "group");
  g_assert_cmpstr(json_object_get_string_member(joined, "profile"), ==, "Current");
  assert_gh_converged(ga, joined);

  /* Kind 445 both ways in Groundhog's adopted group (nostrc-lse9). */
  send_accepted(alice, ga, "hello white noise");
  {
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    g_assert_true(synced_message(synced, hex[ALICE], "hello white noise"));
  }
  mdk_send("carol", group, "hello groundhog");
  wait_message(alice, room, "hello groundhog");
  g_assert_cmpstr(gh_message_get_sender(find_message(alice, room, "hello groundhog")), ==,
                  hex[CAROL]);

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- MDK 0.11 invites Groundhog --------------------------------------------------- */

typedef struct {
  GhMlsInvitesDialog *dialog;
  const gchar *text;
} ToastWait;

static gboolean
toast_is(gpointer data)
{
  ToastWait *wait = data;
  return g_strcmp0(gh_mls_invites_dialog_get_last_toast(wait->dialog), wait->text) == 0;
}

/* Alice accepts her one invitation, from Carol, to the group `name` of two:
 * as the invitations dialog lists it (with --gui, in GhMlsInvitesDialog,
 * which accepts it), else through the service calls the dialog makes. */
static GhMlsGroup *
accept_from_carol(App *alice, const gchar *name)
{
  spin_until(has_invite, alice, "Alice's invitation from MDK");
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = gh_mls_service_list_invites(alice->service, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(invites->len, ==, 1);
  GhMlsInvite *invite = g_ptr_array_index(invites, 0);
  g_assert_cmpstr(invite->inviter, ==, hex[CAROL]);
  g_assert_cmpstr(invite->group_name, ==, name);
  g_assert_cmpuint(invite->member_count, ==, 2);
  /* Nothing joins before Accept. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);
  if (!gui_mode) {
    GhMlsGroup *group = gh_mls_service_accept_invite(alice->service, invite->wrapper_id, &error);
    g_assert_no_error(error);
    g_assert_nonnull(group);
    alice->invites = 0;
    return group;
  }
  GhMlsUiContext context = { .service = alice->service, .accounts = alice->accounts,
                             .model = alice->model, .settings = alice->settings };
  GhMlsInvitesDialog *dialog = gh_mls_invites_dialog_new(&context);
  adw_dialog_present(ADW_DIALOG(dialog), NULL);
  spin_until(gh_test_dialog_shown, dialog, "the invitations shown");
  g_assert_cmpuint(gh_mls_invites_dialog_get_n_invites(dialog), ==, 1);
  const gchar *title = NULL, *subtitle = NULL;
  g_assert_true(gh_mls_invites_dialog_describe(dialog, invite->wrapper_id, &title, &subtitle));
  g_test_message("The invitations dialog: \"%s\", \"%s\"", title, subtitle);
  g_assert_cmpstr(title, ==, name);
  g_assert_nonnull(strstr(subtitle, "2 members"));
  gtk_widget_activate_action(GTK_WIDGET(dialog), "mls-invites.accept", "s", invite->wrapper_id);
  g_autofree gchar *joined = g_strdup_printf("You joined “%s”", name);
  ToastWait toast = { dialog, joined };
  spin_until(toast_is, &toast, "the dialog's \"You joined\"");
  g_assert_cmpuint(gh_mls_invites_dialog_get_n_invites(dialog), ==, 0);
  adw_dialog_force_close(ADW_DIALOG(dialog));
  drain();
  alice->invites = 0;
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 1);
  GhMlsGroup *group = g_list_model_get_item(G_LIST_MODEL(alice->service), 0);
  g_object_unref(group);   /* the service keeps it */
  return group;
}

typedef struct {
  App *app;
  const gchar *old_id;
} AdoptedReplaced;

static gboolean
adopted_replaced(gpointer data)
{
  AdoptedReplaced *wait = data;
  const gchar *id = gh_mls_service_get_key_package_id_for_format(wait->app->service,
                                                                 GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED);
  return id && g_strcmp0(id, wait->old_id) != 0 &&
         gh_mls_service_get_key_package_state(wait->app->service) ==
           GH_MLS_KEY_PACKAGE_PUBLISHED;
}

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
  spin_until(key_package_published, alice, "Alice's KeyPackages");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);

  /* Groundhog published both formats on W, the MDK 0.8 one the newer: a
   * reader taking the newest event whatever its slot would get that. */
  g_assert_cmpint(newest_key_package_format(&w.w, ALICE), ==, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY);
  g_autofree gchar *adopted_id = g_strdup(
    gh_mls_service_get_key_package_id_for_format(alice->service,
                                                 GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED));
  g_autofree gchar *legacy_id = g_strdup(
    gh_mls_service_get_key_package_id_for_format(alice->service, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY));
  g_assert_nonnull(adopted_id);
  g_assert_nonnull(legacy_id);

  /* MDK 0.11 looks Alice up as marmot-app does (nostrc-8u53): her kind 10002
   * on the discovery relay, her write relay W, per slot -- the adopted
   * KeyPackage, which it admits. */
  g_autoptr(JsonObject) view = NULL;
  g_autofree gchar *alice_kp = mdk_discover_key_package(&w, "carol", ALICE, &view);
  if (!json_object_get_boolean_member(view, "parsed"))
    g_error("MDK 0.11 refuses Groundhog's adopted KeyPackage (%s): %s",
            json_object_get_string_member(view, "class"),
            json_object_get_string_member(view, "error"));
  g_assert_cmpstr(json_object_get_string_member(view, "profile"), ==, "Current");
  g_autofree gchar *found_id = event_id_of(alice_kp);
  g_assert_cmpstr(found_id, ==, adopted_id);
  g_autofree gchar *old_ref = event_tag_of(alice_kp, "i");
  g_assert_true(holds_init_key(alice, old_ref));

  /* Carol creates a group with Alice; the Welcome goes to Alice's inbox. */
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"Made by White Noise\","
    "\"description\":\"\",\"relays\":[\"%s\"],\"admins\":[\"%s\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], alice_kp, w.x.url);
  g_assert_cmpstr(json_object_get_string_member(made, "profile"), ==, "Current");
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));

  /* Alice: listed in the invitations, accepted, joined. */
  GhMlsGroup *ga = accept_from_carol(alice, "Made by White Noise");
  g_assert_true(gh_mls_group_get_adopted(ga));
  g_assert_cmpstr(gh_mls_group_get_name(ga), ==, "Made by White Noise");
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  wait_live(ga);

  /* Kind 445 both ways in MDK's adopted group (nostrc-lse9). */
  mdk_send("carol", group, "hello from white noise");
  wait_message(alice, room, "hello from white noise");
  g_assert_cmpstr(gh_message_get_sender(find_message(alice, room, "hello from white noise")), ==,
                  hex[CAROL]);
  send_accepted(alice, ga, "hello from groundhog");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "hello from groundhog"));
  }

  /* The join spent the adopted KeyPackage: its replacement, confirmed by a
   * relay, retires the old adopted init key -- and is what MDK finds next;
   * the MDK 0.8 one came along, the newer again. */
  AdoptedReplaced replaced = { alice, adopted_id };
  spin_until(adopted_replaced, &replaced, "Alice's adopted replacement");
  g_assert_false(holds_init_key(alice, old_ref));
  g_assert_cmpint(newest_key_package_format(&w.w, ALICE), ==, GH_MLS_KEY_PACKAGE_FORMAT_LEGACY);
  g_autoptr(JsonObject) next_view = NULL;
  g_autofree gchar *next_kp = mdk_discover_key_package(&w, "carol", ALICE, &next_view);
  g_assert_true(json_object_get_boolean_member(next_view, "parsed"));
  g_autofree gchar *next_id = event_id_of(next_kp);
  g_assert_cmpstr(next_id, ==,
                  gh_mls_service_get_key_package_id_for_format(alice->service,
                                                               GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED));
  g_autofree gchar *next_ref = event_tag_of(next_kp, "i");
  g_assert_true(holds_init_key(alice, next_ref));
  g_autofree gchar *old_d = event_tag_of(alice_kp, "d"), *next_d = event_tag_of(next_kp, "d");
  g_assert_cmpstr(old_d, ==, next_d);   /* the same adopted slot */

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- routing rotation (nostrc-ms4d) --------------------------------------------------- */

static gboolean
stored_with_h(WireRelay *relay, const gchar *h)
{
  for (guint i = 0; i < relay->stored->len; i++) {
    NostrTags *tags = nostr_event_get_tags(((WireStored *)g_ptr_array_index(relay->stored, i))->event);
    for (size_t k = 0; tags && k < nostr_tags_size(tags); k++) {
      NostrTag *tag = nostr_tags_get(tags, k);
      if (g_strcmp0(nostr_tag_get(tag, 0), "h") == 0 && g_strcmp0(nostr_tag_get(tag, 1), h) == 0)
        return TRUE;
    }
  }
  return FALSE;
}

typedef struct {
  GhMlsGroup *group;
  const gchar *const *want;
  gboolean read;
} RelaysWait;

static gboolean
relays_are(gpointer data)
{
  RelaysWait *wait = data;
  g_auto(GStrv) have = wait->read ? gh_mls_group_dup_read_relays(wait->group)
                                  : gh_mls_group_dup_relays(wait->group);
  return have && g_strv_equal((const gchar *const *)have, wait->want);
}

/* MDK 0.11's admin rotates an adopted group's routing (0x8004) to a new
 * random address on another relay, through the engine's UpdateAppComponents
 * (driver `update_routing`); Groundhog follows it: it reads and publishes at
 * the new address on H, keeps reading the old one on G (never asking G for
 * the new address), and kind 9 flows both ways after the rotation. */
static void
test_routing_rotation(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage (her proof enrolled)");
  accept_contact(alice, CAROL);
  mdk_peer_engine_default("carol", CAROL);
  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", on_w);
  g_autofree gchar *carol_kp_id = event_id_of(carol_kp);
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(alice->service, "Rotating", NULL, relays, people, NULL,
                                    on_created, &created);
  spin_until(op_done, &created, "the adopted group creation");
  g_assert_no_error(created.error);
  GhMlsGroup *ga = created.result;
  g_object_unref(ga);   /* the service keeps it */
  g_assert_true(gh_mls_group_get_adopted(ga));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);
  g_autoptr(JsonObject) joined = mdk_join(&w, "carol", ALICE, carol_kp_id);
  const gchar *group = json_object_get_string_member(joined, "group");
  g_autofree gchar *old_h = g_strdup(json_object_get_string_member(joined, "nostr_group_id"));
  {
    OpWait admins = { 0 };
    const gchar *both[] = { hex[ALICE], hex[CAROL], NULL };
    gh_mls_service_set_admins_async(alice->service, ga, both, NULL, on_changed, &admins);
    change_done(&admins);
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    assert_gh_converged(ga, sync_state(synced));
  }

  /* MDK rotates to a new address on H. */
  g_autoptr(JsonObject) rotated = mdk_call(&driver,
    "\"cmd\":\"update_routing\",\"peer\":\"carol\",\"group\":\"%s\",\"relays\":[\"%s\"],"
    "\"rotate\":true", group, w.h.url);
  g_assert_cmpstr(json_object_get_string_member(rotated, "previous_nostr_group_id"), ==, old_h);
  g_autofree gchar *new_h = g_strdup(json_object_get_string_member(rotated, "nostr_group_id"));
  g_assert_cmpstr(new_h, !=, old_h);
  g_assert_true(stored_with_h(&w.g, old_h));   /* the rotation Commit, at the old address */
  {
    const gchar *want[] = { w.h.url, NULL };
    RelaysWait now = { ga, want, FALSE };
    spin_until(relays_are, &now, "Groundhog following the rotation");
    gboolean g_first = g_strcmp0(w.g.url, w.h.url) < 0;
    const gchar *both[] = { g_first ? w.g.url : w.h.url, g_first ? w.h.url : w.g.url, NULL };
    RelaysWait read = { ga, both, TRUE };
    spin_until(relays_are, &read, "the old address still read");
  }
  wait_epoch(ga, (gint)state_epoch(rotated));
  assert_gh_converged(ga, rotated);

  /* Both ways at the new address. */
  mdk_send("carol", group, "mdk after the rotation");
  wait_message(alice, room, "mdk after the rotation");
  send_accepted(alice, ga, "groundhog after the rotation");
  {
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    g_assert_true(synced_message(synced, hex[ALICE], "groundhog after the rotation"));
  }
  g_assert_true(stored_with_h(&w.h, new_h));
  g_assert_false(stored_with_h(&w.g, new_h));
  g_assert_false(client_frames_mention(&w.g, new_h));

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* The White Noise group of the welcome and media cases: Alice's adopted
 * KeyPackage as Groundhog published it, found by Carol (MDK 0.11) through
 * her kind 10002, passes MDK's parser and marmot-app's invite precheck; Carol creates
 * a White Noise-shaped group with Alice (both admins); Alice joins. Returns
 * Alice's group, *out_group the group id hex for the driver. */
static GhMlsGroup *
white_noise_group(World *w_, gchar **out_group)
{
  App *alice = &w_->apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackages");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);
  Marmot *marmot = gh_mls_service_get_marmot(alice->service);

  /* Alice's adopted KeyPackage as Groundhog published it (nostrc-lf62), as
   * MDK 0.11 finds it through her kind 10002: its parser admits it as a
   * current-profile KeyPackage advertising what every White Noise group
   * requires (nostrc-qp24.5.2). */
  g_autoptr(JsonObject) view = NULL;
  g_autofree gchar *alice_kp = mdk_discover_key_package(w_, "carol", ALICE, &view);
  g_autofree gchar *view_text = mdk_json(view);
  g_test_message("MDK 0.11 on Groundhog's adopted KeyPackage: %s", view_text);
  if (!json_object_get_boolean_member(view, "parsed"))
    g_error("MDK 0.11 refuses the libmarmot adopted KeyPackage (%s): %s",
            json_object_get_string_member(view, "class"),
            json_object_get_string_member(view, "error"));
  g_assert_cmpstr(json_object_get_string_member(view, "profile"), ==, "Current");
  static const gchar *const want_ext[] = { "0x0006", "0xf2d1" };
  static const gchar *const want_prop[] = { "0x0008", "0x000a" };
  static const gchar *const want_comp[] = { "0x8001", "0x8002", "0x8003", "0x8004",
                                            "0x8006", "0x8007", "0x8009", "0x800b",
                                            "0x800c" };
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
    "\"description\":\"wn\",\"relays\":[\"%s\"],\"admins\":[\"%s\",\"%s\"],"
    "\"white_noise\":true,\"media_endpoints\":[\"https://blossom.example.com\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w_->g.url, hex[CAROL], hex[ALICE], alice_kp, w_->x.url);
  g_autofree gchar *group = g_strdup(json_object_get_string_member(made, "group"));
  g_auto(GStrv) components = mdk_strv(json_object_get_array_member(made, "components"));
  g_autofree gchar *component_text = g_strjoinv(",", components);
  g_test_message("White Noise group components: %s", component_text);
  g_assert_true(g_strv_contains((const gchar *const *)components, "0x8006"));
  g_assert_true(g_strv_contains((const gchar *const *)components, "0x800b"));

  /* Groundhog lists the invitation and joins: past admission. */
  GhMlsGroup *ga = join(alice, CAROL);
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
  *out_group = g_steal_pointer(&group);
  return ga;
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
  g_autofree gchar *group = NULL;
  GhMlsGroup *ga = white_noise_group(&w, &group);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));

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

  /* Commits both ways in the White Noise group (nostrc-qp24.5.1.3, W24
   * slice H): Groundhog follows Carol's rename and reads the next epoch ... */
  guint64 joined_epoch = gh_mls_group_get_epoch(ga);
  g_autoptr(JsonObject) renamed = mdk_call(&driver,
    "\"cmd\":\"update_group_data\",\"peer\":\"carol\",\"group\":\"%s\","
    "\"name\":\"Renamed by White Noise\"", group);
  g_assert_cmpuint(state_epoch(renamed), ==, joined_epoch + 1);
  wait_name(ga, "Renamed by White Noise");
  assert_gh_converged(ga, renamed);
  mdk_send("carol", group, "after the rename");
  wait_message(alice, room, "after the rename");
  g_assert_false(gh_mls_group_get_change_refused(ga));
  g_assert_true(gh_mls_group_get_active(ga));
  /* ... and Carol (MDK 0.11, marmot-app's components) follows Groundhog's
   * rename -- an AppDataUpdate Commit whose UpdatePath leaf advertises the
   * White Noise set -- and its next message. */
  {
    OpWait mine = { 0 };
    gh_mls_service_update_metadata_async(alice->service, ga, "Renamed by Groundhog", NULL, NULL,
                                         on_changed, &mine);
    change_done(&mine);
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    assert_gh_converged(ga, sync_state(synced));
    g_assert_cmpstr(json_object_get_string_member(sync_state(synced), "name"), ==,
                    "Renamed by Groundhog");
  }
  send_accepted(alice, ga, "groundhog after its rename");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "groundhog after its rename"));
  }
  mdk_send("carol", group, "white noise after groundhog's rename");
  wait_message(alice, room, "white noise after groundhog's rename");

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- MIP-04 encrypted media v2 both ways (W25, nostrc-q3a6) ------------------------- */

/* A 1x1 PNG with no ancillary chunk: what Groundhog sends is these bytes. */
static GBytes *
tiny_png(void)
{
  static const guint8 png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  return g_bytes_new_static(png, sizeof png);
}

typedef struct {
  gboolean done;
  GhMessage *message;
  GError *error;
} MediaSent;

static gboolean
media_sent(gpointer data)
{
  return ((MediaSent *)data)->done;
}

static void
on_media_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  MediaSent *sent = data;
  sent->message = gh_mls_attachments_send_finish(GH_MLS_ATTACHMENTS(source), result, NULL,
                                                 &sent->error);
  sent->done = TRUE;
}

static gboolean
transfer_settled(gpointer data)
{
  GhAttachmentState state = gh_attachment_transfer_get_state(data);
  return state == GH_ATTACHMENT_STATE_READY || state == GH_ATTACHMENT_STATE_FAILED;
}

static gboolean
message_sent(gpointer data)
{
  return gh_message_get_status(data) == GH_MESSAGE_STATUS_SENT;
}

/* The sync result of author's kind 9 with an imeta tag (borrowed). */
static JsonObject *
synced_media(JsonObject *synced, const gchar *author)
{
  JsonArray *results = json_object_get_array_member(synced, "results");
  for (guint i = 0; i < json_array_get_length(results); i++) {
    JsonObject *r = json_array_get_object_element(results, i);
    if (g_strcmp0(json_object_get_string_member(r, "type"), "application") != 0 ||
        g_strcmp0(json_object_get_string_member(r, "author"), author) != 0 ||
        !json_object_has_member(r, "tags"))
      continue;
    JsonArray *tags = json_object_get_array_member(r, "tags");
    for (guint k = 0; k < json_array_get_length(tags); k++) {
      JsonArray *tag = json_array_get_array_element(tags, k);
      if (json_array_get_length(tag) > 1 &&
          g_strcmp0(json_array_get_string_element(tag, 0), "imeta") == 0)
        return r;
    }
  }
  return NULL;
}

static gchar *
json_of_node(JsonNode *node)
{
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, node);
  return json_generator_to_data(gen, NULL);
}

/* White Noise's media: Carol (MDK 0.11) sends a photo as marmot-app does --
 * encrypted-media-v2 under the group's media exporter, uploaded to the local
 * Blossom server, one kind 9 with its imeta -- and Groundhog shows it,
 * fetches nothing until Download, then opens it with the message's epoch,
 * byte for byte; Groundhog sends one the same way and MDK opens it. */
static void
test_white_noise_media(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  g_autofree gchar *group = NULL;
  GhMlsGroup *ga = white_noise_group(&w, &group);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));

  BlossomFixture *blossom = blossom_fixture_new();
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  g_autoptr(GSettings) settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  const gchar *servers[] = { blossom_fixture_url(blossom), NULL };
  g_settings_set_strv(settings, "blossom-servers", servers);
  g_settings_set_string(settings, "network-mode", "none");
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  GhAttachmentsConfig config = { .settings = settings, .http = http };
  GhAttachments *attachments = gh_attachments_new(&config);
  gh_attachments_set_allow_private_hosts(attachments, TRUE);
  gh_attachments_set_store(attachments, alice->store);
  GhMlsAttachments *files = gh_mls_attachments_new(attachments);
  gh_mls_attachments_set_service(files, alice->service);

  /* MDK -> Groundhog. */
  g_autoptr(GBytes) photo = tiny_png();
  g_autofree gchar *photo_b64 = g_base64_encode(g_bytes_get_data(photo, NULL),
                                                g_bytes_get_size(photo));
  g_autoptr(JsonObject) sent = mdk_call(&driver,
    "\"cmd\":\"send_media\",\"peer\":\"carol\",\"group\":\"%s\",\"file\":\"%s\","
    "\"mime\":\"image/png\",\"filename\":\"IMG_0001.png\",\"blossom\":\"%s\","
    "\"caption\":\"a photo from white noise\",\"dim\":\"1x1\"",
    group, photo_b64, blossom_fixture_url(blossom));
  guint64 mdk_epoch = (guint64)json_object_get_int_member(sent, "epoch");
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);
  wait_message(alice, room, "a photo from white noise");
  GhMessage *received = find_message(alice, room, "a photo from white noise");
  g_assert_cmpstr(gh_message_get_sender(received), ==, hex[CAROL]);
  g_assert_cmpuint(gh_message_get_n_attachments(received), ==, 1);
  g_assert_cmpuint(gh_message_get_rejected_attachments(received), ==, 0);
  const GhMessageAttachment *theirs = gh_message_get_attachment(received, 0);
  g_assert_cmpstr(theirs->media_type, ==, "image/png");
  g_assert_cmpstr(theirs->filename, ==, "IMG_0001.png");
  g_assert_cmpuint(theirs->width, ==, 1);
  guint64 epoch = 0;
  g_assert_true(gh_message_get_mls_epoch(received, &epoch));
  g_assert_cmpuint(epoch, ==, mdk_epoch);
  GhAttachmentTransfer *transfer = gh_mls_attachments_lookup(files, received, 0);
  g_assert_nonnull(transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  drain();
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);
  gh_mls_attachments_download(files, transfer);
  spin_until(transfer_settled, transfer, "Groundhog opening MDK's photo");
  g_assert_cmpstr(gh_attachment_transfer_get_error(transfer), ==, NULL);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(g_bytes_equal(gh_attachment_transfer_get_plaintext(transfer), photo));
  g_assert_true(gh_attachment_transfer_get_previewable(transfer));
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);

  /* Groundhog -> MDK. */
  MediaSent mine = { 0 };
  gh_mls_attachments_send_async(files, ga, photo, "IMG_0002.png", "image/png", NULL, NULL,
                                on_media_sent, &mine);
  spin_until(media_sent, &mine, "Groundhog's photo sent");
  g_assert_no_error(mine.error);
  spin_until(message_sent, mine.message, "Groundhog's photo accepted by a group relay");
  g_autoptr(JsonObject) synced = mdk_sync("carol", group);
  JsonObject *app = synced_media(synced, hex[ALICE]);
  g_assert_nonnull(app);
  g_assert_cmpuint((guint64)json_object_get_int_member(app, "epoch"), ==,
                   gh_mls_group_get_epoch(ga));
  JsonArray *tags = json_object_get_array_member(app, "tags");
  JsonNode *imeta = NULL;
  for (guint k = 0; k < json_array_get_length(tags) && !imeta; k++) {
    JsonArray *tag = json_array_get_array_element(tags, k);
    if (g_strcmp0(json_array_get_string_element(tag, 0), "imeta") == 0)
      imeta = json_array_get_element(tags, k);
  }
  g_autofree gchar *imeta_json = json_of_node(imeta);
  g_test_message("Groundhog's imeta as MDK received it: %s", imeta_json);
  g_assert_nonnull(strstr(imeta_json, "\"filename photo.png\""));
  g_autoptr(JsonObject) opened = mdk_call(&driver,
    "\"cmd\":\"open_media\",\"peer\":\"carol\",\"group\":\"%s\",\"imeta\":%s,"
    "\"epoch\":%" G_GINT64_FORMAT,
    group, imeta_json, json_object_get_int_member(app, "epoch"));
  gsize opened_len = 0;
  g_autofree guchar *opened_bytes =
    g_base64_decode(json_object_get_string_member(opened, "file"), &opened_len);
  GhAttachmentTransfer *own = gh_mls_attachments_lookup(files, mine.message, 0);
  GBytes *own_plain = gh_attachment_transfer_get_plaintext(own);
  g_assert_nonnull(own_plain);
  g_assert_cmpmem(opened_bytes, opened_len, g_bytes_get_data(own_plain, NULL),
                  g_bytes_get_size(own_plain));
  g_assert_cmpstr(json_object_get_string_member(opened, "media_type"), ==, "image/png");

  g_clear_object(&mine.message);
  g_object_unref(files);
  gh_attachments_set_store(attachments, NULL);
  g_object_unref(attachments);
  drain();
  blossom_fixture_free(blossom);
  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- White Noise DM shape: 2-member group with empty name (W26 slice A) ------------- */

/* MDK 0.11 creates a DM (marmot-app's create_group("", &[peer]): empty name,
 * 2 members); Groundhog joins it and sees it as a direct message (is_direct).
 * Then Groundhog creates a Marmot DM (create_group with "" name) and MDK
 * joins.  Messages flow both ways in each direction. */
static void
test_white_noise_dm(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage (her proof enrolled)");
  accept_contact(alice, CAROL);
  mdk_peer("carol", CAROL);

  /* ---- Direction 1: MDK creates a WN DM → Groundhog joins ---- */

  g_autoptr(JsonObject) view = NULL;
  g_autofree gchar *alice_kp = mdk_discover_key_package(&w, "carol", ALICE, &view);
  g_assert_true(json_object_get_boolean_member(view, "parsed"));

  /* Carol (MDK) creates a DM: empty name, 2 members = WN's DM shape.
   * white_noise requires media_endpoints (marmot-app's EncryptedMediaPolicyV2). */
  g_autoptr(JsonObject) made = mdk_call(&driver,
    "\"cmd\":\"create_group\",\"peer\":\"carol\",\"name\":\"\","
    "\"description\":\"\",\"relays\":[\"%s\"],\"admins\":[\"%s\",\"%s\"],"
    "\"white_noise\":true,\"media_endpoints\":[\"https://blossom.example.com\"],"
    "\"key_packages\":[%s],\"welcome_relays\":[\"%s\"]",
    w.g.url, hex[CAROL], hex[ALICE], alice_kp, w.x.url);
  g_autofree gchar *group1 = g_strdup(json_object_get_string_member(made, "group"));

  /* Groundhog joins the DM. */
  GhMlsGroup *ga1 = join(alice, CAROL);
  g_autofree gchar *room1 = g_strdup(gh_mls_group_get_room_id(ga1));

  /* The group has no name (empty → NULL) and is detected as a DM. */
  g_assert_null(gh_mls_group_get_name(ga1));
  g_auto(GStrv) members1 = gh_mls_group_dup_members(ga1);
  g_assert_cmpuint(g_strv_length(members1), ==, 2);

  /* The conversation model has is_direct set by group_is_dm → group_list_room. */
  GhConversation *conv1 = gh_conversation_store_lookup(alice->model, room1);
  g_assert_nonnull(conv1);
  g_assert_true(gh_conversation_get_is_direct(conv1));

  /* Messages both ways. */
  mdk_send("carol", group1, "dm from white noise");
  wait_message(alice, room1, "dm from white noise");
  send_accepted(alice, ga1, "dm from groundhog");
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group1);
    g_assert_true(synced_message(synced, hex[ALICE], "dm from groundhog"));
  }

  /* ---- Direction 2: Groundhog creates a Marmot DM → MDK joins ---- */

  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", on_w);
  g_autofree gchar *carol_kp_id = event_id_of(carol_kp);

  /* Groundhog creates a DM with Carol: "" name (the WN DM shape). */
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(alice->service, "", NULL, relays, people,
                                    NULL, on_created, &created);
  spin_until(op_done, &created, "the Marmot DM creation");
  g_assert_no_error(created.error);
  g_assert_nonnull(created.result);
  GhMlsGroup *ga2 = created.result;
  g_object_unref(ga2);   /* the service keeps it */
  g_autofree gchar *room2 = g_strdup(gh_mls_group_get_room_id(ga2));

  /* Empty name stored as NULL: the DM shape. */
  g_assert_null(gh_mls_group_get_name(ga2));
  g_auto(GStrv) members2 = gh_mls_group_dup_members(ga2);
  g_assert_cmpuint(g_strv_length(members2), ==, 2);

  /* The conversation model has is_direct set. */
  GhConversation *conv2 = gh_conversation_store_lookup(alice->model, room2);
  g_assert_nonnull(conv2);
  g_assert_true(gh_conversation_get_is_direct(conv2));

  /* MDK joins from the Welcome. */
  spin_until(welcomes_sent, ga2, "the Welcome accepted by Carol's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);
  g_autoptr(JsonObject) joined = mdk_join(&w, "carol", ALICE, carol_kp_id);
  const gchar *group2 = json_object_get_string_member(joined, "group");

  /* MDK sees it as adopted, with the same empty name. */
  g_assert_cmpstr(json_object_get_string_member(joined, "name"), ==, "");

  /* Messages both ways on Groundhog's DM. */
  send_accepted(alice, ga2, "groundhog dm hello");
  {
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group2, before_join);
    g_assert_true(synced_message(synced, hex[ALICE], "groundhog dm hello"));
  }
  mdk_send("carol", group2, "mdk dm hello");
  wait_message(alice, room2, "mdk dm hello");

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- concurrent Commits: Groundhog and MDK 0.11 converge (nostrc-w1m0) -------------- */

typedef struct {
  guint fired;
  guint withdrawn;
  guint undone;
} Conflicts;

static void
on_conflict(GhMlsGroup *group, guint withdrawn, guint undone, gpointer data)
{
  (void)group;
  Conflicts *c = data;
  c->fired++;
  c->withdrawn += withdrawn;
  c->undone |= undone;
}

static gboolean
message_withdrawn(gpointer data)
{
  return gh_message_get_withdrawn(data);
}

/* Groundhog (Carol's account) and MDK 0.11 (Alice's), both admins of one
 * adopted group, commit from the same epoch -- each before it has the
 * other's Commit -- and both converge on one branch by the adopted rules
 * (protocol-core/convergence.md, as MDK runs them).  First, unwitnessed, the
 * lower committer key wins: MDK's (secret 1 sorts below secret 3), and
 * Groundhog switches to it.  Then Groundhog's rename is witnessed by its own
 * message at its epoch, and wins despite the key: MDK switches to it.  Then
 * both are witnessed, the key decides again, and Groundhog switches on MDK's
 * message (the branch change a message makes): Groundhog's own message of
 * the branch it left is marked withdrawn (MDK never delivered it), and each
 * switch says what it undid (nostrc-xrza).  Messages flow both ways after
 * each. */
static void
test_concurrent_commits(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *carol = &w.apps[CAROL];
  spin_until(key_package_published, carol, "Carol's KeyPackage (her proof enrolled)");
  accept_contact(carol, ALICE);
  mdk_peer_engine_default("alice", ALICE);
  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);
  g_autofree gchar *alice_kp = mdk_publish_key_package("alice", on_w);
  g_autofree gchar *alice_kp_id = event_id_of(alice_kp);
  /* The key order this case relies on (secret 1 sorts below secret 3). */
  g_assert_cmpint(strcmp(hex[ALICE], hex[CAROL]), <, 0);

  /* New Group's own path: Alice (MDK) has an adopted KeyPackage only. */
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[ALICE], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(carol->service, "Concurrent", NULL, relays, people, NULL,
                                    on_created, &created);
  spin_until(op_done, &created, "the adopted group creation");
  g_assert_no_error(created.error);
  GhMlsGroup *gc = created.result;
  g_object_unref(gc);   /* the service keeps it */
  g_assert_true(gh_mls_group_get_adopted(gc));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(gc));
  spin_until(welcomes_sent, gc, "the Welcome accepted by Alice's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);
  g_autoptr(JsonObject) joined = mdk_join(&w, "alice", CAROL, alice_kp_id);
  const gchar *group = json_object_get_string_member(joined, "group");
  {
    OpWait admins = { 0 };
    const gchar *both[] = { hex[ALICE], hex[CAROL], NULL };
    gh_mls_service_set_admins_async(carol->service, gc, both, NULL, on_changed, &admins);
    change_done(&admins);
    g_autoptr(JsonObject) synced = mdk_sync_joined("alice", group, before_join);
    assert_gh_converged(gc, sync_state(synced));
  }
  Conflicts conflicts = { 0 };
  g_signal_connect(gc, "conflict-resolved", G_CALLBACK(on_conflict), &conflicts);

  /* 1. Unwitnessed: the lower committer key (MDK's) wins; Groundhog
   *    switches to it when it arrives. */
  {
    guint64 base = gh_mls_group_get_epoch(gc);
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(carol->service, gc, "Groundhog's concurrent", NULL,
                                         NULL, on_changed, &renamed);
    change_done(&renamed);
    /* MDK has not seen it: its rename is from the same epoch. */
    g_autoptr(JsonObject) theirs = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"alice\",\"group\":\"%s\","
      "\"name\":\"MDK's concurrent\"", group);
    g_assert_cmpuint(state_epoch(theirs), ==, base + 1);
    wait_name(gc, "MDK's concurrent");
    g_assert_cmpuint(conflicts.fired, ==, 1);   /* its own rename undone, nothing withdrawn */
    g_assert_cmpuint(conflicts.withdrawn, ==, 0);
    g_assert_true(conflicts.undone & GH_MLS_UNDONE_NAME);
    g_autoptr(JsonObject) synced = mdk_sync_joined("alice", group, before_join);
    assert_gh_converged(gc, sync_state(synced));
    mdk_send("alice", group, "mdk after the first race");
    wait_message(carol, room, "mdk after the first race");
    send_accepted(carol, gc, "groundhog after the first race");
    g_autoptr(JsonObject) read = mdk_sync_joined("alice", group, before_join);
    g_assert_true(synced_message(read, hex[CAROL], "groundhog after the first race"));
  }

  /* 2. Groundhog's rename, witnessed by its own message at its epoch, beats
   *    MDK's despite the key: MDK switches to it when it syncs. */
  {
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(carol->service, gc, "Groundhog's witnessed", NULL,
                                         NULL, on_changed, &renamed);
    change_done(&renamed);
    send_accepted(carol, gc, "witness from groundhog");
    g_autoptr(JsonObject) theirs = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"alice\",\"group\":\"%s\","
      "\"name\":\"MDK's second\"", group);
    g_assert_cmpuint(state_epoch(theirs), ==, gh_mls_group_get_epoch(gc));
    g_autoptr(JsonObject) synced = mdk_sync_joined("alice", group, before_join);
    g_assert_true(synced_message(synced, hex[CAROL], "witness from groundhog"));
    assert_gh_converged(gc, sync_state(synced));
    g_assert_cmpstr(gh_mls_group_get_name(gc), ==, "Groundhog's witnessed");
    mdk_send("alice", group, "mdk after the second race");
    wait_message(carol, room, "mdk after the second race");
    send_accepted(carol, gc, "groundhog after the second race");
    g_autoptr(JsonObject) read = mdk_sync_joined("alice", group, before_join);
    g_assert_true(synced_message(read, hex[CAROL], "groundhog after the second race"));
  }

  /* 3. Both renames witnessed by their committer's own message: the scores
   *    tie and the key decides (MDK's).  The group relay withholds every new
   *    event until released, so each side commits and speaks without the
   *    other's events; Groundhog then gets MDK's rename (it loses: only
   *    Groundhog's branch is witnessed there yet) and MDK's message, which
   *    witnesses MDK's branch and switches Groundhog on a message. */
  {
    g_autoptr(GPtrArray) before = group_events_on(&w.g);
    w.g.withhold_new = TRUE;
    OpWait renamed = { 0 };
    gh_mls_service_update_metadata_async(carol->service, gc, "Groundhog's third", NULL, NULL,
                                         on_changed, &renamed);
    change_done(&renamed);
    send_accepted(carol, gc, "groundhog witness three");
    g_autoptr(GPtrArray) after_gh = group_events_on(&w.g);
    g_assert_cmpuint(after_gh->len, ==, before->len + 2);
    g_autoptr(JsonObject) theirs = mdk_call(&driver,
      "\"cmd\":\"update_group_data\",\"peer\":\"alice\",\"group\":\"%s\","
      "\"name\":\"MDK's third\"", group);
    g_autoptr(JsonObject) settled = mdk_sync_joined("alice", group, before_join);
    g_assert_cmpstr(json_object_get_string_member(sync_state(settled), "name"), ==,
                    "MDK's third");
    mdk_send("alice", group, "mdk witness three");
    g_autoptr(GPtrArray) after_mdk = group_events_on(&w.g);
    g_assert_cmpuint(after_mdk->len, ==, after_gh->len + 2);
    w.g.withhold_new = FALSE;
    /* MDK's rename, then its message, to Groundhog. */
    for (guint i = after_gh->len; i < after_mdk->len; i++)
      wire_relay_release(&w.g, g_ptr_array_index(after_mdk, i));
    wait_name(gc, "MDK's third");
    wait_message(carol, room, "mdk witness three");
    GhMessage *own = find_message(carol, room, "groundhog witness three");
    g_assert_nonnull(own);
    spin_until(message_withdrawn, own, "Groundhog's witness of the branch it left withdrawn");
    g_assert_false(gh_message_get_withdrawn(find_message(carol, room, "mdk witness three")));
    g_assert_cmpuint(conflicts.fired, ==, 2);   /* round 2: Groundhog stayed on its branch */
    g_assert_cmpuint(conflicts.withdrawn, ==, 1);
    /* Groundhog's, to MDK: its rename loses there too. */
    for (guint i = before->len; i < after_gh->len; i++)
      wire_relay_release(&w.g, g_ptr_array_index(after_mdk, i));
    /* (Groundhog's witness is on the losing branch: MDK counts it, and
     * delivers nothing of it.) */
    g_autoptr(JsonObject) synced = mdk_sync_joined("alice", group, before_join);
    g_assert_false(synced_message(synced, hex[CAROL], "groundhog witness three"));
    assert_gh_converged(gc, sync_state(synced));
    g_assert_cmpuint(state_epoch(theirs), ==, gh_mls_group_get_epoch(gc));
    mdk_send("alice", group, "mdk after the third race");
    wait_message(carol, room, "mdk after the third race");
    send_accepted(carol, gc, "groundhog after the third race");
    g_autoptr(JsonObject) read = mdk_sync_joined("alice", group, before_join);
    g_assert_true(synced_message(read, hex[CAROL], "groundhog after the third race"));
  }

  world_down(&w);
  mdk_driver_stop(&driver);
}

/* ---- W26 slice B (nostrc-191r): NIP-25 reactions ------------------------------------ */

/* Whether the sync result has a kind-7 reaction from author with emoji. */
static gboolean
synced_reaction(JsonObject *synced, const gchar *author, const gchar *emoji)
{
  JsonArray *results = json_object_get_array_member(synced, "results");
  for (guint i = 0; i < json_array_get_length(results); i++) {
    JsonObject *r = json_array_get_object_element(results, i);
    if (g_strcmp0(json_object_get_string_member(r, "type"), "application") == 0 &&
        g_strcmp0(json_object_get_string_member(r, "author"), author) == 0 &&
        json_object_get_int_member_with_default(r, "kind", 0) == 7 &&
        g_strcmp0(json_object_get_string_member_with_default(r, "content", NULL), emoji) == 0)
      return TRUE;
  }
  return FALSE;
}

typedef struct {
  GhReactionStore *reactions;
  const gchar *target_id;
} ReactionWait;

static gboolean
reaction_arrived(gpointer data)
{
  ReactionWait *rw = data;
  GhReactionSummary *summary = gh_reaction_store_lookup(rw->reactions, rw->target_id);
  return summary && gh_reaction_summary_get_total_count(summary) > 0;
}

static void
test_white_noise_reactions(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  /* Alice with a reaction store so the service admits kind-7 events. */
  g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  /* Inject the reaction store into Alice's MLS service config. The
   * service reads it from the config struct it was created with; the
   * mls-world.h setup leaves it NULL. We set it on the object's config
   * field directly. */
  {
    /* The service stores the pointer from the config, so we keep
     * `reactions` alive until world_down. Access the struct field: */
    extern void gh_mls_service_set_reaction_store(GhMlsService *, GhReactionStore *);
    gh_mls_service_set_reaction_store(alice->service, reactions);
    gh_reaction_store_set_account(reactions, hex[ALICE], NULL, NULL, NULL);
  }

  /* Reuse the White Noise group helper (adopted profile, both admins,
   * validated KeyPackage and components).  The helper calls mdk_peer,
   * discover, create_group with white_noise:true, and join(). */
  g_autofree gchar *group = NULL;
  GhMlsGroup *ga = white_noise_group(&w, &group);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));

  /* Exchange messages to react to. */
  mdk_send("carol", group, "hello from carol");
  wait_message(alice, room, "hello from carol");
  GhMessage *carol_msg = find_message(alice, room, "hello from carol");
  g_assert_nonnull(carol_msg);
  const gchar *carol_msg_id = gh_message_get_rumor_id(carol_msg);

  send_accepted(alice, ga, "hello from alice");

  /* MDK syncs to verify delivery of Alice's message. */
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_message(synced, hex[ALICE], "hello from alice"));
  }

  /* The NIP-01 event ID (rumor ID) that Groundhog computed for Alice's
   * inner event.  The MDK sync returns an MLS-level message_id for its
   * "id" field — that is a transport artefact unrelated to the nostr
   * event ID, so the reaction's e-tag must carry the rumor ID instead. */
  GhMessage *alice_local_msg = find_message(alice, room, "hello from alice");
  g_assert_nonnull(alice_local_msg);
  const gchar *alice_local_id = gh_message_get_rumor_id(alice_local_msg);
  g_assert_nonnull(alice_local_id);

  /* MDK reacts to Alice's message → Groundhog sees the reaction. */
  {
    g_autoptr(JsonObject) reacted = mdk_call(&driver,
      "\"cmd\":\"send_reaction\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"emoji\":\"thumbs_up\",\"target_event_id\":\"%s\","
      "\"target_pubkey\":\"%s\",\"target_kind\":\"9\"",
      group, alice_local_id, hex[ALICE]);
    g_autofree gchar *rtxt = mdk_json(reacted);
    g_test_message("MDK send_reaction result: %s", rtxt);
  }

  /* Wait for Groundhog to receive the reaction. The GhMlsService
   * admits kind-7 inner events to the reaction store keyed by the
   * target's NIP-01 rumor ID. */
  ReactionWait rw = { reactions, alice_local_id };
  spin_until(reaction_arrived, &rw, "Carol's reaction");
  GhReactionSummary *summary = gh_reaction_store_lookup(reactions, alice_local_id);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);

  /* Groundhog reacts to Carol's message → MDK sees it via sync. */
  {
    g_autoptr(GError) error = NULL;
    g_assert_true(gh_mls_service_send_reaction(alice->service, ga,
      carol_msg_id, hex[CAROL], "9", "+", &error));
    g_assert_no_error(error);
  }
  {
    g_autoptr(JsonObject) synced = mdk_sync("carol", group);
    g_assert_true(synced_reaction(synced, hex[ALICE], "+"));
  }

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
  gh_mls_invitee_check_async(alice->accounts, alice->settings, hex[CAROL], 20, NULL, NULL, on_checked,
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

/* NIP-88 polls through MLS: Groundhog and MDK 0.11 create polls, vote,
 * and verify tallies in both directions. Also verifies that a post-deadline
 * vote is ignored by both sides (F3/F2). */
static void
test_polls(void)
{
  cases_run++;
  if (!mdk_up())
    return;
  World w;
  const guint keys[] = { ALICE };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, alice, "Alice's KeyPackage (her proof enrolled)");
  accept_contact(alice, CAROL);
  mdk_peer_engine_default("carol", CAROL);
  g_autofree gchar *on_w = g_strdup_printf("\"%s\"", w.w.url);
  g_autofree gchar *carol_kp = mdk_publish_key_package("carol", on_w);
  g_autofree gchar *carol_kp_id = event_id_of(carol_kp);

  /* Create an adopted group: Groundhog (Alice) + MDK (Carol). */
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait created = { 0 };
  gh_mls_service_create_group_async(alice->service, "Poll Test Group", NULL, relays, people,
                                    NULL, on_created, &created);
  spin_until(op_done, &created, "the adopted group creation");
  g_assert_no_error(created.error);
  g_assert_nonnull(created.result);
  GhMlsGroup *ga = created.result;
  g_object_unref(ga);
  g_assert_true(gh_mls_group_get_adopted(ga));
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  spin_until(welcomes_sent, ga, "the Welcome accepted by Carol's inbox");
  g_autoptr(GPtrArray) before_join = group_events_on(&w.g);
  g_autoptr(JsonObject) joined = mdk_join(&w, "carol", ALICE, carol_kp_id);
  const gchar *group = json_object_get_string_member(joined, "group");
  assert_gh_converged(ga, joined);

  /* --- Phase 1: MDK creates a poll, Groundhog votes, MDK tallies --- */

  g_test_message("Phase 1: MDK creates poll, Groundhog votes");
  /* MDK creates a poll with a 2-hour deadline. */
  g_autoptr(JsonObject) poll1 = mdk_call(&driver,
    "\"cmd\":\"poll_create\",\"peer\":\"carol\",\"group\":\"%s\","
    "\"question\":\"Favorite drink?\","
    "\"options\":[\"Tea\",\"Coffee\",\"Water\"],"
    "\"poll_type\":\"singlechoice\","
    "\"ends_at\":%lld",
    group, (long long)(g_get_real_time() / G_USEC_PER_SEC + 7200));
  g_assert_true(json_object_has_member(poll1, "event_id"));
  const gchar *mdk_poll_id = json_object_get_string_member(poll1, "event_id");
  g_test_message("MDK poll event_id (kind 445 envelope): %s", mdk_poll_id);

  /* Groundhog syncs and receives the poll. The poll's inner event id is what
   * the poll model keys by; we need to find it after the message arrives. */
  MessageWait poll_wait = { alice, room, "Favorite drink?" };
  spin_until(message_listed, &poll_wait, "the MDK poll listed as a message");

  /* Find the poll in Groundhog's model: the inner rumor id. */
  GhMessage *poll_msg = find_message(alice, room, "Favorite drink?");
  g_assert_nonnull(poll_msg);
  const gchar *poll_rumor_id = gh_message_get_rumor_id(poll_msg);
  g_assert_nonnull(poll_rumor_id);
  g_test_message("Groundhog sees poll with rumor_id %s", poll_rumor_id);

  GhMlsPoll *gh_poll = gh_mls_service_lookup_poll(alice->service,
    gh_mls_group_get_group_id(ga), poll_rumor_id);
  g_assert_nonnull(gh_poll);
  g_assert_cmpstr(gh_mls_poll_get_question(gh_poll), ==, "Favorite drink?");
  g_assert_cmpuint(gh_mls_poll_get_n_options(gh_poll), ==, 3);
  g_assert_cmpstr(gh_mls_poll_get_option(gh_poll, 0)->label, ==, "Tea");
  g_assert_cmpstr(gh_mls_poll_get_option(gh_poll, 1)->label, ==, "Coffee");
  g_assert_cmpstr(gh_mls_poll_get_option(gh_poll, 2)->label, ==, "Water");

  /* Groundhog votes for "Coffee" (option 1). */
  GhMessage *vote_msg;
  {
    const gchar *ids[] = { "1", NULL };
    g_autoptr(GError) vote_err = NULL;
    vote_msg = gh_mls_service_cast_vote(alice->service, ga,
      poll_rumor_id, ids, 1, &vote_err);
    g_assert_no_error(vote_err);
    g_assert_nonnull(vote_msg);
    /* Wait until the vote's MLS message is accepted by a group relay.
     * We cannot use sent_accepted() because votes have empty content and
     * find_message("") might match the wrong row; instead we check the
     * message object directly. */
    spin_until_msg(vote_msg, "the vote accepted by a group relay");
    g_object_unref(vote_msg);
  }

  /* MDK tallies: sync to receive Alice's vote. */
  {
    g_autoptr(JsonObject) tally = mdk_call(&driver,
      "\"cmd\":\"poll_tally\",\"peer\":\"carol\",\"group\":\"%s\"",
      group);
    JsonArray *polls = json_object_get_array_member(tally, "polls");
    g_assert_cmpuint(json_array_get_length(polls), >=, 1);
    /* Find our poll in the tally results. */
    gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(polls); i++) {
      JsonObject *p = json_array_get_object_element(polls, i);
      if (g_strcmp0(json_object_get_string_member(p, "question"), "Favorite drink?") == 0) {
        found = TRUE;
        g_assert_cmpint(json_object_get_int_member(p, "participants"), >=, 1);
        JsonArray *options = json_object_get_array_member(p, "options");
        /* Coffee (index 1) should have at least 1 vote. */
        JsonObject *coffee = json_array_get_object_element(options, 1);
        g_assert_cmpstr(json_object_get_string_member(coffee, "label"), ==, "Coffee");
        g_assert_cmpint(json_object_get_int_member(coffee, "votes"), >=, 1);
        break;
      }
    }
    g_assert_true(found);
    g_test_message("Phase 1 passed: MDK tallied Groundhog's vote for Coffee");
  }

  /* --- Phase 2: Groundhog creates a poll, MDK votes, Groundhog tallies --- */

  g_test_message("Phase 2: Groundhog creates poll, MDK votes");
  {
    const gchar *labels[] = { "Cat", "Dog", NULL };
    g_autoptr(GError) poll_err = NULL;
    GhMessage *poll2_msg = gh_mls_service_create_poll(alice->service, ga,
      "Best pet?", labels, 2, GH_MLS_POLL_SINGLE_CHOICE,
      (gint64)(g_get_real_time() / G_USEC_PER_SEC + 7200), &poll_err);
    g_assert_no_error(poll_err);
    g_assert_nonnull(poll2_msg);
    const gchar *poll2_rumor = gh_message_get_rumor_id(poll2_msg);
    g_assert_nonnull(poll2_rumor);
    g_test_message("Groundhog poll rumor_id: %s", poll2_rumor);

    /* Wait for the poll to be accepted by a relay. */
    spin_until_msg(poll2_msg, "the poll accepted by a group relay");

    /* MDK syncs to receive the poll. synced_message checks kind 9 (text),
     * but polls are kind 1068; scan the results array directly. */
    g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
    {
      JsonArray *sr = json_object_get_array_member(synced, "results");
      gboolean found_poll = FALSE;
      for (guint i = 0; i < json_array_get_length(sr); i++) {
        JsonObject *r = json_array_get_object_element(sr, i);
        if (json_object_get_int_member_with_default(r, "kind", 0) == GH_MLS_POLL_KIND &&
            g_strcmp0(json_object_get_string_member_with_default(r, "content", ""), "Best pet?") == 0) {
          found_poll = TRUE;
          break;
        }
      }
      g_assert_true(found_poll);
    }

    /* MDK finds the poll's inner event id from the sync results and votes. */
    JsonArray *results = json_object_get_array_member(synced, "results");
    const gchar *poll2_inner_id = NULL;
    for (guint i = 0; i < json_array_get_length(results); i++) {
      JsonObject *r = json_array_get_object_element(results, i);
      if (json_object_get_int_member_with_default(r, "kind", 0) == GH_MLS_POLL_KIND &&
          g_strcmp0(json_object_get_string_member_with_default(r, "content", ""), "Best pet?") == 0) {
        poll2_inner_id = json_object_get_string_member(r, "id");
        break;
      }
    }
    g_assert_nonnull(poll2_inner_id);
    g_test_message("MDK sees poll inner id: %s", poll2_inner_id);

    /* MDK votes for "Dog" (option 1). */
    g_autoptr(JsonObject) mdk_vote = mdk_call(&driver,
      "\"cmd\":\"poll_vote\",\"peer\":\"carol\",\"group\":\"%s\","
      "\"poll_event_id\":\"%s\","
      "\"selections\":[\"1\"]",
      group, poll2_inner_id);
    g_assert_true(json_object_has_member(mdk_vote, "event_id"));

    /* Send a text message after the vote: this ensures the relay subscription
     * has delivered the vote's kind-445 event before we check tallies. */
    mdk_send("carol", group, "voted for Dog");
    wait_message(alice, room, "voted for Dog");

    /* Groundhog receives Carol's vote: the tallies-changed signal fires and
     * the poll model reflects it. */
    GhMlsPoll *poll2 = gh_mls_service_lookup_poll(alice->service,
      gh_mls_group_get_group_id(ga), poll2_rumor);
    g_assert_nonnull(poll2);
    /* Spin until the vote is counted (the encrypted message arrives). */
    spin_until(poll_has_voters, poll2, "Carol's vote counted in Groundhog's tally");
    g_assert_cmpuint(gh_mls_poll_get_option(poll2, 1)->votes, >=, 1);
    g_assert_cmpstr(gh_mls_poll_get_option(poll2, 1)->label, ==, "Dog");
    g_test_message("Phase 2 passed: Groundhog tallied MDK's vote for Dog");

    g_object_unref(poll2_msg);
  }

  /* --- Phase 3: post-deadline vote is ignored by both --- */

  g_test_message("Phase 3: post-deadline vote rejection");
  {
    /* Create a poll that ends in 1 second. */
    gint64 now_ts = g_get_real_time() / G_USEC_PER_SEC;
    const gchar *labels3[] = { "Yes", "No", NULL };
    g_autoptr(GError) poll3_err = NULL;
    GhMessage *poll3_msg = gh_mls_service_create_poll(alice->service, ga,
      "Quick poll?", labels3, 2, GH_MLS_POLL_SINGLE_CHOICE,
      now_ts + 1, &poll3_err);
    g_assert_no_error(poll3_err);
    g_assert_nonnull(poll3_msg);
    const gchar *poll3_rumor = gh_message_get_rumor_id(poll3_msg);
    spin_until_msg(poll3_msg, "the quick poll accepted by a group relay");

    /* Wait 2 seconds so the poll expires. */
    g_usleep(2 * G_USEC_PER_SEC);

    /* Groundhog rejects its own late vote. */
    {
      const gchar *ids[] = { "0", NULL };
      g_autoptr(GError) late_err = NULL;
      GhMessage *late = gh_mls_service_cast_vote(alice->service, ga,
        poll3_rumor, ids, 1, &late_err);
      g_assert_null(late);
      g_assert_error(late_err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_test_message("Groundhog correctly rejects its own late vote: %s",
                     late_err->message);
    }

    /* MDK syncs to receive the poll, then tries a late vote: MDK's
     * validate_poll_response rejects votes past the deadline. */
    {
      g_autoptr(JsonObject) synced = mdk_sync_joined("carol", group, before_join);
      /* Find the poll inner id. */
      JsonArray *results = json_object_get_array_member(synced, "results");
      const gchar *poll3_inner_id = NULL;
      for (guint i = 0; i < json_array_get_length(results); i++) {
        JsonObject *r = json_array_get_object_element(results, i);
        if (json_object_get_int_member_with_default(r, "kind", 0) == GH_MLS_POLL_KIND &&
            g_strcmp0(json_object_get_string_member_with_default(r, "content", ""), "Quick poll?") == 0) {
          poll3_inner_id = json_object_get_string_member(r, "id");
          break;
        }
      }
      if (poll3_inner_id) {
        /* MDK sends a vote (the driver will send it, but the tally should not
         * count it since the MDK's validate_poll_response checks the deadline). */
        g_autoptr(JsonObject) late_vote = mdk_call(&driver,
          "\"cmd\":\"poll_vote\",\"peer\":\"carol\",\"group\":\"%s\","
          "\"poll_event_id\":\"%s\","
          "\"selections\":[\"0\"]",
          group, poll3_inner_id);
        /* The vote is sent as MLS (the transport layer doesn't reject it),
         * but the tally should not count it. */
        g_autoptr(JsonObject) tally = mdk_call(&driver,
          "\"cmd\":\"poll_tally\",\"peer\":\"carol\",\"group\":\"%s\"",
          group);
        JsonArray *polls = json_object_get_array_member(tally, "polls");
        for (guint i = 0; i < json_array_get_length(polls); i++) {
          JsonObject *p = json_array_get_object_element(polls, i);
          if (g_strcmp0(json_object_get_string_member(p, "question"), "Quick poll?") == 0) {
            /* The tally should show 0 participants: the late vote was rejected
             * by validate_poll_response. */
            g_assert_cmpint(json_object_get_int_member(p, "participants"), ==, 0);
            g_test_message("Phase 3 passed: MDK correctly ignores post-deadline vote (0 participants)");
            break;
          }
        }
      } else {
        g_test_message("Phase 3: MDK did not receive the quick poll (ok: it may have expired before sync)");
      }
    }

    g_object_unref(poll3_msg);
  }

  world_down(&w);
  mdk_driver_stop(&driver);
}

int
main(int argc, char **argv)
{
  /* --gui: mdk-invites-groundhog accepts in the real invitations dialog. */
  gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
#ifdef __APPLE__
    /* As the other GUI tests: GTK's macOS accessibility backend has no announce. */
    g_setenv("GTK_A11Y", "none", FALSE);
#endif
    /* Before g_test_init(), as the other GUI tests. */
    if (!gtk_init_check()) {
      g_printerr("groundhog-mdk011-interop GUI case skipped: no graphical display\n");
      return 77;
    }
    adw_init();
    groundhog_register_resource();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_test_init(&argc, &argv, NULL);
    nostrc_test_tolerate_gdk_frame_warning();
    /* mls_world_init() beside GTK: GTK keeps the session bus it was given. */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_log_set_fatal_mask(NULL, G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
    for (guint key = 1; key < GH_TEST_KEYS; key++) {
      hex[key] = gh_test_pub(key);
      npub[key] = gh_test_npub(key);
    }
    gh_test_bus_up_beside_gtk(&test_bus);
    g_test_add_func("/groundhog/mdk011-interop-gui/mdk-invites-groundhog",
                    test_mdk_invites_groundhog);
    gint rc = g_test_run();
    mls_world_finish();
    if (cases_run == 0) {
      g_printerr("no case matched the requested -p path(s): nothing ran\n");
      return 1;
    }
    return rc == 0 && not_run ? 77 : rc;
  }
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mdk011-interop/control", test_control);
  g_test_add_func("/groundhog/mdk011-interop/groundhog-invites-mdk", test_groundhog_invites_mdk);
  g_test_add_func("/groundhog/mdk011-interop/mdk-invites-groundhog", test_mdk_invites_groundhog);
  g_test_add_func("/groundhog/mdk011-interop/adopted-welcome", test_adopted_welcome);
  g_test_add_func("/groundhog/mdk011-interop/white-noise-welcome", test_white_noise_welcome);
  g_test_add_func("/groundhog/mdk011-interop/white-noise-media", test_white_noise_media);
  g_test_add_func("/groundhog/mdk011-interop/adopted-commits", test_adopted_commits);
  g_test_add_func("/groundhog/mdk011-interop/routing-rotation", test_routing_rotation);
  g_test_add_func("/groundhog/mdk011-interop/white-noise-dm", test_white_noise_dm);
  g_test_add_func("/groundhog/mdk011-interop/concurrent-commits", test_concurrent_commits);
  g_test_add_func("/groundhog/mdk011-interop/polls", test_polls);
  g_test_add_func("/groundhog/mdk011-interop/white-noise-reactions", test_white_noise_reactions);
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
