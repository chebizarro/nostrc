/*
 * nd-event — event JSON admission for the dispatcher.
 *
 * `validated` means libnostr's nostr_event_validate() accepted the event:
 * all signed fields present, declared id == canonical NIP-01 hash, and the
 * Schnorr signature verifies. Only validated events are ever handed to a
 * handler via org.nostr.Handler1.OpenEvent (the Handler1 contract).
 *
 * Unvalidated events (e.g. an unsigned draft opened from a local .nostr
 * file) still yield a kind for routing, but are launched as file/URI only.
 */
#ifndef ND_EVENT_H
#define ND_EVENT_H

#include <glib.h>
#include "nd-uri.h"

G_BEGIN_DECLS

#define ND_EVENT_MAX_JSON (1024 * 1024)

typedef struct {
  char *json;        /* canonical compact JSON (validated) or input JSON */
  gint kind;
  char *id_hex;      /* NULL if absent */
  char *pubkey_hex;  /* NULL if absent */
  char *d_tag;       /* first "d" tag value, NULL if none */
  gboolean validated;
} NdEvent;

/* Parse @json. Fails (ND_ERROR_INVALID_EVENT) only when no integer kind in
 * 0..65535 can be read; signature problems just leave validated = FALSE. */
NdEvent *nd_event_parse(const char *json, gssize len, GError **error);
void nd_event_free(NdEvent *ev);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdEvent, nd_event_free)

/* TRUE if a validated @ev is what @t points at: same id for events; same
 * kind + pubkey + d-tag for addresses; kind 0 + pubkey for profiles. */
gboolean nd_event_matches_target(const NdEvent *ev, const NdTarget *t);

G_END_DECLS

#endif /* ND_EVENT_H */
