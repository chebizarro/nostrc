/* Two-instance acceptance test (nostrc-sjl1): drives two real Groundhog
 * application processes through the full MLS group lifecycle via D-Bus.
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
#include "gh-mls-service.h"
#include "wire-relay.h"
#ifdef GH_TEST_REMOTE_VARIANT
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_envelope.h>
#endif

#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* ---- global test state ---------------------------------------------------- */

static GhTestBus test_bus;
static GhTestSigner signer;
static WireRelay relay_e, relay_w, relay_x, relay_g;
#ifdef GH_TEST_REMOTE_VARIANT
static WireRelay relay_bunker;
static guint bunker_calls;
static guint bunker_sign_calls;
#endif
static gchar *hex[GH_TEST_KEYS];
static gchar *npub[GH_TEST_KEYS];

/* Data directories for restart persistence. */
static gchar *root_dir;

/* Child process PIDs. Signal cleanup also runs when a GTest assertion aborts. */
static GPid pid_a, pid_b;

static void
kill_children_on_signal(int signo)
{
  if (pid_a > 0)
    kill(pid_a, SIGKILL);
  if (pid_b > 0)
    kill(pid_b, SIGKILL);
  _exit(128 + signo);
}

static void
install_child_cleanup(void)
{
  struct sigaction action = { 0 };
  action.sa_handler = kill_children_on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGABRT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGSEGV, &action, NULL);
}

#ifdef GH_TEST_REMOTE_VARIANT
/* The signer relay is separate from discovery, write, inbox and group relays.
 * It checks both NIP-46 transport keys and the account author on every call. */
static void
wire_bunker_on_event(WireRelay *relay, SoupWebsocketConnection *connection,
                     const gchar *event_id, gpointer data)
{
  (void)connection; (void)data;
  WireStored *stored = NULL;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *candidate = g_ptr_array_index(relay->stored, i);
    if (g_str_equal(candidate->id, event_id)) { stored = candidate; break; }
  }
  g_assert_nonnull(stored);
  NostrEvent *event = stored->event;
  if (nostr_event_get_kind(event) != 24133) return;
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex[3]);
  g_assert_true(wire_tag_is(nostr_event_get_tags(event), "p", hex[4]));

  guint8 transport_sk[32], client_pk[32], user_sk[32];
  g_assert_true(nostr_hex2bin(transport_sk, gh_test_secret[4], sizeof transport_sk));
  g_assert_true(nostr_hex2bin(client_pk, hex[3], sizeof client_pk));
  g_assert_true(nostr_hex2bin(user_sk, gh_test_secret[1], sizeof user_sk));
  guint8 *plain = NULL;
  size_t plain_len = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(transport_sk, client_pk,
    nostr_event_get_content(event), &plain, &plain_len), ==, 0);
  g_autofree gchar *request_json = g_strndup((const gchar *)plain, plain_len);
  free(plain);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  bunker_calls++;
  gchar *value = NULL;
  if (g_str_equal(request.method, "sign_event")) {
    g_assert_cmpuint(request.n_params, ==, 1);
    NostrEvent *unsigned_event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(unsigned_event, request.params[0], NULL), ==, 1);
    g_assert_cmpstr(nostr_event_get_pubkey(unsigned_event), ==, hex[1]);
    g_assert_cmpint(nostr_event_sign(unsigned_event, gh_test_secret[1]), ==, 0);
    value = nostr_event_serialize_compact(unsigned_event);
    nostr_event_free(unsigned_event);
    bunker_sign_calls++;
  } else if (g_str_equal(request.method, "nip44_encrypt") ||
             g_str_equal(request.method, "nip44_decrypt")) {
    g_assert_cmpuint(request.n_params, ==, 2);
    guint8 peer_pk[32];
    g_assert_true(nostr_hex2bin(peer_pk, request.params[0], sizeof peer_pk));
    if (g_str_equal(request.method, "nip44_encrypt")) {
      g_assert_cmpint(nostr_nip44_encrypt_v2(user_sk, peer_pk,
        (const guint8 *)request.params[1], strlen(request.params[1]), &value), ==, 0);
    } else {
      guint8 *decrypted = NULL;
      size_t len = 0;
      g_assert_cmpint(nostr_nip44_decrypt_v2(user_sk, peer_pk, request.params[1],
        &decrypted, &len), ==, 0);
      value = g_strndup((const gchar *)decrypted, len);
      free(decrypted);
    }
  } else if (g_str_equal(request.method, "ping")) {
    value = strdup("pong"); /* the health ping after restore (nostrc-8xfib.1) */
  } else {
    g_error("unpermitted bunker method: %s", request.method);
  }
  g_autofree gchar *quoted = g_strescape(value, NULL);
  g_autofree gchar *result = g_strdup_printf("\"%s\"", quoted);
  char *response = nostr_nip46_response_build_ok(request.id, result);
  g_assert_nonnull(response);
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(transport_sk, client_pk,
    (const guint8 *)response, strlen(response), &ciphertext), ==, 0);
  NostrEvent *reply = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(hex[4], hex[3], response, &reply), ==, 0);
  nostr_event_set_content(reply, ciphertext);
  g_assert_cmpint(nostr_event_sign(reply, gh_test_secret[4]), ==, 0);
  char *reply_json = nostr_event_serialize_compact(reply);
  wire_relay_inject(relay, reply_json);
  free(reply_json);
  nostr_event_free(reply);
  free(ciphertext);
  free(response);
  if (g_str_equal(request.method, "nip44_decrypt")) g_free(value); else free(value);
  nostr_nip46_request_free(&request);
}
#endif

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
  if (!reply_type) {
    g_clear_pointer(&data.reply, g_variant_unref);
    return NULL;
  }
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
  if (!expired)
    g_source_remove(timer);
  g_bus_unwatch_name(watcher);
  g_assert_false(expired);
}

/* Object registration follows acquisition of the GApplication bus name.
 * Wait for the actual control object, not just the well-known name. */
static void
wait_for_control(const gchar *bus_name)
{
  GDBusConnection *conn = nostrc_test_bus_connect(test_bus.bus);
  gboolean expired = FALSE;
  guint deadline = g_timeout_add_seconds(30, gh_test_deadline_hit, &expired);
  while (!expired) {
    TcCallData data = { 0 };
    g_dbus_connection_call(conn, bus_name, TC_PATH,
      "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, tc_call_cb, &data);
    while (!data.done)
      g_main_context_iteration(NULL, TRUE);
    if (data.reply) {
      const gchar *xml = NULL;
      g_variant_get(data.reply, "(&s)", &xml);
      gboolean ready = xml && strstr(xml, TC_IFACE) != NULL;
      g_variant_unref(data.reply);
      if (ready) {
        g_clear_error(&data.error);
        if (!expired)
          g_source_remove(deadline);
        return;
      }
    }
    g_clear_error(&data.error);
    gboolean retry = FALSE;
    guint tick = g_timeout_add(50, gh_test_deadline_hit, &retry);
    while (!retry && !expired)
      g_main_context_iteration(NULL, TRUE);
    if (!retry)
      g_source_remove(tick);
  }
  g_error("Groundhog %s did not export TestControl", bus_name);
}

/* ---- real application process management ---------------------------------- */

static GPid
spawn_groundhog(const gchar *instance)
{
  const gchar *bin = g_getenv("GROUNDHOG_BIN");
  g_assert_nonnull(bin);
  gchar *argv[] = { (gchar *)bin, "--instance", (gchar *)instance,
                    "--gapplication-service", NULL };
  g_auto(GStrv) env = g_get_environ();
  env = g_environ_setenv(env, "GH_TEST_CONTROL", "1", TRUE);
  /* A hostile inherited dconf backend must not defeat named isolation. */
  env = g_environ_setenv(env, "GSETTINGS_BACKEND", "dconf", TRUE);
  g_autofree gchar *config = g_build_filename(root_dir, "config", NULL);
  g_autofree gchar *data = g_build_filename(root_dir, "data", NULL);
  g_autofree gchar *cache = g_build_filename(root_dir, "cache", NULL);
  g_autofree gchar *state = g_build_filename(root_dir, "state", NULL);
  g_mkdir_with_parents(config, 0700);
  g_mkdir_with_parents(data, 0700);
  g_mkdir_with_parents(cache, 0700);
  g_mkdir_with_parents(state, 0700);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", config, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", data, TRUE);
  env = g_environ_setenv(env, "XDG_CACHE_HOME", cache, TRUE);
  env = g_environ_setenv(env, "XDG_STATE_HOME", state, TRUE);

  g_autoptr(GError) error = NULL;
  GPid pid;
  if (!g_spawn_async(NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD,
                     NULL, NULL, &pid, &error))
    g_error("cannot spawn Groundhog instance %s: %s", instance, error->message);
  if (g_str_equal(instance, "testA"))
    pid_a = pid;
  else
    pid_b = pid;
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
reap(GPid pid, gboolean require_success)
{
  gboolean expired = FALSE;
  gboolean exited = FALSE;
  guint timer = g_timeout_add_seconds(15, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(100, gh_test_tick, NULL);
  int status = 0;
  while (!expired) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = TRUE;
      break;
    }
    g_main_context_iteration(NULL, FALSE);
  }
  g_source_remove(tick);
  if (!expired)
    g_source_remove(timer);
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  if (pid_a == pid)
    pid_a = 0;
  if (pid_b == pid)
    pid_b = 0;
  g_spawn_close_pid(pid);
  if (!exited)
    g_error("Groundhog child %ld did not exit within 15 s", (long)pid);
  if (require_success)
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
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

static void
assert_history(const gchar *bus_name, const gchar *room_id)
{
  static const gchar *const expected[] = {
    "hello from A", "hello from B",
    "hello after re-add from A", "hello after re-add from B"
  };
  g_autoptr(GVariant) reply = tc_call(bus_name, "ListMessages",
    g_variant_new("(s)", room_id), G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) messages = g_variant_get_child_value(reply, 0);
  for (guint e = 0; e < G_N_ELEMENTS(expected); e++) {
    gboolean found = FALSE;
    for (gsize i = 0; i < g_variant_n_children(messages); i++) {
      const gchar *text;
      g_variant_get_child(messages, i, "&s", &text);
      if (g_strcmp0(text, expected[e]) == 0) {
        found = TRUE;
        break;
      }
    }
    if (!found)
      g_error("%s lost '%s' after restart", bus_name, expected[e]);
  }
}

/* ---- the test ------------------------------------------------------------- */

static void
test_acceptance(void)
{
  /* Spawn both real application processes. */
  pid_a = spawn_groundhog("testA");
  pid_b = spawn_groundhog("testB");

  g_test_message("waiting for Groundhog processes to register on D-Bus");
  wait_for_name("org.nostr.Groundhog.testA");
  wait_for_name("org.nostr.Groundhog.testB");
  wait_for_control("org.nostr.Groundhog.testA");
  wait_for_control("org.nostr.Groundhog.testB");

  /* 1. Both onboard through the production account controller. */
  g_test_message("onboarding A (key 1) and B (key 2)");
  tc_call("org.nostr.Groundhog.testA",
#ifdef GH_TEST_REMOTE_VARIANT
    "OnboardRemote",
#else
    "Onboard",
#endif
    g_variant_new("(sssss)", npub[1], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  tc_call("org.nostr.Groundhog.testB", "Onboard",
    g_variant_new("(sssss)", npub[2], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  tc_call("org.nostr.Groundhog.testA", "WaitReady", g_variant_new("(s)", relay_g.url), NULL);
  tc_call("org.nostr.Groundhog.testB", "WaitReady", g_variant_new("(s)", relay_g.url), NULL);
  g_test_message("both instances onboarded with KeyPackages published");
#ifdef GH_TEST_REMOTE_VARIANT
  g_assert_cmpuint(bunker_sign_calls, >, 0);
#endif
  g_autoptr(GVariant) background = tc_call("org.nostr.Groundhog.testA",
    "TryEnableBackground", NULL, G_VARIANT_TYPE("(b)"));
  gboolean background_enabled = TRUE;
  g_variant_get(background, "(b)", &background_enabled);
  g_assert_false(background_enabled);

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
  tc_call("org.nostr.Groundhog.testA", "WaitMember",
    g_variant_new("(ssb)", room_a, hex[2], TRUE), NULL);

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
  g_autoptr(GVariant) msgs_a_arr = g_variant_get_child_value(msgs_a, 0);
  g_autoptr(GVariant) msgs_b_arr = g_variant_get_child_value(msgs_b, 0);
  g_test_message("A has %zu messages, B has %zu messages",
    g_variant_n_children(msgs_a_arr), g_variant_n_children(msgs_b_arr));

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
  if (!expired)
    g_source_remove(timer);
  g_assert_true(name_match);
  g_test_message("B sees renamed group");

  /* 6. A removes B, then re-adds B. */
  g_test_message("A removes B");
  tc_call("org.nostr.Groundhog.testA", "RemoveMember",
    g_variant_new("(ss)", room_a, hex[2]), NULL);
  tc_call("org.nostr.Groundhog.testB", "WaitGroupEnd",
    g_variant_new("(si)", room_b, GH_MLS_GROUP_END_REMOVED), NULL);
  tc_call("org.nostr.Groundhog.testA", "WaitMember",
    g_variant_new("(ssb)", room_a, hex[2], FALSE), NULL);

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
  tc_call("org.nostr.Groundhog.testA", "WaitMember",
    g_variant_new("(ssb)", room_a, hex[2], TRUE), NULL);
  tc_call("org.nostr.Groundhog.testA", "SendText",
    g_variant_new("(ss)", room_a, "hello after re-add from A"), NULL);
  tc_call("org.nostr.Groundhog.testB", "WaitMessage",
    g_variant_new("(ss)", room_b2, "hello after re-add from A"), NULL);
  tc_call("org.nostr.Groundhog.testB", "SendText",
    g_variant_new("(ss)", room_b2, "hello after re-add from B"), NULL);
  tc_call("org.nostr.Groundhog.testA", "WaitMessage",
    g_variant_new("(ss)", room_a, "hello after re-add from B"), NULL);

  /* 7. B leaves. */
  g_test_message("B leaves the group");
  tc_call("org.nostr.Groundhog.testB", "Leave",
    g_variant_new("(s)", room_b2), NULL);
  tc_call("org.nostr.Groundhog.testA", "WaitMember",
    g_variant_new("(ssb)", room_a, hex[2], FALSE), NULL);

  /* 8. Restart both and verify history. */
  g_test_message("restarting both instances");
  quit_harness("org.nostr.Groundhog.testA");
  quit_harness("org.nostr.Groundhog.testB");
  reap(pid_a, TRUE);
  reap(pid_b, TRUE);

  /* Re-spawn with the same named-instance XDG directories. */
  pid_a = spawn_groundhog("testA");
  pid_b = spawn_groundhog("testB");
  wait_for_name("org.nostr.Groundhog.testA");
  wait_for_name("org.nostr.Groundhog.testB");
  wait_for_control("org.nostr.Groundhog.testA");
  wait_for_control("org.nostr.Groundhog.testB");

  /* Keyfile settings must restore the distinct selected identities without
   * calling Onboard again: a forced dconf backend must not share them. */
  g_autoptr(GVariant) active_a = tc_call("org.nostr.Groundhog.testA", "GetActiveNpub",
    NULL, G_VARIANT_TYPE("(s)"));
  g_autoptr(GVariant) active_b = tc_call("org.nostr.Groundhog.testB", "GetActiveNpub",
    NULL, G_VARIANT_TYPE("(s)"));
  const gchar *restored_a, *restored_b;
  g_variant_get(active_a, "(&s)", &restored_a);
  g_variant_get(active_b, "(&s)", &restored_b);
  g_assert_cmpstr(restored_a, ==, npub[1]);
  g_assert_cmpstr(restored_b, ==, npub[2]);

  /* Re-onboard uses the same keys and relays and reopens the real store. */
  tc_call("org.nostr.Groundhog.testA",
#ifdef GH_TEST_REMOTE_VARIANT
    "OnboardRemote",
#else
    "Onboard",
#endif
    g_variant_new("(sssss)", npub[1], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);
  tc_call("org.nostr.Groundhog.testB", "Onboard",
    g_variant_new("(sssss)", npub[2], relay_e.url, relay_w.url, relay_x.url, relay_g.url),
    NULL);

  tc_call("org.nostr.Groundhog.testA", "WaitReady", g_variant_new("(s)", relay_g.url), NULL);
  tc_call("org.nostr.Groundhog.testB", "WaitReady", g_variant_new("(s)", relay_g.url), NULL);

  /* Verify history: A still has the messages. */
  g_test_message("verifying history after restart");
  g_autoptr(GVariant) groups_a = tc_call("org.nostr.Groundhog.testA", "ListGroups",
    NULL, G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) groups_arr = g_variant_get_child_value(groups_a, 0);
  g_assert_cmpuint(g_variant_n_children(groups_arr), >, 0);
  const gchar *restored_room;
  g_variant_get_child(groups_arr, 0, "&s", &restored_room);
  g_test_message("A's restored group: %s", restored_room);

  g_autoptr(GVariant) groups_b = tc_call("org.nostr.Groundhog.testB", "ListGroups",
    NULL, G_VARIANT_TYPE("(as)"));
  g_autoptr(GVariant) groups_b_arr = g_variant_get_child_value(groups_b, 0);
  g_assert_cmpuint(g_variant_n_children(groups_b_arr), >, 0);
  assert_history("org.nostr.Groundhog.testA", restored_room);
  assert_history("org.nostr.Groundhog.testB", room_b2);
  g_test_message("history preserved on both devices after restart");

#ifdef GH_TEST_REMOTE_VARIANT
  g_assert_cmpuint(bunker_calls, >, bunker_sign_calls);
#endif
  /* Clean up. */
  quit_harness("org.nostr.Groundhog.testA");
  quit_harness("org.nostr.Groundhog.testB");
  reap(pid_a, TRUE);
  reap(pid_b, TRUE);
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

  root_dir = g_dir_make_tmp("groundhog-acceptance-XXXXXX", NULL);
  g_assert_nonnull(root_dir);
  gh_test_bus_up(&test_bus);

#ifdef GH_TEST_REMOTE_VARIANT
  relay_bunker.serve = TRUE;
  relay_bunker.record = TRUE;
  relay_bunker.on_event = wire_bunker_on_event;
  relay_init(&relay_bunker);
#endif

  /* A real Secret Service, with two signer-visible identities, also owns
   * the per-account encrypted store keys across process restarts. */
  const gchar *secret_script = g_getenv("SECRET_SERVICE_SCRIPT");
  g_assert_nonnull(secret_script);
  g_autofree gchar *secrets_path = g_build_filename(root_dir, "secrets.json", NULL);
#ifdef GH_TEST_REMOTE_VARIANT
  g_autofree gchar *credential_json = g_strdup_printf(
    "{\"version\":1,\"client_secret_hex\":\"%s\",\"remote_signer_pubkey_hex\":\"%s\",\"user_pubkey_hex\":\"%s\",\"transport\":\"nip44-v2\",\"relays\":[\"%s\"]}",
    gh_test_secret[3], hex[4], hex[1], relay_bunker.url);
  g_autofree gchar *credential_b64 = g_base64_encode((const guchar *)credential_json, strlen(credential_json));
#endif
  g_autofree gchar *secrets_json = g_strdup_printf(
#ifdef GH_TEST_REMOTE_VARIANT
    "{\"counter\":3,\"items\":{"
#else
    "{\"counter\":2,\"items\":{"
#endif
    "\"i1\":{\"label\":\"Test A\",\"attrs\":{\"npub\":\"%s\",\"origin\":\"import\",\"key_id\":\"%s\",\"xdg:schema\":\"org.gnostr.Signer/identity\",\"curve\":\"secp256k1\",\"label\":\"Test A\"},\"secret\":\"\",\"ctype\":\"text/plain\",\"created\":0,\"modified\":0},"
    "\"i2\":{\"label\":\"Test B\",\"attrs\":{\"npub\":\"%s\",\"origin\":\"import\",\"key_id\":\"%s\",\"xdg:schema\":\"org.gnostr.Signer/identity\",\"curve\":\"secp256k1\",\"label\":\"Test B\"},\"secret\":\"\",\"ctype\":\"text/plain\",\"created\":0,\"modified\":0}"
#ifdef GH_TEST_REMOTE_VARIANT
    ",\"i3\":{\"label\":\"Groundhog remote signer\",\"attrs\":{\"account\":\"%s\",\"version\":\"1\",\"xdg:schema\":\"org.nostr.Groundhog.Nip46Credential\"},\"secret\":\"%s\",\"ctype\":\"application/json\",\"created\":0,\"modified\":0}"
#endif
    "}}",
    npub[1], npub[1], npub[2], npub[2]
#ifdef GH_TEST_REMOTE_VARIANT
    , hex[1], credential_b64
#endif
    );
  g_assert_true(g_file_set_contents(secrets_path, secrets_json, -1, NULL));
  const gchar *secret_argv[] = { "python3", secret_script, secrets_path, NULL };
  nostrc_test_bus_spawn_supervised(test_bus.bus, "secret-service.log", NULL, secret_argv);
  wait_for_name("org.freedesktop.secrets");
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

}

static void
teardown(void)
{
  /* Kill any lingering processes. */
  if (pid_a) { kill(pid_a, SIGTERM); reap(pid_a, FALSE); }
  if (pid_b) { kill(pid_b, SIGTERM); reap(pid_b, FALSE); }

  WireRelay *relays[] = { &relay_e, &relay_w, &relay_x, &relay_g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    relay_clear(relays[i]);
#ifdef GH_TEST_REMOTE_VARIANT
  relay_clear(&relay_bunker);
#endif

  gh_test_signer_down(&test_bus, &signer);
  gh_test_bus_down(&test_bus);

  if (root_dir) {
    rm_rf(root_dir);
    g_free(root_dir);
  }

  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(hex[key]);
    g_free(npub[key]);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  if (!g_getenv("GROUNDHOG_BIN")) {
    g_test_message("GROUNDHOG_BIN not set — skipping");
    return 77;
  }

  if (!nostrc_test_bus_available()) {
    g_test_message("no D-Bus daemon available — skipping");
    return 77;
  }

  g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  install_child_cleanup();

  setup();
#ifdef GH_TEST_REMOTE_VARIANT
  g_test_add_func("/groundhog/two-instance/remote-acceptance", test_acceptance);
#else
  g_test_add_func("/groundhog/two-instance/acceptance", test_acceptance);
#endif
  int result = g_test_run();
  teardown();
  return result;
}
