/* nss-lists.h — NIP-65 relay list (kind 10002) and BUD-03 Blossom server
 * list (kind 10063): URL validation, parsing and unsigned-event builders.
 * SPDX-License-Identifier: MIT
 *
 * Pure (no I/O, no GTK). Signing and publishing live in nss-net.h.
 */
#ifndef NSS_LISTS_H
#define NSS_LISTS_H

#include <glib.h>

G_BEGIN_DECLS

#define NSS_KIND_RELAY_LIST   10002
#define NSS_KIND_BLOSSOM_LIST 10063

#define NSS_LISTS_ERROR (nss_lists_error_quark())
GQuark nss_lists_error_quark(void);
typedef enum {
  NSS_LISTS_ERROR_BAD_URL = 1,
  NSS_LISTS_ERROR_DUPLICATE,
  NSS_LISTS_ERROR_NO_MARKER,   /* relay neither read nor write */
  NSS_LISTS_ERROR_BAD_EVENT,
  NSS_LISTS_ERROR_EMPTY,
} NssListsError;

typedef struct {
  gchar   *url;
  gboolean read;
  gboolean write;
} NssRelayEntry;

NssRelayEntry *nss_relay_entry_new(const gchar *url, gboolean read, gboolean write);
void           nss_relay_entry_free(NssRelayEntry *e);

/* ws:// or wss:// with a host; no userinfo, query or fragment; trims
 * whitespace and lower-cases scheme and host. Path kept verbatim (NIP-65
 * relays are matched by exact string elsewhere). */
gchar *nss_relay_url_normalize(const gchar *in, GError **error);

/* https:// with a host; no userinfo/query/fragment; trailing slashes
 * dropped (BUD-03 servers are base URLs). */
gchar *nss_blossom_url_normalize(const gchar *in, GError **error);

/* Kind-10002 event JSON → NssRelayEntry* array (tag order; an unmarked
 * `r` tag is read+write; duplicate URLs merge their markers; invalid URLs
 * are skipped). */
GPtrArray *nss_relay_list_parse(const gchar *event_json, GError **error);

/* Unsigned kind-10002 JSON: ["r", url] for read+write, ["r", url, "read"
 * | "write"] otherwise. @pubkey_hex may be NULL (the signer fills it).
 * Errors on an invalid URL, a duplicate, or an entry with neither marker. */
gchar *nss_relay_list_build(GPtrArray *entries, const gchar *pubkey_hex,
                            gint64 created_at, GError **error);

/* URLs with the given marker, in order. */
gchar **nss_relay_list_urls(GPtrArray *entries, gboolean want_write);

/* Kind-10063 event JSON → server URLs (["server", url] tags, validated,
 * de-duplicated, order kept — the first is the preferred server). */
gchar **nss_blossom_list_parse(const gchar *event_json, GError **error);

/* Unsigned kind-10063 JSON. Errors on invalid/duplicate URLs or an empty
 * list (publishing an empty list would erase the user's servers). */
gchar *nss_blossom_list_build(const gchar *const *servers, const gchar *pubkey_hex,
                              gint64 created_at, GError **error);

/* Where to publish a new kind 10002 (NIP-65: spread it widely, and tell
 * relays being removed so they stop serving the old list):
 * union(new list, old list, signer relays). @required ← the new list's
 * write relays: success means every one of them holds the new list. */
gchar **nss_relay_list_publish_targets(GPtrArray *new_list, GPtrArray *old_list,
                                       const gchar *const *signer_relays,
                                       gchar ***required);

/* Where to publish kind 10063: the user's write relays (NIP-65), else the
 * signer's relays; all of them required. */
gchar **nss_blossom_publish_targets(GPtrArray *relay_list, const gchar *const *signer_relays,
                                    gchar ***required);

/* Ordered union without duplicates of the given NULL-terminated lists. */
gchar **nss_strv_union(const gchar *const *a, const gchar *const *b,
                       const gchar *const *c);

G_END_DECLS

#endif /* NSS_LISTS_H */
