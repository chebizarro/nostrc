#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <sys/stat.h>

#include "../src/services/gnostr-media-service.h"

static SoupSession *test_session;
static char *test_disk_root;
static const char *test_namespace = "npub1testnamespace";

SoupSession *
gnostr_get_shared_soup_session(void)
{
  if (!test_session)
    test_session = soup_session_new();
  return test_session;
}

gboolean
gnostr_is_remote_media_allowed(void)
{
  return TRUE;
}

static GdkTexture *
make_texture(int width, int height)
{
  gsize stride = (gsize)width * 4;
  gsize size = stride * (gsize)height;
  guint8 *pixels = g_malloc0(size);
  g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, size);
  return GDK_TEXTURE(gdk_memory_texture_new(width, height,
                                            GDK_MEMORY_R8G8B8A8,
                                            bytes, stride));
}

static void
configure_test_service(GnostrMediaService *service)
{
  gnostr_media_service_test_set_disk_root(service, test_disk_root);
  gnostr_media_service_test_set_namespace(service, test_namespace);
}

static GnostrMediaService *
new_test_service(const GnostrMediaServiceConfig *config)
{
  GnostrMediaService *service = gnostr_media_service_new(config);
  configure_test_service(service);
  return service;
}

static GnostrMediaService *
make_service(guint64 inline_budget, gint64 negative_ttl_usec)
{
  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = inline_budget;
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = 16;
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = 16;
  config.negative_cache_max_entries = 2;
  config.negative_cache_ttl_usec = negative_ttl_usec;
  config.max_in_flight = 8;
  config.max_concurrent_downloads = 2;
  return new_test_service(&config);
}

static void
wait_for_disk_jobs(GnostrMediaService *service)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (gnostr_media_service_test_get_outstanding_disk_jobs(service) > 0 &&
         g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(
      gnostr_media_service_test_get_outstanding_disk_jobs(service), ==, 0);
}

static void
remove_tree(const char *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
          !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree(child);
      else
        g_unlink(child);
    }
  }
  g_rmdir(path);
}

static guint64
directory_regular_file_size(const char *dir, guint *out_count)
{
  guint64 total = 0;
  guint count = 0;
  g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
  if (handle) {
    const char *name;
    while ((name = g_dir_read_name(handle)) != NULL) {
      g_autofree char *path = g_build_filename(dir, name, NULL);
      GStatBuf st;
      if (g_stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        total += st.st_size;
        count++;
      }
    }
  }
  if (out_count)
    *out_count = count;
  return total;
}

static void
test_cache_respects_decoded_byte_budget(void)
{
  g_autoptr(GnostrMediaService) service = make_service(32, G_USEC_PER_SEC);
  g_autoptr(GdkTexture) first = make_texture(2, 2);
  g_autoptr(GdkTexture) second = make_texture(2, 2);
  g_autoptr(GdkTexture) third = make_texture(2, 2);

  gnostr_media_service_test_store_texture(
      service, "https://example.test/1.png", GNOSTR_MEDIA_RESOURCE_INLINE,
      2, 2, first);
  gnostr_media_service_test_store_texture(
      service, "https://example.test/2.png", GNOSTR_MEDIA_RESOURCE_INLINE,
      2, 2, second);
  gnostr_media_service_test_store_texture(
      service, "https://example.test/3.png", GNOSTR_MEDIA_RESOURCE_INLINE,
      2, 2, third);

  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].resident_bytes,
                   <=, 32);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].entries, ==, 2);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].evictions, ==, 1);

  gnostr_media_service_test_store_texture(
      service, "https://example.test/og.png", GNOSTR_MEDIA_RESOURCE_OG_IMAGE,
      2, 2, first);
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].resident_bytes,
                   ==, 32);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE].resident_bytes,
                   ==, 16);
}

static void
test_negative_cache_is_bounded_and_expires(void)
{
  g_autoptr(GnostrMediaService) service = make_service(32, 20 * 1000);

  gnostr_media_service_test_store_negative(
      service, "https://example.test/a", FALSE);
  g_assert_true(gnostr_media_service_test_is_negative(
      service, "https://example.test/a", FALSE));

  gnostr_media_service_test_store_negative(
      service, "https://example.test/b", FALSE);
  gnostr_media_service_test_store_negative(
      service, "https://example.test/c", FALSE);

  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.negative_entries, ==, 2);
  g_assert_false(gnostr_media_service_test_is_negative(
      service, "https://example.test/a", FALSE));

  g_usleep(30 * 1000);
  g_assert_false(gnostr_media_service_test_is_negative(
      service, "https://example.test/c", FALSE));
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.negative_entries, ==, 0);
}

typedef struct {
  GMainLoop *loop;
  guint callbacks;
  guint successful_callbacks;
  guint cancelled_callbacks;
  guint server_requests;
  GError *error;
} DedupFixture;

static void
server_image_handler(SoupServer *server,
                     SoupServerMessage *message,
                     const char *path,
                     GHashTable *query,
                     gpointer user_data)
{
  (void)server;
  (void)path;
  (void)query;
  DedupFixture *fixture = user_data;
  fixture->server_requests++;

  gsize png_size = 0;
  g_autofree guchar *png = g_base64_decode(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
      "+A8AAQUBAScY42YAAAAASUVORK5CYII=", &png_size);
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "image/png", SOUP_MEMORY_COPY,
                                   (const char *)png, png_size);
}

static void
texture_ready(GnostrMediaService *service,
              const char *url,
              GdkTexture *texture,
              const GError *error,
              gpointer user_data)
{
  (void)service;
  (void)url;
  DedupFixture *fixture = user_data;
  fixture->callbacks++;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    fixture->cancelled_callbacks++;
  } else if (error && !fixture->error) {
    fixture->error = g_error_copy(error);
  } else if (texture) {
    fixture->successful_callbacks++;
  } else if (!fixture->error) {
    fixture->error = g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                         GNOSTR_MEDIA_ERROR_DECODE,
                                         "Callback returned no texture");
  }
  if (fixture->callbacks == 2)
    g_main_loop_quit(fixture->loop);
}

static gboolean
dedup_timeout(gpointer user_data)
{
  DedupFixture *fixture = user_data;
  if (!fixture->error)
    fixture->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                         "Timed out waiting for callbacks");
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}

static void
test_in_flight_requests_are_coalesced(void)
{
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  DedupFixture fixture = { 0 };
  soup_server_add_handler(server, "/image.png", server_image_handler,
                          &fixture, NULL);

  g_autoptr(GError) listen_error = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                        SOUP_SERVER_LISTEN_IPV4_ONLY,
                                        &listen_error));
  g_assert_no_error(listen_error);

  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  g_autofree char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autofree char *url = g_strconcat(base, "image.png", NULL);

  g_autoptr(GnostrMediaService) service =
      make_service(4 * 1024 * 1024, G_USEC_PER_SEC);
  fixture.loop = g_main_loop_new(NULL, FALSE);

  g_autoptr(GCancellable) first_cancellable = g_cancellable_new();
  gnostr_media_service_request_texture(
      service, url, GNOSTR_MEDIA_RESOURCE_INLINE, 32, 32, first_cancellable,
      texture_ready, &fixture, NULL);
  gnostr_media_service_request_texture(
      service, url, GNOSTR_MEDIA_RESOURCE_INLINE, 32, 32, NULL,
      texture_ready, &fixture, NULL);
  /* Cancelling one coalesced subscriber must not cancel the shared transfer
   * while another subscriber still needs it. */
  g_cancellable_cancel(first_cancellable);

  guint timeout_id = g_timeout_add_seconds(5, dedup_timeout, &fixture);
  g_main_loop_run(fixture.loop);
  if (timeout_id)
    g_source_remove(timeout_id);

  g_assert_no_error(fixture.error);
  g_assert_cmpuint(fixture.callbacks, ==, 2);
  g_assert_cmpuint(fixture.successful_callbacks, ==, 1);
  g_assert_cmpuint(fixture.cancelled_callbacks, ==, 1);
  g_assert_cmpuint(fixture.server_requests, ==, 1);

  g_clear_error(&fixture.error);
  g_main_loop_unref(fixture.loop);
  soup_server_disconnect(server);
}


typedef struct {
  GMainLoop *loop;
  guint callbacks;
  GError *error;
} SingleTextureFixture;

static void
single_texture_ready(GnostrMediaService *service,
                     const char *url,
                     GdkTexture *texture,
                     const GError *error,
                     gpointer user_data)
{
  (void)service;
  (void)url;
  SingleTextureFixture *fixture = user_data;
  fixture->callbacks++;
  if (error)
    fixture->error = g_error_copy(error);
  else if (!texture)
    fixture->error = g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                         GNOSTR_MEDIA_ERROR_DECODE,
                                         "Callback returned no texture");
  g_main_loop_quit(fixture->loop);
}

static gboolean
single_texture_timeout(gpointer user_data)
{
  SingleTextureFixture *fixture = user_data;
  if (!fixture->error)
    fixture->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                         "Timed out waiting for texture");
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}

static void
request_texture_and_wait(GnostrMediaService *service,
                         const char *url,
                         SingleTextureFixture *fixture)
{
  fixture->loop = g_main_loop_new(NULL, FALSE);
  gnostr_media_service_request_texture(
      service, url, GNOSTR_MEDIA_RESOURCE_INLINE, 32, 32, NULL,
      single_texture_ready, fixture, NULL);
  guint timeout_id = g_timeout_add_seconds(5, single_texture_timeout, fixture);
  g_main_loop_run(fixture->loop);
  if (timeout_id)
    g_source_remove(timeout_id);
  g_main_loop_unref(fixture->loop);
  fixture->loop = NULL;
  g_assert_no_error(fixture->error);
}

static void
test_disk_hit_falls_back_before_network(void)
{
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  DedupFixture server_fixture = { 0 };
  soup_server_add_handler(server, "/image.png", server_image_handler,
                          &server_fixture, NULL);
  g_autoptr(GError) listen_error = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                        SOUP_SERVER_LISTEN_IPV4_ONLY,
                                        &listen_error));
  g_assert_no_error(listen_error);
  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  g_autofree char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autofree char *url = g_strconcat(base, "image.png", NULL);

  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 0;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 1024 * 1024;

  SingleTextureFixture fixture = { 0 };
  {
    g_autoptr(GnostrMediaService) first = new_test_service(&config);
    request_texture_and_wait(first, url, &fixture);
    wait_for_disk_jobs(first);
  }
  g_assert_cmpuint(server_fixture.server_requests, ==, 1);

  g_autoptr(GnostrMediaService) second = new_test_service(&config);
  request_texture_and_wait(second, url, &fixture);
  wait_for_disk_jobs(second);
  g_assert_cmpuint(fixture.callbacks, ==, 2);
  g_assert_cmpuint(server_fixture.server_requests, ==, 1);

  g_autofree char *digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, url, -1);
  g_autofree char *path = g_build_filename(
      test_disk_root, test_namespace, "inline", digest, NULL);
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
  g_clear_error(&fixture.error);
  soup_server_disconnect(server);
}

static void
test_disk_write_enforces_encoded_byte_budget(void)
{
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  DedupFixture server_fixture = { 0 };
  soup_server_add_handler(server, NULL, server_image_handler,
                          &server_fixture, NULL);
  g_autoptr(GError) listen_error = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                        SOUP_SERVER_LISTEN_IPV4_ONLY,
                                        &listen_error));
  g_assert_no_error(listen_error);
  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  g_autofree char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autofree char *first_url = g_strconcat(base, "one.png", NULL);
  g_autofree char *second_url = g_strconcat(base, "two.png", NULL);

  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 0;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 100;
  g_autoptr(GnostrMediaService) service = new_test_service(&config);
  SingleTextureFixture fixture = { 0 };

  request_texture_and_wait(service, first_url, &fixture);
  wait_for_disk_jobs(service);
  request_texture_and_wait(service, second_url, &fixture);
  wait_for_disk_jobs(service);

  g_autofree char *dir = g_build_filename(
      test_disk_root, test_namespace, "inline", NULL);
  guint count = 0;
  guint64 total = directory_regular_file_size(dir, &count);
  g_assert_cmpuint(total, <=, config.disk_budget_bytes[
      GNOSTR_MEDIA_RESOURCE_INLINE]);
  g_assert_cmpuint(count, ==, 1);
  g_assert_cmpuint(server_fixture.server_requests, ==, 2);
  g_clear_error(&fixture.error);
  soup_server_disconnect(server);
}


typedef struct {
  GMainLoop *loop;
  guint callbacks;
  guint server_requests;
  GError *error;
} OgFixture;

static void
server_og_handler(SoupServer *server,
                  SoupServerMessage *message,
                  const char *path,
                  GHashTable *query,
                  gpointer user_data)
{
  (void)server;
  (void)path;
  (void)query;
  OgFixture *fixture = user_data;
  fixture->server_requests++;
  static const char html[] =
      "<html><head>"
      "<meta property='og:title' content='Shared title'>"
      "<meta property='og:description' content='Shared description'>"
      "<meta property='og:image' content='/poster.png'>"
      "</head></html>";
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "text/html", SOUP_MEMORY_STATIC,
                                   html, sizeof(html) - 1);
}

static void
og_ready(GnostrMediaService *service,
         const char *url,
         GnostrOgMetadata *metadata,
         const GError *error,
         gpointer user_data)
{
  (void)service;
  (void)url;
  OgFixture *fixture = user_data;
  fixture->callbacks++;
  if (error && !fixture->error) {
    fixture->error = g_error_copy(error);
  } else if (!metadata && !fixture->error) {
    fixture->error = g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                         GNOSTR_MEDIA_ERROR_DECODE,
                                         "Callback returned no metadata");
  } else if (metadata) {
    if (g_strcmp0(gnostr_og_metadata_get_title(metadata),
                  "Shared title") != 0 ||
        g_strcmp0(gnostr_og_metadata_get_description(metadata),
                  "Shared description") != 0 ||
        !g_str_has_suffix(gnostr_og_metadata_get_image_url(metadata),
                          "/poster.png")) {
      fixture->error = g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                           GNOSTR_MEDIA_ERROR_DECODE,
                                           "Parsed metadata did not match");
    }
  }
  g_main_loop_quit(fixture->loop);
}

static gboolean
og_timeout(gpointer user_data)
{
  OgFixture *fixture = user_data;
  if (!fixture->error)
    fixture->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                         "Timed out waiting for OG callback");
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}

static void
request_og_and_wait(GnostrMediaService *service,
                    const char *url,
                    OgFixture *fixture)
{
  fixture->loop = g_main_loop_new(NULL, FALSE);
  gnostr_media_service_request_og_metadata(service, url, NULL, og_ready,
                                           fixture, NULL);
  guint timeout_id = g_timeout_add_seconds(5, og_timeout, fixture);
  g_main_loop_run(fixture->loop);
  if (timeout_id)
    g_source_remove(timeout_id);
  g_main_loop_unref(fixture->loop);
  fixture->loop = NULL;
  g_assert_no_error(fixture->error);
}

static void
test_og_metadata_is_shared_and_ttl_cached(void)
{
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  OgFixture fixture = { 0 };
  soup_server_add_handler(server, "/page", server_og_handler, &fixture, NULL);

  g_autoptr(GError) listen_error = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                        SOUP_SERVER_LISTEN_IPV4_ONLY,
                                        &listen_error));
  g_assert_no_error(listen_error);
  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  g_autofree char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autofree char *url = g_strconcat(base, "page", NULL);

  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.og_metadata_ttl_usec = 20 * 1000;
  g_autoptr(GnostrMediaService) service =
      new_test_service(&config);

  request_og_and_wait(service, url, &fixture);
  request_og_and_wait(service, url, &fixture);
  g_assert_cmpuint(fixture.server_requests, ==, 1);

  g_usleep(30 * 1000);
  request_og_and_wait(service, url, &fixture);
  g_assert_cmpuint(fixture.callbacks, ==, 3);
  g_assert_cmpuint(fixture.server_requests, ==, 2);

  wait_for_disk_jobs(service);
  g_clear_error(&fixture.error);
  soup_server_disconnect(server);
}

static void
test_og_metadata_persists_with_ttl(void)
{
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  OgFixture fixture = { 0 };
  soup_server_add_handler(server, "/persist", server_og_handler,
                          &fixture, NULL);
  g_autoptr(GError) listen_error = NULL;
  g_assert_true(soup_server_listen_local(server, 0,
                                        SOUP_SERVER_LISTEN_IPV4_ONLY,
                                        &listen_error));
  g_assert_no_error(listen_error);
  GSList *uris = soup_server_get_uris(server);
  g_assert_nonnull(uris);
  g_autofree char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autofree char *url = g_strconcat(base, "persist", NULL);

  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.og_metadata_ttl_usec = 250 * 1000;
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = 0;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = 1024 * 1024;

  {
    g_autoptr(GnostrMediaService) first = new_test_service(&config);
    request_og_and_wait(first, url, &fixture);
    wait_for_disk_jobs(first);
  }
  g_assert_cmpuint(fixture.server_requests, ==, 1);

  {
    g_autoptr(GnostrMediaService) second = new_test_service(&config);
    request_og_and_wait(second, url, &fixture);
    wait_for_disk_jobs(second);
  }
  g_assert_cmpuint(fixture.server_requests, ==, 1);

  g_usleep(350 * 1000);
  {
    g_autoptr(GnostrMediaService) third = new_test_service(&config);
    request_og_and_wait(third, url, &fixture);
    wait_for_disk_jobs(third);
  }
  g_assert_cmpuint(fixture.callbacks, ==, 3);
  g_assert_cmpuint(fixture.server_requests, ==, 2);

  g_clear_error(&fixture.error);
  soup_server_disconnect(server);
}

static void
test_account_eviction_removes_namespace_and_memory(void)
{
  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 1024;
  g_autoptr(GnostrMediaService) service = new_test_service(&config);
  g_autoptr(GdkTexture) texture = make_texture(2, 2);

  gnostr_media_service_test_set_namespace(service, "npub1evicted");
  gnostr_media_service_test_store_texture(
      service, "https://example.test/evicted.png",
      GNOSTR_MEDIA_RESOURCE_INLINE, 2, 2, texture);
  g_autofree char *account_dir = g_build_filename(
      test_disk_root, "npub1evicted", "inline", NULL);
  g_assert_cmpint(g_mkdir_with_parents(account_dir, 0700), ==, 0);
  g_autofree char *disk_file = g_build_filename(account_dir, "entry", NULL);
  g_assert_true(g_file_set_contents(disk_file, "cached", -1, NULL));

  gnostr_media_service_test_set_namespace(service, "npub1retained");
  gnostr_media_service_test_store_texture(
      service, "https://example.test/retained.png",
      GNOSTR_MEDIA_RESOURCE_INLINE, 2, 2, texture);

  gnostr_media_service_evict_account(service, "npub1evicted");

  g_autofree char *namespace_dir = g_build_filename(
      test_disk_root, "npub1evicted", NULL);
  g_assert_false(g_file_test(namespace_dir, G_FILE_TEST_EXISTS));
  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].entries, ==, 1);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_INLINE].resident_bytes,
                   ==, 16);
}

typedef struct {
  GMutex lock;
  guint active;
  guint max_active;
  guint callbacks;
  guint successes;
  GMainLoop *loop;
  GError *error;
} ThumbnailFixture;

static GBytes *
stub_thumbnail_extract(const char *url,
                       guint timeout_msec,
                       GCancellable *cancellable,
                       GError **error,
                       gpointer user_data)
{
  (void)url;
  (void)timeout_msec;
  ThumbnailFixture *fixture = user_data;
  g_mutex_lock(&fixture->lock);
  fixture->active++;
  fixture->max_active = MAX(fixture->max_active, fixture->active);
  g_mutex_unlock(&fixture->lock);

  g_usleep(50 * 1000);
  GBytes *bytes = NULL;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Stub extraction cancelled");
  } else {
    gsize png_size = 0;
    guchar *png = g_base64_decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
        "+A8AAQUBAScY42YAAAAASUVORK5CYII=", &png_size);
    bytes = g_bytes_new_take(png, png_size);
  }

  g_mutex_lock(&fixture->lock);
  fixture->active--;
  g_mutex_unlock(&fixture->lock);
  return bytes;
}

static void
evicted_thumbnail_ready(GnostrMediaService *service,
                        const char *url,
                        GdkTexture *texture,
                        const GError *error,
                        gpointer user_data)
{
  (void)service;
  (void)url;
  ThumbnailFixture *fixture = user_data;
  fixture->callbacks++;
  g_assert_null(texture);
  g_assert_true(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
}

static void
test_account_eviction_cancels_in_flight_thumbnail(void)
{
  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.thumbnail_timeout_msec = 1000;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = 1024 * 1024;
  g_autoptr(GnostrMediaService) service = new_test_service(&config);
  gnostr_media_service_test_set_namespace(service, "npub1inflight");
  ThumbnailFixture fixture = { 0 };
  g_mutex_init(&fixture.lock);
  gnostr_media_service_test_set_thumbnail_extractor(
      service, stub_thumbnail_extract, &fixture, NULL);

  gnostr_media_service_request_texture(
      service, "https://example.test/inflight.mp4",
      GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER, 32, 32, NULL,
      evicted_thumbnail_ready, &fixture, NULL);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  GnostrMediaCacheStats stats;
  do {
    g_main_context_iteration(NULL, TRUE);
    gnostr_media_service_get_stats(service, &stats);
  } while (stats.active_thumbnails == 0 && g_get_monotonic_time() < deadline);
  g_assert_cmpuint(stats.active_thumbnails, ==, 1);

  gnostr_media_service_evict_account(service, "npub1inflight");
  do {
    g_main_context_iteration(NULL, TRUE);
    gnostr_media_service_get_stats(service, &stats);
  } while ((stats.active_thumbnails > 0 || stats.pending_requests > 0) &&
           g_get_monotonic_time() < deadline);

  g_assert_cmpuint(fixture.callbacks, ==, 1);
  g_assert_cmpuint(stats.active_thumbnails, ==, 0);
  g_assert_cmpuint(stats.pending_requests, ==, 0);
  g_assert_cmpuint(stats.classes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER].entries,
                   ==, 0);
  wait_for_disk_jobs(service);
  g_autofree char *namespace_dir = g_build_filename(
      test_disk_root, "npub1inflight", NULL);
  g_assert_false(g_file_test(namespace_dir, G_FILE_TEST_EXISTS));
  g_mutex_clear(&fixture.lock);
}

static void
thumbnail_ready(GnostrMediaService *service,
                const char *url,
                GdkTexture *texture,
                const GError *error,
                gpointer user_data)
{
  (void)service;
  (void)url;
  ThumbnailFixture *fixture = user_data;
  fixture->callbacks++;
  if (texture)
    fixture->successes++;
  else if (!fixture->error)
    fixture->error = error ? g_error_copy(error) :
        g_error_new_literal(GNOSTR_MEDIA_ERROR, GNOSTR_MEDIA_ERROR_DECODE,
                            "Thumbnail callback returned no texture");
  if (fixture->callbacks == 5)
    g_main_loop_quit(fixture->loop);
}

static gboolean
thumbnail_timeout(gpointer user_data)
{
  ThumbnailFixture *fixture = user_data;
  if (!fixture->error)
    fixture->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                         "Timed out waiting for thumbnails");
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}

static void
test_thumbnail_queue_is_bounded_and_persisted(void)
{
  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.max_concurrent_thumbnails = 2;
  config.thumbnail_timeout_msec = 1000;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = 1024 * 1024;
  g_autoptr(GnostrMediaService) service = new_test_service(&config);
  ThumbnailFixture fixture = { 0 };
  g_mutex_init(&fixture.lock);
  fixture.loop = g_main_loop_new(NULL, FALSE);
  gnostr_media_service_test_set_thumbnail_extractor(
      service, stub_thumbnail_extract, &fixture, NULL);

  for (guint i = 0; i < 5; i++) {
    g_autofree char *url = g_strdup_printf("https://example.test/%u.mp4", i);
    gnostr_media_service_request_texture(
        service, url, GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER, 32, 32, NULL,
        thumbnail_ready, &fixture, NULL);
  }
  guint timeout_id = g_timeout_add_seconds(5, thumbnail_timeout, &fixture);
  g_main_loop_run(fixture.loop);
  if (timeout_id)
    g_source_remove(timeout_id);

  g_assert_no_error(fixture.error);
  g_assert_cmpuint(fixture.callbacks, ==, 5);
  g_assert_cmpuint(fixture.successes, ==, 5);
  g_assert_cmpuint(fixture.max_active, <=, 2);
  g_assert_cmpuint(fixture.max_active, ==, 2);
  wait_for_disk_jobs(service);

  const char *first_url = "https://example.test/0.mp4";
  g_autofree char *digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, first_url, -1);
  g_autofree char *poster_path = g_build_filename(
      test_disk_root, test_namespace, "video-poster", digest, NULL);
  g_assert_true(g_file_test(poster_path, G_FILE_TEST_IS_REGULAR));

  g_clear_error(&fixture.error);
  g_main_loop_unref(fixture.loop);
  g_mutex_clear(&fixture.lock);
}

static void
test_disk_executor_bounds_and_coalesces(void)
{
  GnostrMediaServiceConfig config;
  gnostr_media_service_config_init(&config);
  config.disk_worker_count = 1;
  config.disk_max_queued_jobs = 2;
  config.disk_max_queued_bytes = 8;
  config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = 1024 * 1024;
  g_autoptr(GnostrMediaService) service = new_test_service(&config);
  g_autoptr(GBytes) bytes = g_bytes_new_static("data", 4);

  /* Completion callbacks cannot run until the main context iterates, so the
   * first job occupies the sole worker while the following submissions test
   * the waiting queue deterministically. */
  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/active.png", bytes);
  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/active.png", bytes);
  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.active_disk_jobs, ==, 1);
  g_assert_cmpuint(stats.queued_disk_jobs, ==, 1);
  g_assert_cmpuint(stats.dropped_disk_jobs, ==, 0);

  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/coalesced.png", bytes);
  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/coalesced.png", bytes);
  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/third.png", bytes);
  gnostr_media_service_test_enqueue_disk_write(
      service, test_namespace, GNOSTR_MEDIA_RESOURCE_INLINE,
      "https://example.test/fourth.png", bytes);

  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.active_disk_jobs, ==, 1);
  g_assert_cmpuint(stats.queued_disk_jobs, ==, 2);
  g_assert_cmpuint(stats.queued_disk_bytes, ==, 8);
  g_assert_cmpuint(stats.dropped_disk_jobs, ==, 3);
  g_assert_cmpuint(
      gnostr_media_service_test_get_outstanding_disk_jobs(service), ==, 3);

  wait_for_disk_jobs(service);
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.active_disk_jobs, ==, 0);
  g_assert_cmpuint(stats.queued_disk_jobs, ==, 0);
  g_assert_cmpuint(stats.queued_disk_bytes, ==, 0);

  /* An active sweep retains exactly one latest follow-up instead of running
   * duplicate scans or losing a settings-triggered resweep. */
  gnostr_media_service_test_enqueue_sweep(service, test_namespace);
  gnostr_media_service_test_enqueue_sweep(service, test_namespace);
  gnostr_media_service_test_enqueue_sweep(service, test_namespace);
  gnostr_media_service_get_stats(service, &stats);
  g_assert_cmpuint(stats.active_disk_jobs, ==, 1);
  g_assert_cmpuint(stats.queued_disk_jobs, ==, 1);
  g_assert_cmpuint(stats.queued_disk_bytes, ==, 0);
  g_assert_cmpuint(stats.dropped_disk_jobs, ==, 4);
  wait_for_disk_jobs(service);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_autoptr(GError) tmp_error = NULL;
  test_disk_root = g_dir_make_tmp("gnostr-media-service-XXXXXX", &tmp_error);
  g_assert_no_error(tmp_error);
  g_assert_nonnull(test_disk_root);

  g_test_add_func("/media-service/cache/decoded-byte-budget",
                  test_cache_respects_decoded_byte_budget);
  g_test_add_func("/media-service/negative/bounded-ttl",
                  test_negative_cache_is_bounded_and_expires);
  g_test_add_func("/media-service/in-flight/dedup-independent-cancel",
                  test_in_flight_requests_are_coalesced);
  g_test_add_func("/media-service/disk/hit-before-network",
                  test_disk_hit_falls_back_before_network);
  g_test_add_func("/media-service/disk/encoded-byte-budget",
                  test_disk_write_enforces_encoded_byte_budget);
  g_test_add_func("/media-service/disk/executor-bounds-coalescing",
                  test_disk_executor_bounds_and_coalesces);
  g_test_add_func("/media-service/og/shared-ttl-cache",
                  test_og_metadata_is_shared_and_ttl_cached);
  g_test_add_func("/media-service/og/persisted-ttl",
                  test_og_metadata_persists_with_ttl);
  g_test_add_func("/media-service/account/eviction",
                  test_account_eviction_removes_namespace_and_memory);
  g_test_add_func("/media-service/account/eviction-in-flight",
                  test_account_eviction_cancels_in_flight_thumbnail);
  g_test_add_func("/media-service/video/thumbnail-bounds",
                  test_thumbnail_queue_is_bounded_and_persisted);
  int result = g_test_run();
  g_clear_object(&test_session);
  remove_tree(test_disk_root);
  g_clear_pointer(&test_disk_root, g_free);
  return result;
}
