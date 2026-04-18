/* test_dav_propfind.c - Unit tests for nostr-dav PROPFIND responses
 *
 * SPDX-License-Identifier: MIT
 *
 * Starts an NdDavServer on a dedicated GMainContext thread, issues
 * HTTP requests via libsoup client, and validates the DAV XML responses.
 */

#include "nd-dav-server.h"
#include "nd-token-store.h"

#include <glib.h>
#include <libsoup/soup.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <string.h>

/* ---- Test fixtures ---- */

typedef struct {
  NdTokenStore *token_store;
  NdDavServer  *server;
  SoupSession  *session;
  guint         port;
  gchar        *base_url;

  /* Server runs on its own thread with its own GMainContext */
  GMainLoop    *server_loop;
  GThread      *server_thread;
  GMutex        ready_mutex;
  GCond         ready_cond;
  gboolean      ready;
} DavFixture;

static gpointer
server_thread_func(gpointer data)
{
  DavFixture *f = data;

  /* Create everything in this thread so GMainContext is native */
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  f->token_store = nd_token_store_new();
  f->server = nd_dav_server_new(f->token_store);

  GError *err = NULL;
  gboolean started = nd_dav_server_start(f->server, "127.0.0.1", f->port, &err);
  if (!started) {
    f->port += 1000;
    g_clear_error(&err);
    started = nd_dav_server_start(f->server, "127.0.0.1", f->port, &err);
  }
  g_assert_no_error(err);
  g_assert_true(started);

  f->server_loop = g_main_loop_new(ctx, FALSE);

  /* Signal the main thread that the server is ready */
  g_mutex_lock(&f->ready_mutex);
  f->ready = TRUE;
  g_cond_signal(&f->ready_cond);
  g_mutex_unlock(&f->ready_mutex);

  g_main_loop_run(f->server_loop);

  g_main_context_pop_thread_default(ctx);
  g_main_loop_unref(f->server_loop);
  f->server_loop = NULL;
  g_main_context_unref(ctx);
  return NULL;
}

static void
dav_fixture_setup(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_mutex_init(&f->ready_mutex);
  g_cond_init(&f->ready_cond);
  f->ready = FALSE;

  f->port = 17680 + (guint)(g_test_rand_int_range(0, 1000));

  /* Start the server thread — it creates server objects internally */
  f->server_thread = g_thread_new("dav-server", server_thread_func, f);

  /* Wait for the server to be ready */
  g_mutex_lock(&f->ready_mutex);
  while (!f->ready)
    g_cond_wait(&f->ready_cond, &f->ready_mutex);
  g_mutex_unlock(&f->ready_mutex);

  f->base_url = g_strdup_printf("http://127.0.0.1:%u", f->port);
  f->session = soup_session_new();
}

static void
dav_fixture_teardown(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_main_loop_quit(f->server_loop);
  g_thread_join(f->server_thread);

  nd_dav_server_stop(f->server);
  g_clear_object(&f->server);
  nd_token_store_free(f->token_store);
  g_clear_object(&f->session);
  g_free(f->base_url);
  g_mutex_clear(&f->ready_mutex);
  g_cond_clear(&f->ready_cond);
}

/* ---- Helpers ---- */

static SoupMessage *
make_propfind(const gchar *url, int depth)
{
  SoupMessage *msg = soup_message_new("PROPFIND", url);
  SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
  g_autofree gchar *depth_str = g_strdup_printf("%d", depth);
  soup_message_headers_replace(hdrs, "Depth", depth_str);
  soup_message_headers_replace(hdrs, "Content-Type", "application/xml");
  return msg;
}

static GBytes *
send_msg(DavFixture *f, SoupMessage *msg)
{
  GError *err = NULL;
  GBytes *body = soup_session_send_and_read(f->session, msg, NULL, &err);
  g_assert_no_error(err);
  return body;
}

/* ---- Tests ---- */

static void
test_options_root(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("OPTIONS", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 200);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *dav = soup_message_headers_get_one(hdrs, "DAV");
  g_assert_nonnull(dav);
  g_assert_true(strstr(dav, "calendar-access") != NULL);
  g_assert_true(strstr(dav, "addressbook") != NULL);
}

static void
test_wellknown_caldav(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/.well-known/caldav", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
  g_autoptr(GBytes) body = send_msg(f, msg);

  guint status = soup_message_get_status(msg);
  g_assert_true(status == 301 || status == 302);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *location = soup_message_headers_get_one(hdrs, "Location");
  g_assert_cmpstr(location, ==, "/calendars/");
}

static void
test_wellknown_carddav(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/.well-known/carddav", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
  g_autoptr(GBytes) body = send_msg(f, msg);

  guint status = soup_message_get_status(msg);
  g_assert_true(status == 301 || status == 302);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *location = soup_message_headers_get_one(hdrs, "Location");
  g_assert_cmpstr(location, ==, "/contacts/");
}

static void
test_propfind_root_depth0(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/", f->base_url);
  g_autoptr(SoupMessage) msg = make_propfind(url, 0);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);
  g_assert_true(len > 0);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  xmlXPathObjectPtr result = xmlXPathEvalExpression(
    BAD_CAST "//D:current-user-principal/D:href", ctx);
  g_assert_nonnull(result);
  g_assert_true(result->nodesetval != NULL && result->nodesetval->nodeNr == 1);

  xmlChar *href = xmlNodeGetContent(result->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)href, ==, "/principals/me/");
  xmlFree(href);

  xmlXPathFreeObject(result);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_root_depth1(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/", f->base_url);
  g_autoptr(SoupMessage) msg = make_propfind(url, 1);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  xmlXPathObjectPtr result = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_nonnull(result);
  g_assert_cmpint(result->nodesetval->nodeNr, ==, 3);

  xmlXPathFreeObject(result);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_calendars_depth1(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/calendars/", f->base_url);
  g_autoptr(SoupMessage) msg = make_propfind(url, 1);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");
  xmlXPathRegisterNs(ctx, BAD_CAST "C",
                     BAD_CAST "urn:ietf:params:xml:ns:caldav");

  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 2);

  xmlXPathObjectPtr cal = xmlXPathEvalExpression(
    BAD_CAST "//C:calendar", ctx);
  g_assert_cmpint(cal->nodesetval->nodeNr, ==, 1);

  xmlXPathFreeObject(cal);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_contacts_depth1(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/contacts/", f->base_url);
  g_autoptr(SoupMessage) msg = make_propfind(url, 1);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");
  xmlXPathRegisterNs(ctx, BAD_CAST "CR",
                     BAD_CAST "urn:ietf:params:xml:ns:carddav");

  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 2);

  xmlXPathObjectPtr ab = xmlXPathEvalExpression(
    BAD_CAST "//CR:addressbook", ctx);
  g_assert_cmpint(ab->nodesetval->nodeNr, ==, 1);

  xmlXPathFreeObject(ab);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_principal(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/principals/me/", f->base_url);
  g_autoptr(SoupMessage) msg = make_propfind(url, 0);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");
  xmlXPathRegisterNs(ctx, BAD_CAST "C",
                     BAD_CAST "urn:ietf:params:xml:ns:caldav");
  xmlXPathRegisterNs(ctx, BAD_CAST "CR",
                     BAD_CAST "urn:ietf:params:xml:ns:carddav");

  xmlXPathObjectPtr cal_home = xmlXPathEvalExpression(
    BAD_CAST "//C:calendar-home-set/D:href", ctx);
  g_assert_cmpint(cal_home->nodesetval->nodeNr, ==, 1);
  xmlChar *cal_href = xmlNodeGetContent(cal_home->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)cal_href, ==, "/calendars/");
  xmlFree(cal_href);

  xmlXPathObjectPtr ab_home = xmlXPathEvalExpression(
    BAD_CAST "//CR:addressbook-home-set/D:href", ctx);
  g_assert_cmpint(ab_home->nodesetval->nodeNr, ==, 1);
  xmlChar *ab_href = xmlNodeGetContent(ab_home->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)ab_href, ==, "/contacts/");
  xmlFree(ab_href);

  xmlXPathFreeObject(ab_home);
  xmlXPathFreeObject(cal_home);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_report_empty(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/calendars/nostr/", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("REPORT", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 0);

  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_method_not_allowed(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf("%s/calendars/", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("POST", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 405);
}

/* ---- Main ---- */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  xmlInitParser();

  g_test_add("/dav/options-root", DavFixture, NULL,
             dav_fixture_setup, test_options_root, dav_fixture_teardown);
  g_test_add("/dav/wellknown-caldav", DavFixture, NULL,
             dav_fixture_setup, test_wellknown_caldav, dav_fixture_teardown);
  g_test_add("/dav/wellknown-carddav", DavFixture, NULL,
             dav_fixture_setup, test_wellknown_carddav, dav_fixture_teardown);
  g_test_add("/dav/propfind-root-depth0", DavFixture, NULL,
             dav_fixture_setup, test_propfind_root_depth0, dav_fixture_teardown);
  g_test_add("/dav/propfind-root-depth1", DavFixture, NULL,
             dav_fixture_setup, test_propfind_root_depth1, dav_fixture_teardown);
  g_test_add("/dav/propfind-calendars-depth1", DavFixture, NULL,
             dav_fixture_setup, test_propfind_calendars_depth1, dav_fixture_teardown);
  g_test_add("/dav/propfind-contacts-depth1", DavFixture, NULL,
             dav_fixture_setup, test_propfind_contacts_depth1, dav_fixture_teardown);
  g_test_add("/dav/propfind-principal", DavFixture, NULL,
             dav_fixture_setup, test_propfind_principal, dav_fixture_teardown);
  g_test_add("/dav/report-empty", DavFixture, NULL,
             dav_fixture_setup, test_report_empty, dav_fixture_teardown);
  g_test_add("/dav/method-not-allowed", DavFixture, NULL,
             dav_fixture_setup, test_method_not_allowed, dav_fixture_teardown);

  int result = g_test_run();
  xmlCleanupParser();
  return result;
}
