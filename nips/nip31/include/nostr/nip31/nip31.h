#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"

int  nostr_nip31_set_alt(NostrEvent *ev, const char *alt);
int  nostr_nip31_get_alt(const NostrEvent *ev, char **out_alt); /* malloc'd */

#ifdef __cplusplus
}
#endif
