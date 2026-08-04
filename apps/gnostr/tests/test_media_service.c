#include <glib.h>
#include <libsoup/soup.h>

#include "../src/services/gnostr-media-service.h"

static SoupSession *test_session;

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
  return gnostr_media_service_new(&config);
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
      gnostr_media_service_new(&config);

  request_og_and_wait(service, url, &fixture);
  request_og_and_wait(service, url, &fixture);
  g_assert_cmpuint(fixture.server_requests, ==, 1);

  g_usleep(30 * 1000);
  request_og_and_wait(service, url, &fixture);
  g_assert_cmpuint(fixture.callbacks, ==, 3);
  g_assert_cmpuint(fixture.server_requests, ==, 2);

  g_clear_error(&fixture.error);
  soup_server_disconnect(server);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/media-service/cache/decoded-byte-budget",
                  test_cache_respects_decoded_byte_budget);
  g_test_add_func("/media-service/negative/bounded-ttl",
                  test_negative_cache_is_bounded_and_expires);
  g_test_add_func("/media-service/in-flight/dedup-independent-cancel",
                  test_in_flight_requests_are_coalesced);
  g_test_add_func("/media-service/og/shared-ttl-cache",
                  test_og_metadata_is_shared_and_ttl_cached);
  int result = g_test_run();
  g_clear_object(&test_session);
  return result;
}
