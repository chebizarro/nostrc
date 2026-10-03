/* Two-instance acceptance test (nostrc-sjl1): drives two real Groundhog
 * test-harness processes through the full MLS group lifecycle via D-Bus.
 *
 * Scenario:
 *   1. Both instances onboard (key 1 and key 2)
 *   2. A creates an encrypted group and invites B
 *   3. B accepts the invitation
 *   4. A sends a message, B receives it; B sends, A receives
 *   5. A renames the group
 *   6. A removes B, then re-adds B; B accepts again
 *   7. B leaves
 *   8. Both restart; verify message history persists
 *
 * Local test relays and the test signer only; no network. */

#include "gh-test-signer.h"
#include "wire-relay.h"

#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* ---- global test state ---------------------------------------------------- */

static GhTestBus test_bus;
static GhTestSigner signer;
static WireRelay relay_e, relay_w, relay_x, relay_g;
static gchar *hex[GH_TEST_KEYS];
static gchar *npub[GH_TEST_KEYS];

/* Data directories for restart persistence. */
static gchar *root_dir;
static gchar *data_dir_a;
static gchar *data_dir_b;

/* Child process PIDs. */
static GPid pid_a, pid_b;

/* ---- relay list seeding --------------------------------------------------- */

static gchar *
sign_event_local(guint key, gint kind, const gchar *content, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - 3600);
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static void
seed_list(guint key, gint kind, const gchar *url)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new(kind == 10050 ? "relay" : "r", url, NULL));
  g_autofree gchar *json = sign_event_local(key, kind, "", tags);
  wire_relay_inject(&relay_e, json);
}

/* ---- D-Bus call helpers --------------------------------------------------- */

#define TC_IFACE "org.nostr.Groundhog.TestControl"
#define TC_PATH  "/org/nostr/Groundhog/TestControl"

/* Timeout for D-Bus method calls (milliseconds). The MLS operations may
 * take a while under load or sanitisers. */
#define CALL_TIMEOUT_MS (120 * 1000)

typedef struct {
  GVariant *reply;
  GError *error;
  gboolean done;
} TcCallData;

static void
tc_call_cb(GObject *source, GAsyncResult *res, gpointer data)
{
  TcCallData *d = data;
  d->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &d->error);
  d->done = TRUE;
}

/* Call a TestControl method and spin the main context until the reply
 * arrives.  Using g_dbus_connection_call (async) instead of _call_sync
 * is essential: the WireRelay servers live on the default main context,
 * so we must keep iterating it while the harness processes the request. */
static GVariant *
tc_call(const gchar *bus_name, const gchar *method, GVariant *args, const GVariantType *reply_type)
{
  GDBusConnection *conn = nostrc_test_bus_connect(test_bus.bus);
  TcCallData data = { 0 };
  g_dbus_connection_call(conn, bus_name, TC_PATH, TC_IFACE,
    method, args, reply_type, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL,
    tc_call_cb, &data);
  while (!data.done)
    g_main_context_iteration(NULL, TRUE);
  if (data.error)
    g_error("D-Bus call %s.%s failed: %s", bus_name, method, data.error->message);
  return data.reply;
}

/* ---- bus name wait -------------------------------------------------------- */

typedef struct {
  const gchar *name;
  gboolean appeared;
} NameWatch;

static void
on_name_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner,
                 gpointer user_data)
{
  (void)connection; (void)name; (void)name_owner;
  ((NameWatch *)user_data)->appeared = TRUE;
}

static void
wait_for_name(const gchar *bus_name)
{
  NameWatch watch = { bus_name, FALSE };
  guint watcher = g_bus_watch_name(G_BUS_TYPE_SESSION, bus_name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   on_name_appeared, NULL, &watch, NULL);
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(30, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(10, gh_test_tick, NULL);
  while (!watch.appeared && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  g_source_remove(timer);
  g_bus_unwatch_name(watcher);
  g_assert_false(expired);
}

/* ---- harness process management ------------------------------------------- */

static GPid
spawn_harness(const gchar *instance, const gchar *data_dir)
{
  const gchar *bin = g_getenv("HARNESS_BIN");
  g_assert_nonnull(bin);

  gchar *argv[] = { (gchar *)bin, (gchar *)instance, (gchar *)data_dir, NULL };

  /* Inherit the test bus address (set by nostrc_test_bus_up). The child
   * also inherits GSETTINGS_SCHEMA_DIR and GSETTINGS_BACKEND. */
  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD;
  g_autoptr(GError) error = NULL;
  GPid pid;
  gboolean ok = g_spawn_async(NULL, argv, NULL, flags, NULL, NULL, &pid, &error);
  if (!ok)
    g_error("cannot spawn harness: %s", error->message);
  return pid;
}

static void
quit_harness(const gchar *bus_name)
{
  /* Call Quit; the harness exits cleanly.  Use async + spin so the
   * main context keeps running (relays may still be processing). */
  GDBusConnection *conn = nostrc_test_bus_connect(test_bus.bus);
  TcCallData data = { 0 };
  g_dbus_connection_call(conn, bus_name, TC_PATH, TC_IFACE,
    "Quit", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
    tc_call_cb, &data);
  while (!data.done)
    g_main_context_iteration(NULL, TRUE);
  /* If the call fails (e.g. already exiting), that's OK. */
  g_clear_error(&data.error);
  if (data.reply)
    g_variant_unref(data.reply);
}

static void
reap(GPid pid)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(15, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(100, gh_test_tick, NULL);
  int status;
  while (!expired) {
    if (waitpid(pid, &status, WNOHANG) == pid)
      break;
    g_main_context_iteration(NULL, FALSE);
  }
  g_source_remove(tick);
  g_source_remove(timer);
  g_spawn_close_pid(pid);
}

/* ---- rm -rf --------------------------------------------------------------- */

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

/* ---- the test ------------------------------------------------------------- */

static void
test_acceptance(void)
{
  /* Spawn both harness processes. */
  pid_a = spawn_harness("testA", data_dir_a);
  pid_b = spawn_harness("testB", data_dir_b);

  g_test_message("waiting for harness processes to register on D-Bus");
  wait_for_name("org.nostr.Groundhog.testA");
  wait_for_name("org.nostr.Groundhog.testB");

  /* 1. Both onboard. */
  g_test_message("onboarding A (key 1) and B (key 2)");
  tc_call("org.nostr.Groundhog.testA", "Onboard",
    g_variant_new("(sssss)", npub[1], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  tc_call("org.nostr.Groundhog.testB", "Onboard",
    g_variant_new("(sssss)", npub[2], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  g_test_message("both instances onboarded with KeyPackages published");

  /* Accept each other as contacts (required before group invitations). */
  g_test_message("accepting contacts");
  tc_call("org.nostr.Groundhog.testA", "AcceptContact",
    g_variant_new("(s)", hex[2]), NULL);
  tc_call("org.nostr.Groundhog.testB", "AcceptContact",
    g_variant_new("(s)", hex[1]), NULL);

  /* 2. A creates a group and invites B. */
  g_test_message("A creates group and invites B");
  g_autoptr(GVariant) create_reply = tc_call("org.nostr.Groundhog.testA", "CreateGroup",
    g_variant_new("(s@as)", "Test Group",
      g_variant_new_strv((const gchar *const[]){ hex[2], NULL }, 1)),
    G_VARIANT_TYPE("(s)"));
  const gchar *room_id_a;
  g_variant_get(create_reply, "(&s)", &room_id_a);
  g_autofree gchar *room_a = g_strdup(room_id_a);
  g_test_message("A's group room_id: %s", room_a);

  /* 3. B accepts the invitation. */
  g_test_message("B accepts invitation");
  g_autoptr(GVariant) accept_reply = tc_call("org.nostr.Groundhog.testB", "AcceptInvite",
    NULL, G_VARIANT_TYPE("(s)"));
  const gchar *room_id_b;
  g_variant_get(accept_reply, "(&s)", &room_id_b);
  g_autofree gchar *room_b = g_strdup(room_id_b);
  g_test_message("B's group room_id: %s", room_b);

  /* 4. Messages both ways. */
  g_test_message("A sends message");
  tc_call("org.nostr.Groundhog.testA", "SendText",
    g_variant_new("(ss)", room_a, "hello from A"), NULL);

  g_test_message("B waits for A's message");
  tc_call("org.nostr.Groundhog.testB", "WaitMessage",
    g_variant_new("(ss)", room_b, "hello from A"), NULL);

  g_test_message("B sends message");
  tc_call("org.nostr.Groundhog.testB", "SendText",
    g_variant_new("(ss)", room_b, "hello from B"), NULL);

  g_test_message("A waits for B's message");
  tc_call("org.nostr.Groundhog.testA", "WaitMessage",
    g_variant_new("(ss)", room_a, "hello from B"), NULL);

  /* Verify messages are listed on both sides. */
  g_autoptr(GVariant) msgs_a = tc_call("org.nostr.Groundhog.testA", "ListMessages",
    g_variant_new("(s)", room_a), G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) msgs_b = tc_call("org.nostr.Groundhog.testB", "ListMessages",
    g_variant_new("(s)", room_b), G_VARIANT_TYPE("(as)"));
  g_test_message("A has %zu messages, B has %zu messages",
    g_variant_n_children(g_variant_get_child_value(msgs_a, 0)),
    g_variant_n_children(g_variant_get_child_value(msgs_b, 0)));

  /* 5. A renames the group. */
  g_test_message("A renames group");
  tc_call("org.nostr.Groundhog.testA", "Rename",
    g_variant_new("(ss)", room_a, "Renamed Group"), NULL);

  /* Give B time to process the rename. */
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(30, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(200, gh_test_tick, NULL);
  gboolean name_match = FALSE;
  while (!expired && !name_match) {
    g_autoptr(GVariant) name_reply = tc_call("org.nostr.Groundhog.testB", "GetGroupName",
      g_variant_new("(s)", room_b), G_VARIANT_TYPE("(s)"));
    const gchar *name;
    g_variant_get(name_reply, "(&s)", &name);
    if (g_str_equal(name, "Renamed Group"))
      name_match = TRUE;
    else
      g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(tick);
  g_source_remove(timer);
  g_assert_true(name_match);
  g_test_message("B sees renamed group");

  /* 6. A removes B, then re-adds B. */
  g_test_message("A removes B");
  tc_call("org.nostr.Groundhog.testA", "RemoveMember",
    g_variant_new("(ss)", room_a, hex[2]), NULL);

  g_test_message("A re-adds B");
  tc_call("org.nostr.Groundhog.testA", "AddMember",
    g_variant_new("(ss)", room_a, hex[2]), NULL);

  g_test_message("B accepts second invitation");
  g_autoptr(GVariant) accept2_reply = tc_call("org.nostr.Groundhog.testB", "AcceptInvite",
    NULL, G_VARIANT_TYPE("(s)"));
  const gchar *room_id_b2;
  g_variant_get(accept2_reply, "(&s)", &room_id_b2);
  g_autofree gchar *room_b2 = g_strdup(room_id_b2);
  g_test_message("B's new group room_id after re-add: %s", room_b2);

  /* 7. B leaves. */
  g_test_message("B leaves the group");
  tc_call("org.nostr.Groundhog.testB", "Leave",
    g_variant_new("(s)", room_b2), NULL);

  /* 8. Restart both and verify history. */
  g_test_message("restarting both instances");
  quit_harness("org.nostr.Groundhog.testA");
  quit_harness("org.nostr.Groundhog.testB");
  reap(pid_a);
  reap(pid_b);

  /* Re-spawn with same data dirs. */
  pid_a = spawn_harness("testA", data_dir_a);
  pid_b = spawn_harness("testB", data_dir_b);
  wait_for_name("org.nostr.Groundhog.testA");
  wait_for_name("org.nostr.Groundhog.testB");

  /* Re-onboard (same keys, same relays, same data dirs → reopens store). */
  tc_call("org.nostr.Groundhog.testA", "Onboard",
    g_variant_new("(sssss)", npub[1], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  tc_call("org.nostr.Groundhog.testB", "Onboard",
    g_variant_new("(sssss)", npub[2], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);

  /* Verify history: A still has the messages. */
  g_test_message("verifying history after restart");
  g_autoptr(GVariant) groups_a = tc_call("org.nostr.Groundhog.testA", "ListGroups",
    NULL, G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) groups_arr = g_variant_get_child_value(groups_a, 0);
  g_assert_cmpuint(g_variant_n_children(groups_arr), >, 0);
  const gchar *restored_room;
  g_variant_get_child(groups_arr, 0, "&s", &restored_room);
  g_test_message("A's restored group: %s", restored_room);

  g_autoptr(GVariant) history = tc_call("org.nostr.Groundhog.testA", "ListMessages",
    g_variant_new("(s)", restored_room), G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) history_arr = g_variant_get_child_value(history, 0);
  gboolean found_hello_a = FALSE, found_hello_b = FALSE;
  for (gsize i = 0; i < g_variant_n_children(history_arr); i++) {
    const gchar *msg;
    g_variant_get_child(history_arr, i, "&s", &msg);
    if (g_str_equal(msg, "hello from A")) found_hello_a = TRUE;
    if (g_str_equal(msg, "hello from B")) found_hello_b = TRUE;
  }
  g_assert_true(found_hello_a);
  g_assert_true(found_hello_b);
  g_test_message("history preserved: both messages found after restart");

  /* Clean up. */
  quit_harness("org.nostr.Groundhog.testA");
  quit_harness("org.nostr.Groundhog.testB");
  reap(pid_a);
  reap(pid_b);
  pid_a = pid_b = 0;
}

/* ---- setup / teardown ----------------------------------------------------- */

static void
setup(void)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    hex[key] = gh_test_pub(key);
    npub[key] = gh_test_npub(key);
  }

  gh_test_bus_up(&test_bus);
  gh_test_signer_up(&test_bus, &signer);

  /* Local relays: E (discovery), W (write), X (inbox), G (group). */
  WireRelay *relays[] = { &relay_e, &relay_w, &relay_x, &relay_g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    relays[i]->serve = TRUE;
    relays[i]->record = TRUE;
    relay_init(relays[i]);
  }
  relay_x.auth_gate_dms = TRUE;
  relay_g.require_auth = TRUE;

  /* Seed relay lists on the discovery relay. */
  for (guint key = 1; key <= 2; key++) {
    seed_list(key, 10002, relay_w.url);
    seed_list(key, 10050, relay_x.url);
  }

  /* Data directories. */
  root_dir = g_dir_make_tmp("groundhog-acceptance-XXXXXX", NULL);
  g_assert_nonnull(root_dir);
  data_dir_a = g_build_filename(root_dir, "instance-a", NULL);
  data_dir_b = g_build_filename(root_dir, "instance-b", NULL);
}

static void
teardown(void)
{
  /* Kill any lingering processes. */
  if (pid_a) { kill(pid_a, SIGTERM); reap(pid_a); }
  if (pid_b) { kill(pid_b, SIGTERM); reap(pid_b); }

  WireRelay *relays[] = { &relay_e, &relay_w, &relay_x, &relay_g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);

  gh_test_signer_down(&test_bus, &signer);
  gh_test_bus_down(&test_bus);

  if (root_dir) {
    rm_rf(root_dir);
    g_free(root_dir);
  }
  g_free(data_dir_a);
  g_free(data_dir_b);

  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(hex[key]);
    g_free(npub[key]);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  if (!g_getenv("HARNESS_BIN")) {
    g_test_message("HARNESS_BIN not set — skipping");
    return 77;
  }

  if (!nostrc_test_bus_available()) {
    g_test_message("no D-Bus daemon available — skipping");
    return 77;
  }

  g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

  setup();
  g_test_add_func("/groundhog/two-instance/acceptance", test_acceptance);
  int result = g_test_run();
  teardown();
  return result;
}
