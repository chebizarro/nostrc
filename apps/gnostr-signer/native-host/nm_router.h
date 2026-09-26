/* nm_router.h - request router for the NIP-07 bridge (nostrc-jjyp)
 *
 * The router owns one browser connection: it parses each request frame,
 * enforces the per-connection limits (id shape, duplicate ids, in-flight
 * cap), resolves the page origin into the signer app_id and hands the
 * request to the provider that owns the method. Providers complete
 * requests asynchronously; every reply is serialized on the router's main
 * context and delivered through the NmReplyFunc, so replies to concurrent
 * requests may arrive out of order and are correlated by "id".
 *
 * Providers are a small static vtable so a new API surface drops in as a
 * single file: nm_provider_nip07.c (window.nostr -> org.nostr.Signer) and
 * nm_provider_webln.c (window.webln -> org.nostr.Wallet1; currently a
 * documented stub pending nostrc-yka8).
 */
#ifndef APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ROUTER_H
#define APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ROUTER_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nm_errors.h"

G_BEGIN_DECLS

#define NM_PROTOCOL_VERSION 1
#define NM_HOST_NAME        "org.nostr.signer_bridge"
#define NM_MAX_ID_LEN       64
#define NM_MAX_IN_FLIGHT    16

typedef struct _NmRouter NmRouter;

/* One parsed request. Owned by the router; providers must finish it with
 * exactly one nm_request_reply_*() call. */
typedef struct {
  NmRouter   *router;
  gchar      *id;       /* correlation id from the extension */
  gchar      *method;   /* e.g. "signEvent", "nip44.decrypt" */
  gchar      *app_id;   /* canonical origin (NULL for host.* methods) */
  JsonObject *params;   /* never NULL (empty object when absent) */
  JsonNode   *root;     /* keeps params alive */
} NmRequest;

typedef struct {
  const gchar *name;             /* "nip07", "webln" */
  const gchar *const *methods;   /* NULL-terminated list it answers */
  gboolean requires_origin;      /* request must carry a valid origin */
  void (*dispatch)(NmRequest *req);
} NmProvider;

extern const NmProvider nm_provider_nip07;
extern const NmProvider nm_provider_webln;

typedef void (*NmReplyFunc)(const gchar *json, gsize len, gpointer user_data);

typedef struct {
  const gchar *identity;          /* forwarded to the signer; "" = active identity */
  gint         call_timeout_ms;   /* non-interactive D-Bus calls */
  gint         approval_timeout_ms; /* calls that may raise an approval dialog */
  const gchar *signer_bus_name;   /* default "org.nostr.Signer" */
} NmRouterConfig;

/* @bus may be NULL: the router then connects to the session bus lazily on
 * the first request that needs it (so a host started without a session bus
 * still answers host.hello and reports signer_unavailable per request). */
NmRouter *nm_router_new(GDBusConnection *bus, const NmRouterConfig *config,
                        NmReplyFunc reply, gpointer reply_data);
void      nm_router_free(NmRouter *router);

/* Handle one request frame (not NUL-terminated necessarily). */
void      nm_router_handle(NmRouter *router, const gchar *json, gsize len);

/* Report a framing-level failure (oversized / empty frame). */
void      nm_router_reply_frame_error(NmRouter *router, NmErrorCode code);

guint     nm_router_in_flight(NmRouter *router);

/* ---- provider-facing API ------------------------------------------------ */

/* Session bus connection, or NULL when unavailable. */
GDBusConnection *nm_router_get_bus(NmRouter *router);
const NmRouterConfig *nm_router_get_config(NmRouter *router);
GCancellable    *nm_router_get_cancellable(NmRouter *router);

/* Finish @req. Each takes ownership of nothing and frees @req. */
void nm_request_reply_result(NmRequest *req, JsonNode *result /* transfer full */);
void nm_request_reply_string(NmRequest *req, const gchar *result);
void nm_request_reply_error(NmRequest *req, NmErrorCode code, const gchar *message /* nullable */);

/* Params helpers: string member or NULL (wrong type counts as absent). */
const gchar *nm_request_param_string(NmRequest *req, const gchar *name);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ROUTER_H */
