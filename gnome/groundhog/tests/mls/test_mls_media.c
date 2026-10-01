/* Encrypted group attachments (gh-mls-media, nostrc-u7cb): encrypted-media-v2
 * through libmarmot, uploads and downloads through GhBlossomClient over
 * GhNetHttp to the local Blossom fixture. Only loopback is contacted. */

#include "blossom-fixture.h"
#include "gh-attachment.h"
#include "gh-mls-media.h"
#include "gh-mls-media-private.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const guint8 GID[32] = { 0x47, 0x48, 1, 2, 3 };
#define GID_HEX "4748010203000000000000000000000000000000000000000000000000000000"

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

typedef struct {
  gboolean done;
  GAsyncResult *result;
} Wait;

static void
on_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Wait *w = data;
  w->result = g_object_ref(result);
  w->done = TRUE;
}

static GAsyncResult *
wait_for(Wait *w)
{
  while (!w->done)
    g_main_context_iteration(NULL, TRUE);
  return w->result;
}

typedef struct {
  GSettings *settings;
  GhNetHttp *http;
  BlossomFixture *blossom;
  GhBlossomClient *client;
  MarmotStorage *storage; /* owned by marmot */
  Marmot *marmot;
} Fixture;

static void
set_epoch(Fixture *f, guint64 epoch)
{
  MarmotGroupId gid = marmot_group_id_new(GID, sizeof GID);
  guint8 secret[32];
  memset(secret, (int)(0x30 + epoch), sizeof secret);
  g_assert_cmpint(f->storage->save_exporter_secret(f->storage->ctx, &gid, epoch, secret), ==,
                  MARMOT_OK);
  MarmotGroup *g = marmot_group_new();
  g->mls_group_id = marmot_group_id_new(GID, sizeof GID);
  memset(g->nostr_group_id, 0xAA, 32);
  g->name = strdup("Attachments");
  g->state = MARMOT_GROUP_STATE_ACTIVE;
  g->epoch = epoch;
  g_assert_cmpint(f->storage->save_group(f->storage->ctx, g), ==, MARMOT_OK);
  marmot_group_free(g);
  marmot_group_id_free(&gid);
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "network-mode", "none");
  f->blossom = blossom_fixture_new();
  const gchar *servers[] = { blossom_fixture_url(f->blossom), NULL };
  g_settings_set_strv(f->settings, "blossom-servers", servers);
  f->http = gh_net_http_new(f->settings);
  f->client = gh_blossom_client_new(f->settings, f->http);
  /* The fixture is on loopback, which real downloads may not reach. */
  gh_blossom_client_set_allow_private_hosts(f->client, TRUE);
  f->storage = marmot_storage_memory_new();
  MarmotConfig config = marmot_config_default();
  f->marmot = marmot_new_with_config(f->storage, &config);
  g_assert_nonnull(f->marmot);
  set_epoch(f, 3);
}

static void
fixture_down(Fixture *f)
{
  marmot_free(f->marmot);
  g_clear_object(&f->client);
  g_clear_object(&f->http);
  drain();
  blossom_fixture_free(f->blossom);
  g_settings_reset(f->settings, "network-mode");
  g_settings_reset(f->settings, "blossom-servers");
  g_clear_object(&f->settings);
}

static GhMlsAttachment *
upload(Fixture *f, const GhMlsMediaSealed *sealed, GError **error)
{
  Wait w = { 0 };
  gh_mls_media_upload_async(f->client, sealed, NULL, on_done, &w);
  GhMlsAttachment *a = gh_mls_media_upload_finish(wait_for(&w), error);
  g_object_unref(w.result);
  return a;
}

static GBytes *
fetch(Fixture *f, const GhMlsAttachment *a, GError **error)
{
  Wait w = { 0 };
  gh_mls_media_fetch_async(f->client, a, NULL, on_done, &w);
  GBytes *b = gh_mls_media_fetch_finish(wait_for(&w), error);
  g_object_unref(w.result);
  return b;
}

/* Send to receive: seal (epoch 3), upload, imeta in an inner event, parse
 * it back with the message's epoch, then fetch and open after the group
 * moved to epoch 4 (the retained epoch-3 key). */
static void
test_roundtrip_through_blossom(void)
{
  Fixture f;
  fixture_up(&f);
  static const gchar text[] = "a plain document attached to a group message";
  g_autoptr(GBytes) file = g_bytes_new_static(text, sizeof text - 1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsMediaSealed) sealed =
    gh_mls_media_seal(f.marmot, GID_HEX, file, "Text/Plain; charset=utf-8", "notes.txt",
                      GH_BLOSSOM_MAX_FILE_SIZE, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(gh_mls_attachment_get_source_epoch(sealed->attachment), ==, 3);
  g_assert_cmpstr(gh_mls_attachment_get_media_type(sealed->attachment), ==, "text/plain");
  g_assert_cmpstr(gh_mls_attachment_get_filename(sealed->attachment), ==, "notes.txt");
  /* Not JPEG/PNG: sent unchanged, and the result says so (review M2). */
  g_assert_false(sealed->stripped);
  g_assert_true(sealed->may_have_metadata);
  /* Without a locator there is nothing to send yet. */
  g_auto(GStrv) none = gh_mls_attachment_dup_imeta(sealed->attachment, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_INVALID);
  g_assert_null(none);
  g_clear_error(&error);
  /* Only ciphertext reaches the server. */
  g_assert_null(memmem(g_bytes_get_data(sealed->ciphertext, NULL),
                       g_bytes_get_size(sealed->ciphertext), "plain document", 14));

  g_autoptr(GhMlsAttachment) sent = upload(&f, sealed, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "PUT"), ==, 1);
  g_auto(GStrv) urls = gh_mls_attachment_dup_blossom_urls(sent);
  g_assert_cmpuint(g_strv_length(urls), ==, 1);
  g_autofree gchar *sha = gh_mls_attachment_dup_ciphertext_sha256(sent);
  g_assert_true(g_str_has_suffix(urls[0], sha));
  g_assert_true(gh_mls_media_check_epoch(f.marmot, GID_HEX, sent, &error));
  g_assert_no_error(error);

  g_auto(GStrv) tag = gh_mls_attachment_dup_imeta(sent, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(tag[0], ==, "imeta");
  g_assert_cmpstr(tag[1], ==, "v encrypted-media-v2");
  g_assert_true(g_str_has_prefix(tag[2], "locator blossom-v1 "));
  g_assert_true(g_str_has_prefix(tag[3], "ciphertext_sha256 "));
  g_assert_true(g_str_has_prefix(tag[4], "plaintext_sha256 "));
  g_assert_true(g_str_has_prefix(tag[5], "nonce "));
  g_assert_cmpstr(tag[6], ==, "m text/plain");
  g_assert_cmpstr(tag[7], ==, "filename notes.txt");
  g_assert_null(tag[8]);

  /* The inner event: this tag, a v1 one and a broken one (each rejected
   * alone), and an unrelated tag. */
  g_autoptr(GString) json = g_string_new("{\"kind\":9,\"content\":\"see file\",\"created_at\":1,"
                                         "\"pubkey\":\"");
  for (int i = 0; i < 64; i++)
    g_string_append_c(json, 'a');
  g_string_append(json, "\",\"tags\":[[\"p\",\"x\"],[");
  for (guint i = 0; tag[i]; i++)
    g_string_append_printf(json, "%s\"%s\"", i ? "," : "", tag[i]);
  g_string_append(json, "],[\"imeta\",\"v encrypted-media-v1\"],[\"imeta\",\"m image/png\"]]}");
  guint rejected = 0;
  g_autoptr(GPtrArray) received = gh_mls_attachments_from_inner_event(json->str, 3, &rejected);
  g_assert_cmpuint(received->len, ==, 1);
  g_assert_cmpuint(rejected, ==, 2);
  GhMlsAttachment *got = g_ptr_array_index(received, 0);
  g_assert_cmpuint(gh_mls_attachment_get_source_epoch(got), ==, 3);

  /* A Commit lands: a pending send must seal again, but epoch-3 media
   * still opens. */
  set_epoch(&f, 4);
  g_assert_false(gh_mls_media_check_epoch(f.marmot, GID_HEX, sent, &error));
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_EPOCH_CHANGED);
  g_clear_error(&error);

  g_autoptr(GBytes) ciphertext = fetch(&f, got, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  GBytes *plain = gh_mls_media_open(f.marmot, GID_HEX, got, ciphertext, &error);
  g_assert_no_error(error);
  g_assert_cmpmem(g_bytes_get_data(plain, NULL), g_bytes_get_size(plain), text, sizeof text - 1);
  /* The decrypted file is wiped when its last reference goes (review L4). */
  gsize wiped = gh_mls_media_test_wiped_bytes();
  g_bytes_unref(plain);
  g_assert_cmpuint(gh_mls_media_test_wiped_bytes() - wiped, ==, sizeof text - 1);

  /* Damaged: other bytes under the attachment's name. */
  g_autoptr(GBytes) wrong = g_bytes_new_static("not the file", 12);
  g_autoptr(GBytes) bad = gh_mls_media_open(f.marmot, GID_HEX, got, wrong, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_DAMAGED);
  g_assert_null(bad);
  g_clear_error(&error);
  /* An epoch whose key is gone. */
  g_autoptr(GhMlsAttachment) old = gh_mls_attachment_copy(got);
  g_auto(GStrv) old_tag = gh_mls_attachment_dup_imeta(old, NULL);
  g_autoptr(GhMlsAttachment) ancient = gh_mls_attachment_new_from_imeta(
    (const gchar *const *)old_tag, 1, &error);
  g_assert_no_error(error);
  g_autoptr(GBytes) gone = gh_mls_media_open(f.marmot, GID_HEX, ancient, ciphertext, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NO_KEY);
  g_assert_null(gone);
  g_clear_error(&error);
  fixture_down(&f);
}

static guint32
crc32_of(const guint8 *data, gsize size)
{
  guint32 c = 0xffffffffu;
  for (gsize i = 0; i < size; i++) {
    c ^= data[i];
    for (guint k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return c ^ 0xffffffffu;
}

static void
png_chunk(GByteArray *out, const gchar *type, const void *data, gsize size)
{
  guint8 head[8] = { (guint8)(size >> 24), (guint8)(size >> 16), (guint8)(size >> 8),
                     (guint8)size, (guint8)type[0], (guint8)type[1], (guint8)type[2],
                     (guint8)type[3] };
  g_byte_array_append(out, head, 8);
  if (size)
    g_byte_array_append(out, data, (guint)size);
  g_autoptr(GByteArray) covered = g_byte_array_new();
  g_byte_array_append(covered, head + 4, 4);
  if (size)
    g_byte_array_append(covered, data, (guint)size);
  guint32 c = crc32_of(covered->data, covered->len);
  const guint8 crc[4] = { (guint8)(c >> 24), (guint8)(c >> 16), (guint8)(c >> 8), (guint8)c };
  g_byte_array_append(out, crc, 4);
}

/* A 1x1 PNG whose tEXt chunk names its author. */
static GBytes *
make_png(void)
{
  GByteArray *out = g_byte_array_new();
  const guint8 signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
  g_byte_array_append(out, signature, 8);
  const guint8 ihdr[13] = { 0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0 };
  png_chunk(out, "IHDR", ihdr, sizeof ihdr);
  png_chunk(out, "tEXt", "Author\0Secret Name", 18);
  png_chunk(out, "IDAT", "\x78\x9c\x63\x60\x00\x00\x00\x02\x00\x01", 10);
  png_chunk(out, "IEND", NULL, 0);
  return g_byte_array_free_to_bytes(out);
}

/* Metadata is removed before encryption, and a picture's size is a hint. */
static void
test_seal_strips_metadata(void)
{
  Fixture f;
  fixture_up(&f);
  g_autoptr(GBytes) file = make_png();
  g_autoptr(GError) error = NULL;
  g_autoptr(GhAttachmentPrepared) prepared = gh_attachment_prepare(file, NULL, 1 << 20, &error);
  g_assert_no_error(error);
  g_autoptr(GhMlsMediaSealed) sealed = gh_mls_media_seal(f.marmot, GID_HEX, file, NULL,
                                                         "pixel.png", 1 << 20, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gh_mls_attachment_get_media_type(sealed->attachment), ==, "image/png");
  g_assert_cmpstr(gh_mls_attachment_get_dim(sealed->attachment), ==, "1x1");
  g_assert_true(sealed->stripped);
  g_assert_false(sealed->may_have_metadata);
  g_autoptr(GhMlsAttachment) sent = upload(&f, sealed, &error);
  g_assert_no_error(error);
  g_autoptr(GBytes) ct = fetch(&f, sent, &error);
  g_assert_no_error(error);
  g_autoptr(GBytes) plain = gh_mls_media_open(f.marmot, GID_HEX, sent, ct, &error);
  g_assert_no_error(error);
  /* What opens is the prepared (stripped) image, not the original. */
  g_assert_true(g_bytes_equal(plain, prepared->plaintext));
  g_assert_null(memmem(g_bytes_get_data(plain, NULL), g_bytes_get_size(plain), "Secret Name", 11));

  /* A type with no canonical form is sent as opaque bytes. */
  g_autoptr(GBytes) blob = g_bytes_new_static("some bytes", 10);
  g_autoptr(GhMlsMediaSealed) odd =
    gh_mls_media_seal(f.marmot, GID_HEX, blob, "x/y/z", "blob.bin", 1 << 20, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gh_mls_attachment_get_media_type(odd->attachment), ==,
                  "application/octet-stream");

  /* Input rules. */
  g_autoptr(GhMlsMediaSealed) no_name =
    gh_mls_media_seal(f.marmot, GID_HEX, file, NULL, "", 1 << 20, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_INVALID);
  g_assert_null(no_name);
  g_clear_error(&error);
  g_autoptr(GhMlsMediaSealed) too_big =
    gh_mls_media_seal(f.marmot, GID_HEX, file, NULL, "pixel.png", 16, &error);
  g_assert_error(error, GH_ATTACHMENT_ERROR, GH_ATTACHMENT_ERROR_TOO_LARGE);
  g_assert_null(too_big);
  g_clear_error(&error);
  fixture_down(&f);
}

/* Locators: a private host is never contacted outside tests, a URL that
 * does not name the blob is skipped, and the next locator is tried. */
static void
test_fetch_locators(void)
{
  Fixture f;
  fixture_up(&f);
  static const gchar text[] = "fetched from the second locator";
  g_autoptr(GBytes) file = g_bytes_new_static(text, sizeof text - 1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsMediaSealed) sealed = gh_mls_media_seal(
    f.marmot, GID_HEX, file, "text/plain", "t.txt", GH_BLOSSOM_MAX_FILE_SIZE, &error);
  g_assert_no_error(error);
  g_autoptr(GhMlsAttachment) sent = upload(&f, sealed, &error);
  g_assert_no_error(error);
  g_auto(GStrv) tag = gh_mls_attachment_dup_imeta(sent, &error);
  g_autofree gchar *sha = gh_mls_attachment_dup_ciphertext_sha256(sent);

  /* [wrong-blob URL, the real one] */
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  for (guint i = 0; tag[i]; i++) {
    if (i == 2) {
      g_autofree gchar *wrong = g_strdup_printf("locator blossom-v1 %s/%064d",
                                                blossom_fixture_url(f.blossom), 0);
      g_strv_builder_add(b, wrong);
    }
    g_strv_builder_add(b, tag[i]);
  }
  g_auto(GStrv) two = g_strv_builder_end(b);
  g_autoptr(GhMlsAttachment) a =
    gh_mls_attachment_new_from_imeta((const gchar *const *)two, 3, &error);
  g_assert_no_error(error);
  g_autoptr(GBytes) ct = fetch(&f, a, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  g_autoptr(GBytes) plain = gh_mls_media_open(f.marmot, GID_HEX, a, ct, &error);
  g_assert_no_error(error);

  /* Outside tests the loopback fixture is a private host: refused before
   * any request, so the file is unavailable. */
  gh_blossom_client_set_allow_private_hosts(f.client, FALSE);
  g_autoptr(GBytes) none = fetch(&f, a, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_UNAVAILABLE);
  g_assert_null(none);
  g_assert_cmpuint(blossom_fixture_count(f.blossom, "GET"), ==, 1);
  g_clear_error(&error);

  /* A server that answers with other bytes: damaged, not opened. */
  gh_blossom_client_set_allow_private_hosts(f.client, TRUE);
  g_autoptr(GBytes) lie = g_bytes_new_static("forged", 6);
  blossom_fixture_put_blob(f.blossom, sha, lie);
  g_autoptr(GBytes) forged = fetch(&f, sent, &error);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_DAMAGED);
  g_assert_null(forged);
  g_clear_error(&error);
  fixture_down(&f);
}

/* Only files on this device: a remote location is refused before any I/O. */
static void
test_read_file_native_only(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("gh-mls-media-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "photo.png", NULL);
  g_assert_true(g_file_set_contents(path, "\x89PNG\r\n\x1a\n", 8, &error));
  g_autoptr(GFile) local = g_file_new_for_path(path);
  Wait w = { 0 };
  gh_mls_media_read_file_async(local, 1 << 20, NULL, on_done, &w);
  g_autofree gchar *name = NULL, *type = NULL;
  g_autoptr(GBytes) bytes = gh_mls_media_read_file_finish(wait_for(&w), &name, &type, &error);
  g_object_unref(w.result);
  g_assert_no_error(error);
  g_assert_cmpuint(g_bytes_get_size(bytes), ==, 8);
  g_assert_cmpstr(name, ==, "photo.png");

  g_autoptr(GFile) remote = g_file_new_for_uri("https://example.invalid/photo.png");
  Wait r = { 0 };
  gh_mls_media_read_file_async(remote, 1 << 20, NULL, on_done, &r);
  g_autoptr(GBytes) refused = gh_mls_media_read_file_finish(wait_for(&r), NULL, NULL, &error);
  g_object_unref(r.result);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL);
  g_assert_null(refused);
  g_clear_error(&error);

  g_autoptr(GFile) folder = g_file_new_for_path(dir);
  Wait d = { 0 };
  gh_mls_media_read_file_async(folder, 1 << 20, NULL, on_done, &d);
  g_autoptr(GBytes) not_file = gh_mls_media_read_file_finish(wait_for(&d), NULL, NULL, &error);
  g_object_unref(d.result);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL);
  g_assert_null(not_file);
  g_clear_error(&error);

  /* Over the limit: refused from its size, before it is read (review L6).
   * A sparse 64 MiB file against a 1 MiB limit. */
  g_autofree gchar *big_path = g_build_filename(dir, "big.bin", NULL);
  FILE *big = fopen(big_path, "wb");
  g_assert_nonnull(big);
  g_assert_cmpint(ftruncate(fileno(big), 64 << 20), ==, 0);
  fclose(big);
  g_autoptr(GFile) big_file = g_file_new_for_path(big_path);
  Wait l = { 0 };
  gh_mls_media_read_file_async(big_file, 1 << 20, NULL, on_done, &l);
  g_autoptr(GBytes) too_big = gh_mls_media_read_file_finish(wait_for(&l), NULL, NULL, &error);
  g_object_unref(l.result);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_TOO_LARGE);
  g_assert_null(too_big);
  g_clear_error(&error);
  g_unlink(big_path);
  g_unlink(path);
  g_rmdir(dir);
}

/* A GVfs FUSE mount ($XDG_RUNTIME_DIR/gvfs, set to a temporary directory in
 * main()) is native to GIO but fetched by gvfsd outside GhNetHttp: refused,
 * also through a symlink (review L6). */
static void
test_read_file_refuses_gvfs_fuse(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *share = g_build_filename(g_get_user_runtime_dir(), "gvfs",
                                             "smb-share:server=nas,share=photos", NULL);
  g_assert_cmpint(g_mkdir_with_parents(share, 0700), ==, 0);
  g_autofree gchar *path = g_build_filename(share, "holiday.png", NULL);
  g_assert_true(g_file_set_contents(path, "\x89PNG\r\n\x1a\n", 8, &error));
  g_autoptr(GFile) inside = g_file_new_for_path(path);
  Wait w = { 0 };
  gh_mls_media_read_file_async(inside, 1 << 20, NULL, on_done, &w);
  g_autoptr(GBytes) refused = gh_mls_media_read_file_finish(wait_for(&w), NULL, NULL, &error);
  g_object_unref(w.result);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL);
  g_assert_null(refused);
  g_clear_error(&error);

  g_autofree gchar *dir = g_dir_make_tmp("gh-mls-media-link-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *link = g_build_filename(dir, "holiday.png", NULL);
  g_assert_cmpint(symlink(path, link), ==, 0);
  g_autoptr(GFile) via_link = g_file_new_for_path(link);
  Wait v = { 0 };
  gh_mls_media_read_file_async(via_link, 1 << 20, NULL, on_done, &v);
  g_autoptr(GBytes) refused2 = gh_mls_media_read_file_finish(wait_for(&v), NULL, NULL, &error);
  g_object_unref(v.result);
  g_assert_error(error, GH_MLS_MEDIA_ERROR, GH_MLS_MEDIA_ERROR_NOT_LOCAL);
  g_assert_null(refused2);
  g_unlink(link);
  g_rmdir(dir);
  g_unlink(path);
}

int
main(int argc, char **argv)
{
  /* A private runtime dir for the GVfs FUSE case, before GLib reads it. */
  g_autofree gchar *runtime = g_dir_make_tmp("gh-mls-media-run-XXXXXX", NULL);
  g_assert_nonnull(runtime);
  g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mls-media/read-file-refuses-gvfs-fuse", test_read_file_refuses_gvfs_fuse);
  g_test_add_func("/mls-media/roundtrip-through-blossom", test_roundtrip_through_blossom);
  g_test_add_func("/mls-media/seal-strips-metadata", test_seal_strips_metadata);
  g_test_add_func("/mls-media/fetch-locators", test_fetch_locators);
  g_test_add_func("/mls-media/read-file-native-only", test_read_file_native_only);
  int rc = g_test_run();
  g_autofree gchar *gvfs = g_build_filename(runtime, "gvfs", NULL);
  g_autofree gchar *share = g_build_filename(gvfs, "smb-share:server=nas,share=photos", NULL);
  g_rmdir(share);
  g_rmdir(gvfs);
  g_rmdir(runtime);
  return rc;
}
