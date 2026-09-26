/* ns-event.h - Tag builders and unsigned event JSON
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure (no I/O). Everything here is covered by tests/test_event.c.
 */
#ifndef NS_EVENT_H
#define NS_EVENT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* One uploaded (or about-to-be-uploaded) blob. */
typedef struct {
  gchar   *url;          /* https://server/<sha256>.<ext> */
  gchar   *mime;
  gchar    sha256[65];   /* hash of the bytes actually uploaded */
  guint64  size;
  guint    width;        /* 0 = unknown */
  guint    height;
  gchar   *alt;          /* nullable accessibility text */
} NsBlobMeta;

void ns_blob_meta_clear(NsBlobMeta *m);

/* Append ["<v0>", "<v1>", ...] (NULL-terminated varargs) to @tags. */
void ns_tags_add(JsonArray *tags, const gchar *first, ...) G_GNUC_NULL_TERMINATED;

/* True iff @tags already contains a tag whose first two values are
 * (@name, @value). */
gboolean ns_tags_has(JsonArray *tags, const gchar *name, const gchar *value);

/* http(s) URLs found in @text in order of appearance, deduplicated.
 * Trailing sentence punctuation and unbalanced closing brackets are
 * trimmed. `nostr:` URIs (NIP-27) are never returned — they are left
 * untouched in the content and get no tags from us. */
GPtrArray *ns_extract_urls(const gchar *text);

/* One ["r", url] per ns_extract_urls(@text) entry not already tagged. */
void ns_tags_add_urls(JsonArray *tags, const gchar *text);

/* NIP-92 inline media metadata:
 *   ["imeta", "url <u>", "m <mime>", "x <sha256>", "size <n>",
 *    "dim <w>x<h>"?, "alt <text>"?] */
JsonArray *ns_imeta_tag_new(const NsBlobMeta *m);

/* NIP-94 kind-1063 tags: url, m, x, ox, size, dim?, alt?. `ox` equals
 * `x`: we upload exactly the bytes we hashed (after metadata stripping)
 * and Blossom does not transform them. */
void ns_tags_add_file_metadata(JsonArray *tags, const NsBlobMeta *m);

/* BUD-01 style URL: <server>/<sha256>[.<ext>] (ext derived from @mime). */
gchar *ns_blossom_blob_url(const gchar *server, const gchar *sha256,
                           const gchar *mime);

/* BUD-03 kind-10063 server list → ordered https:// server URLs (no
 * trailing slash). Non-https entries are dropped. */
GStrv ns_blossom_servers_from_event(const gchar *event_json);

/* NIP-23 helpers. */
gchar *ns_slugify(const gchar *title);                /* never NULL/empty */
gchar *ns_markdown_title(const gchar *markdown);      /* first ATX H1, or NULL */
void   ns_tags_add_article(JsonArray *tags, const gchar *slug,
                           const gchar *title, gint64 published_at);

/* --to parsing. */
typedef enum {
  NS_RECIPIENT_NONE = 0,
  NS_RECIPIENT_MENTION,  /* npub / hex pubkey: public p-tag mention */
  NS_RECIPIENT_GROUP,    /* NIP-29 group: h tag, published only to its relay */
} NsRecipientType;

typedef struct {
  NsRecipientType type;
  gchar          *pubkey_hex;  /* MENTION */
  gchar          *npub;        /* MENTION, for the nostr: reference */
  gchar          *group_id;    /* GROUP */
  gchar          *relay_url;   /* GROUP: wss://<host> */
} NsRecipient;

void     ns_recipient_clear(NsRecipient *r);
/* Accepts npub1…, 64-hex, `<host>'<group-id>` or `wss://<host>'<group-id>`. */
gboolean ns_recipient_parse(const gchar *to, NsRecipient *out, GError **error);
void     ns_tags_add_recipient(JsonArray *tags, const NsRecipient *r);

/* {"pubkey"?,"created_at","kind","tags","content"} — compact (what the
 * signer receives) or pretty (what the dialog shows). Takes a ref on
 * nothing: @tags is serialised, not stolen. */
gchar *ns_event_unsigned_json(gint         kind,
                              gint64       created_at,
                              const gchar *pubkey_hex,
                              JsonArray   *tags,
                              const gchar *content,
                              gboolean     pretty);

/* Re-indent any JSON document for display. Returns a copy of @json if it
 * does not parse. */
gchar *ns_json_pretty(const gchar *json);

/* Lowercase hex SHA-256 of @data. */
gchar *ns_sha256_hex(const guint8 *data, gsize len);

G_END_DECLS

#endif /* NS_EVENT_H */
