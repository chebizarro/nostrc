/* ns-net.c - Signer, relay discovery and publishing for nostr-share
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-net.h"
#include "ns-kind.h"

#include <nostr/nip19/nip19.h>
#include "nostr-event.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Session relay ---- */

gchar *
ns_session_relay_socket(void)
{
  const gchar *override = g_getenv("NOSTR_SHARE_SESSION_RELAY_SOCKET");
  g_autofree gchar *path = override != NULL
    ? g_strdup(override)
    : g_build_filename(g_get_user_runtime_dir(), "nostr", "relay.sock", NULL);
  if (path == NULL || *path == '\0')
    return NULL;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
    return NULL;
  return g_steal_pointer(&path);
}

void
ns_net_init(NsNet *net)
{
  memset(net, 0, sizeof(*net));
  net->session_socket = ns_session_relay_socket();
}

void
ns_net_clear(NsNet *net)
{
  g_clear_pointer(&net->session_socket, g_free);
}

NostrPublishTransport *
ns_net_transport_new(NsNet *net, const gchar *url)
{
  if (net->factory != NULL)
    return net->factory(url, net->factory_data);
  if (net->session_socket != NULL && g_str_equal(url, NS_SESSION_RELAY_URL))
    return nostr_publish_transport_new_websocket_unix(url, net->session_socket);
  return nostr_publish_transport_new_websocket(url);
}

static NostrPublishTransport *
factory_trampoline(const gchar *url, gpointer user_data)
{
  return ns_net_transport_new(user_data, url);
}

static gboolean
wake_cb(gpointer user_data)
{
  (void)user_data;
  return G_SOURCE_CONTINUE;
}

/* ---- Signer ---- */

typedef struct {
  GDBusConnection *bus;
  GError          *error;
  gboolean         done;
} BusWait;

static void
on_bus(GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void)src;
  BusWait *w = user_data;
  w->bus = g_bus_get_finish(res, &w->error);
  w->done = TRUE;
}

/* g_bus_get_sync() has no timeout: a session bus address that accepts
 * the connection but never completes auth hangs the caller forever.
 * Bound it. */
static GDBusConnection *
session_bus_bounded(guint timeout_ms, GError **error)
{
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  BusWait w = { 0 };
  g_bus_get(G_BUS_TYPE_SESSION, cancel, on_bus, &w);
  GSource *wake = g_timeout_source_new(100);
  g_source_set_callback(wake, wake_cb, NULL, NULL);
  g_source_attach(wake, ctx);
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (!w.done) {
    if (g_get_monotonic_time() >= deadline && !g_cancellable_is_cancelled(cancel))
      g_cancellable_cancel(cancel);
    g_main_context_iteration(ctx, TRUE);
  }
  g_source_destroy(wake);
  g_source_unref(wake);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  if (w.bus == NULL) {
    if (g_error_matches(w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error(&w.error);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                  "the session bus did not answer within %u s", timeout_ms / 1000);
    } else {
      g_propagate_error(error, w.error);
    }
  }
  return w.bus;
}

NostrPublishSigner *
ns_signer_connect(gchar **out_pubkey_hex, GError **error)
{
  GError *local = NULL;
  g_autoptr(GDBusConnection) bus = session_bus_bounded(5000, &local);
  if (bus == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER,
                "no signer: cannot reach the session bus (%s)", local->message);
    g_clear_error(&local);
    return NULL;
  }

  /* GetPublicKey doubles as the liveness probe; D-Bus activation starts
   * a signer that is installed but not running. */
  g_autoptr(GVariant) reply =
    g_dbus_connection_call_sync(bus, "org.nostr.Signer", "/org/nostr/signer",
                                "org.nostr.Signer", "GetPublicKey", NULL,
                                G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                                10000, NULL, &local);
  if (reply == NULL) {
    g_autofree gchar *remote = g_dbus_error_get_remote_error(local);
    g_dbus_error_strip_remote_error(local);
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER,
                "no signer: org.nostr.Signer is not available on the session "
                "bus (%s%s%s). Install and unlock a NIP-55L signer (e.g. "
                "gnostr-signer) and try again.",
                remote ? remote : "", remote ? ": " : "", local->message);
    g_clear_error(&local);
    return NULL;
  }

  const gchar *npub = NULL;
  g_variant_get(reply, "(&s)", &npub);
  uint8_t pk[32];
  if (npub == NULL || nostr_nip19_decode_npub(npub, pk) != 0) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER,
                "no signer: GetPublicKey returned '%s', not an npub (is the "
                "signer locked or without an active account?)",
                npub ? npub : "");
    return NULL;
  }
  GString *hex = g_string_sized_new(64);
  for (int i = 0; i < 32; i++)
    g_string_append_printf(hex, "%02x", pk[i]);

  NostrPublishSigner *signer = nostr_publish_signer_new_dbus(bus, NS_APP_ID, &local);
  if (signer == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER, "no signer: %s",
                local ? local->message : "proxy creation failed");
    g_clear_error(&local);
    g_string_free(hex, TRUE);
    return NULL;
  }
  if (out_pubkey_hex != NULL)
    *out_pubkey_hex = g_string_free(hex, FALSE);
  else
    g_string_free(hex, TRUE);
  return signer;
}

/* ---- Event verification ---- */

static gboolean
verified_event(const gchar *json, gint kind, const gchar *author_hex,
               gint64 *created_at)
{
  NostrEvent *ev = nostr_event_new();
  if (ev == NULL)
    return FALSE;
  gboolean ok =
    nostr_event_deserialize_signed(ev, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(ev) == kind &&
    g_strcmp0(nostr_event_get_pubkey(ev), author_hex) == 0;
  if (ok)
    *created_at = nostr_event_get_created_at(ev);
  nostr_event_free(ev);
  return ok;
}

/* ---- Replaceable-event fetch ---- */

typedef struct {
  NostrPublishTransport *t;
  gboolean               done;
} FetchConn;

typedef struct {
  gchar     *sub_id;
  gchar     *req_frame;
  gint       kind;
  gchar     *author;
  gchar     *best_json;
  gint64     best_ts;
  GPtrArray *conns;      /* FetchConn* */
} FetchCtx;

static FetchConn *
fetch_conn_for(FetchCtx *fc, NostrPublishTransport *t)
{
  for (guint i = 0; i < fc->conns->len; i++) {
    FetchConn *c = g_ptr_array_index(fc->conns, i);
    if (c->t == t)
      return c;
  }
  return NULL;
}

static void
fetch_on_state(NostrPublishTransport *t, gboolean connected, const GError *error,
               gpointer user_data)
{
  FetchCtx *fc = user_data;
  FetchConn *c = fetch_conn_for(fc, t);
  if (c == NULL || c->done)
    return;
  if (connected) {
    if (!nostr_publish_transport_send_frame(t, fc->req_frame, NULL))
      c->done = TRUE;
  } else if (error != NULL) {
    g_debug("nostr-share: %s: %s", nostr_publish_transport_get_url(t),
            error->message);
    c->done = TRUE;
  }
}

static void
fetch_on_frame(NostrPublishTransport *t, const gchar *cmd, const gchar *envelope,
               gpointer user_data)
{
  FetchCtx *fc = user_data;
  FetchConn *c = fetch_conn_for(fc, t);
  if (c == NULL || c->done)
    return;

  if (g_strcmp0(cmd, "EOSE") == 0 || g_strcmp0(cmd, "CLOSED") == 0) {
    g_autoptr(JsonParser) p = json_parser_new();
    if (json_parser_load_from_data(p, envelope, -1, NULL)) {
      JsonNode *root = json_parser_get_root(p);
      if (JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *a = json_node_get_array(root);
        if (json_array_get_length(a) >= 2 &&
            g_strcmp0(json_array_get_string_element(a, 1), fc->sub_id) == 0)
          c->done = TRUE;
      }
    }
    return;
  }
  if (g_strcmp0(cmd, "EVENT") != 0)
    return;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, envelope, -1, NULL))
    return;
  JsonNode *root = json_parser_get_root(p);
  if (!JSON_NODE_HOLDS_ARRAY(root))
    return;
  JsonArray *a = json_node_get_array(root);
  if (json_array_get_length(a) < 3 ||
      g_strcmp0(json_array_get_string_element(a, 1), fc->sub_id) != 0)
    return;
  JsonNode *evn = json_array_get_element(a, 2);
  if (!JSON_NODE_HOLDS_OBJECT(evn))
    return;
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, evn);
  g_autofree gchar *ev_json = json_generator_to_data(gen, NULL);
  gint64 ts = 0;
  if (!verified_event(ev_json, fc->kind, fc->author, &ts)) {
    g_debug("nostr-share: dropping unverifiable kind-%d event from %s",
            fc->kind, nostr_publish_transport_get_url(t));
    return;
  }
  if (fc->best_json == NULL || ts > fc->best_ts) {
    g_free(fc->best_json);
    fc->best_json = g_steal_pointer(&ev_json);
    fc->best_ts = ts;
  }
}

gchar *
ns_net_fetch_replaceable(NsNet *net, const gchar *const *relays, gint kind,
                         const gchar *author_hex, guint timeout_ms,
                         GError **error)
{
  (void)error;
  if (relays == NULL || relays[0] == NULL || author_hex == NULL)
    return NULL;

  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  FetchCtx fc = { 0 };
  fc.kind = kind;
  fc.author = g_strdup(author_hex);
  fc.sub_id = g_strdup_printf("ns-%08x", g_random_int());
  fc.req_frame = g_strdup_printf(
    "[\"REQ\",\"%s\",{\"kinds\":[%d],\"authors\":[\"%s\"],\"limit\":1}]",
    fc.sub_id, kind, author_hex);
  fc.conns = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; relays[i] != NULL; i++) {
    NostrPublishTransport *t = ns_net_transport_new(net, relays[i]);
    if (t == NULL)
      continue;
    FetchConn *c = g_new0(FetchConn, 1);
    c->t = t;
    g_ptr_array_add(fc.conns, c);
    nostr_publish_transport_set_listener(t, fetch_on_frame, &fc);
    nostr_publish_transport_set_state_callback(t, fetch_on_state, &fc);
    nostr_publish_transport_connect_async(t);
  }

  GSource *wake = g_timeout_source_new(100);
  g_source_set_callback(wake, wake_cb, NULL, NULL);
  g_source_attach(wake, ctx);

  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  for (;;) {
    gboolean all_done = TRUE;
    for (guint i = 0; i < fc.conns->len; i++)
      if (!((FetchConn *)g_ptr_array_index(fc.conns, i))->done)
        all_done = FALSE;
    if (all_done || g_get_monotonic_time() >= deadline)
      break;
    g_main_context_iteration(ctx, TRUE);
  }

  g_source_destroy(wake);
  g_source_unref(wake);

  g_autofree gchar *close_frame = g_strdup_printf("[\"CLOSE\",\"%s\"]", fc.sub_id);
  for (guint i = 0; i < fc.conns->len; i++) {
    FetchConn *c = g_ptr_array_index(fc.conns, i);
    nostr_publish_transport_set_listener(c->t, NULL, NULL);
    nostr_publish_transport_set_state_callback(c->t, NULL, NULL);
    if (nostr_publish_transport_is_connected(c->t))
      (void)nostr_publish_transport_send_frame(c->t, close_frame, NULL);
    nostr_publish_transport_disconnect(c->t);
    nostr_publish_transport_unref(c->t);
  }
  /* Let close handshakes and cancellations run on our context. */
  while (g_main_context_iteration(ctx, FALSE))
    ;
  g_ptr_array_unref(fc.conns);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);

  g_free(fc.sub_id);
  g_free(fc.req_frame);
  g_free(fc.author);
  return fc.best_json;
}

/* ---- Target resolution ---- */

void
ns_targets_clear(NsTargets *t)
{
  if (t == NULL)
    return;
  g_clear_pointer(&t->targets, g_strfreev);
  g_clear_pointer(&t->direct, g_strfreev);
  g_clear_pointer(&t->write_source, g_free);
  t->session_included = FALSE;
}

/* Relays we can ask for the user's 10002/10063: the session relay (a
 * cache) first, then the configured home relays. */
static gchar **
discovery_relays(const NsConfig *cfg, NsNet *net)
{
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  if (net->session_socket != NULL && cfg->upstream != NS_UPSTREAM_DIRECT_ONLY)
    g_strv_builder_add(b, NS_SESSION_RELAY_URL);
  if (cfg->upstream != NS_UPSTREAM_SESSION_RELAY_ONLY)
    for (guint i = 0; cfg->home_relays && cfg->home_relays[i]; i++)
      g_strv_builder_add(b, cfg->home_relays[i]);
  return g_strv_builder_end(b);
}

static gboolean
relay_url_ok(const gchar *u)
{
  return g_str_has_prefix(u, "wss://") || g_str_has_prefix(u, "ws://");
}

gboolean
ns_resolve_targets(const NsConfig *cfg, NsNet *net, const gchar *pubkey_hex,
                   const NsRecipient *to, NsTargets *out, GError **error)
{
  memset(out, 0, sizeof(*out));

  /* A NIP-29 group post goes to the group's relay and nowhere else:
   * fanning it out to public write relays would leak a group-scoped
   * message. */
  if (to != NULL && to->type == NS_RECIPIENT_GROUP) {
    const gchar *one[] = { to->relay_url, NULL };
    out->targets = g_strdupv((gchar **)one);
    out->direct = g_strdupv((gchar **)one);
    out->write_source = g_strdup("NIP-29 group relay");
    return TRUE;
  }

  gboolean use_session = net->session_socket != NULL &&
                         cfg->upstream != NS_UPSTREAM_DIRECT_ONLY;

  if (cfg->upstream == NS_UPSTREAM_SESSION_RELAY_ONLY ||
      (cfg->upstream == NS_UPSTREAM_SESSION_RELAY_OR_DIRECT && use_session)) {
    if (!use_session) {
      g_set_error(error, NS_ERROR, NS_ERROR_NO_RELAYS,
                  "upstream_mode=session_relay_only but no session relay "
                  "socket at $XDG_RUNTIME_DIR/nostr/relay.sock");
      return FALSE;
    }
    const gchar *one[] = { NS_SESSION_RELAY_URL, NULL };
    out->targets = g_strdupv((gchar **)one);
    out->direct = g_new0(gchar *, 1);
    out->session_included = TRUE;
    out->write_source = g_strdup("session relay");
    return TRUE;
  }

  /* Direct write relays: kind 10002 (NIP-65) when we can find one,
   * else the configured home relays. */
  g_auto(GStrv) write = NULL;
  g_auto(GStrv) disc = discovery_relays(cfg, net);
  g_autofree gchar *rl = ns_net_fetch_replaceable(net, (const gchar *const *)disc,
                                                  10002, pubkey_hex,
                                                  cfg->query_timeout_ms, NULL);
  if (rl != NULL) {
    write = nostr_publish_nip65_relays(rl, NOSTR_PUBLISH_NIP65_WRITE, NULL);
    if (write != NULL && write[0] != NULL)
      out->write_source = g_strdup("kind 10002 (NIP-65)");
  }
  if (write == NULL || write[0] == NULL) {
    g_clear_pointer(&write, g_strfreev);
    write = g_strdupv(cfg->home_relays);
    out->write_source = g_strdup_printf("home_relays in %s", cfg->config_path);
  }

  g_autoptr(GStrvBuilder) direct = g_strv_builder_new();
  guint n_direct = 0;
  for (guint i = 0; write && write[i]; i++)
    if (relay_url_ok(write[i])) {
      g_strv_builder_add(direct, write[i]);
      n_direct++;
    }
  if (n_direct == 0) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_RELAYS,
                "no write relays: publish a kind-10002 relay list or set "
                "home_relays in %s", cfg->config_path);
    return FALSE;
  }
  out->direct = g_strv_builder_end(direct);

  g_autoptr(GStrvBuilder) all = g_strv_builder_new();
  if (use_session) {
    g_strv_builder_add(all, NS_SESSION_RELAY_URL);
    out->session_included = TRUE;
  }
  g_strv_builder_addv(all, (const gchar **)out->direct);
  out->targets = g_strv_builder_end(all);
  return TRUE;
}

gchar **
ns_resolve_blossom_servers(const NsConfig *cfg, NsNet *net, const gchar *pubkey_hex,
                           gchar **out_source, GError **error)
{
  g_auto(GStrv) disc = discovery_relays(cfg, net);
  g_autofree gchar *ev = ns_net_fetch_replaceable(net, (const gchar *const *)disc,
                                                  10063, pubkey_hex,
                                                  cfg->query_timeout_ms, NULL);
  gchar **servers = ev ? ns_blossom_servers_from_event(ev) : NULL;
  if (servers != NULL && servers[0] != NULL) {
    if (out_source) *out_source = g_strdup("kind 10063 (BUD-03)");
    return servers;
  }
  g_strfreev(servers);

  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  guint n = 0;
  for (guint i = 0; cfg->blossom_servers && cfg->blossom_servers[i]; i++)
    if (g_str_has_prefix(cfg->blossom_servers[i], "https://")) {
      g_autofree gchar *norm = g_strdup(cfg->blossom_servers[i]);
      gsize len = strlen(norm);
      while (len > 8 && norm[len - 1] == '/')
        norm[--len] = '\0';
      g_strv_builder_add(b, norm);
      n++;
    }
  if (n == 0) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SERVERS,
                "no Blossom servers: publish a kind-10063 server list or set "
                "blossom_servers (https:// only) in %s", cfg->config_path);
    return NULL;
  }
  if (out_source) *out_source = g_strdup_printf("blossom_servers in %s",
                                                cfg->config_path);
  return g_strv_builder_end(b);
}

/* ---- Publish ---- */

void
ns_publish_report_clear(NsPublishReport *r)
{
  if (r == NULL)
    return;
  g_clear_pointer(&r->lines, g_ptr_array_unref);
  memset(r, 0, sizeof(*r));
}

typedef struct {
  gboolean         done;
  NsPublishReport *report;
} PublishCtx;

static const gchar *
status_word(NostrPublishRelayStatus s)
{
  switch (s) {
  case NOSTR_PUBLISH_RELAY_PENDING:            return "pending";
  case NOSTR_PUBLISH_RELAY_ACCEPTED:           return "accepted";
  case NOSTR_PUBLISH_RELAY_REJECTED_TRANSIENT: return "rejected (transient)";
  case NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT: return "rejected";
  case NOSTR_PUBLISH_RELAY_UNREACHABLE:        return "unreachable";
  case NOSTR_PUBLISH_RELAY_TIMED_OUT:          return "timed out";
  }
  return "?";
}

static void
publish_on_done(NostrPublisher *p, const NostrPublishResult *res, gpointer user_data)
{
  (void)p;
  PublishCtx *pc = user_data;
  NsPublishReport *r = pc->report;
  guint n = nostr_publish_result_get_n_relays(res);
  r->n_targets = n;
  for (guint i = 0; i < n; i++) {
    NostrPublishRelayStatus st = NOSTR_PUBLISH_RELAY_PENDING;
    const gchar *reason = NULL;
    const gchar *url = nostr_publish_result_get_relay(res, i, &st, &reason);
    gboolean session = g_strcmp0(url, NS_SESSION_RELAY_URL) == 0;
    if (st == NOSTR_PUBLISH_RELAY_ACCEPTED) {
      r->n_accepted++;
      if (session) r->session_accepted = TRUE;
      else r->n_direct_accepted++;
    }
    g_ptr_array_add(r->lines,
                    g_strdup_printf("%s: %s%s%s%s",
                                    session ? "session relay (relay.sock)" : url,
                                    status_word(st),
                                    reason && *reason ? " (" : "",
                                    reason && *reason ? reason : "",
                                    reason && *reason ? ")" : ""));
  }
  pc->done = TRUE;
}

gboolean
ns_net_publish(NsNet *net, const NsConfig *cfg, const gchar *signed_json,
               const NsTargets *targets, NsPublishReport *report, GError **error)
{
  memset(report, 0, sizeof(*report));
  report->lines = g_ptr_array_new_with_free_func(g_free);

  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  NostrPublisher *pub = nostr_publisher_new(NULL);
  nostr_publisher_set_transport_factory(pub, factory_trampoline, net, NULL);

  NostrPublishPolicy policy;
  nostr_publish_policy_init(&policy);
  policy.ok_wait_sec = cfg->ok_wait_sec;

  PublishCtx pc = { FALSE, report };
  GError *local = NULL;
  gboolean started = nostr_publisher_publish_signed(
    pub, signed_json, (const gchar *const *)targets->targets, &policy,
    g_get_real_time() / G_USEC_PER_SEC, NULL, publish_on_done, &pc, NULL, &local);

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
    g_set_error(error, NS_ERROR, NS_ERROR_PUBLISH, "publish failed: %s",
                local ? local->message : "unknown error");
    g_clear_error(&local);
    return FALSE;
  }

  gboolean session_only = targets->direct == NULL || targets->direct[0] == NULL;
  gboolean ok = session_only ? report->session_accepted
                             : report->n_direct_accepted > 0;
  if (!ok) {
    g_set_error(error, NS_ERROR, NS_ERROR_PUBLISH,
                "no relay accepted the event (%u target%s)", report->n_targets,
                report->n_targets == 1 ? "" : "s");
    return FALSE;
  }
  return TRUE;
}
