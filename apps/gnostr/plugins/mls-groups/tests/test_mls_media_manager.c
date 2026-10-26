/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real manager entrypoints + real libmarmot crypto/storage. Only the host
 * service/signer boundary and GnBlossomClient transport are in-memory fixtures.
 * This does not claim HTTP/server or GUI acceptance.
 */
#include "gn-mls-media-manager.h"
#include <json-glib/json-glib.h>
#include <marmot/marmot.h>
#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <nostr-event.h>
#include <glib/gstdio.h>
#include <string.h>

static MarmotStorage *storage; /* borrowed from the real client */
static const guint8 gid_bytes[] = {0x01, 0x02};
static GBytes *blob;
static gchar *uploaded_hash;
static gchar *requested_hash;
static gchar *expected_mime;
static guint uploads, downloads;
static gboolean corrupt_download, advance_during_upload;

static void
save_epoch(guint64 epoch)
{
  MarmotGroup *group = marmot_group_new();
  group->mls_group_id = marmot_group_id_new(gid_bytes, sizeof gid_bytes);
  memset(group->nostr_group_id, 0xaa, 32);
  group->name = g_strdup("Media fixture");
  group->description = g_strdup("");
  group->state = MARMOT_GROUP_STATE_ACTIVE;
  group->epoch = epoch;
  g_assert_cmpint(storage->save_group(storage->ctx, group), ==, MARMOT_OK);
  marmot_group_free(group);
  guint8 secret[32];
  memset(secret, (guint8)epoch, sizeof secret);
  g_assert_cmpint(storage->save_exporter_secret(storage->ctx,
    &(MarmotGroupId){(guint8 *)gid_bytes, sizeof gid_bytes}, epoch, secret), ==, MARMOT_OK);
}

/* The service fixture owns a real memory-backed MarmotGobjectClient. */
struct _GnMarmotService { GObject parent_instance; MarmotGobjectClient *client; };
G_DEFINE_TYPE(GnMarmotService, gn_marmot_service, G_TYPE_OBJECT)
static void service_finalize(GObject *object)
{
  g_clear_object(&GN_MARMOT_SERVICE(object)->client);
  G_OBJECT_CLASS(gn_marmot_service_parent_class)->finalize(object);
}
static void gn_marmot_service_class_init(GnMarmotServiceClass *klass)
{ G_OBJECT_CLASS(klass)->finalize = service_finalize; }
static void gn_marmot_service_init(GnMarmotService *self)
{
  g_autoptr(MarmotGobjectMemoryStorage) store = marmot_gobject_memory_storage_new();
  storage = marmot_gobject_storage_get_raw_storage(MARMOT_GOBJECT_STORAGE(store));
  self->client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));
}
MarmotGobjectClient *gn_marmot_service_get_client(GnMarmotService *self)
{ return self->client; }

void
gnostr_plugin_context_request_sign_event(GnostrPluginContext *context,
    const char *json, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  (void)context;
  GTask *task = g_task_new(NULL, cancel, callback, data);
  NostrEvent *event = nostr_event_new();
  g_assert_true(nostr_event_deserialize_compact(event, json, NULL));
  g_assert_cmpint(nostr_event_sign(event,
    "1111111111111111111111111111111111111111111111111111111111111111"), ==, 0);
  g_task_return_pointer(task, nostr_event_serialize_compact(event), g_free);
  nostr_event_free(event);
  g_object_unref(task);
}
char *gnostr_plugin_context_request_sign_event_finish(GnostrPluginContext *context,
    GAsyncResult *result, GError **error)
{ (void)context; return g_task_propagate_pointer(G_TASK(result), error); }

struct _GnBlossomClient { GObject parent_instance; };
G_DEFINE_TYPE(GnBlossomClient, gn_blossom_client, G_TYPE_OBJECT)
static void gn_blossom_client_class_init(GnBlossomClientClass *klass) { (void)klass; }
static void gn_blossom_client_init(GnBlossomClient *self) { (void)self; }
GnBlossomClient *gn_blossom_client_new(const gchar *endpoint)
{ g_assert_nonnull(endpoint); return g_object_new(GN_TYPE_BLOSSOM_CLIENT, NULL); }
void gn_blossom_client_set_network(GnBlossomClient *self, GProxyResolver *resolver,
    GnBlossomNetworkFlags flags)
{ (void)self; (void)resolver; g_assert_cmpint(flags, ==, GN_BLOSSOM_NETWORK_DEFAULT); }
void gn_blossom_client_set_limits(GnBlossomClient *self, gsize max, guint timeout,
    const gchar *agent)
{ (void)self; (void)max; (void)timeout; (void)agent; }
void gn_blossom_client_upload_async(GnBlossomClient *self, GBytes *bytes,
    const gchar *sha, const gchar *mime, const gchar *signed_event,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  uploads++;
  g_autofree gchar *actual = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
  g_assert_cmpstr(sha, ==, actual);
  g_assert_cmpstr(mime, ==, "application/octet-stream");
  /* The authorization must bind the wire hash rather than the plaintext hash. */
  g_autofree gchar *x_tag = g_strdup_printf("\"x\",\"%s\"", sha);
  g_assert_nonnull(strstr(signed_event, x_tag));
  g_clear_pointer(&blob, g_bytes_unref);
  blob = g_bytes_ref(bytes);
  g_free(uploaded_hash);
  uploaded_hash = g_strdup(sha);
  if (advance_during_upload) save_epoch(6);
  GTask *task = g_task_new(self, cancel, callback, data);
  /* Not a hash-addressed URL: the imeta hash, not a URL guess, is authoritative. */
  g_task_return_pointer(task, g_strdup("https://media.example/attachment"), g_free);
  g_object_unref(task);
}
gchar *gn_blossom_client_upload_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{ (void)self; return g_task_propagate_pointer(G_TASK(result), error); }
void gn_blossom_client_download_async(GnBlossomClient *self, const gchar *url,
    const gchar *sha, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  downloads++;
  g_assert_cmpstr(url, ==, "https://media.example/attachment");
  g_free(requested_hash);
  requested_hash = g_strdup(sha);
  GTask *task = g_task_new(self, cancel, callback, data);
  GBytes *downloaded = g_bytes_ref(blob);
  if (corrupt_download) {
    gsize n;
    const guint8 *p = g_bytes_get_data(blob, &n);
    guint8 *bad = g_memdup2(p, n);
    bad[0] ^= 1;
    g_bytes_unref(downloaded);
    downloaded = g_bytes_new_take(bad, n);
  }
  /* Deliberately deliver corruption too, to exercise the manager's independent
   * libmarmot verification even if a transport ever returns unchecked bytes. */
  g_task_return_pointer(task, downloaded, (GDestroyNotify)g_bytes_unref);
  g_object_unref(task);
}
GBytes *gn_blossom_client_download_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{ (void)self; return g_task_propagate_pointer(G_TASK(result), error); }

static void completed(GObject *source, GAsyncResult *result, gpointer data)
{ (void)source; *(GAsyncResult **)data = g_object_ref(result); }
static void wait_result(GAsyncResult **result)
{ while (!*result) g_main_context_iteration(NULL, TRUE); }

static gchar *tag_json(gchar **fields)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  for (guint i = 0; fields[i]; i++) json_builder_add_string_value(builder, fields[i]);
  json_builder_end_array(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  return json_to_string(root, FALSE);
}

static GBytes *download(GnMlsMediaManager *manager, const gchar *tag, guint64 epoch, GError **error)
{
  g_autoptr(GAsyncResult) result = NULL;
  gn_mls_media_manager_download_async(manager, "0102", tag, epoch, NULL, completed, &result);
  wait_result(&result);
  gchar *mime = NULL;
  GBytes *bytes = gn_mls_media_manager_download_finish(manager, result, &mime, error);
  if (bytes) g_assert_cmpstr(mime, ==, expected_mime);
  else g_assert_null(mime);
  g_free(mime);
  return bytes;
}

static void test_media(void)
{
  static const guint8 original[] = {0, 1, 2, 0xff, 'M', 'L', 'S', 0, 42};
  g_autoptr(GError) error = NULL;
  g_autoptr(GnMarmotService) service = g_object_new(GN_TYPE_MARMOT_SERVICE, NULL);
  save_epoch(5);
  g_autoptr(GnMlsMediaManager) manager = gn_mls_media_manager_new(service,
    (GnostrPluginContext *)service, "https://media.example");
  g_autofree gchar *dir = g_dir_make_tmp("gnostr-media-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "attachment.bin", NULL);
  g_assert_true(g_file_set_contents(path, (const gchar *)original, sizeof original, &error));
  g_assert_no_error(error);
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GAsyncResult) result = NULL;
  gn_mls_media_manager_upload_async(manager, "0102", file, NULL, completed, &result);
  wait_result(&result);
  GnMlsMediaUploadResult *up = gn_mls_media_manager_upload_finish(manager, result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(up);
  g_assert_cmpuint(uploads, ==, 1);
  g_assert_cmpuint(up->source_epoch, ==, 5);
  g_assert_true(gn_mls_media_manager_check_epoch(manager, "0102", up->source_epoch, &error));
  g_assert_no_error(error);
  g_autofree gchar *tag = tag_json(up->imeta_tags);
  g_assert_nonnull(strstr(tag, "v encrypted-media-v2"));
  g_assert_nonnull(strstr(tag, "plaintext_sha256 "));
  g_assert_nonnull(strstr(tag, "ciphertext_sha256 "));
  g_assert_nonnull(strstr(tag, "filename attachment.bin"));
  g_assert_null(strstr(tag, "epoch "));
  g_assert_null(strstr(tag, "encoding mls"));
  MarmotMediaReference ref = {0};
  g_assert_true(gn_mls_media_manager_parse_imeta(tag, &ref, &error));
  g_assert_no_error(error);
  g_autofree gchar *plain_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, original, sizeof original);
  g_autofree gchar *plain_field = g_strconcat("plaintext_sha256 ", plain_hash, NULL);
  g_assert_nonnull(strstr(tag, plain_field));
  g_assert_cmpstr(plain_hash, !=, uploaded_hash);
  /* MIME databases differ by platform; the authenticated type must be canonical. */
  g_assert_nonnull(strchr(ref.media_type, '/'));
  expected_mime = g_strdup(ref.media_type);
  marmot_media_reference_clear(&ref);

  /* Do not accept a truncated authenticated filename from a JSON NUL escape. */
  g_autoptr(GString) nul_tag = g_string_new(tag);
  const gchar *filename_end = strstr(tag, "filename attachment.bin") + strlen("filename attachment.bin");
  g_string_insert(nul_tag, filename_end - tag, "\\u0000suffix");
  g_assert_false(gn_mls_media_manager_parse_imeta(nul_tag->str, &ref, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  marmot_media_reference_clear(&ref);

  g_autoptr(GBytes) opened = download(manager, tag, up->source_epoch, &error);
  g_assert_no_error(error);
  g_assert_nonnull(opened);
  g_assert_cmpmem(g_bytes_get_data(opened, NULL), g_bytes_get_size(opened), original, sizeof original);
  g_assert_cmpstr(requested_hash, ==, uploaded_hash);

  /* Retained source epoch, not the group's current epoch, opens old media. */
  save_epoch(6);
  g_clear_pointer(&opened, g_bytes_unref);
  opened = download(manager, tag, up->source_epoch, &error);
  g_assert_no_error(error);
  g_assert_cmpmem(g_bytes_get_data(opened, NULL), g_bytes_get_size(opened), original, sizeof original);
  g_assert_false(gn_mls_media_manager_check_epoch(manager, "0102", up->source_epoch, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  corrupt_download = TRUE;
  g_assert_null(download(manager, tag, 5, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  corrupt_download = FALSE;

  /* Wrong ciphertext hash and wrong plaintext hash are independently refused. */
  const gchar *hash_fields[] = {"ciphertext_sha256 ", "plaintext_sha256 "};
  for (guint i = 0; i < G_N_ELEMENTS(hash_fields); i++) {
    g_autofree gchar *bad = g_strdup(tag);
    gchar *hash = strstr(bad, hash_fields[i]) + strlen(hash_fields[i]);
    hash[0] = hash[0] == '0' ? '1' : '0';
    g_assert_null(download(manager, bad, 5, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
  }
  g_assert_null(download(manager, tag, 99, &error)); /* missing retained secret */
  g_assert_nonnull(error);
  g_clear_error(&error);

  guint before = downloads;
  g_assert_null(download(manager, "[\"imeta\",\"url https://media.example/old\"]", 5, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_cmpuint(downloads, ==, before); /* malformed/legacy metadata never fetches */

  /* An epoch transition during the async upload cannot produce a sendable result. */
  save_epoch(5);
  advance_during_upload = TRUE;
  g_clear_object(&result);
  gn_mls_media_manager_upload_async(manager, "0102", file, NULL, completed, &result);
  wait_result(&result);
  g_assert_null(gn_mls_media_manager_upload_finish(manager, result, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  advance_during_upload = FALSE;

  gn_mls_media_upload_result_free(up);
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(dir), ==, 0);
  g_clear_pointer(&blob, g_bytes_unref);
  g_clear_pointer(&uploaded_hash, g_free);
  g_clear_pointer(&requested_hash, g_free);
  g_clear_pointer(&expected_mime, g_free);
  storage = NULL;
}

static void test_invalid_imeta(void)
{
  const gchar *invalid[] = {"{}", "[\"imeta\",1]", "[]", "[\"imeta\",null]"};
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    MarmotMediaReference ref = {0};
    g_autoptr(GError) error = NULL;
    g_assert_false(gn_mls_media_manager_parse_imeta(invalid[i], &ref, &error));
    g_assert_nonnull(error);
    marmot_media_reference_clear(&ref);
  }
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/mls-media/manager-roundtrip-and-rejection", test_media);
  g_test_add_func("/gnostr/mls-media/invalid-imeta", test_invalid_imeta);
  return g_test_run();
}
