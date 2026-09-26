/* ns-dav.h - Hand calendar/contact files to nostr-dav
 *
 * SPDX-License-Identifier: MIT
 *
 * nostr-dav already owns the calendar (NIP-52) and contact (kind 30085)
 * mappings, its local store and its publish outbox. Rather than
 * re-implement those kinds, nostr-share PUTs the file into nostr-dav's
 * documented DAV collections:
 *
 *   text/calendar → PUT http://127.0.0.1:7680/calendars/nostr/<UID>.ics
 *   text/vcard    → PUT http://127.0.0.1:7680/contacts/nostr/<UID>.vcf
 *
 * with HTTP Basic auth (user `nostr`, password = the bearer token printed
 * by `nostr-dav --show-credentials`). nostr-dav then signs and publishes
 * through its own outbox.
 */
#ifndef NS_DAV_H
#define NS_DAV_H

#include <glib.h>

#include "ns-kind.h"

G_BEGIN_DECLS

/* First UID: property value of an iCalendar/vCard body, sanitised to a
 * safe path segment. NULL when absent. Pure. */
gchar *ns_dav_extract_uid(const gchar *body);

/* One DAV resource holds one calendar object / one vCard. Fails with a
 * clear message when @body carries several (distinct UIDs, or several
 * BEGIN:VCARD). Pure. */
gboolean ns_dav_check_single(NsInputClass cls, const gchar *body, GError **error);

/* Collection-relative href for @cls and @uid (e.g. calendars/nostr/x.ics). */
gchar *ns_dav_href(NsInputClass cls, const gchar *uid);

gboolean ns_dav_stage(const gchar  *dav_url_override,
                      NsInputClass  cls,
                      GBytes       *body,
                      gboolean      dry_run,
                      gchar       **out_url,
                      GError      **error);

G_END_DECLS

#endif /* NS_DAV_H */
