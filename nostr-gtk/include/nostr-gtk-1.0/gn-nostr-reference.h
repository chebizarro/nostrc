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

#define GN_NOSTR_REFERENCE_ERROR (gn_nostr_reference_error_quark())
GQuark gn_nostr_reference_error_quark(void);

typedef enum {
  GN_NOSTR_REFERENCE_ERROR_INVALID,     /* not a decodable NIP-21 reference */
  GN_NOSTR_REFERENCE_ERROR_REFUSED,     /* nsec / ncryptsec: never decoded */
  GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED, /* nrelay and other valid-but-unhandled entities */
} GnNostrReferenceError;

/* Decode NIP-21/NIP-19 without network lookup. nsec is deliberately rejected.
 * The "nostr:" prefix is optional and case-insensitive; a single-case bech32
 * entity is accepted in either case. Query strings, fragments, paths and
 * percent-encoding are rejected: normalise those at the app boundary. */
GnNostrReference *gn_nostr_reference_parse_full(const gchar *uri, GError **error);
/* Same as gn_nostr_reference_parse_full(uri, NULL). */
GnNostrReference *gn_nostr_reference_parse(const gchar *uri);
void gn_nostr_reference_free(GnNostrReference *reference);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrReference, gn_nostr_reference_free)

/* Canonical "nostr:" URI builders (ported from gnostr nip21_uri). Relay hints
 * are only encoded, never contacted. NULL on invalid input. */
/* npub when @relays is NULL/empty, else nprofile. */
gchar *gn_nostr_reference_build_person(const gchar *pubkey_hex,
                                       const gchar *const *relays);
/* note when only @id_hex is given, else nevent (author/kind<=0/relays optional). */
gchar *gn_nostr_reference_build_event(const gchar *id_hex,
                                      const gchar *author_hex,
                                      gint kind,
                                      const gchar *const *relays);
/* naddr; @identifier may be "" (NIP-01 replaceable events). */
gchar *gn_nostr_reference_build_address(const gchar *author_hex,
                                        gint kind,
                                        const gchar *identifier,
                                        const gchar *const *relays);

/* A signed event whose NIP-01 canonical id and Schnorr signature verified
 * (ported from gnostr-nostr-target). Hex values are lowercase. */
typedef struct {
  gchar *id;
  gchar *pubkey;
  gint kind;
  gint64 created_at;
  gchar *d_tag;            /* first "d" tag value, or NULL */
  gchar *content;          /* event content, never NULL */
} GnNostrEventInfo;

/* Bounded (256 KiB) strict parse + verification; NULL and
 * GN_NOSTR_REFERENCE_ERROR_INVALID when malformed or not verifying. */
GnNostrEventInfo *gn_nostr_event_parse(const gchar *event_json, GError **error);
void gn_nostr_event_info_free(GnNostrEventInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrEventInfo, gn_nostr_event_info_free)
gboolean gn_nostr_event_verify(const gchar *event_json, GError **error);
/* Successful verifications are memoised (bounded, by SHA-256 of the JSON), so
 * re-rendering the same event does not repeat the Schnorr check. Drops it. */
void gn_nostr_event_verify_cache_clear(void);

/* TRUE when @info is what @reference points at: same id (EVENT; and kind and
 * author when the reference carries them), same kind + author + identifier
 * (ADDRESS), the author's kind 0 (PERSON). */
gboolean gn_nostr_reference_matches_event_info(const GnNostrReference *reference,
                                               const GnNostrEventInfo *info);
/* Verifies @event_json, then gn_nostr_reference_matches_event_info(). */
gboolean gn_nostr_reference_matches_event(const GnNostrReference *reference,
                                          const gchar *event_json);

typedef struct {
  GnNostrReference *target;
  gchar *original_json;    /* only when the embedded event verified (by the
                            * caller, or here via gn_nostr_event_parse) */
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
