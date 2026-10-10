/*
 * gnostr-nostr-target — what does a nostr: link or a handed-off event open
 * in GNostr? (nostrc-prqu.3)
 *
 * nostr-dispatcher (gnome/nostr-dispatcher) launches GNostr with a
 * canonical NIP-21 URI (`gnostr nostr:nevent1…`) or, while GNostr is
 * running, hands it the already-validated event over org.nostr.Handler1.
 * This module is the GTK-free half of both paths:
 *
 *   - gnostr_nostr_target_parse() normalises the dispatcher URI (web+nostr:,
 *     query, fragment) and decodes it with nostr-gtk's shared
 *     gn_nostr_reference_parse_full(), refusing anything GNostr must never
 *     act on (nsec, ncryptsec, nrelay, other schemes);
 *   - gnostr_nostr_view_for_kind() is the single kind -> view table. The
 *     kinds it maps to a purpose-built view are exactly the numeric entries
 *     of X-Nostr-Kinds= in the desktop file (a unit test keeps the two in
 *     sync). Every other kind reaches GNostr through the `*` fallback and
 *     is shown as a generic event in the thread view;
 *   - gnostr_nostr_event_parse() validates event JSON (NIP-01 canonical id
 *     and Schnorr signature, via gn_nostr_event_parse()) before anything is
 *     shown, and
 *     gnostr_nostr_event_matches_target() checks that a fetched event is
 *     what the link points at.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GNOSTR_NOSTR_TARGET_H
#define GNOSTR_NOSTR_TARGET_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GNOSTR_NOSTR_VIEW_THREAD = 0, /* thread view; also the generic `*` viewer */
  GNOSTR_NOSTR_VIEW_PROFILE,    /* profile pane (kind 0, npub, nprofile) */
  GNOSTR_NOSTR_VIEW_ARTICLE,    /* NIP-23 long-form reader (kind 30023) */
  GNOSTR_NOSTR_VIEW_MESSAGES,   /* DM inbox (kinds 14, 1059): never render ciphertext */
} GnostrNostrView;

typedef enum {
  GNOSTR_NOSTR_TARGET_PROFILE,  /* npub / nprofile */
  GNOSTR_NOSTR_TARGET_EVENT,    /* note / nevent: event id known */
  GNOSTR_NOSTR_TARGET_ADDRESS,  /* naddr: kind + author + d, id unknown */
} GnostrNostrTargetType;

typedef struct {
  GnostrNostrTargetType type;
  char *pubkey_hex;    /* PROFILE: the profile; ADDRESS: author; EVENT: author TLV or NULL */
  char *event_id_hex;  /* EVENT only */
  int kind;            /* PROFILE: 0; ADDRESS: >= 0; EVENT: kind TLV or -1 */
  char *d_tag;         /* ADDRESS only (may be "") */
  char **relays;       /* NULL-terminated relay hints (never NULL) */
} GnostrNostrTarget;

#define GNOSTR_NOSTR_TARGET_ERROR (gnostr_nostr_target_error_quark())
GQuark gnostr_nostr_target_error_quark(void);

typedef enum {
  GNOSTR_NOSTR_TARGET_ERROR_INVALID,     /* not a decodable nostr: URI / event */
  GNOSTR_NOSTR_TARGET_ERROR_REFUSED,     /* nsec / ncryptsec: never acted on */
  GNOSTR_NOSTR_TARGET_ERROR_UNSUPPORTED, /* nrelay and other valid-but-unhandled entities */
} GnostrNostrTargetError;

/* Parse "nostr:<bech32>" or "web+nostr:<bech32>" (scheme case-insensitive;
 * query strings and fragments are ignored). */
GnostrNostrTarget *gnostr_nostr_target_parse(const char *uri, GError **error);
void gnostr_nostr_target_free(GnostrNostrTarget *target);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnostrNostrTarget, gnostr_nostr_target_free)

/* The view that renders @kind. Unknown kinds (and -1) -> THREAD. */
GnostrNostrView gnostr_nostr_view_for_kind(int kind);

/* Kinds with a purpose-built view, ascending. Must equal the numeric
 * entries of X-Nostr-Kinds= in data/org.gnostr.gnostr.desktop. */
const int *gnostr_nostr_declared_kinds(gsize *n_kinds);

/* A validated event (id + signature verified). */
typedef struct {
  char *id_hex;
  char *pubkey_hex;
  int kind;
  gint64 created_at;
  char *d_tag;   /* first "d" tag value, or NULL */
} GnostrNostrEventInfo;

/* Strictly parse and validate signed event JSON. NULL (and @error) when it
 * is malformed or its id/signature do not verify. */
GnostrNostrEventInfo *gnostr_nostr_event_parse(const char *event_json, GError **error);
void gnostr_nostr_event_info_free(GnostrNostrEventInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnostrNostrEventInfo, gnostr_nostr_event_info_free)

/* TRUE when @info is what @target points at: same id (EVENT; and same kind
 * and author when the link carries them), same kind + author + d (ADDRESS),
 * the author's kind 0 (PROFILE). */
gboolean gnostr_nostr_event_matches_target(const GnostrNostrEventInfo *info,
                                           const GnostrNostrTarget *target);

/* The newest of @event_jsons that validates and matches @target (ties:
 * lowest id, as for NIP-01 replaceable events). Returns a copy or NULL. */
char *gnostr_nostr_pick_event_for_target(const GnostrNostrTarget *target,
                                         const char *const *event_jsons,
                                         gsize n_events);

G_END_DECLS

#endif /* GNOSTR_NOSTR_TARGET_H */
