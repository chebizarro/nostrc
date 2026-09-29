/* G21 transfer tests (privacy charter §6, §9.2 AT-1..AT-3, AT-5..AT-9) on a
 * local Blossom server (tests/media/blossom-fixture.c, SoupServer on
 * 127.0.0.1) and the SOCKS5 fixture (H3). Every wait spins the main context
 * until a condition holds; nothing sleeps. The process runs with private XDG
 * directories and TMPDIR under one root, which the H7 canary scanner reads at
 * the end of the tests that handle plaintext (AT-5). */
#include "gh-attachment.h"
#include "gh-message.h"
#include "gh-store-media.h"
#include "blossom-fixture.h"
#include "canary-scan.h"
#include "socks5-fixture.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define ONION "groundhogtestblobqw4yvkpvrbwe5wtamgzhhdrhm5k2ww7m3tgdwbzdyd.onion"
#define GPS_CANARY "GPS-CANARY-47.3769N-8.5417E-blossom"

static gchar *root;
static const gchar *const xdg_names[] = { "data", "config", "cache", "runtime", "state", "tmp" };
static const gchar *const xdg_vars[] = { "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                         "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "TMPDIR" };
static const gchar *const alice = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const gchar *const bob = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

static gchar *
root_dir(const gchar *name)
{
  return g_build_filename(root, name, NULL);
}

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

/* ---- a plaintext that must never reach the disk ---------------------------------- */

static gchar *file_canary;

/* A JPEG of about size bytes: APP0, an APP1 EXIF with a GPS canary, a frame
 * header and entropy data that carries file_canary (the plaintext H7 looks
 * for) and never a marker. */
static GBytes *
make_jpeg(gsize size)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00,
  };
  g_byte_array_append(out, head, sizeof head);
  g_autofree gchar *exif = g_strdup_printf("Exif%c%cMM GPSInfo %s", 0, 0, GPS_CANARY);
  gsize exif_size = 6 + strlen(exif + 6);
  guint8 app1[4] = { 0xFF, 0xE1, (guint8)((exif_size + 2) >> 8), (guint8)(exif_size + 2) };
  g_byte_array_append(out, app1, 4);
  g_byte_array_append(out, (const guint8 *)exif, (guint)exif_size);
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x03, 0x00, 0x04, 0x00, 1, 1, 0x11,
                                0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  guint32 seed = 7;
  while (out->len + 2 < size) {
    if (out->len % 65536 == 1000)
      g_byte_array_append(out, (const guint8 *)file_canary, (guint)strlen(file_canary));
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

static gboolean
contains(GBytes *bytes, const gchar *needle)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  gsize n = strlen(needle);
  for (gsize i = 0; n <= size && i <= size - n; i++)
    if (memcmp(data + i, needle, n) == 0)
      return TRUE;
  return FALSE;
}

static gchar *
sha256_hex(GBytes *bytes)
{
  gsize size = 0;
  gconstpointer data = g_bytes_get_data(bytes, &size);
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
}

/* ---- the account signer (AT-6) ----------------------------------------------------- */

typedef struct {
  gchar *secret;
  gchar *pubkey;
  guint calls;
  gboolean lie; /* signs something else than asked */
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
  g_assert_cmpint(nostr_event_get_kind(event), ==, 24242);
  if (signer->lie)
    nostr_event_set_content(event, "Delete everything");
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
  signer->lie = FALSE;
}

static void
fake_signer_clear(FakeSigner *signer)
{
  g_free(signer->secret);
  g_free(signer->pubkey);
}

/* ---- fixture --------------------------------------------------------------------- */

typedef struct {
  GSettings *settings;
  GhNetHttp *http;
  GhBlossomClient *client;
  BlossomFixture *blossom;
  GhStore *store;       /* the account's encrypted store: the media cache */
  gchar *store_dir;
} Fixture;

static void
fixture_up(Fixture *f, const gchar *mode)
{
  memset(f, 0, sizeof *f);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "network-mode", mode);
  f->blossom = blossom_fixture_new();
  const gchar *servers[] = { blossom_fixture_url(f->blossom), NULL };
  g_settings_set_strv(f->settings, "blossom-servers", servers);
  f->http = gh_net_http_new(f->settings);
  f->client = gh_blossom_client_new(f->settings, f->http);
}

static GhStore *
fixture_store(Fixture *f)
{
  if (f->store)
    return f->store;
  guint8 raw[32];
  for (guint i = 0; i < sizeof raw; i++)
    raw[i] = (guint8)g_random_int();
  g_autoptr(GBytes) key = g_bytes_new(raw, sizeof raw);
  f->store_dir = root_dir("data");
  GhStoreConfig config = { .data_dir = f->store_dir, .account_pubkey = bob };
  g_autofree gchar *store_id = g_uuid_string_random();
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  return f->store;
}

static void
fixture_down(Fixture *f)
{
  g_clear_object(&f->client);
  g_clear_object(&f->http);
  drain();
  blossom_fixture_free(f->blossom);
  if (f->store) {
    gh_store_close(f->store);
    g_assert_true(gh_store_delete_files(f->store_dir, bob, NULL));
  }
  g_free(f->store_dir);
  g_settings_reset(f->settings, "network-mode");
  g_settings_reset(f->settings, "tor-socks-address");
  g_settings_reset(f->settings, "blossom-servers");
  g_clear_object(&f->settings);
}

typedef struct {
  gboolean done;
  GhNip17File *file;
  GBytes *bytes;
  gboolean cached;
  GError *error;
} Result;

static gboolean
result_done(gpointer data)
{
  return ((Result *)data)->done;
}

static void
result_clear(Result *r)
{
  gh_nip17_file_free(r->file);
  g_clear_pointer(&r->bytes, g_bytes_unref);
  g_clear_error(&r->error);
  memset(r, 0, sizeof *r);
}

static void
on_upload(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Result *r = data;
  r->file = gh_attachment_upload_finish(result, &r->error);
  r->done = TRUE;
}

static void
on_download(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Result *r = data;
  r->bytes = gh_attachment_download_finish(result, &r->cached, &r->error);
  r->done = TRUE;
}

static void
upload(Fixture *f, GBytes *file, Result *r, GCancellable *cancellable)
{
  result_clear(r);
  gh_attachment_upload_async(f->client, file, NULL, cancellable, on_upload, r);
  spin_until(result_done, r);
}

static void
download(Fixture *f, GhStore *cache, const GhNip17File *file, Result *r,
         GCancellable *cancellable)
{
  result_clear(r);
  gh_attachment_download_async(f->client, cache, file, cancellable, on_download, r);
  spin_until(result_done, r);
}

/* Files anywhere under the transient directories (never: P3, AT-5, AT-8). */
static void
assert_no_transient_files(const gchar *what)
{
  static const gchar *const transient[] = { "cache", "runtime", "tmp" };
  for (guint i = 0; i < G_N_ELEMENTS(transient); i++) {
    g_autofree gchar *dir = root_dir(transient[i]);
    CanaryScan *scan = canary_scan_new();
    guint files = 0;
    canary_scan_tree(scan, dir, &files);
    canary_scan_free(scan);
    if (files)
      g_error("%s: %u file(s) in %s", what, files, transient[i]);
  }
}

/* H7 (AT-5): the plaintext canary is nowhere on disk (the encrypted store
 * included) and in no captured log. */
static void
assert_no_plaintext(const gchar *what)
{
  CanaryScan *scan = canary_scan_new();
  canary_scan_add(scan, "file plaintext", file_canary);
  canary_scan_add(scan, "EXIF GPS", GPS_CANARY);
  guint files = 0;
  canary_scan_tree(scan, root, &files);
  canary_log_capture_sync();
  g_autofree gchar *logs = canary_log_capture_dup();
  canary_scan_text(scan, "logs", logs);
  g_assert_true(canary_scan_check_clean(scan, what));
  canary_scan_free(scan);
  assert_no_transient_files(what);
}

/* ---- tests ----------------------------------------------------------------------- */

/* D6: no server chosen, nothing contacted, an honest error. */
static void
test_no_server(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_settings_set_strv(f.settings, "blossom-servers", NULL);
  g_autoptr(GBytes) jpeg = make_jpeg(4096);
  Result r = { 0 };
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  /* Blank entries are no servers either. */
  const gchar *blank[] = { "", "  ", NULL };
  g_settings_set_strv(f.settings, "blossom-servers", blank);
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  result_clear(&r);
  fixture_down(&f);
}

/* AT-1 (transfer half; the relay half is test-groundhog-privacy-e2e): a
 * 3 MiB JPEG with EXIF GPS goes to the local Blossom server and back through
 * a kind-15 rumor, and AT-7: receiving it fetches nothing until Download. */
static void
test_at1_round_trip(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GBytes) jpeg = make_jpeg(3 * 1024 * 1024);
  g_assert_true(contains(jpeg, GPS_CANARY));
  g_autoptr(GhAttachmentPrepared) expected = gh_attachment_prepare(jpeg, NULL, 1 << 25, NULL);

  Result r = { 0 };
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  GhNip17File *sent = r.file;
  g_autofree gchar *url = g_strdup_printf("%s/%s", blossom_fixture_url(f.blossom), sent->x);
  g_assert_cmpstr(sent->url, ==, url);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(f.blossom), 0);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_x, ==, sent->x);
  g_assert_cmpstr(put->auth_server, ==, "127.0.0.1");
  g_assert_cmpstr(put->auth_pubkey, !=, alice);
  /* The server holds ciphertext only, and it is exactly x. */
  GBytes *blob = blossom_fixture_get_blob(f.blossom, sent->x);
  g_assert_nonnull(blob);
  g_autofree gchar *x = sha256_hex(blob);
  g_assert_cmpstr(x, ==, sent->x);
  g_assert_false(contains(blob, file_canary));
  g_assert_false(contains(blob, "JFIF"));

  /* Sent as a kind-15 rumor, received as a message: no request (AT-7). */
  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor = gh_nip17_file_rumor_new(alice, bob, sent, 1700000000, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(bob, rumor, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip17File) received = gh_message_dup_file(message);
  g_assert_nonnull(received);
  drain();
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 0);

  /* Bob's inbox stores the message (the cache keeps only files a stored
   * message names), then Download (the user's consent): x, decrypt, ox;
   * into the encrypted cache. */
  g_autofree gchar *id = NULL;
  g_autofree gchar *stored = gh_nip17_file_rumor_new(alice, bob, sent, 1700000000, 0, &id, &error);
  g_assert_no_error(error);
  g_autofree gchar *room = strcmp(alice, bob) < 0 ? g_strconcat(alice, ",", bob, NULL)
                                                  : g_strconcat(bob, ",", alice, NULL);
  g_autofree gchar *wrap = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
  const gchar *participants[] = { alice, bob, NULL };
  GhStoreMessage admitted = {
    .backend = GH_STORE_BACKEND_NIP17, .backend_key = room, .backend_msg_id = id,
    .wrap_id = wrap, .sender_pubkey = alice, .kind = GH_NIP17_FILE_KIND,
    .created_at = 1700000000, .direction = GH_STORE_DIRECTION_IN, .body = sent->url,
    .raw_json = stored, .participants = participants,
  };
  g_assert_true(gh_store_admit(fixture_store(&f), &admitted, NULL, NULL, &error));
  g_assert_no_error(error);
  download(&f, fixture_store(&f), received, &r, NULL);
  g_assert_no_error(r.error);
  g_assert_false(r.cached);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  g_assert_true(g_bytes_equal(r.bytes, expected->plaintext)); /* the stripped original */
  g_assert_false(contains(r.bytes, GPS_CANARY));              /* no APP1 */
  g_assert_false(contains(r.bytes, "Exif"));
  g_assert_true(contains(r.bytes, file_canary));
  g_autofree gchar *ox = sha256_hex(r.bytes);
  g_assert_cmpstr(ox, ==, received->ox);
  /* A second Download comes from the encrypted cache: no request. */
  download(&f, fixture_store(&f), received, &r, NULL);
  g_assert_no_error(r.error);
  g_assert_true(r.cached);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  g_assert_true(g_bytes_equal(r.bytes, expected->plaintext));
  /* Control: the scanner does see this plaintext where it would leak. */
  g_autofree gchar *control = g_build_filename(root, "tmp", "control", NULL);
  g_assert_true(g_file_set_contents(control, g_bytes_get_data(r.bytes, NULL),
                                    (gssize)g_bytes_get_size(r.bytes), NULL));
  CanaryScan *scan = canary_scan_new();
  canary_scan_add(scan, "file plaintext", file_canary);
  g_assert_cmpuint(canary_scan_tree(scan, root, NULL), >, 0);
  canary_scan_free(scan);
  g_assert_cmpint(g_unlink(control), ==, 0);
  result_clear(&r);
  gh_store_checkpoint(f.store, NULL);
  assert_no_plaintext("AT-1/AT-5");
  fixture_down(&f);
}

/* AT-2 over the wire: a server that flips one bit is caught by x before any
 * decryption, and nothing is cached. */
static void
test_at2_tampered_download(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GBytes) jpeg = make_jpeg(64 * 1024);
  Result r = { 0 };
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_autoptr(GhNip17File) file = g_steal_pointer(&r.file);
  gsize size = 0;
  guint8 *flipped = g_memdup2(g_bytes_get_data(blossom_fixture_get_blob(f.blossom, file->x),
                                               &size), size);
  flipped[size / 2] ^= 0x04;
  g_autoptr(GBytes) tampered = g_bytes_new_take(flipped, size);
  blossom_fixture_put_blob(f.blossom, file->x, tampered);
  download(&f, fixture_store(&f), file, &r, NULL);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  g_assert_null(r.bytes);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_store_media_get(f.store, file->x, NULL, &error));
  g_assert_no_error(error);
  result_clear(&r);
  fixture_down(&f);
}

/* AT-3: the download aborts at the cap (+ 1 KiB), with or without a
 * Content-Length; a size tag over the cap is refused before any request, and
 * so is an upload over it. */
static void
test_at3_size_cap(void)
{
  Fixture f;
  fixture_up(&f, "none");
  gh_blossom_client_set_max_file_size(f.client, 64 * 1024);
  const gsize cap = 64 * 1024 + 16;
  g_autoptr(GBytes) huge = g_bytes_new_take(g_malloc0(cap + GH_BLOSSOM_DOWNLOAD_SLACK + 1),
                                            cap + GH_BLOSSOM_DOWNLOAD_SLACK + 1);
  g_autofree gchar *x = sha256_hex(huge);
  blossom_fixture_put_blob(f.blossom, x, huge);
  GhNip17File file = { 0 };
  file.url = g_strdup_printf("%s/%s", blossom_fixture_url(f.blossom), x);
  file.file_type = g_strdup("application/octet-stream");
  file.nonce_size = 12;
  g_strlcpy(file.x, x, sizeof file.x);
  Result r = { 0 };
  for (guint chunked = 0; chunked < 2; chunked++) {
    blossom_fixture_set_chunked(f.blossom, chunked);
    download(&f, NULL, &file, &r, NULL);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  }
  /* Exactly the cap + slack is still read (then refused as damaged: the
   * bytes are not a real ciphertext of this file). */
  g_autoptr(GBytes) fits = g_bytes_new_from_bytes(huge, 0, cap + GH_BLOSSOM_DOWNLOAD_SLACK);
  g_autofree gchar *fits_x = sha256_hex(fits);
  blossom_fixture_put_blob(f.blossom, fits_x, fits);
  GhNip17File fit = file;
  fit.url = g_strdup_printf("%s/%s", blossom_fixture_url(f.blossom), fits_x);
  g_strlcpy(fit.x, fits_x, sizeof fit.x);
  download(&f, NULL, &fit, &r, NULL);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_DAMAGED);
  /* A size tag caps it lower: size + 1 KiB. */
  file.size = 1000;
  download(&f, NULL, &file, &r, NULL);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  /* A size tag over the cap: no request at all. */
  guint gets = blossom_fixture_count(f.blossom, "GET");
  file.size = cap + 1;
  download(&f, NULL, &file, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, gets);
  /* An upload over the cap: refused before any request. */
  g_autoptr(GBytes) big = make_jpeg(80 * 1024);
  upload(&f, big, &r, NULL);
  g_assert_error(r.error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_TOO_LARGE);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 0);
  /* The default cap is D6's 25 MiB. */
  GhBlossomClient *fresh = gh_blossom_client_new(NULL, f.http);
  g_assert_cmpuint(gh_blossom_client_get_max_file_size(fresh), ==, 25 * 1024 * 1024);
  g_object_unref(fresh);
  g_free(file.url);
  g_free(file.file_type);
  g_free(fit.url);
  result_clear(&r);
  fixture_down(&f);
}

/* AT-6: the kind-24242 pubkey is never the account's unless the user
 * consented for that server; the account signer is not called otherwise. A
 * server that demands a known pubkey ends the upload (no other server gets
 * the file) until the user decides. */
static void
test_at6_upload_auth_key(void)
{
  Fixture f;
  fixture_up(&f, "none");
  BlossomFixture *second = blossom_fixture_new();
  const gchar *servers[] = { blossom_fixture_url(f.blossom), blossom_fixture_url(second), NULL };
  g_settings_set_strv(f.settings, "blossom-servers", servers);
  FakeSigner signer;
  fake_signer_init(&signer);
  gh_blossom_client_set_account_signer(f.client, signer.pubkey, fake_sign_async,
                                       fake_sign_finish, &signer, NULL);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024);
  Result r = { 0 };

  /* Default: a throwaway key, new for every upload. */
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_assert_cmpuint(signer.calls, ==, 0);
  GPtrArray *requests = blossom_fixture_requests(f.blossom);
  g_assert_cmpuint(requests->len, ==, 2);
  BlossomRequest *one = g_ptr_array_index(requests, 0), *two = g_ptr_array_index(requests, 1);
  g_assert_true(one->auth_valid && two->auth_valid);
  g_assert_cmpstr(one->auth_pubkey, !=, signer.pubkey);
  g_assert_cmpstr(two->auth_pubkey, !=, signer.pubkey);
  g_assert_cmpstr(one->auth_pubkey, !=, two->auth_pubkey);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_cmpint(one->auth_expiration, >, now);
  g_assert_cmpint(one->auth_expiration, <=, now + GH_BLOSSOM_AUTH_LIFETIME_S + 5);

  /* The server wants the account: the upload stops there, unsigned by it. */
  blossom_fixture_require_pubkey(f.blossom, signer.pubkey);
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(signer.calls, ==, 0);
  g_assert_cmpuint(blossom_fixture_count(second, NULL), ==, 0);
  /* Consent for another server changes nothing here. */
  gh_blossom_client_set_account_consent(f.client, blossom_fixture_url(second), TRUE);
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(signer.calls, ==, 0);

  /* With consent for this server (as the preferences store it, trailing
   * slash and all): the account signs, once. */
  g_autofree gchar *with_slash = g_strconcat(blossom_fixture_url(f.blossom), "/", NULL);
  gh_blossom_client_set_account_consent(f.client, with_slash, TRUE);
  g_assert_true(gh_blossom_client_get_account_consent(f.client, blossom_fixture_url(f.blossom)));
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_assert_cmpuint(signer.calls, ==, 1);
  BlossomRequest *last = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(last->auth_pubkey, ==, signer.pubkey);
  g_assert_true(last->auth_valid);

  /* A signer that signs something else is caught before any request. */
  guint puts = blossom_fixture_count(f.blossom, "PUT");
  signer.lie = TRUE;
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, puts);
  signer.lie = FALSE;

  /* A new account signer (a new generation) forgets every consent. */
  gh_blossom_client_set_account_signer(f.client, signer.pubkey, fake_sign_async,
                                       fake_sign_finish, &signer, NULL);
  g_assert_false(gh_blossom_client_get_account_consent(f.client, blossom_fixture_url(f.blossom)));
  guint calls = signer.calls;
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(signer.calls, ==, calls);

  /* A server that fails otherwise hands over to the next one. */
  blossom_fixture_require_pubkey(f.blossom, NULL);
  blossom_fixture_set_lie(f.blossom, "0000000000000000000000000000000000000000000000000000000000000000");
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_assert_true(g_str_has_prefix(r.file->url, blossom_fixture_url(second)));
  g_assert_cmpuint(blossom_fixture_count(second, "PUT"), ==, 1);
  result_clear(&r);
  gh_blossom_client_set_account_signer(f.client, NULL, NULL, NULL, NULL, NULL);
  fake_signer_clear(&signer);
  blossom_fixture_free(second);
  fixture_down(&f);
}

typedef struct {
  BlossomFixture *blossom;
  guint count;
} HeldWait;

static gboolean
held(gpointer data)
{
  HeldWait *wait = data;
  return blossom_fixture_held(wait->blossom) >= wait->count;
}

/* AT-8: cancelling mid-transfer ends the upload or download with
 * G_IO_ERROR_CANCELLED, keeps nothing and writes no file. */
static void
test_at8_cancel(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GBytes) jpeg = make_jpeg(512 * 1024);
  Result r = { 0 };
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_autoptr(GhNip17File) file = g_steal_pointer(&r.file);

  /* Upload: the server has the body and holds its answer. */
  blossom_fixture_set_hold(f.blossom, TRUE);
  g_autoptr(GCancellable) up = g_cancellable_new();
  result_clear(&r);
  gh_attachment_upload_async(f.client, jpeg, NULL, up, on_upload, &r);
  HeldWait wait = { f.blossom, 1 };
  spin_until(held, &wait);
  g_cancellable_cancel(up);
  spin_until(result_done, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(r.file);
  blossom_fixture_release_held(f.blossom);
  blossom_fixture_set_hold(f.blossom, FALSE);

  /* Download: the first chunk arrived, the rest never does. */
  blossom_fixture_set_stall(f.blossom, TRUE);
  g_autoptr(GCancellable) down = g_cancellable_new();
  result_clear(&r);
  gh_attachment_download_async(f.client, fixture_store(&f), file, down, on_download, &r);
  spin_until(held, &wait);
  drain();
  g_assert_false(r.done);
  g_cancellable_cancel(down);
  spin_until(result_done, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(r.bytes);
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_store_media_get(f.store, file->x, NULL, &error));
  blossom_fixture_release_held(f.blossom);
  blossom_fixture_set_stall(f.blossom, FALSE);
  drain();
  result_clear(&r);
  assert_no_transient_files("AT-8");
  fixture_down(&f);
}

/* AT-9: in Tor mode the upload and the download go through the SOCKS5 proxy
 * (H3) with the host name (remote DNS), each on its own circuit; and a .onion
 * server is refused outside Tor mode without being contacted. */
static void
test_at9_tor(void)
{
  Fixture f;
  fixture_up(&f, "tor");
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, blossom_fixture_port(f.blossom));
  g_settings_set_string(f.settings, "tor-socks-address", socks5_fixture_address(socks));
  g_autofree gchar *onion = g_strdup_printf("http://" ONION ":%u", blossom_fixture_port(f.blossom));
  const gchar *servers[] = { onion, NULL };
  g_settings_set_strv(f.settings, "blossom-servers", servers);
  g_autoptr(GBytes) jpeg = make_jpeg(32 * 1024);
  Result r = { 0 };
  upload(&f, jpeg, &r, NULL);
  g_assert_no_error(r.error);
  g_autoptr(GhNip17File) file = g_steal_pointer(&r.file);
  g_assert_true(g_str_has_prefix(file->url, onion));
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(f.blossom), 0);
  g_assert_cmpstr(put->auth_server, ==, ONION);
  download(&f, NULL, file, &r, NULL);
  g_assert_no_error(r.error);
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 2);
  for (guint i = 0; i < requests->len; i++) {
    Socks5Request *request = g_ptr_array_index(requests, i);
    g_assert_cmpuint(request->method, ==, 0x02);
    g_assert_cmpuint(request->atyp, ==, 0x03);
    g_assert_cmpstr(request->host, ==, ONION);
    g_assert_cmpuint(request->port, ==, blossom_fixture_port(f.blossom));
    g_assert_cmpuint(strlen(request->username), ==, 16);
  }
  Socks5Request *a = g_ptr_array_index(requests, 0), *b = g_ptr_array_index(requests, 1);
  g_assert_cmpstr(a->username, !=, b->username); /* a fresh circuit per request */
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);

  /* No Proxy mode: the .onion server is refused, nothing connects. */
  g_settings_set_string(f.settings, "network-mode", "none");
  upload(&f, jpeg, &r, NULL);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  download(&f, NULL, file, &r, NULL);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpuint(requests->len, ==, 2);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 2);
  /* Tor mode with the proxy gone: fails closed, never direct. */
  g_settings_set_string(f.settings, "network-mode", "tor");
  const gchar *direct[] = { blossom_fixture_url(f.blossom), NULL };
  g_settings_set_strv(f.settings, "blossom-servers", direct);
  socks5_fixture_set_refuse(socks, TRUE);
  upload(&f, jpeg, &r, NULL);
  g_assert_nonnull(r.error);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 2);
  result_clear(&r);
  socks5_fixture_free(socks);
  fixture_down(&f);
}

/* ---- main ------------------------------------------------------------------------ */

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

int
main(int argc, char **argv)
{
  /* Private XDG directories and TMPDIR under one root before GLib reads them. */
  const gchar *base = g_getenv("TMPDIR");
  gchar *template = g_build_filename(base && *base ? base : "/tmp", "groundhog-blossom-XXXXXX",
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
  g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
  g_setenv("GIO_USE_VFS", "local", TRUE);
  g_test_init(&argc, &argv, NULL);
  canary_log_capture_install();
  g_autofree gchar *uuid = g_uuid_string_random();
  file_canary = g_strdup_printf("G21-FILE-PLAINTEXT-CANARY-%s", uuid);
  g_test_add_func("/groundhog/blossom/no-server", test_no_server);
  g_test_add_func("/groundhog/blossom/at1-round-trip", test_at1_round_trip);
  g_test_add_func("/groundhog/blossom/at2-tampered-download", test_at2_tampered_download);
  g_test_add_func("/groundhog/blossom/at3-size-cap", test_at3_size_cap);
  g_test_add_func("/groundhog/blossom/at6-upload-auth-key", test_at6_upload_auth_key);
  g_test_add_func("/groundhog/blossom/at8-cancel", test_at8_cancel);
  g_test_add_func("/groundhog/blossom/at9-tor", test_at9_tor);
  int status = g_test_run();
  canary_log_capture_uninstall();
  remove_tree(root);
  g_free(root);
  g_free(file_canary);
  return status;
}
