#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>

/* Marked e-tags */
typedef enum { NOSTR_E_MARK_NONE=0, NOSTR_E_MARK_ROOT, NOSTR_E_MARK_REPLY } NostrEMarker;

int nostr_nip10_add_marked_e_tag(NostrEvent *ev,
                                 const unsigned char event_id[32],
                                 const char *relay_opt,
                                 NostrEMarker marker,
                                 const unsigned char *author_pk_opt);

typedef struct {
  bool has_root, has_reply;
  unsigned char root_id[32];
  unsigned char reply_id[32];
} NostrThreadContext;

int  nostr_nip10_get_thread(const NostrEvent *ev, NostrThreadContext *out);
int  nostr_nip10_ensure_p_participants(NostrEvent *reply_ev, const NostrEvent *parent_ev);

#ifdef __cplusplus
}
#endif
