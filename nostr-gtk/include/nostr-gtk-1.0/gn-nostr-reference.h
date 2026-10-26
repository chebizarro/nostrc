#ifndef GN_NOSTR_REFERENCE_H
#define GN_NOSTR_REFERENCE_H

#include "nostr-gtk-portable-export.h"
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
NOSTR_GTK_PORTABLE_API GQuark gn_nostr_reference_error_quark(void);

typedef enum {
  GN_NOSTR_REFERENCE_ERROR_INVALID,     /* not a decodable NIP-21 reference */
  GN_NOSTR_REFERENCE_ERROR_REFUSED,     /* nsec / ncryptsec: never decoded */
  GN_NOSTR_REFERENCE_ERROR_UNSUPPORTED, /* nrelay and other valid-but-unhandled entities */
} GnNostrReferenceError;

/* Decode NIP-21/NIP-19 without network lookup. nsec is deliberately rejected.
 * The "nostr:" prefix is optional and case-insensitive; a single-case bech32
 * entity is accepted in either case. Query strings, fragments, paths and
 * percent-encoding are rejected: normalise those at the app boundary. */
NOSTR_GTK_PORTABLE_API GnNostrReference *gn_nostr_reference_parse_full(const gchar *uri, GError **error);
/* Same as gn_nostr_reference_parse_full(uri, NULL). */
NOSTR_GTK_PORTABLE_API GnNostrReference *gn_nostr_reference_parse(const gchar *uri);
NOSTR_GTK_PORTABLE_API void gn_nostr_reference_free(GnNostrReference *reference);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrReference, gn_nostr_reference_free)

/* Canonical "nostr:" URI builders (ported from gnostr nip21_uri). Relay hints
 * are only encoded, never contacted. NULL on invalid input. */
/* npub when @relays is NULL/empty, else nprofile. */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_reference_build_person(const gchar *pubkey_hex,
                                       const gchar *const *relays);
/* note when only @id_hex is given, else nevent (author/kind<=0/relays optional). */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_reference_build_event(const gchar *id_hex,
                                      const gchar *author_hex,
                                      gint kind,
                                      const gchar *const *relays);
/* naddr; @identifier may be "" (NIP-01 replaceable events). */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_reference_build_address(const gchar *author_hex,
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
NOSTR_GTK_PORTABLE_API GnNostrEventInfo *gn_nostr_event_parse(const gchar *event_json, GError **error);
NOSTR_GTK_PORTABLE_API void gn_nostr_event_info_free(GnNostrEventInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrEventInfo, gn_nostr_event_info_free)
NOSTR_GTK_PORTABLE_API gboolean gn_nostr_event_verify(const gchar *event_json, GError **error);
/* Successful verifications are memoised (bounded, by SHA-256 of the JSON), so
 * re-rendering the same event does not repeat the Schnorr check. Drops it. */
NOSTR_GTK_PORTABLE_API void gn_nostr_event_verify_cache_clear(void);

/* TRUE when @info is what @reference points at: same id (EVENT; and kind and
 * author when the reference carries them), same kind + author + identifier
 * (ADDRESS), the author's kind 0 (PERSON). */
NOSTR_GTK_PORTABLE_API gboolean gn_nostr_reference_matches_event_info(const GnNostrReference *reference,
                                               const GnNostrEventInfo *info);
/* Verifies @event_json, then gn_nostr_reference_matches_event_info(). */
NOSTR_GTK_PORTABLE_API gboolean gn_nostr_reference_matches_event(const GnNostrReference *reference,
                                          const gchar *event_json);

typedef struct {
  GnNostrReference *target;
  gchar *original_json;    /* only when the embedded event verified (by the
                            * caller, or here via gn_nostr_event_parse) */
  gboolean quote;
  gint source_kind;        /* 6, 16 or 1 */
} GnNostrRepostDescriptor;

/* Parses kind 6/16 reposts and kind-1 q tags. Invalid/ambiguous targets fail. */
NOSTR_GTK_PORTABLE_API GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse(const gchar *event_json,
                                                           gboolean embedded_event_verified);
NOSTR_GTK_PORTABLE_API GnNostrRepostDescriptor *gn_nostr_repost_descriptor_parse_tags(gint kind,
                                                                const gchar *tags_json);
NOSTR_GTK_PORTABLE_API void gn_nostr_repost_descriptor_free(GnNostrRepostDescriptor *descriptor);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrRepostDescriptor, gn_nostr_repost_descriptor_free)

/* A NIP-18 repost (kind 6/16) or quote (kind 1 with a "q" tag) shared as a
 * chat message has an event's JSON as its body, which is never text for
 * people (nostrc-tv98u, nostrc-yarxx). The one parse every surface uses:
 *
 * gn_nostr_shared_note_parse(): the descriptor when @body (at most 64 KiB)
 * is one, else NULL; *quote_content (nullable out) is then a quote's own
 * text as written (valid UTF-8, NULL for a repost or an empty quote). */
NOSTR_GTK_PORTABLE_API GnNostrRepostDescriptor *gn_nostr_shared_note_parse(const gchar *body, gchar **quote_content);
/* A quote's text as shown: @quote_content without a trailing nostr:
 * reference to @target (NIP-18 puts one there; the card shows it). */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_shared_note_shown_text(const gchar *quote_content,
                                       const GnNostrReference *target);
/* What to copy: the quote's text with a reference to the note (its own, or
 * @target's URI appended); only the URI for a repost. */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_shared_note_copy_text(const gchar *quote_content,
                                      const GnNostrReference *target);
/* TRUE when @body is a shared note; *shown_text (nullable out) is then the
 * quote's shown text (NULL for a repost) and *uri (nullable out) the shared
 * note's nostr: URI. */
NOSTR_GTK_PORTABLE_API gboolean gn_nostr_shared_note_dup_text(const gchar *body, gchar **shown_text, gchar **uri);
/* @body as text to copy: as it is, or gn_nostr_shared_note_copy_text(). */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_shared_note_dup_copy_text(const gchar *body);

/* Unsigned event templates: caller supplies pubkey, timestamp, signing and relays. */
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_build_repost_template(const GnNostrReference *target,
                                      const gchar *verified_original_json);
NOSTR_GTK_PORTABLE_API gchar *gn_nostr_build_quote_template(const GnNostrReference *target,
                                     const gchar *comment);

G_END_DECLS
#endif
