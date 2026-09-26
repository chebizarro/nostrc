/**
 * gnostr-signer-availability — is there a signer GNostr can use? (nostrc-e5nz)
 *
 * GNostr holds no private keys: without org.nostr.Signer on the session bus
 * (and without a NIP-46 remote signer) it can read but not sign. This module
 * answers, off the main thread:
 *   - is org.nostr.Signer running, installed (D-Bus activatable) or absent;
 *   - are keys an older GNostr stored itself still in the key store, waiting
 *     to be imported into the signer;
 * starts the signer through D-Bus activation on request, and turns that
 * status into user-facing copy. The copy functions are pure, so the UX
 * decisions are unit-tested (tests/test_signer_availability.c).
 *
 * There is no local-key fallback: when no signer is available the UI says
 * so and offers to start or install one.
 */
#ifndef GNOSTR_SIGNER_AVAILABILITY_H
#define GNOSTR_SIGNER_AVAILABILITY_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_SIGNER_BUS_NAME "org.nostr.Signer"

typedef enum {
  GNOSTR_SIGNER_PRESENCE_NO_BUS,        /* no session bus: nothing can be asked */
  GNOSTR_SIGNER_PRESENCE_NOT_INSTALLED, /* not running, no D-Bus activation file */
  GNOSTR_SIGNER_PRESENCE_ACTIVATABLE,   /* not running; D-Bus can start it */
  GNOSTR_SIGNER_PRESENCE_RUNNING,       /* org.nostr.Signer has an owner */
} GnostrSignerPresence;

typedef struct {
  GnostrSignerPresence presence;
  /* Keys a pre-e5nz GNostr stored in its own keystore that are still there. */
  guint legacy_keys;
  /* The signer daemon imports those by itself when it starts (Linux). */
  gboolean legacy_auto_migrates;
} GnostrSignerStatus;

/* Query presence + legacy keys in a worker thread. */
void gnostr_signer_status_query_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);
gboolean gnostr_signer_status_query_finish(GAsyncResult *result,
                                           GnostrSignerStatus *out,
                                           GError **error);

/* D-Bus-activate org.nostr.Signer (StartServiceByName). Succeeds when the
 * signer is started or was already running. */
void gnostr_signer_start_async(GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean gnostr_signer_start_finish(GAsyncResult *result, GError **error);

/* ---- Copy (pure; transfer full; NULL = nothing to show) ---- */

/* TRUE when a "Start GNostr Signer" action makes sense. */
gboolean gnostr_signer_status_can_start(const GnostrSignerStatus *status);

/* Local-signer line on the sign-in page when the signer is not running.
 * NULL when it is running (the caller then asks the signer for its key). */
char *gnostr_signer_status_login_text(const GnostrSignerStatus *status);

/* Notice about keys an older GNostr stored itself; NULL if there are none. */
char *gnostr_signer_status_legacy_text(const GnostrSignerStatus *status);

/* How the current session depends on org.nostr.Signer. */
typedef enum {
  GNOSTR_SIGNER_NEED_NONE,        /* NIP-46 session, or no saved account */
  GNOSTR_SIGNER_NEED_SIGNED_OUT,  /* saved account that signed in through the
                                   * local signer, not signed in right now */
  GNOSTR_SIGNER_NEED_ACTIVE,      /* signed in; signs through org.nostr.Signer */
} GnostrSignerNeed;

/* Main-window banner. NULL when no banner is needed: the signer is running,
 * or the session neither needs it nor has legacy keys waiting for it. */
char *gnostr_signer_status_banner_text(const GnostrSignerStatus *status,
                                       GnostrSignerNeed need);

G_END_DECLS

#endif /* GNOSTR_SIGNER_AVAILABILITY_H */
