/* nd-error — error domain shared by the dispatcher modules. The D-Bus
 * mapping (org.nostr.Dispatcher1.Error.*) lives in nostr-dispatcher.c. */
#ifndef ND_ERROR_H
#define ND_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_ERROR (nd_error_quark())
GQuark nd_error_quark(void);

typedef enum {
  ND_ERROR_INVALID_URI,   /* not a NIP-21 URI we understand */
  ND_ERROR_FORBIDDEN,     /* nsec / ncryptsec: never handled, never echoed */
  ND_ERROR_NOT_FOUND,     /* event could not be fetched */
  ND_ERROR_NO_HANDLER,    /* no installed application handles the kind */
  ND_ERROR_INVALID_EVENT, /* malformed / oversized event JSON */
  ND_ERROR_LAUNCH_FAILED, /* GAppInfo launch failed */
} NdError;

G_END_DECLS

#endif /* ND_ERROR_H */
