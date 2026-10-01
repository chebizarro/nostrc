/* G24 privacy harness, encrypted groups (nostrc-qp24.13 part 1; charter
 * §2.2 "MLS encrypted group", §4.3, §4.4, PD-8, H7):
 *  - PT-8 (MLS): no KeyPackage is looked up for a message request or a
 *    stranger; nothing about them reaches any relay; accepting the request
 *    is what allows it;
 *  - Welcomes only as NIP-59 gift wraps, to the invitee's own kind-10050
 *    inbox relays and nowhere else; no bare kind 444 anywhere; every kind
 *    445 is signed by a fresh key that is no account's, and the group relay
 *    only ever sees ephemeral AUTH keys;
 *  - member identity (W24 review H1): no KeyPackage lookup about a group
 *    member happens without the user asking (Verify), and none ever reaches
 *    a group relay -- only the discovery relays and the person's own write
 *    relays, which can't tie it to the group;
 *  - H7: no plaintext, group name or key canary in any file outside the
 *    encrypted store, raw (open and after close), nor in the logs
 *    (G_MESSAGES_DEBUG=all).
 * The accounts, relays and signer are mls-world.h's. */
#include "canary-scan.h"
#include "mls-world.h"

#include "nostr/nip59/nip59.h"

/* A NIP-17 kind-14 rumor from `from` to `to`, admitted to `app`'s model as a
 * relay would have delivered it: a message request (nobody on this account
 * wrote in the room, nobody accepted it). */
static void
receive_request(App *app, guint from, const gchar *text)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, hex[from]);
  nostr_event_set_created_at(rumor, g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_set_content(rumor, text);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", hex[app->key], NULL));
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  char *json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex[app->key], json, &error);
  free(json);
  g_assert_no_error(error);
  g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(sum, (const guchar *)text, -1);
  g_assert_cmpint(gh_conversation_store_admit(app->model, message, g_checksum_get_string(sum),
                                              &error), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
}

static void
try_invite(App *app, guint who, GError **out_error)
{
  const gchar *relays[] = { app->world->g.url, NULL };
  const gchar *people[] = { hex[who], NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(app->service, "Nope", NULL, relays, people, NULL,
                                    on_created, &wait);
  spin_until(op_done, &wait, "the invitation attempt");
  if (wait.result)
    g_object_unref(wait.result);
  *out_error = wait.error;
}

/* A KeyPackage lookup of pubkey: a REQ asking kind 30443 by it (the person's
 * own client never asks for its own KeyPackages). */
static gboolean
key_package_req(WireRelay *relay, const gchar *pubkey)
{
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, "30443") && strstr(frame->text, pubkey))
      return TRUE;
  }
  return FALSE;
}

/* Anything about pubkey from a client that is not pubkey's own: a REQ naming
 * it (as author or tag) on any relay. Only the stranger has no client. */
static gboolean
looked_up_anywhere(World *w, const gchar *pubkey, gboolean has_own_client)
{
  WireRelay *relays[] = { &w->e, &w->w, &w->x, &w->g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++)
    if (has_own_client ? key_package_req(relays[i], pubkey)
                       : client_frames_mention(relays[i], pubkey))
      return TRUE;
  return FALSE;
}

/* PT-8 for encrypted groups. */
static void
test_pt8_no_key_package_lookup_for_requests(void)
{
  World w;
  const guint keys[] = { ALICE, CAROL };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  spin_until(key_package_published, &w.apps[CAROL], "Carol's KeyPackage");
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  receive_request(alice, CAROL, "hi, add me to your group?");

  /* Neither the request's sender nor a stranger may be looked up. */
  g_autoptr(GError) error = NULL;
  try_invite(alice, CAROL, &error);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_CONSENT);
  g_clear_error(&error);
  try_invite(alice, STRANGER, &error);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_NO_CONSENT);
  g_clear_error(&error);
  g_assert_false(looked_up_anywhere(&w, hex[CAROL], TRUE));
  g_assert_false(looked_up_anywhere(&w, hex[STRANGER], FALSE));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(alice->service)), ==, 0);

  /* Accepting the request is the consent: now Carol is looked up (her
   * relay list on the discovery relay, then her KeyPackage on her write
   * relay only, nostrc-0bdg). */
  GListModel *rooms = G_LIST_MODEL(alice->model);
  for (guint i = 0; i < g_list_model_get_n_items(rooms); i++) {
    g_autoptr(GhConversation) room = g_list_model_get_item(rooms, i);
    if (gh_conversation_get_is_request(room))
      gh_conversation_accept(room);
  }
  const gchar *relays[] = { w.g.url, NULL };
  const gchar *people[] = { hex[CAROL], NULL };
  OpWait wait = { 0 };
  gh_mls_service_create_group_async(alice->service, "Now", NULL, relays, people, NULL,
                                    on_created, &wait);
  spin_until(op_done, &wait, "the consented invitation");
  g_assert_no_error(wait.error);
  g_object_unref(wait.result);
  g_assert_true(client_frames_mention(&w.e, hex[CAROL]));
  g_assert_false(key_package_req(&w.e, hex[CAROL]));
  g_assert_true(key_package_req(&w.w, hex[CAROL]));
  g_assert_false(looked_up_anywhere(&w, hex[STRANGER], FALSE));
  world_down(&w);
}

/* Welcomes only as gift wraps to the invitee's inbox; the group relay sees
 * only fresh keys. */
static void
test_welcome_only_as_gift_wrap(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  spin_until(key_package_published, alice, "Alice's KeyPackage");
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Wrapped", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  send_text(alice, ga, "one");
  wait_message(bob, gh_mls_group_get_room_id(gb), "one");
  send_text(bob, gb, "two");
  wait_message(alice, gh_mls_group_get_room_id(ga), "two");

  WireRelay *relays[] = { &w.e, &w.w, &w.x, &w.g };
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    g_autoptr(GPtrArray) bare = published(relays[i], 444);
    g_assert_cmpuint(bare->len, ==, 0);           /* never a bare Welcome */
  }
  /* The Welcome: kind 1059 on Bob's inbox X only, p-tagged Bob, signed by a
   * key that is no account's. */
  g_autoptr(GPtrArray) wraps = published(&w.x, 1059);
  g_assert_cmpuint(wraps->len, >=, 1);
  for (guint i = 0; i < wraps->len; i++) {
    NostrEvent *wrap = g_ptr_array_index(wraps, i);
    g_assert_false(is_account(nostr_event_get_pubkey(wrap)));
    char *recipient = nostr_nip59_get_recipient(wrap);
    g_assert_cmpstr(recipient, ==, hex[BOB]);
    free(recipient);
  }
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    if (relays[i] == &w.x)
      continue;
    g_autoptr(GPtrArray) elsewhere = published(relays[i], 1059);
    g_assert_cmpuint(elsewhere->len, ==, 0);
  }
  /* Group messages and Commits: G only, each under its own fresh key. */
  g_autoptr(GPtrArray) group_events = published(&w.g, 445);
  g_assert_cmpuint(group_events->len, >=, 3);   /* the Add, "one", "two" */
  g_autoptr(GHashTable) keys_seen = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  for (guint i = 0; i < group_events->len; i++) {
    NostrEvent *event = g_ptr_array_index(group_events, i);
    const gchar *pubkey = nostr_event_get_pubkey(event);
    g_assert_false(is_account(pubkey));
    /* A republished event is the same event; a new one never reuses a key. */
    gchar *id = event_id_dup(event);
    const gchar *earlier = g_hash_table_lookup(keys_seen, pubkey);
    g_assert_true(!earlier || g_str_equal(earlier, id));
    g_hash_table_insert(keys_seen, (gpointer)pubkey, id);   /* the table frees it */
  }
  for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
    if (relays[i] == &w.g)
      continue;
    g_autoptr(GPtrArray) elsewhere = published(relays[i], 445);
    g_assert_cmpuint(elsewhere->len, ==, 0);
  }
  /* §4.4 R1: the group relay demanded AUTH; nobody signed in as an account. */
  g_assert_cmpuint(w.g.auth_pubkeys->len, >, 0);
  for (guint i = 0; i < w.g.auth_pubkeys->len; i++)
    g_assert_false(is_account(g_ptr_array_index(w.g.auth_pubkeys, i)));
  world_down(&w);
}

static void
collect_files(const gchar *path, GPtrArray *out)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      collect_files(child, out);
    }
    if (dir)
      g_dir_close(dir);
  } else if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    g_ptr_array_add(out, g_strdup(path));
  }
}

/* H7: nothing readable outside the encrypted store, nor in the logs. */
static void
test_no_secrets_outside_the_store(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  accept_contact(alice, BOB);
  g_autofree gchar *uuid = g_uuid_string_random();
  g_autofree gchar *name = g_strdup_printf("canary-group-%.8s", uuid);
  g_autofree gchar *text = g_strdup_printf("canary-message-%.8s", uuid + 9);
  g_autofree gchar *reply = g_strdup_printf("canary-reply-%.8s", uuid + 24);
  GhMlsGroup *ga = create_group(alice, name, (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) sent = gh_mls_service_send(alice->service, ga, text, &error);
  g_assert_no_error(error);
  MessageWait got = { bob, gh_mls_group_get_room_id(gb), text };
  spin_until(message_listed, &got, "the canary message");
  g_autoptr(GhMessage) answer = gh_mls_service_send(bob->service, gb, reply, &error);
  g_assert_no_error(error);
  MessageWait back = { alice, gh_mls_group_get_room_id(ga), reply };
  spin_until(message_listed, &back, "the canary reply");

  CanaryScan *scan = canary_scan_new();
  canary_scan_add(scan, "group name", name);
  canary_scan_add(scan, "message", text);
  canary_scan_add(scan, "reply", reply);
  for (guint key = 1; key < GH_TEST_KEYS; key++)
    canary_scan_add(scan, "account secret key", gh_test_secret[key]);
  /* Open stores (with their -wal and -shm), then closed ones. */
  guint n_files = 0;
  canary_scan_tree(scan, w.root, &n_files);
  g_assert_cmpuint(n_files, >, 0);
  canary_scan_check_clean(scan, "the files of open stores");
  /* Every file an account wrote is its encrypted store's (store.db and its
   * -wal/-shm): libmarmot keeps no second file (charter §3.9 D5). */
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    App *app = &w.apps[keys[i]];
    g_autofree gchar *account_dir = gh_store_account_dir_name(hex[app->key]);
    g_autofree gchar *store_dir = g_build_filename(app->data_dir, "groundhog", "accounts",
                                                   account_dir, NULL);
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
    collect_files(app->data_dir, files);
    g_assert_cmpuint(files->len, >, 0);
    for (guint f = 0; f < files->len; f++) {
      const gchar *path = g_ptr_array_index(files, f);
      g_autofree gchar *dir = g_path_get_dirname(path);
      g_autofree gchar *base = g_path_get_basename(path);
      g_assert_cmpstr(dir, ==, store_dir);
      g_assert_true(g_str_equal(base, "store.db") || g_str_equal(base, "store.db-wal") ||
                    g_str_equal(base, "store.db-shm"));
    }
  }
  canary_log_capture_sync();
  g_autofree gchar *logs = canary_log_capture_dup();
  canary_scan_text(scan, "logs", logs);
  canary_scan_check_clean(scan, "the logs");
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
    app_store_down(&w.apps[keys[i]]);
  canary_scan_tree(scan, w.root, &n_files);
  canary_scan_check_clean(scan, "the files of closed stores");
  canary_scan_free(scan);
  world_down(&w);
}

/* REQs naming `key` for any kind a member lookup uses -- its relay list
 * (10002) or KeyPackages (443, 30443) -- that `relay` received (W24 review
 * A3: whatever kind a regression would fetch). */
static guint
key_package_reqs(WireRelay *relay, guint key)
{
  guint n = 0;
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, hex[key]) &&
        (strstr(frame->text, "10002") || strstr(frame->text, "443")))
      n++;
  }
  return n;
}

/* On either group relay of the world (g, and h, the second one). */
static guint
group_relay_reqs(World *w, guint key)
{
  return key_package_reqs(&w->g, key) + key_package_reqs(&w->h, key);
}

typedef struct {
  gboolean done;
  GhMlsMemberIdentity identity;
} VerifyWait;

static gboolean
verify_finished(gpointer data)
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

typedef struct {
  gboolean done;
  GError *error;
} RefusedWait;

static gboolean
refused_finished(gpointer data)
{
  return ((RefusedWait *)data)->done;
}

static void
on_refused(GObject *source, GAsyncResult *result, gpointer data)
{
  RefusedWait *wait = data;
  gh_mls_service_verify_member_finish(GH_MLS_SERVICE(source), result, &wait->error);
  wait->done = TRUE;
}

/* W24 review H1: Carol (an older app, no account proof) joins through
 * Alice's Add -- into an MDK 0.8-format group, the only one she can join, so
 * Alice and Bob publish that format only here (nostrc-lf62). Bob learns
 * nothing about her by himself: no KeyPackage REQ
 * naming her leaves Bob, and the group relay sees none from anyone. Only
 * Bob's Verify asks, and only the discovery relay and Carol's write relay;
 * a group relay that is also a discovery or write relay is still never
 * asked, and with nothing else to ask Verify refuses (W24 review A1, A3). */
static void
test_member_lookups_on_demand_only(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_legacy_only = TRUE;
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
    spin_until(key_package_published, &w.apps[keys[i]], "a KeyPackage published");
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Lookups", (const guint[]){ BOB }, 1);
  GhMlsGroup *gb = join(bob, ALICE);

  /* Carol's KeyPackage without the proof, on her write relay. */
  MarmotConfig config = marmot_config_default();
  config.allow_unproven_self = true;
  Marmot *legacy = marmot_new_with_config(marmot_storage_memory_new(), &config);
  guint8 pubkey[32];
  g_assert_true(nostr_hex2bin(pubkey, hex[CAROL], sizeof pubkey));
  const char *relays[] = { w.w.url };
  MarmotKeyPackageResult made;
  memset(&made, 0, sizeof made);
  g_assert_cmpint(marmot_create_key_package_unsigned(legacy, pubkey, relays, 1, &made), ==,
                  MARMOT_OK);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, made.event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[CAROL]), ==, 0);
  char *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  wire_relay_inject(&w.w, signed_json);
  free(signed_json);
  marmot_key_package_result_free(&made);
  marmot_free(legacy);

  accept_contact(alice, CAROL);
  const gchar *carol[] = { hex[CAROL], NULL };
  OpWait added = { 0 };
  gh_mls_service_add_members_async(alice->service, ga, carol, NULL, on_changed, &added);
  spin_until(op_done, &added, "Alice's Add of Carol");
  g_assert_no_error(added.error);
  guint asked = key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL);
  wait_members(gb, 3);
  wait_live(gb);
  drain();
  g_assert_cmpint(gh_mls_group_get_member_identity(gb, hex[CAROL], NULL), ==,
                  GH_MLS_MEMBER_UNVERIFIED);
  g_assert_cmpuint(key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL), ==, asked);
  g_assert_cmpuint(key_package_reqs(&w.x, CAROL) + group_relay_reqs(&w, CAROL), ==, 0);

  VerifyWait wait = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_verified, &wait);
  spin_until(verify_finished, &wait, "Bob's Verify");
  g_assert_cmpint(wait.identity, ==, GH_MLS_MEMBER_VERIFIED);
  g_assert_cmpuint(key_package_reqs(&w.e, CAROL), >, 0);
  g_assert_cmpuint(key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL), >, asked);
  g_assert_cmpuint(key_package_reqs(&w.x, CAROL) + group_relay_reqs(&w, CAROL), ==, 0);

  /* A1: the group relay is also a discovery relay and one of Carol's write
   * relays. Verify asks the others, never it, in either phase. */
  const gchar *discovery[] = { w.e.url, w.g.url, NULL };
  g_settings_set_strv(bob->settings, "discovery-relays", discovery);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", w.w.url, NULL));
  nostr_tags_append(tags, nostr_tag_new("r", w.g.url, NULL));
  g_autofree gchar *list = sign_event(CAROL, 10002, g_get_real_time() / G_USEC_PER_SEC - 60, "",
                                      tags);
  wire_relay_inject(&w.e, list);
  guint on_w = key_package_reqs(&w.w, CAROL);
  VerifyWait again = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_verified, &again);
  spin_until(verify_finished, &again, "Bob's Verify with overlapping relays");
  g_assert_cmpint(again.identity, ==, GH_MLS_MEMBER_VERIFIED);
  g_assert_cmpuint(key_package_reqs(&w.w, CAROL), >, on_w);   /* phase 2 ran */
  g_assert_cmpuint(group_relay_reqs(&w, CAROL), ==, 0);

  /* Nothing but the group relay to ask: refused, nothing sent. */
  const gchar *only_group[] = { w.g.url, NULL };
  g_settings_set_strv(bob->settings, "discovery-relays", only_group);
  guint before = key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL);
  RefusedWait refused = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_refused, &refused);
  spin_until(refused_finished, &refused, "Bob's refused Verify");
  g_assert_error(refused.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&refused.error);
  g_assert_cmpuint(key_package_reqs(&w.e, CAROL) + key_package_reqs(&w.w, CAROL), ==, before);
  g_assert_cmpuint(group_relay_reqs(&w, CAROL), ==, 0);
  world_down(&w);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  canary_log_capture_install();
  mls_world_init();
  g_test_add_func("/groundhog/privacy-mls/pt8-no-key-package-lookup-for-requests",
                  test_pt8_no_key_package_lookup_for_requests);
  g_test_add_func("/groundhog/privacy-mls/welcome-only-as-gift-wrap",
                  test_welcome_only_as_gift_wrap);
  g_test_add_func("/groundhog/privacy-mls/member-lookups-on-demand-only",
                  test_member_lookups_on_demand_only);
  g_test_add_func("/groundhog/privacy-mls/no-secrets-outside-the-store",
                  test_no_secrets_outside_the_store);
  gint rc = g_test_run();
  mls_world_finish();
  canary_log_capture_uninstall();
  return rc;
}
