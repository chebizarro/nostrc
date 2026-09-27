/* np-fake-session-relay.h - Test double for org.nostr.SessionRelay1
 *
 * SPDX-License-Identifier: MIT
 *
 * Serves the upstream-federation part of org.nostr.SessionRelay1 from a
 * thread with its own main context, on whatever session bus
 * DBUS_SESSION_BUS_ADDRESS names when it starts (tests bring up a private
 * one with GTestDBus: never the developer's or the lab's real bus). Used
 * by the libnostr-publish, nostr-share and nostr-dav tests (nostrc-t24q).
 */
#ifndef NP_FAKE_SESSION_RELAY_H
#define NP_FAKE_SESSION_RELAY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NpFakeSessionRelay NpFakeSessionRelay;

/* TRUE when a private bus can be started (dbus-daemon is installed);
 * otherwise the caller skips its D-Bus cases with a reason. */
gboolean np_fake_session_relay_bus_available(void);

/* Owns org.nostr.SessionRelay1 and returns once the name is acquired.
 * @federation_state: FederationState value, or NULL for a daemon that
 * predates the property (Get answers UnknownProperty). */
NpFakeSessionRelay *np_fake_session_relay_start(const gchar *federation_state);

/* Releases the name, closes the connection and joins the thread. */
void np_fake_session_relay_stop(NpFakeSessionRelay *fake);

/* GetEventUpstream reply for every id: @state, @detail and targets given
 * as "url relay_state" or "url relay_state reason…" strings. */
void np_fake_session_relay_set_reply(NpFakeSessionRelay *fake,
                                     const gchar        *state,
                                     const gchar        *detail,
                                     const gchar *const *relays);

/* After answering a GetEventUpstream call, emit
 * UpstreamStatusChanged(id, @relay_url, @relay_state, @reason,
 * @event_state) for that id (NULL @event_state: no follow-up). */
void np_fake_session_relay_set_followup(NpFakeSessionRelay *fake,
                                        const gchar        *relay_url,
                                        const gchar        *relay_state,
                                        const gchar        *reason,
                                        const gchar        *event_state);

/* Emits UpstreamStatusChanged now. */
void np_fake_session_relay_emit(NpFakeSessionRelay *fake,
                                const gchar        *event_id,
                                const gchar        *relay_url,
                                const gchar        *relay_state,
                                const gchar        *reason,
                                const gchar        *event_state);

/* Number of GetEventUpstream calls answered, and the last id asked. */
guint  np_fake_session_relay_query_count(NpFakeSessionRelay *fake);
gchar *np_fake_session_relay_last_query(NpFakeSessionRelay *fake);

/* Iterate @ctx (NULL: thread default) until *@flag is TRUE or @timeout_ms
 * passes; returns *@flag. Event-driven: no sleeps. */
gboolean np_wait_for(GMainContext *ctx, const gboolean *flag, guint timeout_ms);

G_END_DECLS

#endif /* NP_FAKE_SESSION_RELAY_H */
