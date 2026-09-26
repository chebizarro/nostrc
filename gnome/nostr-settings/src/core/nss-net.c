/* nss-net.c — see nss-net.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-net.h"
#include "nss-config.h"

#include "nostr-event.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

G_DEFINE_QUARK(nss-net-error-quark, nss_net_error)

void
nss_net_init(NssNet *net)
{
  memset(net, 0, sizeof *net);
  const gchar *override = g_getenv("NOSTR_SETTINGS_SESSION_RELAY_SOCKET");
  g_autofree gchar *path = override && *override
    ? g_strdup(override)
    : g_build_filename(g_get_user_runtime_dir(), "nostr", "relay.sock", NULL);
  struct stat st;
  if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
    net->session_socket = g_steal_pointer(&path);
}

void
nss_net_clear(NssNet *net)
{
  g_clear_pointer(&net->session_socket, g_free);
}

static NostrPublishTransport *
transport_new(NssNet *net, const gchar *url)
{
  if (net->factory != NULL)
    return net->factory(url, net->factory_data);
  if (g_str_equal(url, NSS_SESSION_RELAY_URL))
    return net->session_socket
      ? nostr_publish_transport_new_websocket_unix(url, net->session_socket) : NULL;
  return nostr_publish_transport_new_websocket(url);
}

static NostrPublishTransport *
factory_trampoline(const gchar *url, gpointer data)
{
  return transport_new(data, url);
}

static gboolean
wake_cb(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

/* ── fetch ─────────────────────────────────────────────────────────────── */

static gboolean
verified(const gchar *json, gint kind, const gchar *author, gint64 *created_at)
{
  NostrEvent *ev = nostr_event_new();
  if (ev == NULL)
    return FALSE;
  gboolean ok =
    nostr_event_deserialize_signed(ev, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(ev) == kind &&
    g_strcmp0(nostr_event_get_pubkey(ev), author) == 0;
  if (ok)
    *created_at = nostr_event_get_created_at(ev);
  nostr_event_free(ev);
  return ok;
}

typedef struct {
  NostrPublishTransport *t;
  gboolean               done;
} Conn;

typedef struct {
  gchar     *sub_id;
  gchar     *req;
  gint       kind;
  const gchar *author;
  gchar     *best;
  gchar     *best_src;
  gint64     best_ts;
  GPtrArray *conns;
} Fetch;

static Conn *
conn_for(Fetch *f, NostrPublishTransport *t)
{
  for (guint i = 0; i < f->conns->len; i++)
    if (((Conn *)g_ptr_array_index(f->conns, i))->t == t)
      return g_ptr_array_index(f->conns, i);
  return NULL;
}

static void
on_state(NostrPublishTransport *t, gboolean connected, const GError *error, gpointer data)
{
  Fetch *f = data;
  Conn *c = conn_for(f, t);
  if (c == NULL || c->done)
    return;
  if (connected) {
    if (!nostr_publish_transport_send_frame(t, f->req, NULL))
      c->done = TRUE;
  } else if (error != NULL) {
    c->done = TRUE;
  }
}

static void
on_frame(NostrPublishTransport *t, const gchar *cmd, const gchar *envelope, gpointer data)
{
  Fetch *f = data;
  Conn *c = conn_for(f, t);
  if (c == NULL || c->done)
    return;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, envelope, -1, NULL) ||
      json_parser_get_root(p) == NULL ||
      !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p)))
    return;
  JsonArray *a = json_node_get_array(json_parser_get_root(p));
  if (json_array_get_length(a) < 2 ||
      g_strcmp0(json_array_get_string_element(a, 1), f->sub_id) != 0)
    return;
  if (g_strcmp0(cmd, "EOSE") == 0 || g_strcmp0(cmd, "CLOSED") == 0) {
    c->done = TRUE;
    return;
  }
  if (g_strcmp0(cmd, "EVENT") != 0 || json_array_get_length(a) < 3 ||
      !JSON_NODE_HOLDS_OBJECT(json_array_get_element(a, 2)))
    return;
  g_autofree gchar *ev = json_to_string(json_array_get_element(a, 2), FALSE);
  gint64 ts = 0;
  if (!verified(ev, f->kind, f->author, &ts))
    return;
  if (f->best == NULL || ts > f->best_ts) {
    g_free(f->best);
    g_free(f->best_src);
    f->best = g_steal_pointer(&ev);
    f->best_src = g_strdup(nostr_publish_transport_get_url(t));
    f->best_ts = ts;
  }
}

gchar *
nss_net_fetch_replaceable(NssNet *net, const gchar *const *relays, gint kind,
                          const gchar *author_hex, guint timeout_ms, gchar **out_source)
{
  if (out_source)
    *out_source = NULL;
  if (relays == NULL || relays[0] == NULL || author_hex == NULL)
    return NULL;
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  Fetch f = { 0 };
  f.kind = kind;
  f.author = author_hex;
  f.sub_id = g_strdup_printf("nss-%08x", g_random_int());
  f.req = g_strdup_printf("[\"REQ\",\"%s\",{\"kinds\":[%d],\"authors\":[\"%s\"],\"limit\":1}]",
                          f.sub_id, kind, author_hex);
  f.conns = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; relays[i]; i++) {
    NostrPublishTransport *t = transport_new(net, relays[i]);
    if (t == NULL)
      continue;
    Conn *c = g_new0(Conn, 1);
    c->t = t;
    g_ptr_array_add(f.conns, c);
    nostr_publish_transport_set_listener(t, on_frame, &f);
    nostr_publish_transport_set_state_callback(t, on_state, &f);
    nostr_publish_transport_connect_async(t);
  }
  GSource *wake = g_timeout_source_new(100);
  g_source_set_callback(wake, wake_cb, NULL, NULL);
  g_source_attach(wake, ctx);
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  for (;;) {
    gboolean all = TRUE;
    for (guint i = 0; i < f.conns->len; i++)
      all &= ((Conn *)g_ptr_array_index(f.conns, i))->done;
    if (all || g_get_monotonic_time() >= deadline)
      break;
    g_main_context_iteration(ctx, TRUE);
  }
  g_source_destroy(wake);
  g_source_unref(wake);
  g_autofree gchar *close = g_strdup_printf("[\"CLOSE\",\"%s\"]", f.sub_id);
  for (guint i = 0; i < f.conns->len; i++) {
    Conn *c = g_ptr_array_index(f.conns, i);
    nostr_publish_transport_set_listener(c->t, NULL, NULL);
    nostr_publish_transport_set_state_callback(c->t, NULL, NULL);
    if (nostr_publish_transport_is_connected(c->t))
      (void)nostr_publish_transport_send_frame(c->t, close, NULL);
    nostr_publish_transport_disconnect(c->t);
    nostr_publish_transport_unref(c->t);
  }
  while (g_main_context_iteration(ctx, FALSE))
    ;
  g_ptr_array_unref(f.conns);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  g_free(f.sub_id);
  g_free(f.req);
  if (out_source)
    *out_source = g_steal_pointer(&f.best_src);
  g_free(f.best_src);
  return f.best;
}

/* ── publish ───────────────────────────────────────────────────────────── */

void
nss_relay_result_free(NssRelayResult *r)
{
  if (r == NULL)
    return;
  g_free(r->url);
  g_free(r->detail);
  g_free(r);
}

void
nss_publish_report_clear(NssPublishReport *r)
{
  g_clear_pointer(&r->signed_json, g_free);
  g_clear_pointer(&r->results, g_ptr_array_unref);
  r->n_required = r->n_required_ok = 0;
}

typedef struct {
  gboolean          done;
  NssPublishReport *report;
} PubCtx;

static void
on_relay(NostrPublisher *pub, const gchar *id, const gchar *url,
         NostrPublishRelayStatus status, const gchar *reason, gpointer data)
{
  (void)pub; (void)id;
  PubCtx *pc = data;
  NssRelayResult *r = g_new0(NssRelayResult, 1);
  r->url = g_strdup(url);
  r->accepted = status == NOSTR_PUBLISH_RELAY_ACCEPTED;
  switch (status) {
  case NOSTR_PUBLISH_RELAY_ACCEPTED:
    r->detail = g_strdup(reason && *reason ? reason : "accepted");
    break;
  case NOSTR_PUBLISH_RELAY_UNREACHABLE:
    r->detail = g_strdup("unreachable");
    break;
  case NOSTR_PUBLISH_RELAY_TIMED_OUT:
    r->detail = g_strdup("no answer (timed out)");
    break;
  default:
    r->detail = g_strdup_printf("rejected%s%s", reason && *reason ? ": " : "",
                                reason ? reason : "");
    break;
  }
  g_ptr_array_add(pc->report->results, r);
}

static void
on_done(NostrPublisher *pub, const NostrPublishResult *res, gpointer data)
{
  (void)pub;
  PubCtx *pc = data;
  pc->report->signed_json = g_strdup(nostr_publish_result_get_signed_json(res));
  pc->done = TRUE;
}

gboolean
nss_net_publish(NssNet *net, NostrPublishSigner *signer, const gchar *unsigned_json,
                const gchar *const *targets, const gchar *const *required,
                guint ok_wait_sec, NssPublishReport *report, GError **error)
{
  memset(report, 0, sizeof *report);
  report->results = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_result_free);
  if (targets == NULL || targets[0] == NULL) {
    g_set_error_literal(error, NSS_NET_ERROR, NSS_NET_ERROR_NO_RELAYS,
                        "no relays to publish to — add a write relay first");
    return FALSE;
  }
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  NostrPublisher *pub = nostr_publisher_new(signer);
  nostr_publisher_set_transport_factory(pub, factory_trampoline, net, NULL);
  NostrPublishPolicy policy;
  nostr_publish_policy_init(&policy);
  policy.ok_wait_sec = ok_wait_sec ? ok_wait_sec : 15;
  PubCtx pc = { FALSE, report };
  GError *local = NULL;
  gboolean started = nostr_publisher_publish(pub, unsigned_json, targets, &policy,
                                             g_get_real_time() / G_USEC_PER_SEC,
                                             on_relay, on_done, &pc, NULL, &local);
  if (started) {
    GSource *wake = g_timeout_source_new(250);
    g_source_set_callback(wake, wake_cb, NULL, NULL);
    g_source_attach(wake, ctx);
    while (!pc.done) {
      g_main_context_iteration(ctx, TRUE);
      nostr_publisher_tick(pub, g_get_real_time() / G_USEC_PER_SEC);
    }
    g_source_destroy(wake);
    g_source_unref(wake);
  }
  nostr_publisher_free(pub);
  while (g_main_context_iteration(ctx, FALSE))
    ;
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  if (!started) {
    if (local != NULL && local->domain == NOSTR_PUBLISH_SIGNER_ERROR) {
      g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_NO_SIGNER,
                  "the signer did not sign the event: %s", local->message);
      g_error_free(local);
    } else {
      g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_PUBLISH, "publish failed: %s",
                  local ? local->message : "unknown error");
      g_clear_error(&local);
    }
    return FALSE;
  }
  for (guint i = 0; required && required[i]; i++) {
    report->n_required++;
    for (guint j = 0; j < report->results->len; j++) {
      NssRelayResult *r = g_ptr_array_index(report->results, j);
      if (r->accepted && g_str_equal(r->url, required[i])) {
        report->n_required_ok++;
        break;
      }
    }
  }
  if (report->n_required == 0 || report->n_required_ok < report->n_required) {
    g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_PUBLISH,
                "%u of %u write relays confirmed", report->n_required_ok,
                report->n_required);
    return FALSE;
  }
  return TRUE;
}

/* ── signer ────────────────────────────────────────────────────────────── */

NostrPublishSigner *
nss_signer_connect(GDBusConnection *bus, gchar **out_pubkey_hex, GError **error)
{
  GError *local = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer", "GetPublicKey",
    NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &local);
  if (r == NULL) {
    g_dbus_error_strip_remote_error(local);
    g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_NO_SIGNER,
                "no signer on the session bus (%s)", local->message);
    g_error_free(local);
    return NULL;
  }
  const gchar *npub = NULL;
  g_variant_get(r, "(&s)", &npub);
  guint8 pk[32];
  if (!nss_parse_pubkey(npub, pk, NULL)) {
    g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_NO_SIGNER,
                "the signer has no active identity (is it locked?)");
    return NULL;
  }
  NostrPublishSigner *s = nostr_publish_signer_new_dbus(bus, NSS_APP_ID, &local);
  if (s == NULL) {
    g_set_error(error, NSS_NET_ERROR, NSS_NET_ERROR_NO_SIGNER, "%s",
                local ? local->message : "signer proxy failed");
    g_clear_error(&local);
    return NULL;
  }
  if (out_pubkey_hex)
    *out_pubkey_hex = nss_pubkey_to_hex(pk);
  return s;
}
