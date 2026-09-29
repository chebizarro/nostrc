/* G22 attachments, without a display (privacy charter §6, D6, §9.2 AT-7 and
 * AT-8 at the service, nostrc-dnsc): the attachment server list's rules
 * (add, remove, order), the per-server upload consent kept per account in
 * the encrypted store and loaded into the client (a restart keeps it for
 * that account only; revoked means the account signer is not asked again),
 * GhAttachments' downloads only on request from the encrypted cache or the
 * local Blossom fixture (tests/media/blossom-fixture.c), Cancel, the
 * held-bytes bound, Clear, the sender's own file, the upload's server, the
 * words for every failure, and the card's type words. Every wait spins the
 * main context until a condition holds; nothing sleeps. The process runs
 * with private XDG directories and TMPDIR under one root, which must hold no
 * transient file at the end (P3, AT-5). */
#include "gh-attachment-card.h"
#include "gh-attachments.h"
#include "gh-preferences-dialog.h"
#include "gh-store-blossom.h"
#include "gh-store-media.h"
#include "blossom-fixture.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <stdlib.h>
#include <string.h>

static gchar *root;
static const gchar *const xdg_names[] = { "data", "config", "cache", "runtime", "state", "tmp" };
static const gchar *const xdg_vars[] = { "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                         "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "TMPDIR" };
static const gchar *const alice =
  "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const gchar *const bob_pubkey =
  "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

typedef gboolean (*Cond)(gpointer data);

static void
spin_until(Cond cond, gpointer data)
{
  while (!cond(data))
    g_main_context_iteration(NULL, TRUE);
}

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static gchar *
root_dir(const gchar *name)
{
  return g_build_filename(root, name, NULL);
}

/* A JPEG of about size bytes (APP0, an APP1 with a GPS text, a 4x3 frame and
 * entropy data without markers): one Groundhog strips and may preview. */
static GBytes *
make_jpeg(gsize size, guint32 seed)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00 };
  g_byte_array_append(out, head, sizeof head);
  static const gchar exif[] = "Exif\0\0MM GPSInfo 47.3769N 8.5417E";
  guint8 app1[4] = { 0xFF, 0xE1, 0x00, (guint8)(sizeof exif - 1 + 2) };
  g_byte_array_append(out, app1, 4);
  g_byte_array_append(out, (const guint8 *)exif, sizeof exif - 1);
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x03, 0x00, 0x04, 1, 1, 0x11,
                                0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  while (out->len + 2 < size) {
    seed = seed * 1103515245u + 12345u;
    guint8 byte = (guint8)(seed >> 16);
    if (byte == 0xFF)
      byte = 0xFE;
    g_byte_array_append(out, &byte, 1);
  }
  static const guint8 eoi[] = { 0xFF, 0xD9 };
  g_byte_array_append(out, eoi, 2);
  return g_byte_array_free_to_bytes(out);
}

/* ---- the account signer (AT-6) ----------------------------------------------------- */

typedef struct {
  gchar *secret;
  gchar *pubkey;
  guint calls;
} FakeSigner;

static void
fake_sign_async(gpointer data, const gchar *unsigned_json, GCancellable *cancellable,
                GAsyncReadyCallback callback, gpointer callback_data)
{
  FakeSigner *signer = data;
  signer->calls++;
  GTask *task = g_task_new(NULL, cancellable, callback, callback_data);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, unsigned_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, GH_BLOSSOM_AUTH_KIND);
  g_assert_cmpint(nostr_event_sign(event, signer->secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_task_return_pointer(task, g_strdup(json), g_free);
  free(json);
  g_object_unref(task);
}

static gchar *
fake_sign_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_signer_init(FakeSigner *signer)
{
  char *secret = nostr_key_generate_private();
  char *pubkey = nostr_key_get_public(secret);
  signer->secret = g_strdup(secret);
  signer->pubkey = g_strdup(pubkey);
  free(secret);
  free(pubkey);
  signer->calls = 0;
}

static void
fake_signer_clear(FakeSigner *signer)
{
  g_free(signer->secret);
  g_free(signer->pubkey);
}

/* ---- stores ------------------------------------------------------------------------- */

typedef struct {
  gchar *account;
  GBytes *key;
  gchar *store_id;
  gchar *dir;
  GhStore *store;
} Account;

/* The account's encrypted store: created on the first open, then reopened
 * with the same key (a restart). */
static GhStore *
account_open(Account *account)
{
  g_assert_null(account->store);
  if (!account->key) {
    guint8 raw[32];
    for (guint i = 0; i < sizeof raw; i++)
      raw[i] = (guint8)g_random_int();
    account->key = g_bytes_new(raw, sizeof raw);
    account->store_id = g_uuid_string_random();
    account->dir = root_dir("data");
  }
  GhStoreConfig config = { .data_dir = account->dir, .account_pubkey = account->account };
  g_autoptr(GError) error = NULL;
  account->store = gh_store_open_with_key(&config, account->key, account->store_id,
                                          GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  return account->store;
}

static void
account_close(Account *account)
{
  g_clear_pointer(&account->store, gh_store_close);
}

static void
account_clear(Account *account)
{
  account_close(account);
  if (account->dir)
    g_assert_true(gh_store_delete_files(account->dir, account->account, NULL));
  g_free(account->account);
  g_clear_pointer(&account->key, g_bytes_unref);
  g_free(account->store_id);
  g_free(account->dir);
}

/* Stores the kind-15 message from sender to recipient carrying file (the
 * cache keeps only files a stored message names) and returns it as the
 * account sees it. */
static GhMessage *
admit_file(GhStore *store, const gchar *account, const gchar *sender, const gchar *recipient,
           const GhNip17File *file)
{
  static gint64 created_at = 1700000000;
  created_at++;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = NULL;
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(sender, recipient, file, created_at, 0, &id,
                                                    &error);
  g_assert_no_error(error);
  g_autofree gchar *room = strcmp(sender, recipient) < 0
                             ? g_strconcat(sender, ",", recipient, NULL)
                             : g_strconcat(recipient, ",", sender, NULL);
  g_autofree gchar *wrap = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  const gchar *participants[] = { sender, recipient, NULL };
  gboolean own = g_str_equal(sender, account);
  GhStoreMessage admitted = {
    .backend = GH_STORE_BACKEND_NIP17, .backend_key = room, .backend_msg_id = id,
    .wrap_id = wrap, .sender_pubkey = sender, .kind = GH_NIP17_FILE_KIND,
    .created_at = created_at,
    .direction = own ? GH_STORE_DIRECTION_OUT : GH_STORE_DIRECTION_IN, .body = file->url,
    .raw_json = rumor, .participants = participants,
  };
  g_assert_true(gh_store_admit(store, &admitted, NULL, NULL, &error));
  g_assert_no_error(error);
  GhMessage *message = gh_message_new_from_rumor(account, rumor, &error);
  g_assert_no_error(error);
  return message;
}

/* ---- the service ----------------------------------------------------------------------- */

typedef struct {
  GSettings *settings;
  GhNetHttp *http;
  BlossomFixture *blossom;
  FakeSigner signer;
  GhAttachments *attachments;
} Fixture;

static void
fixture_up(Fixture *f, gboolean with_signer)
{
  memset(f, 0, sizeof *f);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "network-mode", "none");
  f->blossom = blossom_fixture_new();
  const gchar *servers[] = { blossom_fixture_url(f->blossom), NULL };
  g_settings_set_strv(f->settings, "blossom-servers", servers);
  f->http = gh_net_http_new(f->settings);
  fake_signer_init(&f->signer);
  GhAttachmentsConfig config = {
    .settings = f->settings,
    .http = f->http,
    .sign_async = with_signer ? fake_sign_async : NULL,
    .sign_finish = with_signer ? fake_sign_finish : NULL,
    .sign_data = &f->signer,
  };
  f->attachments = gh_attachments_new(&config);
  /* The fixture is on loopback, which real downloads may not reach. */
  gh_attachments_set_allow_private_hosts(f->attachments, TRUE);
}

static void
fixture_down(Fixture *f)
{
  g_object_run_dispose(G_OBJECT(f->attachments));
  g_clear_object(&f->attachments);
  g_clear_object(&f->http);
  drain();
  blossom_fixture_free(f->blossom);
  fake_signer_clear(&f->signer);
  g_settings_reset(f->settings, "network-mode");
  g_settings_reset(f->settings, "blossom-servers");
  g_clear_object(&f->settings);
}

typedef struct {
  gboolean done;
  GhNip17File *file;
  gchar *server;
  GError *error;
} Upload;

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Upload *u = data;
  u->file = gh_attachments_upload_finish(GH_ATTACHMENTS(source), result, &u->server, &u->error);
  u->done = TRUE;
}

static gboolean
upload_done(gpointer data)
{
  return ((Upload *)data)->done;
}

static void
upload_clear(Upload *u)
{
  gh_nip17_file_free(u->file);
  g_free(u->server);
  g_clear_error(&u->error);
  memset(u, 0, sizeof *u);
}

static void
upload(Fixture *f, GBytes *file, Upload *u)
{
  upload_clear(u);
  gh_attachments_upload_async(f->attachments, file, "image/jpeg", NULL, on_uploaded, u);
  spin_until(upload_done, u);
}

static gboolean
not_downloading(gpointer data)
{
  return gh_attachment_transfer_get_state(data) != GH_ATTACHMENT_STATE_DOWNLOADING;
}

static gboolean
held_one(gpointer data)
{
  return blossom_fixture_held(data) >= 1;
}

/* ---- the server list (Preferences › Attachments, the first use) ----------------------- */

static void
assert_list(GStrv have, const gchar *const *want)
{
  if (!g_strv_equal((const gchar *const *)have, want)) {
    g_autofree gchar *joined = g_strjoinv(" ", have);
    g_error("the list is [%s]", joined);
  }
}

static void
test_server_list(void)
{
  g_autoptr(GError) error = NULL;
  /* Added normalized; the first one is where an empty list starts. */
  g_auto(GStrv) one = gh_preferences_server_list_add(NULL, " HTTPS://Blossom.Example.COM/ ",
                                                      FALSE, &error);
  g_assert_no_error(error);
  assert_list(one, (const gchar *const[]){ "https://blossom.example.com", NULL });
  g_auto(GStrv) two = gh_preferences_server_list_add((const gchar *const *)one,
                                                     "https://files.example.org:8443/media/",
                                                     FALSE, &error);
  g_assert_no_error(error);
  assert_list(two, (const gchar *const[]){ "https://blossom.example.com",
                                           "https://files.example.org:8443/media", NULL });
  /* The same server in another spelling is already there. */
  g_assert_null(gh_preferences_server_list_add((const gchar *const *)two,
                                               "https://BLOSSOM.example.com/", FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error(&error);
  /* Never a guess: only https, no credentials, query or fragment; a .onion
   * over http only where Tor is available. */
  static const gchar *const bad[] = {
    "blossom.example.com", "http://blossom.example.com", "http://127.0.0.1:3000",
    "wss://blossom.example.com", "https://user:pw@blossom.example.com",
    "https://blossom.example.com/?x=1", "https://blossom.example.com/#top", "https://", "",
    "http://abcdefgh.onion", NULL,
  };
  for (guint i = 0; bad[i]; i++) {
    g_assert_null(gh_preferences_server_list_add((const gchar *const *)two, bad[i], FALSE,
                                                 &error));
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
      g_error("\"%s\" was not refused as invalid", bad[i]);
    g_assert_cmpstr(error->message, !=, "");
    g_clear_error(&error);
  }
  g_auto(GStrv) onion = gh_preferences_server_list_add((const gchar *const *)two,
                                                       "http://abcdefgh.onion", TRUE, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(onion[2], ==, "http://abcdefgh.onion");
  /* At most 16. */
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; i < 16; i++) {
    g_autofree gchar *url = g_strdup_printf("https://blossom.example.com/%u", i);
    g_strv_builder_add(builder, url);
  }
  g_auto(GStrv) full = g_strv_builder_end(builder);
  g_assert_null(gh_preferences_server_list_add((const gchar *const *)full,
                                               "https://more.example.com", FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_clear_error(&error);

  /* Order: tried first to last; a move swaps neighbours, out of range is
   * no change. */
  g_auto(GStrv) down = gh_preferences_server_list_move((const gchar *const *)onion, 0, 1);
  assert_list(down, (const gchar *const[]){ "https://files.example.org:8443/media",
                                            "https://blossom.example.com",
                                            "http://abcdefgh.onion", NULL });
  g_auto(GStrv) up = gh_preferences_server_list_move((const gchar *const *)down, 2, -1);
  assert_list(up, (const gchar *const[]){ "https://files.example.org:8443/media",
                                          "http://abcdefgh.onion",
                                          "https://blossom.example.com", NULL });
  g_auto(GStrv) same_top = gh_preferences_server_list_move((const gchar *const *)up, 0, -1);
  assert_list(same_top, (const gchar *const *)up);
  g_auto(GStrv) same_end = gh_preferences_server_list_move((const gchar *const *)up, 2, 1);
  assert_list(same_end, (const gchar *const *)up);
  g_auto(GStrv) same_index = gh_preferences_server_list_move((const gchar *const *)up, 7, -1);
  assert_list(same_index, (const gchar *const *)up);
  g_auto(GStrv) none = gh_preferences_server_list_move(NULL, 0, 1);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  /* Remove: exactly that entry. */
  g_auto(GStrv) removed = gh_preferences_server_list_remove((const gchar *const *)up,
                                                            "http://abcdefgh.onion");
  assert_list(removed, (const gchar *const[]){ "https://files.example.org:8443/media",
                                               "https://blossom.example.com", NULL });
  g_auto(GStrv) kept = gh_preferences_server_list_remove((const gchar *const *)removed,
                                                         "https://nowhere.example.com");
  assert_list(kept, (const gchar *const *)removed);
}

/* ---- consent (nostrc-dnsc) ----------------------------------------------------------- */

/* The store keeps a consent per server, for its account only, across a
 * restart; a revocation deletes it. */
static void
test_consent_store(void)
{
  Account a = { .account = g_strdup(alice) };
  Account b = { .account = g_strdup(bob_pubkey) };
  g_autoptr(GError) error = NULL;
  GhStore *store = account_open(&a);
  g_auto(GStrv) empty = gh_store_blossom_dup_consents(store, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(g_strv_length(empty), ==, 0);
  g_assert_true(gh_store_blossom_set_consent(store, "https://b.example.com", TRUE, 1700000000,
                                             &error));
  g_assert_true(gh_store_blossom_set_consent(store, "https://a.example.com", TRUE, 1700000001,
                                             &error));
  /* Twice is once. */
  g_assert_true(gh_store_blossom_set_consent(store, "https://a.example.com", TRUE, 1700000002,
                                             &error));
  g_assert_no_error(error);
  /* Not a server: refused. */
  static const gchar *const bad[] = { "", "blossom.example.com", "https://a.example.com/\n",
                                      "ftp://a.example.com", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_assert_false(gh_store_blossom_set_consent(store, bad[i], TRUE, 1700000000, &error));
    g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
    g_clear_error(&error);
  }
  account_close(&a);

  /* A restart keeps it, for this account. */
  store = account_open(&a);
  g_auto(GStrv) kept = gh_store_blossom_dup_consents(store, &error);
  g_assert_no_error(error);
  assert_list(kept, (const gchar *const[]){ "https://a.example.com", "https://b.example.com",
                                            NULL });
  /* Another account's store has none of them. */
  g_auto(GStrv) other = gh_store_blossom_dup_consents(account_open(&b), &error);
  g_assert_no_error(error);
  g_assert_cmpuint(g_strv_length(other), ==, 0);
  account_close(&b);

  /* Revoked: gone, also after a restart; revoking twice is harmless. */
  g_assert_true(gh_store_blossom_set_consent(store, "https://a.example.com", FALSE, 0, &error));
  g_assert_true(gh_store_blossom_set_consent(store, "https://a.example.com", FALSE, 0, &error));
  g_assert_no_error(error);
  account_close(&a);
  store = account_open(&a);
  g_auto(GStrv) after = gh_store_blossom_dup_consents(store, &error);
  g_assert_no_error(error);
  assert_list(after, (const gchar *const[]){ "https://b.example.com", NULL });
  account_clear(&a);
  account_clear(&b);
}

/* The service: without consent the account signer is never asked and a
 * server that wants a known account ends the upload, naming itself; with
 * it the account signs; the consent survives a restart for that account
 * only, and once revoked the signer is not asked again (AT-6, nostrc-dnsc). */
static void
test_consent_service(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  Account a = { .account = g_strdup(f.signer.pubkey) };
  Account b = { .account = g_strdup(alice) };
  blossom_fixture_require_pubkey(f.blossom, f.signer.pubkey);
  g_autoptr(GBytes) jpeg = make_jpeg(8192, 1);
  g_autofree gchar *server = gh_blossom_client_normalize_server(blossom_fixture_url(f.blossom),
                                                                NULL);
  Upload u = { 0 };

  gh_attachments_set_store(f.attachments, account_open(&a));
  upload(&f, jpeg, &u);
  g_assert_error(u.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpstr(u.server, ==, server);
  g_assert_cmpuint(f.signer.calls, ==, 0);
  g_assert_false(gh_attachments_get_consent(f.attachments, server));

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_attachments_set_consent(f.attachments, blossom_fixture_url(f.blossom), TRUE,
                                           &error));
  g_assert_no_error(error);
  upload(&f, jpeg, &u);
  g_assert_no_error(u.error);
  g_assert_nonnull(u.file);
  g_assert_cmpstr(u.server, ==, server);
  g_assert_cmpuint(f.signer.calls, ==, 1);
  GPtrArray *requests = blossom_fixture_requests(f.blossom);
  BlossomRequest *put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(put->auth_pubkey, ==, f.signer.pubkey);
  g_auto(GStrv) listed = gh_attachments_dup_consents(f.attachments);
  assert_list(listed, (const gchar *const[]){ server, NULL });

  /* A restart: the store closes and opens again; the consent is back. */
  gh_attachments_set_store(f.attachments, NULL);
  account_close(&a);
  gh_attachments_set_store(f.attachments, account_open(&a));
  g_assert_true(gh_attachments_get_consent(f.attachments, server));
  upload(&f, jpeg, &u);
  g_assert_no_error(u.error);
  g_assert_cmpuint(f.signer.calls, ==, 2);

  /* Another account: no consent, no signer call. */
  gh_attachments_set_store(f.attachments, NULL);
  account_close(&a);
  gh_attachments_set_store(f.attachments, account_open(&b));
  g_assert_false(gh_attachments_get_consent(f.attachments, server));
  upload(&f, jpeg, &u);
  g_assert_error(u.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(f.signer.calls, ==, 2);
  gh_attachments_set_store(f.attachments, NULL);
  account_close(&b);

  /* Revoked: the signer is not asked again, also after a restart. */
  gh_attachments_set_store(f.attachments, account_open(&a));
  g_assert_true(gh_attachments_set_consent(f.attachments, server, FALSE, &error));
  g_assert_no_error(error);
  upload(&f, jpeg, &u);
  g_assert_error(u.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(f.signer.calls, ==, 2);
  gh_attachments_set_store(f.attachments, NULL);
  account_close(&a);
  gh_attachments_set_store(f.attachments, account_open(&a));
  g_assert_false(gh_attachments_get_consent(f.attachments, server));
  g_auto(GStrv) none = gh_attachments_dup_consents(f.attachments);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  upload(&f, jpeg, &u);
  g_assert_error(u.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(f.signer.calls, ==, 2);

  /* Not a server address: refused, nothing kept. */
  g_assert_false(gh_attachments_set_consent(f.attachments, "not a server", TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  upload_clear(&u);
  gh_attachments_set_store(f.attachments, NULL);
  account_clear(&a);
  account_clear(&b);
  fixture_down(&f);
}

/* ---- received files --------------------------------------------------------------------- */

/* AT-7 at the service: looking a file up fetches nothing; Download does,
 * once; the next session gets it from the encrypted cache; Clear empties
 * the cache and returns the transfer to IDLE. */
static void
test_download_on_request(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  Account bob = { .account = g_strdup(bob_pubkey) };
  GhStore *store = account_open(&bob);
  gh_attachments_set_store(f.attachments, store);
  g_autoptr(GBytes) jpeg = make_jpeg(64 * 1024, 2);
  g_autoptr(GhAttachmentPrepared) prepared = gh_attachment_prepare(jpeg, NULL, 1 << 25, NULL);
  Upload u = { 0 };
  upload(&f, jpeg, &u);
  g_assert_no_error(u.error);
  g_autoptr(GhMessage) message = admit_file(store, bob.account, alice, bob.account, u.file);

  /* A card keeps a reference: the service lets go at a store change. */
  g_autoptr(GhAttachmentTransfer) transfer =
    g_object_ref(gh_attachments_lookup(f.attachments, message));
  g_assert_nonnull(transfer);
  g_assert_true(gh_attachments_lookup(f.attachments, message) == transfer);
  drain();
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);
  g_assert_cmpuint(gh_attachments_get_downloads_started(f.attachments), ==, 0);

  gh_attachments_download(f.attachments, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==,
                  GH_ATTACHMENT_STATE_DOWNLOADING);
  /* A second Download while one runs is the same one. */
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  g_assert_true(g_bytes_equal(gh_attachment_transfer_get_plaintext(transfer),
                              prepared->plaintext));
  g_assert_true(gh_attachment_transfer_get_previewable(transfer));
  g_assert_false(gh_attachment_transfer_get_from_cache(transfer));
  gint64 cached = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_attachments_get_cache_size(f.attachments, &cached, &error));
  g_assert_cmpint(cached, ==, (gint64)g_bytes_get_size(prepared->plaintext));

  /* The next session: the encrypted cache, no request. */
  gh_attachments_set_store(f.attachments, NULL);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_null(gh_attachment_transfer_get_plaintext(transfer));
  gh_attachments_set_store(f.attachments, store);
  GhAttachmentTransfer *again = gh_attachments_lookup(f.attachments, message);
  g_assert_true(again != transfer);
  gh_attachments_download(f.attachments, again);
  spin_until(not_downloading, again);
  g_assert_cmpint(gh_attachment_transfer_get_state(again), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(gh_attachment_transfer_get_from_cache(again));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  /* An old transfer is no longer the service's: nothing happens. */
  gh_attachments_download(f.attachments, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);

  /* Clear: the cache is empty, the same transfer is IDLE, Download goes to
   * the server again. */
  g_assert_true(gh_attachments_clear_cache(f.attachments, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_attachment_transfer_get_state(again), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_true(gh_attachments_lookup(f.attachments, message) == again);
  g_assert_true(gh_attachments_get_cache_size(f.attachments, &cached, &error));
  g_assert_cmpint(cached, ==, 0);
  gh_attachments_download(f.attachments, again);
  spin_until(not_downloading, again);
  g_assert_cmpint(gh_attachment_transfer_get_state(again), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 2);
  g_assert_cmpuint(gh_attachments_get_downloads_started(f.attachments), ==, 3);
  upload_clear(&u);
  gh_attachments_set_store(f.attachments, NULL);
  account_clear(&bob);
  fixture_down(&f);
}

/* AT-8 at the service: Cancel is at once (IDLE, nothing held), the late
 * answer changes nothing, nothing is cached, and Download works again. */
static void
test_cancel(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  Account bob = { .account = g_strdup(bob_pubkey) };
  GhStore *store = account_open(&bob);
  gh_attachments_set_store(f.attachments, store);
  g_autoptr(GBytes) jpeg = make_jpeg(256 * 1024, 3);
  Upload u = { 0 };
  upload(&f, jpeg, &u);
  g_assert_no_error(u.error);
  g_autoptr(GhMessage) message = admit_file(store, bob.account, alice, bob.account, u.file);
  g_autoptr(GhAttachmentTransfer) transfer =
    g_object_ref(gh_attachments_lookup(f.attachments, message));

  blossom_fixture_set_stall(f.blossom, TRUE);
  gh_attachments_download(f.attachments, transfer);
  spin_until(held_one, f.blossom);
  drain();
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==,
                  GH_ATTACHMENT_STATE_DOWNLOADING);
  gh_attachments_cancel(f.attachments, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_null(gh_attachment_transfer_get_plaintext(transfer));
  blossom_fixture_release_held(f.blossom);
  blossom_fixture_set_stall(f.blossom, FALSE);
  drain();
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip17File) file = gh_message_dup_file(message);
  g_assert_null(gh_store_media_get(store, file, NULL, &error));
  g_assert_no_error(error);
  /* Cancelling what doesn't run does nothing. */
  gh_attachments_cancel(f.attachments, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);

  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);

  /* A store change cancels a download in flight too, before the store goes. */
  g_assert_true(gh_attachments_clear_cache(f.attachments, &error));
  blossom_fixture_set_stall(f.blossom, TRUE);
  guint held = blossom_fixture_held(f.blossom);
  gh_attachments_download(f.attachments, transfer);
  while (blossom_fixture_held(f.blossom) <= held)
    g_main_context_iteration(NULL, TRUE);
  gh_attachments_set_store(f.attachments, NULL);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  blossom_fixture_release_held(f.blossom);
  blossom_fixture_set_stall(f.blossom, FALSE);
  drain();
  g_assert_null(gh_store_media_get(store, file, NULL, &error));
  upload_clear(&u);
  account_clear(&bob);
  fixture_down(&f);
}

/* Failures in words, with Try Again only where it can help. */
static void
test_download_errors(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  Account bob = { .account = g_strdup(bob_pubkey) };
  GhStore *store = account_open(&bob);
  gh_attachments_set_store(f.attachments, store);
  g_autoptr(GBytes) jpeg = make_jpeg(32 * 1024, 4);
  Upload u = { 0 };
  upload(&f, jpeg, &u);
  g_assert_no_error(u.error);

  /* Damaged: the server's copy has one flipped bit. */
  g_autoptr(GBytes) blob = g_bytes_ref(blossom_fixture_get_blob(f.blossom, u.file->x));
  gsize size = 0;
  guint8 *tampered = g_memdup2(g_bytes_get_data(blob, &size), size);
  tampered[size / 2] ^= 0x01;
  g_autoptr(GBytes) bad = g_bytes_new_take(tampered, size);
  blossom_fixture_put_blob(f.blossom, u.file->x, bad);
  g_autoptr(GhMessage) damaged = admit_file(store, bob.account, alice, bob.account, u.file);
  GhAttachmentTransfer *transfer = gh_attachments_lookup(f.attachments, damaged);
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_nonnull(strstr(gh_attachment_transfer_get_error(transfer), "changed or damaged"));
  g_assert_false(gh_attachment_transfer_get_can_retry(transfer));
  g_assert_null(gh_attachment_transfer_get_plaintext(transfer));

  /* Gone from the server. */
  GhNip17File *gone = gh_nip17_file_copy(u.file);
  memset(gone->x, 'a', 64);
  g_free(gone->url);
  gone->url = g_strdup_printf("%s/%s", blossom_fixture_url(f.blossom), gone->x);
  g_autoptr(GhMessage) missing = admit_file(store, bob.account, alice, bob.account, gone);
  transfer = gh_attachments_lookup(f.attachments, missing);
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_cmpstr(gh_attachment_transfer_get_error(transfer), ==,
                  "The server no longer has this file.");
  g_assert_false(gh_attachment_transfer_get_can_retry(transfer));

  /* Unreachable: nothing listens there. */
  g_autoptr(GSocketListener) probe = g_socket_listener_new();
  guint16 port = g_socket_listener_add_any_inet_port(probe, NULL, NULL);
  g_socket_listener_close(probe);
  g_free(gone->url);
  gone->url = g_strdup_printf("http://127.0.0.1:%u/%s", port, u.file->x);
  memcpy(gone->x, u.file->x, sizeof gone->x);
  gone->nonce[0] ^= 1; /* another file identity than the damaged one */
  g_autoptr(GhMessage) unreachable = admit_file(store, bob.account, alice, bob.account, gone);
  transfer = gh_attachments_lookup(f.attachments, unreachable);
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_cmpstr(gh_attachment_transfer_get_error(transfer), ==,
                  "Can't reach the server that holds this file.");
  g_assert_true(gh_attachment_transfer_get_can_retry(transfer));
  gh_nip17_file_free(gone);

  /* Not a public server (the real policy, without the tests' loopback
   * exception): refused before any request. */
  gh_attachments_set_allow_private_hosts(f.attachments, FALSE);
  guint gets = blossom_fixture_count(f.blossom, "GET");
  g_autoptr(GhMessage) local = admit_file(store, bob.account, alice, bob.account, u.file);
  transfer = gh_attachments_lookup(f.attachments, local);
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_FAILED);
  g_assert_nonnull(strstr(gh_attachment_transfer_get_error(transfer),
                          "isn't a public attachment server"));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, gets);
  upload_clear(&u);
  gh_attachments_set_store(f.attachments, NULL);
  account_clear(&bob);
  fixture_down(&f);
}

/* The words for each failure (pure). */
static void
test_error_words(void)
{
  static const struct {
    GQuark (*domain)(void);
    gint code;
    GhAttachmentsDirection direction;
    gboolean tor;
    gboolean unreachable;
    const gchar *host;
    const gchar *contains;
    gboolean retry;
  } cases[] = {
    { gh_attachment_error_quark, GH_ATTACHMENT_ERROR_DAMAGED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE,
      NULL, "changed or damaged", FALSE },
    { gh_blossom_error_quark, GH_BLOSSOM_ERROR_TOO_LARGE, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE,
      NULL, "larger than Groundhog accepts (25 MB)", FALSE },
    { g_io_error_quark, G_IO_ERROR_MESSAGE_TOO_LARGE, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL,
      "larger than Groundhog accepts", FALSE },
    { gh_blossom_error_quark, GH_BLOSSOM_ERROR_TOO_LARGE, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE, NULL,
      "at most 25 MB", FALSE },
    { g_io_error_quark, G_IO_ERROR_PERMISSION_DENIED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE,
      "files.example.com", "isn't a public attachment server", FALSE },
    { g_io_error_quark, G_IO_ERROR_PERMISSION_DENIED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE,
      "abc.onion", "Tor onion service", FALSE },
    { g_io_error_quark, G_IO_ERROR_NOT_FOUND, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL,
      "no longer has this file", FALSE },
    { g_io_error_quark, G_IO_ERROR_CONNECTION_REFUSED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL,
      "Can't reach the server that holds this file.", TRUE },
    { g_io_error_quark, G_IO_ERROR_TIMED_OUT, GH_ATTACHMENTS_DOWNLOAD, TRUE, FALSE, NULL,
      "through Tor", TRUE },
    { g_io_error_quark, G_IO_ERROR_CONNECTION_REFUSED, GH_ATTACHMENTS_DOWNLOAD, TRUE, FALSE, NULL,
      "Can't reach Tor, so nothing was downloaded", TRUE },
    { g_io_error_quark, G_IO_ERROR_TIMED_OUT, GH_ATTACHMENTS_DOWNLOAD, TRUE, TRUE, NULL,
      "Can't reach Tor", TRUE },
    { g_io_error_quark, G_IO_ERROR_PROXY_FAILED, GH_ATTACHMENTS_UPLOAD, TRUE, FALSE, NULL,
      "Can't reach Tor, so nothing was sent", TRUE },
    { g_io_error_quark, G_IO_ERROR_HOST_NOT_FOUND, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE,
      "files.example.com", "Can't reach files.example.com, so nothing was sent", TRUE },
    { g_io_error_quark, G_IO_ERROR_CONNECTION_CLOSED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL,
      "network setting changed", TRUE },
    { gh_blossom_error_quark, GH_BLOSSOM_ERROR_SIGNER, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE, NULL,
      "Nostr Signer didn't sign", TRUE },
    { gh_blossom_error_quark, GH_BLOSSOM_ERROR_BAD_ANSWER, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE,
      NULL, "different file", TRUE },
    { gh_blossom_error_quark, GH_BLOSSOM_ERROR_AUTH_REQUIRED, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE,
      "files.example.com", "files.example.com refused the upload, even from your account", TRUE },
    { g_io_error_quark, G_IO_ERROR_INVALID_DATA, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE, NULL,
      "This image is damaged", FALSE },
    { gh_attachment_error_quark, GH_ATTACHMENT_ERROR_EMPTY, GH_ATTACHMENTS_UPLOAD, FALSE, FALSE,
      NULL, "empty", FALSE },
    { gh_net_http_error_quark, 500, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL, "HTTP 500", TRUE },
    { g_io_error_quark, G_IO_ERROR_FAILED, GH_ATTACHMENTS_DOWNLOAD, FALSE, FALSE, NULL,
      "refused the download", TRUE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GError) error = g_error_new_literal(cases[i].domain(), cases[i].code, "detail");
    gboolean retry = !cases[i].retry;
    g_autofree gchar *text = gh_attachments_describe_error(error, cases[i].direction, cases[i].tor,
                                                           cases[i].unreachable, cases[i].host,
                                                           &retry);
    if (!strstr(text, cases[i].contains))
      g_error("case %u: \"%s\" does not say \"%s\"", i, text, cases[i].contains);
    g_assert_cmpint(retry, ==, cases[i].retry);
  }
}

/* The decrypted bytes held for the cards are bounded: the least recently
 * finished transfer is let go (IDLE again), the newest kept. */
static void
test_held_bound(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  Account bob = { .account = g_strdup(bob_pubkey) };
  GhStore *store = account_open(&bob);
  gh_attachments_set_store(f.attachments, store);
  gh_attachments_set_held_max(f.attachments, 100 * 1024);
  GhAttachmentTransfer *transfers[3];
  GhMessage *messages[3];
  for (guint i = 0; i < 3; i++) {
    g_autoptr(GBytes) jpeg = make_jpeg(40 * 1024, 10 + i);
    Upload u = { 0 };
    upload(&f, jpeg, &u);
    g_assert_no_error(u.error);
    messages[i] = admit_file(store, bob.account, alice, bob.account, u.file);
    transfers[i] = gh_attachments_lookup(f.attachments, messages[i]);
    gh_attachments_download(f.attachments, transfers[i]);
    spin_until(not_downloading, transfers[i]);
    g_assert_cmpint(gh_attachment_transfer_get_state(transfers[i]), ==,
                    GH_ATTACHMENT_STATE_READY);
    upload_clear(&u);
  }
  /* 3 x 40 KiB > 100 KiB: the first went, the two newer stay. */
  g_assert_cmpint(gh_attachment_transfer_get_state(transfers[0]), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfers[1]), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfers[2]), ==, GH_ATTACHMENT_STATE_READY);
  /* Asking again comes from the encrypted cache. */
  guint gets = blossom_fixture_count(f.blossom, "GET");
  gh_attachments_download(f.attachments, transfers[0]);
  spin_until(not_downloading, transfers[0]);
  g_assert_true(gh_attachment_transfer_get_from_cache(transfers[0]));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, gets);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfers[1]), ==, GH_ATTACHMENT_STATE_IDLE);
  for (guint i = 0; i < 3; i++)
    g_object_unref(messages[i]);
  gh_attachments_set_store(f.attachments, NULL);
  account_clear(&bob);
  fixture_down(&f);
}

/* ---- sent files ------------------------------------------------------------------------- */

/* The sender's own file: kept in the encrypted cache as the sent message's,
 * READY without a download; a plaintext that is not the file's is not. */
static void
test_sent_own_file(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  Account bob = { .account = g_strdup(bob_pubkey) };
  GhStore *store = account_open(&bob);
  gh_attachments_set_store(f.attachments, store);
  g_autoptr(GBytes) jpeg = make_jpeg(16 * 1024, 5);
  g_autoptr(GhAttachmentPrepared) prepared = gh_attachment_prepare(jpeg, "image/jpeg", 1 << 25,
                                                                   NULL);
  Upload u = { 0 };
  /* What the attach UI uploads: the prepared (already stripped) bytes. */
  upload(&f, prepared->plaintext, &u);
  g_assert_no_error(u.error);
  g_autofree gchar *server = gh_blossom_client_normalize_server(blossom_fixture_url(f.blossom),
                                                                NULL);
  g_assert_cmpstr(u.server, ==, server);
  g_assert_true(g_str_has_prefix(u.file->url, server));
  g_autoptr(GhMessage) mine = admit_file(store, bob.account, bob.account, alice, u.file);

  /* Another plaintext: nothing is kept or shown. */
  gh_attachments_remember_sent(f.attachments, gh_message_get_rumor_id(mine), u.file, jpeg);
  GhAttachmentTransfer *transfer = gh_attachments_lookup(f.attachments, mine);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);

  gh_attachments_remember_sent(f.attachments, gh_message_get_rumor_id(mine), u.file,
                               prepared->plaintext);
  g_assert_true(gh_attachments_lookup(f.attachments, mine) == transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(gh_attachment_transfer_get_previewable(transfer));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);
  /* Kept for later sessions. */
  gh_attachments_set_store(f.attachments, NULL);
  gh_attachments_set_store(f.attachments, store);
  transfer = gh_attachments_lookup(f.attachments, mine);
  gh_attachments_download(f.attachments, transfer);
  spin_until(not_downloading, transfer);
  g_assert_true(gh_attachment_transfer_get_from_cache(transfer));
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);
  upload_clear(&u);
  gh_attachments_set_store(f.attachments, NULL);
  account_clear(&bob);
  fixture_down(&f);
}

/* Without an open store: nothing to look up, upload, consent to or clear. */
static void
test_no_store(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  g_autoptr(GBytes) jpeg = make_jpeg(4096, 6);
  Upload u = { 0 };
  upload(&f, jpeg, &u);
  g_assert_error(u.error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_assert_null(gh_attachments_get_client(f.attachments));
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_attachments_set_consent(f.attachments, blossom_fixture_url(f.blossom), TRUE,
                                            &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&error);
  gint64 size = 1;
  g_assert_false(gh_attachments_get_cache_size(f.attachments, &size, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&error);
  g_assert_false(gh_attachments_clear_cache(f.attachments, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_auto(GStrv) consents = gh_attachments_dup_consents(f.attachments);
  g_assert_cmpuint(g_strv_length(consents), ==, 0);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  upload_clear(&u);
  fixture_down(&f);
}

/* ---- the card's words -------------------------------------------------------------------- */

static void
test_card_words(void)
{
  static const struct {
    const gchar *mime;
    const gchar *kind;
  } kinds[] = {
    { "image/jpeg", "Photo" },       { "image/heic", "Photo" },  { "video/mp4", "Video" },
    { "audio/ogg", "Audio" },        { "application/pdf", "File (PDF)" },
    { "application/zip", "File (ZIP)" }, { "text/plain", "File (Text)" },
    { "application/octet-stream", "File" }, { NULL, "File" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    g_autofree gchar *kind = gh_attachment_card_describe_type(kinds[i].mime);
    g_assert_cmpstr(kind, ==, kinds[i].kind);
    g_assert_nonnull(gh_attachment_card_type_icon(kinds[i].mime));
  }
  /* The saved name follows the bytes, not the declared type. */
  g_autoptr(GBytes) jpeg = make_jpeg(1024, 7);
  g_autofree gchar *jpeg_name = gh_attachment_card_suggest_name("application/pdf", jpeg);
  g_assert_cmpstr(jpeg_name, ==, "photo.jpg");
  g_autoptr(GBytes) pdf = g_bytes_new_static("%PDF-1.7\n", 9);
  g_autofree gchar *pdf_name = gh_attachment_card_suggest_name("image/png", pdf);
  g_assert_cmpstr(pdf_name, ==, "file.pdf");
  g_autoptr(GBytes) other = g_bytes_new_static("#!/bin/sh\n", 10);
  g_autofree gchar *other_name = gh_attachment_card_suggest_name("application/x-sh", other);
  g_assert_cmpstr(other_name, ==, "file");
}

/* ---- main ------------------------------------------------------------------------------ */

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree(child);
      else
        g_unlink(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

/* P3, AT-5: the transient directories stay empty. */
static void
assert_no_transient_files(void)
{
  static const gchar *const transient[] = { "cache", "runtime", "tmp" };
  for (guint i = 0; i < G_N_ELEMENTS(transient); i++) {
    g_autofree gchar *path = root_dir(transient[i]);
    GDir *dir = g_dir_open(path, 0, NULL);
    g_assert_nonnull(dir);
    const gchar *name = g_dir_read_name(dir);
    if (name)
      g_error("%s holds %s", transient[i], name);
    g_dir_close(dir);
  }
}

int
main(int argc, char **argv)
{
  /* Private XDG directories and TMPDIR under one root before GLib reads them. */
  const gchar *base = g_getenv("TMPDIR");
  gchar *template = g_build_filename(base && *base ? base : "/tmp", "groundhog-attachments-XXXXXX",
                                     NULL);
  if (!mkdtemp(template))
    g_error("cannot create the test root: %s", g_strerror(errno));
  char *real = realpath(template, NULL);
  g_assert_nonnull(real);
  root = g_strdup(real);
  free(real);
  g_free(template);
  for (guint i = 0; i < G_N_ELEMENTS(xdg_names); i++) {
    g_autofree gchar *dir = root_dir(xdg_names[i]);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(xdg_vars[i], dir, TRUE);
  }
  g_setenv("GIO_USE_VFS", "local", TRUE);
  g_test_init(&argc, &argv, NULL);
#define ADD(path, func) g_test_add_func("/groundhog/attachments/" path, func)
  ADD("server-list", test_server_list);
  ADD("consent-store", test_consent_store);
  ADD("consent-service", test_consent_service);
  ADD("download-on-request", test_download_on_request);
  ADD("cancel", test_cancel);
  ADD("download-errors", test_download_errors);
  ADD("error-words", test_error_words);
  ADD("held-bound", test_held_bound);
  ADD("sent-own-file", test_sent_own_file);
  ADD("no-store", test_no_store);
  ADD("card-words", test_card_words);
#undef ADD
  int status = g_test_run();
  assert_no_transient_files();
  remove_tree(root);
  g_free(root);
  return status;
}