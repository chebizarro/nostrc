/*
 * nd-uri — NIP-21 `nostr:` URI parsing and canonical re-encoding.
 *
 * Accepted input:
 *   nostr:<bech32>, web+nostr:<bech32> (scheme case-insensitive; a stray
 *   `//` after the scheme is tolerated; anything after `?`, `#` or `/` is
 *   dropped) and a bare NIP-19 bech32 string (CLI convenience). The notify
 *   daemon's legacy `nostr://open?event=<hex64>` form is no longer accepted
 *   (nostrc-prqu.7; notify emits NIP-21 nevent links since nostrc-1v65).
 *
 * Entity mapping:
 *   note1     -> event, kind unknown
 *   nevent1   -> event, kind from TLV type 3 when present (optional)
 *   naddr1    -> addressable event, kind from TLV (required)
 *   npub1 / nprofile1 -> profile, kind 0
 *   nsec1 / ncryptsec1 -> ND_ERROR_FORBIDDEN (NIP-21 forbids nsec; the raw
 *                         input is never echoed in any message)
 *   nrelay1 / anything else -> ND_ERROR_INVALID_URI
 *
 * Relay hints are untrusted input: only wss:// URLs (and ws:// to a
 * loopback host) survive, deduplicated, capped at ND_URI_MAX_RELAYS. The
 * same filter applies before fetching AND before re-encoding the URI that
 * is handed to a handler app.
 */
#ifndef ND_URI_H
#define ND_URI_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_URI_MAX_RELAYS 3
#define ND_URI_MAX_LEN 4096

typedef enum {
  ND_ENTITY_EVENT = 1,   /* note / nevent */
  ND_ENTITY_ADDRESS,     /* naddr */
  ND_ENTITY_PROFILE,     /* npub / nprofile */
} NdEntity;

typedef struct {
  NdEntity entity;
  char *id_hex;       /* ND_ENTITY_EVENT */
  char *pubkey_hex;   /* PROFILE / ADDRESS; optional nevent author */
  char *identifier;   /* ADDRESS d-tag ("" allowed) */
  gint kind;          /* -1 = unknown */
  char **relays;      /* NULL-terminated, filtered */
} NdTarget;

NdTarget *nd_target_parse_uri(const char *uri, GError **error);
NdTarget *nd_target_copy(const NdTarget *t);
void nd_target_free(NdTarget *t);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdTarget, nd_target_free)

/* Canonical `nostr:<bech32>` for @t (note/nevent/naddr/npub/nprofile).
 * Returns NULL on encode failure. */
char *nd_target_to_uri(const NdTarget *t);

/* Kind TLV (type 3) of a nevent/naddr bech32 string, distinguishing
 * "absent" (returns FALSE) from kind 0. */
gboolean nd_bech32_tlv_kind(const char *bech, guint32 *out_kind);

/* Relay-hint admission filter (see header comment). */
gboolean nd_relay_url_acceptable(const char *url);

/* Filter + dedupe + cap an arbitrary relay list. Returns a new strv. */
char **nd_relays_filter(const char *const *relays, gsize n);

G_END_DECLS

#endif /* ND_URI_H */
