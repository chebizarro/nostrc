/* Files and pictures in encrypted groups (W25; nostrc-q3a6, nostrc-m6tp):
 * GhMlsAttachments over real accounts, stores and GhMlsService instances
 * (mls-world.h) with the local Blossom fixture, through GhNetHttp. Checked:
 * a received file is never fetched before the user's Download, then opened
 * with the epoch libmarmot authenticated for the message, kept in the
 * encrypted cache bound to that message, and found there again after a
 * restart (the epoch restored from the store); the name sent is neutral; a
 * Commit during the upload means the file is sealed and uploaded again, and
 * a message in a stale epoch is never stored or sent; in Tor mode every
 * transfer goes through Tor; a legacy group has no picture; and Save As's
 * name is sanitized. Only loopback is contacted. Nothing sleeps. */

#include "mls-world.h"

#include "blossom-fixture.h"
#include "gh-attachments.h"
#include "gh-mls-attachments.h"
#include "gh-mls-imeta.h"
#include "gh-store-media.h"
#include "socks5-fixture.h"

#include <nostr-event.h>
#include <string.h>

#define ONION "groundhogtestmlsfilesqw4yvkpvrbwe5wtamgzhhdrhm5k2ww7m3tgdwbzuxyd.onion"

static void
wait_key_packages(World *w, const guint *keys, guint n)
{
  for (guint i = 0; i < n; i++)
    spin_until(key_package_published, &w->apps[keys[i]], "a published KeyPackage");
}

/* ---- a PNG with a hidden author (as test_mls_media.c) ----------------------------- */

static void
png_chunk(GByteArray *out, const gchar *type, const void *data, gsize size)
{
  guint8 len[4] = { (guint8)(size >> 24), (guint8)(size >> 16), (guint8)(size >> 8),
                    (guint8)size };
  g_byte_array_append(out, len, 4);
  GByteArray *crc_input = g_byte_array_new();
  g_byte_array_append(crc_input, (const guint8 *)type, 4);
  if (size)
    g_byte_array_append(crc_input, data, (guint)size);
  g_byte_array_append(out, crc_input->data, crc_input->len);
  guint32 crc = 0xffffffffu;
  for (guint i = 0; i < crc_input->len; i++) {
    crc ^= crc_input->data[i];
    for (guint k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  crc ^= 0xffffffffu;
  guint8 c[4] = { (guint8)(crc >> 24), (guint8)(crc >> 16), (guint8)(crc >> 8), (guint8)crc };
  g_byte_array_append(out, c, 4);
  g_byte_array_unref(crc_input);
}

static GBytes *
make_png(void)
{
  GByteArray *out = g_byte_array_new();
  g_byte_array_append(out, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
  const guint8 ihdr[13] = { 0, 0, 0, 1, 0, 0, 0, 1, 8, 0, 0, 0, 0 };
  png_chunk(out, "IHDR", ihdr, sizeof ihdr);
  png_chunk(out, "tEXt", "Author\0Secret Name", 18);
  png_chunk(out, "IDAT", "\x78\x9c\x63\x60\x00\x00\x00\x02\x00\x01", 10);
  png_chunk(out, "IEND", NULL, 0);
  return g_byte_array_free_to_bytes(out);
}

static gboolean
has_secret(GBytes *bytes)
{
  return memmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "Secret Name", 11) !=
         NULL;
}

/* ---- an account's files -------------------------------------------------------------- */

typedef struct {
  App *app;
  GSettings *settings;   /* the attachments' own: blossom-servers, network-mode */
  GhNetHttp *http;
  GhAttachments *attachments;
  GhMlsAttachments *files;
} Files;

static void
files_up(Files *x, App *app, const gchar *server, Socks5Fixture *tor)
{
  memset(x, 0, sizeof *x);
  x->app = app;
  g_autoptr(GSettingsBackend) backend = g_memory_settings_backend_new();
  x->settings = g_settings_new_with_backend("org.nostr.Groundhog", backend);
  const gchar *servers[] = { server, NULL };
  g_settings_set_strv(x->settings, "blossom-servers", servers);
  g_settings_set_string(x->settings, "network-mode", tor ? "tor" : "none");
  if (tor)
    g_settings_set_string(x->settings, "tor-socks-address", socks5_fixture_address(tor));
  x->http = gh_net_http_new(x->settings);
  GhAttachmentsConfig config = { .settings = x->settings, .http = x->http };
  x->attachments = gh_attachments_new(&config);
  /* The fixture is on loopback, which real downloads may not reach. */
  gh_attachments_set_allow_private_hosts(x->attachments, TRUE);
  gh_attachments_set_store(x->attachments, app->store);
  x->files = gh_mls_attachments_new(x->attachments);
  gh_mls_attachments_set_service(x->files, app->service);
}

/* Around app_restart(), as the application does: the closing store is let
 * go first (its transfers with it), then the new store and service. */
static void
files_detach(Files *x)
{
  gh_attachments_set_store(x->attachments, NULL);
}

static void
files_follow(Files *x)
{
  gh_attachments_set_store(x->attachments, x->app->store);
  gh_mls_attachments_set_service(x->files, x->app->service);
}

static void
files_down(Files *x)
{
  g_clear_object(&x->files);
  gh_attachments_set_store(x->attachments, NULL);
  g_clear_object(&x->attachments);
  g_clear_object(&x->http);
  drain();
  g_clear_object(&x->settings);
}

typedef struct {
  gboolean done;
  GhMessage *message;
  gchar *server;
  GError *error;
} SendWait;

static gboolean
send_done(gpointer data)
{
  return ((SendWait *)data)->done;
}

static void
on_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  SendWait *wait = data;
  wait->message = gh_mls_attachments_send_finish(GH_MLS_ATTACHMENTS(source), result,
                                                 &wait->server, &wait->error);
  wait->done = TRUE;
}

static void
send_wait_clear(SendWait *wait)
{
  g_clear_object(&wait->message);
  g_clear_pointer(&wait->server, g_free);
  g_clear_error(&wait->error);
}

static gboolean
transfer_settled(gpointer data)
{
  GhAttachmentState state = gh_attachment_transfer_get_state(data);
  return state == GH_ATTACHMENT_STATE_READY || state == GH_ATTACHMENT_STATE_FAILED;
}

/* The one listed message of the room carrying files (empty caption). */
static GhMessage *
file_message(App *app, const gchar *room)
{
  return find_message(app, room, "");
}

typedef struct {
  App *app;
  const gchar *room;
} FileWait;

static gboolean
file_listed(gpointer data)
{
  FileWait *wait = data;
  return file_message(wait->app, wait->room) != NULL;
}

static gchar *
inner_imeta(GhMessage *message)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(event, gh_message_get_rumor_json(message),
                                                   NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  NostrTags *tags = nostr_event_get_tags(event);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get(tag, 0), "imeta") != 0)
      continue;
    for (size_t k = 0; k < nostr_tag_size(tag); k++)
      g_string_append_printf(out, "%s|", nostr_tag_get(tag, k));
  }
  nostr_event_free(event);
  return g_string_free(out, FALSE);
}

/* ---- tests ------------------------------------------------------------------------------- */

/* Alice sends a photo; Bob sees its card, nothing fetched, until he chooses
 * Download; then it opens with the message's epoch and stays in his
 * encrypted cache, bound to the message, across a restart, until he forgets
 * the group. */
static void
test_send_receive_on_request(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Files", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  BlossomFixture *blossom = blossom_fixture_new();
  Files fa, fb;
  files_up(&fa, alice, blossom_fixture_url(blossom), NULL);
  files_up(&fb, bob, blossom_fixture_url(blossom), NULL);

  g_autoptr(GBytes) photo = make_png();
  SendWait sent = { 0 };
  gh_mls_attachments_send_async(fa.files, ga, photo, "IMG_20260930_142233.png", "image/png",
                                NULL, NULL, on_sent, &sent);
  spin_until(send_done, &sent, "the file sent");
  g_assert_no_error(sent.error);
  g_assert_nonnull(sent.message);
  g_assert_cmpstr(sent.server, ==, blossom_fixture_url(blossom));
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);
  /* One imeta, the neutral name, the group's current epoch. */
  g_assert_cmpuint(gh_message_get_n_attachments(sent.message), ==, 1);
  const GhMessageAttachment *mine = gh_message_get_attachment(sent.message, 0);
  g_assert_cmpstr(mine->filename, ==, "photo.png");
  g_assert_cmpstr(mine->media_type, ==, "image/png");
  g_autofree gchar *imeta = inner_imeta(sent.message);
  g_assert_nonnull(strstr(imeta, "filename photo.png|"));
  g_assert_null(strstr(imeta, "IMG_2026"));
  guint64 epoch = 0;
  g_assert_true(gh_message_get_mls_epoch(sent.message, &epoch));
  g_assert_cmpuint(epoch, ==, gh_mls_group_get_epoch(ga));
  /* The sender's own card: from the cache, no download. */
  GhAttachmentTransfer *own = gh_mls_attachments_lookup(fa.files, sent.message, 0);
  g_assert_nonnull(own);
  g_assert_cmpint(gh_attachment_transfer_get_state(own), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(gh_attachment_transfer_get_from_cache(own));
  g_assert_false(has_secret(gh_attachment_transfer_get_plaintext(own)));
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);

  /* Bob's message: the card's data, and nothing fetched. */
  FileWait listed = { bob, room };
  spin_until(file_listed, &listed, "Bob's file message");
  GhMessage *received = file_message(bob, room);
  g_assert_cmpuint(gh_message_get_n_attachments(received), ==, 1);
  g_assert_cmpuint(gh_message_get_rejected_attachments(received), ==, 0);
  const GhMessageAttachment *theirs = gh_message_get_attachment(received, 0);
  g_assert_cmpstr(theirs->file_id, ==, mine->file_id);
  guint64 received_epoch = 0;
  g_assert_true(gh_message_get_mls_epoch(received, &received_epoch));
  g_assert_cmpuint(received_epoch, ==, epoch);
  GhAttachmentTransfer *transfer = gh_mls_attachments_lookup(fb.files, received, 0);
  g_assert_nonnull(transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  g_assert_cmpstr(gh_attachment_transfer_get_suggested_name(transfer), ==, "photo.png");
  drain();
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);
  g_assert_cmpuint(gh_mls_attachments_get_downloads_started(fb.files), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_store_media_get_id(bob->store, theirs->file_id, NULL, &error));
  g_assert_no_error(error);

  /* Download: fetched once, checked, opened, kept. */
  gh_mls_attachments_download(fb.files, transfer);
  spin_until(transfer_settled, transfer, "Bob's download");
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_false(gh_attachment_transfer_get_from_cache(transfer));
  g_assert_true(gh_attachment_transfer_get_previewable(transfer));
  g_assert_true(g_bytes_equal(gh_attachment_transfer_get_plaintext(transfer),
                              gh_attachment_transfer_get_plaintext(own)));
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);
  g_autofree gchar *file_id = g_strdup(theirs->file_id);
  g_autoptr(GBytes) kept = gh_store_media_get_id(bob->store, file_id, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(kept);

  /* A restart: the message comes back from the store with its epoch, and
   * the file from the cache, without the network. */
  files_detach(&fb);
  app_restart(bob);
  files_follow(&fb);
  spin_until(file_listed, &listed, "Bob's file message after the restart");
  received = file_message(bob, room);
  g_assert_true(gh_message_get_mls_epoch(received, &received_epoch));
  g_assert_cmpuint(received_epoch, ==, epoch);
  g_assert_cmpstr(gh_message_get_attachment(received, 0)->file_id, ==, file_id);
  transfer = gh_mls_attachments_lookup(fb.files, received, 0);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_IDLE);
  gh_mls_attachments_download(fb.files, transfer);
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(gh_attachment_transfer_get_from_cache(transfer));
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);

  /* Forgetting the group takes the decrypted copy with its message. */
  gint64 conversation = 0;
  g_assert_true(gh_store_find_conversation(bob->store, GH_STORE_BACKEND_MLS,
                                           gh_mls_group_get_group_id(ga), &conversation,
                                           &error));
  g_assert_true(gh_store_forget_conversation(bob->store, conversation, &error));
  g_assert_no_error(error);
  g_assert_null(gh_store_media_get_id(bob->store, file_id, NULL, &error));
  g_assert_no_error(error);

  send_wait_clear(&sent);
  files_down(&fa);
  files_down(&fb);
  blossom_fixture_free(blossom);
  world_down(&w);
}

static gboolean
upload_held(gpointer data)
{
  return blossom_fixture_held(data) > 0;
}

/* A Commit lands while the file is uploading: the file was sealed for the
 * old epoch, so it is sealed and uploaded again and sent in the new one;
 * Bob opens it. */
static void
test_epoch_change_reseals(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Reseal", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  GhMlsGroup *gb = join(bob, ALICE);
  BlossomFixture *blossom = blossom_fixture_new();
  Files fa, fb;
  files_up(&fa, alice, blossom_fixture_url(blossom), NULL);
  files_up(&fb, bob, blossom_fixture_url(blossom), NULL);
  guint64 before = gh_mls_group_get_epoch(ga);

  blossom_fixture_set_hold(blossom, TRUE);
  static const gchar text[] = "a document sealed twice";
  g_autoptr(GBytes) doc = g_bytes_new_static(text, sizeof text - 1);
  SendWait sent = { 0 };
  gh_mls_attachments_send_async(fa.files, ga, doc, "minutes.txt", "text/plain", NULL, NULL,
                                on_sent, &sent);
  spin_until(upload_held, blossom, "the first upload held");
  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Reseal 2", NULL, NULL, on_changed,
                                       &renamed);
  spin_until(op_done, &renamed, "the rename");
  g_assert_no_error(renamed.error);
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, before + 1);
  blossom_fixture_set_hold(blossom, FALSE);
  blossom_fixture_release_held(blossom);
  spin_until(send_done, &sent, "the file sent");
  g_assert_no_error(sent.error);
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 2);
  guint64 epoch = 0;
  g_assert_true(gh_message_get_mls_epoch(sent.message, &epoch));
  g_assert_cmpuint(epoch, ==, before + 1);
  g_assert_cmpstr(gh_message_get_attachment(sent.message, 0)->filename, ==, "file.txt");

  wait_epoch(gb, (gint)before + 1);
  FileWait listed = { bob, room };
  spin_until(file_listed, &listed, "Bob's file message");
  GhMessage *received = file_message(bob, room);
  GhAttachmentTransfer *transfer = gh_mls_attachments_lookup(fb.files, received, 0);
  gh_mls_attachments_download(fb.files, transfer);
  spin_until(transfer_settled, transfer, "Bob's download");
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_true(g_bytes_equal(gh_attachment_transfer_get_plaintext(transfer), doc));

  send_wait_clear(&sent);
  files_down(&fa);
  files_down(&fb);
  blossom_fixture_free(blossom);
  world_down(&w);
}

typedef struct {
  gboolean done;
  GhMlsAttachment *attachment;
  GError *error;
} UploadWait;

static gboolean
upload_done(gpointer data)
{
  return ((UploadWait *)data)->done;
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  UploadWait *wait = data;
  wait->attachment = gh_mls_media_upload_finish(result, &wait->error);
  wait->done = TRUE;
}

static guint
listed_count(App *app, const gchar *room)
{
  GhConversation *conversation = gh_conversation_store_lookup(app->model, room);
  return conversation ? g_list_model_get_n_items(G_LIST_MODEL(conversation)) : 0;
}

/* A file sealed for an epoch the group has left is never stored or sent:
 * the service refuses it in the send transaction (EPOCH_CHANGED). */
static void
test_stale_epoch_refused(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Stale", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(&w.apps[BOB], ALICE);
  BlossomFixture *blossom = blossom_fixture_new();
  Files fa;
  files_up(&fa, alice, blossom_fixture_url(blossom), NULL);
  g_autoptr(GError) error = NULL;
  static const gchar text[] = "sealed in the old epoch";
  g_autoptr(GBytes) doc = g_bytes_new_static(text, sizeof text - 1);
  g_autoptr(GhMlsMediaSealed) sealed = gh_mls_media_seal(
    gh_mls_service_get_marmot(alice->service), gh_mls_group_get_group_id(ga), doc,
    "text/plain", "file.txt", GH_BLOSSOM_MAX_FILE_SIZE, &error);
  g_assert_no_error(error);
  UploadWait up = { 0 };
  gh_mls_media_upload_async(gh_attachments_get_client(fa.attachments), sealed, NULL,
                            on_uploaded, &up);
  spin_until(upload_done, &up, "the upload");
  g_assert_no_error(up.error);
  g_auto(GStrv) imeta = gh_mls_attachment_dup_imeta(up.attachment, &error);
  g_assert_no_error(error);
  guint64 sealed_epoch = gh_mls_attachment_get_source_epoch(up.attachment);

  OpWait renamed = { 0 };
  gh_mls_service_update_metadata_async(alice->service, ga, "Stale 2", NULL, NULL, on_changed,
                                       &renamed);
  spin_until(op_done, &renamed, "the rename");
  g_assert_no_error(renamed.error);
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), >, sealed_epoch);
  guint listed = listed_count(alice, room);
  g_autoptr(GPtrArray) before = published(&w.g, 445);

  g_autoptr(GPtrArray) tags = g_ptr_array_new();
  g_ptr_array_add(tags, imeta);
  g_autoptr(GhMessage) message = gh_mls_service_send_with_imeta(alice->service, ga, "", tags,
                                                                sealed_epoch, &error);
  g_assert_null(message);
  g_assert_error(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_EPOCH_CHANGED);
  g_clear_error(&error);
  drain();
  g_assert_cmpuint(listed_count(alice, room), ==, listed);
  g_autoptr(GPtrArray) after = published(&w.g, 445);
  g_assert_cmpuint(after->len, ==, before->len);
  /* Invalid tags are refused before anything is stored. */
  const gchar *bogus[] = { "imeta", NULL };
  g_autoptr(GPtrArray) bad = g_ptr_array_new();
  g_ptr_array_add(bad, bogus);
  g_assert_null(gh_mls_service_send_with_imeta(alice->service, ga, "", bad,
                                               gh_mls_group_get_epoch(ga), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  gh_mls_attachment_free(up.attachment);
  files_down(&fa);
  blossom_fixture_free(blossom);
  world_down(&w);
}

/* Tor mode: the upload and the download both go through Tor (the SOCKS5
 * fixture sees the .onion by name, never resolved here), and only on the
 * user's action. */
static void
test_tor(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Tor", (const guint[]){ BOB }, 1);
  g_autofree gchar *room = g_strdup(gh_mls_group_get_room_id(ga));
  join(bob, ALICE);
  BlossomFixture *blossom = blossom_fixture_new();
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, blossom_fixture_port(blossom));
  Files fa, fb;
  files_up(&fa, alice, "http://" ONION, socks);
  files_up(&fb, bob, "http://" ONION, socks);

  static const gchar text[] = "through tor only";
  g_autoptr(GBytes) doc = g_bytes_new_static(text, sizeof text - 1);
  SendWait sent = { 0 };
  gh_mls_attachments_send_async(fa.files, ga, doc, "notes.txt", "text/plain", NULL, NULL,
                                on_sent, &sent);
  spin_until(send_done, &sent, "the file sent through Tor");
  g_assert_no_error(sent.error);
  guint connects = *socks5_fixture_connect_count(socks);
  g_assert_cmpuint(connects, >=, 1);

  FileWait listed = { bob, room };
  spin_until(file_listed, &listed, "Bob's file message");
  GhMessage *received = file_message(bob, room);
  GhAttachmentTransfer *transfer = gh_mls_attachments_lookup(fb.files, received, 0);
  drain();
  g_assert_cmpuint(*socks5_fixture_connect_count(socks), ==, connects);
  g_autofree gchar *note = gh_mls_attachments_download_note(fb.files, transfer);
  g_assert_nonnull(strstr(note, "through Tor"));
  gh_mls_attachments_download(fb.files, transfer);
  spin_until(transfer_settled, transfer, "Bob's download through Tor");
  g_assert_cmpint(gh_attachment_transfer_get_state(transfer), ==, GH_ATTACHMENT_STATE_READY);
  g_assert_cmpuint(*socks5_fixture_connect_count(socks), >, connects);
  GPtrArray *requests = socks5_fixture_requests(socks);
  for (guint i = 0; i < requests->len; i++) {
    Socks5Request *r = g_ptr_array_index(requests, i);
    g_assert_cmpint(r->atyp, ==, 0x03);
    g_assert_cmpstr(r->host, ==, ONION);
  }
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);

  send_wait_clear(&sent);
  files_down(&fa);
  files_down(&fb);
  socks5_fixture_free(socks);
  blossom_fixture_free(blossom);
  world_down(&w);
}

typedef struct {
  gboolean done;
  gboolean ok;
  GError *error;
} PictureWait;

static gboolean
picture_done(gpointer data)
{
  return ((PictureWait *)data)->done;
}

static void
on_picture_set(GObject *source, GAsyncResult *result, gpointer data)
{
  PictureWait *wait = data;
  wait->ok = gh_mls_attachments_set_picture_finish(GH_MLS_ATTACHMENTS(source), result,
                                                   &wait->error);
  wait->done = TRUE;
}

/* A legacy (MIP-01) group has no picture component: said, and nothing is
 * encrypted, uploaded or committed. */
static void
test_legacy_group_has_no_picture(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_legacy_only = TRUE;   /* MDK 0.8 KeyPackages only: a legacy group */
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Legacy", (const guint[]){ BOB }, 1);
  BlossomFixture *blossom = blossom_fixture_new();
  Files fa;
  files_up(&fa, alice, blossom_fixture_url(blossom), NULL);
  g_assert_false(gh_mls_service_get_adopted(alice->service, ga));
  g_autoptr(GBytes) picture = NULL;
  g_assert_cmpint(gh_mls_attachments_get_picture(fa.files, ga, &picture, NULL), ==,
                  GH_MLS_PICTURE_UNSUPPORTED);
  g_assert_null(picture);
  guint64 epoch = gh_mls_group_get_epoch(ga);
  g_autoptr(GBytes) photo = make_png();
  PictureWait set = { 0 };
  gh_mls_attachments_set_picture_async(fa.files, ga, photo, "image/png", NULL, on_picture_set,
                                       &set);
  spin_until(picture_done, &set, "the refused picture");
  g_assert_false(set.ok);
  g_assert_error(set.error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED);
  g_clear_error(&set.error);
  OpWait removed = { 0 };
  gh_mls_service_set_image_async(alice->service, ga, NULL, NULL, on_changed, &removed);
  spin_until(op_done, &removed, "the refused removal");
  g_assert_error(removed.error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_UNSUPPORTED);
  g_clear_error(&removed.error);
  g_assert_cmpuint(blossom_fixture_requests(blossom)->len, ==, 0);
  g_assert_cmpuint(gh_mls_group_get_epoch(ga), ==, epoch);
  files_down(&fa);
  blossom_fixture_free(blossom);
  world_down(&w);
}

static void
picture_wait_clear(PictureWait *wait)
{
  g_clear_error(&wait->error);
  memset(wait, 0, sizeof *wait);
}

typedef struct {
  gboolean done;
  GBytes *picture;
  GError *error;
} FetchWait;

static gboolean
fetch_done(gpointer data)
{
  return ((FetchWait *)data)->done;
}

static void
on_picture_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  FetchWait *wait = data;
  wait->picture = gh_mls_attachments_fetch_picture_finish(GH_MLS_ATTACHMENTS(source), result,
                                                          &wait->error);
  wait->done = TRUE;
}

static gchar *
current_picture_id(App *app, GhMlsGroup *group)
{
  MarmotGroupComponents c;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_service_get_components(app->service, group, &c, &error));
  gchar *id = c.image.present ? gh_mls_media_picture_id(&c.image) : NULL;
  marmot_group_components_clear(&c);
  return id;
}

/* An adopted group's picture end to end (W25 review L1, M1): its media
 * servers set by an admin's 0x800b Commit; a server that isn't a public
 * host is never uploaded to (nothing reaches it); Alice sets a picture
 * (signed by its own upload key, never her account) and sees it from the
 * store; Bob sees AVAILABLE with nothing fetched, Show Picture fetches it
 * once and then it comes from the store; a replaced picture's copy goes, and
 * so does the removed one's. */
static void
test_adopted_picture(void)
{
  World w;
  const guint keys[] = { ALICE, BOB };
  world_up(&w, keys, G_N_ELEMENTS(keys));
  App *alice = &w.apps[ALICE], *bob = &w.apps[BOB];
  wait_key_packages(&w, keys, G_N_ELEMENTS(keys));
  /* New Group's own path (nostrc-lf62): Bob has an adopted KeyPackage, so
   * the group is adopted. */
  accept_contact(alice, BOB);
  GhMlsGroup *ga = create_group(alice, "Pictures", (const guint[]){ BOB }, 1);
  g_assert_true(gh_mls_service_get_adopted(alice->service, ga));
  GhMlsGroup *gb = join(bob, ALICE);
  g_assert_true(gh_mls_service_get_adopted(bob->service, gb));
  BlossomFixture *blossom = blossom_fixture_new();
  Files fa, fb;
  files_up(&fa, alice, blossom_fixture_url(blossom), NULL);
  files_up(&fb, bob, blossom_fixture_url(blossom), NULL);
  const gchar *gid = gh_mls_group_get_group_id(gb);

  /* No media server yet: no picture can be set. */
  g_assert_cmpint(gh_mls_attachments_get_picture(fb.files, gb, NULL, NULL), ==,
                  GH_MLS_PICTURE_NONE);
  g_autoptr(GBytes) photo = make_png();
  PictureWait set = { 0 };
  gh_mls_attachments_set_picture_async(fa.files, ga, photo, "image/png", NULL, on_picture_set,
                                       &set);
  spin_until(picture_done, &set, "the refused picture");
  g_assert_error(set.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER);
  picture_wait_clear(&set);

  /* An admin names the group's media server (the loopback fixture). */
  const gchar *endpoints[] = { blossom_fixture_url(blossom), NULL };
  OpWait policy = { 0 };
  gh_mls_service_test_set_media_policy_async(alice->service, ga, endpoints, NULL, on_changed,
                                             &policy);
  spin_until(op_done, &policy, "the media policy Commit");
  g_assert_no_error(policy.error);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));

  /* M1: outside tests a loopback server is not a public host: never
   * contacted for the upload, and not offered in the confirmation. */
  gh_attachments_set_allow_private_hosts(fa.attachments, FALSE);
  g_auto(GStrv) none = gh_mls_attachments_dup_picture_upload_hosts(fa.files, ga);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  gh_mls_attachments_set_picture_async(fa.files, ga, photo, "image/png", NULL, on_picture_set,
                                       &set);
  spin_until(picture_done, &set, "the private-host picture refused");
  g_assert_error(set.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER);
  picture_wait_clear(&set);
  g_assert_cmpuint(blossom_fixture_requests(blossom)->len, ==, 0);
  gh_attachments_set_allow_private_hosts(fa.attachments, TRUE);

  /* The confirmation names the server and what it learns. */
  g_auto(GStrv) hosts = gh_mls_attachments_dup_picture_upload_hosts(fa.files, ga);
  g_assert_cmpuint(g_strv_length(hosts), ==, 1);
  g_assert_cmpstr(hosts[0], ==, "127.0.0.1");
  g_autofree gchar *note = gh_mls_picture_upload_note((const gchar *const *)hosts, FALSE);
  g_assert_nonnull(strstr(note, "127.0.0.1"));
  g_assert_nonnull(strstr(note, "IP address"));

  /* Alice sets it: one keyed upload, then the Commit; she sees it. */
  gh_mls_attachments_set_picture_async(fa.files, ga, photo, "image/png", NULL, on_picture_set,
                                       &set);
  spin_until(picture_done, &set, "the picture set");
  g_assert_no_error(set.error);
  g_assert_true(set.ok);
  picture_wait_clear(&set);
  g_assert_cmpuint(blossom_fixture_count(blossom, "PUT"), ==, 1);
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(blossom), 0);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_pubkey, !=, hex[ALICE]);
  g_autoptr(GBytes) alices = NULL;
  g_assert_cmpint(gh_mls_attachments_get_picture(fa.files, ga, &alices, NULL), ==,
                  GH_MLS_PICTURE_READY);
  g_assert_false(has_secret(alices));
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);

  /* Bob: available, nothing fetched until Show Picture; then the store. */
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_auto(GStrv) show_hosts = NULL;
  g_assert_cmpint(gh_mls_attachments_get_picture(fb.files, gb, NULL, &show_hosts), ==,
                  GH_MLS_PICTURE_AVAILABLE);
  g_assert_cmpstr(show_hosts[0], ==, "127.0.0.1");
  drain();
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 0);
  FetchWait fetched = { 0 };
  gh_mls_attachments_fetch_picture_async(fb.files, gb, NULL, on_picture_fetched, &fetched);
  spin_until(fetch_done, &fetched, "Show Picture");
  g_assert_no_error(fetched.error);
  g_assert_true(g_bytes_equal(fetched.picture, alices));
  g_clear_pointer(&fetched.picture, g_bytes_unref);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);
  g_autoptr(GBytes) bobs = NULL;
  g_assert_cmpint(gh_mls_attachments_get_picture(fb.files, gb, &bobs, NULL), ==,
                  GH_MLS_PICTURE_READY);
  g_assert_cmpuint(blossom_fixture_count(blossom, "GET"), ==, 1);
  g_autofree gchar *first_id = current_picture_id(bob, gb);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) kept = gh_store_group_image_get(bob->store, gid, first_id, NULL, &error);
  g_assert_nonnull(kept);

  /* Replaced: Bob's copy of the first picture goes. */
  gh_mls_attachments_set_picture_async(fa.files, ga, photo, "image/png", NULL, on_picture_set,
                                       &set);
  spin_until(picture_done, &set, "the picture replaced");
  g_assert_no_error(set.error);
  picture_wait_clear(&set);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_assert_cmpint(gh_mls_attachments_get_picture(fb.files, gb, NULL, NULL), ==,
                  GH_MLS_PICTURE_AVAILABLE);
  g_autoptr(GBytes) old = gh_store_group_image_get(bob->store, gid, first_id, NULL, &error);
  g_assert_null(old);
  g_assert_no_error(error);
  FetchWait again = { 0 };
  gh_mls_attachments_fetch_picture_async(fb.files, gb, NULL, on_picture_fetched, &again);
  spin_until(fetch_done, &again, "Show Picture again");
  g_assert_no_error(again.error);
  g_clear_pointer(&again.picture, g_bytes_unref);
  g_autofree gchar *second_id = current_picture_id(bob, gb);

  /* Removed: nothing to show, and the copy is gone. */
  gh_mls_attachments_set_picture_async(fa.files, ga, NULL, NULL, NULL, on_picture_set, &set);
  spin_until(picture_done, &set, "the picture removed");
  g_assert_no_error(set.error);
  picture_wait_clear(&set);
  wait_epoch(gb, (gint)gh_mls_group_get_epoch(ga));
  g_assert_cmpint(gh_mls_attachments_get_picture(fb.files, gb, NULL, NULL), ==,
                  GH_MLS_PICTURE_NONE);
  g_autoptr(GBytes) gone = gh_store_group_image_get(bob->store, gid, second_id, NULL, &error);
  g_assert_null(gone);
  g_assert_no_error(error);

  files_down(&fa);
  files_down(&fb);
  blossom_fixture_free(blossom);
  world_down(&w);
}

/* Which picture shows, and what may ever be loaded: a URL avatar wins over
 * an encrypted picture; it is loaded only under the remote-image policy
 * (none in this build), and an unverified one never. */
static void
test_picture_policy(void)
{
  g_assert_cmpint(gh_mls_picture_state_for_source(MARMOT_GROUP_AVATAR_NONE), ==,
                  GH_MLS_PICTURE_NONE);
  g_assert_cmpint(gh_mls_picture_state_for_source(MARMOT_GROUP_AVATAR_URL), ==,
                  GH_MLS_PICTURE_WEB);
  g_assert_cmpint(gh_mls_picture_state_for_source(MARMOT_GROUP_AVATAR_URL_PLACEHOLDER), ==,
                  GH_MLS_PICTURE_WEB_UNVERIFIED);
  g_assert_cmpint(gh_mls_picture_state_for_source(MARMOT_GROUP_AVATAR_BLOSSOM), ==,
                  GH_MLS_PICTURE_AVAILABLE);
  g_assert_false(gh_mls_picture_may_load(GH_MLS_PICTURE_WEB, FALSE));
  g_assert_true(gh_mls_picture_may_load(GH_MLS_PICTURE_WEB, TRUE));
  g_assert_false(gh_mls_picture_may_load(GH_MLS_PICTURE_WEB_UNVERIFIED, FALSE));
  g_assert_false(gh_mls_picture_may_load(GH_MLS_PICTURE_WEB_UNVERIFIED, TRUE));
  g_assert_true(gh_mls_picture_may_load(GH_MLS_PICTURE_AVAILABLE, FALSE));
  g_assert_false(gh_mls_picture_may_load(GH_MLS_PICTURE_NONE, TRUE));
  g_assert_false(gh_mls_picture_may_load(GH_MLS_PICTURE_NO_SERVER, TRUE));
}

/* The name sent says no more than the type; the name a sender chose is
 * made safe before Save As suggests it (v2 allows any UTF-8 but NUL). */
static void
test_names(void)
{
  struct { const gchar *mime, *name, *sent; } sent[] = {
    { "image/jpeg", "IMG_20260930_142233.jpg", "photo.jpg" },
    { "image/png", "Screenshot from 2026-09-30.png", "photo.png" },
    { "text/plain", "Passport scan John Doe.TXT", "file.txt" },
    { "application/gzip", "archive.tar.gz", "file.gz" },
    { "application/octet-stream", "weird.@@", "file" },
    { "application/octet-stream", "no-extension", "file" },
    { "application/octet-stream", "toolong.abcdefghij", "file" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(sent); i++) {
    g_autofree gchar *name = gh_mls_attachments_neutral_name(sent[i].mime, sent[i].name);
    g_assert_cmpstr(name, ==, sent[i].sent);
  }
  struct { const gchar *in, *out; } safe[] = {
    { "photo.jpg", "photo.jpg" },
    { "../../.bashrc", "_.._.bashrc" },
    { "/etc/passwd", "_etc_passwd" },
    { ".hidden", "hidden" },
    /* U+202E RIGHT-TO-LEFT OVERRIDE as bytes (GCC refuses the character
     * itself in source: -Wbidi-chars). */
    { "report\xe2\x80\xae" "gpj.exe", "report_gpj.exe" },
    { "a\\b:c\td", "a_b_c_d" },
    { "...", NULL },
    { "  ", NULL },
    { "trailing. ", "trailing" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(safe); i++) {
    g_autofree gchar *name = gh_attachment_transfer_sanitize_name(safe[i].in);
    g_assert_cmpstr(name, ==, safe[i].out);
  }
  /* W25 review N2: the Arabic letter mark (a Bidi_Control) and the line
   * and paragraph separators. */
  g_autofree gchar *alm = gh_attachment_transfer_sanitize_name("a\xd8\x9c" "b\xe2\x80\xa8"
                                                               "c\xe2\x80\xa9" "d.txt");
  g_assert_cmpstr(alm, ==, "a_b_c_d.txt");
  /* W25 review L3: every server that may be asked is named. */
  const gchar *one[] = { "a.example", NULL };
  const gchar *three[] = { "a.example", "b.example", "c.example", NULL };
  g_autofree gchar *d1 = gh_mls_describe_hosts(one);
  g_autofree gchar *d3 = gh_mls_describe_hosts(three);
  g_assert_cmpstr(d1, ==, "a.example");
  g_assert_cmpstr(d3, ==, "a.example, b.example or c.example");
  g_assert_null(gh_mls_describe_hosts(NULL));
  g_autofree gchar *note3 = gh_mls_download_note(three, FALSE);
  g_assert_nonnull(strstr(note3, "a.example, b.example or c.example"));
  g_assert_nonnull(strstr(note3, "IP address"));
  g_autofree gchar *tor1 = gh_mls_download_note(one, TRUE);
  g_assert_nonnull(strstr(tor1, "through Tor"));
  g_autofree gchar *up3 = gh_mls_picture_upload_note(three, TRUE);
  g_assert_nonnull(strstr(up3, "a.example, b.example or c.example"));
  g_assert_nonnull(strstr(up3, "through Tor"));
  g_assert_null(gh_mls_picture_upload_note(NULL, FALSE));
  g_autofree gchar *invalid = gh_attachment_transfer_sanitize_name("\xff\xfe");
  g_assert_null(invalid);
  GString *longname = g_string_new(NULL);
  for (guint i = 0; i < 300; i++)
    g_string_append(longname, "\xc3\xa9");   /* é: two bytes */
  g_string_append(longname, ".pdf");
  g_autofree gchar *cut = gh_attachment_transfer_sanitize_name(longname->str);
  g_assert_cmpuint(strlen(cut), <=, 200);
  g_assert_true(g_utf8_validate(cut, -1, NULL));
  g_assert_true(g_str_has_suffix(cut, ".pdf"));
  g_string_free(longname, TRUE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  mls_world_init();
  g_test_add_func("/groundhog/mls-files/send-receive-on-request", test_send_receive_on_request);
  g_test_add_func("/groundhog/mls-files/epoch-change-reseals", test_epoch_change_reseals);
  g_test_add_func("/groundhog/mls-files/stale-epoch-refused", test_stale_epoch_refused);
  g_test_add_func("/groundhog/mls-files/tor", test_tor);
  g_test_add_func("/groundhog/mls-files/legacy-group-has-no-picture",
                  test_legacy_group_has_no_picture);
  g_test_add_func("/groundhog/mls-files/adopted-picture", test_adopted_picture);
  g_test_add_func("/groundhog/mls-files/picture-policy", test_picture_policy);
  g_test_add_func("/groundhog/mls-files/names", test_names);
  int status = g_test_run();
  mls_world_finish();
  return status;
}
