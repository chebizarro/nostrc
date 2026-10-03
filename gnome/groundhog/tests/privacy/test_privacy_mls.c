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
 *    (G_MESSAGES_DEBUG=all);
 *  - inside the store (W25 slice N re-review N2): once Groundhog purges a
 *    message -- disappearing-message expiry, the retention window, forget
 *    conversation, after leaving the group -- its text is nowhere in the
 *    store's pages, decrypted: libmarmot's message rows keep no plaintext
 *    (gh-store-marmot.h).
 * The accounts, relays and signer are mls-world.h's. */
#include "canary-scan.h"
#include "mls-world.h"

#include "gh-store-mls.h"

#include <openssl/evp.h>

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

/* REQs naming `key` in any filter (as author or #p tag) that `relay`
 * received.  Counts any kind, not just the ones a member lookup uses
 * today: a regression that adds an automatic kind-0 fetch would also be
 * caught (W24 review N1). */
static guint
member_naming_reqs(WireRelay *relay, guint key)
{
  guint n = 0;
  for (guint i = 0; i < relay->frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay->frames, i);
    if (frame->inbound && g_str_has_prefix(frame->text, "[\"REQ\"") &&
        strstr(frame->text, hex[key]))
      n++;
  }
  return n;
}

/* On either group relay of the world (g, and h, the second one). */
static guint
group_relay_reqs(World *w, guint key)
{
  return member_naming_reqs(&w->g, key) + member_naming_reqs(&w->h, key);
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
  /* Drain deferred callbacks from Alice's Add before snapshotting: the
   * broadened counter may catch an asynchronous REQ that the kind-specific
   * counter missed, producing a race under the sanitizer gate. */
  drain();
  guint asked = member_naming_reqs(&w.e, CAROL) + member_naming_reqs(&w.w, CAROL);
  wait_members(gb, 3);
  wait_live(gb);
  drain();
  g_assert_cmpint(gh_mls_group_get_member_identity(gb, hex[CAROL], NULL), ==,
                  GH_MLS_MEMBER_UNVERIFIED);
  g_assert_cmpuint(member_naming_reqs(&w.e, CAROL) + member_naming_reqs(&w.w, CAROL), ==, asked);
  g_assert_cmpuint(member_naming_reqs(&w.x, CAROL) + group_relay_reqs(&w, CAROL), ==, 0);

  VerifyWait wait = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_verified, &wait);
  spin_until(verify_finished, &wait, "Bob's Verify");
  g_assert_cmpint(wait.identity, ==, GH_MLS_MEMBER_VERIFIED);
  g_assert_cmpuint(member_naming_reqs(&w.e, CAROL), >, 0);
  g_assert_cmpuint(member_naming_reqs(&w.e, CAROL) + member_naming_reqs(&w.w, CAROL), >, asked);
  g_assert_cmpuint(member_naming_reqs(&w.x, CAROL) + group_relay_reqs(&w, CAROL), ==, 0);

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
  guint on_w = member_naming_reqs(&w.w, CAROL);
  VerifyWait again = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_verified, &again);
  spin_until(verify_finished, &again, "Bob's Verify with overlapping relays");
  g_assert_cmpint(again.identity, ==, GH_MLS_MEMBER_VERIFIED);
  g_assert_cmpuint(member_naming_reqs(&w.w, CAROL), >, on_w);   /* phase 2 ran */
  g_assert_cmpuint(group_relay_reqs(&w, CAROL), ==, 0);

  /* Nothing but the group relay to ask: refused, nothing sent. */
  const gchar *only_group[] = { w.g.url, NULL };
  g_settings_set_strv(bob->settings, "discovery-relays", only_group);
  guint before = member_naming_reqs(&w.e, CAROL) + member_naming_reqs(&w.w, CAROL);
  RefusedWait refused = { 0 };
  gh_mls_service_verify_member_async(bob->service, gb, hex[CAROL], NULL, on_refused, &refused);
  spin_until(refused_finished, &refused, "Bob's refused Verify");
  g_assert_error(refused.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&refused.error);
  g_assert_cmpuint(member_naming_reqs(&w.e, CAROL) + member_naming_reqs(&w.w, CAROL), ==, before);
  g_assert_cmpuint(group_relay_reqs(&w, CAROL), ==, 0);
  world_down(&w);
}

/* ---- Inside the store: a purged message's text is gone (N2) -------------------------- */

#define PAGE_SIZE 4096
#define PAGE_RESERVE 80 /* SQLCipher 4: 16-byte IV + 64-byte HMAC-SHA512 */
#define WAL_HEADER 32
#define WAL_FRAME_HEADER 24

static guint32
be32(const guint8 *p)
{
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

/* Every page of path (store.db, or with wal the frames of its WAL),
 * decrypted with the account's store key as it is on disk (live and free
 * pages and old WAL frames alike), into scan (as in test_media_purge.c). */
static guint
scan_decrypted_file(const guint8 *key, CanaryScan *scan, const gchar *path, gboolean wal)
{
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL))
    return 0;
  const gsize header = wal ? WAL_HEADER : 0, frame = wal ? WAL_FRAME_HEADER : 0;
  guint pages = 0;
  for (gsize at = header; at + frame + PAGE_SIZE <= length; at += frame + PAGE_SIZE) {
    const guint8 *page = (const guint8 *)contents + at + frame;
    guint32 pgno = wal ? be32((const guint8 *)contents + at) : (guint32)(at / PAGE_SIZE) + 1;
    const gsize start = pgno == 1 ? 16 : 0; /* page 1 begins with the plaintext salt */
    guint8 out[PAGE_SIZE] = { 0 };
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, last = 0;
    g_assert_cmpint(EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key,
                                       page + PAGE_SIZE - PAGE_RESERVE), ==, 1);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    g_assert_cmpint(EVP_DecryptUpdate(ctx, out + start, &n, page + start,
                                      (int)(PAGE_SIZE - PAGE_RESERVE - start)), ==, 1);
    g_assert_cmpint(EVP_DecryptFinal_ex(ctx, out + start + n, &last), ==, 1);
    EVP_CIPHER_CTX_free(ctx);
    if (pgno == 1) {   /* the decryption is right: a SQLite header */
      g_assert_cmpuint(((guint)out[16] << 8) | out[17], ==, PAGE_SIZE);
      g_assert_cmpuint(out[20], ==, PAGE_RESERVE);
    }
    g_autofree gchar *source = g_strdup_printf("%s page %u (decrypted)", wal ? "-wal" : "store.db",
                                               pgno);
    canary_scan_bytes(scan, source, out, PAGE_SIZE - PAGE_RESERVE);
    pages++;
  }
  g_free(contents);
  return pages;
}

/* Whether `text` is in app's store, decrypted (store.db and its WAL). */
static gboolean
in_store_pages(App *app, const gchar *text)
{
  guint8 key[GH_STORE_KEY_SIZE];
  app_store_key(app, key);
  CanaryScan *scan = canary_scan_new();
  canary_scan_add_literal(scan, text, text);
  const gchar *path = gh_store_get_path(app->store);
  g_assert_cmpuint(scan_decrypted_file(key, scan, path, FALSE), >, 0);
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  scan_decrypted_file(key, scan, wal, TRUE);
  gboolean found = canary_scan_get_hits(scan)->len > 0;
  canary_scan_free(scan);
  return found;
}

/* The group's id, as bytes. */
static MarmotGroupId
gid_of_group(GhMlsGroup *group)
{
  const gchar *hex_id = gh_mls_group_get_group_id(group);
  gsize len = strlen(hex_id) / 2;
  g_autofree guint8 *bytes = g_malloc(len);
  g_assert_true(nostr_hex2bin(bytes, hex_id, len));
  return marmot_group_id_new(bytes, len);
}

/* `from` sends `text` to the group with a NIP-40 expiration at `expires_at`
 * (Groundhog sends none; another client can): libmarmot directly, published
 * on G. */
static void
send_expiring(World *w, App *from, GhMlsGroup *group, const gchar *text, gint64 expires_at)
{
  g_autofree gchar *inner = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[[\"expiration\",\"%" G_GINT64_FORMAT "\"]],\"content\":\"%s\"}",
    hex[from->key], g_get_real_time() / G_USEC_PER_SEC, expires_at, text);
  MarmotGroupId gid = gid_of_group(group);
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  g_assert_cmpint(marmot_create_message(gh_mls_service_get_marmot(from->service), &gid, inner,
                                        &out), ==, MARMOT_OK);
  wire_relay_inject(&w->g, out.event_json);
  marmot_outgoing_message_free(&out);
  marmot_group_id_free(&gid);
}

static gint64
store_conversation(App *app, GhMlsGroup *group)
{
  gint64 id = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_mls_save_room(app->store, gh_mls_group_get_group_id(group), NULL, &id,
                                       &error));
  g_assert_no_error(error);
  return id;
}

/* A restart closes the store: its WAL is checkpointed into store.db (whose
 * freed pages secure_delete zeroed) and goes. */
static GhMlsGroup *
restart_app(App *app, const gchar *room)
{
  app_restart(app);
  GhMlsGroup *group = gh_mls_service_lookup(app->service, room);
  g_assert_nonnull(group);
  return group;
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

/* N2: Alice's purges -- a disappearing message she received expires; the
 * retention window passes over what she sent and received; she forgets the
 * conversation. Each time the purged text is in no page of her store,
 * decrypted, while what remains still is (the controls). */
static void
test_purged_text_leaves_the_store(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Purged", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  wait_live(ga);

  /* Expiry: a received disappearing message. */
  const gchar *expiring = "N2-CANARY-received-disappearing-7f3a";
  const gchar *kept_in = "N2-CANARY-received-kept-1c9e";
  const gchar *kept_out = "N2-CANARY-sent-kept-5b2d";
  send_expiring(&w, bob, gb, expiring, real_now() + 3);
  wait_message(alice, room, "N2-CANARY-received-disappearing-7f3a");
  send_text(bob, gb, kept_in);
  wait_message(alice, room, "N2-CANARY-received-kept-1c9e");
  send_text(alice, ga, kept_out);
  StatusWait out_sent = { alice, room, kept_out };
  spin_until(sent, &out_sent, "Alice's message sent");
  ga = restart_app(alice, room);
  g_assert_true(in_store_pages(alice, expiring));   /* the scan finds what is there */
  g_usleep(4 * G_USEC_PER_SEC);
  GhStorePurgeStats stats = { 0 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_purge(alice->store, 0, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_expired, ==, 1);
  ga = restart_app(alice, room);
  g_assert_false(in_store_pages(alice, expiring));
  g_assert_true(in_store_pages(alice, kept_in));
  g_assert_true(in_store_pages(alice, kept_out));

  /* Retention: everything received before the cutoff, sent or received. */
  g_usleep(1100 * 1000);
  g_assert_true(gh_store_purge(alice->store, real_now(), &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_retention, >=, 2);
  g_usleep(1100 * 1000);
  const gchar *forget_in = "N2-CANARY-received-forgotten-3e81";
  const gchar *forget_out = "N2-CANARY-sent-forgotten-a64f";
  send_text(bob, gb, forget_in);
  wait_message(alice, room, "N2-CANARY-received-forgotten-3e81");
  send_text(alice, ga, forget_out);
  StatusWait forget_sent = { alice, room, forget_out };
  spin_until(sent, &forget_sent, "Alice's second message sent");
  ga = restart_app(alice, room);
  g_assert_false(in_store_pages(alice, kept_in));
  g_assert_false(in_store_pages(alice, kept_out));
  g_assert_true(in_store_pages(alice, forget_in));
  g_assert_true(in_store_pages(alice, forget_out));

  /* Forget conversation. */
  g_assert_true(gh_store_forget_conversation(alice->store, store_conversation(alice, ga), &error));
  g_assert_no_error(error);
  ga = restart_app(alice, room);
  g_assert_false(in_store_pages(alice, forget_in));
  g_assert_false(in_store_pages(alice, forget_out));
  g_assert_false(in_store_pages(alice, expiring));
  world_down(&w);
}

static gboolean
group_left(gpointer data)
{
  return gh_mls_group_get_end(data) == GH_MLS_GROUP_END_LEFT;
}

/* N2: Bob leaves the group (Alice commits his SelfRemove) and then forgets
 * its conversation: what he sent and received in it is in no page of his
 * store, decrypted. */
static void
test_left_group_text_leaves_the_store(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  spin_until(key_package_published, bob, "Bob's KeyPackage");
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Left", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  const gchar *to_bob = "N2-CANARY-received-before-leaving-90d1";
  const gchar *from_bob = "N2-CANARY-sent-before-leaving-2ac7";
  send_text(alice, ga, to_bob);
  wait_message(bob, room, "N2-CANARY-received-before-leaving-90d1");
  send_text(bob, gb, from_bob);
  wait_message(alice, room, "N2-CANARY-sent-before-leaving-2ac7");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_leave(bob->service, gb, &error));
  g_assert_no_error(error);
  spin_until(group_left, gb, "Bob's leave committed");
  gb = restart_app(bob, room);
  g_assert_true(in_store_pages(bob, to_bob));   /* the messages stay until forgotten */
  g_assert_true(gh_store_forget_conversation(bob->store, store_conversation(bob, gb), &error));
  g_assert_no_error(error);
  restart_app(bob, room);
  g_assert_false(in_store_pages(bob, to_bob));
  g_assert_false(in_store_pages(bob, from_bob));
  g_assert_true(in_store_pages(alice, to_bob));   /* control: Alice keeps hers */
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
  g_test_add_func("/groundhog/privacy-mls/purged-text-leaves-the-store",
                  test_purged_text_leaves_the_store);
  g_test_add_func("/groundhog/privacy-mls/left-group-text-leaves-the-store",
                  test_left_group_text_leaves_the_store);
  gint rc = g_test_run();
  mls_world_finish();
  canary_log_capture_uninstall();
  return rc;
}
