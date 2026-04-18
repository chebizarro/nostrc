/* nd-dav-server.c - Localhost CalDAV/CardDAV/WebDAV bridge
 *
 * SPDX-License-Identifier: MIT
 *
 * Scaffold implementation: answers OPTIONS, PROPFIND depth 0/1, and
 * well-known redirects with empty calendar/address book collections.
 * Authenticated via HTTP Basic (password = bearer token from NdTokenStore).
 */

#include "nd-dav-server.h"
#include "nd-token-store.h"
#include "nd-ical.h"
#include "nd-calendar-store.h"
#include "nd-vcard.h"
#include "nd-contact-store.h"

#include <libsoup/soup.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlreader.h>
#include <string.h>

/* ---- Error domain ---- */

#define ND_DAV_SERVER_ERROR (nd_dav_server_error_quark())
G_DEFINE_QUARK(nd-dav-server-error-quark, nd_dav_server_error)

enum {
  ND_DAV_SERVER_ERROR_BIND = 1,
  ND_DAV_SERVER_ERROR_ALREADY_RUNNING
};

/* ---- DAV namespace URIs ---- */

#define DAV_NS       "DAV:"
#define CALDAV_NS    "urn:ietf:params:xml:ns:caldav"
#define CARDDAV_NS   "urn:ietf:params:xml:ns:carddav"

/* ---- Private structure ---- */

struct _NdDavServer {
  GObject       parent_instance;

  SoupServer   *soup;           /* owned */
  NdTokenStore *token_store;    /* not owned */
  gboolean      running;
  gchar        *listen_addr;    /* owned */
  guint         listen_port;

  /* Default account for auth (v1: single account) */
  gchar        *account_id;     /* owned, nullable */

  /* Calendar store for NIP-52 events */
  NdCalendarStore *cal_store;    /* owned */

  /* Contact store for kind-30085 contacts */
  NdContactStore *contact_store;  /* owned */
};

G_DEFINE_TYPE(NdDavServer, nd_dav_server, G_TYPE_OBJECT)

/* ---- Forward declarations ---- */

static void xml_write_event_response(xmlTextWriterPtr w,
                                     const NdCalendarEvent *event);
static void xml_write_contact_response(xmlTextWriterPtr w,
                                       const NdContact *contact);

/* ---- XML helpers ---- */

/**
 * Start a DAV multistatus XML response.
 * Caller must call xmlTextWriterEndDocument + xmlBufferFree when done.
 */
static xmlTextWriterPtr
xml_begin_multistatus(xmlBufferPtr *out_buf)
{
  xmlBufferPtr buf = xmlBufferCreate();
  xmlTextWriterPtr w = xmlNewTextWriterMemory(buf, 0);

  xmlTextWriterStartDocument(w, "1.0", "UTF-8", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "multistatus",
                               BAD_CAST DAV_NS);
  xmlTextWriterWriteAttributeNS(w, BAD_CAST "xmlns", BAD_CAST "C",
                                 NULL, BAD_CAST CALDAV_NS);
  xmlTextWriterWriteAttributeNS(w, BAD_CAST "xmlns", BAD_CAST "CR",
                                 NULL, BAD_CAST CARDDAV_NS);

  *out_buf = buf;
  return w;
}

static void
xml_start_response(xmlTextWriterPtr w, const gchar *href)
{
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "response", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "href", NULL);
  xmlTextWriterWriteString(w, BAD_CAST href);
  xmlTextWriterEndElement(w); /* href */
}

static void
xml_start_propstat_ok(xmlTextWriterPtr w)
{
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "propstat", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "prop", NULL);
}

static void
xml_end_propstat_ok(xmlTextWriterPtr w)
{
  xmlTextWriterEndElement(w); /* prop */
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "status", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "HTTP/1.1 200 OK");
  xmlTextWriterEndElement(w); /* status */
  xmlTextWriterEndElement(w); /* propstat */
}

static void
xml_end_response(xmlTextWriterPtr w)
{
  xmlTextWriterEndElement(w); /* response */
}

static void
xml_write_resourcetype_collection(xmlTextWriterPtr w)
{
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "resourcetype", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "collection", NULL);
  xmlTextWriterEndElement(w);
  xmlTextWriterEndElement(w);
}

static void
xml_write_displayname(xmlTextWriterPtr w, const gchar *name)
{
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "displayname", NULL);
  xmlTextWriterWriteString(w, BAD_CAST name);
  xmlTextWriterEndElement(w);
}

/* ---- Auth helper ---- */

/**
 * Validate HTTP Basic auth. In v1, we accept any username and check
 * the password against the default account's bearer token.
 *
 * Returns TRUE if authenticated. Sets 401 + WWW-Authenticate on failure.
 */
static void
set_unauthorized(SoupServerMessage *msg)
{
  soup_server_message_set_status(msg, 401, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "WWW-Authenticate",
                               "Basic realm=\"nostr-dav\"");
}

static gboolean
check_auth(NdDavServer *self, SoupServerMessage *msg)
{
  if (self->account_id == NULL) {
    /* No account configured yet — allow unauthenticated for OPTIONS/discovery */
    return TRUE;
  }

  SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
  const gchar *auth_header = soup_message_headers_get_one(req_hdrs, "Authorization");

  if (auth_header == NULL || !g_str_has_prefix(auth_header, "Basic ")) {
    set_unauthorized(msg);
    return FALSE;
  }

  /* Decode Base64 credentials */
  const gchar *b64 = auth_header + 6;
  gsize decoded_len = 0;
  guchar *decoded = g_base64_decode(b64, &decoded_len);
  if (decoded == NULL || decoded_len == 0) {
    g_free(decoded);
    set_unauthorized(msg);
    return FALSE;
  }

  /* Format: "username:password" — we only care about the password */
  gchar *creds = g_strndup((const gchar *)decoded, decoded_len);
  g_free(decoded);

  const gchar *colon = strchr(creds, ':');
  if (colon == NULL) {
    g_free(creds);
    set_unauthorized(msg);
    return FALSE;
  }

  const gchar *password = colon + 1;
  gboolean valid = nd_token_store_validate(self->token_store,
                                           self->account_id, password);
  g_free(creds);

  if (!valid) {
    set_unauthorized(msg);
    return FALSE;
  }

  return TRUE;
}

/* ---- PROPFIND handlers ---- */

/**
 * Build the principal (root) PROPFIND response.
 * Advertises the current-user-principal, calendar-home-set,
 * and addressbook-home-set.
 */
static void
handle_propfind_root(NdDavServer *self, SoupServerMessage *msg, int depth)
{
  (void)self;

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  /* Root collection */
  xml_start_response(w, "/");
  xml_start_propstat_ok(w);
  xml_write_resourcetype_collection(w);
  xml_write_displayname(w, "Nostr DAV Bridge");

  /* current-user-principal */
  xmlTextWriterStartElementNS(w, BAD_CAST "D",
                               BAD_CAST "current-user-principal", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "href", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "/principals/me/");
  xmlTextWriterEndElement(w); /* href */
  xmlTextWriterEndElement(w); /* current-user-principal */

  xml_end_propstat_ok(w);
  xml_end_response(w);

  if (depth >= 1) {
    /* /calendars/ collection */
    xml_start_response(w, "/calendars/");
    xml_start_propstat_ok(w);
    xml_write_resourcetype_collection(w);
    xml_write_displayname(w, "Calendars");
    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* /contacts/ collection */
    xml_start_response(w, "/contacts/");
    xml_start_propstat_ok(w);
    xml_write_resourcetype_collection(w);
    xml_write_displayname(w, "Contacts");
    xml_end_propstat_ok(w);
    xml_end_response(w);
  }

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);

  xmlBufferFree(buf);
}

/**
 * PROPFIND /principals/me/ — user principal discovery.
 * Returns calendar-home-set and addressbook-home-set.
 */
static void
handle_propfind_principal(NdDavServer *self, SoupServerMessage *msg)
{
  (void)self;

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  xml_start_response(w, "/principals/me/");
  xml_start_propstat_ok(w);

  /* resourcetype: principal */
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "resourcetype", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "collection", NULL);
  xmlTextWriterEndElement(w);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "principal", NULL);
  xmlTextWriterEndElement(w);
  xmlTextWriterEndElement(w); /* resourcetype */

  xml_write_displayname(w, "Nostr User");

  /* calendar-home-set */
  xmlTextWriterStartElementNS(w, BAD_CAST "C",
                               BAD_CAST "calendar-home-set", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "href", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "/calendars/");
  xmlTextWriterEndElement(w);
  xmlTextWriterEndElement(w);

  /* addressbook-home-set */
  xmlTextWriterStartElementNS(w, BAD_CAST "CR",
                               BAD_CAST "addressbook-home-set", NULL);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "href", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "/contacts/");
  xmlTextWriterEndElement(w);
  xmlTextWriterEndElement(w);

  xml_end_propstat_ok(w);
  xml_end_response(w);

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);

  xmlBufferFree(buf);
}

/**
 * PROPFIND /calendars/ — calendar home (v1: empty, no calendars yet).
 * depth=1 adds the default "Nostr Events" calendar collection.
 */
static void
handle_propfind_calendars(NdDavServer *self, SoupServerMessage *msg, int depth)
{
  (void)self;

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  /* Calendar home collection */
  xml_start_response(w, "/calendars/");
  xml_start_propstat_ok(w);
  xml_write_resourcetype_collection(w);
  xml_write_displayname(w, "Calendars");
  xml_end_propstat_ok(w);
  xml_end_response(w);

  if (depth >= 1) {
    /* Default calendar: empty NIP-52 calendar */
    xml_start_response(w, "/calendars/nostr/");
    xml_start_propstat_ok(w);

    /* resourcetype: collection + calendar */
    xmlTextWriterStartElementNS(w, BAD_CAST "D",
                                 BAD_CAST "resourcetype", NULL);
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "collection", NULL);
    xmlTextWriterEndElement(w);
    xmlTextWriterStartElementNS(w, BAD_CAST "C", BAD_CAST "calendar", NULL);
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w); /* resourcetype */

    xml_write_displayname(w, "Nostr Events");

    /* supported-calendar-component-set */
    xmlTextWriterStartElementNS(w, BAD_CAST "C",
                                 BAD_CAST "supported-calendar-component-set", NULL);
    xmlTextWriterStartElementNS(w, BAD_CAST "C", BAD_CAST "comp", NULL);
    xmlTextWriterWriteAttribute(w, BAD_CAST "name", BAD_CAST "VEVENT");
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w);

    /* getctag: changes with every store mutation */
    g_autofree gchar *ctag = nd_calendar_store_get_ctag(self->cal_store);
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getctag", NULL);
    xmlTextWriterWriteString(w, BAD_CAST ctag);
    xmlTextWriterEndElement(w);

    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* List individual events as resources */
    g_autoptr(GPtrArray) events = nd_calendar_store_list_all(self->cal_store);
    for (guint i = 0; i < events->len; i++) {
      const NdCalendarEvent *event = g_ptr_array_index(events, i);
      xml_write_event_response(w, event);
    }
  }

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);

  xmlBufferFree(buf);
}

/**
 * PROPFIND /contacts/ — addressbook home with contact listing.
 */
static void
handle_propfind_contacts(NdDavServer *self, SoupServerMessage *msg, int depth)
{
  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  /* Addressbook home */
  xml_start_response(w, "/contacts/");
  xml_start_propstat_ok(w);
  xml_write_resourcetype_collection(w);
  xml_write_displayname(w, "Contacts");
  xml_end_propstat_ok(w);
  xml_end_response(w);

  if (depth >= 1) {
    /* Default addressbook */
    xml_start_response(w, "/contacts/nostr/");
    xml_start_propstat_ok(w);

    /* resourcetype: collection + addressbook */
    xmlTextWriterStartElementNS(w, BAD_CAST "D",
                                 BAD_CAST "resourcetype", NULL);
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "collection", NULL);
    xmlTextWriterEndElement(w);
    xmlTextWriterStartElementNS(w, BAD_CAST "CR",
                                 BAD_CAST "addressbook", NULL);
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w); /* resourcetype */

    xml_write_displayname(w, "Nostr Contacts");

    /* getctag */
    g_autofree gchar *ctag = nd_contact_store_get_ctag(self->contact_store);
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getctag", NULL);
    xmlTextWriterWriteString(w, BAD_CAST ctag);
    xmlTextWriterEndElement(w);

    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* List individual contacts */
    g_autoptr(GPtrArray) contacts = nd_contact_store_list_all(self->contact_store);
    for (guint i = 0; i < contacts->len; i++) {
      const NdContact *contact = g_ptr_array_index(contacts, i);
      xml_write_contact_response(w, contact);
    }
  }

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);

  xmlBufferFree(buf);
}

/* ---- CalDAV resource handlers ---- */

/**
 * Extract the event UID from a path like /calendars/nostr/<uid>.ics
 * Returns NULL if the path doesn't match.
 */
static gchar *
extract_calendar_uid(const gchar *path)
{
  if (!g_str_has_prefix(path, "/calendars/nostr/"))
    return NULL;

  const gchar *start = path + strlen("/calendars/nostr/");
  if (*start == '\0')
    return NULL;

  gchar *uid = g_strdup(start);
  /* Strip .ics extension if present */
  gsize len = strlen(uid);
  if (len > 4 && g_str_has_suffix(uid, ".ics"))
    uid[len - 4] = '\0';

  if (uid[0] == '\0') {
    g_free(uid);
    return NULL;
  }

  return uid;
}

/**
 * Write a single event as a DAV response element in a multistatus.
 */
static void
xml_write_event_response(xmlTextWriterPtr w,
                         const NdCalendarEvent *event)
{
  g_autofree gchar *href = g_strdup_printf("/calendars/nostr/%s.ics",
                                           event->uid);
  xml_start_response(w, href);
  xml_start_propstat_ok(w);

  /* getetag */
  g_autofree gchar *etag = nd_ical_compute_etag(event);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getetag", NULL);
  xmlTextWriterWriteString(w, BAD_CAST etag);
  xmlTextWriterEndElement(w);

  /* getcontenttype */
  xmlTextWriterStartElementNS(w, BAD_CAST "D",
                               BAD_CAST "getcontenttype", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "text/calendar; charset=utf-8");
  xmlTextWriterEndElement(w);

  /* displayname */
  if (event->summary)
    xml_write_displayname(w, event->summary);

  xml_end_propstat_ok(w);
  xml_end_response(w);
}

/**
 * Handle PUT /calendars/nostr/<uid>.ics — create or update event.
 */
static void
handle_put_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  SoupMessageBody *body = soup_server_message_get_request_body(msg);
  g_autoptr(GBytes) body_bytes = NULL;
  if (body)
    body_bytes = soup_message_body_flatten(body);

  if (body_bytes == NULL || g_bytes_get_size(body_bytes) == 0) {
    soup_server_message_set_status(msg, 400, NULL);
    return;
  }

  gsize data_len = 0;
  const gchar *ics_text = g_bytes_get_data(body_bytes, &data_len);
  g_autofree gchar *ics_copy = g_strndup(ics_text, data_len);

  GError *err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(ics_copy, &err);
  if (event == NULL) {
    if (err && err->code == 2 /* ND_ICAL_ERROR_UNSUPPORTED */) {
      /* RRULE — 501 Not Implemented */
      soup_server_message_set_status(msg, 501, NULL);
      SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
      soup_message_headers_replace(hdrs, "X-Reason", err->message);
    } else {
      soup_server_message_set_status(msg, 400, NULL);
      SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
      soup_message_headers_replace(hdrs, "X-Reason",
                                   err ? err->message : "Invalid ICS");
    }
    g_clear_error(&err);
    return;
  }

  /* Override UID from path if different (path takes precedence) */
  if (!g_str_equal(event->uid, uid)) {
    g_free(event->uid);
    event->uid = g_strdup(uid);
  }

  /* Check if this is a create or update */
  gboolean is_new = (nd_calendar_store_get(self->cal_store, uid) == NULL);

  /* Store the event (takes ownership) */
  nd_calendar_store_put(self->cal_store, event);

  /* Retrieve the stored event to compute ETag */
  const NdCalendarEvent *stored = nd_calendar_store_get(self->cal_store, uid);
  g_autofree gchar *etag = nd_ical_compute_etag(stored);

  soup_server_message_set_status(msg, is_new ? 201 : 204, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *location = g_strdup_printf("/calendars/nostr/%s.ics", uid);
  soup_message_headers_replace(hdrs, "Location", location);

  g_message("nostr-dav: %s calendar event %s (%s)",
            is_new ? "created" : "updated", uid,
            stored->summary ? stored->summary : "untitled");
}

/**
 * Handle GET /calendars/nostr/<uid>.ics — retrieve event as ICS.
 */
static void
handle_get_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  const NdCalendarEvent *event = nd_calendar_store_get(self->cal_store, uid);
  if (event == NULL) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  g_autofree gchar *ics = nd_ical_generate_vevent(event);
  g_autofree gchar *etag = nd_ical_compute_etag(event);

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "text/calendar; charset=utf-8");
  soup_message_headers_replace(hdrs, "ETag", etag);
  soup_server_message_set_response(msg, "text/calendar; charset=utf-8",
                                   SOUP_MEMORY_COPY, ics, strlen(ics));
}

/**
 * Handle DELETE /calendars/nostr/<uid>.ics — remove event.
 */
static void
handle_delete_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  if (!nd_calendar_store_remove(self->cal_store, uid)) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  soup_server_message_set_status(msg, 204, NULL);
  g_message("nostr-dav: deleted calendar event %s", uid);
}

/**
 * Handle REPORT on /calendars/nostr/ — return all events in multistatus.
 * (v1: ignores calendar-query filters, returns everything)
 */
static void
handle_report_calendar(NdDavServer *self, SoupServerMessage *msg)
{
  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  g_autoptr(GPtrArray) events = nd_calendar_store_list_all(self->cal_store);
  for (guint i = 0; i < events->len; i++) {
    const NdCalendarEvent *event = g_ptr_array_index(events, i);
    xml_write_event_response(w, event);
  }

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);
  xmlBufferFree(buf);
}

/* ---- CardDAV resource handlers ---- */

/**
 * Extract the contact UID from a path like /contacts/nostr/<uid>.vcf
 * Returns NULL if the path doesn't match.
 */
static gchar *
extract_contact_uid(const gchar *path)
{
  if (!g_str_has_prefix(path, "/contacts/nostr/"))
    return NULL;

  const gchar *start = path + strlen("/contacts/nostr/");
  if (*start == '\0')
    return NULL;

  gchar *uid = g_strdup(start);
  gsize len = strlen(uid);
  if (len > 4 && g_str_has_suffix(uid, ".vcf"))
    uid[len - 4] = '\0';

  if (uid[0] == '\0') {
    g_free(uid);
    return NULL;
  }

  return uid;
}

/**
 * Write a single contact as a DAV response element in a multistatus.
 */
static void
xml_write_contact_response(xmlTextWriterPtr w, const NdContact *contact)
{
  g_autofree gchar *href = g_strdup_printf("/contacts/nostr/%s.vcf",
                                           contact->uid);
  xml_start_response(w, href);
  xml_start_propstat_ok(w);

  /* getetag */
  g_autofree gchar *etag = nd_vcard_compute_etag(contact);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getetag", NULL);
  xmlTextWriterWriteString(w, BAD_CAST etag);
  xmlTextWriterEndElement(w);

  /* getcontenttype */
  xmlTextWriterStartElementNS(w, BAD_CAST "D",
                               BAD_CAST "getcontenttype", NULL);
  xmlTextWriterWriteString(w, BAD_CAST "text/vcard; charset=utf-8");
  xmlTextWriterEndElement(w);

  /* displayname */
  if (contact->fn)
    xml_write_displayname(w, contact->fn);

  xml_end_propstat_ok(w);
  xml_end_response(w);
}

/**
 * Handle PUT /contacts/nostr/<uid>.vcf — create or update contact.
 */
static void
handle_put_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  SoupMessageBody *body = soup_server_message_get_request_body(msg);
  g_autoptr(GBytes) body_bytes = NULL;
  if (body)
    body_bytes = soup_message_body_flatten(body);

  if (body_bytes == NULL || g_bytes_get_size(body_bytes) == 0) {
    soup_server_message_set_status(msg, 400, NULL);
    return;
  }

  gsize data_len = 0;
  const gchar *vcard_text = g_bytes_get_data(body_bytes, &data_len);
  g_autofree gchar *vcard_copy = g_strndup(vcard_text, data_len);

  GError *err = NULL;
  NdContact *contact = nd_vcard_parse(vcard_copy, &err);
  if (contact == NULL) {
    soup_server_message_set_status(msg, 400, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "X-Reason",
                                 err ? err->message : "Invalid vCard");
    g_clear_error(&err);
    return;
  }

  /* Override UID from path if different */
  if (!g_str_equal(contact->uid, uid)) {
    g_free(contact->uid);
    contact->uid = g_strdup(uid);
  }

  gboolean is_new = (nd_contact_store_get(self->contact_store, uid) == NULL);

  nd_contact_store_put(self->contact_store, contact);

  const NdContact *stored = nd_contact_store_get(self->contact_store, uid);
  g_autofree gchar *etag = nd_vcard_compute_etag(stored);

  soup_server_message_set_status(msg, is_new ? 201 : 204, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *location = g_strdup_printf("/contacts/nostr/%s.vcf", uid);
  soup_message_headers_replace(hdrs, "Location", location);

  g_message("nostr-dav: %s contact %s (%s)",
            is_new ? "created" : "updated", uid,
            stored->fn ? stored->fn : "unnamed");
}

/**
 * Handle GET /contacts/nostr/<uid>.vcf — retrieve contact as vCard.
 */
static void
handle_get_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  const NdContact *contact = nd_contact_store_get(self->contact_store, uid);
  if (contact == NULL) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  g_autofree gchar *vcard = nd_vcard_generate(contact);
  g_autofree gchar *etag = nd_vcard_compute_etag(contact);

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "text/vcard; charset=utf-8");
  soup_message_headers_replace(hdrs, "ETag", etag);
  soup_server_message_set_response(msg, "text/vcard; charset=utf-8",
                                   SOUP_MEMORY_COPY, vcard, strlen(vcard));
}

/**
 * Handle DELETE /contacts/nostr/<uid>.vcf — remove contact.
 */
static void
handle_delete_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  if (!nd_contact_store_remove(self->contact_store, uid)) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  soup_server_message_set_status(msg, 204, NULL);
  g_message("nostr-dav: deleted contact %s", uid);
}

/**
 * Handle REPORT on /contacts/nostr/ — return all contacts in multistatus.
 */
static void
handle_report_contacts(NdDavServer *self, SoupServerMessage *msg)
{
  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  g_autoptr(GPtrArray) contacts = nd_contact_store_list_all(self->contact_store);
  for (guint i = 0; i < contacts->len; i++) {
    const NdContact *contact = g_ptr_array_index(contacts, i);
    xml_write_contact_response(w, contact);
  }

  xmlTextWriterEndElement(w); /* multistatus */
  xmlTextWriterEndDocument(w);
  xmlFreeTextWriter(w);

  const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
  gsize xml_len = xmlBufferLength(buf);

  soup_server_message_set_status(msg, 207, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type",
                               "application/xml; charset=utf-8");
  soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                   SOUP_MEMORY_COPY, xml_str, xml_len);
  xmlBufferFree(buf);
}

/* ---- Parse Depth header ---- */

static int
get_depth_header(SoupServerMessage *msg)
{
  SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
  const gchar *depth_str = soup_message_headers_get_one(hdrs, "Depth");

  if (depth_str == NULL || g_str_equal(depth_str, "infinity"))
    return 1; /* Default to depth 1 for safety */

  return (int)g_ascii_strtoll(depth_str, NULL, 10);
}

/* ---- Main request dispatcher ---- */

static void
on_request(SoupServer        *soup_server,
           SoupServerMessage *msg,
           const gchar       *path,
           GHashTable        *query,
           gpointer           user_data)
{
  (void)soup_server;
  (void)query;

  NdDavServer *self = ND_DAV_SERVER(user_data);
  const gchar *method = soup_server_message_get_method(msg);

  g_debug("nostr-dav: %s %s", method, path);

  /* === OPTIONS — advertise DAV capabilities === */
  if (g_str_equal(method, "OPTIONS")) {
    soup_server_message_set_status(msg, 200, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "DAV",
                                 "1, 2, 3, calendar-access, addressbook");
    soup_message_headers_replace(hdrs, "Allow",
                                 "OPTIONS, PROPFIND, REPORT, GET, PUT, DELETE");
    soup_message_headers_replace(hdrs, "Content-Length", "0");
    return;
  }

  /* === Well-known redirects (RFC 6764) === */
  if (g_str_equal(path, "/.well-known/caldav")) {
    soup_server_message_set_status(msg, 301, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Location", "/calendars/");
    return;
  }
  if (g_str_equal(path, "/.well-known/carddav")) {
    soup_server_message_set_status(msg, 301, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Location", "/contacts/");
    return;
  }

  /* === All other methods require auth === */
  if (!g_str_equal(method, "OPTIONS") && !check_auth(self, msg))
    return;

  /* === PROPFIND === */
  if (g_str_equal(method, "PROPFIND")) {
    int depth = get_depth_header(msg);

    if (g_str_equal(path, "/") || g_str_equal(path, "")) {
      handle_propfind_root(self, msg, depth);
      return;
    }

    if (g_str_has_prefix(path, "/principals/")) {
      handle_propfind_principal(self, msg);
      return;
    }

    if (g_str_has_prefix(path, "/calendars")) {
      /* PROPFIND on individual event resource */
      g_autofree gchar *uid = extract_calendar_uid(path);
      if (uid != NULL) {
        const NdCalendarEvent *event =
          nd_calendar_store_get(self->cal_store, uid);
        if (event == NULL) {
          soup_server_message_set_status(msg, 404, NULL);
          return;
        }
        /* Return single-resource multistatus */
        xmlBufferPtr buf = NULL;
        xmlTextWriterPtr w = xml_begin_multistatus(&buf);
        xml_write_event_response(w, event);
        xmlTextWriterEndElement(w);
        xmlTextWriterEndDocument(w);
        xmlFreeTextWriter(w);
        const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
        gsize xml_len = xmlBufferLength(buf);
        soup_server_message_set_status(msg, 207, NULL);
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "Content-Type",
                                     "application/xml; charset=utf-8");
        soup_server_message_set_response(msg,
                                         "application/xml; charset=utf-8",
                                         SOUP_MEMORY_COPY, xml_str, xml_len);
        xmlBufferFree(buf);
        return;
      }
      handle_propfind_calendars(self, msg, depth);
      return;
    }

    if (g_str_has_prefix(path, "/contacts")) {
      /* PROPFIND on individual contact resource */
      g_autofree gchar *cuid = extract_contact_uid(path);
      if (cuid != NULL) {
        const NdContact *contact =
          nd_contact_store_get(self->contact_store, cuid);
        if (contact == NULL) {
          soup_server_message_set_status(msg, 404, NULL);
          return;
        }
        xmlBufferPtr buf = NULL;
        xmlTextWriterPtr w = xml_begin_multistatus(&buf);
        xml_write_contact_response(w, contact);
        xmlTextWriterEndElement(w);
        xmlTextWriterEndDocument(w);
        xmlFreeTextWriter(w);
        const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
        gsize xml_len = xmlBufferLength(buf);
        soup_server_message_set_status(msg, 207, NULL);
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "Content-Type",
                                     "application/xml; charset=utf-8");
        soup_server_message_set_response(msg,
                                         "application/xml; charset=utf-8",
                                         SOUP_MEMORY_COPY, xml_str, xml_len);
        xmlBufferFree(buf);
        return;
      }
      handle_propfind_contacts(self, msg, depth);
      return;
    }

    /* Unknown path */
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  /* === REPORT — calendar-query / addressbook-query / sync-collection === */
  if (g_str_equal(method, "REPORT")) {
    if (g_str_has_prefix(path, "/calendars/nostr")) {
      handle_report_calendar(self, msg);
    } else if (g_str_has_prefix(path, "/contacts/nostr")) {
      handle_report_contacts(self, msg);
    } else {
      /* Empty multistatus for unknown REPORTs */
      xmlBufferPtr buf = NULL;
      xmlTextWriterPtr w = xml_begin_multistatus(&buf);
      xmlTextWriterEndElement(w);
      xmlTextWriterEndDocument(w);
      xmlFreeTextWriter(w);
      const gchar *xml_str = (const gchar *)xmlBufferContent(buf);
      gsize xml_len = xmlBufferLength(buf);
      soup_server_message_set_status(msg, 207, NULL);
      SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
      soup_message_headers_replace(rh, "Content-Type",
                                   "application/xml; charset=utf-8");
      soup_server_message_set_response(msg, "application/xml; charset=utf-8",
                                       SOUP_MEMORY_COPY, xml_str, xml_len);
      xmlBufferFree(buf);
    }
    return;
  }

  /* === PUT — create/update calendar event or contact === */
  if (g_str_equal(method, "PUT")) {
    g_autofree gchar *cal_uid = extract_calendar_uid(path);
    if (cal_uid != NULL) {
      handle_put_event(self, msg, cal_uid);
      return;
    }
    g_autofree gchar *ct_uid = extract_contact_uid(path);
    if (ct_uid != NULL) {
      handle_put_contact(self, msg, ct_uid);
      return;
    }
    soup_server_message_set_status(msg, 405, NULL);
    return;
  }

  /* === GET — retrieve calendar event or contact === */
  if (g_str_equal(method, "GET")) {
    g_autofree gchar *cal_uid = extract_calendar_uid(path);
    if (cal_uid != NULL) {
      handle_get_event(self, msg, cal_uid);
      return;
    }
    g_autofree gchar *ct_uid = extract_contact_uid(path);
    if (ct_uid != NULL) {
      handle_get_contact(self, msg, ct_uid);
      return;
    }
    soup_server_message_set_status(msg, 405, NULL);
    return;
  }

  /* === DELETE — remove calendar event or contact === */
  if (g_str_equal(method, "DELETE")) {
    g_autofree gchar *cal_uid = extract_calendar_uid(path);
    if (cal_uid != NULL) {
      handle_delete_event(self, msg, cal_uid);
      return;
    }
    g_autofree gchar *ct_uid = extract_contact_uid(path);
    if (ct_uid != NULL) {
      handle_delete_contact(self, msg, ct_uid);
      return;
    }
    soup_server_message_set_status(msg, 405, NULL);
    return;
  }

  /* === Fallback === */
  soup_server_message_set_status(msg, 405, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Allow",
                               "OPTIONS, PROPFIND, REPORT, GET, PUT, DELETE");
}

/* ---- GObject lifecycle ---- */

static void
nd_dav_server_dispose(GObject *obj)
{
  NdDavServer *self = ND_DAV_SERVER(obj);
  nd_dav_server_stop(self);
  self->token_store = NULL;
  G_OBJECT_CLASS(nd_dav_server_parent_class)->dispose(obj);
}

static void
nd_dav_server_finalize(GObject *obj)
{
  NdDavServer *self = ND_DAV_SERVER(obj);
  g_clear_object(&self->soup);
  g_clear_pointer(&self->listen_addr, g_free);
  g_clear_pointer(&self->account_id, g_free);
  if (self->cal_store) {
    nd_calendar_store_free(self->cal_store);
    self->cal_store = NULL;
  }
  if (self->contact_store) {
    nd_contact_store_free(self->contact_store);
    self->contact_store = NULL;
  }
  G_OBJECT_CLASS(nd_dav_server_parent_class)->finalize(obj);
}

static void
nd_dav_server_class_init(NdDavServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose  = nd_dav_server_dispose;
  object_class->finalize = nd_dav_server_finalize;
}

static void
nd_dav_server_init(NdDavServer *self)
{
  self->running = FALSE;
  self->listen_addr = NULL;
  self->listen_port = 0;
  self->account_id = NULL;
  self->cal_store = nd_calendar_store_new();
  self->contact_store = nd_contact_store_new();
}

/* ---- Public API ---- */

NdDavServer *
nd_dav_server_new(NdTokenStore *token_store)
{
  g_return_val_if_fail(token_store != NULL, NULL);

  NdDavServer *self = g_object_new(ND_TYPE_DAV_SERVER, NULL);
  self->token_store = token_store;

  return self;
}

gboolean
nd_dav_server_start(NdDavServer *self,
                    const gchar *address,
                    guint        port,
                    GError     **error)
{
  g_return_val_if_fail(ND_IS_DAV_SERVER(self), FALSE);

  if (self->running) {
    g_set_error_literal(error, ND_DAV_SERVER_ERROR,
                        ND_DAV_SERVER_ERROR_ALREADY_RUNNING,
                        "DAV server is already running");
    return FALSE;
  }

  self->soup = soup_server_new("server-header", "nostr-dav/1.0", NULL);
  soup_server_add_handler(self->soup, "/", on_request, self, NULL);

  g_autoptr(GInetAddress) inet_addr = g_inet_address_new_from_string(address);
  if (inet_addr == NULL) {
    g_set_error(error, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_BIND,
                "Invalid listen address: %s", address);
    g_clear_object(&self->soup);
    return FALSE;
  }

  g_autoptr(GSocketAddress) sock_addr =
    G_SOCKET_ADDRESS(g_inet_socket_address_new(inet_addr, port));

  GError *listen_err = NULL;
  if (!soup_server_listen(self->soup, sock_addr, 0, &listen_err)) {
    g_propagate_prefixed_error(error, listen_err,
                               "Failed to listen on %s:%u: ", address, port);
    g_clear_object(&self->soup);
    return FALSE;
  }

  self->running     = TRUE;
  self->listen_addr = g_strdup(address);
  self->listen_port = port;

  g_message("nostr-dav: listening on http://%s:%u", address, port);
  return TRUE;
}

void
nd_dav_server_stop(NdDavServer *self)
{
  g_return_if_fail(ND_IS_DAV_SERVER(self));

  if (!self->running)
    return;

  if (self->soup != NULL)
    soup_server_disconnect(self->soup);

  self->running = FALSE;
  g_message("nostr-dav: stopped");
}

gboolean
nd_dav_server_is_running(NdDavServer *self)
{
  g_return_val_if_fail(ND_IS_DAV_SERVER(self), FALSE);
  return self->running;
}
