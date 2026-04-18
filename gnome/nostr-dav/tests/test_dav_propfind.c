/* test_dav_propfind.c - Unit tests for nostr-dav PROPFIND responses
 *
 * SPDX-License-Identifier: MIT
 *
 * Starts an NdDavServer on a dedicated GMainContext thread, issues
 * HTTP requests via libsoup client, and validates the DAV XML responses.
 */

#include "nd-dav-server.h"
#include "nd-token-store.h"
#include "nd-ical.h"
#include "nd-calendar-store.h"
#include "nd-vcard.h"
#include "nd-contact-store.h"
#include "nd-file-entry.h"
#include "nd-file-store.h"

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
  /* root + calendars + contacts + files = 4 collections */
  g_assert_cmpint(result->nodesetval->nodeNr, ==, 4);

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

/* ---- CalDAV event lifecycle tests ---- */

/* A minimal ICS VEVENT for testing */
#define TEST_ICS_DATE_EVENT \
  "BEGIN:VCALENDAR\r\n" \
  "VERSION:2.0\r\n" \
  "PRODID:-//Test//Test//EN\r\n" \
  "BEGIN:VEVENT\r\n" \
  "UID:test-event-001\r\n" \
  "SUMMARY:Nostr Meetup\r\n" \
  "DESCRIPTION:Monthly community meetup\r\n" \
  "DTSTART;VALUE=DATE:20260420\r\n" \
  "DTEND;VALUE=DATE:20260421\r\n" \
  "LOCATION:Berlin\r\n" \
  "CATEGORIES:nostr,meetup\r\n" \
  "END:VEVENT\r\n" \
  "END:VCALENDAR\r\n"

#define TEST_ICS_TIME_EVENT \
  "BEGIN:VCALENDAR\r\n" \
  "VERSION:2.0\r\n" \
  "PRODID:-//Test//Test//EN\r\n" \
  "BEGIN:VEVENT\r\n" \
  "UID:test-event-002\r\n" \
  "SUMMARY:Dev Call\r\n" \
  "DESCRIPTION:Weekly dev sync\r\n" \
  "DTSTART;TZID=America/New_York:20260422T150000\r\n" \
  "DTEND;TZID=America/New_York:20260422T160000\r\n" \
  "LOCATION:Jitsi\r\n" \
  "URL:https://meet.jit.si/nostr-dev\r\n" \
  "END:VEVENT\r\n" \
  "END:VCALENDAR\r\n"

static SoupMessage *
make_put_ics(const gchar *url, const gchar *ics)
{
  SoupMessage *msg = soup_message_new("PUT", url);
  SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "text/calendar; charset=utf-8");
  GBytes *body = g_bytes_new_static(ics, strlen(ics));
  soup_message_set_request_body_from_bytes(msg, "text/calendar", body);
  g_bytes_unref(body);
  return msg;
}

static void
test_put_event_creates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) msg = make_put_ics(url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 201);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *etag = soup_message_headers_get_one(hdrs, "ETag");
  g_assert_nonnull(etag);
  g_assert_true(etag[0] == '"');

  const gchar *location = soup_message_headers_get_one(hdrs, "Location");
  g_assert_cmpstr(location, ==, "/calendars/nostr/test-event-001.ics");
}

static void
test_put_event_updates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);

  /* First PUT → 201 */
  g_autoptr(SoupMessage) msg1 = make_put_ics(url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) body1 = send_msg(f, msg1);
  g_assert_cmpuint(soup_message_get_status(msg1), ==, 201);

  /* Second PUT → 204 (update) */
  g_autoptr(SoupMessage) msg2 = make_put_ics(url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) body2 = send_msg(f, msg2);
  g_assert_cmpuint(soup_message_get_status(msg2), ==, 204);
}

static void
test_get_event(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT first */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_ics(put_url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* GET */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 200);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(get_msg);
  const gchar *ct = soup_message_headers_get_one(hdrs, "Content-Type");
  g_assert_nonnull(ct);
  g_assert_true(strstr(ct, "text/calendar") != NULL);

  const gchar *etag = soup_message_headers_get_one(hdrs, "ETag");
  g_assert_nonnull(etag);

  /* Verify ICS content */
  gsize len = 0;
  const gchar *ics = g_bytes_get_data(get_body, &len);
  g_assert_true(len > 0);
  g_assert_nonnull(strstr(ics, "BEGIN:VCALENDAR"));
  g_assert_nonnull(strstr(ics, "SUMMARY:Nostr Meetup"));
  g_assert_nonnull(strstr(ics, "LOCATION:Berlin"));
  g_assert_nonnull(strstr(ics, "UID:test-event-001"));
}

static void
test_get_event_not_found(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/calendars/nostr/nonexistent.ics", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 404);
}

static void
test_delete_event(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT first */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_ics(put_url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* DELETE */
  g_autofree gchar *del_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) del_msg = soup_message_new("DELETE", del_url);
  g_autoptr(GBytes) del_body = send_msg(f, del_msg);
  g_assert_cmpuint(soup_message_get_status(del_msg), ==, 204);

  /* GET should now 404 */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 404);
}

static void
test_delete_event_not_found(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/calendars/nostr/nonexistent.ics", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("DELETE", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 404);
}

static void
test_report_with_events(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT two events */
  g_autofree gchar *url1 = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) put1 = make_put_ics(url1, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) body1 = send_msg(f, put1);
  g_assert_cmpuint(soup_message_get_status(put1), ==, 201);

  g_autofree gchar *url2 = g_strdup_printf(
    "%s/calendars/nostr/test-event-002.ics", f->base_url);
  g_autoptr(SoupMessage) put2 = make_put_ics(url2, TEST_ICS_TIME_EVENT);
  g_autoptr(GBytes) body2 = send_msg(f, put2);
  g_assert_cmpuint(soup_message_get_status(put2), ==, 201);

  /* REPORT on calendar collection */
  g_autofree gchar *report_url = g_strdup_printf(
    "%s/calendars/nostr/", f->base_url);
  g_autoptr(SoupMessage) report = soup_message_new("REPORT", report_url);
  g_autoptr(GBytes) report_body = send_msg(f, report);

  g_assert_cmpuint(soup_message_get_status(report), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(report_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  /* Should have 2 responses */
  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 2);

  /* Each should have an ETag */
  xmlXPathObjectPtr etags = xmlXPathEvalExpression(
    BAD_CAST "//D:getetag", ctx);
  g_assert_cmpint(etags->nodesetval->nodeNr, ==, 2);

  xmlXPathFreeObject(etags);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_calendar_depth1_with_events(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT an event */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_ics(put_url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND /calendars/ depth 1 — should list the calendar + events */
  g_autofree gchar *pf_url = g_strdup_printf("%s/calendars/", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 1);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);

  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(pf_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  /* Should have 3 responses: /calendars/ + /calendars/nostr/ + the event */
  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 3);

  /* ctag should be non-zero (store has been mutated) */
  xmlXPathObjectPtr ctag = xmlXPathEvalExpression(
    BAD_CAST "//D:getctag", ctx);
  g_assert_cmpint(ctag->nodesetval->nodeNr, ==, 1);
  xmlChar *ctag_val = xmlNodeGetContent(ctag->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)ctag_val, !=, "0");
  xmlFree(ctag_val);

  xmlXPathFreeObject(ctag);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_propfind_single_event(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT an event */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_ics(put_url, TEST_ICS_DATE_EVENT);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND on the individual event resource */
  g_autofree gchar *pf_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-001.ics", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 0);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);

  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(pf_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  /* 1 response for the event */
  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 1);

  /* Has ETag */
  xmlXPathObjectPtr etag = xmlXPathEvalExpression(
    BAD_CAST "//D:getetag", ctx);
  g_assert_cmpint(etag->nodesetval->nodeNr, ==, 1);

  /* Has displayname = "Nostr Meetup" */
  xmlXPathObjectPtr dn = xmlXPathEvalExpression(
    BAD_CAST "//D:displayname", ctx);
  g_assert_cmpint(dn->nodesetval->nodeNr, ==, 1);
  xmlChar *name = xmlNodeGetContent(dn->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)name, ==, "Nostr Meetup");
  xmlFree(name);

  xmlXPathFreeObject(dn);
  xmlXPathFreeObject(etag);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_put_time_event(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a time-based event */
  g_autofree gchar *url = g_strdup_printf(
    "%s/calendars/nostr/test-event-002.ics", f->base_url);
  g_autoptr(SoupMessage) msg = make_put_ics(url, TEST_ICS_TIME_EVENT);
  g_autoptr(GBytes) body = send_msg(f, msg);
  g_assert_cmpuint(soup_message_get_status(msg), ==, 201);

  /* GET it back and verify TZID roundtrip */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/calendars/nostr/test-event-002.ics", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 200);

  gsize len = 0;
  const gchar *ics = g_bytes_get_data(get_body, &len);
  g_assert_nonnull(strstr(ics, "SUMMARY:Dev Call"));
  g_assert_nonnull(strstr(ics, "TZID=America/New_York"));
  g_assert_nonnull(strstr(ics, "URL:https://meet.jit.si/nostr-dev"));
}

/* ---- ICS ↔ NIP-52 unit tests ---- */

static void
test_ical_parse_date_event(void)
{
  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(TEST_ICS_DATE_EVENT, &err);
  g_assert_no_error(err);
  g_assert_nonnull(event);

  g_assert_cmpstr(event->uid, ==, "test-event-001");
  g_assert_cmpstr(event->summary, ==, "Nostr Meetup");
  g_assert_cmpstr(event->description, ==, "Monthly community meetup");
  g_assert_true(event->is_date_only);
  g_assert_cmpint(event->kind, ==, ND_NIP52_KIND_DATE);
  g_assert_cmpstr(event->dtstart_date, ==, "2026-04-20");
  g_assert_cmpstr(event->dtend_date, ==, "2026-04-21");
  g_assert_cmpstr(event->location, ==, "Berlin");

  /* Categories */
  g_assert_nonnull(event->categories);
  g_assert_cmpstr(event->categories[0], ==, "nostr");
  g_assert_cmpstr(event->categories[1], ==, "meetup");
  g_assert_null(event->categories[2]);

  nd_calendar_event_free(event);
}

static void
test_ical_parse_time_event(void)
{
  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(TEST_ICS_TIME_EVENT, &err);
  g_assert_no_error(err);
  g_assert_nonnull(event);

  g_assert_cmpstr(event->uid, ==, "test-event-002");
  g_assert_cmpstr(event->summary, ==, "Dev Call");
  g_assert_false(event->is_date_only);
  g_assert_cmpint(event->kind, ==, ND_NIP52_KIND_TIME);
  g_assert_cmpstr(event->start_tzid, ==, "America/New_York");
  g_assert_cmpstr(event->end_tzid, ==, "America/New_York");
  g_assert_true(event->dtstart_ts > 0);
  g_assert_true(event->dtend_ts > event->dtstart_ts);
  g_assert_cmpstr(event->url, ==, "https://meet.jit.si/nostr-dev");

  nd_calendar_event_free(event);
}

static void
test_ical_roundtrip_ics(void)
{
  /* Parse → generate → re-parse and verify key fields survive */
  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(TEST_ICS_DATE_EVENT, &err);
  g_assert_no_error(err);

  g_autofree gchar *ics = nd_ical_generate_vevent(event);
  g_assert_nonnull(ics);
  g_assert_nonnull(strstr(ics, "BEGIN:VCALENDAR"));
  g_assert_nonnull(strstr(ics, "SUMMARY:Nostr Meetup"));

  /* Re-parse */
  NdCalendarEvent *event2 = nd_ical_parse_vevent(ics, &err);
  g_assert_no_error(err);
  g_assert_nonnull(event2);
  g_assert_cmpstr(event2->uid, ==, event->uid);
  g_assert_cmpstr(event2->summary, ==, event->summary);
  g_assert_true(event2->is_date_only);
  g_assert_cmpstr(event2->dtstart_date, ==, event->dtstart_date);

  nd_calendar_event_free(event);
  nd_calendar_event_free(event2);
}

static void
test_ical_nip52_roundtrip(void)
{
  /* Parse ICS → NIP-52 JSON → parse back → verify */
  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(TEST_ICS_DATE_EVENT, &err);
  g_assert_no_error(err);

  g_autofree gchar *json = nd_ical_event_to_nip52_json(event);
  g_assert_nonnull(json);

  NdCalendarEvent *event2 = nd_ical_event_from_nip52_json(json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(event2);

  g_assert_cmpstr(event2->uid, ==, "test-event-001");
  g_assert_cmpstr(event2->summary, ==, "Nostr Meetup");
  g_assert_cmpstr(event2->description, ==, "Monthly community meetup");
  g_assert_true(event2->is_date_only);
  g_assert_cmpint(event2->kind, ==, ND_NIP52_KIND_DATE);
  g_assert_cmpstr(event2->dtstart_date, ==, "2026-04-20");
  g_assert_cmpstr(event2->location, ==, "Berlin");

  nd_calendar_event_free(event);
  nd_calendar_event_free(event2);
}

static void
test_ical_etag_stable(void)
{
  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(TEST_ICS_DATE_EVENT, &err);
  g_assert_no_error(err);

  g_autofree gchar *etag1 = nd_ical_compute_etag(event);
  g_autofree gchar *etag2 = nd_ical_compute_etag(event);
  g_assert_cmpstr(etag1, ==, etag2);
  g_assert_true(etag1[0] == '"');

  nd_calendar_event_free(event);
}

static void
test_ical_reject_rrule(void)
{
  const gchar *ics_rrule =
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:recurring-001\r\n"
    "SUMMARY:Weekly Standup\r\n"
    "DTSTART:20260420T090000Z\r\n"
    "RRULE:FREQ=WEEKLY\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(ics_rrule, &err);
  g_assert_null(event);
  g_assert_nonnull(err);
  g_assert_cmpint(err->code, ==, 2); /* ND_ICAL_ERROR_UNSUPPORTED */
  g_clear_error(&err);
}

/* ---- CardDAV contact lifecycle tests ---- */

#define TEST_VCARD_SIMPLE \
  "BEGIN:VCARD\r\n" \
  "VERSION:4.0\r\n" \
  "UID:contact-001\r\n" \
  "FN:Alice Nakamoto\r\n" \
  "N:Nakamoto;Alice;;;\r\n" \
  "EMAIL;TYPE=work:alice@example.com\r\n" \
  "TEL;TYPE=cell:+1-555-0123\r\n" \
  "ORG:Nostr Foundation\r\n" \
  "END:VCARD\r\n"

#define TEST_VCARD_FULL \
  "BEGIN:VCARD\r\n" \
  "VERSION:4.0\r\n" \
  "UID:contact-002\r\n" \
  "FN:Bob Satoshi\r\n" \
  "N:Satoshi;Bob;;;\r\n" \
  "EMAIL;TYPE=home:bob@example.org\r\n" \
  "TEL;TYPE=work:+44-20-7946-0958\r\n" \
  "ORG:Lightning Labs\r\n" \
  "TITLE:Lead Developer\r\n" \
  "ADR;TYPE=work:;;456 Block Ave;London;;EC1A 1BB;UK\r\n" \
  "URL:https://bob.example.org\r\n" \
  "NOTE:Co-author of LN spec\r\n" \
  "END:VCARD\r\n"

static SoupMessage *
make_put_vcard(const gchar *url, const gchar *vcard)
{
  SoupMessage *msg = soup_message_new("PUT", url);
  SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "text/vcard; charset=utf-8");
  GBytes *body = g_bytes_new_static(vcard, strlen(vcard));
  soup_message_set_request_body_from_bytes(msg, "text/vcard", body);
  g_bytes_unref(body);
  return msg;
}

static void
test_carddav_put_creates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) msg = make_put_vcard(url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 201);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *etag = soup_message_headers_get_one(hdrs, "ETag");
  g_assert_nonnull(etag);
  g_assert_true(etag[0] == '"');

  const gchar *location = soup_message_headers_get_one(hdrs, "Location");
  g_assert_cmpstr(location, ==, "/contacts/nostr/contact-001.vcf");
}

static void
test_carddav_put_updates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);

  g_autoptr(SoupMessage) msg1 = make_put_vcard(url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) body1 = send_msg(f, msg1);
  g_assert_cmpuint(soup_message_get_status(msg1), ==, 201);

  g_autoptr(SoupMessage) msg2 = make_put_vcard(url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) body2 = send_msg(f, msg2);
  g_assert_cmpuint(soup_message_get_status(msg2), ==, 204);
}

static void
test_carddav_get_contact(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_vcard(put_url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* GET */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 200);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(get_msg);
  const gchar *ct = soup_message_headers_get_one(hdrs, "Content-Type");
  g_assert_nonnull(ct);
  g_assert_true(strstr(ct, "text/vcard") != NULL);

  gsize len = 0;
  const gchar *vcard = g_bytes_get_data(get_body, &len);
  g_assert_true(len > 0);
  g_assert_nonnull(strstr(vcard, "BEGIN:VCARD"));
  g_assert_nonnull(strstr(vcard, "FN:Alice Nakamoto"));
  g_assert_nonnull(strstr(vcard, "UID:contact-001"));
}

static void
test_carddav_get_not_found(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/contacts/nostr/nonexistent.vcf", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 404);
}

static void
test_carddav_delete_contact(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_vcard(put_url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* DELETE */
  g_autofree gchar *del_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) del_msg = soup_message_new("DELETE", del_url);
  g_autoptr(GBytes) del_body = send_msg(f, del_msg);
  g_assert_cmpuint(soup_message_get_status(del_msg), ==, 204);

  /* GET should 404 */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 404);
}

static void
test_carddav_report_with_contacts(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT two contacts */
  g_autofree gchar *url1 = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) put1 = make_put_vcard(url1, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) body1 = send_msg(f, put1);
  g_assert_cmpuint(soup_message_get_status(put1), ==, 201);

  g_autofree gchar *url2 = g_strdup_printf(
    "%s/contacts/nostr/contact-002.vcf", f->base_url);
  g_autoptr(SoupMessage) put2 = make_put_vcard(url2, TEST_VCARD_FULL);
  g_autoptr(GBytes) body2 = send_msg(f, put2);
  g_assert_cmpuint(soup_message_get_status(put2), ==, 201);

  /* REPORT */
  g_autofree gchar *report_url = g_strdup_printf(
    "%s/contacts/nostr/", f->base_url);
  g_autoptr(SoupMessage) report = soup_message_new("REPORT", report_url);
  g_autoptr(GBytes) report_body = send_msg(f, report);

  g_assert_cmpuint(soup_message_get_status(report), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(report_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 2);

  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_carddav_propfind_depth1_with_contacts(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a contact */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_vcard(put_url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND /contacts/ depth 1 */
  g_autofree gchar *pf_url = g_strdup_printf("%s/contacts/", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 1);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);

  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(pf_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  /* 3 responses: /contacts/ + /contacts/nostr/ + the contact */
  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 3);

  /* ctag should be non-zero */
  xmlXPathObjectPtr ctag = xmlXPathEvalExpression(
    BAD_CAST "//D:getctag", ctx);
  g_assert_cmpint(ctag->nodesetval->nodeNr, ==, 1);
  xmlChar *ctag_val = xmlNodeGetContent(ctag->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)ctag_val, !=, "0");
  xmlFree(ctag_val);

  xmlXPathFreeObject(ctag);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

static void
test_carddav_propfind_single_contact(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a contact */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_vcard(put_url, TEST_VCARD_SIMPLE);
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND on individual contact */
  g_autofree gchar *pf_url = g_strdup_printf(
    "%s/contacts/nostr/contact-001.vcf", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 0);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);

  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize len = 0;
  const gchar *xml = g_bytes_get_data(pf_body, &len);

  xmlDocPtr doc = xmlReadMemory(xml, (int)len, "response.xml", NULL,
                                XML_PARSE_NONET | XML_PARSE_NOERROR);
  g_assert_nonnull(doc);

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathRegisterNs(ctx, BAD_CAST "D", BAD_CAST "DAV:");

  xmlXPathObjectPtr responses = xmlXPathEvalExpression(
    BAD_CAST "//D:response", ctx);
  g_assert_cmpint(responses->nodesetval->nodeNr, ==, 1);

  xmlXPathObjectPtr dn = xmlXPathEvalExpression(
    BAD_CAST "//D:displayname", ctx);
  g_assert_cmpint(dn->nodesetval->nodeNr, ==, 1);
  xmlChar *name = xmlNodeGetContent(dn->nodesetval->nodeTab[0]);
  g_assert_cmpstr((const gchar *)name, ==, "Alice Nakamoto");
  xmlFree(name);

  xmlXPathFreeObject(dn);
  xmlXPathFreeObject(responses);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
}

/* ---- vCard unit tests ---- */

static void
test_vcard_parse_simple(void)
{
  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(TEST_VCARD_SIMPLE, &err);
  g_assert_no_error(err);
  g_assert_nonnull(contact);

  g_assert_cmpstr(contact->uid, ==, "contact-001");
  g_assert_cmpstr(contact->fn, ==, "Alice Nakamoto");
  g_assert_cmpstr(contact->n, ==, "Nakamoto;Alice;;;");
  g_assert_cmpstr(contact->email, ==, "alice@example.com");
  g_assert_cmpstr(contact->email_type, ==, "work");
  g_assert_cmpstr(contact->tel, ==, "+1-555-0123");
  g_assert_cmpstr(contact->tel_type, ==, "cell");
  g_assert_cmpstr(contact->org, ==, "Nostr Foundation");

  nd_contact_free(contact);
}

static void
test_vcard_parse_full(void)
{
  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(TEST_VCARD_FULL, &err);
  g_assert_no_error(err);
  g_assert_nonnull(contact);

  g_assert_cmpstr(contact->uid, ==, "contact-002");
  g_assert_cmpstr(contact->fn, ==, "Bob Satoshi");
  g_assert_cmpstr(contact->title, ==, "Lead Developer");
  g_assert_cmpstr(contact->url, ==, "https://bob.example.org");
  g_assert_cmpstr(contact->note, ==, "Co-author of LN spec");
  g_assert_nonnull(contact->adr);
  g_assert_cmpstr(contact->adr_type, ==, "work");

  nd_contact_free(contact);
}

static void
test_vcard_roundtrip(void)
{
  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(TEST_VCARD_SIMPLE, &err);
  g_assert_no_error(err);

  g_autofree gchar *vcard = nd_vcard_generate(contact);
  g_assert_nonnull(vcard);
  g_assert_nonnull(strstr(vcard, "BEGIN:VCARD"));
  g_assert_nonnull(strstr(vcard, "FN:Alice Nakamoto"));

  /* Re-parse */
  NdContact *contact2 = nd_vcard_parse(vcard, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(contact2->uid, ==, contact->uid);
  g_assert_cmpstr(contact2->fn, ==, contact->fn);
  g_assert_cmpstr(contact2->email, ==, contact->email);

  nd_contact_free(contact);
  nd_contact_free(contact2);
}

static void
test_vcard_nostr_roundtrip(void)
{
  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(TEST_VCARD_SIMPLE, &err);
  g_assert_no_error(err);

  g_autofree gchar *json = nd_vcard_to_nostr_json(contact);
  g_assert_nonnull(json);

  NdContact *contact2 = nd_vcard_from_nostr_json(json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(contact2);

  g_assert_cmpstr(contact2->uid, ==, "contact-001");
  g_assert_cmpstr(contact2->fn, ==, "Alice Nakamoto");

  nd_contact_free(contact);
  nd_contact_free(contact2);
}

static void
test_vcard_etag_stable(void)
{
  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(TEST_VCARD_SIMPLE, &err);
  g_assert_no_error(err);

  g_autofree gchar *etag1 = nd_vcard_compute_etag(contact);
  g_autofree gchar *etag2 = nd_vcard_compute_etag(contact);
  g_assert_cmpstr(etag1, ==, etag2);
  g_assert_true(etag1[0] == '"');

  nd_contact_free(contact);
}

/* ---- WebDAV File lifecycle tests ---- */

#define TEST_FILE_CONTENT "Hello, Nostr! This is a test file.\n"
#define TEST_FILE_CONTENT2 "Updated content for the test file.\n"

static SoupMessage *
make_put_file(const gchar *url, const gchar *content, const gchar *mime)
{
  SoupMessage *msg = soup_message_new("PUT", url);
  GBytes *body = g_bytes_new_static(content, strlen(content));
  soup_message_set_request_body_from_bytes(msg, mime, body);
  g_bytes_unref(body);
  return msg;
}

static void
test_webdav_put_creates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) msg = make_put_file(url, TEST_FILE_CONTENT,
                                             "text/plain");
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 201);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(msg);
  const gchar *etag = soup_message_headers_get_one(hdrs, "ETag");
  g_assert_nonnull(etag);
  g_assert_true(etag[0] == '"');

  const gchar *location = soup_message_headers_get_one(hdrs, "Location");
  g_assert_cmpstr(location, ==, "/files/nostr/notes/hello.txt");
}

static void
test_webdav_put_updates(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);

  /* First PUT -> 201 */
  g_autoptr(SoupMessage) msg1 = make_put_file(url, TEST_FILE_CONTENT,
                                              "text/plain");
  g_autoptr(GBytes) body1 = send_msg(f, msg1);
  g_assert_cmpuint(soup_message_get_status(msg1), ==, 201);

  /* Second PUT -> 204 (update) */
  g_autoptr(SoupMessage) msg2 = make_put_file(url, TEST_FILE_CONTENT2,
                                              "text/plain");
  g_autoptr(GBytes) body2 = send_msg(f, msg2);
  g_assert_cmpuint(soup_message_get_status(msg2), ==, 204);
}

static void
test_webdav_get_file(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT first */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_file(put_url, TEST_FILE_CONTENT,
                                                 "text/plain");
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* GET */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 200);

  SoupMessageHeaders *hdrs = soup_message_get_response_headers(get_msg);
  const gchar *ct = soup_message_headers_get_one(hdrs, "Content-Type");
  g_assert_nonnull(ct);
  g_assert_true(strstr(ct, "text/plain") != NULL);

  const gchar *etag = soup_message_headers_get_one(hdrs, "ETag");
  g_assert_nonnull(etag);

  /* Verify content */
  gsize len = 0;
  const gchar *data_ptr = g_bytes_get_data(get_body, &len);
  g_assert_cmpuint(len, ==, strlen(TEST_FILE_CONTENT));
  g_assert_true(memcmp(data_ptr, TEST_FILE_CONTENT, len) == 0);
}

static void
test_webdav_get_not_found(DavFixture *f, gconstpointer data)
{
  (void)data;

  g_autofree gchar *url = g_strdup_printf(
    "%s/files/nostr/nonexistent.txt", f->base_url);
  g_autoptr(SoupMessage) msg = soup_message_new("GET", url);
  g_autoptr(GBytes) body = send_msg(f, msg);

  g_assert_cmpuint(soup_message_get_status(msg), ==, 404);
}

static void
test_webdav_delete_file(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT first */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_file(put_url, TEST_FILE_CONTENT,
                                                 "text/plain");
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* DELETE */
  g_autofree gchar *del_url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) del_msg = soup_message_new("DELETE", del_url);
  g_autoptr(GBytes) del_body = send_msg(f, del_msg);
  g_assert_cmpuint(soup_message_get_status(del_msg), ==, 204);

  /* GET should now 404 */
  g_autofree gchar *get_url = g_strdup_printf(
    "%s/files/nostr/notes/hello.txt", f->base_url);
  g_autoptr(SoupMessage) get_msg = soup_message_new("GET", get_url);
  g_autoptr(GBytes) get_body = send_msg(f, get_msg);
  g_assert_cmpuint(soup_message_get_status(get_msg), ==, 404);
}

static void
test_webdav_report_with_files(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a file first */
  g_autofree gchar *url = g_strdup_printf(
    "%s/files/nostr/doc.md", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_file(url, "# Hello\n",
                                                 "text/markdown");
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* REPORT */
  g_autofree gchar *report_url = g_strdup_printf(
    "%s/files/nostr/", f->base_url);
  g_autoptr(SoupMessage) report_msg = soup_message_new("REPORT", report_url);
  g_autoptr(GBytes) report_body = send_msg(f, report_msg);
  g_assert_cmpuint(soup_message_get_status(report_msg), ==, 207);

  gsize rlen = 0;
  const gchar *rxml = g_bytes_get_data(report_body, &rlen);
  g_assert_nonnull(strstr(rxml, "/files/nostr/doc.md"));
}

static void
test_webdav_propfind_depth1_with_files(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a file */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/files/nostr/image.png", f->base_url);
  const gchar *fake_png = "\x89PNG fake data";
  g_autoptr(SoupMessage) put_msg = make_put_file(put_url, fake_png,
                                                 "image/png");
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND depth 1 */
  g_autofree gchar *pf_url = g_strdup_printf("%s/files/", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 1);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);
  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize plen = 0;
  const gchar *pxml = g_bytes_get_data(pf_body, &plen);
  /* Should contain the collection and the file */
  g_assert_nonnull(strstr(pxml, "/files/"));
  g_assert_nonnull(strstr(pxml, "/files/nostr/"));
  g_assert_nonnull(strstr(pxml, "/files/nostr/image.png"));
  g_assert_nonnull(strstr(pxml, "image/png"));
}

static void
test_webdav_propfind_single_file(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT a file */
  g_autofree gchar *put_url = g_strdup_printf(
    "%s/files/nostr/readme.txt", f->base_url);
  g_autoptr(SoupMessage) put_msg = make_put_file(put_url, "Read me\n",
                                                 "text/plain");
  g_autoptr(GBytes) put_body = send_msg(f, put_msg);
  g_assert_cmpuint(soup_message_get_status(put_msg), ==, 201);

  /* PROPFIND on single file */
  g_autofree gchar *pf_url = g_strdup_printf(
    "%s/files/nostr/readme.txt", f->base_url);
  g_autoptr(SoupMessage) pf_msg = make_propfind(pf_url, 0);
  g_autoptr(GBytes) pf_body = send_msg(f, pf_msg);
  g_assert_cmpuint(soup_message_get_status(pf_msg), ==, 207);

  gsize plen = 0;
  const gchar *pxml = g_bytes_get_data(pf_body, &plen);
  g_assert_nonnull(strstr(pxml, "/files/nostr/readme.txt"));
  g_assert_nonnull(strstr(pxml, "text/plain"));
}

static void
test_webdav_sha256_etag(DavFixture *f, gconstpointer data)
{
  (void)data;

  /* PUT same content twice to same path - ETags should match */
  g_autofree gchar *url = g_strdup_printf(
    "%s/files/nostr/test.txt", f->base_url);

  g_autoptr(SoupMessage) msg1 = make_put_file(url, TEST_FILE_CONTENT,
                                              "text/plain");
  g_autoptr(GBytes) body1 = send_msg(f, msg1);
  g_assert_cmpuint(soup_message_get_status(msg1), ==, 201);
  SoupMessageHeaders *h1 = soup_message_get_response_headers(msg1);
  g_autofree gchar *etag1 = g_strdup(
    soup_message_headers_get_one(h1, "ETag"));

  /* PUT again (update) -> same content -> same ETag */
  g_autoptr(SoupMessage) msg2 = make_put_file(url, TEST_FILE_CONTENT,
                                              "text/plain");
  g_autoptr(GBytes) body2 = send_msg(f, msg2);
  g_assert_cmpuint(soup_message_get_status(msg2), ==, 204);
  SoupMessageHeaders *h2 = soup_message_get_response_headers(msg2);
  const gchar *etag2 = soup_message_headers_get_one(h2, "ETag");
  g_assert_cmpstr(etag1, ==, etag2);

  /* PUT different content -> different ETag */
  g_autoptr(SoupMessage) msg3 = make_put_file(url, TEST_FILE_CONTENT2,
                                              "text/plain");
  g_autoptr(GBytes) body3 = send_msg(f, msg3);
  g_assert_cmpuint(soup_message_get_status(msg3), ==, 204);
  SoupMessageHeaders *h3 = soup_message_get_response_headers(msg3);
  const gchar *etag3 = soup_message_headers_get_one(h3, "ETag");
  g_assert_cmpstr(etag1, !=, etag3);
}

/* ---- NIP-94 unit tests ---- */

static void
test_nip94_json_roundtrip(void)
{
  const gchar *content = "Test file content for NIP-94";
  g_autoptr(GBytes) bytes = g_bytes_new_static(content, strlen(content));

  NdFileEntry *entry = nd_file_entry_new("docs/test.txt", bytes, "text/plain");
  g_assert_nonnull(entry);
  g_assert_cmpstr(entry->path, ==, "docs/test.txt");
  g_assert_cmpstr(entry->mime_type, ==, "text/plain");
  g_assert_cmpuint(entry->size, ==, strlen(content));
  g_assert_nonnull(entry->sha256);
  g_assert_cmpuint(strlen(entry->sha256), ==, 64);

  /* Generate NIP-94 JSON */
  g_autofree gchar *json = nd_file_entry_to_nip94_json(entry);
  g_assert_nonnull(json);
  g_assert_nonnull(strstr(json, "1063"));
  g_assert_nonnull(strstr(json, entry->sha256));
  g_assert_nonnull(strstr(json, "text/plain"));
  g_assert_nonnull(strstr(json, "docs/test.txt"));

  /* Parse it back */
  GError *err = NULL;
  NdFileEntry *parsed = nd_file_entry_from_nip94_json(json, &err);
  g_assert_no_error(err);
  g_assert_nonnull(parsed);
  g_assert_cmpstr(parsed->sha256, ==, entry->sha256);
  g_assert_cmpstr(parsed->mime_type, ==, "text/plain");
  g_assert_cmpstr(parsed->path, ==, "docs/test.txt");
  g_assert_cmpuint(parsed->size, ==, entry->size);
  g_assert_null(parsed->content); /* content not in JSON */

  nd_file_entry_free(entry);
  nd_file_entry_free(parsed);
}

static void
test_nip94_etag_stable(void)
{
  const gchar *content = "Stable ETag test";
  g_autoptr(GBytes) bytes = g_bytes_new_static(content, strlen(content));

  NdFileEntry *entry = nd_file_entry_new("test.bin", bytes, NULL);
  g_assert_nonnull(entry);

  g_autofree gchar *etag1 = nd_file_entry_compute_etag(entry);
  g_autofree gchar *etag2 = nd_file_entry_compute_etag(entry);
  g_assert_cmpstr(etag1, ==, etag2);
  g_assert_true(etag1[0] == '"');

  nd_file_entry_free(entry);
}

static void
test_nip94_mime_guessing(void)
{
  const gchar *content = "x";
  g_autoptr(GBytes) bytes = g_bytes_new_static(content, 1);

  NdFileEntry *e1 = nd_file_entry_new("photo.jpg", bytes, NULL);
  g_assert_cmpstr(e1->mime_type, ==, "image/jpeg");
  nd_file_entry_free(e1);

  NdFileEntry *e2 = nd_file_entry_new("doc.pdf", bytes, NULL);
  g_assert_cmpstr(e2->mime_type, ==, "application/pdf");
  nd_file_entry_free(e2);

  NdFileEntry *e3 = nd_file_entry_new("archive.zip", bytes, NULL);
  g_assert_cmpstr(e3->mime_type, ==, "application/zip");
  nd_file_entry_free(e3);

  NdFileEntry *e4 = nd_file_entry_new("mystery", bytes, NULL);
  g_assert_cmpstr(e4->mime_type, ==, "application/octet-stream");
  nd_file_entry_free(e4);

  /* Explicit mime overrides guessing */
  NdFileEntry *e5 = nd_file_entry_new("data.bin", bytes, "application/x-custom");
  g_assert_cmpstr(e5->mime_type, ==, "application/x-custom");
  nd_file_entry_free(e5);
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

  /* CalDAV event lifecycle */
  g_test_add("/caldav/put-creates", DavFixture, NULL,
             dav_fixture_setup, test_put_event_creates, dav_fixture_teardown);
  g_test_add("/caldav/put-updates", DavFixture, NULL,
             dav_fixture_setup, test_put_event_updates, dav_fixture_teardown);
  g_test_add("/caldav/get-event", DavFixture, NULL,
             dav_fixture_setup, test_get_event, dav_fixture_teardown);
  g_test_add("/caldav/get-not-found", DavFixture, NULL,
             dav_fixture_setup, test_get_event_not_found, dav_fixture_teardown);
  g_test_add("/caldav/delete-event", DavFixture, NULL,
             dav_fixture_setup, test_delete_event, dav_fixture_teardown);
  g_test_add("/caldav/delete-not-found", DavFixture, NULL,
             dav_fixture_setup, test_delete_event_not_found, dav_fixture_teardown);
  g_test_add("/caldav/report-with-events", DavFixture, NULL,
             dav_fixture_setup, test_report_with_events, dav_fixture_teardown);
  g_test_add("/caldav/propfind-depth1-with-events", DavFixture, NULL,
             dav_fixture_setup, test_propfind_calendar_depth1_with_events, dav_fixture_teardown);
  g_test_add("/caldav/propfind-single-event", DavFixture, NULL,
             dav_fixture_setup, test_propfind_single_event, dav_fixture_teardown);
  g_test_add("/caldav/put-time-event", DavFixture, NULL,
             dav_fixture_setup, test_put_time_event, dav_fixture_teardown);

  /* ICS ↔ NIP-52 unit tests */
  g_test_add_func("/ical/parse-date-event", test_ical_parse_date_event);
  g_test_add_func("/ical/parse-time-event", test_ical_parse_time_event);
  g_test_add_func("/ical/roundtrip-ics", test_ical_roundtrip_ics);
  g_test_add_func("/ical/nip52-roundtrip", test_ical_nip52_roundtrip);
  g_test_add_func("/ical/etag-stable", test_ical_etag_stable);
  g_test_add_func("/ical/reject-rrule", test_ical_reject_rrule);

  /* CardDAV contact lifecycle */
  g_test_add("/carddav/put-creates", DavFixture, NULL,
             dav_fixture_setup, test_carddav_put_creates, dav_fixture_teardown);
  g_test_add("/carddav/put-updates", DavFixture, NULL,
             dav_fixture_setup, test_carddav_put_updates, dav_fixture_teardown);
  g_test_add("/carddav/get-contact", DavFixture, NULL,
             dav_fixture_setup, test_carddav_get_contact, dav_fixture_teardown);
  g_test_add("/carddav/get-not-found", DavFixture, NULL,
             dav_fixture_setup, test_carddav_get_not_found, dav_fixture_teardown);
  g_test_add("/carddav/delete-contact", DavFixture, NULL,
             dav_fixture_setup, test_carddav_delete_contact, dav_fixture_teardown);
  g_test_add("/carddav/report-with-contacts", DavFixture, NULL,
             dav_fixture_setup, test_carddav_report_with_contacts, dav_fixture_teardown);
  g_test_add("/carddav/propfind-depth1-with-contacts", DavFixture, NULL,
             dav_fixture_setup, test_carddav_propfind_depth1_with_contacts, dav_fixture_teardown);
  g_test_add("/carddav/propfind-single-contact", DavFixture, NULL,
             dav_fixture_setup, test_carddav_propfind_single_contact, dav_fixture_teardown);

  /* vCard unit tests */
  g_test_add_func("/vcard/parse-simple", test_vcard_parse_simple);
  g_test_add_func("/vcard/parse-full", test_vcard_parse_full);
  g_test_add_func("/vcard/roundtrip", test_vcard_roundtrip);
  g_test_add_func("/vcard/nostr-roundtrip", test_vcard_nostr_roundtrip);
  g_test_add_func("/vcard/etag-stable", test_vcard_etag_stable);

  /* WebDAV file lifecycle */
  g_test_add("/webdav/put-creates", DavFixture, NULL,
             dav_fixture_setup, test_webdav_put_creates, dav_fixture_teardown);
  g_test_add("/webdav/put-updates", DavFixture, NULL,
             dav_fixture_setup, test_webdav_put_updates, dav_fixture_teardown);
  g_test_add("/webdav/get-file", DavFixture, NULL,
             dav_fixture_setup, test_webdav_get_file, dav_fixture_teardown);
  g_test_add("/webdav/get-not-found", DavFixture, NULL,
             dav_fixture_setup, test_webdav_get_not_found, dav_fixture_teardown);
  g_test_add("/webdav/delete-file", DavFixture, NULL,
             dav_fixture_setup, test_webdav_delete_file, dav_fixture_teardown);
  g_test_add("/webdav/report-with-files", DavFixture, NULL,
             dav_fixture_setup, test_webdav_report_with_files, dav_fixture_teardown);
  g_test_add("/webdav/propfind-depth1-with-files", DavFixture, NULL,
             dav_fixture_setup, test_webdav_propfind_depth1_with_files, dav_fixture_teardown);
  g_test_add("/webdav/propfind-single-file", DavFixture, NULL,
             dav_fixture_setup, test_webdav_propfind_single_file, dav_fixture_teardown);
  g_test_add("/webdav/sha256-etag", DavFixture, NULL,
             dav_fixture_setup, test_webdav_sha256_etag, dav_fixture_teardown);

  /* NIP-94 unit tests */
  g_test_add_func("/nip94/json-roundtrip", test_nip94_json_roundtrip);
  g_test_add_func("/nip94/etag-stable", test_nip94_etag_stable);
  g_test_add_func("/nip94/mime-guessing", test_nip94_mime_guessing);

  int result = g_test_run();
  xmlCleanupParser();
  return result;
}
