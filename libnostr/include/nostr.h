#ifndef NOSTR_H
#define NOSTR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-config.h"
#include "nostr-core.h"

#if defined(NOSTR_HAVE_GLIB) && NOSTR_HAVE_GLIB
#include "nostr-glib.h"
#endif

#ifdef __cplusplus
}
#endif

#endif // NOSTR_H
