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
/* nostrc-jppi: Grotto's window, the only approval UI nip55l 0.4.0
 * accepts. The daemon (org.nostr.Signer) can run without it; a request that
 * needs a prompt then fails at once with Error.ApprovalDenied. */
#define GNOSTR_SIGNER_APPROVER_BUS_NAME "org.nostr.Grotto"

/* nostrc-jppi: what the last approval-gated NIP-55L answers say about
 * GNostr's standing with Grotto (kept by GnostrSignerService). */
typedef enum {
  GNOSTR_SIGNER_APPROVAL_OK,          /* nothing to report */
  GNOSTR_SIGNER_APPROVAL_NO_APPROVER, /* a request needed a prompt and no
                                       * Grotto window was open */
  GNOSTR_SIGNER_APPROVAL_REFUSED,     /* a saved "deny" rule refused GNostr */
} GnostrSignerApproval;

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
  /* The signer daemon imports those by itself when it starts. */
  gboolean legacy_auto_migrates;
  /* nostrc-jppi: Grotto's window (org.nostr.Grotto) is open, so
   * approval prompts can be shown. */
  gboolean approver_running;
  /* nostrc-jppi: identities in the signer's key store, read from its
   * attributes (no D-Bus call, so no approval prompt); -1 = unknown. */
  gint signer_keys;
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

/* nostrc-jppi: open Grotto's window (the approval UI). The desktop
 * entry is DBusActivatable, which only works where its D-Bus service file is
 * installed, so the grotto program on PATH is started instead when
 * there is one; a running instance just presents its window. */
gboolean gnostr_signer_open_app(GError **error);

/* ---- Copy (pure; transfer full; NULL = nothing to show) ---- */

/* TRUE when a "Start Grotto" action makes sense. */
gboolean gnostr_signer_status_can_start(const GnostrSignerStatus *status);

/* Local-signer line on the sign-in page. NULL when the signer is running,
 * holds a key (or its key store cannot be read) and its window is open:
 * "ready". Never asks the signer anything: under nip55l 0.4.0 even
 * GetPublicKey is approval-gated, so merely opening the sign-in page must
 * not raise a prompt (nostrc-jppi). */
char *gnostr_signer_status_login_text(const GnostrSignerStatus *status);

/* nostrc-jppi: TRUE when "Open Grotto" is the useful action: the
 * daemon runs but its window, which shows approval prompts, does not. */
gboolean gnostr_signer_status_can_open(const GnostrSignerStatus *status);

/* Notice about keys an older GNostr stored itself; NULL if there are none. */
char *gnostr_signer_status_legacy_text(const GnostrSignerStatus *status);

/* How the current session depends on org.nostr.Signer. */
typedef enum {
  GNOSTR_SIGNER_NEED_NONE,        /* NIP-46 session, or no saved account */
  GNOSTR_SIGNER_NEED_SIGNED_OUT,  /* saved account that signed in through the
                                   * local signer, not signed in right now */
  GNOSTR_SIGNER_NEED_ACTIVE,      /* signed in; signs through org.nostr.Signer */
} GnostrSignerNeed;

/* nostrc-lwzv: TRUE when the session signs through org.nostr.Signer and the
 * signer is not running: GNostr can read but must not offer to publish. */
gboolean gnostr_signer_status_is_read_only(const GnostrSignerStatus *status,
                                           GnostrSignerNeed need);

/* Main-window banner. NULL when no banner is needed: the signer is running,
 * or the session neither needs it nor has legacy keys waiting for it. */
char *gnostr_signer_status_banner_text(const GnostrSignerStatus *status,
                                       GnostrSignerNeed need);

/* nostrc-jppi: main-window banner for an approval problem while the signer
 * daemon runs. NULL when there is none, when the daemon is not running (the
 * status banner covers that), or for NO_APPROVER once Grotto's
 * window is open. */
char *gnostr_signer_status_approval_text(const GnostrSignerStatus *status,
                                         GnostrSignerApproval approval);

G_END_DECLS

#endif /* GNOSTR_SIGNER_AVAILABILITY_H */
