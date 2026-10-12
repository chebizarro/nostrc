/* GhIssueUploader (nostrc-8xfib.5): public issue attachments over
 * GhBlossomClient and GhNetHttp, against the local Blossom fixture
 * (tests/media/blossom-fixture.c) and the SOCKS5 fixture only. Every wait
 * spins the main context until a condition holds; nothing sleeps. */
#include "gh-issue-uploader.h"
#include "blossom-fixture.h"
#include "socks5-fixture.h"
#include "../gh-test-port.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <stdlib.h>
#include <string.h>

#define ONION "groundhogtestblobqw4yvkpvrbwe5wtamgzhhdrhm5k2ww7m3tgdwbzdyd.onion"
#define GPS_CANARY "GPS-CANARY-47.3769N-8.5417E-issue"

static gchar *root;

typedef gboolean (*Cond)(gpointer data);

static gboolean
spin_expired(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

/* The deadline is a failure bound only. */
static void
spin_until_at(Cond cond, gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(30, spin_expired, &expired);
  while (!cond(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 30s", line);
  g_source_remove(timer);
}
#define spin_until(cond, data) spin_until_at((cond), (data), __LINE__)

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

/* A small JPEG with an APP1 EXIF block that carries GPS_CANARY. */
static GBytes *
make_jpeg(gsize size)
{
  GByteArray *out = g_byte_array_new();
  static const guint8 head[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x07, 'J', 'F', 'I', 'F', 0x00 };
  g_byte_array_append(out, head, sizeof head);
  g_autofree gchar *exif = g_strdup_printf("Exif%c%cMM GPSInfo %s", 0, 0, GPS_CANARY);
  gsize exif_size = 6 + strlen(exif + 6);
  guint8 app1[4] = { 0xFF, 0xE1, (guint8)((exif_size + 2) >> 8), (guint8)(exif_size + 2) };
  g_byte_array_append(out, app1, 4);
  g_byte_array_append(out, (const guint8 *)exif, (guint)exif_size);
  static const guint8 sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 8, 0x03, 0x00, 0x04, 0x00, 1, 1, 0x11, 0 };
  g_byte_array_append(out, sof, sizeof sof);
  static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 63, 0 };
  g_byte_array_append(out, sos, sizeof sos);
  guint32 seed = 7;
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

static GFile *
write_file(const gchar *name, GBytes *bytes)
{
  g_autofree gchar *path = g_build_filename(root, name, NULL);
  gsize size = 0;
  const gchar *data = g_bytes_get_data(bytes, &size);
  g_assert_true(g_file_set_contents(path, data, (gssize)size, NULL));
  return g_file_new_for_path(path);
}

typedef struct {
  GSettings *settings;
  GhNetHttp *http;
  GhBlossomClient *client;
  BlossomFixture *blossom;
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
  /* The fixture is on loopback, which a public issue's server may not be. */
  gh_blossom_client_set_allow_private_hosts(f->client, TRUE);
}

static void
fixture_down(Fixture *f)
{
  g_clear_object(&f->client);
  g_clear_object(&f->http);
  drain();
  blossom_fixture_free(f->blossom);
  g_settings_reset(f->settings, "network-mode");
  g_settings_reset(f->settings, "tor-socks-address");
  g_settings_reset(f->settings, "blossom-servers");
  g_clear_object(&f->settings);
}

typedef struct {
  gboolean done;
  gchar *url;
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
  g_clear_pointer(&r->url, g_free);
  g_clear_error(&r->error);
  r->done = FALSE;
}

static void
on_upload(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *r = data;
  r->url = gn_issue_uploader_upload_finish(GN_ISSUE_UPLOADER(source), result, &r->error);
  r->done = TRUE;
}

static void
upload(GnIssueUploader *uploader, GFile *file, GCancellable *cancellable, Result *r)
{
  result_clear(r);
  gn_issue_uploader_upload_async(uploader, file, "image/jpeg", cancellable, on_upload, r);
  spin_until(result_done, r);
}

/* No server the user chose, or none with a public host: no uploader, so the
 * form offers no files. Never a built-in server. Nothing is contacted. */
static void
test_no_server(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_assert_null(gh_issue_uploader_new(NULL));
  g_settings_set_strv(f.settings, "blossom-servers", NULL);
  g_assert_null(gh_issue_uploader_new(f.client));

  gh_blossom_client_set_allow_private_hosts(f.client, FALSE);
  const gchar *private_only[] = { blossom_fixture_url(f.blossom), "https://192.168.1.5",
                                  "https://printer.local", "https://localhost", NULL };
  g_settings_set_strv(f.settings, "blossom-servers", private_only);
  g_assert_null(gh_issue_uploader_new(f.client));

  const gchar *mixed[] = { "https://10.0.0.1", "https://Blossom.Example.com/", NULL };
  g_settings_set_strv(f.settings, "blossom-servers", mixed);
  g_autoptr(GnIssueUploader) one = gh_issue_uploader_new(f.client);
  g_assert_nonnull(one);
  const gchar *const *servers = gh_issue_uploader_get_servers(GH_ISSUE_UPLOADER(one));
  g_assert_cmpuint(g_strv_length((gchar **)servers), ==, 1);
  g_assert_cmpstr(servers[0], ==, "https://blossom.example.com");
  g_assert_cmpstr(gn_issue_uploader_describe_destination(one), ==, "https://blossom.example.com");

  const gchar *two[] = { "https://a.example.com", "https://b.example.com", NULL };
  g_settings_set_strv(f.settings, "blossom-servers", two);
  g_autoptr(GnIssueUploader) both = gh_issue_uploader_new(f.client);
  const gchar *destination = gn_issue_uploader_describe_destination(both);
  g_assert_nonnull(strstr(destination, "https://a.example.com, https://b.example.com"));
  /* Snapshotted: a later settings change does not widen the consent. */
  const gchar *other[] = { "https://c.example.com", NULL };
  g_settings_set_strv(f.settings, "blossom-servers", other);
  g_assert_cmpuint(g_strv_length((gchar **)gh_issue_uploader_get_servers(GH_ISSUE_UPLOADER(both))),
                   ==, 2);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  fixture_down(&f);
}

/* Nothing is contacted until the form asks for an upload (after consent);
 * then the file goes once, metadata stripped, with a throwaway-key
 * authorization and its sniffed Content-Type, and the address is the user's
 * server's BUD-01 blob with the type's extension (nostrc-u42c7). */
static void
test_round_trip(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  g_assert_nonnull(uploader);
  g_assert_cmpstr(gn_issue_uploader_describe_destination(uploader), ==,
                  blossom_fixture_url(f.blossom));
  g_autoptr(GBytes) jpeg = make_jpeg(16 * 1024);
  g_assert_true(contains(jpeg, GPS_CANARY));
  g_autoptr(GFile) file = write_file("photo.jpg", jpeg);
  drain();
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);

  Result r = { 0 };
  upload(uploader, file, NULL, &r);
  g_assert_no_error(r.error);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 1);
  g_autofree gchar *prefix = g_strconcat(blossom_fixture_url(f.blossom), "/", NULL);
  g_assert_true(g_str_has_prefix(r.url, prefix));
  g_assert_cmpuint(strlen(r.url), ==, strlen(prefix) + 64 + strlen(".jpg"));
  g_assert_true(g_str_has_suffix(r.url, ".jpg"));
  g_autofree gchar *sha = g_strndup(r.url + strlen(prefix), 64);
  GBytes *stored = blossom_fixture_get_blob(f.blossom, sha);
  g_assert_nonnull(stored);
  g_assert_false(contains(stored, GPS_CANARY));
  g_assert_cmpuint(g_bytes_get_size(stored), <, g_bytes_get_size(jpeg));
  g_autofree gchar *stored_sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, stored);
  g_assert_cmpstr(stored_sha, ==, sha);
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(f.blossom), 0);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_x, ==, sha);
  g_assert_cmpstr(put->x_sha256, ==, sha);
  g_assert_cmpstr(put->content_type, ==, "image/jpeg");
  result_clear(&r);
  fixture_down(&f);
}

/* ---- Content type and address (nostrc-u42c7) -------------------------------------- */

static GBytes *
bytes_of(const void *data, gsize size)
{
  return g_bytes_new(data, size);
}

static void
assert_sniff(GBytes *bytes, const gchar *type, const gchar *extension)
{
  const gchar *got_extension = "unset";
  g_assert_cmpstr(gh_issue_uploader_sniff(bytes, &got_extension), ==, type);
  g_assert_cmpstr(got_extension, ==, extension);
}

/* The type is the bytes' alone, from an allowlist; markup is text. */
static void
test_sniff(void)
{
  g_autoptr(GBytes) jpeg = make_jpeg(1024);
  assert_sniff(jpeg, "image/jpeg", "jpg");
  static const guint8 png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13 };
  g_autoptr(GBytes) png_bytes = bytes_of(png, sizeof png);
  assert_sniff(png_bytes, "image/png", "png");
  g_autoptr(GBytes) gif = bytes_of("GIF89a\x01\x00\x01\x00", 10);
  assert_sniff(gif, "image/gif", "gif");
  g_autoptr(GBytes) webp = bytes_of("RIFF\x10\x00\x00\x00WEBPVP8 ", 16);
  assert_sniff(webp, "image/webp", "webp");
  g_autoptr(GBytes) pdf = bytes_of("%PDF-1.7\n\x80\x81", 11);
  assert_sniff(pdf, "application/pdf", "pdf");
  g_autoptr(GBytes) mp4 = bytes_of("\x00\x00\x00\x18" "ftypisom\x00\x00\x02\x00", 16);
  assert_sniff(mp4, "video/mp4", "mp4");
  g_autoptr(GBytes) quicktime = bytes_of("\x00\x00\x00\x14" "ftypqt  \x00\x00\x02\x00", 16);
  assert_sniff(quicktime, "application/octet-stream", NULL);
  g_autoptr(GBytes) webm = bytes_of("\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01\x42\x82\x84webm", 16);
  assert_sniff(webm, "video/webm", "webm");
  g_autoptr(GBytes) mkv = bytes_of("\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01\x42\x82\x88matroska", 20);
  assert_sniff(mkv, "application/octet-stream", NULL);
  const gchar *svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" onload=\"alert(1)\"/>\n";
  g_autoptr(GBytes) svg_bytes = bytes_of(svg, strlen(svg));
  assert_sniff(svg_bytes, "text/plain", "txt");
  const gchar *log = "{\"crash\": \"Grüße\"}\r\n\tend\f";
  g_autoptr(GBytes) log_bytes = bytes_of(log, strlen(log));
  assert_sniff(log_bytes, "text/plain", "txt");
  g_autoptr(GBytes) nul = bytes_of("text\0more", 9);
  assert_sniff(nul, "application/octet-stream", NULL);
  g_autoptr(GBytes) escape = bytes_of("text\x1b[31m", 9);
  assert_sniff(escape, "application/octet-stream", NULL);
  g_autoptr(GBytes) latin1 = bytes_of("caf\xe9", 4);
  assert_sniff(latin1, "application/octet-stream", NULL);
  /* A character cut at the end of the file is not text; one cut by the
   * 64 KiB bound is, and only the bound is read. */
  g_autoptr(GBytes) cut = bytes_of("caf\xc3", 4);
  assert_sniff(cut, "application/octet-stream", NULL);
  GByteArray *long_text = g_byte_array_new();
  for (guint i = 0; i < 64 * 1024 - 1; i++)
    g_byte_array_append(long_text, (const guint8 *)"a", 1);
  g_byte_array_append(long_text, (const guint8 *)"\xc3\xa9\0\0", 4);
  g_autoptr(GBytes) bounded = g_byte_array_free_to_bytes(long_text);
  assert_sniff(bounded, "text/plain", "txt");
  g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
  assert_sniff(empty, "application/octet-stream", NULL);
}

/* The name never decides: random bytes named .png go as an opaque file
 * without an extension, markup named .svg as text. The extension the
 * server's descriptor names is used when its url is that same blob on the
 * same server; any other url is ignored. */
static void
test_type_and_address(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  g_autofree gchar *prefix = g_strconcat(blossom_fixture_url(f.blossom), "/", NULL);
  Result r = { 0 };

  guint8 noise[256];
  for (guint i = 0; i < sizeof noise; i++)
    noise[i] = (guint8)(i * 151u + 7u);
  noise[0] = 0x00;
  g_autoptr(GBytes) noise_bytes = bytes_of(noise, sizeof noise);
  g_autoptr(GFile) fake_png = write_file("not-really.png", noise_bytes);
  upload(uploader, fake_png, NULL, &r);
  g_assert_no_error(r.error);
  g_autofree gchar *noise_sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, noise_bytes);
  g_autofree gchar *bare = g_strconcat(prefix, noise_sha, NULL);
  g_assert_cmpstr(r.url, ==, bare);
  GPtrArray *requests = blossom_fixture_requests(f.blossom);
  BlossomRequest *put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(put->content_type, ==, "application/octet-stream");

  const gchar *svg = "<svg onload=\"alert(1)\"/>\n";
  g_autoptr(GBytes) svg_bytes = bytes_of(svg, strlen(svg));
  g_autoptr(GFile) svg_file = write_file("image.svg", svg_bytes);
  upload(uploader, svg_file, NULL, &r);
  g_assert_no_error(r.error);
  g_autofree gchar *svg_sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, svg_bytes);
  g_autofree gchar *svg_url = g_strconcat(prefix, svg_sha, ".txt", NULL);
  g_assert_cmpstr(r.url, ==, svg_url);
  put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_cmpstr(put->content_type, ==, "text/plain");

  /* The server names its own extension for the blob. */
  g_autoptr(GBytes) jpeg = make_jpeg(4 * 1024);
  g_autoptr(GFile) photo = write_file("photo", jpeg);
  blossom_fixture_set_descriptor_url(f.blossom, NULL, "JPEG");
  upload(uploader, photo, NULL, &r);
  g_assert_no_error(r.error);
  g_assert_true(g_str_has_prefix(r.url, prefix));
  g_assert_true(g_str_has_suffix(r.url, ".jpeg"));
  g_autofree gchar *sha = g_strndup(r.url + strlen(prefix), 64);
  g_assert_nonnull(blossom_fixture_get_blob(f.blossom, sha));

  /* Another host, a bad extension or another path: the server's url is not
   * used, the address is still the user's server's blob with the type's
   * extension. */
  const gchar *bad[][2] = { { "https://cdn.example.com", "jpeg" }, { NULL, "j.peg" },
                            { NULL, "jpg?x=1" }, { NULL, "averyveryverylongextension" } };
  g_autofree gchar *expected = g_strconcat(prefix, sha, ".jpg", NULL);
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    blossom_fixture_set_descriptor_url(f.blossom, bad[i][0], bad[i][1]);
    upload(uploader, photo, NULL, &r);
    g_assert_no_error(r.error);
    g_assert_cmpstr(r.url, ==, expected);
  }
  g_autofree gchar *nested = g_strconcat(blossom_fixture_url(f.blossom), "/media", NULL);
  blossom_fixture_set_descriptor_url(f.blossom, nested, "jpeg");
  upload(uploader, photo, NULL, &r);
  g_assert_cmpstr(r.url, ==, expected);
  result_clear(&r);
  fixture_down(&f);
}

/* ---- "Use my account for this server" (nostrc-u42c7) -------------------------- */

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
  g_assert_cmpint(nostr_event_get_kind(event), ==, 24242);
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

typedef struct {
  GhBlossomClient *client;
  GPtrArray *servers; /* recorded */
  gboolean fail;
} Recorder;

/* What the application's recorder does (gh_attachments_set_consent()):
 * keep the consent, then set it on the client. */
static gboolean
record_consent(gpointer data, const gchar *server, GError **error)
{
  Recorder *recorder = data;
  if (recorder->fail) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "store closed");
    return FALSE;
  }
  g_ptr_array_add(recorder->servers, g_strdup(server));
  gh_blossom_client_set_account_consent(recorder->client, server, TRUE);
  return TRUE;
}

/* A server that only accepts accounts it knows refuses the throwaway key.
 * The upload consent offers, unchecked, to use the account for that server
 * only while the client has an account signer and no consent for it;
 * checking it records the account's kept consent, and the next upload is
 * signed by the account. Nothing signs as the account before that. */
static void
test_account_option(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  /* No account signer: nothing to offer. */
  g_assert_null(gn_issue_uploader_dup_consent_options(uploader));

  FakeSigner signer = { 0 };
  char *secret = nostr_key_generate_private();
  char *pubkey = nostr_key_get_public(secret);
  signer.secret = g_strdup(secret);
  signer.pubkey = g_strdup(pubkey);
  free(secret);
  free(pubkey);
  gh_blossom_client_set_account_signer(f.client, signer.pubkey, fake_sign_async, fake_sign_finish,
                                       &signer, NULL);
  blossom_fixture_require_pubkey(f.blossom, signer.pubkey);
  Recorder recorder = { f.client, g_ptr_array_new_with_free_func(g_free), FALSE };
  gh_issue_uploader_set_consent_recorder(GH_ISSUE_UPLOADER(uploader), record_consent, &recorder);

  g_autoptr(GBytes) jpeg = make_jpeg(4 * 1024);
  g_autoptr(GFile) photo = write_file("known.jpg", jpeg);
  Result r = { 0 };
  upload(uploader, photo, NULL, &r);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED);
  g_assert_cmpuint(signer.calls, ==, 0);
  BlossomRequest *put = g_ptr_array_index(blossom_fixture_requests(f.blossom), 0);
  g_assert_cmpstr(put->auth_pubkey, !=, signer.pubkey);

  g_auto(GStrv) options = gn_issue_uploader_dup_consent_options(uploader);
  g_assert_nonnull(options);
  g_assert_cmpuint(g_strv_length(options), ==, 1);
  g_assert_nonnull(strstr(options[0], "127.0.0.1"));
  g_assert_cmpuint(signer.calls, ==, 0);
  g_assert_cmpuint(recorder.servers->len, ==, 0);

  g_autoptr(GError) error = NULL;
  g_assert_false(gn_issue_uploader_apply_consent_option(uploader, 1, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  recorder.fail = TRUE;
  g_assert_false(gn_issue_uploader_apply_consent_option(uploader, 0, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
  g_assert_false(gh_blossom_client_get_account_consent(f.client, blossom_fixture_url(f.blossom)));
  recorder.fail = FALSE;
  g_assert_true(gn_issue_uploader_apply_consent_option(uploader, 0, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(recorder.servers->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(recorder.servers, 0), ==, blossom_fixture_url(f.blossom));
  g_assert_true(gh_blossom_client_get_account_consent(f.client, blossom_fixture_url(f.blossom)));

  upload(uploader, photo, NULL, &r);
  g_assert_no_error(r.error);
  g_assert_cmpuint(signer.calls, ==, 1);
  GPtrArray *requests = blossom_fixture_requests(f.blossom);
  put = g_ptr_array_index(requests, requests->len - 1);
  g_assert_true(put->auth_valid);
  g_assert_cmpstr(put->auth_pubkey, ==, signer.pubkey);
  g_assert_cmpstr(put->content_type, ==, "image/jpeg");
  g_assert_true(g_str_has_suffix(r.url, ".jpg"));
  /* Consented: nothing more to offer. */
  g_assert_null(gn_issue_uploader_dup_consent_options(uploader));

  result_clear(&r);
  gh_blossom_client_set_account_signer(f.client, NULL, NULL, NULL, NULL, NULL);
  g_ptr_array_unref(recorder.servers);
  g_free(signer.secret);
  g_free(signer.pubkey);
  fixture_down(&f);
}

/* Too large (before reading), not a regular file, empty, or cancelled:
 * refused with nothing sent. */
static void
test_refusals(void)
{
  Fixture f;
  fixture_up(&f, "none");
  gh_blossom_client_set_max_file_size(f.client, 1024 * 1024);
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  Result r = { 0 };

  g_autoptr(GBytes) big = make_jpeg(2 * 1024 * 1024);
  g_autoptr(GFile) large = write_file("large.jpg", big);
  upload(uploader, large, NULL, &r);
  g_assert_error(r.error, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE);

  g_autoptr(GFile) dir = g_file_new_for_path(root);
  upload(uploader, dir, NULL, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE);

  g_autoptr(GBytes) nothing = g_bytes_new_static("", 0);
  g_autoptr(GFile) empty = write_file("empty.log", nothing);
  upload(uploader, empty, NULL, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  g_autoptr(GFile) missing = g_file_new_build_filename(root, "missing.log", NULL);
  upload(uploader, missing, NULL, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  g_autoptr(GBytes) small = make_jpeg(4 * 1024);
  g_autoptr(GFile) ok = write_file("small.jpg", small);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  upload(uploader, ok, cancel, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  drain();
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 0);
  result_clear(&r);
  fixture_down(&f);
}

/* Tor mode: the upload goes through the Tor SOCKS proxy (by name, never
 * resolved here); No Proxy mode refuses the .onion without a request. */
static void
test_tor(void)
{
  Fixture f;
  fixture_up(&f, "tor");
  gh_blossom_client_set_allow_private_hosts(f.client, FALSE); /* .onion is public */
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, blossom_fixture_port(f.blossom));
  g_settings_set_string(f.settings, "tor-socks-address", socks5_fixture_address(socks));
  g_autofree gchar *onion = g_strdup_printf("http://" ONION ":%u", blossom_fixture_port(f.blossom));
  const gchar *servers[] = { onion, NULL };
  g_settings_set_strv(f.settings, "blossom-servers", servers);
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  g_assert_nonnull(uploader);
  g_autoptr(GBytes) jpeg = make_jpeg(8 * 1024);
  g_autoptr(GFile) file = write_file("tor.jpg", jpeg);
  Result r = { 0 };
  upload(uploader, file, NULL, &r);
  g_assert_no_error(r.error);
  g_assert_true(g_str_has_prefix(r.url, onion));
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, ==, 1);
  Socks5Request *request = g_ptr_array_index(requests, 0);
  g_assert_cmpuint(request->atyp, ==, 0x03);
  g_assert_cmpstr(request->host, ==, ONION);

  g_settings_set_string(f.settings, "network-mode", "none");
  upload(uploader, file, NULL, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpuint(requests->len, ==, 1);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, NULL), ==, 1);
  result_clear(&r);
  socks5_fixture_free(socks);
  fixture_down(&f);
}

/* ---- DNS rebinding: the connection is public-only ------------------------------- */

/* A resolver for one public-looking name that leads to 127.0.0.1. */
#define REBIND_HOST "rebind.groundhog.test"

#define REBIND_TYPE_RESOLVER (rebind_resolver_get_type())
G_DECLARE_FINAL_TYPE(RebindResolver, rebind_resolver, REBIND, RESOLVER, GResolver)
struct _RebindResolver {
  GResolver parent_instance;
  guint lookups;
};
G_DEFINE_FINAL_TYPE(RebindResolver, rebind_resolver, G_TYPE_RESOLVER)

static GList *
rebind_lookup_flags(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                    GCancellable *cancellable, GError **error)
{
  (void)cancellable;
  REBIND_RESOLVER(resolver)->lookups++;
  if (g_str_equal(name, REBIND_HOST) && !(flags & G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY))
    return g_list_append(NULL, g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4));
  g_set_error(error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "no address for %s", name);
  return NULL;
}

static GList *
rebind_lookup(GResolver *resolver, const gchar *name, GCancellable *cancellable, GError **error)
{
  return rebind_lookup_flags(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                             error);
}

static void
rebind_lookup_flags_async(GResolver *resolver, const gchar *name, GResolverNameLookupFlags flags,
                          GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new(resolver, cancellable, callback, data);
  GError *error = NULL;
  GList *addresses = rebind_lookup_flags(resolver, name, flags, cancellable, &error);
  if (addresses)
    g_task_return_pointer(task, addresses, (GDestroyNotify)g_resolver_free_addresses);
  else
    g_task_return_error(task, error);
}

static void
rebind_lookup_async(GResolver *resolver, const gchar *name, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer data)
{
  rebind_lookup_flags_async(resolver, name, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable,
                            callback, data);
}

static GList *
rebind_lookup_finish(GResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
rebind_resolver_class_init(RebindResolverClass *klass)
{
  GResolverClass *resolver = G_RESOLVER_CLASS(klass);
  resolver->lookup_by_name = rebind_lookup;
  resolver->lookup_by_name_async = rebind_lookup_async;
  resolver->lookup_by_name_finish = rebind_lookup_finish;
  resolver->lookup_by_name_with_flags = rebind_lookup_flags;
  resolver->lookup_by_name_with_flags_async = rebind_lookup_flags_async;
  resolver->lookup_by_name_with_flags_finish = rebind_lookup_finish;
}

static void
rebind_resolver_init(RebindResolver *self)
{
  (void)self;
}

static gboolean
on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
            gpointer data)
{
  (void)service;
  (void)source;
  (*(guint *)data)++;
  (void)g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

/* A server whose public-looking name passes the preflight but resolves to
 * 127.0.0.1 is refused where the connection is made: nothing connects. */
static void
test_rebinding(void)
{
  Fixture f;
  fixture_up(&f, "none");
  g_autoptr(GResolver) previous = g_resolver_get_default();
  g_autoptr(RebindResolver) resolver = g_object_new(REBIND_TYPE_RESOLVER, NULL);
  g_resolver_set_default(G_RESOLVER(resolver));
  guint accepted = 0;
  g_autoptr(GSocketService) listener = g_socket_service_new();
  guint16 port = gh_test_listen_loopback(G_SOCKET_LISTENER(listener));
  g_signal_connect(listener, "incoming", G_CALLBACK(on_incoming), &accepted);
  g_socket_service_start(listener);
  g_autofree gchar *server = g_strdup_printf("https://" REBIND_HOST ":%u", port);
  const gchar *servers[] = { server, NULL };
  g_settings_set_strv(f.settings, "blossom-servers", servers);
  gh_blossom_client_set_allow_private_hosts(f.client, FALSE);
  g_autoptr(GnIssueUploader) uploader = gh_issue_uploader_new(f.client);
  g_assert_nonnull(uploader); /* the name passes the preflight */
  g_autoptr(GBytes) jpeg = make_jpeg(4 * 1024);
  g_autoptr(GFile) file = write_file("rebind.jpg", jpeg);
  Result r = { 0 };
  upload(uploader, file, NULL, &r);
  g_assert_null(r.url);
  g_assert_nonnull(r.error);
  g_assert_cmpuint(resolver->lookups, >, 0);
  drain();
  g_assert_cmpuint(accepted, ==, 0);
  result_clear(&r);
  g_signal_handlers_disconnect_by_data(listener, &accepted);
  g_socket_service_stop(listener);
  drain();
  g_socket_listener_close(G_SOCKET_LISTENER(listener));
  g_resolver_set_default(previous);
  fixture_down(&f);
}

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
  g_autofree gchar *tmpl = g_build_filename(g_get_tmp_dir(), "gh-issue-uploader-XXXXXX", NULL);
  if (!g_mkdtemp(tmpl))
    g_error("cannot create the test root: %s", g_strerror(errno));
  root = g_strdup(tmpl);
  g_setenv("GIO_USE_VFS", "local", TRUE);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/issue-uploader/no-server", test_no_server);
  g_test_add_func("/groundhog/issue-uploader/round-trip", test_round_trip);
  g_test_add_func("/groundhog/issue-uploader/sniff", test_sniff);
  g_test_add_func("/groundhog/issue-uploader/type-and-address", test_type_and_address);
  g_test_add_func("/groundhog/issue-uploader/account-option", test_account_option);
  g_test_add_func("/groundhog/issue-uploader/refusals", test_refusals);
  g_test_add_func("/groundhog/issue-uploader/tor", test_tor);
  g_test_add_func("/groundhog/issue-uploader/rebinding", test_rebinding);
  int status = g_test_run();
  remove_tree(root);
  g_free(root);
  return status;
}
