/* nd-relay-transport.c - Relay transport interface + backends
 *
 * SPDX-License-Identifier: MIT
 *
 * Two backends live in this file:
 *   1. A websocket scaffold. libsoup 3 has SoupWebsocketConnection but
 *      wiring it end-to-end (TLS, ping/pong, backpressure, close
 *      frames) is a discrete piece of work of its own; that work lands
 *      under bead nostrc-ygu6.ws. Until then the scaffold reports
 *      %G_IO_ERROR_NOT_SUPPORTED from connect_async so a nostr-dav
 *      pointed at a real relay fails loud instead of appearing healthy.
 *   2. An in-process fixture used by tests. It records every outbound
 *      frame in FIFO order and lets the test deliver EVENT/EOSE/OK
 *      frames back through the listener without a socket.
 *
 * The vtable approach keeps NdRelaySync free of ifdefs and lets
 * test_relay_sync run under valgrind without a network stack.
 */

#include "nd-relay-transport.h"

typedef struct {
  gboolean (*connect_async)   (NdRelayTransport *self);
  gboolean (*send_frame)      (NdRelayTransport *self,
                               const gchar      *frame_json,
                               GError          **error);
  void     (*disconnect)      (NdRelayTransport *self);
  gboolean (*is_connected)    (NdRelayTransport *self);
  void     (*finalize)        (NdRelayTransport *self);
} NdRelayTransportOps;

struct _NdRelayTransport {
  int                            ref_count;
  const NdRelayTransportOps     *ops;
  gchar                         *url;

  NdRelayTransportListener       listener;
  gpointer                       listener_data;
  NdRelayTransportStateCallback  state_cb;
  gpointer                       state_data;

  /* Backend-specific state: owned by the ops table, released by
   * ops->finalize(). Only the fixture backend surfaces this at the
   * public boundary. */
  gboolean                       is_fixture;
  gboolean                       fx_connected;
  gpointer                       backend_state;
};

typedef struct {
  GPtrArray *sent;   /* char* frames, FIFO */
} NdRelayFixtureState;

/* ---- Fixture backend ---- */

static gboolean
fx_connect_async(NdRelayTransport *self)
{
  self->fx_connected = TRUE;
  if (self->state_cb != NULL)
    self->state_cb(self, TRUE, NULL, self->state_data);
  return TRUE;
}

static gboolean
fx_send_frame(NdRelayTransport *self, const gchar *frame_json, GError **error)
{
  if (!self->fx_connected) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        "fixture transport is not connected");
    return FALSE;
  }
  NdRelayFixtureState *fx = self->backend_state;
  g_ptr_array_add(fx->sent, g_strdup(frame_json));
  return TRUE;
}

static void
fx_disconnect(NdRelayTransport *self)
{
  gboolean was_connected = self->fx_connected;
  self->fx_connected = FALSE;
  if (was_connected && self->state_cb != NULL)
    self->state_cb(self, FALSE, NULL, self->state_data);
}

static gboolean
fx_is_connected(NdRelayTransport *self)
{
  return self->fx_connected;
}

static void
fx_finalize(NdRelayTransport *self)
{
  NdRelayFixtureState *fx = self->backend_state;
  if (fx != NULL) {
    g_clear_pointer(&fx->sent, g_ptr_array_unref);
    g_free(fx);
    self->backend_state = NULL;
  }
}

static const NdRelayTransportOps FIXTURE_OPS = {
  .connect_async = fx_connect_async,
  .send_frame    = fx_send_frame,
  .disconnect    = fx_disconnect,
  .is_connected  = fx_is_connected,
  .finalize      = fx_finalize,
};

/* ---- Websocket scaffold backend ---- */

typedef struct {
  /* Room for the SoupSession + SoupWebsocketConnection once the
   * follow-up bead lands. Kept as a scaffold so the header is stable. */
  gpointer soup_reserved;
} NdRelayWebsocket;

static gboolean
ws_connect_async(NdRelayTransport *self)
{
  GError *err = NULL;
  g_set_error(&err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
              "nd-relay-transport: websocket backend for %s is not wired "
              "yet (bead nostrc-ygu6.ws); rebuild with the fixture "
              "transport or wait for the follow-up",
              self->url);
  if (self->state_cb != NULL)
    self->state_cb(self, FALSE, err, self->state_data);
  g_error_free(err);
  return FALSE;
}

static gboolean
ws_send_frame(NdRelayTransport *self, const gchar *frame_json, GError **error)
{
  (void)self; (void)frame_json;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "nd-relay-transport: websocket send not implemented");
  return FALSE;
}

static void
ws_disconnect(NdRelayTransport *self)
{
  (void)self;
}

static gboolean
ws_is_connected(NdRelayTransport *self)
{
  (void)self;
  return FALSE;
}

static void
ws_finalize(NdRelayTransport *self)
{
  NdRelayWebsocket *ws = self->backend_state;
  g_free(ws);
  self->backend_state = NULL;
}

static const NdRelayTransportOps WEBSOCKET_OPS = {
  .connect_async = ws_connect_async,
  .send_frame    = ws_send_frame,
  .disconnect    = ws_disconnect,
  .is_connected  = ws_is_connected,
  .finalize      = ws_finalize,
};

/* ---- Public API ---- */

static NdRelayTransport *
new_common(const gchar *url, const NdRelayTransportOps *ops)
{
  NdRelayTransport *self = g_new0(NdRelayTransport, 1);
  self->ref_count = 1;
  self->ops       = ops;
  self->url       = g_strdup(url);
  return self;
}

NdRelayTransport *
nd_relay_transport_new_websocket(const gchar *url)
{
  g_return_val_if_fail(url != NULL, NULL);
  NdRelayTransport *self = new_common(url, &WEBSOCKET_OPS);
  self->backend_state = g_new0(NdRelayWebsocket, 1);
  return self;
}

NdRelayTransport *
nd_relay_transport_new_fixture(const gchar *url)
{
  g_return_val_if_fail(url != NULL, NULL);
  NdRelayTransport *self = new_common(url, &FIXTURE_OPS);
  self->is_fixture = TRUE;
  NdRelayFixtureState *fx = g_new0(NdRelayFixtureState, 1);
  fx->sent = g_ptr_array_new_with_free_func(g_free);
  self->backend_state = fx;
  return self;
}

void
nd_relay_transport_set_listener(NdRelayTransport         *self,
                                NdRelayTransportListener  listener,
                                gpointer                  user_data)
{
  g_return_if_fail(self != NULL);
  self->listener      = listener;
  self->listener_data = user_data;
}

void
nd_relay_transport_set_state_callback(NdRelayTransport              *self,
                                      NdRelayTransportStateCallback  cb,
                                      gpointer                       user_data)
{
  g_return_if_fail(self != NULL);
  self->state_cb   = cb;
  self->state_data = user_data;
}

void
nd_relay_transport_connect_async(NdRelayTransport *self)
{
  g_return_if_fail(self != NULL);
  self->ops->connect_async(self);
}

gboolean
nd_relay_transport_send_frame(NdRelayTransport *self,
                              const gchar      *frame_json,
                              GError          **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(frame_json != NULL, FALSE);
  return self->ops->send_frame(self, frame_json, error);
}

void
nd_relay_transport_disconnect(NdRelayTransport *self)
{
  g_return_if_fail(self != NULL);
  self->ops->disconnect(self);
}

gboolean
nd_relay_transport_is_connected(NdRelayTransport *self)
{
  g_return_val_if_fail(self != NULL, FALSE);
  return self->ops->is_connected(self);
}

const gchar *
nd_relay_transport_get_url(NdRelayTransport *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->url;
}

NdRelayTransport *
nd_relay_transport_ref(NdRelayTransport *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_atomic_int_inc(&self->ref_count);
  return self;
}

void
nd_relay_transport_unref(NdRelayTransport *self)
{
  if (self == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&self->ref_count))
    return;
  if (self->ops->finalize != NULL)
    self->ops->finalize(self);
  g_free(self->url);
  g_free(self);
}

/* ---- Fixture-only helpers ---- */

void
nd_relay_transport_fixture_deliver_frame(NdRelayTransport *self,
                                         const gchar      *kind_hint,
                                         const gchar      *envelope_json)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(self->is_fixture);
  g_return_if_fail(envelope_json != NULL);
  if (self->listener != NULL)
    self->listener(self, kind_hint, envelope_json, self->listener_data);
}

void
nd_relay_transport_fixture_set_state(NdRelayTransport *self,
                                     gboolean          connected,
                                     GError           *error)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(self->is_fixture);
  self->fx_connected = connected;
  if (self->state_cb != NULL)
    self->state_cb(self, connected, error, self->state_data);
}

gchar **
nd_relay_transport_fixture_take_sent(NdRelayTransport *self,
                                     gsize            *out_len)
{
  g_return_val_if_fail(self != NULL, NULL);
  g_return_val_if_fail(self->is_fixture, NULL);

  NdRelayFixtureState *fx = self->backend_state;
  gsize n = fx->sent->len;
  gchar **frames = g_new0(gchar *, n + 1);
  for (gsize i = 0; i < n; i++) {
    /* Steal each element so g_ptr_array_set_size() below does not
     * double-free through the array's free_func. */
    frames[i] = g_ptr_array_index(fx->sent, i);
    g_ptr_array_index(fx->sent, i) = NULL;
  }
  g_ptr_array_set_size(fx->sent, 0);
  if (out_len != NULL)
    *out_len = n;
  return frames;
}
