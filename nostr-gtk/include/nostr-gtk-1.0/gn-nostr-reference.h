#ifndef GN_NOSTR_REFERENCE_H
#define GN_NOSTR_REFERENCE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GN_NOSTR_REFERENCE_PERSON,
  GN_NOSTR_REFERENCE_EVENT,
  GN_NOSTR_REFERENCE_ADDRESS
} GnNostrReferenceType;

typedef struct {
  GnNostrReferenceType type;
  gchar *uri;              /* canonical nostr: URI */
  gchar *id;               /* hex event id, or address identifier */
  gchar *author;           /* hex pubkey, possibly only a hint */
  gint kind;               /* -1 when absent */
  gchar **relay_hints;     /* NULL-terminated, never contacted here */
  gboolean author_authenticated;
} GnNostrReference;

/* Decode NIP-21/NIP-19 without network lookup. nsec is deliberately rejected. */
GnNostrReference *gn_nostr_reference_parse(const gchar *uri);
void gn_nostr_reference_free(GnNostrReference *reference);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrReference, gn_nostr_reference_free)

typedef struct {
  GnNostrReference *target;
  gchar *original_json;    /* only when caller verified the embedded event */
  gboolean quote;
  gint source_kind;        /* 6, 16 or 1 */
} GnNostrRepostDescriptor;

/* Parses kind 6/16 reposts and kind-1 q tags. Invalid/ambiguous targets fail. */
GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse(const gchar *event_json,
                                                           gboolean embedded_event_verified);
GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse_tags(gint kind,
                                                                const gchar *tags_json);
void gn_nostr_repost_descriptor_free(GnNostrRepostDescriptor *descriptor);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrRepostDescriptor, gn_nostr_repost_descriptor_free)

/* Unsigned event templates: caller supplies pubkey, timestamp, signing and relays. */
gchar *gn_nostr_build_repost_template(const GnNostrReference *target,
                                      const gchar *verified_original_json);
gchar *gn_nostr_build_quote_template(const GnNostrReference *target,
                                     const gchar *comment);

G_END_DECLS
#endif
