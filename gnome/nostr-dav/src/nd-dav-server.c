/* nd-dav-server.c - Localhost CalDAV/CardDAV/WebDAV bridge
 *
 * SPDX-License-Identifier: MIT
 *
 * Answers OPTIONS, PROPFIND depth 0/1, REPORT, GET, PUT, DELETE and the
 * well-known redirects over SQLite-backed calendar/contact/file stores.
 * Authenticated via HTTP Basic (password = bearer token from NdTokenStore);
 * fails closed when no account is configured.
 */

#include "nd-dav-server.h"
#include "nd-token-store.h"
#include "nd-ical.h"
#include "nd-calendar-store.h"
#include "nd-vcard.h"
#include "nd-contact-store.h"
#include "nd-file-entry.h"
#include "nd-file-store.h"
#include "nd-publisher.h"
#include "nd-store-db.h"

#include <libsoup/soup.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlreader.h>
#include <sqlite3.h>
#include <string.h>

/* Fetch (kind, pubkey) for a row that a DAV DELETE is about to remove
 * so nd_publisher_stage_tombstone() can build a well-formed `a` tag
 * from the addressable pointer the row represents. Returns FALSE when
 * the row has vanished or the SELECT itself failed — either way the
 * DELETE should not attempt to stage a tombstone. Contacts and files
 * store no `kind` column because their kind is fixed by the collection;
 * callers pass @kind_col = NULL and rely on the compile-time value. */
static gboolean
fetch_row_addressable(NdStoreDb   *db,
                      const gchar *table,
                      const gchar *key_col,
                      const gchar *key_val,
                      const gchar *kind_col,   /* NULL if kind is constant */
                      int         *out_kind,   /* only set when kind_col != NULL */
                      gchar      **out_pubkey_hex)
{
  if (out_pubkey_hex) *out_pubkey_hex = NULL;
  sqlite3 *h = nd_store_db_get_handle(db);
  g_autofree gchar *sql = kind_col
    ? g_strdup_printf("SELECT %s, pubkey FROM %s WHERE %s = ?1",
                      kind_col, table, key_col)
    : g_strdup_printf("SELECT pubkey FROM %s WHERE %s = ?1",
                      table, key_col);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return FALSE;
  sqlite3_bind_text(stmt, 1, key_val, -1, SQLITE_TRANSIENT);
  gboolean ok = FALSE;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (kind_col) {
      if (out_kind) *out_kind = sqlite3_column_int(stmt, 0);
      const unsigned char *pk = sqlite3_column_text(stmt, 1);
      if (out_pubkey_hex && pk) *out_pubkey_hex = g_strdup((const gchar *)pk);
    } else {
      const unsigned char *pk = sqlite3_column_text(stmt, 0);
      if (out_pubkey_hex && pk) *out_pubkey_hex = g_strdup((const gchar *)pk);
    }
    ok = TRUE;
  }
  sqlite3_finalize(stmt);
  return ok;
}

/* ---- Error domain ---- */

G_DEFINE_QUARK(nd-dav-server-error-quark, nd_dav_server_error)

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

  /* Account whose token authorizes requests (v1: single account).
   * NULL means unconfigured: start() refuses and check_auth() rejects. */
  gchar        *account_id;     /* owned, nullable */

  NdStoreDb    *db;             /* owned ref */

  /* Outbox publisher — not owned. When NULL, DAV writes stay local. */
  NdPublisher  *publisher;

  /* Calendar store for NIP-52 events */
  NdCalendarStore *cal_store;    /* owned */

  /* Contact store for kind-30085 contacts */
  NdContactStore *contact_store;  /* owned */

  /* File store for NIP-94 files */
  NdFileStore *file_store;  /* owned */
};

G_DEFINE_TYPE(NdDavServer, nd_dav_server, G_TYPE_OBJECT)

/* ---- Forward declarations ---- */

static void xml_write_event_response(xmlTextWriterPtr w,
                                     const NdCalendarEvent *event);
static void xml_write_contact_response(xmlTextWriterPtr w,
                                       const NdContact *contact);
static void xml_write_file_response(xmlTextWriterPtr w,
                                    const NdFileEntry *entry);

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

/**
 * Close the multistatus document and send it as a 207 response.
 */
static void
send_multistatus(SoupServerMessage *msg, xmlTextWriterPtr w, xmlBufferPtr buf)
{
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
 * Respond 500 for a store failure. Takes ownership of @err.
 */
static void
respond_store_error(SoupServerMessage *msg, GError *err, const gchar *what)
{
  g_warning("nostr-dav: %s failed: %s", what,
            err ? err->message : "unknown error");
  g_clear_error(&err);
  soup_server_message_set_status(msg, 500, NULL);
}

/* ---- Auth helper ---- */

static void
set_unauthorized(SoupServerMessage *msg)
{
  soup_server_message_set_status(msg, 401, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "WWW-Authenticate",
                               "Basic realm=\"nostr-dav\"");
}

/**
 * Validate HTTP Basic auth: any username, password must equal the
 * configured account's bearer token. Fails closed when no account is
 * configured. Returns TRUE if authenticated; sets 401 +
 * WWW-Authenticate on failure.
 */
static gboolean
check_auth(NdDavServer *self, SoupServerMessage *msg)
{
  if (self->account_id == NULL) {
    set_unauthorized(msg);
    return FALSE;
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

    /* /files/ collection */
    xml_start_response(w, "/files/");
    xml_start_propstat_ok(w);
    xml_write_resourcetype_collection(w);
    xml_write_displayname(w, "Files");
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
  /* Read everything before emitting XML: a store failure must be a 500,
   * never an empty listing that a client would sync as "all deleted". */
  g_autofree gchar *ctag = NULL;
  g_autoptr(GPtrArray) events = NULL;
  if (depth >= 1) {
    GError *err = NULL;
    ctag = nd_calendar_store_get_ctag(self->cal_store, &err);
    if (ctag != NULL)
      events = nd_calendar_store_list_all(self->cal_store, &err);
    if (events == NULL) {
      respond_store_error(msg, err, "list calendar events");
      return;
    }
  }

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

    /* getctag: persistent generation, bumped with every store mutation */
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getctag", NULL);
    xmlTextWriterWriteString(w, BAD_CAST ctag);
    xmlTextWriterEndElement(w);

    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* List individual events as resources */
    for (guint i = 0; i < events->len; i++) {
      const NdCalendarEvent *event = g_ptr_array_index(events, i);
      xml_write_event_response(w, event);
    }
  }

  send_multistatus(msg, w, buf);
}

/**
 * PROPFIND /contacts/ — addressbook home with contact listing.
 */
static void
handle_propfind_contacts(NdDavServer *self, SoupServerMessage *msg, int depth)
{
  g_autofree gchar *ctag = NULL;
  g_autoptr(GPtrArray) contacts = NULL;
  if (depth >= 1) {
    GError *err = NULL;
    ctag = nd_contact_store_get_ctag(self->contact_store, &err);
    if (ctag != NULL)
      contacts = nd_contact_store_list_all(self->contact_store, &err);
    if (contacts == NULL) {
      respond_store_error(msg, err, "list contacts");
      return;
    }
  }

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
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getctag", NULL);
    xmlTextWriterWriteString(w, BAD_CAST ctag);
    xmlTextWriterEndElement(w);

    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* List individual contacts */
    for (guint i = 0; i < contacts->len; i++) {
      const NdContact *contact = g_ptr_array_index(contacts, i);
      xml_write_contact_response(w, contact);
    }
  }

  send_multistatus(msg, w, buf);
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

  gboolean is_new = FALSE;
  GError *store_err = NULL;
  if (!nd_calendar_store_put(self->cal_store, event, &is_new, &store_err)) {
    respond_store_error(msg, store_err, "store calendar event");
    nd_calendar_event_free(event);
    return;
  }

  if (self->publisher != NULL) {
    GError *pub_err = NULL;
    if (!nd_publisher_stage_calendar_put(self->publisher, uid, &pub_err)) {
      g_warning("nostr-dav: could not stage publish for %s: %s", uid,
                pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
    }
  }

  g_autofree gchar *etag = nd_ical_compute_etag(event);

  soup_server_message_set_status(msg, is_new ? 201 : 204, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *location = g_strdup_printf("/calendars/nostr/%s.ics", uid);
  soup_message_headers_replace(hdrs, "Location", location);

  g_message("nostr-dav: %s calendar event %s (%s)",
            is_new ? "created" : "updated", uid,
            event->summary ? event->summary : "untitled");
  nd_calendar_event_free(event);
}

/**
 * Handle GET /calendars/nostr/<uid>.ics — retrieve event as ICS.
 */
static void
handle_get_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  GError *err = NULL;
  NdCalendarEvent *event = nd_calendar_store_get(self->cal_store, uid, &err);
  if (event == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read calendar event");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  g_autofree gchar *ics = nd_ical_generate_vevent(event);
  g_autofree gchar *etag = nd_ical_compute_etag(event);
  nd_calendar_event_free(event);

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
 *
 * NIP-09: after the local row is gone, stage a kind-5 tombstone in the
 * outbox so the deletion propagates to the same relay set the original
 * event went to. Address-replaceable events on Nostr are otherwise
 * “undeletable from the outside” without a matching kind-5 (nostrc-ls2c).
 */
static void
handle_delete_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  /* Read the addressable coordinates BEFORE deleting; the tombstone
   * needs kind + pubkey and the row is about to disappear. */
  int target_kind = ND_NIP52_KIND_TIME;
  g_autofree gchar *pubkey_hex = NULL;
  gboolean had_row = fetch_row_addressable(self->db, "events", "uid", uid,
                                           "kind", &target_kind, &pubkey_hex);

  gboolean removed = FALSE;
  GError *err = NULL;
  if (!nd_calendar_store_remove(self->cal_store, uid, &removed, &err)) {
    respond_store_error(msg, err, "delete calendar event");
    return;
  }
  if (!removed) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  if (had_row && self->publisher != NULL) {
    GError *tomb_err = NULL;
    if (!nd_publisher_stage_tombstone(self->publisher, target_kind,
                                      pubkey_hex, uid, &tomb_err)) {
      g_warning("nostr-dav: could not stage tombstone for %s: %s", uid,
                tomb_err ? tomb_err->message : "unknown");
      g_clear_error(&tomb_err);
    }
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
  GError *err = NULL;
  g_autoptr(GPtrArray) events =
    nd_calendar_store_list_all(self->cal_store, &err);
  if (events == NULL) {
    respond_store_error(msg, err, "list calendar events");
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  for (guint i = 0; i < events->len; i++) {
    const NdCalendarEvent *event = g_ptr_array_index(events, i);
    xml_write_event_response(w, event);
  }
  send_multistatus(msg, w, buf);
}

/**
 * PROPFIND on a single event resource.
 */
static void
handle_propfind_event(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  GError *err = NULL;
  NdCalendarEvent *event = nd_calendar_store_get(self->cal_store, uid, &err);
  if (event == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read calendar event");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  xml_write_event_response(w, event);
  send_multistatus(msg, w, buf);
  nd_calendar_event_free(event);
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

  gboolean is_new = FALSE;
  GError *store_err = NULL;
  if (!nd_contact_store_put(self->contact_store, contact, &is_new, &store_err)) {
    respond_store_error(msg, store_err, "store contact");
    nd_contact_free(contact);
    return;
  }

  if (self->publisher != NULL) {
    GError *pub_err = NULL;
    if (!nd_publisher_stage_contact_put(self->publisher, uid, &pub_err)) {
      g_warning("nostr-dav: could not stage publish for %s: %s", uid,
                pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
    }
  }

  g_autofree gchar *etag = nd_vcard_compute_etag(contact);

  soup_server_message_set_status(msg, is_new ? 201 : 204, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *location = g_strdup_printf("/contacts/nostr/%s.vcf", uid);
  soup_message_headers_replace(hdrs, "Location", location);

  g_message("nostr-dav: %s contact %s (%s)",
            is_new ? "created" : "updated", uid,
            contact->fn ? contact->fn : "unnamed");
  nd_contact_free(contact);
}

/**
 * Handle GET /contacts/nostr/<uid>.vcf — retrieve contact as vCard.
 */
static void
handle_get_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  GError *err = NULL;
  NdContact *contact = nd_contact_store_get(self->contact_store, uid, &err);
  if (contact == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read contact");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  g_autofree gchar *vcard = nd_vcard_generate(contact);
  g_autofree gchar *etag = nd_vcard_compute_etag(contact);
  nd_contact_free(contact);

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
 *
 * See handle_delete_event() for the tombstone rationale (nostrc-ls2c).
 * Contacts are always kind 30085 so no per-row kind lookup is needed.
 */
static void
handle_delete_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  g_autofree gchar *pubkey_hex = NULL;
  gboolean had_row = fetch_row_addressable(self->db, "contacts", "uid", uid,
                                           NULL, NULL, &pubkey_hex);

  gboolean removed = FALSE;
  GError *err = NULL;
  if (!nd_contact_store_remove(self->contact_store, uid, &removed, &err)) {
    respond_store_error(msg, err, "delete contact");
    return;
  }
  if (!removed) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  if (had_row && self->publisher != NULL) {
    GError *tomb_err = NULL;
    if (!nd_publisher_stage_tombstone(self->publisher, ND_CONTACT_KIND,
                                      pubkey_hex, uid, &tomb_err)) {
      g_warning("nostr-dav: could not stage tombstone for contact %s: %s",
                uid, tomb_err ? tomb_err->message : "unknown");
      g_clear_error(&tomb_err);
    }
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
  GError *err = NULL;
  g_autoptr(GPtrArray) contacts =
    nd_contact_store_list_all(self->contact_store, &err);
  if (contacts == NULL) {
    respond_store_error(msg, err, "list contacts");
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  for (guint i = 0; i < contacts->len; i++) {
    const NdContact *contact = g_ptr_array_index(contacts, i);
    xml_write_contact_response(w, contact);
  }
  send_multistatus(msg, w, buf);
}

/**
 * PROPFIND on a single contact resource.
 */
static void
handle_propfind_contact(NdDavServer *self, SoupServerMessage *msg, const gchar *uid)
{
  GError *err = NULL;
  NdContact *contact = nd_contact_store_get(self->contact_store, uid, &err);
  if (contact == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read contact");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  xml_write_contact_response(w, contact);
  send_multistatus(msg, w, buf);
  nd_contact_free(contact);
}

/* ---- WebDAV File handlers ---- */

/**
 * Extract the file path from a URL like /files/nostr/<path>
 * Returns NULL if the path doesn't match.
 */
static gchar *
extract_file_path(const gchar *url_path)
{
  if (!g_str_has_prefix(url_path, "/files/nostr/"))
    return NULL;

  const gchar *start = url_path + strlen("/files/nostr/");
  if (*start == '\0')
    return NULL;

  return g_strdup(start);
}

/**
 * Write a single file as a DAV response element in a multistatus.
 */
static void
xml_write_file_response(xmlTextWriterPtr w, const NdFileEntry *entry)
{
  g_autofree gchar *href = g_strdup_printf("/files/nostr/%s", entry->path);
  xml_start_response(w, href);
  xml_start_propstat_ok(w);

  /* getetag */
  g_autofree gchar *etag = nd_file_entry_compute_etag(entry);
  xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getetag", NULL);
  xmlTextWriterWriteString(w, BAD_CAST etag);
  xmlTextWriterEndElement(w);

  /* getcontenttype */
  xmlTextWriterStartElementNS(w, BAD_CAST "D",
                               BAD_CAST "getcontenttype", NULL);
  xmlTextWriterWriteString(w, BAD_CAST entry->mime_type);
  xmlTextWriterEndElement(w);

  /* getcontentlength */
  {
    g_autofree gchar *size_str = g_strdup_printf("%" G_GSIZE_FORMAT,
                                                 entry->size);
    xmlTextWriterStartElementNS(w, BAD_CAST "D",
                                 BAD_CAST "getcontentlength", NULL);
    xmlTextWriterWriteString(w, BAD_CAST size_str);
    xmlTextWriterEndElement(w);
  }

  /* displayname */
  if (entry->display_name)
    xml_write_displayname(w, entry->display_name);

  /* getlastmodified (RFC 2822 format) */
  if (entry->modified_at > 0) {
    time_t t = (time_t)entry->modified_at;
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    xmlTextWriterStartElementNS(w, BAD_CAST "D",
                                 BAD_CAST "getlastmodified", NULL);
    xmlTextWriterWriteString(w, BAD_CAST date_buf);
    xmlTextWriterEndElement(w);
  }

  xml_end_propstat_ok(w);
  xml_end_response(w);
}

/**
 * Handle PROPFIND /files/ — file collection listing.
 */
static void
handle_propfind_files(NdDavServer *self, SoupServerMessage *msg, int depth)
{
  g_autofree gchar *ctag = NULL;
  g_autoptr(GPtrArray) files = NULL;
  if (depth >= 1) {
    GError *err = NULL;
    ctag = nd_file_store_get_ctag(self->file_store, &err);
    if (ctag != NULL)
      files = nd_file_store_list_all(self->file_store, &err);
    if (files == NULL) {
      respond_store_error(msg, err, "list files");
      return;
    }
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);

  /* Files home collection */
  xml_start_response(w, "/files/");
  xml_start_propstat_ok(w);
  xml_write_resourcetype_collection(w);
  xml_write_displayname(w, "Files");
  xml_end_propstat_ok(w);
  xml_end_response(w);

  if (depth >= 1) {
    /* Default file collection */
    xml_start_response(w, "/files/nostr/");
    xml_start_propstat_ok(w);

    /* resourcetype: collection */
    xmlTextWriterStartElementNS(w, BAD_CAST "D",
                                 BAD_CAST "resourcetype", NULL);
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "collection", NULL);
    xmlTextWriterEndElement(w);
    xmlTextWriterEndElement(w); /* resourcetype */

    xml_write_displayname(w, "Nostr Files");

    /* getctag */
    xmlTextWriterStartElementNS(w, BAD_CAST "D", BAD_CAST "getctag", NULL);
    xmlTextWriterWriteString(w, BAD_CAST ctag);
    xmlTextWriterEndElement(w);

    xml_end_propstat_ok(w);
    xml_end_response(w);

    /* List individual files as resources */
    for (guint i = 0; i < files->len; i++) {
      const NdFileEntry *entry = g_ptr_array_index(files, i);
      xml_write_file_response(w, entry);
    }
  }

  send_multistatus(msg, w, buf);
}

/**
 * Handle PUT /files/nostr/<path> — create or update file.
 */
static void
handle_put_file(NdDavServer *self, SoupServerMessage *msg, const gchar *file_path)
{
  SoupMessageBody *body = soup_server_message_get_request_body(msg);
  g_autoptr(GBytes) body_bytes = NULL;
  if (body)
    body_bytes = soup_message_body_flatten(body);

  if (body_bytes == NULL || g_bytes_get_size(body_bytes) == 0) {
    soup_server_message_set_status(msg, 400, NULL);
    return;
  }

  /* Detect MIME type from Content-Type header or guess from path */
  SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
  const gchar *content_type = soup_message_headers_get_content_type(
    req_hdrs, NULL);

  const gchar *mime = NULL;
  if (content_type != NULL && !g_str_equal(content_type, "application/octet-stream"))
    mime = content_type;

  NdFileEntry *entry = nd_file_entry_new(file_path, body_bytes, mime);

  gboolean is_new = FALSE;
  GError *store_err = NULL;
  if (!nd_file_store_put(self->file_store, entry, &is_new, &store_err)) {
    respond_store_error(msg, store_err, "store file");
    nd_file_entry_free(entry);
    return;
  }

  g_autofree gchar *etag = nd_file_entry_compute_etag(entry);

  soup_server_message_set_status(msg, is_new ? 201 : 204, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *location = g_strdup_printf("/files/nostr/%s", file_path);
  soup_message_headers_replace(hdrs, "Location", location);

  g_message("nostr-dav: %s file %s (%s, %" G_GSIZE_FORMAT " bytes)",
            is_new ? "created" : "updated", file_path,
            entry->mime_type, entry->size);
  nd_file_entry_free(entry);
}

/**
 * Handle GET /files/nostr/<path> — retrieve file content.
 */
static void
handle_get_file(NdDavServer *self, SoupServerMessage *msg, const gchar *file_path)
{
  GError *err = NULL;
  NdFileEntry *entry = nd_file_store_get(self->file_store, file_path, &err);
  if (entry == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read file");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  g_autofree gchar *etag = nd_file_entry_compute_etag(entry);

  gsize data_len = 0;
  const gchar *data = g_bytes_get_data(entry->content, &data_len);

  soup_server_message_set_status(msg, 200, NULL);
  SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(hdrs, "Content-Type", entry->mime_type);
  soup_message_headers_replace(hdrs, "ETag", etag);

  g_autofree gchar *len_str = g_strdup_printf("%" G_GSIZE_FORMAT, data_len);
  soup_message_headers_replace(hdrs, "Content-Length", len_str);

  soup_server_message_set_response(msg, entry->mime_type,
                                   SOUP_MEMORY_COPY, data, data_len);
  nd_file_entry_free(entry);
}

/**
 * Handle DELETE /files/nostr/<path> — remove file.
 *
 * See handle_delete_event() for the tombstone rationale (nostrc-ls2c).
 * NIP-94 file entries are kind 1063; the `d` tag equals the storage
 * path we key the row by.
 */
static void
handle_delete_file(NdDavServer *self, SoupServerMessage *msg, const gchar *file_path)
{
  g_autofree gchar *pubkey_hex = NULL;
  gboolean had_row = fetch_row_addressable(self->db, "files", "path", file_path,
                                           NULL, NULL, &pubkey_hex);

  gboolean removed = FALSE;
  GError *err = NULL;
  if (!nd_file_store_remove(self->file_store, file_path, &removed, &err)) {
    respond_store_error(msg, err, "delete file");
    return;
  }
  if (!removed) {
    soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  if (had_row && self->publisher != NULL) {
    GError *tomb_err = NULL;
    if (!nd_publisher_stage_tombstone(self->publisher, ND_NIP94_KIND,
                                      pubkey_hex, file_path, &tomb_err)) {
      g_warning("nostr-dav: could not stage tombstone for file %s: %s",
                file_path, tomb_err ? tomb_err->message : "unknown");
      g_clear_error(&tomb_err);
    }
  }

  soup_server_message_set_status(msg, 204, NULL);
  g_message("nostr-dav: deleted file %s", file_path);
}

/**
 * Handle REPORT on /files/nostr/ — return all files in multistatus.
 * (v1: returns everything, no query filtering)
 */
static void
handle_report_files(NdDavServer *self, SoupServerMessage *msg)
{
  GError *err = NULL;
  g_autoptr(GPtrArray) files = nd_file_store_list_all(self->file_store, &err);
  if (files == NULL) {
    respond_store_error(msg, err, "list files");
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  for (guint i = 0; i < files->len; i++) {
    const NdFileEntry *entry = g_ptr_array_index(files, i);
    xml_write_file_response(w, entry);
  }
  send_multistatus(msg, w, buf);
}

/**
 * PROPFIND on a single file resource.
 */
static void
handle_propfind_file(NdDavServer *self, SoupServerMessage *msg, const gchar *file_path)
{
  GError *err = NULL;
  NdFileEntry *entry = nd_file_store_get(self->file_store, file_path, &err);
  if (entry == NULL) {
    if (err != NULL)
      respond_store_error(msg, err, "read file");
    else
      soup_server_message_set_status(msg, 404, NULL);
    return;
  }

  xmlBufferPtr buf = NULL;
  xmlTextWriterPtr w = xml_begin_multistatus(&buf);
  xml_write_file_response(w, entry);
  send_multistatus(msg, w, buf);
  nd_file_entry_free(entry);
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
  if (!check_auth(self, msg))
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
        handle_propfind_event(self, msg, uid);
        return;
      }
      handle_propfind_calendars(self, msg, depth);
      return;
    }

    if (g_str_has_prefix(path, "/contacts")) {
      /* PROPFIND on individual contact resource */
      g_autofree gchar *cuid = extract_contact_uid(path);
      if (cuid != NULL) {
        handle_propfind_contact(self, msg, cuid);
        return;
      }
      handle_propfind_contacts(self, msg, depth);
      return;
    }

    if (g_str_has_prefix(path, "/files")) {
      /* PROPFIND on individual file resource */
      g_autofree gchar *fpath = extract_file_path(path);
      if (fpath != NULL) {
        handle_propfind_file(self, msg, fpath);
        return;
      }
      handle_propfind_files(self, msg, depth);
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
    } else if (g_str_has_prefix(path, "/files/nostr")) {
      handle_report_files(self, msg);
    } else {
      /* Empty multistatus for unknown REPORTs */
      xmlBufferPtr buf = NULL;
      xmlTextWriterPtr w = xml_begin_multistatus(&buf);
      send_multistatus(msg, w, buf);
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
    g_autofree gchar *f_path = extract_file_path(path);
    if (f_path != NULL) {
      handle_put_file(self, msg, f_path);
      return;
    }
    soup_server_message_set_status(msg, 405, NULL);
    return;
  }

  /* === GET — retrieve calendar event, contact, or file === */
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
    g_autofree gchar *f_path = extract_file_path(path);
    if (f_path != NULL) {
      handle_get_file(self, msg, f_path);
      return;
    }
    soup_server_message_set_status(msg, 405, NULL);
    return;
  }

  /* === DELETE — remove calendar event, contact, or file === */
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
    g_autofree gchar *f_path = extract_file_path(path);
    if (f_path != NULL) {
      handle_delete_file(self, msg, f_path);
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
  g_clear_pointer(&self->cal_store, nd_calendar_store_free);
  g_clear_pointer(&self->contact_store, nd_contact_store_free);
  g_clear_pointer(&self->file_store, nd_file_store_free);
  g_clear_pointer(&self->db, nd_store_db_unref);
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
}

/* ---- Public API ---- */

NdDavServer *
nd_dav_server_new(NdTokenStore *token_store, NdStoreDb *db)
{
  g_return_val_if_fail(token_store != NULL, NULL);
  g_return_val_if_fail(db != NULL, NULL);

  NdDavServer *self = g_object_new(ND_TYPE_DAV_SERVER, NULL);
  self->token_store = token_store;
  self->db = nd_store_db_ref(db);
  self->cal_store = nd_calendar_store_new(db);
  self->contact_store = nd_contact_store_new(db);
  self->file_store = nd_file_store_new(db);

  return self;
}

void
nd_dav_server_set_account_id(NdDavServer *self, const gchar *account_id)
{
  g_return_if_fail(ND_IS_DAV_SERVER(self));
  g_free(self->account_id);
  self->account_id = g_strdup(account_id);
}

void
nd_dav_server_set_publisher(NdDavServer *self, NdPublisher *publisher)
{
  g_return_if_fail(ND_IS_DAV_SERVER(self));
  self->publisher = publisher;
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

  g_return_val_if_fail(address != NULL, FALSE);

  /* Fail closed: never accept a connection before auth is configured. */
  if (self->account_id == NULL ||
      !nd_token_store_has_token(self->token_store, self->account_id)) {
    g_set_error_literal(error, ND_DAV_SERVER_ERROR,
                        ND_DAV_SERVER_ERROR_NOT_CONFIGURED,
                        "Refusing to listen: no account with a loaded "
                        "bearer token is configured");
    return FALSE;
  }

  g_autoptr(GInetAddress) inet_addr = g_inet_address_new_from_string(address);
  if (inet_addr == NULL) {
    g_set_error(error, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_BIND,
                "Invalid listen address: %s", address);
    return FALSE;
  }

  if (!g_inet_address_get_is_loopback(inet_addr)) {
    g_set_error(error, ND_DAV_SERVER_ERROR, ND_DAV_SERVER_ERROR_NOT_LOOPBACK,
                "Refusing to listen on non-loopback address %s", address);
    return FALSE;
  }

  g_autoptr(GSocketAddress) sock_addr =
    G_SOCKET_ADDRESS(g_inet_socket_address_new(inet_addr, port));

  self->soup = soup_server_new("server-header", "nostr-dav/1.0", NULL);
  soup_server_add_handler(self->soup, "/", on_request, self, NULL);

  /* Last step: bind. */
  GError *listen_err = NULL;
  if (!soup_server_listen(self->soup, sock_addr, 0, &listen_err)) {
    g_propagate_prefixed_error(error, listen_err,
                               "Failed to listen on %s:%u: ", address, port);
    g_clear_object(&self->soup);
    return FALSE;
  }

  /* Resolve the bound port (differs from @port when @port is 0). */
  guint bound_port = port;
  GSList *listeners = soup_server_get_listeners(self->soup);
  if (listeners != NULL) {
    g_autoptr(GSocketAddress) local =
      g_socket_get_local_address(G_SOCKET(listeners->data), NULL);
    if (local != NULL && G_IS_INET_SOCKET_ADDRESS(local))
      bound_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
  }
  g_slist_free(listeners);

  self->running     = TRUE;
  g_free(self->listen_addr);
  self->listen_addr = g_strdup(address);
  self->listen_port = bound_port;

  g_message("nostr-dav: listening on http://%s:%u", address, bound_port);
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
  g_clear_object(&self->soup);

  self->running = FALSE;
  self->listen_port = 0;
  g_message("nostr-dav: stopped");
}

gboolean
nd_dav_server_is_running(NdDavServer *self)
{
  g_return_val_if_fail(ND_IS_DAV_SERVER(self), FALSE);
  return self->running;
}

guint
nd_dav_server_get_port(NdDavServer *self)
{
  g_return_val_if_fail(ND_IS_DAV_SERVER(self), 0);
  return self->running ? self->listen_port : 0;
}
