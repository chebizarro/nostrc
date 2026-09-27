#ifndef GH_RELAY_SCOPE_H
#define GH_RELAY_SCOPE_H

#include <glib.h>
#include <nostr-filter.h>

G_BEGIN_DECLS

typedef struct _GhRelayScope GhRelayScope;

typedef enum {
  GH_RELAY_NOTICE_EVENT,
  GH_RELAY_NOTICE_EOSE,
  GH_RELAY_NOTICE_CLOSED,
  GH_RELAY_NOTICE_AUTH,
  GH_RELAY_NOTICE_OK,
  GH_RELAY_NOTICE_DISCONNECTED,
  GH_RELAY_NOTICE_ERROR
} GhRelayNotice;

typedef struct {
  GhRelayNotice notice;
  const gchar *url;
  const gchar *event_json; /* EVENT only; valid only during callback */
  const gchar *event_id;   /* EVENT or OK; valid only during callback */
  const gchar *detail;     /* CLOSED, AUTH, OK or ERROR */
  gboolean accepted;       /* relay-local OK, never upstream delivery */
  gboolean backfill;       /* EVENT received before this URL's EOSE */
} GhRelayUpdate;

typedef void (*GhRelayScopeFunc)(GhRelayScope *scope,
                                 const GhRelayUpdate *update,
                                 gpointer user_data);

/* Internal transport seam. open must issue a REQ only to url; close must stop
 * callbacks before returning. A scope never hands it URLs outside its set. */
typedef struct {
  gpointer (*open)(GhRelayScope *scope, const gchar *url,
                   const NostrFilters *filters, gpointer user_data,
                   GError **error);
  void (*close)(gpointer handle, gpointer user_data);
} GhRelayTransport;

GhRelayScope *gh_relay_scope_new(guint64 account_generation,
                                 NostrFilters *filters,
                                 GhRelayScopeFunc callback,
                                 gpointer user_data);
GhRelayScope *gh_relay_scope_new_with_transport(guint64 account_generation,
                                                NostrFilters *filters,
                                                const GhRelayTransport *transport,
                                                gpointer transport_data,
                                                GhRelayScopeFunc callback,
                                                gpointer user_data);
GhRelayScope *gh_relay_scope_ref(GhRelayScope *scope);
void gh_relay_scope_unref(GhRelayScope *scope);
/* At most 16 distinct ws(s) URLs. A URL added after start receives its own
 * live REQ and independent EOSE boundary. */
gboolean gh_relay_scope_add_url(GhRelayScope *scope, const gchar *url,
                                GError **error);
void gh_relay_scope_start(GhRelayScope *scope);
/* Revoke generation before closing transports; no later callback is admitted. */
void gh_relay_scope_cancel(GhRelayScope *scope);
guint64 gh_relay_scope_get_generation(const GhRelayScope *scope);

/* Transport-to-scope delivery. Unknown URLs and cancelled generations are
 * discarded. EVENT validates signed NIP-01 JSON and deduplicates 4096 IDs. */
void gh_relay_scope_event(GhRelayScope *scope, const gchar *url,
                          const gchar *event_json);
void gh_relay_scope_eose(GhRelayScope *scope, const gchar *url);
void gh_relay_scope_notice(GhRelayScope *scope, const gchar *url,
                           GhRelayNotice notice, const gchar *event_id,
                           gboolean accepted, const gchar *detail);

G_END_DECLS
#endif
